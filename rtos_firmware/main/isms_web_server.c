/*******************************************************************************
 * isms_web_server.c  v3.1.0
 * ISMS Web Dashboard — HTTP/80 with Client Tracking
 * Target  : ESP32 + ESP-IDF v5.3.1
 *
 * Endpoints:
 *   GET /        → serves index.html from SPIFFS  (full dashboard)
 *   GET /data    → live JSON snapshot of g_sensor_data (polled every 2 s)
 *   GET /health  → {"status":"ok"}
 *
 * v3.1.0 — Added client tracking to update g_ws_client_count when browsers
 * connect/disconnect. OLED task checks g_ws_client_count to transition from
 * "Waiting for Web Dashboard" to "Live Sensor Pages".
 ******************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"

#include "isms_web_server.h"

static const char *TAG_HTTP = "ISMS_WEB";
#define HTTP_JSON_BUF 600
#define CLIENT_TIMEOUT_MS 10000  /* 10 s — client considered disconnected if no request */

static httpd_handle_t s_server = NULL;

/* ── Client tracking for OLED "System Connected" display ────────────────── */
#define CLIENT_TIMEOUT_MS 10000  /* 10 s — client considered disconnected if no request */

static uint32_t s_last_request_ms = 0;  /* Timestamp of last /data request */
static SemaphoreHandle_t xClientMutex = NULL;

/* Exported: vOledTask reads this to detect web dashboard connection
 * 0 = no client active, 1 = client actively polling /data endpoint */
volatile uint8_t g_ws_client_count = 0;

/* ── SPIFFS init ─────────────────────────────────────────────────────────── */
static void spiffs_init(void)
{
    esp_vfs_spiffs_conf_t cfg = {
        .base_path              = "/web",
        .partition_label        = NULL,
        .max_files              = 4,
        .format_if_mount_failed = false
    };
    esp_err_t ret = esp_vfs_spiffs_register(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_HTTP, "SPIFFS mount FAILED: %s", esp_err_to_name(ret));
        return;
    }
    size_t total = 0, used = 0;
    esp_spiffs_info(NULL, &total, &used);
    ESP_LOGI(TAG_HTTP, "SPIFFS mounted — total:%u KB  used:%u KB",
             (unsigned)(total / 1024), (unsigned)(used / 1024));
}

/* ── Client activity tracking: update on each /data request ─────────────── */
static void client_activity_update(void)
{
    if (xClientMutex == NULL) return;
    
    if (xSemaphoreTake(xClientMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;  /* Could not acquire mutex — skip this update */
    }

    uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    /* If no previous request, or last one was too long ago, this is "first" client */
    if (s_last_request_ms == 0 || (now_ms - s_last_request_ms) > CLIENT_TIMEOUT_MS) {
        if (g_ws_client_count == 0) {
            g_ws_client_count = 1;
            ESP_LOGI(TAG_HTTP, "Client activity detected — g_ws_client_count = 1");
        }
    }
    
    /* Update timestamp for this request */
    s_last_request_ms = now_ms;
    
    xSemaphoreGive(xClientMutex);
}

/* ── Client activity cleanup task: detect timeout after no /data requests ─ */
static void vClientCleanupTask(void *pvParameters)
{
    (void)pvParameters;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));  /* Check every 2 s */

        if (xClientMutex == NULL) continue;

        if (xSemaphoreTake(xClientMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;  /* Could not acquire mutex */
        }

        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        
        /* If last request was more than CLIENT_TIMEOUT_MS ago, client is gone */
        if (s_last_request_ms > 0) {
            uint32_t age_ms = now_ms - s_last_request_ms;
            if (age_ms > CLIENT_TIMEOUT_MS && g_ws_client_count > 0) {
                g_ws_client_count = 0;
                ESP_LOGI(TAG_HTTP, "Client timeout (age:%u ms) — g_ws_client_count = 0",
                         (unsigned)age_ms);
            }
        }

        xSemaphoreGive(xClientMutex);
    }
}

/* ── JSON builder ────────────────────────────────────────────────────────── */
static int build_json(char *buf, size_t bufsz)
{
    int w = snprintf(buf, bufsz,
        "{"
        "\"temp\":%.2f,"
        "\"gas\":%u,"
        "\"noise\":%u,"
        "\"pir\":%u,"
        "\"door\":%u,"
        "\"vibration\":%u,"
        "\"buzzer\":%u,"
        "\"fan\":%u,"
        "\"button1\":%u,"
        "\"button2\":%u,"
        "\"cpu\":%.1f,"
        "\"rtc_time\":\"%s\","
        "\"rtc_date\":\"%s\","
        "\"state\":\"%s\""
        "}",
        g_sensor_data.temp,
        (unsigned)g_sensor_data.gas,
        (unsigned)g_sensor_data.noise,
        (unsigned)g_sensor_data.pir,
        (unsigned)g_sensor_data.door,
        (unsigned)g_sensor_data.vibration,
        (unsigned)g_sensor_data.buzzer,
        (unsigned)g_sensor_data.fan_relay,
        (unsigned)g_sensor_data.button1,
        (unsigned)g_sensor_data.button2,
        g_sensor_data.cpu_usage,
        g_sensor_data.rtc_time,
        g_sensor_data.rtc_date,
        g_sensor_data.state);
    return (w > 0 && w < (int)bufsz) ? w : -1;
}

/* ── GET /  → serve index.html from SPIFFS ──────────────────────────────── */
static esp_err_t http_get_root(httpd_req_t *req)
{
    FILE *f = fopen("/web/index.html", "r");
    if (!f) {
        ESP_LOGE(TAG_HTTP, "index.html not found in SPIFFS — re-flash with: idf.py flash");
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
            "index.html missing — run: idf.py flash");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/html");
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── GET /data  → live JSON polled every 2 s by browser ─────────────────── */
static esp_err_t http_get_data(httpd_req_t *req)
{
    /* Track that a client is active (by monitoring /data requests) */
    client_activity_update();

    char json[HTTP_JSON_BUF];
    int  w = build_json(json, sizeof(json));
    if (w < 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, (ssize_t)w);
}

/* ── GET /health ─────────────────────────────────────────────────────────── */
static esp_err_t http_get_health(httpd_req_t *req)
{
    static const char *body = "{\"status\":\"ok\"}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, (ssize_t)strlen(body));
}

static const httpd_uri_t uri_root = {
    .uri = "/", .method = HTTP_GET, .handler = http_get_root, .user_ctx = NULL
};
static const httpd_uri_t uri_data = {
    .uri = "/data", .method = HTTP_GET, .handler = http_get_data, .user_ctx = NULL
};
static const httpd_uri_t uri_health = {
    .uri = "/health", .method = HTTP_GET, .handler = http_get_health, .user_ctx = NULL
};

/* ── Public init — call from app_main after WiFi is up ──────────────────── */
/* ── Public init — call from app_main after WiFi is up ──────────────────── */
void isms_web_server_init(void)
{
    /* Initialize client tracking mutex */
    if (xClientMutex == NULL) {
        xClientMutex = xSemaphoreCreateMutex();
        if (xClientMutex == NULL) {
            ESP_LOGE(TAG_HTTP, "Failed to create client mutex");
            return;
        }
    }

    /* Start client cleanup task (detects and removes disconnected clients) */
    xTaskCreate(vClientCleanupTask, "CleanupTask", 2048, NULL, 5, NULL);

    spiffs_init();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.max_open_sockets = 6;
    cfg.lru_purge_enable = true;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG_HTTP, "httpd_start failed");
        return;
    }

    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_data);
    httpd_register_uri_handler(s_server, &uri_health);

    ESP_LOGI(TAG_HTTP, "HTTP server ready — port 80");
    ESP_LOGI(TAG_HTTP, "  GET /       -> index.html (SPIFFS)");
    ESP_LOGI(TAG_HTTP, "  GET /data   -> live sensor JSON");
    ESP_LOGI(TAG_HTTP, "  GET /health -> {\"status\":\"ok\"}");
    ESP_LOGI(TAG_HTTP, "  Client tracking enabled — g_ws_client_count updates on connect/disconnect");
}

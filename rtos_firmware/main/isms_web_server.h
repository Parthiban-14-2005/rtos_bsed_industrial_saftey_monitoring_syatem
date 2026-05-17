/*******************************************************************************
 * isms_web_server.h
 * ISMS Web Dashboard — HTTP/80 + WebSocket/81
 * Target  : ESP32 + ESP-IDF v5.3.1
 * Version : 1.1.0
 *
 * FIX-29: rtc_date[] widened from [12] to [14].
 *
 *   Format   : "%02d/%02d/20%02d"  with uint8_t date, month, year
 *   GCC sees : date  in [0,255] → up to 3 chars
 *              month in [0,255] → up to 3 chars
 *              year  in [0,255] → up to 3 chars
 *   Worst-case output : "255/255/20255\0" = 14 bytes  → rtc_date[14] ✓
 *   DS3231 runtime    : "DD/MM/20YY\0"   = 11 bytes  (year register = 00-99)
 *
 * CHANGELOG
 *   1.0.0  Initial release — HTTP dashboard + WebSocket push
 *   1.1.0  FIX-29: rtc_date[12]→[14], rtc_time[10]→[12] (uint8_t worst-case sizing)
 ******************************************************************************/

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "esp_err.h"

/*******************************************************************************
 * Shared sensor data struct
 * Written by vSensorTask, read by the web server and WebSocket push task.
 * All string fields are NUL-terminated.
 ******************************************************************************/
typedef struct {
    /* Analogue / environmental */
    float    temp;          /* °C  — DS3231 or LM35 fallback        */
    uint16_t gas;           /* MQ-2 ADC counts (0-1000 normalised)  */
    uint16_t noise;         /* ADC raw (0-4095)                     */

    /* Digital / security */
    uint8_t  pir;           /* 1 = motion detected                  */
    uint8_t  door;          /* 1 = door open                        */
    uint8_t  vibration;     /* 1 = vibration detected               */

    /* Button / control inputs */
    uint8_t  button1;       /* 1 = button1 pressed (buzzer)         */
    uint8_t  button2;       /* 1 = button2 pressed (fan toggle)     */

    /* Actuator outputs */
    uint8_t  buzzer;        /* 1 = buzzer ON                        */
    uint8_t  fan_relay;     /* 1 = fan relay ON                     */
    uint8_t  siren;         /* legacy field — always 0 in v10+      */
    uint8_t  exhaust;       /* legacy field — always 0 in v10+      */

    /* System metrics */
    float    cpu_usage;     /* 0.0 – 99.9 %                         */

    /* RTC strings — populated by vSensorTask.
     * rtc_time : "HH:MM:SS\0"             →  9 bytes max  → [10]
     * rtc_date : "DD/MM/20YY\0"           → 11 bytes typical
     *            "255/255/20255\0"         → 14 bytes worst-case → [14]
     *   FIX-29: widened from [12] to [14] so GCC -Wformat-truncation
     *           never fires regardless of uint8_t input range.           */
    char     rtc_time[12]; /* FIX-29: widened [10]→[12]. Worst-case "255:255:255\0" = 12 bytes with uint8_t [0,255] args */
    char     rtc_date[14];  /* FIX-29: was [12] */

    /* System state string: "normal" | "warning" | "emergency" | "critical" */
    char     state[12];
} isms_sensor_data_t;

/* Global instance — defined in ISMS_MQTT_v8_0_RTOS_PERF.c */
extern isms_sensor_data_t g_sensor_data;

/* Client tracking counter — updated by HTTP server on connect/disconnect
 * vOledTask checks this to transition to "System Connected" display */
extern volatile uint8_t g_ws_client_count;

/*******************************************************************************
 * Public API
 ******************************************************************************/

/**
 * @brief  Initialise HTTP (port 80) + WebSocket push server.
 *         Must be called after WiFi SoftAP is up.
 *         Spawns an internal task that pushes g_sensor_data as JSON
 *         to all connected WebSocket clients every 2 s.
 */
void isms_web_server_init(void);

#ifdef __cplusplus
}
#endif

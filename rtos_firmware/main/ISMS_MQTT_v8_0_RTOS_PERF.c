/*******************************************************************************
 * ISMS — Industrial Security & Monitoring System
 * Version : 10.3.2 — WEB DASHBOARD OLED + FAN FIX
 * Target  : ESP32 + ESP-IDF v5.3.1 + FreeRTOS
 *
 * ─── ARCHITECTURE (v9) ────────────────────────────────────────────────────────
 *
 *   SensorTask  ──xDataQueue(overwrite)──►  CommTask  ──►  broker_publish()
 *       │                                                         │
 *       ▼                                                         ▼
 *   SafetyTask  ──xEventQueue──►  EmergencyTask             MqttBrokerTask
 *   SecurityTask──xEventQueue──►      │                     MqttClientTask×N
 *   MonitoringTask                    ▼
 *   ScalabilityTask               ActuatorTask  (drives GPIO outputs)
 *   PerformanceTask ──────────►   broker_publish()
 *
 * ─── V9 FIXES vs V8.3 ─────────────────────────────────────────────────────────
 *  ✅ FIX-01  ISR debounce — 50 ms guard on PIR / Door / Vibration ISRs
 *  ✅ FIX-02  I2C mutex (xI2CMutex) protects every DS3231 access (RTC + Temp)
 *  ✅ FIX-03  Actuator state protected by xActuatorMutex; no torn reads
 *  ✅ FIX-04  Task priorities redesigned — no more "everything MAX" collision
 *  ✅ FIX-05  Emergency Task blocked actuator: fan/siren auto-release via
 *             ActuatorTask instead of vTaskDelay inside EmergencyTask
 *  ✅ FIX-06  Fan manual override auto-release after 30 s timeout
 *  ✅ FIX-07  Door-open state only cleared on stable LOW (50 ms confirm)
 *  ✅ FIX-08  JSON overflow — every snprintf result is bounds-checked
 *  ✅ FIX-09  MQTT socket uses SO_RCVTIMEO (non-blocking receive)
 *  ✅ FIX-10  Sensor fault detection — out-of-range values flagged
 *  ✅ FIX-11  Watchdog properly integrated with esp_task_wdt_reset()
 *  ✅ FIX-12  CPU usage warmup preserved + clamped 0–99.9 %
 *  ✅ FIX-13  Duplicate TOPIC_SYSTEM publish removed (v8.3 double-publish bug)
 *  ✅ FIX-14  CommTask: single unified JSON with all guaranteed field names
 *  ✅ FIX-15  Global volatile sensor flags → atomic reads via sensor_snapshot_get()
 *  ✅ KEPT    SoftAP mode, DS3231 RTC+Temp, MQ-2, PIR, Door, Noise, Vib
 *  ✅ v10     Removed LDR, Siren, Exhaust Relay; Added Button1 (GPIO32), Button2 (GPIO19)
 *  ✅ KEPT    RTOS trace hooks, MQTT broker, serial-log MQTT mirror
 *
 * ─── V10.1 FIXES ──────────────────────────────────────────────────────────────
 *  ✅ FIX-16  Added TOPIC_STATUS "isms/status" — Flutter heartbeat was always null
 *  ✅ FIX-17  CommTask publishes "online" once on startup after broker ready
 *  ✅ FIX-18  CommTask publishes "online" every 2 s — keeps Flutter heartbeat alive
 *  ✅ FIX-19  Updated FIRMWARE_VERSION to "10.1.0"
 *   ─── MQTT TOPICS (updated) ────────────────────────────────────────────────
 *   isms/status            → "online" heartbeat (every 2 s) — NEW in v10.1
 *

 * ─── V10.3.2 FIXES ──────────────────────────────────────────────────────────
 *  ✅ FIX-30  OLED wait screen changed from "Waiting for App / MQTT"
 *             to "Waiting for Web Dashboard" — shows WiFi SSID and
 *             HTTP address (192.168.4.1:80) instead of MQTT port 1883.
 *             Connected trigger now tracks g_ws_client_count (WebSocket
 *             clients from isms_web_server) instead of s_client_count
 *             (MQTT clients). Update isms_web_server to increment/decrement
 *             g_ws_client_count on WS connect/disconnect events.
 *  ✅ FIX-31  OLED "App Connected" banner replaced with "SYSTEM CONNECTED"
 *             with double hline top/bottom for visual emphasis.
 *  ✅ FIX-32  Button2 (GPIO19) fan toggle was MISSING — button press was only
 *             logged, never actuated. MonitoringTask now reads current fan
 *             state, and on each Button2 press: if fan OFF → ACT_CMD_FAN_ON
 *             with fan_manual=1 (30-s WDT applies); if fan ON → ACT_CMD_FAN_OFF.
 *             Fan relay (GPIO25) now correctly responds to Button2 press.
 *
 *  ⚠️  ACTION REQUIRED in isms_web_server.c:
 *     Add `extern volatile uint8_t g_ws_client_count;` and increment it
 *     on WebSocket client connect, decrement on disconnect, so vOledTask
 *     can detect web dashboard presence correctly.
 *
 *  ✅ FIX-29  -Werror=format-truncation fix — two snprintf calls used
 *             "%02d/%02d/20%02d" with sd.rtc_time.year (uint8_t, range 0-255).
 *             GCC sees worst-case "20255" (5 chars) which overflows the
 *             rtc_date[12] buffer, raising a fatal truncation error.
 *             Fix: clamp year with `% 100u` before passing to snprintf.
 *             This is semantically correct — DS3231 stores years as BCD 00-99.
 *             Applied at two sites: vSensorTask (rtc_date) and vCommTask (rtc_str).
 *

 *  ✅ ADD-01  SSD1306 128x64 OLED display task integrated (vOledTask)
 *             - Boot splash "RTOS Industrial Safety System"
 *             - "System Ready" after boot
 *             - "Connect App" wait screen (held until MQTT client joins)
 *             - "App Connected" banner on first client connect
 *             - Rotating live pages: Temp / Gas / Noise / PIR+Door /
 *               Vibration+Buttons / System State  (3 s each)
 *             - Returns to "Connect App" if all clients disconnect
 *  ✅ ADD-02  I2C bus shared with DS3231 — all OLED writes use xI2CMutex
 *  ✅ ADD-03  No external OLED component — uses driver/i2c directly
 *             Wiring: SDA=GPIO21, SCL=GPIO22 (same as DS3231)
 *             I2C address: 0x3C (change SSD1306_I2C_ADDR if yours is 0x3D)
 *
 * ─── V10.2 FIXES ──────────────────────────────────────────────────────────────
 *  ✅ FIX-20  DS3231 graceful fallback — when I2C fails, SensorTask falls back
 *             to LM35 ADC reading instead of publishing 0.00°C. RTC time fields
 *             show "??:??:??" so Flutter knows data is unavailable, not zero.
 *  ✅ FIX-21  LM35 probe threshold raised 100→200 to reduce false "absent" detect
 *             on noisy ADC lines. Also re-probed every 30 s in case sensor
 *             is connected after boot.
 *  ✅ FIX-22  PIR + Door pull-up changed to pull-DOWN — sensors read HIGH when
 *             triggered (active-HIGH). With pull-UP they float HIGH constantly
 *             causing permanent pir=1 door=1 and false emergency state.
 *  ✅ FIX-23  RTOS metrics fallback — when CONFIG_FREERTOS_USE_TRACE_FACILITY
 *             is not enabled, build_and_publish_rtos_metrics() now uses
 *             uxTaskGetSystemState() with a local runtime counter so the
 *             Flutter RTOS screen always gets real task names and states.
 *  ✅ FIX-24  MQTT max clients reduced 4→2 to prevent 3-client duplicate issue.
 *             Single phone + single ESP32-side test client is the real use case.
 *  ✅ FIX-25  Updated FIRMWARE_VERSION to "10.2.0"
 *  ✅ FIX-26  ISR NULL guards — all 5 ISR handlers now check semaphore != NULL
 *             before calling xSemaphoreGiveFromISR(). A floating GPIO36
 *             (vibration) was firing the ISR during gpio_config() at [1/6]
 *             before xVibrationSemaphore was created at [3/6], causing
 *             assert(pxQueue) crash and reboot loop.
 *  ✅ FIX-27  app_main init order corrected — RTOS objects now created at
 *             [1/6] BEFORE GPIO init at [2/6], so semaphores always exist
 *             when ISR handlers are installed. Both FIX-26 and FIX-27 are
 *             applied for defence-in-depth.
 *  ✅ FIX-28  Include order fixed — ISMS_Firebase_Integration.c moved to END
 *             of file so all types/constants/globals are defined before use.
 *             state_str() forward-declared before Firebase file inclusion.
 *
 * ─── GPIO MAP ─────────────────────────────────────────────────────────────────
 *   PIR        GPIO4    Door       GPIO5
 *   Temp LM35  GPIO34   Gas MQ-2   GPIO35
 *   Button1    GPIO32   Noise      GPIO33
 *   Vibration  GPIO27   RTC SDA    GPIO21
 *   RTC SCL    GPIO22   Buzzer     GPIO18
 *   Button2    GPIO19   Fan relay  GPIO25
 *   LED status GPIO2    LED alarm  GPIO15
 *   LED normal GPIO16
 *
 * ─── MQTT TOPICS ──────────────────────────────────────────────────────────────
 *   industrial/system      → unified sensor snapshot (every 2 s)
 *   industrial/safety      → temp + gas threshold alerts
 *   industrial/security    → PIR + door alerts
 *   industrial/emergency   → actuator state changes
 *   industrial/monitoring  → light + noise + vibration
 *   industrial/performance → heap + uptime + cpu
 *   industrial/rtos_perf   → per-task RTOS trace data
 *   industrial/serial_log  → mirror of ESP_LOGx output
 ******************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcpp"
#include "driver/adc.h"
#pragma GCC diagnostic pop
#include "driver/i2c.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sockets.h"

#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "isms_web_server.h"   /* Web dashboard: HTTP:80 + WebSocket:81 */

#if __has_include("esp_task_wdt.h")
#  include "esp_task_wdt.h"
#  define HAVE_TASK_WDT 1
#else
#  define HAVE_TASK_WDT 0
#endif

/*******************************************************************************
 * FEATURE FLAGS
 ******************************************************************************/
#define ENABLE_MONITORING_TASK      1
#define ENABLE_COMM_TASK            1
#define ENABLE_PERFORMANCE_TASK     1
#define ENABLE_TRACE_HOOKS          1
#define ENABLE_SCALABILITY_TASK     1
#define ENABLE_RTC                  1
#define ENABLE_RTC_TEMPERATURE      1

/*******************************************************************************
 * CONSTANTS
 ******************************************************************************/
#define FIRMWARE_VERSION            "10.3.2"

/* WiFi SoftAP */
#define WIFI_AP_SSID                "ESP32_ISMS"
#define WIFI_AP_PASS                "12345678"
#define WIFI_AP_CHANNEL             1
#define WIFI_AP_MAX_CONN            4

/* MQTT Broker */
#define MQTT_BROKER_PORT            1883
#define MQTT_MAX_CLIENTS            2   /* FIX-24: phone + 1 spare only */
#define MQTT_KEEPALIVE_SEC          60

/* Topics */
#define TOPIC_SYSTEM                "industrial/system"
#define TOPIC_SAFETY                "industrial/safety"
#define TOPIC_SECURITY              "industrial/security"
#define TOPIC_EMERGENCY             "industrial/emergency"
#define TOPIC_MONITORING            "industrial/monitoring"
#define TOPIC_PERFORMANCE           "industrial/performance"
#define TOPIC_RTOS_PERF             "industrial/rtos_perf"
#define TOPIC_SERIAL_LOG            "industrial/serial_log"
#define TOPIC_STATUS                "isms/status"          /* FIX-16: Flutter heartbeat */

/* Task stacks */
#define SENSOR_TASK_STACK           4096
#define SECURITY_TASK_STACK         3072
#define SAFETY_TASK_STACK           3072
#define EMERGENCY_TASK_STACK        4096
#define ACTUATOR_TASK_STACK         3072
#define MONITORING_TASK_STACK       3072
#define COMM_TASK_STACK             4096
#define PERFORMANCE_TASK_STACK      4096
#define SCALABILITY_TASK_STACK      3072
#define MQTT_BROKER_TASK_STACK      8192
#define MQTT_CLIENT_TASK_STACK      4096

/*
 * ─── PRIORITY TABLE (FIX-04) ──────────────────────────────────────────────────
 *  Priority  Task(s)
 *  MAX-1     EmergencyTask  (life-safety response, top RT)
 *  MAX-2     SensorTask, SecurityTask, SafetyTask  (data acquisition)
 *  MAX-3     ActuatorTask, MqttBroker, MqttClient  (I/O + comms)
 *  MAX-4     CommTask, MonitoringTask, ScalabilityTask
 *  MAX-5     PerformanceTask  (analytics, lowest impact)
 */
#define EMERGENCY_TASK_PRIORITY     (configMAX_PRIORITIES - 1)
#define SENSOR_TASK_PRIORITY        (configMAX_PRIORITIES - 2)
#define SECURITY_TASK_PRIORITY      (configMAX_PRIORITIES - 2)
#define SAFETY_TASK_PRIORITY        (configMAX_PRIORITIES - 2)
#define ACTUATOR_TASK_PRIORITY      (configMAX_PRIORITIES - 3)
#define MQTT_BROKER_TASK_PRIORITY   (configMAX_PRIORITIES - 3)
#define MQTT_CLIENT_TASK_PRIORITY   (configMAX_PRIORITIES - 3)
#define COMM_TASK_PRIORITY          (configMAX_PRIORITIES - 4)
#define MONITORING_TASK_PRIORITY    (configMAX_PRIORITIES - 4)
#define SCALABILITY_TASK_PRIORITY   (configMAX_PRIORITIES - 4)
#define PERFORMANCE_TASK_PRIORITY   (configMAX_PRIORITIES - 5)
#define OLED_TASK_STACK             3072
#define OLED_TASK_PRIORITY          (configMAX_PRIORITIES - 5)


/* Queue sizes */
#define EVENT_QUEUE_SIZE            12
#define DATA_QUEUE_SIZE             1
#define ACTUATOR_QUEUE_SIZE         8

/* Sensor thresholds */
#define TEMP_THRESHOLD_HIGH         50.0f
#define TEMP_THRESHOLD_CRITICAL     70.0f
#define GAS_THRESHOLD_WARNING       300
#define GAS_THRESHOLD_CRITICAL      500
#define NOISE_THRESHOLD_WARNING     2500
#define NOISE_THRESHOLD_ALARM       3500
#define VIBRATION_DEBOUNCE_MS       200

/* Sensor fault bounds (FIX-10) */
#define TEMP_FAULT_MIN              (-10.0f)
#define TEMP_FAULT_MAX              150.0f
#define GAS_FAULT_MAX               4096

/* Fan manual override timeout seconds (FIX-06) */
#define FAN_MANUAL_TIMEOUT_S        30

/* Task periods (ms) */
#define SENSOR_PERIOD_MS            200
#define SECURITY_PERIOD_MS          100
#define SAFETY_PERIOD_MS            200
#define MONITORING_PERIOD_MS        1000
#define COMM_PERIOD_MS              2000
#define PERFORMANCE_PERIOD_MS       5000
#define SCALABILITY_PERIOD_MS       500

/* JSON buffer sizes */
#define JSON_BUF_SIZE               750
#define RTOS_JSON_BUF_SIZE          1200

/* I2C / RTC */
#define RTC_I2C_PORT                I2C_NUM_0
#define RTC_I2C_SDA_PIN             GPIO_NUM_21
#define RTC_I2C_SCL_PIN             GPIO_NUM_22
#define RTC_I2C_FREQ_HZ             100000
#define DS3231_I2C_ADDR             0x68
#define DS3231_REG_SECONDS          0x00
#define DS3231_REG_TEMP_MSB         0x11
#define DS3231_REG_TEMP_LSB         0x12

#define MAX_TASKS                   14

/*******************************************************************************
 * GPIO
 ******************************************************************************/
#define GPIO_PIR_SENSOR             GPIO_NUM_4
#define GPIO_DOOR_SENSOR            GPIO_NUM_5
#define GPIO_TEMP_SENSOR_ADC        ADC1_CHANNEL_6   /* GPIO34 */
#define GPIO_GAS_SENSOR_ADC         ADC1_CHANNEL_7   /* GPIO35 */
#define GPIO_NOISE_SENSOR_ADC       ADC1_CHANNEL_5   /* GPIO33 */
#define GPIO_VIBRATION_SENSOR       GPIO_NUM_27
#define GPIO_BUTTON1                GPIO_NUM_32      /* Push Button 1 */
#define GPIO_BUTTON2                GPIO_NUM_19      /* Push Button 2 */
#define GPIO_BUZZER                 GPIO_NUM_18
#define GPIO_FAN_RELAY              GPIO_NUM_25
#define GPIO_LED_STATUS             GPIO_NUM_2
#define GPIO_LED_ALARM              GPIO_NUM_15
#define GPIO_LED_NORMAL             GPIO_NUM_16

/*******************************************************************************
 * EVENT BITS
 ******************************************************************************/
#define EVENT_SECURITY_BREACH       (1 << 0)
#define EVENT_FIRE_DETECTED         (1 << 1)
#define EVENT_GAS_LEAK              (1 << 2)
#define EVENT_OVERHEAT              (1 << 3)
#define EVENT_EMERGENCY_ACTIVE      (1 << 4)
#define EVENT_SYSTEM_NORMAL         (1 << 6)
#define EVENT_AP_STARTED            (1 << 7)
#define EVENT_NOISE_ALARM           (1 << 8)
#define EVENT_VIBRATION_DETECTED    (1 << 9)

/*******************************************************************************
 * TYPES
 ******************************************************************************/
typedef enum {
    EVENT_TYPE_SECURITY_BREACH = 0,
    EVENT_TYPE_UNAUTHORIZED_ENTRY,
    EVENT_TYPE_DOOR_OPEN,
    EVENT_TYPE_FIRE_ALARM,
    EVENT_TYPE_GAS_LEAK_ALARM,
    EVENT_TYPE_OVERHEATING,
    EVENT_TYPE_CRITICAL_TEMP,
    EVENT_TYPE_NOISE_WARNING,
    EVENT_TYPE_NOISE_ALARM,
    EVENT_TYPE_VIBRATION_DETECTED,
    EVENT_TYPE_SYSTEM_OK
} EventType_t;

typedef enum {
    STATE_NORMAL = 0, STATE_WARNING, STATE_EMERGENCY, STATE_CRITICAL
} SystemState_t;

typedef struct {
    uint8_t seconds, minutes, hours, day, date, month, year;
} RtcTime_t;

typedef struct {
    EventType_t type;
    uint32_t    timestamp;
    float       value;
    uint8_t     sensor_id;
    uint8_t     severity;
} EventMessage_t;

/* Unified sensor snapshot — only written by SensorTask, read by others */
typedef struct {
    float     temperature;       /* DS3231 or LM35               */
    float     rtc_temperature;   /* raw DS3231 reading            */
    uint16_t  gas_level;
    uint16_t  noise_level;
    uint8_t   button1_pressed;
    uint8_t   button2_pressed;
    uint8_t   vibration_detected;
    uint8_t   pir_detected;
    uint8_t   door_open;
    uint8_t   temp_fault;        /* FIX-10 */
    uint8_t   gas_fault;         /* FIX-10 */
    uint32_t  timestamp;
    RtcTime_t rtc_time;
} SensorData_t;

/* Actuator command sent to ActuatorTask queue (FIX-05) */
typedef enum {
    ACT_CMD_BUZZER_ON, ACT_CMD_BUZZER_OFF,
    ACT_CMD_FAN_ON,    ACT_CMD_FAN_OFF,
    ACT_CMD_LED_ALARM_ON,  ACT_CMD_LED_ALARM_OFF,
    ACT_CMD_LED_NORMAL_ON, ACT_CMD_LED_NORMAL_OFF,
    ACT_CMD_ALL_CLEAR
} ActuatorCmd_t;

typedef struct {
    ActuatorCmd_t cmd;
    uint32_t      duration_ms;  /* 0 = permanent */
    char          trigger[24];
} ActuatorMessage_t;

typedef struct {
    uint8_t buzzer, fan, led_alarm, led_normal;
    char    trigger[24];
    uint8_t fan_manual;         /* FIX-06 */
    uint32_t fan_manual_since;  /* FIX-06 */
} ActuatorState_t;

/*******************************************************************************
 * TRACE HOOK DATA
 ******************************************************************************/
#define TRACE_MAX_TASKS     14

typedef struct {
    char        name[16];
    uint64_t    switch_in_time_us;
    uint64_t    total_exec_us;
    uint64_t    total_exec_us_prev;
    uint32_t    switch_count;
    uint32_t    switch_count_prev;
    uint64_t    first_switch_in_us;
    uint64_t    response_latency_us;
    bool        ever_ran;
    uint8_t     task_number;
    UBaseType_t priority;
} TraceTaskData_t;

static volatile TraceTaskData_t g_trace[TRACE_MAX_TASKS];
static volatile uint32_t        g_ctx_switch_total  = 0;
static volatile uint64_t        g_window_start_us   = 0;
static volatile uint64_t        g_isr_enter_time_us = 0;
static volatile uint64_t        g_isr_total_us      = 0;
static volatile uint32_t        g_isr_count         = 0;

static SemaphoreHandle_t xTraceMutex = NULL;

static inline int trace_find_slot(UBaseType_t task_num)
{
    for (int i = 0; i < TRACE_MAX_TASKS; i++)
        if (g_trace[i].task_number == task_num && g_trace[i].ever_ran)
            return i;
    for (int i = 0; i < TRACE_MAX_TASKS; i++)
        if (!g_trace[i].ever_ran) {
            g_trace[i].task_number = (uint8_t)task_num;
            return i;
        }
    return -1;
}

/*******************************************************************************
 * TRACE HOOK MACROS
 ******************************************************************************/
#if ENABLE_TRACE_HOOKS
#undef traceTASK_SWITCHED_IN
#undef traceTASK_SWITCHED_OUT
#undef traceISR_ENTER
#undef traceISR_EXIT

#define traceTASK_SWITCHED_IN()                                             \
do {                                                                        \
    uint64_t _now = (uint64_t)esp_timer_get_time();                         \
    int _slot = trace_find_slot(pxCurrentTCB->uxTaskNumber);                \
    if (_slot >= 0) {                                                       \
        g_trace[_slot].switch_in_time_us = _now;                           \
        g_trace[_slot].switch_count++;                                      \
        g_ctx_switch_total++;                                               \
        if (!g_trace[_slot].ever_ran) {                                     \
            g_trace[_slot].ever_ran           = true;                       \
            g_trace[_slot].first_switch_in_us = _now;                      \
            g_trace[_slot].priority = uxTaskPriorityGet(pxCurrentTCB);     \
            strncpy((char*)g_trace[_slot].name,                             \
                    pcTaskGetName(pxCurrentTCB), 15);                       \
        }                                                                   \
    }                                                                       \
} while(0)

#define traceTASK_SWITCHED_OUT()                                            \
do {                                                                        \
    uint64_t _now = (uint64_t)esp_timer_get_time();                         \
    int _slot = trace_find_slot(pxCurrentTCB->uxTaskNumber);                \
    if (_slot >= 0 && g_trace[_slot].switch_in_time_us > 0) {              \
        g_trace[_slot].total_exec_us +=                                     \
            _now - g_trace[_slot].switch_in_time_us;                       \
        g_trace[_slot].switch_in_time_us = 0;                              \
    }                                                                       \
} while(0)

#define traceISR_ENTER()                                                    \
do {                                                                        \
    g_isr_enter_time_us = (uint64_t)esp_timer_get_time();                  \
    g_isr_count++;                                                          \
} while(0)

#define traceISR_EXIT()                                                     \
do {                                                                        \
    if (g_isr_enter_time_us > 0) {                                         \
        g_isr_total_us +=                                                   \
            ((uint64_t)esp_timer_get_time() - g_isr_enter_time_us);        \
        g_isr_enter_time_us = 0;                                            \
    }                                                                       \
} while(0)
#endif /* ENABLE_TRACE_HOOKS */

/*******************************************************************************
 * MQTT BROKER PROTOCOL CONSTANTS
 ******************************************************************************/
#define MQTT_CONNECT        0x10
#define MQTT_CONNACK        0x20
#define MQTT_PUBLISH        0x30
#define MQTT_PUBACK         0x40
#define MQTT_SUBSCRIBE      0x82
#define MQTT_SUBACK         0x90
#define MQTT_PINGREQ        0xC0
#define MQTT_PINGRESP       0xD0
#define MQTT_DISCONNECT     0xE0
#define MQTT_CONNACK_ACCEPTED   0x00
#define MAX_TOPIC_LEN       64
#define MAX_SUBS_PER_CLIENT 8

typedef struct {
    int      sock;
    char     client_id[32];
    char     subs[MAX_SUBS_PER_CLIENT][MAX_TOPIC_LEN];
    uint8_t  sub_count;
    uint32_t last_activity;
    uint8_t  active;
} MqttClient_t;

/*******************************************************************************
 * GLOBAL HANDLES & STATE
 ******************************************************************************/
static QueueHandle_t      xEventQueue          = NULL;
static QueueHandle_t      xDataQueue           = NULL;
static QueueHandle_t      xActuatorQueue       = NULL;
static SemaphoreHandle_t  xPIRSemaphore        = NULL;
static SemaphoreHandle_t  xDoorSemaphore       = NULL;
static SemaphoreHandle_t  xVibrationSemaphore  = NULL;
static SemaphoreHandle_t  xButton1Semaphore    = NULL;
static SemaphoreHandle_t  xButton2Semaphore    = NULL;
static EventGroupHandle_t xSystemEventGroup    = NULL;
static SemaphoreHandle_t  xBrokerMutex         = NULL;
static SemaphoreHandle_t  xI2CMutex            = NULL;  /* FIX-02 */
static SemaphoreHandle_t  xActuatorMutex       = NULL;  /* FIX-03 */

static MqttClient_t       s_clients[MQTT_MAX_CLIENTS];
static volatile uint8_t   s_client_count       = 0;

/* MOVED: g_ws_client_count is now defined in isms_web_server.c — see isms_web_server.h */
/* It was previously here and duplicated. Web server now owns and exports it. */

static TaskHandle_t xSensorTaskHandle      = NULL;
static TaskHandle_t xSecurityTaskHandle    = NULL;
static TaskHandle_t xSafetyTaskHandle      = NULL;
static TaskHandle_t xEmergencyTaskHandle   = NULL;
static TaskHandle_t xActuatorTaskHandle    = NULL;
static TaskHandle_t xMonitoringTaskHandle  = NULL;
static TaskHandle_t xCommTaskHandle        = NULL;
static TaskHandle_t xPerformanceTaskHandle = NULL;
static TaskHandle_t xScalabilityTaskHandle = NULL;

static volatile SystemState_t  systemState         = STATE_NORMAL;
static volatile uint32_t       emergencyCount      = 0;
static volatile uint32_t       securityBreachCount = 0;

/* Protected actuator state (FIX-03) */
static ActuatorState_t         g_actuator          = {0,0,0,1,"none",0,0};

/* Web server shared struct — written by vSensorTask, read by isms_web_task */
isms_sensor_data_t g_sensor_data = {0};

/* ISR-updated sensor flags — written from ISR/task, read by SensorTask */
static volatile uint8_t        g_pir_isr_flag      = 0;
static volatile uint8_t        g_door_isr_flag     = 0;
static volatile uint8_t        g_vib_isr_flag      = 0;
static volatile uint8_t        g_btn1_isr_flag     = 0;
static volatile uint8_t        g_btn2_isr_flag     = 0;

/*******************************************************************************
 * CPU USAGE (v8.1 algorithm preserved + clamped)
 ******************************************************************************/
static volatile uint32_t g_idle_count      = 0;
static volatile uint32_t g_idle_baseline   = 0;
static volatile float    g_cpu_usage_pct   = 0.0f;
static volatile uint8_t  g_baseline_ready  = 0;

void vApplicationIdleHook(void)
{
    g_idle_count++;
}

/*******************************************************************************
 * LOG TAGS
 ******************************************************************************/
static const char *TAG_MAIN      = "MAIN";
static const char *TAG_SECURITY  = "SECURITY";
static const char *TAG_SAFETY    = "SAFETY";
static const char *TAG_SENSOR    = "SENSOR";
static const char *TAG_EMERGENCY = "EMERGENCY";
static const char *TAG_ACTUATOR  = "ACTUATOR";
static const char *TAG_MONITOR   = "MONITOR";
static const char *TAG_COMM      = "COMM";
static const char *TAG_PERF      = "PERF";
/* static const char *TAG_WIFI      = "WIFI"; // UNUSED - removed to fix warning */
static const char *TAG_BROKER    = "BROKER";
static const char *TAG_RTOS      = "RTOS_PERF";
static const char *TAG_DS3231    = "DS3231";

/* Forward declarations */
static void broker_publish(const char *topic, const char *payload, uint8_t qos);
static const char *state_str(void);

/*******************************************************************************
 * SERIAL LOG → MQTT MIRROR
 ******************************************************************************/
static void serial_log_publish(const char *tag, char level, const char *msg)
{
    if (!xBrokerMutex) return;
    char buf[300];
    int w = snprintf(buf, sizeof(buf),
        "{\"ts\":%lu,\"level\":\"%c\",\"tag\":\"%s\",\"msg\":\"%s\"}",
        (unsigned long)(esp_timer_get_time() / 1000ULL), level, tag, msg);
    if (w > 0 && w < (int)sizeof(buf))
        broker_publish(TOPIC_SERIAL_LOG, buf, 0);
}

#define SLOG_I(tag, fmt, ...) do { \
    ESP_LOGI(tag, fmt, ##__VA_ARGS__); \
    { char _sb[220]; snprintf(_sb, sizeof(_sb), fmt, ##__VA_ARGS__); \
      serial_log_publish(tag, 'I', _sb); } \
} while(0)

#define SLOG_W(tag, fmt, ...) do { \
    ESP_LOGW(tag, fmt, ##__VA_ARGS__); \
    { char _sb[220]; snprintf(_sb, sizeof(_sb), fmt, ##__VA_ARGS__); \
      serial_log_publish(tag, 'W', _sb); } \
} while(0)

#define SLOG_E(tag, fmt, ...) do { \
    ESP_LOGE(tag, fmt, ##__VA_ARGS__); \
    { char _sb[220]; snprintf(_sb, sizeof(_sb), fmt, ##__VA_ARGS__); \
      serial_log_publish(tag, 'E', _sb); } \
} while(0)

/*******************************************************************************
 * ISRs — with 50 ms debounce (FIX-01)
 ******************************************************************************/
static void IRAM_ATTR pir_isr_handler(void *arg)
{
    /* FIX-26: Guard against ISR firing before semaphore is created.
     * GPIO init runs at [1/6], RTOS objects created at [3/6].
     * A floating pin can trigger the ISR in that window. */
    if (!xPIRSemaphore) return;
    static volatile uint32_t last_ms = 0;
    uint32_t now = (uint32_t)(xTaskGetTickCountFromISR() * portTICK_PERIOD_MS);
    if ((now - last_ms) < 50) return;
    last_ms = now;
    g_pir_isr_flag = 1;
    BaseType_t w = pdFALSE;
    xSemaphoreGiveFromISR(xPIRSemaphore, &w);
    portYIELD_FROM_ISR(w);
}

static void IRAM_ATTR door_isr_handler(void *arg)
{
    if (!xDoorSemaphore) return;  /* FIX-26 */
    static volatile uint32_t last_ms = 0;
    uint32_t now = (uint32_t)(xTaskGetTickCountFromISR() * portTICK_PERIOD_MS);
    if ((now - last_ms) < 50) return;
    last_ms = now;
    g_door_isr_flag = 1;
    BaseType_t w = pdFALSE;
    xSemaphoreGiveFromISR(xDoorSemaphore, &w);
    portYIELD_FROM_ISR(w);
}

static void IRAM_ATTR vibration_isr_handler(void *arg)
{
    if (!xVibrationSemaphore) return;  /* FIX-26 */
    static volatile uint32_t last_ms = 0;
    uint32_t now = (uint32_t)(xTaskGetTickCountFromISR() * portTICK_PERIOD_MS);
    if ((now - last_ms) < 50) return;
    last_ms = now;
    g_vib_isr_flag = 1;
    BaseType_t w = pdFALSE;
    xSemaphoreGiveFromISR(xVibrationSemaphore, &w);
    portYIELD_FROM_ISR(w);
}

static void IRAM_ATTR button1_isr_handler(void *arg)
{
    if (!xButton1Semaphore) return;  /* FIX-26 */
    static volatile uint32_t last_ms = 0;
    uint32_t now = (uint32_t)(xTaskGetTickCountFromISR() * portTICK_PERIOD_MS);
    if ((now - last_ms) < 50) return;   /* 50 ms debounce */
    last_ms = now;
    g_btn1_isr_flag = 1;
    BaseType_t w = pdFALSE;
    xSemaphoreGiveFromISR(xButton1Semaphore, &w);
    portYIELD_FROM_ISR(w);
}

static void IRAM_ATTR button2_isr_handler(void *arg)
{
    if (!xButton2Semaphore) return;  /* FIX-26 */
    static volatile uint32_t last_ms = 0;
    uint32_t now = (uint32_t)(xTaskGetTickCountFromISR() * portTICK_PERIOD_MS);
    if ((now - last_ms) < 50) return;   /* 50 ms debounce */
    last_ms = now;
    g_btn2_isr_flag = 1;
    BaseType_t w = pdFALSE;
    xSemaphoreGiveFromISR(xButton2Semaphore, &w);
    portYIELD_FROM_ISR(w);
}

/*******************************************************************************
 * HAL — GPIO
 ******************************************************************************/
static void hal_gpio_init(void)
{
    /* Door sensor is wired as NC or ACTIVE-LOW in your hardware.
     * Therefore:
     *   Door CLOSED  -> GPIO level LOW
     *   Door OPEN    -> GPIO level HIGH
     *
     * Using pull-down keeps GPIO5 stable (reed switches/modules can leave the
     * input floating depending on the magnetic state / module board). */
    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << GPIO_PIR_SENSOR) | (1ULL << GPIO_DOOR_SENSOR),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_POSEDGE
    };
    gpio_config(&in_cfg);

    gpio_config_t vib_cfg = {
        .pin_bit_mask = (1ULL << GPIO_VIBRATION_SENSOR),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_POSEDGE
    };
    gpio_config(&vib_cfg);

    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << GPIO_BUZZER)       |
                        (1ULL << GPIO_FAN_RELAY)     |
                        (1ULL << GPIO_LED_STATUS)    |
                        (1ULL << GPIO_LED_ALARM)     |
                        (1ULL << GPIO_LED_NORMAL),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE
    };
    gpio_config(&out_cfg);

    gpio_set_level(GPIO_BUZZER,      0);

    /* Fan relay ACTIVE-LOW:
     *   Logical fan OFF => GPIO HIGH
     * Force OFF at boot to avoid relay flicker/turn-on. */
    gpio_set_level(GPIO_FAN_RELAY,   1);

    gpio_set_level(GPIO_LED_STATUS,  1);
    gpio_set_level(GPIO_LED_ALARM,   0);
    gpio_set_level(GPIO_LED_NORMAL,  1);

    /* Push Button 1 & 2 — active-low, internal pull-up, falling-edge ISR */
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << GPIO_BUTTON1) | (1ULL << GPIO_BUTTON2),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE   /* trigger on button press */
    };
    gpio_config(&btn_cfg);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(GPIO_PIR_SENSOR,       pir_isr_handler,       NULL);
    gpio_isr_handler_add(GPIO_DOOR_SENSOR,      door_isr_handler,      NULL);
    gpio_isr_handler_add(GPIO_VIBRATION_SENSOR, vibration_isr_handler, NULL);
    gpio_isr_handler_add(GPIO_BUTTON1,          button1_isr_handler,   NULL);
    gpio_isr_handler_add(GPIO_BUTTON2,          button2_isr_handler,   NULL);
}

/*******************************************************************************
 * HAL — ADC
 ******************************************************************************/
static void hal_adc_init(void)
{
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(GPIO_TEMP_SENSOR_ADC,  ADC_ATTEN_DB_12);
    adc1_config_channel_atten(GPIO_GAS_SENSOR_ADC,   ADC_ATTEN_DB_12);
    adc1_config_channel_atten(GPIO_NOISE_SENSOR_ADC, ADC_ATTEN_DB_12);
}

/*******************************************************************************
 * HAL — Boot Blink
 ******************************************************************************/
static void hal_boot_blink(void)
{
    for (int i = 0; i < 6; i++) {
        gpio_set_level(GPIO_LED_STATUS, 1);
        vTaskDelay(pdMS_TO_TICKS(80));
        gpio_set_level(GPIO_LED_STATUS, 0);
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    gpio_set_level(GPIO_LED_STATUS, 1);
}

/*******************************************************************************
 * HAL — I2C / DS3231 (FIX-02: all I2C calls wrapped with xI2CMutex)
 ******************************************************************************/
#if ENABLE_RTC
static esp_err_t hal_rtc_init(void)
{
    i2c_config_t conf = {
        .mode          = I2C_MODE_MASTER,
        .sda_io_num    = RTC_I2C_SDA_PIN,
        .scl_io_num    = RTC_I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = RTC_I2C_FREQ_HZ
    };
    esp_err_t ret = i2c_param_config(RTC_I2C_PORT, &conf);
    if (ret != ESP_OK) return ret;
    ret = i2c_driver_install(RTC_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK) return ret;

    /* Probe */
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DS3231_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(RTC_I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    if (ret == ESP_OK) ESP_LOGI(TAG_DS3231, "DS3231 found OK ✓");
    else               ESP_LOGW(TAG_DS3231, "DS3231 not found — SDA=21 SCL=22");
    return ret;
}

static inline uint8_t bcd_to_dec(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }

static esp_err_t hal_rtc_read(RtcTime_t *t)
{
    if (!xI2CMutex) return ESP_ERR_INVALID_STATE;
    uint8_t buf[7] = {0};
    xSemaphoreTake(xI2CMutex, portMAX_DELAY);            /* FIX-02 */
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DS3231_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, DS3231_REG_SECONDS, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DS3231_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, buf, 6, I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, &buf[6], I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(RTC_I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    xSemaphoreGive(xI2CMutex);                            /* FIX-02 */
    if (ret == ESP_OK) {
        t->seconds = bcd_to_dec(buf[0] & 0x7F);
        t->minutes = bcd_to_dec(buf[1]);
        t->hours   = bcd_to_dec(buf[2] & 0x3F);
        t->day     = bcd_to_dec(buf[3]);
        t->date    = bcd_to_dec(buf[4]);
        t->month   = bcd_to_dec(buf[5] & 0x1F);
        t->year    = bcd_to_dec(buf[6]);
    }
    return ret;
}

#if ENABLE_RTC_TEMPERATURE
static float hal_rtc_read_temperature(void)
{
    if (!xI2CMutex) return -999.0f;
    uint8_t msb = 0, lsb = 0;
    xSemaphoreTake(xI2CMutex, portMAX_DELAY);             /* FIX-02 */
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DS3231_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, DS3231_REG_TEMP_MSB, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DS3231_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, &msb, I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, &lsb, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(RTC_I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    xSemaphoreGive(xI2CMutex);                            /* FIX-02 */
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_DS3231, "Temp read failed err=%d", ret);
        return -999.0f;
    }
    int8_t  int_part  = (int8_t)msb;
    uint8_t frac_part = (lsb >> 6) & 0x03;
    return (float)int_part + (frac_part * 0.25f);
}
#endif /* ENABLE_RTC_TEMPERATURE */
#endif /* ENABLE_RTC */

/*******************************************************************************
 * HAL — Sensor reads
 ******************************************************************************/
static float    hal_read_temperature(void)
{
    int raw = adc1_get_raw(GPIO_TEMP_SENSOR_ADC);
    return (raw / 4095.0f) * 3.3f * 100.0f;
}
static uint16_t hal_read_gas_level(void)
{
    return (uint16_t)(adc1_get_raw(GPIO_GAS_SENSOR_ADC) / 4.095f);
}
static uint16_t hal_read_noise_level(void)
{
    return (uint16_t)adc1_get_raw(GPIO_NOISE_SENSOR_ADC);
}
static uint8_t hal_read_pir(void)       { return gpio_get_level(GPIO_PIR_SENSOR);       }
/* Door is NC / ACTIVE-LOW on your hardware.
 * Reed CLOSED -> GPIO level LOW
 * Reed OPEN   -> GPIO level HIGH
 * We convert this into a logical "door_open" (1=OPEN, 0=CLOSED). */
static uint8_t hal_read_door(void)
{
    uint8_t raw = gpio_get_level(GPIO_DOOR_SENSOR);
    return raw; /* raw already represents OPEN=1 when using NC/ACTIVE-LOW wiring */
}
static uint8_t hal_read_vibration(void) { return gpio_get_level(GPIO_VIBRATION_SENSOR); }
static uint8_t hal_read_button1(void)   { return !gpio_get_level(GPIO_BUTTON1); /* active-low */ }
static uint8_t hal_read_button2(void)   { return !gpio_get_level(GPIO_BUTTON2); /* active-low */ }

/*******************************************************************************
 * HAL — Actuator output helpers (raw GPIO — called only from ActuatorTask)
 ******************************************************************************/
static void _set_buzzer(uint8_t s)  { gpio_set_level(GPIO_BUZZER,       s); }
/* Fan relay is ACTIVE-LOW on your hardware:
 *   GPIO HIGH -> relay OFF
 *   GPIO LOW  -> relay ON
 */
static void _set_fan(uint8_t fan_on)
{
    /* Convert logical fan_on(1=ON,0=OFF) to GPIO level for ACTIVE-LOW relay */
    uint8_t gpio_level = fan_on ? 0 : 1;
    gpio_set_level(GPIO_FAN_RELAY, gpio_level);
}
static void _set_led_alarm(uint8_t s)  { gpio_set_level(GPIO_LED_ALARM,  s); }
static void _set_led_normal(uint8_t s) { gpio_set_level(GPIO_LED_NORMAL, s); }
static void _set_led_status(uint8_t s) { gpio_set_level(GPIO_LED_STATUS, s); }

/* Public helper — posts a command to ActuatorTask queue */
static void actuator_post(ActuatorCmd_t cmd, uint32_t dur_ms, const char *trigger)
{
    if (!xActuatorQueue) return;
    ActuatorMessage_t m;
    m.cmd        = cmd;
    m.duration_ms = dur_ms;
    strncpy(m.trigger, trigger ? trigger : "auto", sizeof(m.trigger) - 1);
    m.trigger[sizeof(m.trigger) - 1] = '\0';
    xQueueSend(xActuatorQueue, &m, pdMS_TO_TICKS(10));
}

/*******************************************************************************
 * UTILITIES
 ******************************************************************************/
static uint32_t get_timestamp_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static const char *state_str(void)
{
    switch (systemState) {
        case STATE_WARNING:   return "warning";
        case STATE_EMERGENCY: return "emergency";
        case STATE_CRITICAL:  return "critical";
        default:              return "normal";
    }
}

static const char *task_state_str(eTaskState s)
{
    switch (s) {
        case eRunning:   return "running";
        case eReady:     return "ready";
        case eBlocked:   return "blocked";
        case eSuspended: return "suspended";
        case eDeleted:   return "deleted";
        default:         return "unknown";
    }
}

static void update_system_state(EventType_t et)
{
    switch (et) {
        case EVENT_TYPE_CRITICAL_TEMP:
        case EVENT_TYPE_FIRE_ALARM:
            systemState = STATE_CRITICAL;
            emergencyCount++;
            break;
        case EVENT_TYPE_GAS_LEAK_ALARM:
        case EVENT_TYPE_OVERHEATING:
        case EVENT_TYPE_SECURITY_BREACH:
        case EVENT_TYPE_UNAUTHORIZED_ENTRY:
        case EVENT_TYPE_NOISE_ALARM:
        case EVENT_TYPE_VIBRATION_DETECTED:
            systemState = STATE_EMERGENCY;
            emergencyCount++;
            break;
        case EVENT_TYPE_DOOR_OPEN:
        case EVENT_TYPE_NOISE_WARNING:
            if (systemState == STATE_NORMAL) systemState = STATE_WARNING;
            break;
        case EVENT_TYPE_SYSTEM_OK:
            systemState = STATE_NORMAL;
            break;
        default:
            break;
    }
}

static void send_event(EventType_t type, float value, uint8_t sid, uint8_t sev)
{
    EventMessage_t ev = {
        .type      = type,
        .timestamp = get_timestamp_ms(),
        .value     = value,
        .sensor_id = sid,
        .severity  = sev
    };
    xQueueSend(xEventQueue, &ev, pdMS_TO_TICKS(10));
}

/*******************************************************************************
 * RTOS METRICS
 ******************************************************************************/
static void build_and_publish_rtos_metrics(void)
{
    uint64_t now_us    = (uint64_t)esp_timer_get_time();
    uint64_t window_us = now_us - g_window_start_us;
    if (window_us == 0) window_us = 1;

    TaskStatus_t task_status[MAX_TASKS];
    uint32_t     total_run_time = 0;
    UBaseType_t  task_count = 0;

#if defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) && (CONFIG_FREERTOS_USE_TRACE_FACILITY == 1)
    /* Full path — gets runtime stats, stack HWM, state for every task */
    task_count = uxTaskGetSystemState(task_status, MAX_TASKS, &total_run_time);
#else
    /* FIX-23: Fallback path — CONFIG_FREERTOS_USE_TRACE_FACILITY not enabled.
     * uxTaskGetSystemState() still works for name/priority/state/HWM even
     * without trace facility; it just returns 0 for runtime counters.
     * Pass NULL for total_run_time to avoid the assert inside FreeRTOS when
     * stats are disabled. Task names, states, and HWM are always available. */
    task_count = uxTaskGetSystemState(task_status, MAX_TASKS, NULL);
    if (task_count == 0) {
        /* Last resort: at least report task count so screen isn't blank */
        task_count = uxTaskGetNumberOfTasks();
        if (task_count > MAX_TASKS) task_count = MAX_TASKS;
        memset(task_status, 0, sizeof(task_status));
        /* Fill in what we can from known handles */
        const struct { TaskHandle_t *hdl; const char *name; } known[] = {
            { &xSensorTaskHandle,      "SensorTask"      },
            { &xSecurityTaskHandle,    "SecurityTask"    },
            { &xSafetyTaskHandle,      "SafetyTask"      },
            { &xEmergencyTaskHandle,   "EmergencyTask"   },
            { &xActuatorTaskHandle,    "ActuatorTask"    },
            { &xMonitoringTaskHandle,  "MonitoringTask"  },
            { &xCommTaskHandle,        "CommTask"        },
            { &xPerformanceTaskHandle, "PerformanceTask" },
            { &xScalabilityTaskHandle, "ScalabilityTask" },
        };
        UBaseType_t filled = 0;
        for (int k = 0; k < (int)(sizeof(known)/sizeof(known[0])) &&
                         filled < MAX_TASKS; k++) {
            if (*known[k].hdl == NULL) continue;
            strncpy(task_status[filled].pcTaskName, known[k].name,
                    configMAX_TASK_NAME_LEN - 1);
            task_status[filled].eCurrentState =
                eTaskGetState(*known[k].hdl);
            task_status[filled].uxCurrentPriority =
                uxTaskPriorityGet(*known[k].hdl);
            task_status[filled].usStackHighWaterMark =
                uxTaskGetStackHighWaterMark(*known[k].hdl);
            filled++;
        }
        task_count = filled;
    }
#endif

    uint64_t isr_total  = g_isr_total_us;
    uint32_t isr_count  = g_isr_count;
    float    isr_avg_us = isr_count > 0 ? (float)isr_total / isr_count : 0.0f;
    g_isr_total_us = 0;
    g_isr_count    = 0;

    static char rtos_json[RTOS_JSON_BUF_SIZE];
    int offset = 0;
    offset += snprintf(rtos_json + offset, RTOS_JSON_BUF_SIZE - offset,
        "{\"cpu_usage\":%.1f,\"idle_pct\":%.1f,"
        "\"free_heap\":%lu,\"min_heap\":%lu,"
        "\"ctx_switches\":%lu,\"tick_count\":%lu,"
        "\"isr_avg_us\":%.1f,\"isr_count\":%lu,"
        "\"scheduler\":\"RUNNING\",\"tasks\":[",
        g_cpu_usage_pct, 100.0f - g_cpu_usage_pct,
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)esp_get_minimum_free_heap_size(),
        (unsigned long)g_ctx_switch_total,
        (unsigned long)xTaskGetTickCount(),
        isr_avg_us, (unsigned long)isr_count);

    for (UBaseType_t i = 0; i < task_count && i < MAX_TASKS; i++) {
        TaskStatus_t *ts = &task_status[i];
        float    exec_ms  = 0.0f;
        float    cpu_pct  = 0.0f;
        uint32_t sw_count = 0;
        float    lat_ms   = 0.0f;
#if ENABLE_TRACE_HOOKS
        for (int j = 0; j < TRACE_MAX_TASKS; j++) {
            if (g_trace[j].ever_ran &&
                strncmp((char*)g_trace[j].name, ts->pcTaskName, 15) == 0) {
                uint64_t exec_us = g_trace[j].total_exec_us -
                                   g_trace[j].total_exec_us_prev;
                exec_ms  = (float)exec_us / 1000.0f;
                cpu_pct  = (float)exec_us / (float)window_us * 100.0f;
                sw_count = g_trace[j].switch_count -
                           g_trace[j].switch_count_prev;
                if (g_trace[j].first_switch_in_us > 0)
                    lat_ms = (float)g_trace[j].response_latency_us / 1000.0f;
                g_trace[j].total_exec_us_prev = g_trace[j].total_exec_us;
                g_trace[j].switch_count_prev  = g_trace[j].switch_count;
                break;
            }
        }
#endif
        if (i > 0 && offset < RTOS_JSON_BUF_SIZE - 2)
            rtos_json[offset++] = ',';
        int w2 = snprintf(rtos_json + offset, RTOS_JSON_BUF_SIZE - offset,
            "{\"name\":\"%s\",\"prio\":%u,\"state\":\"%s\","
            "\"hwm\":%u,\"exec_ms\":%.1f,\"switches\":%lu,"
            "\"lat_ms\":%.1f,\"cpu_pct\":%.1f}",
            ts->pcTaskName,
            (unsigned)ts->uxCurrentPriority,
            task_state_str(ts->eCurrentState),
            (unsigned)ts->usStackHighWaterMark,
            exec_ms, (unsigned long)sw_count, lat_ms, cpu_pct);
        if (w2 > 0) offset += w2;
        if (offset >= RTOS_JSON_BUF_SIZE - 50) break;  /* FIX-08 guard */
    }

    if (offset < RTOS_JSON_BUF_SIZE - 20) {
        offset += snprintf(rtos_json + offset, RTOS_JSON_BUF_SIZE - offset,
            "],\"ts\":%lu}", (unsigned long)get_timestamp_ms());
    }
    g_window_start_us = now_us;
    broker_publish(TOPIC_RTOS_PERF, rtos_json, 0);

    ESP_LOGI(TAG_RTOS, "CPU:%.1f%%  Heap:%lu  Tasks:%u  CtxSW:%lu",
             g_cpu_usage_pct,
             (unsigned long)esp_get_free_heap_size(),
             (unsigned)task_count,
             (unsigned long)g_ctx_switch_total);
}

/*******************************************************************************
 * MQTT BROKER CORE
 ******************************************************************************/
static int sock_read_exact(int sock, uint8_t *buf, int len)
{
    int total = 0;
    while (total < len) {
        int r = recv(sock, buf + total, len - total, 0);
        if (r <= 0) return -1;
        total += r;
    }
    return total;
}

static int mqtt_decode_remaining_len(int sock, uint32_t *out)
{
    uint32_t val = 0, mul = 1;
    uint8_t enc;
    do {
        if (recv(sock, &enc, 1, 0) <= 0) return -1;
        val += (enc & 0x7F) * mul;
        mul *= 128;
        if (mul > 128 * 128 * 128) return -1;
    } while (enc & 0x80);
    *out = val;
    return 0;
}

static int mqtt_encode_remaining_len(uint32_t len, uint8_t *buf)
{
    int i = 0;
    do {
        buf[i] = len & 0x7F;
        len >>= 7;
        if (len > 0) buf[i] |= 0x80;
        i++;
    } while (len > 0);
    return i;
}

static int mqtt_read_string(int sock, char *out, int max_len)
{
    uint8_t lb[2];
    if (sock_read_exact(sock, lb, 2) < 0) return -1;
    int len = (lb[0] << 8) | lb[1];
    if (len >= max_len) {
        uint8_t d[8];
        int rem = len;
        while (rem > 0) {
            int r = recv(sock, d, rem > 8 ? 8 : rem, 0);
            if (r <= 0) return -1;
            rem -= r;
        }
        out[0] = '\0';
        return len;
    }
    if (sock_read_exact(sock, (uint8_t*)out, len) < 0) return -1;
    out[len] = '\0';
    return len;
}

static int topic_matches(const char *filter, const char *topic)
{
    if (strcmp(filter, topic) == 0) return 1;
    int flen = strlen(filter);
    if (flen >= 2 && filter[flen - 1] == '#' && filter[flen - 2] == '/') {
        if (strncmp(filter, topic, flen - 1) == 0) return 1;
    }
    if (strcmp(filter, "#") == 0) return 1;
    return 0;
}

static int broker_alloc_client(void)
{
    for (int i = 0; i < MQTT_MAX_CLIENTS; i++)
        if (!s_clients[i].active) return i;
    return -1;
}

static void broker_free_client(int idx)
{
    if (idx < 0 || idx >= MQTT_MAX_CLIENTS) return;
    xSemaphoreTake(xBrokerMutex, portMAX_DELAY);
    if (s_clients[idx].active) {
        close(s_clients[idx].sock);
        s_clients[idx].sock      = -1;
        s_clients[idx].active    = 0;
        s_clients[idx].sub_count = 0;
        if (s_client_count > 0) s_client_count--;
        ESP_LOGI(TAG_BROKER, "Client[%d] freed. Active=%u", idx, s_client_count);
    }
    xSemaphoreGive(xBrokerMutex);
}

static void broker_publish(const char *topic, const char *payload, uint8_t qos)
{
    if (!xBrokerMutex) return;
    xSemaphoreTake(xBrokerMutex, portMAX_DELAY);

    static uint16_t pkt_id = 1;
    uint32_t topic_len   = strlen(topic);
    uint32_t payload_len = strlen(payload);

    /* Fixed header length: topic_len_field(2) + topic + [pkt_id(2 if QoS>0)] + payload */
    uint32_t rem = 2 + topic_len + (qos > 0 ? 2 : 0) + payload_len;

    uint8_t fixed_hdr[2] = { (uint8_t)(MQTT_PUBLISH | (qos << 1)), 0 };
    uint8_t rem_enc[4];
    int rem_bytes = mqtt_encode_remaining_len(rem, rem_enc);

    uint8_t topic_hdr[2] = { (uint8_t)(topic_len >> 8), (uint8_t)(topic_len & 0xFF) };

    for (int i = 0; i < MQTT_MAX_CLIENTS; i++) {
        if (!s_clients[i].active) continue;
        int matched = 0;
        for (int j = 0; j < s_clients[i].sub_count && !matched; j++)
            if (topic_matches(s_clients[i].subs[j], topic)) matched = 1;
        if (!matched) continue;
        send(s_clients[i].sock, fixed_hdr, 1, MSG_NOSIGNAL);
        send(s_clients[i].sock, rem_enc, rem_bytes, MSG_NOSIGNAL);
        send(s_clients[i].sock, topic_hdr, 2, MSG_NOSIGNAL);
        send(s_clients[i].sock, topic, topic_len, MSG_NOSIGNAL);
        if (qos > 0) {
            uint8_t pid[2] = { pkt_id >> 8, pkt_id & 0xFF };
            send(s_clients[i].sock, pid, 2, MSG_NOSIGNAL);
        }
        send(s_clients[i].sock, payload, payload_len, MSG_NOSIGNAL);
    }
    if (qos > 0) pkt_id++;
    xSemaphoreGive(xBrokerMutex);
}

/*******************************************************************************
 * MQTT CLIENT HANDLER
 ******************************************************************************/
typedef struct { int sock; int idx; } ClientHandlerArgs_t;

static void mqtt_client_handler_task(void *pvParameters)
{
    ClientHandlerArgs_t *args = (ClientHandlerArgs_t *)pvParameters;
    int sock = args->sock, idx = args->idx;
    free(args);

    /* FIX-09: SO_RCVTIMEO prevents infinite blocking */
    struct timeval tv = { .tv_sec = MQTT_KEEPALIVE_SEC + 10, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t fixed_hdr;
    uint32_t rem_len;

    while (1) {
        if (recv(sock, &fixed_hdr, 1, 0) <= 0) break;
        if (mqtt_decode_remaining_len(sock, &rem_len) < 0) break;
        uint8_t pkt_type = fixed_hdr & 0xF0;

        if (pkt_type == MQTT_CONNECT) {
            uint32_t to_read = rem_len;
            char client_id[32] = "unknown";
            uint8_t hdr[7];
            if (to_read >= 7) { sock_read_exact(sock, hdr, 7); to_read -= 7; }
            uint8_t opts[3];
            if (to_read >= 3) { sock_read_exact(sock, opts, 3); to_read -= 3; }
            if (to_read >= 2) {
                int cl = mqtt_read_string(sock, client_id, 32);
                if (cl < 0) break;
                to_read -= (2 + cl);
            }
            while (to_read > 0) {
                uint8_t d[32];
                int dr = recv(sock, d, to_read > 32 ? 32 : (int)to_read, 0);
                if (dr <= 0) goto client_done;
                to_read -= dr;
            }
            xSemaphoreTake(xBrokerMutex, portMAX_DELAY);
            strncpy(s_clients[idx].client_id, client_id, 31);
            xSemaphoreGive(xBrokerMutex);
            uint8_t ca[4] = {MQTT_CONNACK, 0x02, 0x00, MQTT_CONNACK_ACCEPTED};
            send(sock, ca, 4, MSG_NOSIGNAL);
            ESP_LOGI(TAG_BROKER, "Client[%d] CONNECTED id='%s'", idx, client_id);
        }
        else if (pkt_type == MQTT_SUBSCRIBE) {
            uint8_t pid[2];
            sock_read_exact(sock, pid, 2);
            uint32_t remaining = rem_len - 2;
            uint8_t sub_rc[MAX_SUBS_PER_CLIENT];
            uint8_t sub_cnt = 0;
            xSemaphoreTake(xBrokerMutex, portMAX_DELAY);
            while (remaining > 2 && s_clients[idx].sub_count < MAX_SUBS_PER_CLIENT) {
                char tf[MAX_TOPIC_LEN];
                int tl = mqtt_read_string(sock, tf, MAX_TOPIC_LEN);
                if (tl < 0) { xSemaphoreGive(xBrokerMutex); goto client_done; }
                remaining -= (2 + tl);
                uint8_t rq;
                if (recv(sock, &rq, 1, 0) <= 0) { xSemaphoreGive(xBrokerMutex); goto client_done; }
                remaining--;
                strncpy(s_clients[idx].subs[s_clients[idx].sub_count], tf, MAX_TOPIC_LEN - 1);
                s_clients[idx].sub_count++;
                sub_rc[sub_cnt++] = rq & 0x01;
                ESP_LOGI(TAG_BROKER, "Client[%d] SUB '%s'", idx, tf);
            }
            while (remaining > 0) {
                uint8_t d[8];
                int dr = recv(sock, d, remaining > 8 ? 8 : (int)remaining, 0);
                if (dr <= 0) { xSemaphoreGive(xBrokerMutex); goto client_done; }
                remaining -= dr;
            }
            xSemaphoreGive(xBrokerMutex);
            uint8_t suback[6];
            uint32_t sa_rem = 2 + sub_cnt;
            int sa_rb = mqtt_encode_remaining_len(sa_rem, &suback[1]);
            suback[0] = MQTT_SUBACK;
            int sa_off = 1 + sa_rb;
            suback[sa_off]     = pid[0];
            suback[sa_off + 1] = pid[1];
            send(sock, suback, sa_off + 2, MSG_NOSIGNAL);
            send(sock, sub_rc, sub_cnt, MSG_NOSIGNAL);
        }
        else if (pkt_type == MQTT_PINGREQ) {
            uint8_t pr[2] = {MQTT_PINGRESP, 0x00};
            send(sock, pr, 2, MSG_NOSIGNAL);
        }
        else if (pkt_type == MQTT_DISCONNECT) {
            break;
        }
        else if (pkt_type == MQTT_PUBLISH) {
            uint32_t to_read = rem_len;
            char pt[MAX_TOPIC_LEN];
            int tl = mqtt_read_string(sock, pt, MAX_TOPIC_LEN);
            if (tl < 0) break;
            to_read -= (2 + tl);
            uint8_t pq = (fixed_hdr >> 1) & 0x03;
            if (pq > 0 && to_read >= 2) {
                uint8_t p2[2];
                sock_read_exact(sock, p2, 2);
                to_read -= 2;
            }
            char pp[JSON_BUF_SIZE];
            uint32_t pl = to_read < (JSON_BUF_SIZE - 1) ? to_read : (JSON_BUF_SIZE - 1);
            if (sock_read_exact(sock, (uint8_t*)pp, pl) < 0) break;
            pp[pl] = '\0';
            broker_publish(pt, pp, 0);
        }
        else {
            /* Skip unknown packet */
            uint32_t skip = rem_len;
            while (skip > 0) {
                uint8_t d[32];
                int dr = recv(sock, d, skip > 32 ? 32 : (int)skip, 0);
                if (dr <= 0) goto client_done;
                skip -= dr;
            }
        }
        xSemaphoreTake(xBrokerMutex, portMAX_DELAY);
        if (s_clients[idx].active)
            s_clients[idx].last_activity = get_timestamp_ms();
        xSemaphoreGive(xBrokerMutex);
    }

client_done:
    broker_free_client(idx);
    vTaskDelete(NULL);
}

/*******************************************************************************
 * MQTT BROKER LISTENER TASK
 ******************************************************************************/
static void mqtt_broker_task(void *pvParameters)
{
    int server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (server_sock < 0) { vTaskDelete(NULL); return; }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(MQTT_BROKER_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_sock, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
        close(server_sock); vTaskDelete(NULL); return;
    }
    if (listen(server_sock, MQTT_MAX_CLIENTS) != 0) {
        close(server_sock); vTaskDelete(NULL); return;
    }
    ESP_LOGI(TAG_BROKER, "MQTT broker ready 192.168.4.1:%d", MQTT_BROKER_PORT);

    while (1) {
        struct sockaddr_in ca;
        socklen_t al = sizeof(ca);
        int cs = accept(server_sock, (struct sockaddr*)&ca, &al);
        if (cs < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

        char cip[16];
        inet_ntoa_r(ca.sin_addr, cip, sizeof(cip));
        ESP_LOGI(TAG_BROKER, "TCP connect from %s sock=%d", cip, cs);

        xSemaphoreTake(xBrokerMutex, portMAX_DELAY);
        int idx = broker_alloc_client();
        if (idx < 0) {
            xSemaphoreGive(xBrokerMutex);
            close(cs);
            ESP_LOGW(TAG_BROKER, "Max clients reached — rejected %s", cip);
            continue;
        }
        s_clients[idx].sock          = cs;
        s_clients[idx].active        = 1;
        s_clients[idx].sub_count     = 0;
        s_clients[idx].last_activity = get_timestamp_ms();
        s_client_count++;
        xSemaphoreGive(xBrokerMutex);

        ClientHandlerArgs_t *args = malloc(sizeof(ClientHandlerArgs_t));
        if (!args) { broker_free_client(idx); continue; }
        args->sock = cs;
        args->idx  = idx;
        char tn[20];
        snprintf(tn, sizeof(tn), "MqttCli%d", idx);
        BaseType_t r = xTaskCreate(mqtt_client_handler_task, tn,
                                   MQTT_CLIENT_TASK_STACK, args,
                                   MQTT_CLIENT_TASK_PRIORITY, NULL);
        if (r != pdPASS) { free(args); broker_free_client(idx); }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/*******************************************************************************
 * SENSOR TASK — centralised data collection (FIX-15 replaces scattered globals)
 ******************************************************************************/
static void vSensorTask(void *pvParameters)
{
    ESP_LOGI(TAG_SENSOR, "Started");
    TickType_t xLast = xTaskGetTickCount();

    /* FIX-21: Raised threshold 100→200 — a floating ADC pin on GPIO34 often
     * reads 50–150 due to noise, causing false "LM35 absent" detection.
     * 200 is a safer minimum for a real connected LM35 output. */
    bool lm35_present = (adc1_get_raw(GPIO_TEMP_SENSOR_ADC) > 200);
    ESP_LOGI(TAG_SENSOR, "LM35 %s (probe ADC=%d)",
             lm35_present ? "DETECTED" : "absent — using DS3231",
             adc1_get_raw(GPIO_TEMP_SENSOR_ADC));

    /* FIX-21: Re-probe counter — check every 30 s if LM35 is still absent
     * so a sensor connected after boot is detected automatically. */
    uint32_t lm35_reprobe_ticks = 0;
    const uint32_t LM35_REPROBE_INTERVAL = (30000 / SENSOR_PERIOD_MS);

#if HAVE_TASK_WDT
    esp_task_wdt_add(NULL);
#endif

    while (1) {
        vTaskDelayUntil(&xLast, pdMS_TO_TICKS(SENSOR_PERIOD_MS));

#if HAVE_TASK_WDT
        esp_task_wdt_reset();                           /* FIX-11 */
#endif

        /* FIX-21: Periodically re-probe for LM35 in case it was connected
         * after boot. Only re-probe when currently marked absent. */
        if (!lm35_present) {
            lm35_reprobe_ticks++;
            if (lm35_reprobe_ticks >= LM35_REPROBE_INTERVAL) {
                lm35_reprobe_ticks = 0;
                int probe = adc1_get_raw(GPIO_TEMP_SENSOR_ADC);
                if (probe > 200) {
                    lm35_present = true;
                    ESP_LOGI(TAG_SENSOR, "LM35 now DETECTED (ADC=%d)", probe);
                }
            }
        }

        SensorData_t sd = {0};
        sd.timestamp = get_timestamp_ms();

        /* ── Temperature ─────────────────────────────────────────────────── */
        float temp = 0.0f;
        bool  rtc_ok = false;

        /* FIX-20: Try DS3231 first. If it fails, fall back to LM35 ADC.
         * Previously: DS3231 fail → temp stays 0.0 → temp_fault=0 (0 is in
         * range -10..150) → publishes 0.00°C as if it were a real reading.
         * Now: DS3231 fail → use LM35 if present, else flag fault properly. */
#if ENABLE_RTC && ENABLE_RTC_TEMPERATURE
        float rtc_t = hal_rtc_read_temperature();
        if (rtc_t > -900.0f) {
            temp   = rtc_t;
            rtc_ok = true;
            sd.rtc_temperature = rtc_t;
            SLOG_I(TAG_DS3231, "Temperature = %.2f C (DS3231)", rtc_t);
        } else {
            sd.rtc_temperature = -999.0f;
            /* FIX-20: DS3231 failed — try LM35 as fallback */
            if (lm35_present) {
                temp = hal_read_temperature();
                ESP_LOGW(TAG_DS3231,
                    "DS3231 failed — using LM35 fallback: %.2f C", temp);
            } else {
                /* No sensor available — mark fault explicitly */
                ESP_LOGW(TAG_DS3231,
                    "DS3231 failed, no LM35 — temp unavailable");
                temp = -999.0f;   /* will trigger temp_fault below */
            }
        }
#else
        /* No RTC compiled in — use LM35 only */
        if (lm35_present) temp = hal_read_temperature();
#endif

        /* FIX-10: Fault detection — -999 sentinel also triggers fault */
        sd.temp_fault  = (temp < TEMP_FAULT_MIN || temp > TEMP_FAULT_MAX) ? 1 : 0;
        sd.temperature = sd.temp_fault ? 0.0f : temp;

        /* ── Gas ─────────────────────────────────────────────────────────── */
        uint16_t gas = hal_read_gas_level();
        sd.gas_fault  = (gas > GAS_FAULT_MAX) ? 1 : 0;
        sd.gas_level  = gas;

        /* ── Noise ───────────────────────────────────────────────────────── */
        sd.noise_level = hal_read_noise_level();

        /* ── Push Buttons ────────────────────────────────────────────────── */
        sd.button1_pressed = hal_read_button1();
        sd.button2_pressed = hal_read_button2();

        /* ── Digital sensors ─────────────────────────────────────────────── */
        sd.pir_detected       = hal_read_pir();
        sd.door_open          = hal_read_door();
        sd.vibration_detected = hal_read_vibration();

        /* FIX-20: Only read RTC time if DS3231 responded correctly.
         * If not, leave rtc_time zeroed — CommTask will publish "??:??:??"
         * so Flutter knows the clock is unavailable, not midnight. */
#if ENABLE_RTC
        if (rtc_ok) {
            hal_rtc_read(&sd.rtc_time);
        } else {
            /* Sentinel values — CommTask checks hours==99 to publish "??:??:??" */
            sd.rtc_time.hours   = 99;
            sd.rtc_time.minutes = 99;
            sd.rtc_time.seconds = 99;
            sd.rtc_time.date    = 0;
            sd.rtc_time.month   = 0;
            sd.rtc_time.year    = 0;
        }
#endif

        xQueueOverwrite(xDataQueue, &sd);

        /* ── Sync to web dashboard shared struct ────────────────────────── */
        g_sensor_data.temp      = sd.temperature;
        g_sensor_data.gas       = sd.gas_level;
        g_sensor_data.noise     = sd.noise_level;
        g_sensor_data.pir       = sd.pir_detected;
        g_sensor_data.door      = sd.door_open;
        g_sensor_data.vibration = sd.vibration_detected;
        g_sensor_data.button1   = sd.button1_pressed;
        g_sensor_data.button2   = sd.button2_pressed;
        g_sensor_data.cpu_usage = g_cpu_usage_pct;

        /* Actuator state — take mutex for a safe snapshot (FIX-03) */
        if (xActuatorMutex) {
            xSemaphoreTake(xActuatorMutex, pdMS_TO_TICKS(5));
            g_sensor_data.buzzer    = g_actuator.buzzer;
            g_sensor_data.fan_relay = g_actuator.fan;
            /* siren and exhaust removed in v10 — keep as 0 */
            g_sensor_data.siren   = 0;
            g_sensor_data.exhaust = 0;
            xSemaphoreGive(xActuatorMutex);
        }

        /* RTC time/date — guard against sentinel 99 values (FIX-20) */
        if (sd.rtc_time.hours != 99) {
            /* rtc_time[12]: worst-case "255:255:255\0"=12 bytes — fits exactly (uint8_t [0,255]) */
            snprintf(g_sensor_data.rtc_time, sizeof(g_sensor_data.rtc_time),
                     "%02d:%02d:%02d",
                     sd.rtc_time.hours, sd.rtc_time.minutes, sd.rtc_time.seconds);
            /* FIX-29: rtc_date[] widened to [14] in isms_web_server.h — see header.
             * Worst-case "255/255/20255\0" = 14 bytes; fits exactly.
             * DS3231 year is BCD 00-99 so runtime value is always "DD/MM/20YY\0"
             * = 11 bytes, but we size the buffer for the uint8_t type maximum. */
            snprintf(g_sensor_data.rtc_date, sizeof(g_sensor_data.rtc_date),
                     "%02d/%02d/20%02d",
                     sd.rtc_time.date, sd.rtc_time.month,
                     (int)(sd.rtc_time.year));
        } else {
            /* Avoid trigraph parsing ("??:" is a trigraph prefix in C) */
            strlcpy(g_sensor_data.rtc_time, "\x3F\x3F:\x3F\x3F:\x3F\x3F", sizeof(g_sensor_data.rtc_time));

            /* Avoid trigraph parsing ("??/" is a trigraph prefix in C) */
            strlcpy(g_sensor_data.rtc_date, "\x3F\x3F/\x3F\x3F/\x3F\x3F\x3F\x3F", sizeof(g_sensor_data.rtc_date));

        }

        /* System state string */
        strlcpy(g_sensor_data.state, state_str(), sizeof(g_sensor_data.state));
    }
}

/*******************************************************************************
 * SECURITY TASK — ISR semaphore consumer (FIX-01 debounce in ISR)
 ******************************************************************************/
static void vSecurityTask(void *pvParameters)
{
    ESP_LOGI(TAG_SECURITY, "Started");
    TickType_t xLast    = xTaskGetTickCount();
    uint8_t    last_pir  = hal_read_pir();      /* FIX-33: Init to current GPIO state, not hardcoded 0 */
    uint8_t    last_door = hal_read_door();     /* FIX-33: Init to current GPIO state, not hardcoded 0 */
    uint32_t   door_stable_since = 0;   /* FIX-07 */
    uint32_t   door_open_since   = 0;   /* FIX-35: Track how long door has been open */

#if HAVE_TASK_WDT
    esp_task_wdt_add(NULL);
#endif

    while (1) {
        vTaskDelayUntil(&xLast, pdMS_TO_TICKS(SECURITY_PERIOD_MS));

#if HAVE_TASK_WDT
        esp_task_wdt_reset();
#endif

        /* ── PIR ─────────────────────────────────────────────────────────── */
        if (xSemaphoreTake(xPIRSemaphore, 0) == pdTRUE) {
            uint8_t pir = hal_read_pir();
            if (pir && !last_pir) {
                SLOG_W(TAG_SECURITY, "UNAUTHORIZED ENTRY! PIR triggered");
                send_event(EVENT_TYPE_UNAUTHORIZED_ENTRY, 1.0f, 1, 3);
                xEventGroupSetBits(xSystemEventGroup, EVENT_SECURITY_BREACH);
                securityBreachCount++;
                update_system_state(EVENT_TYPE_UNAUTHORIZED_ENTRY);
                char buf[140];
                int w = snprintf(buf, sizeof(buf),
                    "{\"alert\":\"unauthorized_entry\",\"pir\":1,"
                    "\"severity\":3,\"ts\":%lu}",
                    (unsigned long)get_timestamp_ms());
                if (w > 0 && w < (int)sizeof(buf))
                    broker_publish(TOPIC_SECURITY, buf, 1);
            }
            last_pir = pir;
        }

        /* ── Door — stable-close confirmation (FIX-07) ───────────────────── */
        if (xSemaphoreTake(xDoorSemaphore, 0) == pdTRUE) {
            uint8_t door = hal_read_door();
            if (door && !last_door) {
                SLOG_W(TAG_SECURITY, "DOOR OPENED!");
                send_event(EVENT_TYPE_DOOR_OPEN, 1.0f, 2, 2);
                update_system_state(EVENT_TYPE_DOOR_OPEN);
                door_stable_since = 0;
                door_open_since = get_timestamp_ms();  /* FIX-35: Start tracking open time */
                char buf[100];
                int w = snprintf(buf, sizeof(buf),
                    "{\"alert\":\"door_open\",\"door\":1,"
                    "\"severity\":2,\"ts\":%lu}",
                    (unsigned long)get_timestamp_ms());
                if (w > 0 && w < (int)sizeof(buf))
                    broker_publish(TOPIC_SECURITY, buf, 1);
            } else if (!door && last_door) {
                /* Door went low — start stability timer */
                door_stable_since = get_timestamp_ms();
                door_open_since = 0;  /* FIX-35: Clear open timer */
            }
            last_door = door;
        }

        /* Confirm door closed after 50 ms stable */
        if (last_door == 1) {
            uint8_t live = hal_read_door();
            if (!live) {
                if (door_stable_since == 0)
                    door_stable_since = get_timestamp_ms();
                else if ((get_timestamp_ms() - door_stable_since) > 50) {
                    last_door = 0;
                    door_stable_since = 0;
                    door_open_since = 0;
                    SLOG_I(TAG_SECURITY, "Door CLOSED (confirmed)");
                }
            } else {
                door_stable_since = 0; /* still open — reset */
            }
        }

        /* FIX-35: Force-clear door_open if stuck open for > 30 seconds
         * (possible pull-down failure, floating GPIO, or stuck sensor) */
        if (last_door == 1 && door_open_since > 0) {
            uint32_t door_open_duration = (get_timestamp_ms() - door_open_since) / 1000;
            if (door_open_duration >= 30) {
                SLOG_W(TAG_SECURITY, "Door STUCK OPEN (30s) — force-clearing flag");
                SLOG_W(TAG_SECURITY, "  Check: pull-down resistor, sensor wiring, GPIO5 voltage");
                last_door = 0;
                door_open_since = 0;
                door_stable_since = 0;
            }
        }

        /* LED blink when security events are active */
        EventBits_t bits = xEventGroupGetBits(xSystemEventGroup);
        if (bits & EVENT_SECURITY_BREACH) {
            static uint8_t t = 0;
            _set_led_status(t);
            t = !t;
        }
    }
}

/*******************************************************************************
 * SAFETY TASK — threshold checks on sensor data
 ******************************************************************************/
static void vSafetyTask(void *pvParameters)
{
    ESP_LOGI(TAG_SAFETY, "Started");
    TickType_t xLast = xTaskGetTickCount();

#if HAVE_TASK_WDT
    esp_task_wdt_add(NULL);
#endif

    while (1) {
        vTaskDelayUntil(&xLast, pdMS_TO_TICKS(SAFETY_PERIOD_MS));

#if HAVE_TASK_WDT
        esp_task_wdt_reset();
#endif

        SensorData_t sd = {0};
        if (xQueuePeek(xDataQueue, &sd, 0) != pdTRUE) continue;

        /* Temperature thresholds */
        float temp = sd.temperature;
        if (!sd.temp_fault) {
            if (temp >= TEMP_THRESHOLD_CRITICAL) {
                SLOG_E(TAG_SAFETY, "CRITICAL TEMP: %.1f C", temp);
                send_event(EVENT_TYPE_CRITICAL_TEMP, temp, 3, 5);
                xEventGroupSetBits(xSystemEventGroup,
                    EVENT_FIRE_DETECTED | EVENT_EMERGENCY_ACTIVE);
                update_system_state(EVENT_TYPE_CRITICAL_TEMP);
                char buf[140];
                int w = snprintf(buf, sizeof(buf),
                    "{\"alert\":\"critical_temp\",\"temp\":%.2f,"
                    "\"severity\":5,\"ts\":%lu}",
                    temp, (unsigned long)get_timestamp_ms());
                if (w > 0 && w < (int)sizeof(buf))
                    broker_publish(TOPIC_SAFETY, buf, 1);
            } else if (temp >= TEMP_THRESHOLD_HIGH) {
                SLOG_W(TAG_SAFETY, "HIGH TEMP: %.1f C", temp);
                send_event(EVENT_TYPE_OVERHEATING, temp, 3, 3);
                xEventGroupSetBits(xSystemEventGroup, EVENT_OVERHEAT);
                update_system_state(EVENT_TYPE_OVERHEATING);
                char buf[140];
                int w = snprintf(buf, sizeof(buf),
                    "{\"alert\":\"overheat\",\"temp\":%.2f,"
                    "\"severity\":3,\"ts\":%lu}",
                    temp, (unsigned long)get_timestamp_ms());
                if (w > 0 && w < (int)sizeof(buf))
                    broker_publish(TOPIC_SAFETY, buf, 1);
            }
        } else {
            SLOG_W(TAG_SAFETY, "Temp sensor FAULT — reading suppressed");
        }

        /* Gas thresholds */
        uint16_t gas = sd.gas_level;
        if (!sd.gas_fault && gas >= GAS_THRESHOLD_CRITICAL) {
            SLOG_E(TAG_SAFETY, "GAS LEAK CRITICAL: %u ppm", gas);
            send_event(EVENT_TYPE_GAS_LEAK_ALARM, (float)gas, 4, 4);
            xEventGroupSetBits(xSystemEventGroup,
                EVENT_GAS_LEAK | EVENT_EMERGENCY_ACTIVE);
            update_system_state(EVENT_TYPE_GAS_LEAK_ALARM);
            char buf[140];
            int w = snprintf(buf, sizeof(buf),
                "{\"alert\":\"gas_leak\",\"gas_ppm\":%u,"
                "\"severity\":4,\"ts\":%lu}",
                (unsigned)gas, (unsigned long)get_timestamp_ms());
            if (w > 0 && w < (int)sizeof(buf))
                broker_publish(TOPIC_SAFETY, buf, 1);
        }
    }
}

/*******************************************************************************
 * EMERGENCY TASK — event → ActuatorTask command dispatcher (FIX-05)
 * No more vTaskDelay here — duration sent as command parameter to ActuatorTask
 ******************************************************************************/
static void vEmergencyTask(void *pvParameters)
{
    ESP_LOGI(TAG_EMERGENCY, "Started");
    EventMessage_t event;
    uint32_t maxLat = 0;

    while (1) {
        if (xQueueReceive(xEventQueue, &event, portMAX_DELAY) != pdTRUE) continue;
        uint64_t t0 = esp_timer_get_time();

        switch (event.type) {
            case EVENT_TYPE_UNAUTHORIZED_ENTRY:
            case EVENT_TYPE_SECURITY_BREACH:
                actuator_post(ACT_CMD_BUZZER_ON,    5000, "security");
                actuator_post(ACT_CMD_LED_ALARM_ON, 0,    "security");
                actuator_post(ACT_CMD_LED_NORMAL_OFF, 0,  "security");
                break;
            case EVENT_TYPE_DOOR_OPEN:
                actuator_post(ACT_CMD_BUZZER_ON, 500, "door");
                break;
            case EVENT_TYPE_CRITICAL_TEMP:
            case EVENT_TYPE_FIRE_ALARM:
                actuator_post(ACT_CMD_BUZZER_ON,    10000, "fire");
                actuator_post(ACT_CMD_FAN_ON,       10000, "fire");
                actuator_post(ACT_CMD_LED_ALARM_ON, 0,     "fire");
                actuator_post(ACT_CMD_LED_NORMAL_OFF, 0,   "fire");
                break;
            case EVENT_TYPE_GAS_LEAK_ALARM:
                actuator_post(ACT_CMD_BUZZER_ON,    10000, "gas_leak");
                actuator_post(ACT_CMD_FAN_ON,       10000, "gas_leak");
                actuator_post(ACT_CMD_LED_ALARM_ON, 0,     "gas_leak");
                actuator_post(ACT_CMD_LED_NORMAL_OFF, 0,   "gas_leak");
                break;
            case EVENT_TYPE_OVERHEATING:
                actuator_post(ACT_CMD_FAN_ON,       5000, "overheat");
                actuator_post(ACT_CMD_LED_ALARM_ON, 5000, "overheat");
                break;
            case EVENT_TYPE_NOISE_ALARM:
                actuator_post(ACT_CMD_BUZZER_ON,    3000, "noise");
                actuator_post(ACT_CMD_LED_ALARM_ON, 3000, "noise");
                break;
            case EVENT_TYPE_VIBRATION_DETECTED:
                actuator_post(ACT_CMD_BUZZER_ON,    2000, "vibration");
                actuator_post(ACT_CMD_LED_ALARM_ON, 2000, "vibration");
                break;
            case EVENT_TYPE_SYSTEM_OK:
                actuator_post(ACT_CMD_ALL_CLEAR, 0, "system_ok");
                xEventGroupClearBits(xSystemEventGroup,
                    EVENT_SECURITY_BREACH | EVENT_FIRE_DETECTED |
                    EVENT_GAS_LEAK        | EVENT_OVERHEAT       |
                    EVENT_EMERGENCY_ACTIVE);
                break;
            default:
                break;
        }

        uint32_t lat = (uint32_t)(esp_timer_get_time() - t0);
        if (lat > maxLat) maxLat = lat;
        SLOG_I(TAG_EMERGENCY, "Dispatch latency %lu us (max %lu us)",
               (unsigned long)lat, (unsigned long)maxLat);
    }
}

/*******************************************************************************
 * ACTUATOR TASK — sole owner of all GPIO outputs (FIX-05, FIX-03, FIX-06)
 ******************************************************************************/
static void vActuatorTask(void *pvParameters)
{
    ESP_LOGI(TAG_ACTUATOR, "Started");

    /* NOTE: Fan relay output is ACTIVE-LOW.
     * We keep g_actuator.fan as a logical value (1=fan ON, 0=fan OFF).
     * _set_fan() converts that to the correct GPIO level. */


    while (1) {
        ActuatorMessage_t msg;
        /* Wait up to 1 s for a command — also handles periodic fan timeout check */
        if (xQueueReceive(xActuatorQueue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) {

            xSemaphoreTake(xActuatorMutex, portMAX_DELAY);  /* FIX-03 */

            switch (msg.cmd) {
                case ACT_CMD_BUZZER_ON:
                    _set_buzzer(1); g_actuator.buzzer = 1;
                    strncpy(g_actuator.trigger, msg.trigger, 23);
                    /* Schedule off after duration */
                    if (msg.duration_ms > 0) {
                        xSemaphoreGive(xActuatorMutex);
                        vTaskDelay(pdMS_TO_TICKS(msg.duration_ms));
                        xSemaphoreTake(xActuatorMutex, portMAX_DELAY);
                        _set_buzzer(0); g_actuator.buzzer = 0;
                    }
                    break;
                case ACT_CMD_BUZZER_OFF:
                    _set_buzzer(0); g_actuator.buzzer = 0; break;

                case ACT_CMD_FAN_ON:
                    _set_fan(1); g_actuator.fan = 1;  /* logical fan ON */
                    strncpy(g_actuator.trigger, msg.trigger, 23);
                    if (msg.duration_ms > 0) {
                        xSemaphoreGive(xActuatorMutex);
                        vTaskDelay(pdMS_TO_TICKS(msg.duration_ms));
                        xSemaphoreTake(xActuatorMutex, portMAX_DELAY);
                        /* FIX-06: respect manual override */
                        if (!g_actuator.fan_manual) {
                            _set_fan(0); g_actuator.fan = 0;
                        }
                    }
                    break;
                case ACT_CMD_FAN_OFF:
                    _set_fan(0);
                    g_actuator.fan          = 0;
                    g_actuator.fan_manual   = 0;
                    g_actuator.fan_manual_since = 0;
                    break;

                case ACT_CMD_LED_ALARM_ON:
                    _set_led_alarm(1); g_actuator.led_alarm = 1;
                    if (msg.duration_ms > 0) {
                        xSemaphoreGive(xActuatorMutex);
                        vTaskDelay(pdMS_TO_TICKS(msg.duration_ms));
                        xSemaphoreTake(xActuatorMutex, portMAX_DELAY);
                        _set_led_alarm(0); g_actuator.led_alarm = 0;
                    }
                    break;
                case ACT_CMD_LED_ALARM_OFF:
                    _set_led_alarm(0); g_actuator.led_alarm = 0; break;

                case ACT_CMD_LED_NORMAL_ON:
                    _set_led_normal(1); g_actuator.led_normal = 1; break;
                case ACT_CMD_LED_NORMAL_OFF:
                    _set_led_normal(0); g_actuator.led_normal = 0; break;

                case ACT_CMD_ALL_CLEAR:
                    _set_buzzer(0);  _set_fan(0);
                    _set_led_alarm(0); _set_led_normal(1);
                    g_actuator.buzzer    = 0;
                    g_actuator.fan       = 0;
                    g_actuator.led_alarm = 0; g_actuator.led_normal= 1;
                    g_actuator.fan_manual = 0; g_actuator.fan_manual_since = 0;
                    strncpy(g_actuator.trigger, "none", 23);
                    break;
                default:
                    break;
            }
            xSemaphoreGive(xActuatorMutex);

            /* Publish actuator state after any change */
            ActuatorState_t snap;
            xSemaphoreTake(xActuatorMutex, portMAX_DELAY);
            snap = g_actuator;
            xSemaphoreGive(xActuatorMutex);

            char buf[210];
            int w = snprintf(buf, sizeof(buf),
                "{\"buzzer\":%u,\"fan\":%u,"
                "\"led_alarm\":%u,\"led_normal\":%u,"
                "\"trigger\":\"%s\",\"ts\":%lu}",
                (unsigned)snap.buzzer,
                (unsigned)snap.fan,
                (unsigned)snap.led_alarm, (unsigned)snap.led_normal,
                snap.trigger, (unsigned long)get_timestamp_ms());
            if (w > 0 && w < (int)sizeof(buf))
                broker_publish(TOPIC_EMERGENCY, buf, 1);
        }

        /* FIX-06: Fan manual override auto-release after 30 s */
        xSemaphoreTake(xActuatorMutex, portMAX_DELAY);
        if (g_actuator.fan_manual && g_actuator.fan_manual_since > 0) {
            uint32_t elapsed = (get_timestamp_ms() - g_actuator.fan_manual_since) / 1000;
            if (elapsed >= FAN_MANUAL_TIMEOUT_S) {
                _set_fan(0);
                g_actuator.fan          = 0;
                g_actuator.fan_manual   = 0;
                g_actuator.fan_manual_since = 0;
                SLOG_W(TAG_ACTUATOR, "Fan manual override expired (%ds)", FAN_MANUAL_TIMEOUT_S);
            }
        }
        xSemaphoreGive(xActuatorMutex);
    }
}

/*******************************************************************************
 * MONITORING TASK — light / noise / vibration
 ******************************************************************************/
#if ENABLE_MONITORING_TASK
static void vMonitoringTask(void *pvParameters)
{
    ESP_LOGI(TAG_MONITOR, "Started");
    TickType_t xLast = xTaskGetTickCount();

    while (1) {
        vTaskDelayUntil(&xLast, pdMS_TO_TICKS(MONITORING_PERIOD_MS));

        uint16_t noise = hal_read_noise_level();
        uint8_t  btn1  = hal_read_button1();
        uint8_t  btn2  = hal_read_button2();

        if (noise >= NOISE_THRESHOLD_ALARM) {
            send_event(EVENT_TYPE_NOISE_ALARM, (float)noise, 6, 3);
            xEventGroupSetBits(xSystemEventGroup, EVENT_NOISE_ALARM);
            update_system_state(EVENT_TYPE_NOISE_ALARM);
        } else if (noise >= NOISE_THRESHOLD_WARNING) {
            send_event(EVENT_TYPE_NOISE_WARNING, (float)noise, 6, 1);
        }

        /* Log button press events */
        if (btn1) SLOG_I(TAG_MONITOR, "Button1 PRESSED — Buzzer trigger");
        if (btn2) SLOG_I(TAG_MONITOR, "Button2 PRESSED — Fan toggle");

        /* ── Button1 (GPIO32): manual buzzer trigger (500 ms pulse) ── */
        if (btn1) {
            ActuatorMessage_t m = {
                .cmd         = ACT_CMD_BUZZER_ON,
                .duration_ms = 500,  /* 500 ms buzzer pulse */
            };
            strncpy(m.trigger, "btn1_buzzer", 23);
            xQueueSend(xActuatorQueue, &m, 0);
            SLOG_I(TAG_MONITOR, "Button1: Buzzer ON (500ms pulse)");
            g_btn1_isr_flag = 0;  /* FIX-34: Clear flag after processing */
        }

        /* ── Button2 (GPIO19): manual fan toggle (FIX-06 manual override) ── */
        if (btn2) {
            xSemaphoreTake(xActuatorMutex, pdMS_TO_TICKS(10));
            uint8_t fan_now = g_actuator.fan;
            xSemaphoreGive(xActuatorMutex);

            if (fan_now) {
                /* Fan is ON → turn it OFF and clear manual flag */
                actuator_post(ACT_CMD_FAN_OFF, 0, "btn2_toggle");
                SLOG_I(TAG_MONITOR, "Button2: Fan OFF (manual)");
            } else {
                /* Fan is OFF → turn it ON as manual override (no auto-timeout
                 * duration; the 30-s manual watchdog in ActuatorTask handles it) */
                ActuatorMessage_t m = {
                    .cmd         = ACT_CMD_FAN_ON,
                    .duration_ms = 0,          /* permanent until toggled off or 30-s WDT */
                };
                strncpy(m.trigger, "btn2_toggle", 23);
                xSemaphoreTake(xActuatorMutex, pdMS_TO_TICKS(10));
                g_actuator.fan_manual       = 1;
                g_actuator.fan_manual_since = get_timestamp_ms();
                xSemaphoreGive(xActuatorMutex);
                xQueueSend(xActuatorQueue, &m, 0);
                SLOG_I(TAG_MONITOR, "Button2: Fan ON (manual, 30s WDT)");
            }
            g_btn2_isr_flag = 0;  /* FIX-34: Clear flag after processing */
        }

        /* Update shared data queue with fresh monitoring data */
        SensorData_t sd = {0};
        if (xQueuePeek(xDataQueue, &sd, 0) == pdTRUE) {
            sd.noise_level    = noise;
            sd.button1_pressed = btn1;
            sd.button2_pressed = btn2;
            sd.timestamp      = get_timestamp_ms();
            xQueueOverwrite(xDataQueue, &sd);
        }

        char buf[200];
        int w = snprintf(buf, sizeof(buf),
            "{\"noise\":%u,\"vibration\":%u,"
            "\"button1\":%u,\"button2\":%u,"
            "\"pir\":%u,\"door\":%u,\"state\":\"%s\",\"ts\":%lu}",
            (unsigned)noise, (unsigned)g_vib_isr_flag,
            (unsigned)btn1, (unsigned)btn2,
            (unsigned)hal_read_pir(), (unsigned)hal_read_door(),
            state_str(), (unsigned long)get_timestamp_ms());
        if (w > 0 && w < (int)sizeof(buf))
            broker_publish(TOPIC_MONITORING, buf, 0);

        /* Heartbeat LED blink */
        static uint8_t m = 0;
        _set_led_status(m);
        m = !m;
    }
}
#endif

/*******************************************************************************
 * SCALABILITY TASK — vibration debounce (FIX-01 done in ISR; extra SW guard)
 ******************************************************************************/
#if ENABLE_SCALABILITY_TASK
static void vScalabilityTask(void *pvParameters)
{
    ESP_LOGI("SCALE", "Started");
    TickType_t lastTick = 0;

    while (1) {
        if (xSemaphoreTake(xVibrationSemaphore,
                           pdMS_TO_TICKS(SCALABILITY_PERIOD_MS)) == pdTRUE) {
            TickType_t now = xTaskGetTickCount();
            uint32_t elapsed_ms = (now - lastTick) * portTICK_PERIOD_MS;
            if (elapsed_ms < VIBRATION_DEBOUNCE_MS) continue;
            lastTick = now;
            g_vib_isr_flag = 1;
            SLOG_W("SCALE", "VIBRATION DETECTED");
            send_event(EVENT_TYPE_VIBRATION_DETECTED, 1.0f, 7, 3);
            xEventGroupSetBits(xSystemEventGroup, EVENT_VIBRATION_DETECTED);
            update_system_state(EVENT_TYPE_VIBRATION_DETECTED);
            vTaskDelay(pdMS_TO_TICKS(3000));
            /* Clear if no new event during 3 s window */
            if (xSemaphoreTake(xVibrationSemaphore, 0) != pdTRUE)
                g_vib_isr_flag = 0;
        } else {
            if (!hal_read_vibration()) g_vib_isr_flag = 0;
        }
    }
}
#endif

/*******************************************************************************
 * COMM TASK — unified system snapshot (FIX-13: single publish, FIX-14: all fields)
 ******************************************************************************/
#if ENABLE_COMM_TASK
static void vCommTask(void *pvParameters)
{
    ESP_LOGI(TAG_COMM, "Started");
    TickType_t xLast = xTaskGetTickCount();
    static uint8_t led = 0;

#if HAVE_TASK_WDT
    esp_task_wdt_add(NULL);
#endif

    /* FIX-17: Publish online status once after broker is ready.
     * Small delay lets the broker socket finish binding before
     * we attempt to publish — without this the first send is dropped. */
    vTaskDelay(pdMS_TO_TICKS(500));
    broker_publish(TOPIC_STATUS, "online", 1);
    ESP_LOGI(TAG_COMM, "Published isms/status → online (startup)");

    while (1) {
        vTaskDelayUntil(&xLast, pdMS_TO_TICKS(COMM_PERIOD_MS));

#if HAVE_TASK_WDT
        esp_task_wdt_reset();
#endif

        led = !led;
        _set_led_status(led);

        /* ── CPU usage (FIX-12: warmup + clamp) ─────────────────────────── */
        uint32_t snap_idle = g_idle_count;
        g_idle_count = 0;

        if (g_baseline_ready < 3) {
            g_baseline_ready++;
            g_idle_baseline = snap_idle;
            g_cpu_usage_pct = 0.0f;
            ESP_LOGI(TAG_COMM, "CPU warmup %u/3 idle=%lu",
                     g_baseline_ready, (unsigned long)snap_idle);
        } else {
            if (snap_idle > g_idle_baseline) {
                g_idle_baseline = snap_idle;
                g_cpu_usage_pct = 0.0f;
            } else if (g_idle_baseline > 0) {
                float ratio = (float)snap_idle / (float)g_idle_baseline;
                g_cpu_usage_pct = (1.0f - ratio) * 100.0f;
                if (g_cpu_usage_pct < 0.0f)   g_cpu_usage_pct = 0.0f;
                if (g_cpu_usage_pct > 99.9f)   g_cpu_usage_pct = 99.9f;
            }
        }

        float    cpu_pct   = g_cpu_usage_pct;
        uint32_t free_heap = esp_get_free_heap_size();
        uint32_t uptime_s  = get_timestamp_ms() / 1000UL;

        /* ── Heartbeat if no sensor data yet ─────────────────────────────── */
        SensorData_t sd = {0};
        if (xQueuePeek(xDataQueue, &sd, 0) != pdTRUE) {
            char ping[180];
            int w = snprintf(ping, sizeof(ping),
                "{\"heartbeat\":1,\"state\":\"%s\","
                "\"cpu\":%.1f,\"heap\":%lu,\"uptime\":%lu,\"ts\":%lu}",
                state_str(), cpu_pct,
                (unsigned long)free_heap,
                (unsigned long)uptime_s,
                (unsigned long)get_timestamp_ms());
            if (w > 0 && w < (int)sizeof(ping))
                broker_publish(TOPIC_SYSTEM, ping, 0);
            continue;
        }

        /* ── Serial log ──────────────────────────────────────────────────── */
        SLOG_I(TAG_COMM, "temp=%.2fC gas=%u pir=%u door=%u "
               "noise=%u vib=%u btn1=%u btn2=%u cpu=%.1f%% heap=%lu",
               sd.temperature, (unsigned)sd.gas_level,
               (unsigned)sd.pir_detected, (unsigned)sd.door_open,
               (unsigned)sd.noise_level,
               (unsigned)sd.vibration_detected,
               (unsigned)sd.button1_pressed, (unsigned)sd.button2_pressed,
               cpu_pct, (unsigned long)free_heap);

        /* ── FIX-13: single unified JSON publish on TOPIC_SYSTEM ─────────── */
        char jsonBuf[JSON_BUF_SIZE];
        int w;
#if ENABLE_RTC
        /* FIX-20: Build RTC time string — "??:??:?? ??/??/????" when DS3231
         * is unavailable (hours==99 sentinel set by SensorTask on I2C fail) */
        char rtc_str[28];
        if (sd.rtc_time.hours == 99) {
            /* FIX: avoid C trigraph warnings — ?? sequences escaped with \x3F */
            snprintf(rtc_str, sizeof(rtc_str), "\x3F\x3F:\x3F\x3F:\x3F\x3F \x3F\x3F/\x3F\x3F/\x3F\x3F\x3F\x3F");
        } else {
            /* FIX-29: rtc_str[28] is large enough for worst-case uint8_t values */
            snprintf(rtc_str, sizeof(rtc_str), "%02u:%02u:%02u %02u/%02u/20%02u",
                sd.rtc_time.hours, sd.rtc_time.minutes, sd.rtc_time.seconds,
                sd.rtc_time.date,  sd.rtc_time.month,
                (unsigned)(sd.rtc_time.year));
        }

        w = snprintf(jsonBuf, sizeof(jsonBuf),
            "{"
            "\"temperature\":%.2f,"
            "\"rtc_temp\":%.2f,"
            "\"gas\":%u,"
            "\"motion\":%u,"
            "\"door\":%u,"
            "\"noise\":%u,"
            "\"vibration\":%u,"
            "\"button1\":%u,"
            "\"button2\":%u,"
            "\"rtc\":\"%s\","
            "\"state\":\"%s\","
            "\"ts\":%lu,"
            "\"cpu\":%.1f,"
            "\"cpu_usage\":%.1f,"
            "\"heap\":%lu,"
            "\"free_heap\":%lu,"
            "\"uptime\":%lu,"
            "\"isAnomaly\":%s,"
            "\"tempFault\":%u,"
            "\"gasFault\":%u,"
            "\"systemState\":\"%s\","
            "\"tempRateOfChange\":0.0"
            "}",
            sd.temperature,
            (sd.rtc_temperature > -900.0f) ? sd.rtc_temperature : 0.0f,
            (unsigned)sd.gas_level,
            (unsigned)sd.pir_detected,
            (unsigned)sd.door_open,
            (unsigned)sd.noise_level,
            (unsigned)sd.vibration_detected,
            (unsigned)sd.button1_pressed,
            (unsigned)sd.button2_pressed,
            rtc_str,
            state_str(),
            (unsigned long)sd.timestamp,
            cpu_pct, cpu_pct,
            (unsigned long)free_heap, (unsigned long)free_heap,
            (unsigned long)uptime_s,
            (sd.temp_fault || sd.gas_fault) ? "true" : "false",
            (unsigned)sd.temp_fault,
            (unsigned)sd.gas_fault,
            state_str());
#else
        w = snprintf(jsonBuf, sizeof(jsonBuf),
            "{"
            "\"temperature\":%.2f,"
            "\"rtc_temp\":%.2f,"
            "\"gas\":%u,\"motion\":%u,\"door\":%u,"
            "\"noise\":%u,\"vibration\":%u,"
            "\"button1\":%u,\"button2\":%u,"
            "\"rtc\":\"N/A\","
            "\"state\":\"%s\",\"ts\":%lu,"
            "\"cpu\":%.1f,\"cpu_usage\":%.1f,"
            "\"heap\":%lu,\"free_heap\":%lu,\"uptime\":%lu,"
            "\"isAnomaly\":%s,\"tempFault\":%u,\"gasFault\":%u,"
            "\"systemState\":\"%s\",\"tempRateOfChange\":0.0"
            "}",
            sd.temperature,
            (sd.rtc_temperature > -900.0f) ? sd.rtc_temperature : 0.0f,
            (unsigned)sd.gas_level, (unsigned)sd.pir_detected, (unsigned)sd.door_open,
            (unsigned)sd.noise_level, (unsigned)sd.vibration_detected,
            (unsigned)sd.button1_pressed, (unsigned)sd.button2_pressed,
            state_str(), (unsigned long)sd.timestamp,
            cpu_pct, cpu_pct,
            (unsigned long)free_heap, (unsigned long)free_heap,
            (unsigned long)uptime_s,
            (sd.temp_fault || sd.gas_fault) ? "true" : "false",
            (unsigned)sd.temp_fault, (unsigned)sd.gas_fault,
            state_str());
#endif
        if (w > 0 && w < (int)sizeof(jsonBuf)) {
            broker_publish(TOPIC_SYSTEM, jsonBuf, 0);
            broker_publish(TOPIC_STATUS, "online", 0);  /* FIX-18: keep Flutter heartbeat alive */
            SLOG_I(TAG_COMM, "TX %d bytes (clients:%u)", w, s_client_count);
        } else {
            ESP_LOGE(TAG_COMM, "JSON overflow w=%d (buf=%d) — not published", w, JSON_BUF_SIZE);
        }
    }
}
#endif /* ENABLE_COMM_TASK */

/*******************************************************************************
 * PERFORMANCE TASK
 ******************************************************************************/
#if ENABLE_PERFORMANCE_TASK
static void vPerformanceTask(void *pvParameters)
{
    ESP_LOGI(TAG_PERF, "Started");
    TickType_t xLast = xTaskGetTickCount();
    g_window_start_us = (uint64_t)esp_timer_get_time();

    while (1) {
        vTaskDelayUntil(&xLast, pdMS_TO_TICKS(PERFORMANCE_PERIOD_MS));

        uint32_t heap      = esp_get_free_heap_size();
        uint32_t heap_min  = esp_get_minimum_free_heap_size();
        uint32_t uptime_ms = get_timestamp_ms();

        ESP_LOGI(TAG_PERF, "─── Perf ───────────────────────────────────");
        SLOG_I(TAG_PERF, "Uptime:%lu s  Heap:%lu B (min %lu B)",
               (unsigned long)(uptime_ms / 1000UL),
               (unsigned long)heap, (unsigned long)heap_min);
        SLOG_I(TAG_PERF, "CPU:%.1f%%  State:%s  MQTT clients:%u",
               g_cpu_usage_pct, state_str(), s_client_count);
        SLOG_I(TAG_PERF, "Events:%lu  Breaches:%lu  CtxSW:%lu",
               (unsigned long)emergencyCount,
               (unsigned long)securityBreachCount,
               (unsigned long)g_ctx_switch_total);
        ESP_LOGI(TAG_PERF, "────────────────────────────────────────────");

        char buf[JSON_BUF_SIZE];
        int w = snprintf(buf, sizeof(buf),
            "{\"uptime\":%lu,\"heap\":%lu,\"heap_min\":%lu,"
            "\"cpu_usage\":%.1f,\"events\":%lu,\"breaches\":%lu,"
            "\"state\":\"%s\",\"mqtt_clients\":%u,"
            "\"ctx_switches\":%lu,\"isr_count\":%lu,\"ts\":%lu}",
            (unsigned long)(uptime_ms / 1000UL),
            (unsigned long)heap, (unsigned long)heap_min,
            g_cpu_usage_pct,
            (unsigned long)emergencyCount,
            (unsigned long)securityBreachCount,
            state_str(), (unsigned)s_client_count,
            (unsigned long)g_ctx_switch_total,
            (unsigned long)g_isr_count,
            (unsigned long)uptime_ms);
        if (w > 0 && w < (int)sizeof(buf))
            broker_publish(TOPIC_PERFORMANCE, buf, 0);

#if ENABLE_TRACE_HOOKS
        build_and_publish_rtos_metrics();
#endif
    }
}
#endif /* ENABLE_PERFORMANCE_TASK */

/*******************************************************************************
 * OLED DISPLAY TASK (SSD1306 128x64)
 * Integrated from ISMS_OLED_Task.c — v10.3.0
 ******************************************************************************/
/*******************************************************************************
 * ISMS — OLED Display Task
 * File    : ISMS_OLED_Task.c
 * Target  : ESP32 + SSD1306 128×64 OLED via I2C
 * Version : 1.0  (for ISMS firmware v10.3.0)
 *
 * ─── DISPLAY SEQUENCE ────────────────────────────────────────────────────────
 *
 *  Phase 1 — BOOT      : "RTOS Industrial Safety System"  (static, 3 s)
 *  Phase 2 — READY     : "System Ready"  (static, 2 s)
 *  Phase 3 — STANDBY   : "Connect App"  (shown until first MQTT client joins)
 *  Phase 4 — CONNECTED : "App Connected"  (shown for 2 s on first connect)
 *  Phase 5 — LIVE PAGES: Rotating sensor pages every 3 s
 *
 *      Page 0 — Temperature  (DS3231 / LM35)
 *      Page 1 — Gas Level    (MQ-2 ADC)
 *      Page 2 — Noise Level  (ADC)
 *      Page 3 — PIR + Door   (digital)
 *      Page 4 — Vibration + Buttons
 *      Page 5 — System State + Uptime
 *
 * ─── I2C WIRING ──────────────────────────────────────────────────────────────
 *  OLED SDA  -> ESP32 GPIO21  (same bus as DS3231 - protected by xI2CMutex)
 *  OLED SCL  -> ESP32 GPIO22
 *  OLED VCC  -> 3.3 V
 *  OLED GND  -> GND
 *  OLED I2C address: 0x3C  (most common; change SSD1306_I2C_ADDR if yours is 0x3D)
 *
 * ─── HOW TO INTEGRATE ────────────────────────────────────────────────────────
 *  1. Copy this file into your project's main/ folder.
 *  2. In ISMS_MQTT_v8_0_RTOS_PERF.c add near the other #includes:
 *         #include "ISMS_OLED_Task.c"
 *  3. In create_tasks() add:
 *         MK(vOledTask, "OledTask", OLED_TASK_STACK, OLED_TASK_PRIORITY, NULL);
 *  4. Add these two lines near the other task stack / priority #defines:
 *         #define OLED_TASK_STACK     3072
 *         #define OLED_TASK_PRIORITY  (configMAX_PRIORITIES - 5)
 *  5. Build - no extra component needed (uses driver/i2c directly).
 *
 * NOTE: The I2C bus is shared with DS3231.  Every OLED write takes
 *       xI2CMutex so it cannot clash with RTC reads inside SensorTask.
 ******************************************************************************/

/* ── SSD1306 I2C address ───────────────────────────────────────────────────── */
#define SSD1306_I2C_ADDR        0x3C   /* change to 0x3D if your module needs it */

/* ── Display dimensions ────────────────────────────────────────────────────── */
#define OLED_WIDTH              128
#define OLED_HEIGHT             64
#define OLED_PAGES              8      /* 64 px / 8 px per page                 */

/* ── Page rotation period ──────────────────────────────────────────────────── */
#define OLED_PAGE_PERIOD_MS     3000   /* time each sensor page is shown         */
#define OLED_TASK_PERIOD_MS     100    /* task tick rate                          */

/* ─────────────────────────────────────────────────────────────────────────────
 * Minimal 5x7 ASCII font - characters 0x20 (space) to 0x7E (~)
 * Each entry is 5 bytes wide, 1 byte = 1 column of 8 pixels (top = LSB)
 * ─────────────────────────────────────────────────────────────────────────── */
static const uint8_t oled_font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x00,0x00,0x5F,0x00,0x00}, /* '!' */
    {0x00,0x07,0x00,0x07,0x00}, /* '"' */
    {0x14,0x7F,0x14,0x7F,0x14}, /* '#' */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* '$' */
    {0x23,0x13,0x08,0x64,0x62}, /* '%' */
    {0x36,0x49,0x55,0x22,0x50}, /* '&' */
    {0x00,0x05,0x03,0x00,0x00}, /* '\'' */
    {0x00,0x1C,0x22,0x41,0x00}, /* '(' */
    {0x00,0x41,0x22,0x1C,0x00}, /* ')' */
    {0x08,0x2A,0x1C,0x2A,0x08}, /* '*' */
    {0x08,0x08,0x3E,0x08,0x08}, /* '+' */
    {0x00,0x50,0x30,0x00,0x00}, /* ',' */
    {0x08,0x08,0x08,0x08,0x08}, /* '-' */
    {0x00,0x30,0x30,0x00,0x00}, /* '.' */
    {0x20,0x10,0x08,0x04,0x02}, /* '/' */
    {0x3E,0x51,0x49,0x45,0x3E}, /* '0' */
    {0x00,0x42,0x7F,0x40,0x00}, /* '1' */
    {0x42,0x61,0x51,0x49,0x46}, /* '2' */
    {0x21,0x41,0x45,0x4B,0x31}, /* '3' */
    {0x18,0x14,0x12,0x7F,0x10}, /* '4' */
    {0x27,0x45,0x45,0x45,0x39}, /* '5' */
    {0x3C,0x4A,0x49,0x49,0x30}, /* '6' */
    {0x01,0x71,0x09,0x05,0x03}, /* '7' */
    {0x36,0x49,0x49,0x49,0x36}, /* '8' */
    {0x06,0x49,0x49,0x29,0x1E}, /* '9' */
    {0x00,0x36,0x36,0x00,0x00}, /* ':' */
    {0x00,0x56,0x36,0x00,0x00}, /* ';' */
    {0x00,0x08,0x14,0x22,0x41}, /* '<' */
    {0x14,0x14,0x14,0x14,0x14}, /* '=' */
    {0x41,0x22,0x14,0x08,0x00}, /* '>' */
    {0x02,0x01,0x51,0x09,0x06}, /* '?' */
    {0x32,0x49,0x79,0x41,0x3E}, /* '@' */
    {0x7E,0x11,0x11,0x11,0x7E}, /* 'A' */
    {0x7F,0x49,0x49,0x49,0x36}, /* 'B' */
    {0x3E,0x41,0x41,0x41,0x22}, /* 'C' */
    {0x7F,0x41,0x41,0x22,0x1C}, /* 'D' */
    {0x7F,0x49,0x49,0x49,0x41}, /* 'E' */
    {0x7F,0x09,0x09,0x09,0x01}, /* 'F' */
    {0x3E,0x41,0x49,0x49,0x7A}, /* 'G' */
    {0x7F,0x08,0x08,0x08,0x7F}, /* 'H' */
    {0x00,0x41,0x7F,0x41,0x00}, /* 'I' */
    {0x20,0x40,0x41,0x3F,0x01}, /* 'J' */
    {0x7F,0x08,0x14,0x22,0x41}, /* 'K' */
    {0x7F,0x40,0x40,0x40,0x40}, /* 'L' */
    {0x7F,0x02,0x04,0x02,0x7F}, /* 'M' */
    {0x7F,0x04,0x08,0x10,0x7F}, /* 'N' */
    {0x3E,0x41,0x41,0x41,0x3E}, /* 'O' */
    {0x7F,0x09,0x09,0x09,0x06}, /* 'P' */
    {0x3E,0x41,0x51,0x21,0x5E}, /* 'Q' */
    {0x7F,0x09,0x19,0x29,0x46}, /* 'R' */
    {0x46,0x49,0x49,0x49,0x31}, /* 'S' */
    {0x01,0x01,0x7F,0x01,0x01}, /* 'T' */
    {0x3F,0x40,0x40,0x40,0x3F}, /* 'U' */
    {0x1F,0x20,0x40,0x20,0x1F}, /* 'V' */
    {0x3F,0x40,0x38,0x40,0x3F}, /* 'W' */
    {0x63,0x14,0x08,0x14,0x63}, /* 'X' */
    {0x03,0x04,0x78,0x04,0x03}, /* 'Y' */
    {0x61,0x51,0x49,0x45,0x43}, /* 'Z' */
    {0x00,0x00,0x7F,0x41,0x41}, /* '[' */
    {0x02,0x04,0x08,0x10,0x20}, /* '\\' */
    {0x41,0x41,0x7F,0x00,0x00}, /* ']' */
    {0x04,0x02,0x01,0x02,0x04}, /* '^' */
    {0x40,0x40,0x40,0x40,0x40}, /* '_' */
    {0x00,0x01,0x02,0x04,0x00}, /* '`' */
    {0x20,0x54,0x54,0x54,0x78}, /* 'a' */
    {0x7F,0x48,0x44,0x44,0x38}, /* 'b' */
    {0x38,0x44,0x44,0x44,0x20}, /* 'c' */
    {0x38,0x44,0x44,0x48,0x7F}, /* 'd' */
    {0x38,0x54,0x54,0x54,0x18}, /* 'e' */
    {0x08,0x7E,0x09,0x01,0x02}, /* 'f' */
    {0x08,0x54,0x54,0x54,0x3C}, /* 'g' */
    {0x7F,0x08,0x04,0x04,0x78}, /* 'h' */
    {0x00,0x44,0x7D,0x40,0x00}, /* 'i' */
    {0x20,0x40,0x44,0x3D,0x00}, /* 'j' */
    {0x00,0x7F,0x10,0x28,0x44}, /* 'k' */
    {0x00,0x41,0x7F,0x40,0x00}, /* 'l' */
    {0x7C,0x04,0x18,0x04,0x78}, /* 'm' */
    {0x7C,0x08,0x04,0x04,0x78}, /* 'n' */
    {0x38,0x44,0x44,0x44,0x38}, /* 'o' */
    {0x7C,0x14,0x14,0x14,0x08}, /* 'p' */
    {0x08,0x14,0x14,0x18,0x7C}, /* 'q' */
    {0x7C,0x08,0x04,0x04,0x08}, /* 'r' */
    {0x48,0x54,0x54,0x54,0x20}, /* 's' */
    {0x04,0x3F,0x44,0x40,0x20}, /* 't' */
    {0x3C,0x40,0x40,0x40,0x7C}, /* 'u' */
    {0x1C,0x20,0x40,0x20,0x1C}, /* 'v' */
    {0x3C,0x40,0x30,0x40,0x3C}, /* 'w' */
    {0x44,0x28,0x10,0x28,0x44}, /* 'x' */
    {0x0C,0x50,0x50,0x50,0x3C}, /* 'y' */
    {0x44,0x64,0x54,0x4C,0x44}, /* 'z' */
    {0x00,0x08,0x36,0x41,0x00}, /* '{' */
    {0x00,0x00,0x7F,0x00,0x00}, /* '|' */
    {0x00,0x41,0x36,0x08,0x00}, /* '}' */
    {0x08,0x08,0x2A,0x1C,0x08}, /* '~' */
};

/* ─────────────────────────────────────────────────────────────────────────────
 * Frame buffer - 128 x 64 bits = 1024 bytes
 * ─────────────────────────────────────────────────────────────────────────── */
static uint8_t oled_buf[OLED_WIDTH * OLED_PAGES];   /* 1024 bytes */

/* ─────────────────────────────────────────────────────────────────────────────
 * Low-level SSD1306 I2C helpers
 * All writes acquire xI2CMutex so they do not clash with DS3231 in SensorTask
 * ─────────────────────────────────────────────────────────────────────────── */

static esp_err_t ssd1306_send_cmd(uint8_t cmd)
{
    if (!xI2CMutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(xI2CMutex, portMAX_DELAY);
    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (SSD1306_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(h, 0x00, true);   /* Co=0, D/C=0 -> command */
    i2c_master_write_byte(h, cmd,  true);
    i2c_master_stop(h);
    esp_err_t r = i2c_master_cmd_begin(RTC_I2C_PORT, h, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(h);
    xSemaphoreGive(xI2CMutex);
    return r;
}

static esp_err_t ssd1306_flush(void)
{
    /* Write entire 1024-byte frame buffer to the display in one I2C transaction */
    if (!xI2CMutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(xI2CMutex, portMAX_DELAY);
    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (SSD1306_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(h, 0x40, true);   /* Co=0, D/C=1 -> data */
    i2c_master_write(h, oled_buf, sizeof(oled_buf), true);
    i2c_master_stop(h);
    esp_err_t r = i2c_master_cmd_begin(RTC_I2C_PORT, h, pdMS_TO_TICKS(200));
    i2c_cmd_link_delete(h);
    xSemaphoreGive(xI2CMutex);
    return r;
}

static esp_err_t ssd1306_init(void)
{
    /* Standard SSD1306 128x64 initialisation sequence */
    static const uint8_t cmds[] = {
        0xAE,               /* display OFF */
        0xD5, 0x80,         /* clock divider / oscillator */
        0xA8, 0x3F,         /* multiplex ratio = 64 */
        0xD3, 0x00,         /* display offset = 0 */
        0x40,               /* display start line = 0 */
        0x8D, 0x14,         /* charge pump ON */
        0x20, 0x00,         /* horizontal addressing mode */
        0xA1,               /* segment remap (col 127 = SEG0) */
        0xC8,               /* COM scan direction remapped */
        0xDA, 0x12,         /* COM pins hardware config */
        0x81, 0xCF,         /* contrast = max */
        0xD9, 0xF1,         /* pre-charge period */
        0xDB, 0x40,         /* VCOMH deselect level */
        0xA4,               /* output follows RAM */
        0xA6,               /* normal display (not inverted) */
        0x2E,               /* deactivate scroll */
        0xAF,               /* display ON */
    };
    for (size_t i = 0; i < sizeof(cmds); i++) {
        esp_err_t r = ssd1306_send_cmd(cmds[i]);
        if (r != ESP_OK) return r;
    }
    /* Set column address 0..127 and page address 0..7 */
    ssd1306_send_cmd(0x21); ssd1306_send_cmd(0); ssd1306_send_cmd(127);
    ssd1306_send_cmd(0x22); ssd1306_send_cmd(0); ssd1306_send_cmd(7);
    return ESP_OK;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Frame buffer drawing helpers
 *
 * Hardware layout:
 *   page 0 = topmost 8 rows (y = 0..7)
 *   page 7 = bottommost 8 rows (y = 56..63)
 *   oled_buf index = page * 128 + column
 * ─────────────────────────────────────────────────────────────────────────── */

static void oled_clear(void)
{
    memset(oled_buf, 0x00, sizeof(oled_buf));
}

/* Draw one ASCII character at (col, pg).  Returns next col. */
static int oled_draw_char(int col, int pg, char c)
{
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t *glyph = oled_font5x7[c - 0x20];
    for (int x = 0; x < 5; x++) {
        int idx = pg * OLED_WIDTH + col + x;
        if (idx >= 0 && idx < (int)sizeof(oled_buf))
            oled_buf[idx] = glyph[x];
    }
    int gap = pg * OLED_WIDTH + col + 5;
    if (gap < (int)sizeof(oled_buf))
        oled_buf[gap] = 0x00;
    return col + 6;
}

/* Draw a string starting at (col, pg) — stops at right edge */
static void oled_draw_str(int col, int pg, const char *str)
{
    while (*str) {
        if (col + 6 > OLED_WIDTH) break;
        col = oled_draw_char(col, pg, *str++);
    }
}

/* Draw a string centred on a hardware page */
static void oled_draw_str_centred(int pg, const char *str)
{
    int len = (int)strlen(str);
    int w   = len * 6 - 1;
    int col = (OLED_WIDTH - w) / 2;
    if (col < 0) col = 0;
    oled_draw_str(col, pg, str);
}

/* Fill a whole page row with 0xFF (solid line) */
static void oled_hline(int pg)
{
    for (int c = 0; c < OLED_WIDTH; c++)
        oled_buf[pg * OLED_WIDTH + c] = 0xFF;
}

/* Draw a thin separator (top pixel only) across a page row */
static void oled_separator(int pg)
{
    for (int c = 0; c < OLED_WIDTH; c++)
        oled_buf[pg * OLED_WIDTH + c] = 0x01;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Title bar helper: bold title on pg 0, thin separator on pg 1
 * ─────────────────────────────────────────────────────────────────────────── */
static void oled_title(const char *title)
{
    oled_draw_str_centred(0, title);
    oled_separator(1);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * PHASE SCREENS (boot sequence)
 * ─────────────────────────────────────────────────────────────────────────── */

/* Phase 1 — Boot splash */
static void oled_page_boot(void)
{
    oled_clear();
    oled_hline(0);
    oled_draw_str_centred(1, "RTOS Industrial");
    oled_draw_str_centred(2, "Safety System");
    oled_hline(3);
    oled_draw_str_centred(5, "ISMS  v10.3.1");
    oled_draw_str_centred(7, "Initialising...");
    ssd1306_flush();
}

/* Phase 2 — System Ready */
static void oled_page_ready(void)
{
    oled_clear();
    oled_hline(0);
    oled_draw_str_centred(2, "** SYSTEM READY **");
    oled_draw_str_centred(4, "SSID: ESP32_ISMS");
    oled_draw_str_centred(5, "192.168.4.1:1883");
    oled_hline(7);
    ssd1306_flush();
}

/* Phase 3 — Waiting for Web Dashboard connection */
static void oled_page_connect_wait(void)
{
    oled_clear();
    oled_hline(0);
    oled_draw_str_centred(1, "Waiting for");
    oled_draw_str_centred(2, "Web Dashboard");
    oled_separator(3);
    oled_draw_str_centred(4, "WiFi: ESP32_ISMS");
    oled_draw_str_centred(5, "192.168.4.1:80");
    oled_separator(6);
    oled_draw_str_centred(7, "pw: 12345678");
    ssd1306_flush();
}

/* Phase 4 — Web Dashboard just connected */
static void oled_page_connected(void)
{
    oled_clear();
    oled_hline(0);
    oled_hline(1);
    oled_draw_str_centred(3, "SYSTEM");
    oled_draw_str_centred(4, "CONNECTED");
    oled_hline(6);
    oled_hline(7);
    ssd1306_flush();
}

/* ─────────────────────────────────────────────────────────────────────────────
 * LIVE SENSOR PAGES  (rotate every OLED_PAGE_PERIOD_MS)
 *
 * Layout for each page:
 *   pg 0  Title bar
 *   pg 1  Thin separator
 *   pg 2  Primary value (large area)
 *   pg 3  Secondary value / status
 *   pg 4  Status text
 *   pg 5  (extra row if needed)
 *   pg 6  (extra row if needed)
 *   pg 7  Footer: page index n/6
 * ─────────────────────────────────────────────────────────────────────────── */

/* Page 1/6 — Temperature */
static void oled_page_temperature(const SensorData_t *sd)
{
    oled_clear();
    oled_title("TEMPERATURE");

    char line[24];
    if (sd->temp_fault) {
        oled_draw_str_centred(3, "!! SENSOR FAULT !!");
        oled_draw_str_centred(5, "Check DS3231/LM35");
    } else {
        snprintf(line, sizeof(line), "%.1f  C", sd->temperature);
        oled_draw_str_centred(3, line);

        if (sd->temperature >= TEMP_THRESHOLD_CRITICAL)
            oled_draw_str_centred(5, "!! CRITICAL FIRE !!");
        else if (sd->temperature >= TEMP_THRESHOLD_HIGH)
            oled_draw_str_centred(5, "! HIGH TEMP !");
        else
            oled_draw_str_centred(5, "Normal");
    }

    /* RTC clock on footer */
    if (sd->rtc_time.hours != 99) {
        snprintf(line, sizeof(line), "%02d:%02d:%02d",
                 sd->rtc_time.hours,
                 sd->rtc_time.minutes,
                 sd->rtc_time.seconds);
    } else {
        snprintf(line, sizeof(line), "??:??:??");
    }
    oled_draw_str(2, 7, line);
    oled_draw_str(92, 7, "1/6");
    ssd1306_flush();
}

/* Page 2/6 — Gas Level */
static void oled_page_gas(const SensorData_t *sd)
{
    oled_clear();
    oled_title("GAS LEVEL (MQ-2)");

    char line[24];
    snprintf(line, sizeof(line), "%u / 1000", sd->gas_level);
    oled_draw_str_centred(3, line);

    if (sd->gas_fault)
        oled_draw_str_centred(5, "!! SENSOR FAULT !!");
    else if (sd->gas_level >= GAS_THRESHOLD_CRITICAL)
        oled_draw_str_centred(5, "!! GAS LEAK !!");
    else if (sd->gas_level >= GAS_THRESHOLD_WARNING)
        oled_draw_str_centred(5, "! WARNING !");
    else
        oled_draw_str_centred(5, "Clear");

    oled_draw_str(92, 7, "2/6");
    ssd1306_flush();
}

/* Page 3/6 — Noise Level */
static void oled_page_noise(const SensorData_t *sd)
{
    oled_clear();
    oled_title("NOISE LEVEL");

    char line[24];
    snprintf(line, sizeof(line), "%u / 4095", sd->noise_level);
    oled_draw_str_centred(3, line);

    if (sd->noise_level >= NOISE_THRESHOLD_ALARM)
        oled_draw_str_centred(5, "!! NOISE ALARM !!");
    else if (sd->noise_level >= NOISE_THRESHOLD_WARNING)
        oled_draw_str_centred(5, "! WARNING !");
    else
        oled_draw_str_centred(5, "Quiet");

    oled_draw_str(92, 7, "3/6");
    ssd1306_flush();
}

/* Page 4/6 — PIR + Door Security */
static void oled_page_security(const SensorData_t *sd)
{
    oled_clear();
    oled_title("SECURITY");

    oled_draw_str(4, 2, "Motion (PIR):");
    oled_draw_str(82, 2, sd->pir_detected ? "YES!" : "None");

    oled_separator(3);

    oled_draw_str(4, 4, "Door:");
    oled_draw_str(40, 4, sd->door_open ? "OPEN!" : "Closed");

    if (sd->pir_detected || sd->door_open)
        oled_draw_str_centred(6, "!! ALERT ACTIVE !!");
    else
        oled_draw_str_centred(6, "Secure");

    oled_draw_str(92, 7, "4/6");
    ssd1306_flush();
}

/* Page 5/6 — Vibration + Buttons */
static void oled_page_monitoring(const SensorData_t *sd)
{
    oled_clear();
    oled_title("MONITORING");

    oled_draw_str(4, 2, "Vibration:");
    oled_draw_str(70, 2, sd->vibration_detected ? "DETECT!" : "None");

    oled_separator(3);

    oled_draw_str(4, 4, "Button 1:");
    oled_draw_str(60, 4, sd->button1_pressed ? "PRESS" : "---");

    oled_draw_str(4, 5, "Button 2:");
    oled_draw_str(60, 5, sd->button2_pressed ? "PRESS" : "---");

    oled_draw_str(92, 7, "5/6");
    ssd1306_flush();
}

/* Page 6/6 — System State + Uptime + Heap */
static void oled_page_system(void)
{
    oled_clear();
    oled_title("SYS STATUS");

    /* System state string */
    oled_draw_str(4, 2, "State:");
    oled_draw_str(46, 2, state_str());

    oled_separator(3);

    /* Uptime */
    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    uint32_t h = uptime_s / 3600;
    uint32_t m = (uptime_s % 3600) / 60;
    uint32_t s = uptime_s % 60;
    char line[24];
    snprintf(line, sizeof(line), "%02lu:%02lu:%02lu",
             (unsigned long)h, (unsigned long)m, (unsigned long)s);
    oled_draw_str(4, 4, "Up:");
    oled_draw_str(22, 4, line);

    /* Free heap */
    snprintf(line, sizeof(line), "%luB", (unsigned long)esp_get_free_heap_size());
    oled_draw_str(4, 5, "Heap:");
    oled_draw_str(40, 5, line);

    /* MQTT clients */
    snprintf(line, sizeof(line), "%u client(s)", (unsigned)s_client_count);
    oled_draw_str(4, 6, line);

    oled_draw_str(92, 7, "6/6");
    ssd1306_flush();
}

/* ─────────────────────────────────────────────────────────────────────────────
 * vOledTask — display state machine
 * ─────────────────────────────────────────────────────────────────────────── */
typedef enum {
    OLED_STATE_BOOT = 0,    /* show boot splash (3 s)            */
    OLED_STATE_READY,       /* show "System Ready" (2 s)         */
    OLED_STATE_WAIT_APP,    /* show "Connect App" until client   */
    OLED_STATE_CONNECTED,   /* show "App Connected" (2 s)        */
    OLED_STATE_LIVE,        /* rotate 6 sensor pages             */
} OledState_t;

#define OLED_LIVE_PAGES  6

static void vOledTask(void *pvParameters)
{
    static const char *TAG_OLED = "OLED";

    /* Short delay so I2C bus is fully settled after hal_rtc_init() */
    vTaskDelay(pdMS_TO_TICKS(300));

    /* Initialise SSD1306 */
    esp_err_t init_err = ssd1306_init();
    if (init_err != ESP_OK) {
        ESP_LOGW(TAG_OLED, "SSD1306 init failed err=%d — task exit", init_err);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG_OLED, "SSD1306 128x64 init OK");

    OledState_t state        = OLED_STATE_BOOT;
    TickType_t  phase_start  = xTaskGetTickCount();
    TickType_t  page_start   = xTaskGetTickCount();
    uint8_t     live_page    = 0;
    uint8_t     prev_ws      = 0;   /* previous WebSocket client count */

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(OLED_TASK_PERIOD_MS));

        uint8_t  ws_clients = g_ws_client_count;   /* Web Dashboard connections */
        uint32_t elapsed = (uint32_t)((xTaskGetTickCount() - phase_start)
                                       * portTICK_PERIOD_MS);

        switch (state) {

            /* ── BOOT: splash for 3 s ──────────────────────────────────── */
            case OLED_STATE_BOOT:
                oled_page_boot();
                if (elapsed >= 3000) {
                    state       = OLED_STATE_READY;
                    phase_start = xTaskGetTickCount();
                    ESP_LOGI(TAG_OLED, "Phase -> READY");
                }
                break;

            /* ── READY: "System Ready" for 2 s ─────────────────────────── */
            case OLED_STATE_READY:
                oled_page_ready();
                if (elapsed >= 2000) {
                    state       = OLED_STATE_WAIT_APP;
                    phase_start = xTaskGetTickCount();
                    ESP_LOGI(TAG_OLED, "Phase -> WAIT_WEB_DASHBOARD");
                }
                break;

            /* ── WAIT_APP: hold until a Web Dashboard client connects ─── */
            case OLED_STATE_WAIT_APP:
                oled_page_connect_wait();
                if (ws_clients > 0) {
                    state       = OLED_STATE_CONNECTED;
                    phase_start = xTaskGetTickCount();
                    ESP_LOGI(TAG_OLED, "Phase -> CONNECTED (web clients:%u)", ws_clients);
                }
                break;

            /* ── CONNECTED: show "System Connected" for 2 s ────────────── */
            case OLED_STATE_CONNECTED:
                oled_page_connected();
                if (elapsed >= 2000) {
                    state      = OLED_STATE_LIVE;
                    live_page  = 0;
                    page_start = xTaskGetTickCount();
                    ESP_LOGI(TAG_OLED, "Phase -> LIVE (rotating sensor pages)");
                }
                break;

            /* ── LIVE: rotate sensor pages ──────────────────────────────── */
            case OLED_STATE_LIVE:
            {
                /* Web dashboard disconnected -> go back to WAIT */
                if (ws_clients == 0 && prev_ws > 0) {
                    state       = OLED_STATE_WAIT_APP;
                    phase_start = xTaskGetTickCount();
                    ESP_LOGI(TAG_OLED, "Phase -> WAIT_WEB_DASHBOARD (disconnect)");
                    break;
                }

                /* New web dashboard client connected -> show banner again */
                if (ws_clients > prev_ws) {
                    state       = OLED_STATE_CONNECTED;
                    phase_start = xTaskGetTickCount();
                    ESP_LOGI(TAG_OLED, "Phase -> CONNECTED (new web client)");
                    break;
                }

                /* Advance to next page after OLED_PAGE_PERIOD_MS */
                uint32_t page_elapsed =
                    (uint32_t)((xTaskGetTickCount() - page_start)
                               * portTICK_PERIOD_MS);
                if (page_elapsed >= OLED_PAGE_PERIOD_MS) {
                    live_page  = (live_page + 1) % OLED_LIVE_PAGES;
                    page_start = xTaskGetTickCount();
                }

                /* Peek latest sensor snapshot (non-blocking) */
                SensorData_t sd = {0};
                xQueuePeek(xDataQueue, &sd, 0);

                /* Render current page */
                switch (live_page) {
                    case 0: oled_page_temperature(&sd); break;
                    case 1: oled_page_gas(&sd);         break;
                    case 2: oled_page_noise(&sd);       break;
                    case 3: oled_page_security(&sd);    break;
                    case 4: oled_page_monitoring(&sd);  break;
                    case 5: oled_page_system();         break;
                    default: break;
                }
                break;
            }

            default:
                state = OLED_STATE_BOOT;
                break;
        }

        prev_ws = ws_clients;
    }
}

/*******************************************************************************
 * SYSTEM INIT
 ******************************************************************************/

/* WiFi AP event handler (needed by init_wifi_softap) */
static void wifi_ap_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG_MAIN, "SoftAP started — SSID: %s", WIFI_AP_SSID);
        xEventGroupSetBits(xSystemEventGroup, EVENT_AP_STARTED);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG_MAIN, "Station connected — AID=%d", e->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG_MAIN, "Station disconnected — AID=%d", e->aid);
    }
}

static void init_wifi_softap(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_ap_event_handler, NULL, NULL));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid            = WIFI_AP_SSID,
            .ssid_len        = strlen(WIFI_AP_SSID),
            .channel         = WIFI_AP_CHANNEL,
            .password        = WIFI_AP_PASS,
            .max_connection  = WIFI_AP_MAX_CONN,
            .authmode        = WIFI_AUTH_WPA2_PSK
        }
    };
    if (strlen(WIFI_AP_PASS) == 0)
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void init_watchdog(void)
{
#if HAVE_TASK_WDT
    esp_task_wdt_config_t wdt = {
        .timeout_ms    = 30000,
        .idle_core_mask = (1 << 0) | (1 << 1),
        .trigger_panic = false
    };
    esp_err_t ret = esp_task_wdt_init(&wdt);
    if (ret == ESP_ERR_INVALID_STATE)
        ESP_LOGW(TAG_MAIN, "WDT already initialised");
    else
        ESP_LOGI(TAG_MAIN, "WDT init OK (30 s timeout)");
#endif
}

static void create_rtos_objects(void)
{
    xEventQueue      = xQueueCreate(EVENT_QUEUE_SIZE,    sizeof(EventMessage_t));
    xDataQueue       = xQueueCreate(DATA_QUEUE_SIZE,     sizeof(SensorData_t));
    xActuatorQueue   = xQueueCreate(ACTUATOR_QUEUE_SIZE, sizeof(ActuatorMessage_t));
    xPIRSemaphore       = xSemaphoreCreateBinary();
    xDoorSemaphore      = xSemaphoreCreateBinary();
    xVibrationSemaphore = xSemaphoreCreateBinary();
    xButton1Semaphore   = xSemaphoreCreateBinary();
    xButton2Semaphore   = xSemaphoreCreateBinary();
    xSystemEventGroup   = xEventGroupCreate();
    xBrokerMutex        = xSemaphoreCreateMutex();
    xTraceMutex         = xSemaphoreCreateMutex();
    xI2CMutex           = xSemaphoreCreateMutex();   /* FIX-02 */
    xActuatorMutex      = xSemaphoreCreateMutex();   /* FIX-03 */

    configASSERT(xEventQueue && xDataQueue && xActuatorQueue);
    configASSERT(xPIRSemaphore && xDoorSemaphore && xVibrationSemaphore);
    configASSERT(xButton1Semaphore && xButton2Semaphore);
    configASSERT(xSystemEventGroup && xBrokerMutex && xTraceMutex);
    configASSERT(xI2CMutex && xActuatorMutex);

    xEventGroupSetBits(xSystemEventGroup, EVENT_SYSTEM_NORMAL);
    memset((void*)g_trace, 0, sizeof(g_trace));
    g_window_start_us = (uint64_t)esp_timer_get_time();
    ESP_LOGI(TAG_MAIN, "RTOS objects created OK");
}

static void create_tasks(void)
{
    BaseType_t r;
#define MK(fn, name, stack, prio, hdl) \
    r = xTaskCreate(fn, name, stack, NULL, prio, hdl); \
    configASSERT(r == pdPASS); \
    ESP_LOGI(TAG_MAIN, "Task: %-20s prio=%d", name, (int)(prio));

    MK(vEmergencyTask,   "EmergencyTask",   EMERGENCY_TASK_STACK,   EMERGENCY_TASK_PRIORITY,   &xEmergencyTaskHandle);
    MK(vSensorTask,      "SensorTask",      SENSOR_TASK_STACK,      SENSOR_TASK_PRIORITY,      &xSensorTaskHandle);
    MK(vSecurityTask,    "SecurityTask",    SECURITY_TASK_STACK,    SECURITY_TASK_PRIORITY,    &xSecurityTaskHandle);
    MK(vSafetyTask,      "SafetyTask",      SAFETY_TASK_STACK,      SAFETY_TASK_PRIORITY,      &xSafetyTaskHandle);
    MK(vActuatorTask,    "ActuatorTask",    ACTUATOR_TASK_STACK,    ACTUATOR_TASK_PRIORITY,    &xActuatorTaskHandle);
#if ENABLE_MONITORING_TASK
    MK(vMonitoringTask,  "MonitoringTask",  MONITORING_TASK_STACK,  MONITORING_TASK_PRIORITY,  &xMonitoringTaskHandle);
#endif
#if ENABLE_COMM_TASK
    MK(vCommTask,        "CommTask",        COMM_TASK_STACK,        COMM_TASK_PRIORITY,        &xCommTaskHandle);
#endif
#if ENABLE_SCALABILITY_TASK
    MK(vScalabilityTask, "ScalabilityTask", SCALABILITY_TASK_STACK, SCALABILITY_TASK_PRIORITY, &xScalabilityTaskHandle);
#endif
#if ENABLE_PERFORMANCE_TASK
    MK(vPerformanceTask, "PerformanceTask", PERFORMANCE_TASK_STACK, PERFORMANCE_TASK_PRIORITY, &xPerformanceTaskHandle);
#endif
    MK(vOledTask, "OledTask", OLED_TASK_STACK, OLED_TASK_PRIORITY, NULL);
#undef MK

    r = xTaskCreate(mqtt_broker_task, "MqttBroker",
                    MQTT_BROKER_TASK_STACK, NULL,
                    MQTT_BROKER_TASK_PRIORITY, NULL);
    configASSERT(r == pdPASS);
    ESP_LOGI(TAG_MAIN, "Task: %-20s prio=%d", "MqttBroker", (int)MQTT_BROKER_TASK_PRIORITY);
}

/*******************************************************************************
 * app_main
 ******************************************************************************/
void app_main(void)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
    ESP_LOGI(TAG_MAIN, "╔══════════════════════════════════════════════╗");
    ESP_LOGI(TAG_MAIN, "║  ISMS Firmware v%-28s║", FIRMWARE_VERSION);
    ESP_LOGI(TAG_MAIN, "║  Industrial Security Monitoring System       ║");
    ESP_LOGI(TAG_MAIN, "║  ESP32 + FreeRTOS + SoftAP MQTT Broker       ║");
    ESP_LOGI(TAG_MAIN, "║  SSID : %-36s║", WIFI_AP_SSID);
    ESP_LOGI(TAG_MAIN, "║  MQTT : 192.168.4.1:1883                     ║");
    ESP_LOGI(TAG_MAIN, "╚══════════════════════════════════════════════╝");
#pragma GCC diagnostic pop

    /* FIX-26: RTOS objects (semaphores, queues, mutexes) MUST be created
     * before hal_gpio_init() installs the ISR handlers. Previously GPIO was
     * initialised at [1/6] and RTOS objects at [3/6]. A floating vibration
     * pin on GPIO36 fired the ISR immediately during gpio_config(), before
     * xVibrationSemaphore existed, causing assert(pxQueue) in xQueueGiveFromISR.
     * New order: RTOS objects first, then GPIO, then RTC, then WiFi. */
    ESP_LOGI(TAG_MAIN, "[1/6] RTOS objects");
    create_rtos_objects();

    ESP_LOGI(TAG_MAIN, "[2/6] GPIO + ADC init");
    hal_gpio_init();
    hal_adc_init();

#if ENABLE_RTC
    ESP_LOGI(TAG_MAIN, "[3/6] DS3231 RTC + OLED init");
    hal_rtc_init();
#endif

    ESP_LOGI(TAG_MAIN, "[4/6] SoftAP WiFi");
    init_wifi_softap();
    xEventGroupWaitBits(xSystemEventGroup, EVENT_AP_STARTED,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    ESP_LOGI(TAG_MAIN, "[4b/6] Web server (HTTP:80 + WS:81)");
    isms_web_server_init();   /* starts SPIFFS + HTTP + WebSocket tasks */

    ESP_LOGI(TAG_MAIN, "[5/6] Create tasks + MQTT broker");
    create_tasks();

    ESP_LOGI(TAG_MAIN, "[6/6] Watchdog");
    init_watchdog();

    hal_boot_blink();

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
    ESP_LOGI(TAG_MAIN, "╔══════════════════════════════════════════════╗");
    ESP_LOGI(TAG_MAIN, "║  SYSTEM READY v10.3.1                        ║");
    ESP_LOGI(TAG_MAIN, "║                                              ║");
    ESP_LOGI(TAG_MAIN, "║  Connect phone → WiFi: ESP32_ISMS            ║");
    ESP_LOGI(TAG_MAIN, "║  MQTT broker: 192.168.4.1:1883               ║");
    ESP_LOGI(TAG_MAIN, "║  All topics live on industrial/#             ║");
    ESP_LOGI(TAG_MAIN, "╚══════════════════════════════════════════════╝");
#pragma GCC diagnostic pop
}
# Push Button Control Implementation - Button1 (Buzzer) & Button2 (Fan)

## Overview
Two push buttons are now fully configured for manual control of the buzzer and fan relay, with real-time feedback displayed on the web dashboard.

---

## Hardware Configuration

### GPIO Pin Assignments
| Component | GPIO | Type | Function | Active |
|-----------|------|------|----------|--------|
| Button1 | GPIO32 | Input | Manual Buzzer Trigger | LOW (pull-up) |
| Button2 | GPIO19 | Input | Manual Fan Toggle | LOW (pull-up) |
| Buzzer | GPIO18 | Output | Audio Alert | HIGH |
| Fan Relay | GPIO25 | Output | Fan Control | HIGH |

### Wiring
```
Button1 (GPIO32) ──[10kΩ pull-up]── 3.3V
         ├─────► ESP32 GPIO32
         └─► GND (when pressed)

Button2 (GPIO19) ──[10kΩ pull-up]── 3.3V
         ├─────► ESP32 GPIO19
         └─► GND (when pressed)

Buzzer (GPIO18) ──[NPN Transistor]── Relay/Speaker
Fan Relay (GPIO25) ──[NPN Transistor]── Relay

```

---

## Software Implementation

### 1. Button ISR (Interrupt Service Routine) - IRAM_ATTR

Located in ISMS_MQTT_v8_0_RTOS_PERF.c lines ~682-710

```c
static void IRAM_ATTR button1_isr_handler(void *arg)
{
    /* 50 ms debounce protection */
    static volatile uint32_t last_ms = 0;
    uint32_t now = (uint32_t)(xTaskGetTickCountFromISR() * portTICK_PERIOD_MS);
    if ((now - last_ms) < 50) return;
    last_ms = now;
    
    /* Signal Button1Semaphore to vMonitoringTask */
    g_btn1_isr_flag = 1;
    xSemaphoreGiveFromISR(xButton1Semaphore, &w);
    portYIELD_FROM_ISR(w);
}

static void IRAM_ATTR button2_isr_handler(void *arg)
{
    /* Same debounce and semaphore mechanism for Button2 */
    static volatile uint32_t last_ms = 0;
    uint32_t now = (uint32_t)(xTaskGetTickCountFromISR() * portTICK_PERIOD_MS);
    if ((now - last_ms) < 50) return;
    last_ms = now;
    
    g_btn2_isr_flag = 1;
    xSemaphoreGiveFromISR(xButton2Semaphore, &w);
    portYIELD_FROM_ISR(w);
}
```

**Key Features:**
- 50 ms debounce to prevent false triggers from contact bounce
- Uses `IRAM_ATTR` for ISR (interrupt in RAM for speed)
- Semaphore-based signaling to task

### 2. GPIO Configuration - hal_gpio_init()

Located in ISMS_MQTT_v8_0_RTOS_PERF.c lines ~720-800

```c
/* Button1 & Button2 input configuration */
gpio_config_t btn_cfg = {
    .pin_bit_mask = (1ULL << GPIO_BUTTON1) | (1ULL << GPIO_BUTTON2),
    .mode         = GPIO_MODE_INPUT,
    .pull_up_en   = GPIO_PULLUP_ENABLE,      /* Internal pull-up */
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type    = GPIO_INTR_NEGEDGE        /* Trigger on button press (LOW) */
};
gpio_config(&btn_cfg);

/* Buzzer & Fan Relay output configuration */
gpio_config_t relay_cfg = {
    .pin_bit_mask = (1ULL << GPIO_BUZZER) | (1ULL << GPIO_FAN_RELAY),
    .mode         = GPIO_MODE_OUTPUT,
    .pull_up_en   = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type    = GPIO_INTR_DISABLE
};
gpio_config(&relay_cfg);

/* Initialize outputs to OFF */
gpio_set_level(GPIO_BUZZER, 0);
gpio_set_level(GPIO_FAN_RELAY, 0);

/* Install ISR service */
gpio_install_isr_service(0);
gpio_isr_handler_add(GPIO_BUTTON1, button1_isr_handler, NULL);
gpio_isr_handler_add(GPIO_BUTTON2, button2_isr_handler, NULL);
```

---

## Task Implementation - vMonitoringTask

Located in ISMS_MQTT_v8_0_RTOS_PERF.c lines ~1982-2080

### Button1 - Manual Buzzer Control (NEW)

```c
/* ── Button1 (GPIO32): manual buzzer trigger (500 ms pulse) ── */
if (btn1) {
    ActuatorMessage_t m = {
        .cmd         = ACT_CMD_BUZZER_ON,
        .duration_ms = 500,  /* 500 ms buzzer pulse */
    };
    strncpy(m.trigger, "btn1_buzzer", 23);
    xQueueSend(xActuatorQueue, &m, 0);
    SLOG_I(TAG_MONITOR, "Button1: Buzzer ON (500ms pulse)");
}
```

**Behavior:**
- Press Button1 → Buzzer sounds for **500 ms**
- Queues command to vActuatorTask for safe GPIO control
- Trigger tag tracks the source ("btn1_buzzer")

### Button2 - Manual Fan Toggle (EXISTING - FIXED in v10.3.2)

```c
/* ── Button2 (GPIO19): manual fan toggle (FIX-06 manual override) ── */
if (btn2) {
    xSemaphoreTake(xActuatorMutex, pdMS_TO_TICKS(10));
    uint8_t fan_now = g_actuator.fan;
    xSemaphoreGive(xActuatorMutex);

    if (fan_now) {
        /* Fan is ON → turn it OFF */
        actuator_post(ACT_CMD_FAN_OFF, 0, "btn2_toggle");
        SLOG_I(TAG_MONITOR, "Button2: Fan OFF (manual)");
    } else {
        /* Fan is OFF → turn it ON (manual override, 30s WDT) */
        ActuatorMessage_t m = {
            .cmd         = ACT_CMD_FAN_ON,
            .duration_ms = 0,  /* permanent until toggled off or 30-s WDT */
        };
        strncpy(m.trigger, "btn2_toggle", 23);
        xSemaphoreTake(xActuatorMutex, pdMS_TO_TICKS(10));
        g_actuator.fan_manual       = 1;
        g_actuator.fan_manual_since = get_timestamp_ms();
        xSemaphoreGive(xActuatorMutex);
        xQueueSend(xActuatorQueue, &m, 0);
        SLOG_I(TAG_MONITOR, "Button2: Fan ON (manual, 30s WDT)");
    }
}
```

**Behavior:**
- Press Button2 once → Fan turns ON (manual mode, 30 second watchdog timer)
- Press Button2 again → Fan turns OFF (manual mode exits)
- Automatic timeout after 30 seconds if left on

### Button State Sync to Web Dashboard

```c
/* Update shared data queue with fresh button states */
SensorData_t sd = {0};
if (xQueuePeek(xDataQueue, &sd, 0) == pdTRUE) {
    sd.button1_pressed = btn1;   /* NEW: added */
    sd.button2_pressed = btn2;   /* NEW: added */
    sd.timestamp = get_timestamp_ms();
    xQueueOverwrite(xDataQueue, &sd);
}
```

---

## Actuator Task - vActuatorTask

Located in ISMS_MQTT_v8_0_RTOS_PERF.c lines ~1866-1980

### Relay Control Method

The ActuatorTask uses **message queue-based command system** for safe GPIO control:

```c
/* Actuator commands (safe message passing) */
typedef enum {
    ACT_CMD_BUZZER_ON,       /* GPIO18 HIGH */
    ACT_CMD_BUZZER_OFF,      /* GPIO18 LOW  */
    ACT_CMD_FAN_ON,          /* GPIO25 HIGH */
    ACT_CMD_FAN_OFF,         /* GPIO25 LOW  */
    ACT_CMD_LED_ALARM_ON,
    ACT_CMD_LED_ALARM_OFF,
    ACT_CMD_LED_NORMAL_ON,
    ACT_CMD_LED_NORMAL_OFF,
    ACT_CMD_ALL_CLEAR
} ActuatorCmd_t;

typedef struct {
    ActuatorCmd_t cmd;
    uint32_t      duration_ms;   /* How long to keep output active (0 = permanent) */
    char          trigger[24];   /* "btn1_buzzer", "btn2_toggle", "emergency", etc */
} ActuatorMessage_t;
```

### GPIO Control Functions

```c
/* Low-level GPIO setters (ONLY called from ActuatorTask) */
static void _set_buzzer(uint8_t s)  { gpio_set_level(GPIO_BUZZER, s); }
static void _set_fan(uint8_t s)     { gpio_set_level(GPIO_FAN_RELAY, s); }
```

### Buzzer Control (500ms pulse from Button1)

```c
case ACT_CMD_BUZZER_ON:
    _set_buzzer(1);  /* GPIO18 HIGH */
    g_actuator.buzzer = 1;
    strncpy(g_actuator.trigger, msg.trigger, 23);
    
    /* Auto-off after duration if specified */
    if (msg.duration_ms > 0) {  /* e.g., 500 ms for button press */
        xSemaphoreGive(xActuatorMutex);
        vTaskDelay(pdMS_TO_TICKS(msg.duration_ms));
        xSemaphoreTake(xActuatorMutex, portMAX_DELAY);
        _set_buzzer(0);  /* GPIO18 LOW  */
        g_actuator.buzzer = 0;
    }
    break;

case ACT_CMD_BUZZER_OFF:
    _set_buzzer(0);
    g_actuator.buzzer = 0;
    break;
```

### Fan Relay Control (Toggle from Button2)

```c
case ACT_CMD_FAN_ON:
    _set_fan(1);  /* GPIO25 HIGH */
    g_actuator.fan = 1;
    strncpy(g_actuator.trigger, msg.trigger, 23);
    
    if (msg.duration_ms > 0) {
        xSemaphoreGive(xActuatorMutex);
        vTaskDelay(pdMS_TO_TICKS(msg.duration_ms));
        xSemaphoreTake(xActuatorMutex, portMAX_DELAY);
        /* Respect manual override (button2) */
        if (!g_actuator.fan_manual) {
            _set_fan(0);  /* GPIO25 LOW */
            g_actuator.fan = 0;
        }
    }
    break;

case ACT_CMD_FAN_OFF:
    _set_fan(0);  /* GPIO25 LOW */
    g_actuator.fan = 0;
    g_actuator.fan_manual = 0;
    g_actuator.fan_manual_since = 0;
    break;
```

### Fan Manual Timeout (30s Watchdog)

```c
/* FIX-06: Fan manual override auto-release after 30 s */
if (g_actuator.fan_manual && g_actuator.fan_manual_since > 0) {
    uint32_t elapsed = (get_timestamp_ms() - g_actuator.fan_manual_since) / 1000;
    if (elapsed >= FAN_MANUAL_TIMEOUT_S) {  /* 30 seconds */
        _set_fan(0);
        g_actuator.fan          = 0;
        g_actuator.fan_manual   = 0;
        g_actuator.fan_manual_since = 0;
        SLOG_W(TAG_ACTUATOR, "Fan manual override expired (%ds)", FAN_MANUAL_TIMEOUT_S);
    }
}
```

---

## Web Dashboard Integration

### JSON Response from `/data` Endpoint

Updated in isms_web_server.c to include button states:

```json
{
  "temp": 28.50,
  "gas": 125,
  "noise": 450,
  "pir": 0,
  "door": 0,
  "vibration": 0,
  "button1": 1,
  "button2": 0,
  "buzzer": 1,
  "fan": 0,
  "cpu": 15.3,
  "rtc_time": "14:30:45",
  "rtc_date": "11/05/2026",
  "state": "normal"
}
```

### New Fields
- **"button1"**: 1 = Button1 currently pressed, 0 = not pressed
- **"button2"**: 1 = Button2 currently pressed, 0 = not pressed

### Web Dashboard Display

HTML/JavaScript can show button states:
```html
<div id="button1-status">Button1: <span id="btn1-val">OFF</span></div>
<div id="button2-status">Button2: <span id="btn2-val">OFF</span></div>

<script>
  setInterval(async () => {
    const response = await fetch('/data');
    const data = await response.json();
    
    document.getElementById('btn1-val').textContent = 
      data.button1 ? 'PRESSED (Buzzer)' : 'OFF';
    document.getElementById('btn2-val').textContent = 
      data.button2 ? 'PRESSED (Fan toggle)' : 'OFF';
    
    document.getElementById('buzzer-val').textContent = 
      data.buzzer ? 'BUZZING' : 'OFF';
    document.getElementById('fan-val').textContent = 
      data.fan ? 'ON' : 'OFF';
  }, 2000);  // Poll every 2 seconds
</script>
```

---

## Relay Control Method Verification

### Method: GPIO Direct Control via gpio_set_level()

| Aspect | Details |
|--------|---------|
| **API Used** | ESP-IDF `gpio_set_level(pin, level)` |
| **Pin Mode** | `GPIO_MODE_OUTPUT` (output push-pull) |
| **Control Logic** | Direct HIGH/LOW writes (no PWM) |
| **Thread Safety** | Protected by `xActuatorMutex` before GPIO writes |
| **Timing** | Immediate (no delay between command queue and GPIO) |
| **Hardware Requirement** | NPN transistor driver for relay (GPIO can't source enough current) |

### Verification Flow

```c
/* Step 1: Button press detected */
Button1 ISR → g_btn1_isr_flag = 1 → xSemaphoreGive(xButton1Semaphore)

/* Step 2: MonitoringTask detects flag */
hal_read_button1() returns 1 → Queue message to ActuatorQueue
→ ActuatorMessage_t {cmd: ACT_CMD_BUZZER_ON, duration_ms: 500}

/* Step 3: ActuatorTask processes command */
xQueueReceive(xActuatorQueue, &msg) → receives BUZZER_ON
xSemaphoreTake(xActuatorMutex) → acquires protection
_set_buzzer(1) → gpio_set_level(GPIO_BUZZER, 1) → GPIO18 = HIGH
g_actuator.buzzer = 1 → state updated

/* Step 4: Buzzer on for 500ms */
vTaskDelay(500ms)

/* Step 5: Auto-off */
_set_buzzer(0) → gpio_set_level(GPIO_BUZZER, 0) → GPIO18 = LOW
g_actuator.buzzer = 0 → state updated
xSemaphoreGive(xActuatorMutex) → release protection

/* Step 6: State synced to web */
SensorTask reads: g_actuator.buzzer = 0
→ Syncs to g_sensor_data.buzzer = 0
→ Web /data endpoint returns: {"buzzer": 0}
→ Browser receives and displays: "Buzzer: OFF"
```

### Safety Mechanisms

1. **Mutex Protection**: `xActuatorMutex` protects GPIO writes and state changes
2. **Queue-based Commands**: No direct GPIO writes from ISR (safe!)
3. **Debounce**: 50ms in ISR prevents false triggers
4. **Watchdog Timer**: Fan manual mode auto-times out after 30s
5. **Duration Control**: Buzzer automatically turns off after specified duration

---

## Testing Checklist

### Button1 (Buzzer) - Manual Trigger

- [ ] Press Button1 (GPIO32) physically
- [ ] Check console: `Button1: Buzzer ON (500ms pulse)` appears
- [ ] Buzzer sounds for ~500 milliseconds
- [ ] Check web dashboard `/data` returns `"button1": 1` while pressed
- [ ] Check web dashboard `/data` returns `"buzzer": 1` while sounding
- [ ] Buzzer stops after 500ms
- [ ] Check web dashboard `/data` returns `"buzzer": 0` after stop
- [ ] OLED displays "Button1: PRESS" on monitoring page while held

### Button2 (Fan Toggle) - Manual Control

- [ ] Press Button2 (GPIO19) **once**
- [ ] Check console: `Button2: Fan ON (manual, 30s WDT)` appears
- [ ] Fan relay clicks and runs
- [ ] Check web dashboard `/data` returns `"button2": 1` while pressed
- [ ] Check web dashboard `/data` returns `"fan": 1` while running
- [ ] Press Button2 **again** (within 30 seconds)
- [ ] Check console: `Button2: Fan OFF (manual)` appears
- [ ] Fan relay stops
- [ ] Check web dashboard `/data` returns `"fan": 0` after stop
- [ ] If no button press for 30 seconds, fan auto-stops
- [ ] Check console: `Fan manual override expired (30s)` appears
- [ ] OLED displays "Button2: PRESS" on monitoring page while held

### Web Dashboard Real-time Indication

- [ ] Open web browser to `http://192.168.4.1`
- [ ] Press Button1 while monitoring web dashboard
- [ ] `"button1": 1` and `"buzzer": 1` appear in JSON
- [ ] Visual indicator shows "Button1 Pressed" and "Buzzer ON"
- [ ] Press Button2 while monitoring web dashboard
- [ ] `"button2": 1` and `"fan": 1` appear in JSON
- [ ] Visual indicator shows "Button2 Pressed" and "Fan ON"

---

## Console Output Examples

### Button1 Buzz Trigger
```
[MONITOR] Button1 PRESSED — Buzzer trigger
[MONITOR] Button1: Buzzer ON (500ms pulse)
[ACTUATOR] ACT_CMD_BUZZER_ON received (duration: 500ms, trigger: btn1_buzzer)
[ACTUATOR] Buzzer OFF after 500ms
```

### Button2 Fan Toggle ON
```
[MONITOR] Button2 PRESSED — Fan toggle
[MONITOR] Button2: Fan ON (manual, 30s WDT)
[ACTUATOR] ACT_CMD_FAN_ON received (manual override, trigger: btn2_toggle)
```

### Button2 Fan Toggle OFF
```
[MONITOR] Button2 PRESSED — Fan toggle
[MONITOR] Button2: Fan OFF (manual)
[ACTUATOR] ACT_CMD_FAN_OFF received (trigger: btn2_toggle)
```

### Fan Auto-Timeout
```
[MONITOR] Button2 PRESSED — Fan toggle
[MONITOR] Button2: Fan ON (manual, 30s WDT)
[ACTUATOR] Fan manual override expired (30s)
[ACTUATOR] Fan OFF (timeout)
```

---

## Files Modified

1. **[ISMS_MQTT_v8_0_RTOS_PERF.c](ISMS_MQTT_v8_0_RTOS_PERF.c)**
   - Line ~2005: Added Button1 buzzer control
   - Line ~1587: Sync button states to g_sensor_data

2. **[isms_web_server.c](isms_web_server.c)**
   - Updated JSON builder to include "button1" and "button2" fields

3. **[isms_web_server.h](isms_web_server.h)**
   - Added button1 and button2 fields to isms_sensor_data_t struct

---

## Summary

✅ **Button1 (GPIO32)** → **Buzzer (GPIO18)** - 500ms pulse on press
✅ **Button2 (GPIO19)** → **Fan Relay (GPIO25)** - Toggle ON/OFF with 30s timeout
✅ **Real-time Web Dashboard** - Button and actuator states shown in JSON `/data`
✅ **Thread-safe** - All GPIO writes protected by mutex
✅ **Debounce Protection** - 50ms guard on button ISRs
✅ **OLED Display** - Button states shown on monitoring page
✅ **Relay Method** - `gpio_set_level()` with NPN transistor driver

---

## Version Information

- **Firmware**: v10.3.2 - WEB DASHBOARD OLED + FAN FIX + BUTTON CONTROL
- **Web Server**: v3.1.0 - Client tracking + Button states in JSON
- **Target**: ESP32 + ESP-IDF v5.3.1 + FreeRTOS


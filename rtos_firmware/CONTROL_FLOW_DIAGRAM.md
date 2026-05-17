# Quick Reference — GPIO Control Flow & Pin Map
## RTOS Firmware v10.3.2

---

## GPIO PIN ASSIGNMENTS

```
┌─────────────────────────────────────────────────────────────┐
│           ESP32 GPIO MAP — INPUT/OUTPUT/ADC                │
└─────────────────────────────────────────────────────────────┘

INPUTS (Sensors & Buttons):
  GPIO4   │ PIR Sensor              │ Active-HIGH, pull-down
  GPIO5   │ Door Sensor             │ Active-HIGH, pull-down
  GPIO27  │ Vibration Sensor        │ Active-HIGH, pull-up
  GPIO33  │ Noise (ADC5)            │ ADC input
  GPIO32  │ Button1 (Buzzer)        │ Active-LOW, pull-up, ISR
  GPIO19  │ Button2 (Fan)           │ Active-LOW, pull-up, ISR
  GPIO34  │ Temperature LM35 (ADC6) │ ADC input
  GPIO35  │ Gas MQ-2 (ADC7)         │ ADC input
  GPIO21  │ RTC SDA (I2C)           │ I2C data
  GPIO22  │ RTC SCL (I2C)           │ I2C clock

OUTPUTS (Actuators & LEDs):
  GPIO18  │ Buzzer                  │ Active-HIGH
  GPIO25  │ Fan Relay               │ Active-LOW (inverted)
  GPIO2   │ LED Status              │ Active-HIGH
  GPIO15  │ LED Alarm               │ Active-HIGH
  GPIO16  │ LED Normal              │ Active-HIGH
```

---

## BUTTON1 (GPIO32) — BUZZER CONTROL FLOW

```
User presses Button1 (GPIO32)
         ↓
GPIO pulls LOW (active-low)
         ↓
ISR: button1_isr_handler() [IRAM_ATTR]
  ├─ 50ms debounce check
  ├─ Set g_btn1_isr_flag = 1
  └─ xSemaphoreGiveFromISR(xButton1Semaphore)
         ↓
MonitoringTask checks: if (btn1) [every 100ms]
  ├─ Read: uint8_t btn1 = hal_read_button1()  [GPIO32 ≡ 0 → btn1 = 1]
  ├─ Create ActuatorMessage:
  │    .cmd = ACT_CMD_BUZZER_ON
  │    .duration_ms = 500
  │    .trigger = "btn1_buzzer"
  └─ xQueueSend(xActuatorQueue, &msg, 0)
         ↓
ActuatorTask processes command:
  ├─ case ACT_CMD_BUZZER_ON:
  ├─ _set_buzzer(1)  [GPIO_BUZZER ← 1 (HIGH)]
  ├─ vTaskDelay(500ms)  [Buzzer sounds for 500ms]
  └─ _set_buzzer(0)  [GPIO_BUZZER ← 0 (LOW)]
         ↓
Result: 500ms buzzer pulse
Serial Log: "Button1: Buzzer ON (500ms pulse)"
```

---

## BUTTON2 (GPIO19) — FAN TOGGLE CONTROL FLOW

```
User presses Button2 (GPIO19)
         ↓
GPIO pulls LOW (active-low)
         ↓
ISR: button2_isr_handler() [IRAM_ATTR]
  ├─ 50ms debounce check
  ├─ Set g_btn2_isr_flag = 1
  └─ xSemaphoreGiveFromISR(xButton2Semaphore)
         ↓
MonitoringTask checks: if (btn2) [every 100ms]
  ├─ Read: uint8_t btn2 = hal_read_button2()  [GPIO19 ≡ 0 → btn2 = 1]
  ├─ Read current fan state (with mutex):
  │  uint8_t fan_now = g_actuator.fan
  │
  ├─ IF fan_now = 1 (FAN ALREADY ON):
  │  │  actuator_post(ACT_CMD_FAN_OFF, 0, "btn2_toggle")
  │  │  Log: "Button2: Fan OFF (manual)"
  │  │
  │  └─ ActuatorTask:
  │     ├─ case ACT_CMD_FAN_OFF:
  │     ├─ _set_fan(0)  [GPIO_FAN_RELAY ← 1 (HIGH=OFF)]
  │     └─ Clear: g_actuator.fan_manual = 0
  │
  └─ ELSE if fan_now = 0 (FAN OFF):
     │  Create ActuatorMessage:
     │    .cmd = ACT_CMD_FAN_ON
     │    .duration_ms = 0 (no auto-timeout)
     │  Set flags:
     │    g_actuator.fan_manual = 1
     │    g_actuator.fan_manual_since = get_timestamp_ms()
     │  xQueueSend(xActuatorQueue, &msg, 0)
     │  Log: "Button2: Fan ON (manual, 30s WDT)"
     │
     └─ ActuatorTask:
        ├─ case ACT_CMD_FAN_ON:
        ├─ _set_fan(1)  [GPIO_FAN_RELAY ← 0 (LOW=ON)]
        ├─ g_actuator.fan = 1 (logical state)
        ├─ Wait for manual timeout...
        │
        └─ Every 1 second check:
           if (g_actuator.fan_manual && elapsed >= 30 seconds):
             ├─ _set_fan(0)  [GPIO_FAN_RELAY ← 1 (HIGH=OFF)]
             ├─ g_actuator.fan = 0
             ├─ g_actuator.fan_manual = 0
             └─ Log: "Fan manual override timeout — auto-release"
         ↓
Result: Fan toggles on, runs for 30 seconds, auto-stops
        (or stops immediately if pressed again)
```

---

## DOOR SENSOR (GPIO5) — OPEN/CLOSE DETECTION FLOW

```
Physical door state changes
         ↓
Door opens: GPIO5 transitions LOW → HIGH
         ↓
ISR: door_isr_handler() [IRAM_ATTR]
  ├─ 50ms debounce check
  ├─ Set g_door_isr_flag = 1
  └─ xSemaphoreGiveFromISR(xDoorSemaphore)
         ↓
SecurityTask [every 200ms]:
  ├─ if (xSemaphoreTake(xDoorSemaphore, 0)):
  │  │
  │  ├─ Read: uint8_t door = hal_read_door()
  │  │
  │  ├─ IF door = 1 (OPEN) and last_door = 0 (was CLOSED):
  │  │  │  Log: "DOOR OPENED!"
  │  │  │  send_event(EVENT_TYPE_DOOR_OPEN, ...)
  │  │  │  broker_publish(TOPIC_SECURITY, "alert:door_open")
  │  │  │  Buzzer: 500ms pulse triggered
  │  │  │  door_stable_since = 0 (reset timer)
  │  │  └─ last_door = 1
  │  │
  │  └─ ELSE IF door = 0 (CLOSED) and last_door = 1 (was OPEN):
  │     │  Log: "Door closing..."
  │     │  door_stable_since = get_timestamp_ms()  [Start 50ms timer]
  │     └─ last_door = 0
  │
  └─ Parallel: Confirm stable close (FIX-07):
     if (last_door = 1):  [Still marked as open]
       ├─ Read live: uint8_t live = hal_read_door()
       │
       ├─ IF live = 0 (GPIO shows CLOSED):
       │  │
       │  ├─ IF door_stable_since = 0:
       │  │  │  door_stable_since = get_timestamp_ms()
       │  │  └─ Start timing...
       │  │
       │  └─ ELSE IF elapsed > 50ms:
       │     │  last_door = 0  [Mark as truly closed]
       │     │  Log: "Door CLOSED (confirmed)"
       │     └─ last_door = 0
       │
       └─ ELSE (live = 1, still open):
          │  Reset timer: door_stable_since = 0
          └─ Stay in "door open" state

         ↓
Result: Door OPEN → immediate alert
        Door CLOSE → wait 50ms for stable LOW then confirm
        This prevents false "closed" triggers from contact bounce
```

---

## BUZZER (GPIO18) — TRIGGER SOURCES & DURATIONS

```
Trigger Source          │ Duration │ Control Path
─────────────────────────────────────────────────────────────
Button1 Press           │  500ms   │ MonitoringTask → ActuatorQueue
Door Open               │  500ms   │ SecurityTask → EmergencyTask
PIR (Unauthorized Entry)│ 5000ms   │ SecurityTask → EmergencyTask
Fire Alarm (Temp>85°C)  │10000ms   │ SafetyTask → EmergencyTask
Gas Leak (MQ2>alarm)    │10000ms   │ SafetyTask → EmergencyTask
Noise Alarm (>70dB)     │ 3000ms   │ MonitoringTask → EmergencyTask
Vibration Alert         │ 2000ms   │ ScalabilityTask → EmergencyTask
─────────────────────────────────────────────────────────────

ActuatorTask processes all via:
  case ACT_CMD_BUZZER_ON:
    _set_buzzer(1)         [GPIO18 ← 1 (HIGH)]
    vTaskDelay(duration_ms)
    _set_buzzer(0)         [GPIO18 ← 0 (LOW)]
    
Emergency Task can override/extend pulses via ACT_CMD_ALL_CLEAR
```

---

## FAN RELAY (GPIO25) — TRIGGER SOURCES & DURATIONS

```
Trigger Source              │ Duration │ Control Path
─────────────────────────────────────────────────────────────
Button2 Manual Toggle       │ 30000ms  │ MonitoringTask (manual WDT)
                            │ (auto)   │
Fire Alarm (Temp>85°C)      │10000ms   │ SafetyTask → EmergencyTask
Gas Leak (MQ2>alarm)        │10000ms   │ SafetyTask → EmergencyTask
Overheat (Temp>75°C)        │ 5000ms   │ SafetyTask → EmergencyTask
─────────────────────────────────────────────────────────────

ACTIVE-LOW relay logic:
  _set_fan(1):  GPIO_FAN_RELAY ← 0 (LOW)  → Relay CLOSES → Fan ON
  _set_fan(0):  GPIO_FAN_RELAY ← 1 (HIGH) → Relay OPENS → Fan OFF

ActuatorTask process:
  case ACT_CMD_FAN_ON:
    _set_fan(1)              [GPIO25 ← 0 (LOW)]
    if (duration_ms > 0):
      vTaskDelay(duration_ms)
      if (!g_actuator.fan_manual):
        _set_fan(0)          [GPIO25 ← 1 (HIGH)]
    
  case ACT_CMD_FAN_OFF:
    _set_fan(0)              [GPIO25 ← 1 (HIGH)]
    Clear manual flags

Manual Override Auto-Release (FIX-06):
  if (g_actuator.fan_manual):
    elapsed = (now - fan_manual_since) / 1000
    if (elapsed >= 30):      [30 second timeout]
      _set_fan(0)
      g_actuator.fan_manual = 0
```

---

## INITIALIZATION SEQUENCE

```
app_main()
  │
  ├─ [1/6] Create RTOS objects:
  │   ├─ xButton1Semaphore = xSemaphoreCreateBinary()
  │   ├─ xButton2Semaphore = xSemaphoreCreateBinary()
  │   ├─ xDoorSemaphore = xSemaphoreCreateBinary()
  │   ├─ xVibrationSemaphore = xSemaphoreCreateBinary()
  │   ├─ xActuatorMutex = xSemaphoreCreateMutex()
  │   └─ [... other queues, mutexes, event groups ...]
  │
  ├─ [2/6] GPIO initialization: hal_gpio_init()
  │   ├─ Input pins: PIR, Door, Vibration, Button1, Button2
  │   │   .intr_type = GPIO_INTR_NEGEDGE or GPIO_INTR_POSEDGE
  │   │   .pull_up_en / pull_down_en configured
  │   ├─ Output pins: Buzzer, Fan, LEDs (all initialized OFF)
  │   │   gpio_set_level(GPIO_BUZZER, 0)
  │   │   gpio_set_level(GPIO_FAN_RELAY, 1)  [ACTIVE-LOW!]
  │   └─ ISR handlers registered:
  │       gpio_isr_handler_add(GPIO_BUTTON1, button1_isr_handler, NULL)
  │       gpio_isr_handler_add(GPIO_BUTTON2, button2_isr_handler, NULL)
  │       gpio_isr_handler_add(GPIO_DOOR_SENSOR, door_isr_handler, NULL)
  │       [... others ...]
  │
  ├─ [3/6] ADC initialization: hal_adc_init()
  │
  ├─ [4/6] RTC/I2C initialization: hal_rtc_init()
  │
  ├─ [5/6] MQTT broker: broker_init()
  │
  └─ [6/6] Task creation:
      ├─ xTaskCreate(vSensorTask, ...)      [reads ADCs, RTC]
      ├─ xTaskCreate(vSafetyTask, ...)      [temp/gas thresholds]
      ├─ xTaskCreate(vSecurityTask, ...)    [PIR/Door events]
      ├─ xTaskCreate(vMonitoringTask, ...)  [noise, buttons, feedback]
      ├─ xTaskCreate(vScalabilityTask, ...) [vibration debounce]
      ├─ xTaskCreate(vActuatorTask, ...)    [relay/buzzer/LED control]
      ├─ xTaskCreate(vEmergencyTask, ...)   [emergency orchestration]
      ├─ xTaskCreate(vCommTask, ...)        [MQTT publishing]
      ├─ xTaskCreate(vPerformanceTask, ...) [heap/uptime/CPU]
      └─ [... others ...]
         ↓
      All tasks ready, event loop begins
```

---

## TASK PRIORITIES

| Task | Priority | Frequency | Purpose |
|------|----------|-----------|---------|
| ISR | 31+ | Immediate | GPIO interrupt handlers |
| ActuatorTask | 24 | 1s check | Relay/Buzzer/LED control |
| SensorTask | 23 | 500ms | ADC/RTC reads |
| SafetyTask | 22 | 500ms | Threshold alerts |
| SecurityTask | 21 | 200ms | PIR/Door/Vibration |
| EmergencyTask | 20 | On-demand | Emergency orchestration |
| CommTask | 19 | 2s | MQTT publishing |
| MonitoringTask | 18 | 100ms | Noise/Button feedback |
| ScalabilityTask | 17 | 500ms | Vibration debounce |
| PerformanceTask | 16 | 5s | Metrics |
| OledTask | 15 | 3s | Display updates |

---

## SAFETY INTERLOCKS

```
Manual Button2 Override:
  ├─ Sets: g_actuator.fan_manual = 1
  ├─ Timestamp: g_actuator.fan_manual_since = get_timestamp_ms()
  ├─ ActuatorTask monitors every 1 second
  └─ Auto-release after 30 seconds

Buzzer During Fan Transition:
  ├─ Uses separate GPIO pins (18 vs 25)
  ├─ No electrical interference
  └─ Can sound simultaneously

Mutex Protection:
  ├─ xActuatorMutex protects g_actuator state
  ├─ All reads/writes guarded by xSemaphoreTake/Give
  ├─ Prevents torn state during context switch
  └─ MonitoringTask uses 10ms timeout for quick response

ISR Safety:
  ├─ All ISRs in IRAM_ATTR (interrupt RAM)
  ├─ 50ms debounce in hardware (not software delay)
  ├─ Semaphore NULL check (FIX-26)
  └─ Signals task via semaphore, not direct GPIO manipulation

Door Stable Confirmation:
  ├─ Requires 50ms of stable LOW state (FIX-07)
  ├─ Prevents false "closed" from contact bounce
  └─ Only SecurityTask can clear door_open flag
```

---


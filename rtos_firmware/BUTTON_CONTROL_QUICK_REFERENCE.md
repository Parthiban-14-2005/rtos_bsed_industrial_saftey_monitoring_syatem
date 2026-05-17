# Button Control Quick Reference

## Two Push Buttons Configured

### Button1 (GPIO32) - Buzzer Manual Trigger
- **Hardware**: Push button with internal pull-up
- **Action**: Press → 500ms buzzer pulse (GPIO18)
- **Control Method**: Queue-based command to ActuatorTask
- **Web Status**: `"button1": 1`, `"buzzer": 1`
- **Console Log**: `Button1: Buzzer ON (500ms pulse)`

### Button2 (GPIO19) - Fan Toggle
- **Hardware**: Push button with internal pull-up
- **Action**: Press → Toggle fan relay ON/OFF
- **Auto-timeout**: 30 seconds if left on manually
- **Control Method**: Queue-based command to ActuatorTask
- **Web Status**: `"button2": 1`, `"fan": 1`
- **Console Log**: `Button2: Fan ON/OFF (manual, 30s WDT)`

---

## Function Flow

```
Physical Button Press
         ↓
Button ISR (50ms debounce)
         ↓
Set g_btn1/btn2_isr_flag
         ↓
Signal Semaphore
         ↓
vMonitoringTask receives signal
         ↓
Queue ActuatorMessage (cmd, duration, trigger)
         ↓
vActuatorTask processes command
         ↓
gpio_set_level(GPIO_PIN, HIGH/LOW)
         ↓
Relay activates (through NPN transistor)
         ↓
Actuator state updated: g_actuator.buzzer/fan = 1/0
         ↓
SensorTask syncs state to g_sensor_data
         ↓
Web server reads g_sensor_data
         ↓
JSON /data endpoint returns button & actuator states
         ↓
Web Dashboard receives and displays in real-time
```

---

## Relay Control Method Confirmed

**Method**: ESP-IDF `gpio_set_level(pin, level)`

| Aspect | Details |
|--------|---------|
| Pin Setup | `GPIO_MODE_OUTPUT`, mode OUTPUT push-pull |
| Control | Direct HIGH (1) / LOW (0) writes |
| Safety | Protected by `xActuatorMutex` |
| Hardware | NPN transistor driver (GPIO can't drive relay directly) |
| Function | `_set_buzzer(uint8_t s)` → `gpio_set_level(GPIO_BUZZER, s)` |
| | `_set_fan(uint8_t s)` → `gpio_set_level(GPIO_FAN_RELAY, s)` |

---

## Web Dashboard JSON Fields

### New Fields Added
```json
{
  "button1": 0,    ← NEW: Button1 press state (0/1)
  "button2": 0,    ← NEW: Button2 press state (0/1)
  "buzzer": 0,     ← Buzzer actuator state (0=OFF, 1=ON)
  "fan": 0,        ← Fan relay state (0=OFF, 1=ON)
  "rtc_time": "HH:MM:SS",
  "rtc_date": "DD/MM/YYYY",
  "temp": 28.5,
  "gas": 120,
  "noise": 400,
  "pir": 0,
  "door": 0,
  "vibration": 0,
  "cpu": 15.3,
  "state": "normal"
}
```

---

## Testing

### Button1 Test
1. Press physical Button1 (GPIO32)
2. ✓ Buzzer sounds for 500ms
3. ✓ Console shows: `Button1: Buzzer ON (500ms pulse)`
4. ✓ Web dashboard shows: `"button1": 1, "buzzer": 1`
5. ✓ After 500ms: `"buzzer": 0`

### Button2 Test
1. Press physical Button2 (GPIO19)
2. ✓ Fan relay activates
3. ✓ Console shows: `Button2: Fan ON (manual, 30s WDT)`
4. ✓ Web dashboard shows: `"button2": 1, "fan": 1`
5. Press Button2 again → Fan stops
6. ✓ Console shows: `Button2: Fan OFF (manual)`
7. ✓ Web dashboard shows: `"fan": 0`
8. If no press for 30s → Fan auto-stops
9. ✓ Console shows: `Fan manual override expired (30s)`

---

## Key Files

- **Main Code**: `main/ISMS_MQTT_v8_0_RTOS_PERF.c` (v10.3.2)
  - Button ISRs: ~line 682
  - MonitoringTask: ~line 1982 (Button1 control added)
  - ActuatorTask: ~line 1866
  - GPIO setup: ~line 720

- **Web Server**: `main/isms_web_server.c` (v3.1.0)
  - JSON builder: Updated with button fields

- **Header**: `main/isms_web_server.h` (v1.1.0)
  - isms_sensor_data_t struct: Added button1, button2 fields

---

## Console Commands to Monitor

```bash
# Watch console for button events
idf.py monitor | grep -i button

# Or specific actions
idf.py monitor | grep -E "Button1|Button2|Buzzer|Fan"
```

---

## Summary

✅ Button1 (GPIO32) triggers 500ms buzzer pulse on press
✅ Button2 (GPIO19) toggles fan relay ON/OFF with 30s timeout
✅ Both buttons show real-time state in web dashboard JSON
✅ Relay control uses gpio_set_level() with mutex protection
✅ 50ms hardware debounce on button ISRs
✅ Thread-safe queue-based command passing to ActuatorTask
✅ OLED display shows button states on monitoring page


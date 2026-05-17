# Hardware Testing & Troubleshooting Guide
## Relay, Buzzer, Door, Button Functions

---

## QUICK TEST PROCEDURES

### Test 1: Button1 (GPIO32) — Manual Buzzer Trigger
**Expected Behavior**: Press button → 500ms buzzer sound  
**How to Test**:
1. Build and flash firmware
2. Connect Button1 to GPIO32 with pull-up
3. Connect Buzzer to GPIO18
4. Press Button1 → Should hear 500ms beep
5. Check serial log: `Button1: Buzzer ON (500ms pulse)`

**If Not Working**:
- [ ] Check GPIO32 is connected and pulls to GND when pressed
- [ ] Check GPIO18 buzzer is powered and connected
- [ ] Check pull-up resistor (10kΩ typical)
- [ ] Verify ISR firing: Watch for semaphore messages in ESP_LOG

---

### Test 2: Button2 (GPIO19) — Manual Fan Toggle
**Expected Behavior**: Press → fan turns ON (30s timeout), press again → fan OFF  
**How to Test**:
1. Build and flash firmware
2. Connect Button2 to GPIO19 with pull-up
3. Connect Fan Relay to GPIO25 with NPN transistor
4. Press Button2 → Relay should activate (fan motor or relay click)
5. Wait 30 seconds → Relay should auto-deactivate
6. Check serial log: `Button2: Fan ON (manual, 30s WDT)`

**If Not Working**:
- [ ] Check GPIO19 is connected and pulls to GND when pressed
- [ ] Check GPIO25 relay output with multimeter (HIGH=OFF, LOW=ON)
- [ ] Verify relay NPN transistor base is connected to GPIO25
- [ ] Check 30-second timeout: `elapsed >= FAN_MANUAL_TIMEOUT_S (30)`

---

### Test 3: Door Sensor (GPIO5)
**Expected Behavior**: Open → immediate alert, Close → 50ms stable confirmation  
**How to Test**:
1. Build and flash firmware
2. Connect Door sensor/reed switch to GPIO5
3. Open door → Should trigger `EVENT_TYPE_DOOR_OPEN`
4. Check serial log: `DOOR OPENED!` + `{alert:door_open}`
5. Close door → System should wait 50ms for stable LOW
6. Check log: `Door CLOSED (confirmed)`

**If Not Working**:
- [ ] Check GPIO5 sensor connection (should read HIGH when open)
- [ ] Verify pull-down resistor is present (prevents float)
- [ ] Check reed switch is NC (Normally Closed)
- [ ] Verify door stable timer: 50ms in SecurityTask

**Door Logic Verification**:
```
GPIO Level  | Door State
    0       | CLOSED
    1       | OPEN
```

---

### Test 4: Buzzer (GPIO18)
**Expected Behavior**: Fires on security/emergency events, pulse duration varies by trigger  
**How to Test**:
1. Trigger security event (e.g., open door)
   - Door open: 500ms pulse
   - Unauthorized entry (PIR): 5 sec pulse
   - Fire alarm: 10 sec continuous
   - Gas leak: 10 sec continuous

2. Check durations match expected values
3. Verify buzzer can be hardware-overridden (no PWM)

**If Always On/Off**:
- [ ] Check initialization: `gpio_set_level(GPIO_BUZZER, 0)` at boot
- [ ] Verify GPIO18 is configured as OUTPUT
- [ ] Check transistor base is properly connected
- [ ] Monitor: `SLOG_I(TAG_MONITOR, "Button1: Buzzer ON (500ms pulse)")`

---

### Test 5: Fan Relay (GPIO25)
**Expected Behavior**: Emergency events (10s), Manual override (30s), Timed durations  
**How to Test**:
1. Trigger fire/gas alarm (should run 10 seconds)
2. Manually toggle with Button2 (should run 30 seconds)
3. Check manual timeout occurs exactly at 30 seconds
4. Verify relay clicks on/off at state changes

**if Never Activates**:
- [ ] Check GPIO25 output level with multimeter
- [ ] Verify relay ACTIVE-LOW logic: HIGH=OFF, LOW=ON
- [ ] Check NPN transistor connections
- [ ] Monitor: `_set_fan(0)` / `_set_fan(1)` execution

**Relay Logic Verification**:
```
GPIO Level  | Relay State  | Motor
    1       | OFF (open)   | Stopped
    0       | ON (closed)  | Running
```

---

## SERIAL LOG MARKERS TO WATCH

### Button Press Events
```
Button1 PRESSED — Buzzer trigger
Button1: Buzzer ON (500ms pulse)
Button2 PRESSED — Fan toggle
Button2: Fan ON (manual, 30s WDT)
Button2: Fan OFF (manual)
```

### Door Events
```
DOOR OPENED!
Door CLOSED (confirmed)
```

### Fan Timeout
```
Fan manual override timeout (elapsed: 30 s) — auto-release
```

### Buzzer Events
```
Security alert: buzzer 5s (unauthorized_entry)
Buzzer ON (500ms pulse)
Buzzer OFF
```

---

## DEBUGGING WITH MULTIMETER

### Button1 (GPIO32)
```
State       | Voltage
Not Pressed |  3.3V (pulled up)
Pressed     |  0V   (pulled to GND)
```

### Button2 (GPIO19)
```
State       | Voltage
Not Pressed |  3.3V (pulled up)
Pressed     |  0V   (pulled to GND)
```

### Buzzer Output (GPIO18)
```
State       | Voltage
OFF         |  0V (LOW)
ON          |  3.3V or 5V (depending on supply)
```

### Fan Relay Output (GPIO25)
```
State       | Voltage  | Relay
OFF         |  3.3V    | Open (no current)
ON          |  0V      | Closed (current flows)
```

### Door Sensor (GPIO5) — Active-HIGH
```
State       | Voltage
Closed      |  0V or ~0.2V (pulled down)
Open        |  3.3V (high when triggered)
```

---

## COMMON ISSUES & FIXES

### Issue: Button Press Not Detected
**Symptom**: Button1/Button2 not triggering any action  
**Root Causes**:
1. Pull-up resistor missing or wrong value (should be 10kΩ)
2. Button wired backwards (should pull to GND when pressed)
3. GPIO configured as input but not in ISR setup
4. Semaphore not created before hal_gpio_init()

**Fix**:
```
In hal_gpio_init():
- Verify GPIO_PULLUP_ENABLE for button pins
- Verify GPIO_INTR_NEGEDGE (falling edge)
- Confirm gpio_isr_handler_add() is called
- Check: if (!xButtonXSemaphore) return; in ISR
```

---

### Issue: Buzzer Always On or Always Off
**Symptom**: Buzzer stuck in one state regardless of events  
**Root Causes**:
1. GPIO18 not configured as OUTPUT
2. Transistor base not connected to GPIO18
3. Startup fails before gpio_set_level(GPIO_BUZZER, 0)
4. _set_buzzer() function not being called

**Fix**:
```c
Check hal_gpio_init():
gpio_set_level(GPIO_BUZZER, 0);  // Must initialize to OFF

Check _set_buzzer():
static void _set_buzzer(uint8_t s)  
{ 
    gpio_set_level(GPIO_BUZZER, s); 
}
```

---

### Issue: Fan Relay Not Responding
**Symptom**: Fan doesn't turn on or stays stuck in one state  
**Root Causes**:
1. ACTIVE-LOW logic inverted in code or hardware
2. GPIO25 not configured as OUTPUT
3. NPN transistor not conducting
4. Manual override flag never cleared

**Fix**:
```c
Verify _set_fan():
static void _set_fan(uint8_t fan_on)
{
    uint8_t gpio_level = fan_on ? 0 : 1;  /* ACTIVE-LOW: invert */
    gpio_set_level(GPIO_FAN_RELAY, gpio_level);
}

Check initialization:
gpio_set_level(GPIO_FAN_RELAY, 1);  // Start OFF (HIGH)
```

---

### Issue: Door Event Triggers Constantly
**Symptom**: Door open/close events firing repeatedly  
**Root Causes**:
1. Pull-down resistor missing (GPIO floating)
2. Debounce timeout too short (< 50ms)
3. Reed switch contact bounce not damped
4. Stable close confirmation not working

**Fix**:
```c
In hal_gpio_init():
.pull_down_en = GPIO_PULLDOWN_ENABLE,  /* Must be enabled */
.intr_type = GPIO_INTR_POSEDGE         /* Rising edge */

In door_isr_handler():
if ((now - last_ms) < 50) return;      /* 50ms debounce */

In SecurityTask() @ line 1720:
if ((get_timestamp_ms() - door_stable_since) > 50) { ... }
```

---

### Issue: Button2 Manual Timeout Not Working
**Symptom**: Fan stays on longer than 30 seconds  
**Root Causes**:
1. FAN_MANUAL_TIMEOUT_S not set to 30
2. fan_manual_since timestamp not updated
3. ActuatorTask timeout check not running
4. Time calculation overflow

**Fix**:
```c
Check FAN_MANUAL_TIMEOUT_S = 30  (in #define)

Check fan_manual tracking:
g_actuator.fan_manual = 1;
g_actuator.fan_manual_since = get_timestamp_ms();

In ActuatorTask() @ line 1975:
uint32_t elapsed = (get_timestamp_ms() - g_actuator.fan_manual_since) / 1000;
if (elapsed >= FAN_MANUAL_TIMEOUT_S) { /* 30 seconds */ ... }
```

---

## TESTING MATRIX

| Component | Test | Expected | Status |
|-----------|------|----------|--------|
| Button1   | Press | Buzzer 500ms | ☐ |
| Button2   | Press | Fan ON + timeout @ 30s | ☐ |
| Door      | Open  | Immediate alert | ☐ |
| Door      | Close | Wait 50ms then confirm | ☐ |
| Buzzer    | Fire alarm | 10 sec pulse | ☐ |
| Buzzer    | Gas alert | 10 sec pulse | ☐ |
| Fan       | Fire alert | 10 sec pulse | ☐ |
| Fan       | Manual override | 30 sec timeout | ☐ |

---

## HARDWARE WIRING CHECKLIST

### Pull-Ups/Pull-Downs Required
- [ ] Button1 (GPIO32): 10kΩ pull-up to 3.3V
- [ ] Button2 (GPIO19): 10kΩ pull-up to 3.3V
- [ ] Door Sensor (GPIO5): Pull-down enabled in GPIO config
- [ ] Vibration (GPIO27): Pull-up enabled in GPIO config
- [ ] PIR Sensor (GPIO4): Pull-down enabled in GPIO config

### Output Connections
- [ ] Buzzer (GPIO18): Via NPN transistor base resistor (1kΩ)
- [ ] Fan Relay (GPIO25): Via NPN transistor base resistor (1kΩ)
- [ ] All GND connections solid and short

### Debouncing
- [ ] 50ms in ISR handler (FIX-01)
- [ ] Hardware RC filter optional but not required

---


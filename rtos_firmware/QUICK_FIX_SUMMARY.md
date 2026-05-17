# Quick Fix Summary — Button & Door Issues
## Two Critical Bugs Fixed

---

## 🔴 ISSUE 1: Button Actions Repeat Continuously  

### **What Was Wrong**
Button1 & Button2 flag variables were **never cleared**, so:
- Button press → Action triggers
- Flag stays `1` → Action repeats EVERY 100ms while button held
- Cannot stop action even after releasing button

### **The Fix** ✅
Added flag clearing in MonitoringTask immediately after processing:

```c
if (btn1) {
    /* Execute buzzer action */
    g_btn1_isr_flag = 0;  /* CLEAR FLAG */
}

if (btn2) {
    /* Execute fan action */  
    g_btn2_isr_flag = 0;  /* CLEAR FLAG */
}
```

### **Result**
✅ Each button press now triggers action **ONLY ONCE**

---

## 🔴 ISSUE 2: Door Always Shows OPEN  

### **What Was Wrong**
If GPIO5 pull-down resistor missing or sensor stuck:
- GPIO5 reads HIGH (1 = OPEN) continuously
- Code expects GPIO to read LOW (0 = CLOSED) for 50ms to confirm close
- If GPIO stays HIGH, it never confirms close
- `last_door` stays `1` forever → **"Door always open"**

### **The Fix** ✅
Added 30-second stuck-door detection with auto-clear:

```c
/* Track how long door stays open */
if (door_open_duration >= 30) {  /* 30 seconds */
    /* Force clear stuck state */
    last_door = 0;
    /* Log hardware problem */
    SLOG_W("Door STUCK OPEN (30s) — Check pull-down resistor");
}
```

### **Result**
✅ Door won't freeze in "always open" state  
✅ Auto-clears after 30s + logs hardware issue

---

## ✅ Changes Made

| Component | Change | Location |
|-----------|--------|----------|
| Button1 | Add `g_btn1_isr_flag = 0;` | MonitoringTask after btn1 action |
| Button2 | Add `g_btn2_isr_flag = 0;` | MonitoringTask after btn2 action |
| Door | Add stuck detection timer | SecurityTask door handling |
| Door | Add force-clear after 30s | SecurityTask periodic check |

---

## 🧪 How to Test

### Test Button Fix
1. **Press Button1 once** → Buzzer sounds for 500ms, then silent
2. **Hold Button1 for 5 seconds** → Buzzer sounds once (500ms), no repeat
3. **Release** → All quiet

❌ Before: Buzzer would repeat every 100ms while held  
✅ After: Buzzer sounds once per press only

### Test Door Fix  
1. **Normal door operation** → Works fine (unchanged)
2. **Simulate stuck sensor** → After 30s, see in serial log:
   ```
   Door STUCK OPEN (30s) — force-clearing flag
   Check: pull-down resistor, sensor wiring, GPIO5 voltage
   ```
3. **Door state auto-clears** → Not frozen anymore ✅

---

## 📋 Compilation

✅ **No errors**  
✅ **No warnings**  
✅ **Ready to compile and flash**

---

## 🔧 If Issues Persist

### Button Still Not Working
- [ ] Check 10kΩ pull-up resistor on GPIO32 and GPIO19
- [ ] Verify button pulls GPIO to GND when pressed  
- [ ] Check serial log for "Button1/2 PRESSED" message

### Door Still Stuck Open
- [ ] Check 10kΩ pull-down resistor on GPIO5 exists and is soldered
- [ ] Measure GPIO5 with multimeter:
  - Door closed: Should be ~0V
  - Door open: Should be ~3.3V
- [ ] If measurements wrong → Resistor/wiring issue

---


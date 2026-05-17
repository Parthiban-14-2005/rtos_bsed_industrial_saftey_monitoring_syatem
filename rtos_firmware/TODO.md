## TODO

### Step 1: Add SPIFFS dependency to main component
- [x] Edit `main/CMakeLists.txt` to add `spiffs` to `PRIV_REQUIRES`.

### Step 2: Fix RTC string build failures in `vSensorTask`
- [x] Edit `main/ISMS_MQTT_v8_0_RTOS_PERF.c` to prevent trigraph parsing in the "??/..." RTC strings (also addressing rtc_date/truncation warnings).



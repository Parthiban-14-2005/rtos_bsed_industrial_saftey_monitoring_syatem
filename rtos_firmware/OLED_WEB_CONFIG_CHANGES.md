# OLED + Web Dashboard Integration - Configuration Changes

## Problem
- OLED display was stuck on "Waiting for Web Dashboard" page
- Web server wasn't tracking connected clients
- `g_ws_client_count` was never being updated, so OLED task couldn't detect web connections
- OLED couldn't transition to showing live sensor pages

## Solution
Modified the web server to track HTTP clients and automatically update `g_ws_client_count` so the OLED display can detect web dashboard connections.

## Changes Made

### 1. **isms_web_server.c** - Added Client Tracking (v3.1.0)

#### New Features:
- **Client Tracking Structure**: Each connected client is tracked by IP address and last request timestamp
- **Automatic Connection Detection**: When a browser requests `/data`, it's registered as a connected client
- **Automatic Disconnection Detection**: A background cleanup task removes clients that haven't made requests in 10 seconds
- **g_ws_client_count Export**: Counter is updated and exported for OLED task to monitor

#### Key Additions:
```c
/* Client tracking */
#define MAX_TRACKED_CLIENTS 4
#define CLIENT_TIMEOUT_MS 10000

typedef struct {
    uint32_t ip_addr;
    uint32_t last_request_ms;
} HttpClient_t;

/* Exported counter for OLED task */
volatile uint8_t g_ws_client_count = 0;
```

#### New Functions:
1. **`client_tracking_update()`** - Called from `/data` handler to register/update client activity
2. **`vClientCleanupTask()`** - Background task that removes stale clients every 2 seconds

#### Updated Functions:
1. **`http_get_data()`** - Now calls `client_tracking_update()` to track each request
2. **`isms_web_server_init()`** - Now creates mutex and starts cleanup task

---

### 2. **isms_web_server.h** - Exported g_ws_client_count

Added public declaration:
```c
extern volatile uint8_t g_ws_client_count;
```

This allows `vOledTask` to access the counter directly.

---

### 3. **ISMS_MQTT_v8_0_RTOS_PERF.c** - Removed Duplicate Declaration

Removed duplicate `g_ws_client_count` definition (now defined in web server, not main file):
```c
/* MOVED: g_ws_client_count is now defined in isms_web_server.c */
```

---

## How It Works (Flow Diagram)

```
┌─────────────────────────────────────────────────────────┐
│ Web Browser Opens http://192.168.4.1                    │
└─────────┬───────────────────────────────────────────────┘
          │
          ▼
┌─────────────────────────────────────────────────────────┐
│ Browser starts polling GET /data every 2 seconds        │
└─────────┬───────────────────────────────────────────────┘
          │
          ▼
┌─────────────────────────────────────────────────────────┐
│ http_get_data() handler:                                │
│  1. Extract client IP from request                      │
│  2. Call client_tracking_update()                       │
│  3. If new IP → increment g_ws_client_count              │
│  4. Send JSON response                                  │
└─────────┬───────────────────────────────────────────────┘
          │
          ▼
┌─────────────────────────────────────────────────────────┐
│ vOledTask detects g_ws_client_count > 0:                │
│  1. Transitions from WAIT_APP → CONNECTED               │
│  2. Shows "SYSTEM CONNECTED" banner for 2 s             │
│  3. Enters LIVE state → rotating sensor pages           │
└─────────────────────────────────────────────────────────┘

If Browser Disconnects (no /data request for 10s):
┌─────────────────────────────────────────────────────────┐
│ vClientCleanupTask detects timeout:                     │
│  1. Removes stale client from tracking                  │
│  2. Decrements g_ws_client_count                         │
│  3. If count == 0, vOledTask returns to WAIT_APP        │
└─────────────────────────────────────────────────────────┘
```

---

## Display Behavior After Changes

### **Startup Sequence:**
1. **Boot** (3 s) → "RTOS Industrial Safety System"
2. **Ready** (2 s) → "System Ready"  
3. **Wait** → "Connect Web Dashboard / HTTP 192.168.4.1"
   - *(OLED waits here until web client connects)*
4. **Connected** (2 s) → "SYSTEM CONNECTED" ✓
5. **Live** (continuous) → Rotating sensor pages:
   - Temperature & RTC
   - Gas Level (MQ-2)
   - Noise Level
   - Security (PIR + Door)
   - Monitoring (Vibration + Buttons)
   - System State

### **When Web Client Disconnects:**
- OLED automatically returns to "Connect Web Dashboard" wait screen after 10 seconds of inactivity
- If client reconnects → "SYSTEM CONNECTED" banner again + Live pages resume

---

## Technical Notes

- **Thread Safety**: Client tracking uses `xSemaphoreTake/Give` mutex for thread-safe access
- **Timeout Mechanism**: Clients are considered disconnected if no HTTP request for 10 seconds
- **Maximum Clients**: Tracks up to 4 simultaneous clients (configurable via `MAX_TRACKED_CLIENTS`)
- **Log Messages**: Console logs when clients connect/disconnect for debugging
- **Backward Compatible**: No changes to HTTP endpoints or JSON format — existing web dashboard works unchanged

---

## Next Steps

1. **Build** the project: `idf.py build`
2. **Flash** to ESP32: `idf.py flash`
3. **Test** the flow:
   - Power on ESP32 → OLED shows boot sequence
   - Open web browser → `http://192.168.4.1`
   - OLED transitions to "SYSTEM CONNECTED" + live pages
   - Close browser → OLED returns to "Connect Web Dashboard" after 10 s

---

## Troubleshooting

- **OLED still stuck on "Connect Web Dashboard"?**
  - Check browser is actually polling `/data` endpoint (check network tab)
  - Check ESP32 logs for "Client connected" messages
  - Verify `http://192.168.4.1/data` returns valid JSON with sensor values

- **OLED shows data but doesn't rotate pages?**
  - Check `OLED_PAGE_PERIOD_MS` value (default 3000 = 3 seconds)
  - Verify `OLED_LIVE_PAGES` count matches actual page functions

---

## Version Info

- **Web Server**: v3.1.0 (added client tracking)
- **Main Firmware**: v10.3.2 (OLED + FAN FIX)
- **Target**: ESP32 + ESP-IDF v5.3.1 + FreeRTOS

# 🏭 RTOS-Driven Industrial Security and Performance Monitoring System

> An intelligent real-time industrial monitoring and security system built with ESP32, FreeRTOS, IoT (MQTT), and Machine Learning for anomaly detection and predictive maintenance.

---

## 📌 Table of Contents

- [Overview](#overview)
- [Features](#features)
- [System Architecture](#system-architecture)
- [Hardware Components](#hardware-components)
- [Software Stack](#software-stack)
- [RTOS Task Design](#rtos-task-design)
- [Machine Learning](#machine-learning)
- [IoT Data Format](#iot-data-format)
- [GPIO Pin Mapping](#gpio-pin-mapping)
- [Results](#results)
- [Getting Started](#getting-started)
- [Future Scope](#future-scope)
- [Authors](#authors)
- [License](#license)

---

## 📖 Overview

This project presents the design and development of a **Real-Time Operating System (RTOS)-based industrial monitoring and security system** using the **ESP32 DevKit** microcontroller. The system continuously monitors industrial environments through multiple sensors, transmits data via **MQTT over Wi-Fi**, and uses **Machine Learning** to detect anomalies and predict hazards before they escalate.

Traditional monitoring systems relying on manual supervision lead to delayed hazard detection and reduced operational efficiency. This system automates that process with intelligent, real-time, and predictive capabilities.

---

## ✨ Features

- ✅ Real-time multi-sensor monitoring (gas, motion, vibration, noise, temperature, light, door)
- ✅ FreeRTOS-based priority-driven multitasking
- ✅ MQTT-based IoT communication to web/mobile dashboard
- ✅ Machine Learning anomaly detection (Random Forest, XGBoost, ANN)
- ✅ Hybrid alert system — threshold-based + ML prediction
- ✅ Web dashboard with live sensor graphs and RTOS performance metrics
- ✅ Flutter mobile app for remote monitoring and alerts
- ✅ Automatic safety responses — buzzer, fan, LED, relay activation
- ✅ 95.2% ML model accuracy with ~30% reduction in false alarms
- ✅ Tested for 48 hours of continuous operation with no crashes

---

## 🏗 System Architecture

The system is divided into three layers:

```
┌──────────────────────────────────────────────────┐
│               APPLICATION LAYER                  │
│     Flutter Mobile App  |  Web Dashboard         │
│     Firebase Realtime DB |  Alert Notifications  │
└─────────────────────┬────────────────────────────┘
                      │ MQTT / WebSocket
┌─────────────────────▼────────────────────────────┐
│             COMMUNICATION LAYER                  │
│         Wi-Fi  |  MQTT Broker (HiveMQ)           │
└─────────────────────┬────────────────────────────┘
                      │
┌─────────────────────▼────────────────────────────┐
│               HARDWARE LAYER                     │
│  ESP32 DevKit  +  FreeRTOS  +  Multiple Sensors  │
│  Buzzer | Fan | LED | Relay | OLED Display       │
└──────────────────────────────────────────────────┘
```

---

## 🔧 Hardware Components

| Component | Model | Purpose |
|---|---|---|
| Microcontroller | ESP32 DevKit | Central processing unit |
| Gas Sensor | MQ-2 | Detect LPG, smoke, methane |
| Motion Sensor | HC-SR501 PIR | Intrusion / human detection |
| Vibration Sensor | SW-420 | Machine fault detection |
| Noise Sensor | KY-038 | Abnormal sound detection |
| Temperature & Humidity | DHT11 | Environmental monitoring |
| Door Sensor | Magnetic Reed Switch | Unauthorized access detection |
| RTC Module | DS3231 | Accurate event timestamping |
| Relay Module | Single Channel | Control external devices |
| Buzzer | Active Buzzer Driver | Audio alerts |
| Display | I2C LCD / OLED | Local status display |
| LED Indicators | RGB LEDs | Visual status indication |
| Cooling Fan | DC Exhaust Fan | Auto-activated on gas/heat |
| Power Supply | 12V DC + Buck Converter | Powers entire system (5V/3.3V) |

---

## 💻 Software Stack

| Category | Tool / Library |
|---|---|
| Firmware IDE | ESP-IDF / Arduino IDE |
| RTOS | FreeRTOS |
| IoT Protocol | MQTT (Mosquitto / HiveMQ) |
| Cloud Database | Firebase Realtime DB / Firestore |
| ML Language | Python |
| ML Library | Scikit-learn |
| Mobile App | Flutter + Dart |
| State Management | Riverpod |
| Local Storage | Hive Database |
| Real-time Comms | WebSocket / MQTT |

---

## ⚙️ RTOS Task Design

Each sensor and system function runs as an independent FreeRTOS task with assigned priorities:

| Task Name | Priority | Responsibility |
|---|---|---|
| Emergency Task | 5 (Highest) | Immediate response to gas leaks, intrusion, or critical anomalies |
| Security Task | 4 | Monitors PIR and door sensor for unauthorized access |
| Safety Task | 3 | Monitors gas, vibration, and noise for hazard detection |
| Communication Task | 2 | Publishes sensor data to MQTT broker over Wi-Fi |
| Monitor Task | 2 | Aggregates sensor readings for dashboard |
| Performance Task | 1 (Lowest) | Collects task execution times and CPU load |

Inter-task communication is handled using **queues** and **semaphores** to prevent resource conflicts. A **preemptive scheduling** strategy ensures higher-priority tasks interrupt lower-priority ones when needed.

---

## 🤖 Machine Learning

### Models Used

- Baseline: Random Forest, XGBoost, Artificial Neural Network (ANN)
- Advanced (Proposed): Neuro-Symbolic Model (Neural Network + Symbolic Logic)

### Dataset

| Property | Value |
|---|---|
| Total Samples | 12,500 |
| Training Set | 10,000 (80%) |
| Test Set | 2,500 (20%) |
| Features | 7 (gas, motion, vibration, sound, light, temperature, door status) |
| Classes | Normal (0), Warning (1), Critical (2) |

### Performance Metrics

| Metric | Score |
|---|---|
| Accuracy | 95.2% |
| Precision | 94.6% |
| Recall | 95.0% |
| F1-Score | 94.8% |

### Data Preprocessing Steps

1. Handle missing values
2. Normalize sensor values
3. Noise filtering
4. Feature scaling and label encoding
5. Train-test split (80:20)

---

## 📡 IoT Data Format

Sensor data is published to the MQTT broker as JSON:

```json
{
  "device_id": "ESP32_001",
  "timestamp": "2026-04-18 10:30:25",
  "temperature": 28.5,
  "humidity": 65.2,
  "gas_level": 320,
  "motion_status": true,
  "vibration_level": 0.85,
  "noise_level": 72.4,
  "light_intensity": 450,
  "alert_status": "NORMAL"
}
```

**MQTT Topics:**
```
industrial/system     ← Sensor data publisher
industrial/alerts     ← Alert notifications
industrial/control    ← Remote control commands
```

---

## 🔌 GPIO Pin Mapping

| GPIO Pin | Component | Signal Type |
|---|---|---|
| GPIO 4 | PIR Motion Sensor (HC-SR501) | Digital Input |
| GPIO 5 | Magnetic Door Sensor | Digital Input |
| GPIO 34 | MQ-2 Gas Sensor | Analog Input (ADC) |
| GPIO 35 | KY-038 Noise Sensor | Analog Input (ADC) |
| GPIO 32 | SW-420 Vibration Sensor | Digital Input |
| GPIO 21/22 | DS3231 RTC Module | I2C (SDA/SCL) |
| GPIO 25 | Buzzer | Digital Output (PWM) |
| GPIO 26 | Fan Relay Module | Digital Output |
| GPIO 14 | LED Status Indicator | Digital Output |
| GPIO 27 | LED Alarm Indicator | Digital Output |
| GPIO 18/19/23 | OLED Display | SPI Interface |
| GPIO 13 | Push Button (Reset) | Digital Input (Pull-up) |

---

## 📊 Results

### Real-Time Performance

| Metric | Value |
|---|---|
| Data acquisition time | 1–2 seconds |
| Cloud/app update latency | < 3 seconds |
| Alert trigger time | < 1 second |
| Continuous operation tested | 48 hours |
| System crashes observed | None |

### ML Outcomes

- Detected abnormal conditions in **95% of cases**
- Reduced false alarms by approximately **30%** compared to threshold-only systems
- Successfully identified early warning patterns before critical failure

---

## 🚀 Getting Started

### Prerequisites

- ESP32 DevKit board
- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/) or Arduino IDE with ESP32 board support
- Python 3.8+ with `scikit-learn`, `pandas`, `numpy`
- Flutter SDK (for mobile app)
- MQTT Broker (HiveMQ Cloud or local Mosquitto)
- Firebase project (for cloud database)

### Firmware Setup

```bash
# Clone the repository
git clone https://github.com/your-username/rtos-industrial-monitoring.git
cd rtos-industrial-monitoring/firmware

# Configure Wi-Fi and MQTT credentials
cp config_example.h config.h
# Edit config.h with your SSID, password, and broker address

# Build and flash using ESP-IDF
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### ML Model Training

```bash
cd ml_model

# Install dependencies
pip install -r requirements.txt

# Train the model
python train_model.py

# Run anomaly detection on sample data
python predict.py --input sample_data.csv
```

### Mobile App Setup

```bash
cd flutter_app

# Install Flutter dependencies
flutter pub get

# Run the app
flutter run
```

---

## 👥 Team
- Sakthi Raja V
- Mohammed Zaheer K
- Parthiban N

---

## 🔮 Future Scope

- Integration of **LSTM / CNN** deep learning for time-series anomaly prediction
- **TinyML** deployment on ESP32 for on-device edge AI inference
- Cloud-based historical data analytics for large-scale deployment
- Additional sensors: humidity, pressure, toxic gas (CO, H₂S)
- Enhanced mobile app with voice-based alerts and predictive dashboards
- Data encryption and secure MQTT authentication (TLS/SSL)
- Integration with **Industry 4.0** and smart factory infrastructure
- Wearable **WBAN** devices for simultaneous worker safety monitoring

---

## 👥 Authors

| Name | Register No. |
|---|---|
| Mohammed Zaheer K | 950322106009 |
| Parthiban N | 950322106011 |
| Sakthi Raja V | 950321106012 |

**Supervisor:** Mr. R. James Nesaratnam M.E., Assistant Professor, Dept. of ECE

**Institution:** Grace College of Engineering, Thoothukudi — Anna University, Chennai

---

## 📄 License

This project was submitted in partial fulfillment of the requirements for the **Bachelor of Engineering in Electronics and Communication Engineering** at Anna University, Chennai (April 2026).

For academic and research use only. Please cite appropriately if referencing this work.

---

> ⭐ If you find this project useful, consider giving it a star!

# Fleet Tracker Firmware
ESP32-based GPS fleet tracking firmware using **SIM808** (GPRS + GPS) and **MQTT** over a cellular connection.

---

## Overview

This Arduino project runs on an **ESP32** and:
- Reads GPS coordinates from a **SIM808** module via UART
- Connects to the internet via **GPRS** (Ooredoo TN APN)
- Publishes real-time location data to an **MQTT broker** every 5 seconds

---

## Hardware Required

| Component | Details |
|-----------|---------|
| Microcontroller | ESP32 |
| GSM/GPS Module | SIM808 (dual: GPRS + GPS) |
| SIM Card | Ooredoo TN (or any GPRS-enabled SIM) |

### Wiring

| ESP32 Pin | SIM808 |
|-----------|--------|
| GPIO 16 (RX2) | SIM800 TX |
| GPIO 17 (TX2) | SIM800 RX |
| GPIO 4 (RX1) | GPS TX |
| GPIO 5 (TX1) | GPS RX |

---

## Dependencies

Install these libraries via the **Arduino Library Manager**:
- [`TinyGSM`](https://github.com/vshymanskyy/TinyGSM)
- [`PubSubClient`](https://github.com/knolleary/pubsubclient)
- [`ArduinoJson`](https://arduinojson.org/)

---

## Configuration

Before flashing, update the following in `main.ino`:

```cpp
const char apn[] = "internet.ooredoo.tn";
const char* mqttBroker = "broker.hivemq.com";
const int   mqttPort   = 1883;
```

> For production, move credentials to a `secrets.h` file and add it to `.gitignore`.

---

## MQTT Payload

Data is published to: `fleet/<deviceId>/gps`

```json
{
  "deviceId": "ESP32-XXXXXXXXXXXX",
  "location": {
    "lat": 36.8189,
    "lng": 10.1658
  },
  "altitude": 12.5,
  "speed": 0.0,
  "heading": 142,
  "satellites": 8,
  "hdop": 1.2,
  "timestamp": "2026-05-15T12:00:00Z"
}
```

---

## How to Flash

1. Open `main.ino` in **Arduino IDE**
2. Select board: `ESP32 Dev Module`
3. Select the correct **COM port**
4. Click **Upload**
5. Open **Serial Monitor** at `115200 baud` to view logs

---

## Project Structure

```
fleet-firmware/
├── sim808_gps_mqtt.ino   # Main Arduino sketch
├── .gitignore            # Ignores build artifacts and secrets
└── README.md             # This file
```

---

## Related Repositories

- [`fleet-management-frontend`](https://github.com/sirineelayeb/fleet-management-frontend) — Map dashboard UI
- [`fleet-management-backend`](https://github.com/sirineelayeb/fleet-management-backend) — MQTT consumer & API

---

## License

MIT

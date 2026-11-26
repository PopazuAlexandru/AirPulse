# AirPulse

AirPulse combines an ESP32 indoor-air monitor with an authenticated AI
ingestion service, Prometheus metrics, and a provisioned Grafana dashboard.

## Requirements

- PlatformIO Core and an ESP32 Dev Board (30-pin, `esp32dev`)
- Docker Desktop with Docker Compose
- The ESP32 and Docker host reachable on the same LAN

## 1. Start the backend and monitoring stack

From the repository root, create the local environment file before starting
Compose:

```powershell
if (-not (Test-Path .env)) { Copy-Item .env.example .env }
# Edit .env and set API_TOKEN and GF_SECURITY_ADMIN_PASSWORD.
docker compose up --build -d
docker compose ps
```

The AI service trains a deterministic, synthetic indoor baseline during image
build if its model artifacts are absent. The local `.env` is ignored by Git.
For any non-test deployment, replace both example values with private values
and configure the ESP32's `api_token` provisioning value to match `API_TOKEN`.

```powershell
# Edit .env with strong, unique values before starting the services.
docker compose up --build -d
```

The default endpoints are:

- Ingestion API: `http://localhost:8000`
- Prometheus: `http://localhost:9090`
- Grafana: `http://localhost:3000` (login `admin` and the password in `.env`)

## Hardware Wiring

Use a 30-pin ESP32 Dev Board on the breadboard with the USB-C connector facing
up. Mount the board across columns B–J and rows 3–17. Bridge the left and right
blue ground rails with a black jumper on row 29 to provide common ground. The
PMS5003 and MH-Z19C connect through solderless plug-and-play adapters; do not
cut or strip their sensor cables.

| Component / Adapter | Adapter or Component Pin | ESP32 GPIO / Bus | Breadboard Connection (Row 1 at Top) |
| :--- | :--- | :--- | :--- |
| **ESP32 (Right – 5V)** | `VIN` / `GND` | Power Input | `J3` -> Right `+` rail (5V) / `J4` -> Right `-` rail (GND) |
| **ESP32 (Left – 3.3V)** | `3V3` / `GND` | 3.3V Output | `A3` -> Left `+` rail (3.3V) / `A4` -> Left `-` rail (GND) |
| **Common Ground** | Left & Right `-` Rails | GND | Black jumper on **Row 29** bridging both blue `-` rails |
| **PMS5003** via **1.25mm-to-2.54mm Breakout Adapter Board** | `VCC` / `GND` | 5V Power | `VCC` -> Right `+` rail (5V) / `GND` -> `-` GND rail |
| **PMS5003** via **1.25mm-to-2.54mm Breakout Adapter Board** | `RXD` / `TXD` | `UART1` (`GPIO 19` / `GPIO 18`) | `RXD` -> `GPIO 19` (`D19`) / `TXD` -> `GPIO 18` (`D18`); leave `SET` and `RST` unconnected |
| **MH-Z19C (CO2)** via **7-pin JST 1.25mm to 2.54mm DuPont Adapter Cable** | `Vin` / `GND` | 5V Power | `Vin` -> Right `+` rail (5V) / `GND` -> `-` GND rail |
| **MH-Z19C (CO2)** via **7-pin JST 1.25mm to 2.54mm DuPont Adapter Cable** | `TX` / `RX` | `UART2` (`GPIO 16` / `GPIO 17`) | `TX` -> `GPIO 16` (`RX2`) / `RX` -> `GPIO 17` (`TX2`); remaining 3 adapter pins unused |
| **Sensirion SHT35** | `VCC` / `GND` | 3.3V Power | Left `+` rail (3.3V) / `-` GND rail |
| **Sensirion SHT35** | `SDA` / `SCL` | Shared `I2C` (`GPIO 21` / `GPIO 22`, address `0x44`) | `SDA` -> **Row 22** (`A13`/`D21`) / `SCL` -> **Row 23** (`A16`/`D22`) |
| **0.96" SSD1306 OLED** | `VCC` / `GND` | 3.3V Power | Left `+` rail (3.3V) / `-` GND rail |
| **0.96" SSD1306 OLED** | `SDA` / `SCL` | Shared `I2C` (`GPIO 21` / `GPIO 22`, address `0x3C`) | `SDA` -> **Row 22** (`A13`/`D21`) / `SCL` -> **Row 23** (`A16`/`D22`) |

### Bill of Materials

- 30-pin ESP32 Dev Board (`esp32dev`)
- Plantower PMS5003 particulate-matter sensor
- PMS5003 1.25mm-to-2.54mm breakout adapter board
- MH-Z19C CO2 sensor
- 7-pin JST 1.25mm to 2.54mm DuPont adapter cable
- Sensirion SHT35 temperature/humidity sensor
- 0.96" SSD1306 OLED display
- Breadboard, jumper wires, and USB-C cable

## 2. Flash the ESP32 firmware

Connect the ESP32 over USB, then from PowerShell:

```powershell
Set-Location .\firmware
pio run -t upload
pio run -t uploadfs
pio device monitor
```

On first boot, connect to the `AirPulse-Setup` Wi-Fi portal. Configure
`ai_server_url` as `http://<DOCKER_HOST_LAN_IP>:8000/telemetry` and set
`api_token` to the same value as `API_TOKEN` used by Compose. Use the Docker
host's LAN IP, not `localhost` or the Compose-only hostname `ai-service`.

The firmware project setup is documented in
[firmware/README.md](./firmware/README.md).

## 3. Send a test telemetry sample

Run this from any machine that can reach the Docker host. Replace the URL if
the service is not on the local machine:

```sh
API_TOKEN="$(grep '^API_TOKEN=' .env | cut -d= -f2-)"
curl -X POST http://localhost:8000/telemetry \
  -H "Authorization: Bearer ${API_TOKEN}" \
  -H "Content-Type: application/json" \
  -d '{"wifi":-52,"rco2":850,"pm02":8,"pm10":12,"atmp":22.40,"rhum":45.10,"serialno":"esp32:30pin_01"}'
```

The response contains `status`, an EMA-smoothed `anomaly_score` in the range
0–1, `anomaly_flag`, and a reason. Three consecutive smoothed scores above
`0.65` are needed for a model anomaly flag (`0.80` marks a critical spike);
deterministic sensor faults flag immediately.
Prometheus scrapes `/metrics` every three seconds, and Grafana provisions the
AirPulse dashboard automatically.

Useful checks:

```sh
curl http://localhost:8000/health
curl http://localhost:8000/metrics
docker compose logs -f ai-service
```

Run focused service tests with:

```powershell
$env:API_TOKEN = "set-this-to-the-API_TOKEN-value-from-.env"
python -m pip install -r .\backend\ai_service\requirements-dev.txt
python -m pytest .\backend\tests
```

## Model and service notes

Per-device CO2 rates, 15-minute temperature/humidity EMA deltas, and rolling
sensor history are kept in process memory and reset when the service restarts.
The Isolation Forest and its scaler are stored as `model.joblib` and
`scaler.joblib` in the container's model directory. Rebuild the image to
retrain them with `docker compose build --no-cache ai-service`.

## Scrum Sprint 2 Deliverables & GitFlow

- **US-01 — On-device real-time metrics:** the ESP32 samples sensors on a
  `millis()` cadence, refreshes the 0.96" SSD1306 OLED (`0x3C`) every 3 seconds,
  and displays CO2, PM2.5/PM10, temperature/humidity, AI status, and
  `ERR: SENSOR OFF` when a sensor is disconnected. WiFiManager provisioning
  and HTTP telemetry delivery run separately from the sampling/display loop.
- **US-02 — Grafana dashboard and alerts:** the provisioned dashboard includes
  time-series views, live CO2 and PM2.5 gauges, critical thresholds at
  1,000 ppm and 35 µg/m³, and a `SENSOR DISCONNECTED / NO DATA` telemetry
  state. Prometheus evaluates the provisioned threshold and missing-data
  alert rules every 3 seconds.
- **US-03 — AI anomaly detection:** the Python 3.11 FastAPI service uses an
  Isolation Forest with CO2-rate, PM ratio, and 15-minute temperature/humidity
  EMA-delta features. It applies EMA smoothing, immediately flags deterministic
  sensor faults, and debounces model anomalies across three samples above
  `0.65` (`0.80` is the critical-spike threshold). Tests live in
  `backend/tests/test_ai_engine.py`.
- **US-04 — Wireless provisioning:** WiFiManager opens the `AirPulse-Setup`
  captive portal for 2.4 GHz Wi-Fi credentials, `ai_server_url`, and
  `api_token` without blocking the sensor loop. Network and service settings
  persist in NVS `Preferences`;
  LittleFS buffers up to 500 records in `/buffer.jsonl`.
- **Hardware standard:** 30-pin ESP32 Dev Board on a breadboard with MH-Z19C,
  PMS5003, SHT35 (`0x44`), and 0.96" SSD1306 OLED (`0x3C`). The PMS5003 and
  MH-Z19C use 1.25mm-to-2.54mm solderless adapters; UART1/UART2 and I2C
  (`GPIO 21/22`) follow the wiring table above.
- **GitFlow:** `main` contains release-ready increments; `develop` integrates
  sprint work. User-story branches are
  `feature/us-01-oled-display`, `feature/us-02-grafana-alerts`,
  `feature/us-03-ai-anomaly-engine`, and `feature/us-04-wifimanager`.
  Completed increments are merged into `develop`, then promoted to `main`
  and tagged.

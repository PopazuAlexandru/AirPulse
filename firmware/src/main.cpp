#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiManager.h>

#include "Config.h"
#include "DisplayUi.h"
#include "OfflineBuffer.h"
#include "Sensors.h"
#include "Telemetry.h"

using namespace AirPulseConfig;

Preferences preferences;
WiFiManager wifiManager;
WiFiManagerParameter* serverUrlParameter = nullptr;
WiFiManagerParameter* apiTokenParameter = nullptr;
char configuredServerUrl[192] = {};
char configuredApiToken[128] = {};

MhZ19cDriver co2Sensor;
Pms5003Driver particulateSensor;
Sht35Driver temperatureHumiditySensor;
SensorReadings readings;
OfflineBuffer offlineBuffer;
TelemetryClient telemetryClient;
DisplayUi displayUi;
AiStatus aiStatus;

uint32_t bootTimeMs = 0;
uint32_t lastAcquisitionMs = 0;
uint32_t lastDisplayMs = 0;
uint32_t telemetryDueMs = 0;
uint32_t co2ResponseDeadlineMs = 0;
uint32_t lastPmSampleMs = 0;
uint32_t lastPmWarningMs = 0;
bool telemetryPending = false;
bool co2ResponsePending = false;
bool storageReady = false;

void persistNetworkParameters() {
  if (serverUrlParameter == nullptr || apiTokenParameter == nullptr) {
    return;
  }
  strlcpy(configuredServerUrl, serverUrlParameter->getValue(),
          sizeof(configuredServerUrl));
  strlcpy(configuredApiToken, apiTokenParameter->getValue(),
          sizeof(configuredApiToken));
  preferences.begin("airpulse", false);
  preferences.putString("ai_server_url", configuredServerUrl);
  preferences.putString("api_token", configuredApiToken);
  preferences.end();
  Serial.println("[wifi] Telemetry settings saved to NVS");
}

void loadNetworkParameters() {
  preferences.begin("airpulse", false);
  const String savedUrl =
      preferences.getString("ai_server_url", DEFAULT_AI_SERVER_URL);
  const String savedToken =
      preferences.getString("api_token", DEFAULT_API_TOKEN);
  preferences.end();

  strlcpy(configuredServerUrl, savedUrl.c_str(), sizeof(configuredServerUrl));
  strlcpy(configuredApiToken, savedToken.c_str(), sizeof(configuredApiToken));
}

void provisionWifi() {
  loadNetworkParameters();
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  wifiManager.setConfigPortalTimeout(180);
  wifiManager.setConnectTimeout(20);

  serverUrlParameter = new WiFiManagerParameter(
      "ai_server_url", "AI server URL", configuredServerUrl,
      sizeof(configuredServerUrl) - 1);
  apiTokenParameter = new WiFiManagerParameter(
      "api_token", "API token", configuredApiToken,
      sizeof(configuredApiToken) - 1);
  wifiManager.addParameter(serverUrlParameter);
  wifiManager.addParameter(apiTokenParameter);
  wifiManager.setSaveConfigCallback(persistNetworkParameters);

  Serial.printf("[wifi] Starting provisioning AP: %s\n", WIFI_AP_NAME);
  if (!wifiManager.autoConnect(WIFI_AP_NAME)) {
    Serial.println("[wifi] Provisioning timed out; continuing offline");
  } else {
    Serial.printf("[wifi] Connected, IP=%s\n",
                  WiFi.localIP().toString().c_str());
  }

  persistNetworkParameters();
  telemetryClient.configure(configuredServerUrl, configuredApiToken);
}

void pollSensors(uint32_t nowMs) {
  uint16_t co2Ppm = 0;
  if (co2Sensor.poll(nowMs, co2Ppm)) {
    readings.co2Ppm = co2Ppm;
    readings.hasCo2 = true;
    co2ResponsePending = false;
  }

  uint16_t pm1 = 0;
  uint16_t pm25 = 0;
  uint16_t pm10 = 0;
  if (particulateSensor.poll(nowMs, pm1, pm25, pm10)) {
    readings.pm1 = pm1;
    readings.pm25 = pm25;
    readings.pm10 = pm10;
    readings.hasParticulate = true;
    lastPmSampleMs = nowMs;
  }

  float temperatureC = 0.0F;
  float humidityPercent = 0.0F;
  bool shtConversionCompleted = false;
  if (temperatureHumiditySensor.poll(nowMs, temperatureC, humidityPercent,
                                     shtConversionCompleted)) {
    readings.temperatureC = temperatureC;
    readings.humidityPercent = humidityPercent;
    readings.hasTemperatureHumidity = true;
  } else if (shtConversionCompleted) {
    Serial.println("[sensor] SHT35 conversion/read failed; retaining last valid sample");
  }
}

void acquireSensors(uint32_t nowMs) {
  co2Sensor.requestReading();
  co2ResponsePending = true;
  co2ResponseDeadlineMs = nowMs + 1000;
  if (!temperatureHumiditySensor.request(nowMs)) {
    Serial.println("[sensor] SHT35 read failed; retaining last valid sample");
  }
  if (nowMs - bootTimeMs >= PM_WARMUP_MS &&
      (!readings.hasParticulate || nowMs - lastPmSampleMs > 5000) &&
      nowMs - lastPmWarningMs >= 30000) {
    lastPmWarningMs = nowMs;
    Serial.println("[sensor] PMS5003 has not produced a valid frame recently");
  }

  telemetryDueMs = nowMs + CO2_RESPONSE_GRACE_MS;
  telemetryPending = true;
}

void sendOrBufferTelemetry() {
  const int wifiRssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -100;
  const String payload = telemetryClient.createPayload(wifiRssi, readings);

  if (!storageReady) {
    Serial.println("[storage] Buffer unavailable; telemetry cannot be persisted");
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[telemetry] Wi-Fi offline; buffering sample");
    offlineBuffer.append(payload);
    return;
  }

  size_t flushed = 0;
  while (flushed < OFFLINE_FLUSH_BATCH_SIZE && offlineBuffer.hasRecords()) {
    String bufferedPayload;
    if (!offlineBuffer.peekFirst(bufferedPayload) ||
        !telemetryClient.post(bufferedPayload, aiStatus)) {
      Serial.println("[telemetry] Buffer flush paused; preserving queued records");
      offlineBuffer.append(payload);
      return;
    }
    if (!offlineBuffer.removeFirst()) {
      Serial.println("[telemetry] Sent record remains queued after storage error");
      offlineBuffer.append(payload);
      return;
    }
    ++flushed;
  }

  if (offlineBuffer.hasRecords()) {
    Serial.println("[telemetry] Buffer remains; deferring live sample");
    offlineBuffer.append(payload);
    return;
  }

  if (!telemetryClient.post(payload, aiStatus)) {
    Serial.println("[telemetry] Live POST failed; buffering sample");
    offlineBuffer.append(payload);
  }
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  bootTimeMs = millis();
  Serial.println("\n[AirPulse] Starting firmware");

  temperatureHumiditySensor.begin();
  displayUi.begin();
  storageReady = offlineBuffer.begin();

  // Serial1 and Serial2 are the ESP32 Arduino core hardware UART instances.
  Serial2.begin(SENSOR_BAUD, SERIAL_8N1, CO2_RX_PIN, CO2_TX_PIN);
  Serial1.begin(SENSOR_BAUD, SERIAL_8N1, PM_RX_PIN, PM_TX_PIN);
  co2Sensor.begin(Serial2, bootTimeMs);
  particulateSensor.begin(Serial1, bootTimeMs);
  Serial.println("[sensor] MH-Z19C ABC disabled; sensor warmup is non-blocking");

  provisionWifi();
  lastAcquisitionMs = millis() - ACQUISITION_INTERVAL_MS;
}

void loop() {
  const uint32_t nowMs = millis();
  pollSensors(nowMs);

  if (nowMs - lastAcquisitionMs >= ACQUISITION_INTERVAL_MS) {
    lastAcquisitionMs = nowMs;
    acquireSensors(nowMs);
  }

  if (co2ResponsePending &&
      static_cast<int32_t>(nowMs - co2ResponseDeadlineMs) >= 0) {
    co2ResponsePending = false;
    if (nowMs - bootTimeMs >= CO2_WARMUP_MS) {
      Serial.println("[sensor] MH-Z19C did not return a valid CO2 frame");
    }
  }

  if (telemetryPending &&
      static_cast<int32_t>(nowMs - telemetryDueMs) >= 0) {
    telemetryPending = false;
    sendOrBufferTelemetry();
  }

  if (nowMs - lastDisplayMs >= 500) {
    lastDisplayMs = nowMs;
    const bool connected = WiFi.status() == WL_CONNECTED;
    displayUi.render(readings, connected, connected ? WiFi.RSSI() : -100,
                     aiStatus);
  }
}

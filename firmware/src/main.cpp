#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <freertos/queue.h>

#include "Config.h"
#include "DisplayUi.h"
#include "OfflineBuffer.h"
#include "Sensors.h"
#include "Telemetry.h"

using namespace AirPulseConfig;

Preferences preferences;
WiFiManager wifiManager;
QueueHandle_t telemetryQueue = nullptr;
portMUX_TYPE aiStatusMux = portMUX_INITIALIZER_UNLOCKED;
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
uint32_t lastCo2SampleMs = 0;
uint32_t lastPmSampleMs = 0;
uint32_t lastSht35SampleMs = 0;
uint32_t lastPmWarningMs = 0;
uint32_t wifiPortalStartedMs = 0;
bool telemetryPending = false;
bool co2ResponsePending = false;
bool storageReady = false;
bool telemetryWorkerReady = false;
bool wifiPortalActive = false;
bool co2SensorOffline = false;
bool pmSensorOffline = false;
bool sht35SensorOffline = false;

struct TelemetryJob {
  char payload[256];
};

AiStatus copyAiStatus() {
  AiStatus snapshot;
  portENTER_CRITICAL(&aiStatusMux);
  snapshot = aiStatus;
  portEXIT_CRITICAL(&aiStatusMux);
  return snapshot;
}

void updateAiStatus(const AiStatus& updatedStatus) {
  if (!updatedStatus.available) {
    return;
  }
  portENTER_CRITICAL(&aiStatusMux);
  aiStatus = updatedStatus;
  portEXIT_CRITICAL(&aiStatusMux);
}

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
  wifiManager.setConfigPortalTimeout(
      AirPulseConfig::WIFI_CONFIG_PORTAL_TIMEOUT_MS / 1000);
  wifiManager.setConnectTimeout(20);
  wifiManager.setConfigPortalBlocking(false);

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
    wifiPortalActive = true;
    wifiPortalStartedMs = millis();
    Serial.println("[wifi] Provisioning portal active; continuing offline");
  } else {
    Serial.printf("[wifi] Connected, IP=%s\n",
                  WiFi.localIP().toString().c_str());
  }

  persistNetworkParameters();
  telemetryClient.configure(configuredServerUrl, configuredApiToken);
}

void pollSensors(uint32_t nowMs) {
  uint16_t co2Ppm = 0;
  if (co2Sensor.poll(co2Ppm)) {
    readings.co2Ppm = co2Ppm;
    readings.hasCo2 = true;
    lastCo2SampleMs = nowMs;
    co2SensorOffline = false;
    co2ResponsePending = false;
  }

  uint16_t pm1 = 0;
  uint16_t pm25 = 0;
  uint16_t pm10 = 0;
  if (particulateSensor.poll(pm1, pm25, pm10)) {
    readings.pm1 = pm1;
    readings.pm25 = pm25;
    readings.pm10 = pm10;
    readings.hasParticulate = true;
    lastPmSampleMs = nowMs;
    pmSensorOffline = false;
  }

  float temperatureC = 0.0F;
  float humidityPercent = 0.0F;
  bool shtConversionCompleted = false;
  if (temperatureHumiditySensor.poll(nowMs, temperatureC, humidityPercent,
                                     shtConversionCompleted)) {
    readings.temperatureC = temperatureC;
    readings.humidityPercent = humidityPercent;
    readings.hasTemperatureHumidity = true;
    lastSht35SampleMs = nowMs;
    sht35SensorOffline = false;
  } else if (shtConversionCompleted) {
    sht35SensorOffline = true;
    Serial.println("[sensor] SHT35 conversion/read failed; retaining last valid sample");
  }
}

void updateSensorHealth(uint32_t nowMs) {
  co2SensorOffline =
      !readings.hasCo2 ||
      nowMs - lastCo2SampleMs >= SENSOR_STALE_TIMEOUT_MS;
  pmSensorOffline =
      !readings.hasParticulate ||
      nowMs - lastPmSampleMs >= SENSOR_STALE_TIMEOUT_MS;
  if (readings.hasTemperatureHumidity) {
    sht35SensorOffline =
        nowMs - lastSht35SampleMs >= SENSOR_STALE_TIMEOUT_MS;
  } else if (nowMs - bootTimeMs >= SENSOR_STALE_TIMEOUT_MS) {
    sht35SensorOffline = true;
  }
}

void acquireSensors(uint32_t nowMs) {
  co2Sensor.requestReading();
  co2ResponsePending = true;
  co2ResponseDeadlineMs = nowMs + 1000;
  if (!temperatureHumiditySensor.request(nowMs)) {
    sht35SensorOffline = true;
    Serial.println("[sensor] SHT35 read failed; retaining last valid sample");
  }
  const bool particulateStale =
      readings.hasParticulate
          ? nowMs - lastPmSampleMs >= SENSOR_STALE_TIMEOUT_MS
          : nowMs - bootTimeMs >= SENSOR_STALE_TIMEOUT_MS;
  if (particulateStale &&
      nowMs - lastPmWarningMs >= 30000) {
    lastPmWarningMs = nowMs;
    Serial.println("[sensor] PMS5003 has not produced a valid frame recently");
  }

  telemetryDueMs = nowMs + CO2_RESPONSE_GRACE_MS;
  telemetryPending = true;
}

bool postTelemetry(const String& payload) {
  AiStatus responseStatus;
  const bool accepted = telemetryClient.post(payload, responseStatus);
  if (accepted) {
    updateAiStatus(responseStatus);
  }
  return accepted;
}

void sendOrBufferTelemetry(const String& payload) {
  const int wifiRssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -100;

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
        !postTelemetry(bufferedPayload)) {
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

  if (!postTelemetry(payload)) {
    Serial.println("[telemetry] Live POST failed; buffering sample");
    offlineBuffer.append(payload);
  }
}

void telemetryWorker(void*) {
  TelemetryJob job;
  for (;;) {
    if (xQueueReceive(telemetryQueue, &job, portMAX_DELAY) == pdTRUE) {
      sendOrBufferTelemetry(String(job.payload));
    }
  }
}

void enqueueTelemetry() {
  if (!telemetryWorkerReady || telemetryQueue == nullptr) {
    Serial.println("[telemetry] Worker unavailable; sample was not queued");
    return;
  }
  const int wifiRssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -100;
  const String payload = telemetryClient.createPayload(wifiRssi, readings);
  TelemetryJob job = {};
  if (strlcpy(job.payload, payload.c_str(), sizeof(job.payload)) >=
      sizeof(job.payload)) {
    Serial.println("[telemetry] Payload exceeds queue record capacity");
    return;
  }
  if (xQueueSend(telemetryQueue, &job, 0) != pdTRUE) {
    Serial.println("[telemetry] Queue full; sample was not queued");
  }
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  bootTimeMs = millis();
  Serial.println("\n[AirPulse] Starting firmware");

  temperatureHumiditySensor.begin();
  displayUi.begin();
  displayUi.render(readings, false, -100, aiStatus, false);
  storageReady = offlineBuffer.begin();
  telemetryQueue = xQueueCreate(TELEMETRY_QUEUE_LENGTH, sizeof(TelemetryJob));
  if (telemetryQueue == nullptr) {
    Serial.println("[telemetry] Could not create telemetry queue");
  }

  // Serial1 and Serial2 are the ESP32 Arduino core hardware UART instances.
  Serial2.begin(SENSOR_BAUD, SERIAL_8N1, CO2_RX_PIN, CO2_TX_PIN);
  Serial1.begin(SENSOR_BAUD, SERIAL_8N1, PM_RX_PIN, PM_TX_PIN);
  co2Sensor.begin(Serial2);
  particulateSensor.begin(Serial1);
  Serial.println("[sensor] Startup sampling is non-blocking");

  provisionWifi();
  if (telemetryQueue != nullptr &&
      xTaskCreatePinnedToCore(telemetryWorker, "telemetry", 8192, nullptr, 1,
                              nullptr, 0) == pdPASS) {
    telemetryWorkerReady = true;
  } else {
    Serial.println("[telemetry] Could not start telemetry worker");
  }
  lastAcquisitionMs = millis() - ACQUISITION_INTERVAL_MS;
  lastDisplayMs = millis() - DISPLAY_REFRESH_INTERVAL_MS;
}

void loop() {
  const uint32_t nowMs = millis();
  if (wifiPortalActive) {
    wifiManager.process();
    if (WiFi.status() == WL_CONNECTED) {
      wifiPortalActive = false;
      Serial.printf("[wifi] Connected, IP=%s\n",
                    WiFi.localIP().toString().c_str());
    } else if (nowMs - wifiPortalStartedMs >=
               WIFI_CONFIG_PORTAL_TIMEOUT_MS) {
      wifiPortalActive = false;
      Serial.println("[wifi] Provisioning timed out; continuing offline");
    }
  }
  pollSensors(nowMs);

  if (nowMs - lastAcquisitionMs >= ACQUISITION_INTERVAL_MS) {
    lastAcquisitionMs = nowMs;
    acquireSensors(nowMs);
  }

  if (co2ResponsePending &&
      static_cast<int32_t>(nowMs - co2ResponseDeadlineMs) >= 0) {
    co2ResponsePending = false;
    if (!readings.hasCo2 ||
        nowMs - lastCo2SampleMs >= SENSOR_STALE_TIMEOUT_MS) {
      Serial.println("[sensor] MH-Z19C did not return a valid CO2 frame");
    }
  }

  if (telemetryPending &&
      static_cast<int32_t>(nowMs - telemetryDueMs) >= 0) {
    telemetryPending = false;
    enqueueTelemetry();
  }

  updateSensorHealth(nowMs);
  if (nowMs - lastDisplayMs >= DISPLAY_REFRESH_INTERVAL_MS) {
    lastDisplayMs = nowMs;
    const bool connected = WiFi.status() == WL_CONNECTED;
    displayUi.render(readings, connected, connected ? WiFi.RSSI() : -100,
                     copyAiStatus(), co2SensorOffline || pmSensorOffline ||
                                         sht35SensorOffline);
  }
}

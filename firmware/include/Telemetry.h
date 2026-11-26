#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <math.h>

#include "Config.h"
#include "Sensors.h"

struct AiStatus {
  bool available = false;
  int anomalyFlag = 0;
  float anomalyScore = 0.0F;
};

class TelemetryClient {
 public:
  void configure(const char* serverUrl, const char* apiToken) {
    serverUrl_ = serverUrl;
    apiToken_ = apiToken;
  }

  String createPayload(int wifiRssi, const SensorReadings& readings) const {
    StaticJsonDocument<256> document;
    document["wifi"] = wifiRssi;
    document["rco2"] = readings.co2Ppm;
    document["pm02"] = readings.pm25;
    document["pm10"] = readings.pm10;
    document["atmp"] = readings.temperatureC;
    document["rhum"] = readings.humidityPercent;
    document["serialno"] = AirPulseConfig::SERIAL_NUMBER;

    String payload;
    serializeJson(document, payload);
    return payload;
  }

  bool post(const String& payload, AiStatus& responseStatus) const {
    if (serverUrl_.length() == 0 || apiToken_.length() == 0) {
      Serial.println("[http] Server URL or API token is empty");
      return false;
    }

    HTTPClient http;
    http.setConnectTimeout(1500);
    http.setTimeout(2000);
    if (!http.begin(serverUrl_)) {
      Serial.println("[http] Could not initialize HTTP client");
      return false;
    }
    http.addHeader("Authorization", String("Bearer ") + apiToken_);
    http.addHeader("Content-Type", "application/json");
    const int statusCode = http.POST(payload);
    if (statusCode < 200 || statusCode >= 300) {
      Serial.printf("[http] POST failed, status=%d\n", statusCode);
      http.end();
      return false;
    }

    const String response = http.getString();
    http.end();
    updateAiStatus(response, responseStatus);
    Serial.printf("[http] Telemetry accepted, status=%d\n", statusCode);
    return true;
  }

 private:
  static void updateAiStatus(const String& response, AiStatus& aiStatus) {
    StaticJsonDocument<256> document;
    const DeserializationError error = deserializeJson(document, response);
    if (error) {
      Serial.printf("[http] Invalid AI response JSON: %s\n", error.c_str());
      return;
    }

    if (!document["anomaly_score"].is<float>() ||
        !document["anomaly_flag"].is<int>()) {
      Serial.println("[http] AI response is missing numeric anomaly fields");
      return;
    }
    const float score = document["anomaly_score"].as<float>();
    const int flag = document["anomaly_flag"].as<int>();
    if (!isfinite(score) || score < 0.0F || score > 1.0F ||
        (flag != 0 && flag != 1)) {
      Serial.println("[http] AI response values are out of range");
      return;
    }

    aiStatus.available = true;
    aiStatus.anomalyScore = score;
    aiStatus.anomalyFlag = flag;
  }

  String serverUrl_;
  String apiToken_;
};

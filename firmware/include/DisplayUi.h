#pragma once

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>

#include "Config.h"
#include "Sensors.h"
#include "Telemetry.h"

class DisplayUi {
 public:
  bool begin() {
    if (!display_.begin(SSD1306_SWITCHCAPVCC, AirPulseConfig::OLED_ADDRESS)) {
      Serial.println("[display] SSD1306 initialization failed");
      return false;
    }
    display_.clearDisplay();
    display_.setTextColor(SSD1306_WHITE);
    display_.setTextSize(1);
    display_.display();
    ready_ = true;
    return true;
  }

  void render(const SensorReadings& readings, bool wifiConnected, int wifiRssi,
              const AiStatus& aiStatus, bool sensorOffline) {
    if (!ready_) {
      return;
    }
    display_.clearDisplay();
    display_.setTextColor(SSD1306_WHITE);
    display_.setCursor(0, 0);
    if (sensorOffline) {
      display_.print("ERR: SENSOR OFF");
    } else {
      display_.print("AirPulse");
      display_.setCursor(68, 0);
      if (wifiConnected) {
        display_.printf("%d dBm", wifiRssi);
      } else {
        display_.print("OFFLINE");
      }
    }
    display_.drawFastHLine(0, 10, 128, SSD1306_WHITE);

    display_.setCursor(0, 16);
    display_.print("CO2: ");
    if (readings.hasCo2) {
      display_.printf("%u ppm", readings.co2Ppm);
    } else {
      display_.print("-- ppm");
    }

    display_.setCursor(0, 29);
    display_.print("PM2.5: ");
    if (readings.hasParticulate) {
      display_.printf("%u | PM10: %u", readings.pm25, readings.pm10);
    } else {
      display_.print("-- | PM10: --");
    }

    display_.setCursor(0, 42);
    if (readings.hasTemperatureHumidity) {
      display_.printf("T: %.2fC  H: %.2f%%", readings.temperatureC,
                      readings.humidityPercent);
    } else {
      display_.print("T: --.--C  H: --.--%");
    }

    const bool anomaly = aiStatus.available && aiStatus.anomalyFlag == 1;
    if (anomaly) {
      display_.fillRect(0, 54, 128, 10, SSD1306_WHITE);
      display_.setTextColor(SSD1306_BLACK);
    } else {
      display_.setTextColor(SSD1306_WHITE);
    }
    display_.setCursor(2, 55);
    if (!aiStatus.available) {
      display_.print("AI: WAITING");
    } else if (anomaly) {
      display_.printf("AI: ANOMALY! (%.2f)", aiStatus.anomalyScore);
    } else {
      display_.printf("AI: NORMAL (%.2f)", aiStatus.anomalyScore);
    }
    display_.display();
  }

 private:
  Adafruit_SSD1306 display_{128, 64, &Wire, -1};
  bool ready_ = false;
};

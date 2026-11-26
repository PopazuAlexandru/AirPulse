#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "Config.h"

struct SensorReadings {
  uint16_t co2Ppm = 0;
  uint16_t pm1 = 0;
  uint16_t pm25 = 0;
  uint16_t pm10 = 0;
  float temperatureC = 0.0F;
  float humidityPercent = 0.0F;
  bool hasCo2 = false;
  bool hasParticulate = false;
  bool hasTemperatureHumidity = false;
};

class MhZ19cDriver {
 public:
  void begin(HardwareSerial& serial) {
    serial_ = &serial;
    disableAbc();
  }

  void requestReading() {
    static const uint8_t request[] = {0xFF, 0x01, 0x86, 0, 0, 0, 0, 0, 0x79};
    if (serial_ != nullptr) {
      serial_->write(request, sizeof(request));
    }
  }

  bool poll(uint16_t& ppm) {
    if (serial_ == nullptr) {
      return false;
    }

    while (serial_->available() > 0) {
      const uint8_t value = static_cast<uint8_t>(serial_->read());
      if (frameLength_ == 0 && value != 0xFF) {
        continue;
      }

      frame_[frameLength_++] = value;
      if (frameLength_ == sizeof(frame_)) {
        frameLength_ = 0;
        if (isValidFrame()) {
          const uint16_t reading =
              (static_cast<uint16_t>(frame_[2]) << 8) | frame_[3];
          if (reading > 0) {
            ppm = reading;
            return true;
          }
        }
      }
    }
    return false;
  }

 private:
  void disableAbc() {
    static const uint8_t command[] = {0xFF, 0x01, 0x79, 0, 0, 0, 0, 0, 0x86};
    if (serial_ != nullptr) {
      serial_->write(command, sizeof(command));
    }
  }

  bool isValidFrame() const {
    if (frame_[0] != 0xFF || frame_[1] != 0x86) {
      return false;
    }
    uint8_t sum = 0;
    for (size_t i = 1; i < 8; ++i) {
      sum = static_cast<uint8_t>(sum + frame_[i]);
    }
    return static_cast<uint8_t>(0xFF - sum + 1) == frame_[8];
  }

  HardwareSerial* serial_ = nullptr;
  uint8_t frame_[9] = {};
  size_t frameLength_ = 0;
};

class Pms5003Driver {
 public:
  void begin(HardwareSerial& serial) {
    serial_ = &serial;
  }

  bool poll(uint16_t& pm1, uint16_t& pm25, uint16_t& pm10) {
    if (serial_ == nullptr) {
      return false;
    }

    while (serial_->available() > 0) {
      const uint8_t value = static_cast<uint8_t>(serial_->read());
      if (frameLength_ == 0 && value != 0x42) {
        continue;
      }
      if (frameLength_ == 1 && value != 0x4D) {
        frameLength_ = value == 0x42 ? 1 : 0;
        continue;
      }

      frame_[frameLength_++] = value;
      if (frameLength_ == sizeof(frame_)) {
        frameLength_ = 0;
        if (!isValidFrame()) {
          continue;
        }

        pm1 = readBigEndian(10);
        pm25 = readBigEndian(12);
        pm10 = readBigEndian(14);
        return true;
      }
    }
    return false;
  }

 private:
  uint16_t readBigEndian(size_t offset) const {
    return (static_cast<uint16_t>(frame_[offset]) << 8) | frame_[offset + 1];
  }

  bool isValidFrame() const {
    if (frame_[0] != 0x42 || frame_[1] != 0x4D ||
        readBigEndian(2) != 28) {
      return false;
    }
    uint16_t sum = 0;
    for (size_t i = 0; i < sizeof(frame_) - 2; ++i) {
      sum = static_cast<uint16_t>(sum + frame_[i]);
    }
    return sum == readBigEndian(sizeof(frame_) - 2);
  }

  HardwareSerial* serial_ = nullptr;
  uint8_t frame_[32] = {};
  size_t frameLength_ = 0;
};

class Sht35Driver {
 public:
  bool begin() {
    Wire.begin(AirPulseConfig::I2C_SDA_PIN, AirPulseConfig::I2C_SCL_PIN);
    Wire.setTimeOut(50);
    return true;
  }

  bool request(uint32_t nowMs) {
    Wire.beginTransmission(AirPulseConfig::SHT35_ADDRESS);
    Wire.write(0x24);
    Wire.write(0x00);
    if (Wire.endTransmission() != 0) {
      conversionPending_ = false;
      return false;
    }
    conversionReadyAtMs_ = nowMs + 16;
    conversionPending_ = true;
    return true;
  }

  bool poll(uint32_t nowMs, float& temperatureC, float& humidityPercent,
            bool& conversionCompleted) {
    conversionCompleted = false;
    if (!conversionPending_ ||
        static_cast<int32_t>(nowMs - conversionReadyAtMs_) < 0) {
      return false;
    }
    conversionPending_ = false;
    conversionCompleted = true;

    if (Wire.requestFrom(static_cast<uint8_t>(AirPulseConfig::SHT35_ADDRESS),
                         static_cast<uint8_t>(6)) != 6) {
      return false;
    }

    uint8_t data[6];
    for (uint8_t& value : data) {
      value = static_cast<uint8_t>(Wire.read());
    }
    if (crc8(data, 2) != data[2] || crc8(data + 3, 2) != data[5]) {
      return false;
    }

    const uint16_t rawTemperature =
        (static_cast<uint16_t>(data[0]) << 8) | data[1];
    const uint16_t rawHumidity =
        (static_cast<uint16_t>(data[3]) << 8) | data[4];
    temperatureC = -45.0F + (175.0F * rawTemperature / 65535.0F);
    humidityPercent = 100.0F * rawHumidity / 65535.0F;
    return temperatureC >= -40.0F && temperatureC <= 125.0F &&
           humidityPercent >= 0.0F && humidityPercent <= 100.0F;
  }

 private:
  uint8_t crc8(const uint8_t* data, size_t length) const {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < length; ++i) {
      crc ^= data[i];
      for (uint8_t bit = 0; bit < 8; ++bit) {
        crc = (crc & 0x80) != 0
                  ? static_cast<uint8_t>((crc << 1) ^ 0x31)
                  : static_cast<uint8_t>(crc << 1);
      }
    }
    return crc;
  }

  uint32_t conversionReadyAtMs_ = 0;
  bool conversionPending_ = false;
};

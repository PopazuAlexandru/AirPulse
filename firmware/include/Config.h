#pragma once

#include <Arduino.h>

namespace AirPulseConfig {
constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint32_t SENSOR_BAUD = 9600;
constexpr int CO2_RX_PIN = 16;
constexpr int CO2_TX_PIN = 17;
constexpr int PM_RX_PIN = 18;
constexpr int PM_TX_PIN = 19;
constexpr int I2C_SDA_PIN = 21;
constexpr int I2C_SCL_PIN = 22;
constexpr uint8_t SHT35_ADDRESS = 0x44;
constexpr uint8_t OLED_ADDRESS = 0x3C;
constexpr uint32_t ACQUISITION_INTERVAL_MS = 3000;
constexpr uint32_t CO2_WARMUP_MS = 180000;
constexpr uint32_t PM_WARMUP_MS = 30000;
constexpr uint32_t CO2_RESPONSE_GRACE_MS = 250;
constexpr size_t OFFLINE_BUFFER_MAX_LINES = 500;
constexpr size_t OFFLINE_FLUSH_BATCH_SIZE = 5;
constexpr char SERIAL_NUMBER[] = "esp32:30pin_01";
constexpr char DEFAULT_AI_SERVER_URL[] =
    "http://192.168.1.100:8000/telemetry";
constexpr char DEFAULT_API_TOKEN[] = "change-me";
constexpr char WIFI_AP_NAME[] = "AirPulse-Setup";
}  // namespace AirPulseConfig

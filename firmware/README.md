# AirPulse ESP32 firmware

PlatformIO firmware for an ESP32 Dev Board (`esp32dev`) with MH-Z19C CO2,
PMS5003 particulate, SHT35 temperature/humidity, and SSD1306 OLED sensors.

## Wiring

| Device | Device pin | ESP32 GPIO |
| --- | --- | --- |
| MH-Z19C | TX | 16 (UART2 RX) |
| MH-Z19C | RX | 17 (UART2 TX) |
| PMS5003 | Pin 5 TXD | 18 (UART1 RX) |
| PMS5003 | Pin 4 RXD | 19 (UART1 TX) |
| SHT35 and OLED | SDA | 21 |
| SHT35 and OLED | SCL | 22 |

The I2C addresses are `0x44` for SHT35 and `0x3C` for the OLED. Ensure all
modules share ground and use appropriate 3.3 V-compatible logic.

## Build and upload

From this directory, use `pio run`, `pio run -t upload`, and `pio device
monitor`. Format the LittleFS image with `pio run -t uploadfs` when needed.
Serial output is configured for 115200 baud.

At startup the device attempts saved Wi-Fi credentials and opens the
`AirPulse-Setup` provisioning portal when needed. Set `ai_server_url` and
`api_token` there. Both values are retained in ESP32 NVS. When telemetry cannot
be delivered, JSON Lines records are retained in LittleFS at `/buffer.jsonl`
with a 500-record cap; queued records are sent before live samples resume.
LittleFS is formatted automatically on mount failure, which can discard an
unreadable/corrupt queue.

MH-Z19C UART frames and PMS5003 32-byte frames are validated in the firmware.
The SHT35 single-shot I2C readings use the sensor CRC. The custom partition
table provides a 2 MiB application slot and 1.875 MiB LittleFS area (without
an OTA application slot). The first valid UART readings are displayed without
blocking for sensor warmup; a sensor that stops producing valid readings for
7 seconds is reported as `ERR: SENSOR OFF`, while the remaining sensors and
telemetry continue running. Sampling and OLED refresh use `millis()` timers;
HTTP delivery runs in a separate FreeRTOS task, and the WiFiManager portal is
serviced non-blockingly so both can continue during provisioning.

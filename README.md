# SparkFun MicroMod ESP32 Temperature/Humidity RTC Logger

This Arduino sketch is set up for a SparkFun MicroMod ESP32 Processor on the SparkFun MicroMod Data Logging Carrier Board. It hosts a webpage on the ESP32, shows the latest temperature, humidity, RTC time, recent logged readings, a CSV download, and a button that synchronises the external RTC to the local time of the phone/laptop viewing the page.

If a microSD card is fitted in the Data Logging Carrier, one-minute average readings are appended to `/temp_humidity_1min.csv`. If no card is fitted, the page still shows recent average rows stored in RAM.

## Hardware

- SparkFun MicroMod ESP32 Processor
- SparkFun MicroMod Data Logging Carrier Board
- Adafruit DS3231 Precision RTC - STEMMA QT
- I2C SHT30 temperature and humidity sensor
- Optional microSD card for persistent CSV logging

## Connections

Use the Qwiic/STEMMA QT connectors:

- Plug the DS3231 STEMMA QT board into one Qwiic connector on the carrier.
- Plug the SHT31 board into the other Qwiic connector, or daisy-chain it from the DS3231 if your SHT31 breakout has two connectors.
- Insert a FAT32-formatted microSD card into the Data Logging Carrier if you want persistent CSV logging.
- The Data Logging Carrier has controllable Qwiic/peripheral power rails. If the Serial Monitor says the SHT31 or DS3231 is not found, check the carrier board's power-enable jumper or example code for your board revision.

The sketch uses the SparkFun MicroMod ESP32 primary bus mapping:

| Function | ESP32 GPIO |
| --- | --- |
| I2C SDA | GPIO 21 |
| I2C SCL | GPIO 22 |
| SPI CS for microSD | GPIO 5 |

Most SHT31 boards use I2C address `0x44`; the DS3231 uses `0x68`.

## Arduino libraries

Install these from Arduino IDE Library Manager:

- `Adafruit SHT31 Library`
- `RTClib` by Adafruit

For ESP32 board support, install `esp32` by Espressif Systems in Boards Manager. Select the SparkFun MicroMod ESP32 board if your board package provides it; otherwise use a compatible ESP32 board target.

## Wi-Fi setup

By default the ESP32 creates its own access point:

- SSID: `ESP32-Temp-Logger`
- Password: `12345678`
- Page: `http://192.168.4.1`

To connect it to your normal Wi-Fi instead, edit these lines near the top of the sketch:

```cpp
const char *WIFI_SSID = "";
const char *WIFI_PASSWORD = "";
```

After upload, open the Serial Monitor at `115200` baud to see the IP address.

## Notes

- With a microSD card inserted, one-minute average readings persist in `/temp_humidity_1min.csv`.
- Without a microSD card, the logger stores recent average rows in RAM, so they reset when the ESP32 restarts.
- The sensor is sampled every 5 seconds and logged as one average row every minute.
- Each CSV row includes average, minimum, maximum, and sample count for temperature and humidity.
- The RTC sync button sets the DS3231 to the local date/time of the browser device. Make sure the phone or laptop time is correct before pressing it.
- If you are using a different I2C temperature/humidity sensor, the web and RTC logic can stay the same, but the sensor library and read calls will need changing.

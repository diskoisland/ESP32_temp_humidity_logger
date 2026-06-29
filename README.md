# ESP32 Temperature/Humidity Logger

Arduino firmware for a SparkFun MicroMod ESP32 temperature and humidity logger with:

- Adafruit SHT-30 weather-proof temperature/RH sensor
- Adafruit DS3231 Precision RTC
- microSD CSV logging
- hosted webpage for live readings, RTC sync, and CSV download
- watchdog and safe scheduled reboot
- NimBLE control for turning Wi-Fi on and off

The logger boots with **Wi-Fi off** to reduce power use and radio activity. A BLE command is used to turn the Wi-Fi access point on when the webpage is needed.

## Hardware

- SparkFun MicroMod ESP32 Processor
- SparkFun MicroMod Data Logging Carrier Board
- Adafruit SHT-30 mesh-protected weather-proof temperature/humidity sensor, Product ID 4099
- Plastic solar radiation shield for the SHT-30 sensor
- Adafruit DS3231 Precision RTC - STEMMA QT
- microSD card, FAT32 formatted, for persistent CSV logging

## Sensor and RTC connections

Use the Qwiic/STEMMA QT I2C connectors.

- Connect the DS3231 RTC to the Qwiic/STEMMA QT I2C bus.
- Connect the SHT-30 sensor to the same I2C bus.
- Insert a FAT32-formatted microSD card into the Data Logging Carrier if persistent logging is required.

The sketch uses the SparkFun MicroMod ESP32 primary bus mapping:

| Function | ESP32 GPIO |
| --- | --- |
| I2C SDA | GPIO 21 |
| I2C SCL | GPIO 22 |
| SPI CS for microSD | GPIO 5 |

I2C addresses:

| Device | Address |
| --- | --- |
| SHT-30 temperature/RH sensor | `0x44` |
| DS3231 RTC | `0x68` |

The SHT-30 and DS3231 can share the same I2C bus because their addresses are different.

## Arduino libraries

Install these from Arduino IDE Library Manager:

- `Adafruit SHT31 Library`
- `RTClib` by Adafruit
- `NimBLE-Arduino` by h2zero

For ESP32 board support, install `esp32` by Espressif Systems in Boards Manager.

Select the SparkFun MicroMod ESP32 board if available. Otherwise, use a compatible ESP32 board target.

Because the firmware uses Wi-Fi, web server, SD, RTC, and NimBLE, the compiled sketch may exceed the default ESP32 app partition size. In Arduino IDE, select a larger partition scheme such as:

```text
Tools → Partition Scheme → Huge APP / No OTA
```

The SparkFun MicroMod ESP32 Processor has enough flash for this firmware; the larger partition is needed because the default app slot may be too small.

## Secrets setup

The real Wi-Fi access point name and password are stored in `Secrets.h`.

Do not commit `Secrets.h` to Git.

Copy:

```text
Secrets.example.h
```

to:

```text
Secrets.h
```

Then edit the values:

```cpp
const char *AP_SSID = "SHT30-Logger";
const char *AP_PASSWORD = "change-this-password";
```

`Secrets.h` is ignored by Git.

## BLE control

The logger advertises as:

```text
SHT30-Logger
```

Use a BLE app such as **nRF Connect** to connect to the device.

Supported BLE commands:

| Command | Purpose |
| --- | --- |
| `STATUS` | Returns logger status |
| `WIFI_ON` | Starts the Wi-Fi access point and webpage |
| `WIFI_OFF` | Stops the Wi-Fi access point |

Typical BLE test sequence:

```text
STATUS
WIFI_ON
STATUS
WIFI_OFF
STATUS
```

Example status response:

```text
STATUS wifi=off, logging=off, sensor=ok, rtc=ok, sd=ok, ip=none
```

After `WIFI_ON`, the response should include the access point IP address, usually:

```text
OK WIFI_ON ip=192.168.4.1
```

## Webpage

After sending the BLE command:

```text
WIFI_ON
```

connect your phone or laptop to the logger Wi-Fi access point.

Then open:

```text
http://192.168.4.1
```

The webpage shows:

- latest temperature
- latest humidity
- RTC time
- latest reading time
- microSD status
- logging state
- SHT-30 status
- RTC battery/time status
- RTC timezone
- recent one-minute averaged readings

The webpage also provides buttons to:

- sync the RTC to the browser device time
- start logging
- stop logging
- download the CSV log

## RTC sync

The DS3231 RTC provides timestamps for logged data.

Before starting logging:

1. Turn Wi-Fi on with BLE command `WIFI_ON`.
2. Connect to the logger webpage.
3. Press **Sync RTC to this device**.
4. Confirm that the RTC time shown on the page is correct.
5. Start logging.

The RTC sync uses the local date, time, timezone, and UTC offset from the phone or laptop viewing the webpage. Make sure that device has the correct time before syncing.

If the RTC loses power or reports an invalid time, logging is blocked until the RTC is synced again.

## Logging behavior

The sensor is sampled every 5 seconds.

Data are averaged and logged once per minute.

Each CSV row includes:

- timestamp
- timezone
- UTC offset
- average temperature
- minimum temperature
- maximum temperature
- average humidity
- minimum humidity
- maximum humidity
- sample count

With a microSD card inserted, one-minute average readings are appended to:

```text
/temp_humidity_1min.csv
```

If no microSD card is fitted, the webpage still shows recent averaged readings stored in RAM. RAM-stored readings are lost when the ESP32 restarts.

## CSV columns

```text
timestamp,timezone,utc_offset_minutes,avg_temperature_c,min_temperature_c,max_temperature_c,avg_humidity_percent,min_humidity_percent,max_humidity_percent,sample_count
```

## Watchdog and scheduled reboot

The firmware enables a watchdog timer.

The logger also performs a scheduled reboot after approximately 7 days. If logging is active, the reboot waits until just after a completed log row so that the current one-minute average is not interrupted.

## Solar shield placement

The SHT-30 sensor should be mounted inside a plastic solar radiation shield.

Good placement:

- open airflow
- away from walls, pavement, roofs, vehicles, and other heat sources
- shield mounted upright
- vents clear
- sensor mesh tip inside the shield
- sensor not pressed against plastic

Suggested field note:

```text
SHT-30 sensor mounted in plastic solar shield, approximately 1.5 m above grass, shaded from nearby buildings, open airflow.
```

## Troubleshooting

### BLE device does not appear

- Confirm the Serial Monitor shows `NimBLE started`.
- Confirm the device is powered.
- Restart the BLE app scan.
- Power-cycle the logger.

### Wi-Fi does not start

Send:

```text
WIFI_ON
```

Expected response:

```text
OK WIFI_ON ip=192.168.4.1
```

If the response is:

```text
ERROR WIFI_ON failed
```

check the Serial Monitor for Wi-Fi errors.

### Webpage does not load

- Confirm BLE command `WIFI_ON` succeeded.
- Connect to the logger Wi-Fi AP.
- Open `http://192.168.4.1`.
- Try disabling mobile data temporarily if using a phone.

### SHT-30 not found

- Check the Qwiic/STEMMA QT cable.
- Confirm the sensor is on address `0x44`.
- Confirm I2C SDA/SCL are GPIO 21/22.
- Check that the carrier board peripheral/Qwiic power rail is enabled.

### RTC not found

- Check the Qwiic/STEMMA QT cable.
- Confirm the DS3231 is on address `0x68`.
- Check the RTC battery if the time resets after power loss.

### microSD not found

- Confirm the card is inserted.
- Format the card as FAT32.
- Confirm the chip select pin is GPIO 5.
- Try a different microSD card.

## Repository files

| File | Purpose |
| --- | --- |
| `ESP32_TempHumidity_RTC_Logger_ble.ino` | Main Arduino sketch |
| `BleControl.h` / `BleControl.cpp` | NimBLE command interface |
| `WifiControl.h` / `WifiControl.cpp` | Wi-Fi access point control |
| `Secrets.example.h` | Template for local Wi-Fi AP settings |
| `Secrets.h` | Local secrets file, not committed |
| `README.md` | Project documentation |

## License

This project is licensed under the MIT License. See `LICENSE` for details.

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

A BLE client that stays connected but sends no command for 5 minutes is disconnected automatically. This prevents a phone that silently holds the connection (for example a backgrounded BLE app) from keeping the logger invisible to other scanners — while a client is connected, the device does not advertise. Advertising also restarts automatically after unclean disconnects, so the logger never needs a reboot to become discoverable again.

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
- free heap and largest free block (memory-health indicator)
- last restart reason (e.g. `poweron`, `brownout`, `sw_restart`, `task_watchdog`)
- current log file name and size
- SD card free space
- site/deployment ID
- RTC (enclosure) temperature
- recent one-minute averaged readings

When a reading is valid but the RTC is not set or has lost power, the latest-reading field shows **Live reading (RTC not set)** instead of a timestamp, so live temperature and humidity are still displayed while the clock needs attention.

The webpage also provides buttons to:

- sync the RTC to the browser device time
- start logging
- stop logging
- download the measurement CSV log
- download the event log
- start a new log file (archives the current one)
- edit configuration: site ID, calibration offsets, daily rotation, sensor heater cycling

## RTC sync

The DS3231 RTC provides timestamps for logged data.

Before starting logging:

1. Turn Wi-Fi on with BLE command `WIFI_ON`.
2. Connect to the logger webpage.
3. Press **Sync RTC to this device**.
4. Confirm that the RTC time shown on the page is correct.
5. Start logging.

The RTC sync uses the local date, time, timezone, and UTC offset from the phone or laptop viewing the webpage. Make sure that device has the correct time before syncing.

Each sync also measures clock drift: the difference between the browser time and the RTC just before adjustment is recorded in the `rtc_sync` event (`driftSeconds`, positive = RTC was behind), along with the time since the previous sync. Over repeated syncs this characterizes each unit's drift rate.

If the RTC loses power or reports an invalid time, logging is blocked until the RTC is synced again.

## Logging behavior

The sensor is sampled every 5 seconds.

Data are averaged and logged once per minute.

Each CSV row includes:

- timestamp
- timezone
- UTC offset
- site/deployment ID
- average temperature
- minimum temperature
- maximum temperature
- average humidity
- minimum humidity
- maximum humidity
- RTC (enclosure) temperature
- sample count

With a microSD card inserted, one-minute average readings are appended to:

```text
/temp_humidity_1min.csv
```

### Starting a new log file

The webpage's **New log file** button archives the current measurement CSV and starts a fresh one — useful at the start of a new deployment or site visit. The previous file is renamed on the card using the RTC time, for example:

```text
/temp_humidity_1min_20260701_143005.csv
```

(with no clock available, a numbered `_oldN` name is used instead). Archived files stay on the microSD card and can be retrieved by reading the card in a computer; the download button always serves the current file. The rotation is recorded as a `log_file_rotated` event, and the recent-readings table on the webpage is cleared so it reflects only the new file.

With **Daily file rotation** enabled in the configuration form, the same rotation happens automatically at the first logged row after each midnight, so files map one-to-one to calendar days.

### Configuration

The webpage's configuration form sets, persisted across reboots:

- **Site ID** — a deployment/site label written into every CSV row (`site_id` column), so data from multiple units or sites stays attributable.
- **Temperature / RH calibration offsets** — per-unit offsets applied to readings at sample time. Every change is recorded as a `config_changed` event with the values, so logged data remains traceable to the calibration in effect.
- **Daily file rotation** — see above.
- **Sensor heater cycling** — every 30 minutes the SHT-30's built-in heater runs for 30 seconds to drive off condensation, which otherwise pins RH near 100% and drifts the sensor in outdoor use. Sampling is suspended during the pulse and for a 90-second settle period, so heated readings never enter the averages (visible as a slightly lower `sample_count` in those minutes). Enabled by default; disable it for indoor use if preferred.

### microSD auto-remount

If the SD card fails or is removed, the logger keeps running (RAM-only) and retries mounting every 30 seconds. Re-seating or swapping the card in the field works without a reboot; recovery is recorded as an `sd_remounted` event.

If no microSD card is fitted, the webpage still shows recent averaged readings stored in RAM. RAM-stored readings are lost when the ESP32 restarts.

Logger events (boot, logging start/stop, Wi-Fi on/off, sensor/SD errors, scheduled reboots) are recorded separately in:

```text
/logger_events.csv
```

Each `boot` event records why the ESP32 last restarted in its `reason` column, from the chip's reset cause:

| Reason | Meaning |
| --- | --- |
| `poweron` | Cold power-up |
| `brownout` | Supply voltage dipped below the safe threshold — a power problem |
| `sw_restart` | Normal scheduled reboot or firmware-requested restart |
| `task_watchdog` / `int_watchdog` | A hang tripped the watchdog |
| `panic` | Firmware crash |
| `external_pin` | Reset via the reset pin |

The same value is shown on the webpage as **Last restart** and returned in `/api/status` as `bootResetReason`. A run of `brownout` restarts points at an undervoltage or marginal power supply rather than a firmware issue.

### Resuming logging after a reboot

When logging is started, the choice is saved to the ESP32's non-volatile storage. After any reboot — the scheduled 7-day reboot, a watchdog reset, or a power cycle — the logger automatically resumes logging, provided the RTC is present, was previously synced, and has not lost power. Resumed logging is recorded as a `logging_start / auto_resume` event.

A manual **Stop logging** is also saved, so a stopped logger stays stopped after a reboot. Logging only resumes automatically if it was active when the reboot occurred.

Two things do not carry across a reboot: the recent-readings table shown on the webpage (RAM only) and the partially accumulated current minute. The on-SD CSV file is appended to, so the logged record itself is continuous. If the RTC coin cell has died and the clock is lost, auto-resume is held off until the RTC is synced again, to avoid logging with an unreliable timestamp.

## CSV columns

```text
timestamp,timezone,utc_offset_minutes,site_id,avg_temperature_c,min_temperature_c,max_temperature_c,avg_humidity_percent,min_humidity_percent,max_humidity_percent,rtc_temperature_c,sample_count
```

If the log file on the card was written by an older firmware with a different column layout, it is automatically archived at boot (`log_file_rotated / schema_changed` event) and a fresh file is started, so every file is internally consistent.

## Watchdog and scheduled reboot

The firmware enables a watchdog timer.

The logger also performs a scheduled reboot after approximately 7 days. If logging is active, the reboot waits until just after a completed log row so that the current one-minute average is not interrupted. The periodic reboot also keeps long-term heap fragmentation in check.

The webpage's **Free heap** card reports free memory and the largest contiguous free block. If free heap stays high but the largest block shrinks over time, the heap is fragmenting; the scheduled reboot resets it. CSV downloads are streamed row by row rather than built in memory, so exporting a full log does not require a large allocation.

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

## Planned upgrades

Ideas noted for future work, roughly in priority order:

- **Web file browser for archived logs** — list the archived `temp_humidity_1min_*.csv` files on the microSD card and allow downloading (and possibly deleting) them from the webpage, so old deployment segments can be retrieved without pulling the card.
- **Battery / supply voltage sensing** — read the input rail so the logger can report battery state and warn before brownout. Needs a check of the Data Logging Carrier schematic for an ADC-accessible VIN/battery sense pin (may need a resistor divider).
- **Reload recent readings from SD at boot** — repopulate the webpage's recent-readings table from the tail of the current CSV after a reboot, so the display is continuous across the 7-day restart.
- **Calendar-valid RTC sync check** — reject impossible dates (e.g. February 31) in `/api/sync` instead of relying on per-field range checks.

## License

This project is licensed under the MIT License. See `LICENSE` for details.

/*
  SparkFun MicroMod ESP32 Temperature/Humidity Logger with hosted webpage,
  microSD logging, RTC sync, watchdog, safe scheduled reboot, event logging,
  and NimBLE Wi-Fi control.

  Hardware assumed:
    - SparkFun MicroMod ESP32 Processor
    - SparkFun MicroMod Data Logging Carrier Board
    - Adafruit SHT-30 mesh-protected weather-proof temperature/humidity sensor,
      Product ID 4099, mounted in a plastic solar radiation shield, I2C address 0x44
    - Adafruit DS3231 Precision RTC - STEMMA QT, address 0x68

  Arduino libraries:
    - Adafruit SHT31 Library
    - RTClib by Adafruit
    - NimBLE-Arduino by h2zero

  BLE commands:
    - WIFI_ON
    - WIFI_OFF
    - LOG_ON
    - LOG_OFF
    - STATUS

  Open the serial monitor at 115200 baud after uploading.
  In this NimBLE variant, Wi-Fi starts OFF by default.
  Use BLE command WIFI_ON to start the access point and webpage.
*/

#include "Secrets.h"
#include "BleControl.h"
#include "WifiControl.h"

#include <WebServer.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_SHT31.h>
#include <RTClib.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <esp_arduino_version.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

// -------- BLE settings --------
const char *BLE_DEVICE_NAME = "LumberjackBLE";

// -------- User settings --------
const unsigned long SAMPLE_INTERVAL_MS = 5000;      // Sensor sampling time, 5 sec
const unsigned long LOG_INTERVAL_MS = 60000;        // Logging interval, 1 minute
const unsigned long REBOOT_INTERVAL_MS = 7UL * 24UL * 60UL * 60UL * 1000UL;  // 7 days
const uint16_t MAX_LOG_ROWS = 1440;                 // 24 hours at one-minute averages
const uint32_t WATCHDOG_TIMEOUT_MS = 30000;         // 30 second watchdog

// SHT-30 heater cycling: a periodic pulse drives off condensation that can
// pin RH near 100% and drift the sensor in outdoor/high-humidity use.
// Samples are suspended during the heat pulse and the settle period after it.
const unsigned long HEATER_PERIOD_MS = 30UL * 60UL * 1000UL;  // every 30 minutes
const unsigned long HEATER_ON_MS = 30UL * 1000UL;             // heat for 30 s
const unsigned long HEATER_SETTLE_MS = 90UL * 1000UL;         // re-equilibrate 90 s

// SparkFun MicroMod ESP32 maps the primary MicroMod/Qwiic I2C bus to GPIO 21/22.
const int I2C_SDA_PIN = 21;
const int I2C_SCL_PIN = 22;

// SparkFun MicroMod ESP32 primary SPI mapping used by the Data Logging Carrier.
const int SD_CS_PIN = 5;
const char *LOG_FILENAME = "/temp_humidity_1min.csv";
const char *EVENT_LOG_FILENAME = "/logger_events.csv";

const char *LOG_CSV_HEADER =
  "timestamp,timezone,utc_offset_minutes,site_id,"
  "avg_temperature_c,min_temperature_c,max_temperature_c,"
  "avg_humidity_percent,min_humidity_percent,max_humidity_percent,"
  "rtc_temperature_c,sample_count";

const char *EVENT_CSV_HEADER =
  "timestamp,timezone,utc_offset_minutes,event,reason,details";

// -------- Globals --------
WebServer server(80);
Adafruit_SHT31 sht31 = Adafruit_SHT31();
RTC_DS3231 rtc;
Preferences prefs;

struct LogRow {
  DateTime timestamp;
  int16_t utcOffsetMinutes;  // UTC offset in effect when this row was logged
  float avgTemperatureC;
  float minTemperatureC;
  float maxTemperatureC;
  float avgHumidity;
  float minHumidity;
  float maxHumidity;
  float rtcTemperatureC;  // DS3231 die temperature = enclosure temperature
  uint16_t sampleCount;
  bool valid;
};

LogRow logRows[MAX_LOG_ROWS];
uint16_t logWriteIndex = 0;
uint16_t logCount = 0;

unsigned long lastSampleMillis = 0;
unsigned long lastLogMillis = 0;
unsigned long bootMillis = 0;

float latestTemperatureC = NAN;
float latestHumidity = NAN;
DateTime latestTimestamp;
bool latestValid = false;

bool sensorOk = false;
bool sdOk = false;
bool loggingEnabled = false;
bool autoLoggingWanted = false;
bool rebootPending = false;
bool wifiOffRequested = false;  // web request to drop Wi-Fi, applied in loop()

bool rtcOk = false;
bool rtcLostPower = false;
bool rtcTimeWasBadAtBoot = false;
bool rtcBatteryConcern = false;
bool rtcWasSynced = false;
String rtcSyncTimezone = "Not synced";
int rtcSyncUtcOffsetMinutes = 0;

const char *bootResetReason = "unknown";  // cause of the most recent restart

// User configuration, persisted in Preferences and settable via the web page.
String siteId = "";                 // deployment/site label, written per CSV row
float tempOffsetC = 0.0;            // per-unit calibration offset, applied at sample time
float humidityOffset = 0.0;
bool dailyRotateEnabled = false;    // archive the log file at each midnight boundary
bool sensorHeaterEnabled = true;    // periodic SHT-30 condensation purge
uint32_t rtcLastSyncUnix = 0;       // for drift-rate calculation between syncs

// SHT-30 heater state machine (see manageSensorHeater()).
bool heaterOn = false;
bool heaterSettling = false;
unsigned long heaterOnStartMillis = 0;
unsigned long heaterSettleStartMillis = 0;
unsigned long heaterLastCycleEndMillis = 0;

int lastLogDay = -1;  // calendar day of the last logged row, for daily rotation

// -------- Battery / power monitoring --------
// The carrier puts input-rail/3 on MicroMod BATT_VIN/3 = ESP32 GPIO39
// (ADC1_CH3, input-only, WiFi-safe). VIN = 3x the pin reading; on battery the
// cell is ~VIN + one Schottky drop (D3). Used to gracefully close the log file
// before a dying LiPo cuts off, and to show power source on the webpage.
const int BATTERY_SENSE_PIN = 39;
const uint32_t USB_PRESENT_MV = 4500;         // VIN above this => running on USB
const uint32_t BATTERY_LOW_MV = 3200;         // cell at/under this => graceful close
const uint32_t BATTERY_RECOVER_MV = 3400;     // cell must exceed this to clear the low latch
const uint32_t SCHOTTKY_DROP_MV = 280;        // D3 drop between the cell and VIN
const unsigned long POWER_CHECK_INTERVAL_MS = 5000;

uint32_t vinMillivolts = 0;
uint32_t batteryMillivolts = 0;               // 0 while on USB (cell not the source)
bool onUsbPower = true;
bool batteryLow = false;
bool loggingSuspendedLowBatt = false;         // stopped for low battery; resume on power restore
unsigned long lastPowerCheckMillis = 0;

float sumTemperatureC = 0.0;
float minTemperatureC = NAN;
float maxTemperatureC = NAN;
float sumHumidity = 0.0;
float minHumidity = NAN;
float maxHumidity = NAN;
uint16_t aggregateSampleCount = 0;

// -------- Forward declarations --------
void processBleCommand(const String &command);
String bleStatusText();
void appendEventLog(const char *event, const char *reason, const String &details);
void handleEventCsv();
bool startLogging(const char *source);
void stopLogging(const char *source);

// -------- Helper functions --------
String twoDigits(int value) {
  if (value < 10) return "0" + String(value);
  return String(value);
}

String formatDateTime(const DateTime &dt) {
  return String(dt.year()) + "-" + twoDigits(dt.month()) + "-" + twoDigits(dt.day()) +
         " " + twoDigits(dt.hour()) + ":" + twoDigits(dt.minute()) + ":" + twoDigits(dt.second());
}

String csvDateTime(const DateTime &dt) {
  return String(dt.year()) + "-" + twoDigits(dt.month()) + "-" + twoDigits(dt.day()) +
         "T" + twoDigits(dt.hour()) + ":" + twoDigits(dt.minute()) + ":" + twoDigits(dt.second());
}

String csvEscape(const String &value) {
  String escaped = value;
  escaped.replace("\"", "\"\"");

  if (escaped.indexOf(',') >= 0 ||
      escaped.indexOf('"') >= 0 ||
      escaped.indexOf('\n') >= 0 ||
      escaped.indexOf('\r') >= 0) {
    escaped = "\"" + escaped + "\"";
  }

  return escaped;
}

// A transient I2C glitch can make rtc.now() return garbage (a real example
// from the field: month 83, day 165, hour 45 — while the SHT-30 on the same
// bus returned NaN). Read with validation and one retry so a corrupt
// timestamp never enters the logs.
bool readRtcTime(DateTime &out) {
  if (!rtcOk) return false;

  for (int attempt = 0; attempt < 2; attempt++) {
    DateTime t = rtc.now();
    if (t.isValid() && t.year() >= 2020 && t.year() <= 2099) {
      out = t;
      return true;
    }
  }

  return false;
}

void appendEventLog(const char *event, const char *reason, const String &details) {
  Serial.print("Event: ");
  Serial.print(event);
  Serial.print(" / ");
  Serial.print(reason);
  Serial.print(" / ");
  Serial.println(details);

  // Event rows need a reliable timestamp. If the SD card or RTC is missing,
  // or the RTC returns an invalid time, the event is still printed to Serial
  // but not written to the event CSV.
  if (!sdOk || !rtcOk) return;

  DateTime now;
  if (!readRtcTime(now)) {
    Serial.println("Event not written to CSV: RTC returned an invalid timestamp.");
    return;
  }

  File file = SD.open(EVENT_LOG_FILENAME, FILE_APPEND);
  if (!file) {
    Serial.println("Could not open event log file.");
    sdOk = false;
    return;
  }

  if (file.size() == 0) {
    file.println(EVENT_CSV_HEADER);
  }

  file.print(csvDateTime(now));
  file.print(",");
  file.print(csvEscape(rtcSyncTimezone));
  file.print(",");
  file.print(rtcSyncUtcOffsetMinutes);
  file.print(",");
  file.print(csvEscape(String(event)));
  file.print(",");
  file.print(csvEscape(String(reason)));
  file.print(",");
  file.println(csvEscape(details));

  file.close();
}

// -------- BLE command handling --------
String bleStatusText() {
  String status = "STATUS ";
  status += "wifi=";
  status += isWifiEnabled() ? "on" : "off";
  status += ", logging=";
  status += loggingEnabled ? "on" : "off";
  status += ", sensor=";
  status += sensorOk ? "ok" : "bad";
  status += ", rtc=";
  status += rtcOk ? "ok" : "bad";
  status += ", sd=";
  status += sdOk ? "ok" : "bad";
  status += ", ip=";
  status += isWifiEnabled() ? currentIpAddress() : "none";
  return status;
}

void processBleCommand(const String &command) {
  Serial.print("BLE command received: ");
  Serial.println(command);

  if (command == "WIFI_ON") {
    startWifi();

    if (isWifiEnabled()) {
      appendEventLog("wifi_on", "ble_command", "WIFI_ON");
      sendBleResponse("OK WIFI_ON ip=" + currentIpAddress());
    } else {
      appendEventLog("wifi_on_failed", "ble_command", "WIFI_ON failed");
      sendBleResponse("ERROR WIFI_ON failed");
    }

    return;
  }

  if (command == "WIFI_OFF") {
    stopWifi();
    appendEventLog("wifi_off", "ble_command", "WIFI_OFF");
    sendBleResponse("OK WIFI_OFF");
    return;
  }

  if (command == "LOG_ON") {
    if (startLogging("ble_command")) {
      sendBleResponse("OK LOG_ON logging=on");
    } else {
      sendBleResponse("ERROR LOG_ON blocked: sync RTC and confirm it is valid first");
    }
    return;
  }

  if (command == "LOG_OFF") {
    stopLogging("ble_command");
    sendBleResponse("OK LOG_OFF logging=off");
    return;
  }

  if (command == "STATUS") {
    appendEventLog("status", "ble_command", "STATUS");
    sendBleResponse(bleStatusText());
    return;
  }

  appendEventLog("unknown_ble_command", "ble_command", command);
  sendBleResponse("ERROR Unknown command. Use WIFI_ON, WIFI_OFF, LOG_ON, LOG_OFF, or STATUS.");
}

// -------- Watchdog --------
void setupWatchdog() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  // On Arduino-ESP32 3.x the Task WDT is already initialized by the core at
  // boot, so esp_task_wdt_init() would return ESP_ERR_INVALID_STATE and leave
  // the core defaults in place. Reconfigure the existing WDT instead, and only
  // fall back to init() if the core did not bring it up.
  esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = WATCHDOG_TIMEOUT_MS,
    .idle_core_mask = 0,       // watch only the loop task, not the idle tasks
    .trigger_panic = false
  };

  if (esp_task_wdt_reconfigure(&wdtConfig) == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_init(&wdtConfig);
  }
  esp_task_wdt_add(NULL);  // Add current Arduino loop task to watchdog
#else
  esp_task_wdt_init(WATCHDOG_TIMEOUT_MS / 1000, false);
  esp_task_wdt_add(NULL);
#endif

  Serial.println("Watchdog enabled.");
  appendEventLog("watchdog_setup", "setup", "watchdog enabled");
}

// -------- Persistent config --------
void saveLoggingConfig() {
  prefs.begin("logger", false);
  prefs.putBool("autoLog", autoLoggingWanted);
  prefs.putBool("rtcSynced", rtcWasSynced);
  prefs.putString("tz", rtcSyncTimezone);
  prefs.putInt("utcOffset", rtcSyncUtcOffsetMinutes);
  prefs.putString("siteId", siteId);
  prefs.putFloat("tOffset", tempOffsetC);
  prefs.putFloat("hOffset", humidityOffset);
  prefs.putBool("dailyRot", dailyRotateEnabled);
  prefs.putBool("heater", sensorHeaterEnabled);
  prefs.putULong("lastSync", rtcLastSyncUnix);
  prefs.end();
}

void loadLoggingConfig() {
  prefs.begin("logger", true);
  autoLoggingWanted = prefs.getBool("autoLog", false);
  rtcWasSynced = prefs.getBool("rtcSynced", false);
  rtcSyncTimezone = prefs.getString("tz", "Not synced");
  rtcSyncUtcOffsetMinutes = prefs.getInt("utcOffset", 0);
  siteId = prefs.getString("siteId", "");
  tempOffsetC = prefs.getFloat("tOffset", 0.0);
  humidityOffset = prefs.getFloat("hOffset", 0.0);
  dailyRotateEnabled = prefs.getBool("dailyRot", false);
  sensorHeaterEnabled = prefs.getBool("heater", true);
  rtcLastSyncUnix = prefs.getULong("lastSync", 0);
  prefs.end();
}

// -------- Sensor aggregation --------
void resetAggregate() {
  sumTemperatureC = 0.0;
  minTemperatureC = NAN;
  maxTemperatureC = NAN;
  sumHumidity = 0.0;
  minHumidity = NAN;
  maxHumidity = NAN;
  aggregateSampleCount = 0;
}

void sampleSensor() {
  // Live readings only need the sensor. Timestamping and aggregation need the
  // RTC, but a dead RTC should not blank out the live temp/humidity display.
  if (!sensorOk) return;

  // Skip samples while the heater is on or the sensor is re-equilibrating;
  // heated readings are biased warm/dry and must not enter the aggregates.
  if (heaterOn || heaterSettling) return;

  float temperatureC = sht31.readTemperature();
  float humidity = sht31.readHumidity();

  if (isnan(temperatureC) || isnan(humidity)) {
    latestValid = false;
    appendEventLog("sensor_error", "sample_failed", "SHT-30 returned NaN");
    return;
  }

  // Apply per-unit calibration offsets (set via the web page, stored in
  // Preferences; changes are recorded in the event log for provenance).
  temperatureC += tempOffsetC;
  humidity += humidityOffset;
  if (humidity < 0.0) humidity = 0.0;
  if (humidity > 100.0) humidity = 100.0;

  latestTemperatureC = temperatureC;
  latestHumidity = humidity;
  DateTime sampleTime;
  if (readRtcTime(sampleTime)) latestTimestamp = sampleTime;  // else keep previous
  latestValid = true;

  if (loggingEnabled && rtcOk) {
    sumTemperatureC += temperatureC;
    sumHumidity += humidity;

    if (aggregateSampleCount == 0) {
      minTemperatureC = temperatureC;
      maxTemperatureC = temperatureC;
      minHumidity = humidity;
      maxHumidity = humidity;
    } else {
      minTemperatureC = min(minTemperatureC, temperatureC);
      maxTemperatureC = max(maxTemperatureC, temperatureC);
      minHumidity = min(minHumidity, humidity);
      maxHumidity = max(maxHumidity, humidity);
    }

    aggregateSampleCount++;
  }
}

void logAggregate() {
  // A pending scheduled reboot normally waits for a completed log row. But if
  // no samples are accumulating (RTC dead or sensor returning NaN), a row will
  // never complete, so don't defer the reboot forever.
  if (rebootPending && (!rtcOk || aggregateSampleCount == 0)) {
    appendEventLog(
      "scheduled_reboot",
      "seven_day_reboot",
      "rebooting without a final log row (no pending samples)"
    );

    Serial.println("Scheduled reboot with no pending samples.");
    delay(100);
    ESP.restart();
  }

  if (!rtcOk || aggregateSampleCount == 0) return;

  // Bad RTC read: keep accumulating and try again next interval, so the next
  // row is a longer average rather than one with a corrupt timestamp.
  DateTime now;
  if (!readRtcTime(now)) {
    Serial.println("Log row deferred: RTC returned an invalid timestamp.");
    return;
  }

  float avgTemperatureC = sumTemperatureC / aggregateSampleCount;
  float avgHumidity = sumHumidity / aggregateSampleCount;
  float rtcTemperatureNow = rtc.getTemperature();  // DS3231 die = enclosure temp

  // Optional daily rotation: when the calendar day changes, archive the
  // previous day's file so files map one-to-one to days. The minute that spans
  // midnight becomes the first row of the new day's file.
  if (dailyRotateEnabled && sdOk && lastLogDay >= 0 && now.day() != lastLogDay) {
    String archiveName;
    if (rotateLogFile(archiveName)) {
      appendEventLog("log_file_rotated", "daily_rotation",
                     "previous day's file archived as " + archiveName);
    }
  }
  lastLogDay = now.day();

  logRows[logWriteIndex] = {
    now,
    (int16_t)rtcSyncUtcOffsetMinutes,
    avgTemperatureC,
    minTemperatureC,
    maxTemperatureC,
    avgHumidity,
    minHumidity,
    maxHumidity,
    rtcTemperatureNow,
    aggregateSampleCount,
    true
  };

  logWriteIndex = (logWriteIndex + 1) % MAX_LOG_ROWS;
  if (logCount < MAX_LOG_ROWS) logCount++;

  if (sdOk) {
    File file = SD.open(LOG_FILENAME, FILE_APPEND);
    if (file) {
      if (file.size() == 0) {
        file.println(LOG_CSV_HEADER);
      }

      file.print(csvDateTime(now));
      file.print(",");
      file.print(csvEscape(rtcSyncTimezone));
      file.print(",");
      file.print(rtcSyncUtcOffsetMinutes);
      file.print(",");
      file.print(csvEscape(siteId));
      file.print(",");
      file.print(avgTemperatureC, 2);
      file.print(",");
      file.print(minTemperatureC, 2);
      file.print(",");
      file.print(maxTemperatureC, 2);
      file.print(",");
      file.print(avgHumidity, 2);
      file.print(",");
      file.print(minHumidity, 2);
      file.print(",");
      file.print(maxHumidity, 2);
      file.print(",");
      file.print(rtcTemperatureNow, 2);
      file.print(",");
      file.println(aggregateSampleCount);
      file.close();
    } else {
      appendEventLog("sd_error", "measurement_log_open_failed", "could not open measurement CSV");
      sdOk = false;
    }
  }

  resetAggregate();

  if (rebootPending) {
    appendEventLog(
      "scheduled_reboot",
      "seven_day_reboot",
      "rebooting after completed log row"
    );

    Serial.println("Scheduled reboot after completed log row.");
    delay(100);
    ESP.restart();
  }
}

// -------- Web handlers --------
// Shared start/stop logic so the web page and BLE both enforce the same RTC
// gating. `source` is recorded in the event log (e.g. "web_command",
// "ble_command"). startLogging returns false if the RTC precondition fails.
bool startLogging(const char *source) {
  if (!rtcOk || !rtcWasSynced || rtcBatteryConcern) {
    appendEventLog(
      "logging_blocked",
      source,
      "start requested but RTC was not synced, invalid, or battery concern was active"
    );
    return false;
  }

  loggingEnabled = true;
  autoLoggingWanted = true;
  loggingSuspendedLowBatt = false;  // manual start re-arms the low-battery auto-stop
  resetAggregate();
  lastLogMillis = millis();

  saveLoggingConfig();
  appendEventLog("logging_start", source, "logging started");
  return true;
}

void stopLogging(const char *source) {
  loggingEnabled = false;
  autoLoggingWanted = false;
  rebootPending = false;
  loggingSuspendedLowBatt = false;  // manual stop clears the suspend (no unwanted auto-resume)
  resetAggregate();

  saveLoggingConfig();
  appendEventLog("logging_stop", source, "logging stopped");
}

void handleStartLogging() {
  if (startLogging("web_command")) {
    server.send(200, "text/plain", "Logging started.");
  } else {
    server.send(400, "text/plain", "Sync RTC and confirm RTC is valid before starting logging.");
  }
}

void handleStopLogging() {
  stopLogging("web_command");
  server.send(200, "text/plain", "Logging stopped.");
}

void handleWifiOff() {
  // Reply first, then drop Wi-Fi on the next loop() tick. Calling stopWifi()
  // here would tear down the web server from inside its own request handler.
  server.send(200, "text/plain", "Turning Wi-Fi off; this page will disconnect.");
  wifiOffRequested = true;
}

String statusJson() {
  DateTime now;
  bool rtcReadOk = readRtcTime(now);

  // Heap health. largestFreeBlock / freeHeap is the fragmentation indicator:
  // when free heap stays high but the largest block shrinks, the heap is
  // fragmenting. Watch these over days of real use before optimizing further.
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t largestFreeBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

  // Current log file size; only polled while Wi-Fi is up, so the extra SD
  // open every status refresh is acceptable.
  uint32_t logFileSize = 0;
  bool logFileExists = false;
  if (sdOk && SD.exists(LOG_FILENAME)) {
    File logFile = SD.open(LOG_FILENAME, FILE_READ);
    if (logFile) {
      logFileSize = logFile.size();
      logFileExists = true;
      logFile.close();
    }
  }

  // Card capacity/free space. The FAT free-cluster scan can be slow on large
  // cards, so cache it and refresh only once a minute.
  static unsigned long lastSpaceCheckMillis = 0;
  static float sdTotalMb = 0.0;
  static float sdFreeMb = 0.0;
  if (sdOk) {
    if (lastSpaceCheckMillis == 0 || millis() - lastSpaceCheckMillis >= 60000) {
      lastSpaceCheckMillis = millis();
      uint64_t totalBytes = SD.totalBytes();
      uint64_t usedBytes = SD.usedBytes();
      sdTotalMb = totalBytes / 1048576.0;
      sdFreeMb = (totalBytes - usedBytes) / 1048576.0;
    }
  } else {
    lastSpaceCheckMillis = 0;
    sdTotalMb = 0.0;
    sdFreeMb = 0.0;
  }

  String json;
  json.reserve(2048);   // one allocation instead of many reallocs while building
  json = "{";
  json += "\"sensorOk\":";
  json += sensorOk ? "true" : "false";
  json += ",\"rtcOk\":";
  json += rtcOk ? "true" : "false";
  json += ",\"rtcLostPower\":";
  json += rtcLostPower ? "true" : "false";
  json += ",\"rtcTimeWasBadAtBoot\":";
  json += rtcTimeWasBadAtBoot ? "true" : "false";
  json += ",\"rtcBatteryConcern\":";
  json += rtcBatteryConcern ? "true" : "false";
  json += ",\"rtcWasSynced\":";
  json += rtcWasSynced ? "true" : "false";
  json += ",\"rtcSyncTimezone\":\"";
  json += rtcSyncTimezone;
  json += "\"";
  json += ",\"rtcSyncUtcOffsetMinutes\":";
  json += String(rtcSyncUtcOffsetMinutes);
  json += ",\"sdOk\":";
  json += sdOk ? "true" : "false";
  json += ",\"loggingEnabled\":";
  json += loggingEnabled ? "true" : "false";
  json += ",\"autoLoggingWanted\":";
  json += autoLoggingWanted ? "true" : "false";
  json += ",\"rebootPending\":";
  json += rebootPending ? "true" : "false";
  json += ",\"rtcTime\":\"";
  json += rtcReadOk ? formatDateTime(now)
                    : String(rtcOk ? "RTC read error" : "RTC not found");
  json += "\",\"latestValid\":";
  json += latestValid ? "true" : "false";
  json += ",\"temperatureC\":";
  json += String(latestValid ? latestTemperatureC : 0.0, 2);
  json += ",\"humidity\":";
  json += String(latestValid ? latestHumidity : 0.0, 2);
  json += ",\"latestTimestamp\":\"";
  json += latestValid ? formatDateTime(latestTimestamp) : String("No reading yet");
  json += "\",\"logCount\":";
  json += String(logCount);
  json += ",\"wifiEnabled\":";
  json += isWifiEnabled() ? "true" : "false";
  json += ",\"ip\":\"";
  json += currentIpAddress();
  json += "\"";
  json += ",\"freeHeap\":";
  json += String(freeHeap);
  json += ",\"largestFreeBlock\":";
  json += String(largestFreeBlock);
  json += ",\"bootResetReason\":\"";
  json += bootResetReason;
  json += "\"";
  json += ",\"logFileName\":\"";
  json += LOG_FILENAME;
  json += "\",\"logFileExists\":";
  json += logFileExists ? "true" : "false";
  json += ",\"logFileSize\":";
  json += String(logFileSize);
  json += ",\"sdTotalMb\":";
  json += String(sdTotalMb, 0);
  json += ",\"sdFreeMb\":";
  json += String(sdFreeMb, 0);
  json += ",\"siteId\":\"";
  json += siteId;
  json += "\",\"tempOffsetC\":";
  json += String(tempOffsetC, 2);
  json += ",\"humidityOffset\":";
  json += String(humidityOffset, 1);
  json += ",\"dailyRotate\":";
  json += dailyRotateEnabled ? "true" : "false";
  json += ",\"heaterEnabled\":";
  json += sensorHeaterEnabled ? "true" : "false";
  json += ",\"heaterActive\":";
  json += (heaterOn || heaterSettling) ? "true" : "false";
  json += ",\"rtcTemperatureC\":";
  json += String(rtcOk ? rtc.getTemperature() : 0.0, 2);
  json += ",\"powerSource\":\"";
  json += onUsbPower ? "usb" : "battery";
  json += "\",\"vinMillivolts\":";
  json += String(vinMillivolts);
  json += ",\"batteryMillivolts\":";
  json += String(batteryMillivolts);
  json += ",\"batteryLow\":";
  json += batteryLow ? "true" : "false";
  json += "}";

  return json;
}

void handleRoot() {
  static const char html[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Data Logger</title>
  <style>
    :root { color-scheme: light dark; font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; }
    body { margin: 0; background: #f5f7f8; color: #172026; }
    main { max-width: 920px; margin: 0 auto; padding: 24px; }
    h1 { margin: 0 0 18px; font-size: 30px; }
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(190px, 1fr)); gap: 12px; }
    .card { background: white; border: 1px solid #d7dee2; border-radius: 8px; padding: 16px; }
    .label { color: #52616b; font-size: 13px; }
    .value { font-size: 28px; font-weight: 700; margin-top: 4px; word-break: break-word; }
    .small { font-size: 16px; font-weight: 600; }
    .row { display: flex; gap: 10px; flex-wrap: wrap; margin: 18px 0; }

    button, a.button {
      border: 0;
      border-radius: 8px;
      padding: 11px 14px;
      background: #146c94;
      color: white;
      font-weight: 700;
      text-decoration: none;
      cursor: pointer;
      transition: background 0.2s ease;
    }

    button.secondary, a.secondary {
      background: #146c94;
    }

    button.pressed,
    button.secondary.pressed,
    a.button.pressed,
    a.secondary.pressed {
      background: #238636;
    }

    table { width: 100%; border-collapse: collapse; background: white; border: 1px solid #d7dee2; border-radius: 8px; overflow: hidden; }
    th, td { padding: 10px; border-bottom: 1px solid #e5eaed; text-align: left; }
    th { color: #52616b; font-size: 13px; }
    #message { min-height: 24px; color: #146c94; font-weight: 700; }

    .config label { display: flex; align-items: center; gap: 6px; font-size: 14px; }
    .config input[type="text"], .config input[type="number"],
    .config input[type="date"], .config select {
      padding: 8px; border: 1px solid #d7dee2; border-radius: 6px;
      background: inherit; color: inherit; max-width: 160px;
    }

    @media (prefers-color-scheme: dark) {
      body { background: #11181d; color: #edf4f7; }
      .card, table { background: #172026; border-color: #31414b; }
      th, td { border-color: #2a3740; }
      .label, th { color: #aab8c0; }
    }
  </style>
</head>
<body>
  <main>
    <h1>Data Logger</h1>

    <section class="grid">
      <div class="card"><div class="label">Temperature</div><div id="temp" class="value">--</div></div>
      <div class="card"><div class="label">Humidity</div><div id="humidity" class="value">--</div></div>
      <div class="card"><div class="label">RTC time</div><div id="rtcTime" class="value small">--</div></div>
      <div class="card"><div class="label">Latest reading</div><div id="latestTime" class="value small">--</div></div>
      <div class="card"><div class="label">microSD logging</div><div id="sdStatus" class="value small">--</div></div>
      <div class="card"><div class="label">Logging state</div><div id="loggingStatus" class="value small">--</div></div>
      <div class="card"><div class="label">SHT-30 sensor</div><div id="sensorStatus" class="value small">--</div></div>
      <div class="card"><div class="label">RTC battery</div><div id="rtcBatteryStatus" class="value small">--</div></div>
      <div class="card"><div class="label">RTC timezone</div><div id="rtcTimezone" class="value small">--</div></div>
      <div class="card"><div class="label">Free heap</div><div id="heapStatus" class="value small">--</div></div>
      <div class="card"><div class="label">Last restart</div><div id="resetReason" class="value small">--</div></div>
      <div class="card"><div class="label">Log file</div><div id="logFile" class="value small">--</div></div>
      <div class="card"><div class="label">SD space</div><div id="sdSpace" class="value small">--</div></div>
      <div class="card"><div class="label">Site ID</div><div id="siteIdView" class="value small">--</div></div>
      <div class="card"><div class="label">RTC temp (enclosure)</div><div id="rtcTemp" class="value small">--</div></div>
      <div class="card"><div class="label">Power</div><div id="powerStatus" class="value small">--</div></div>
    </section>

    <div class="row">
      <button onclick="flashButton(this); syncRtc()">Sync RTC to this device</button>
      <button onclick="flashButton(this); startLogging()">Start logging</button>
      <button onclick="flashButton(this); stopLogging()">Stop logging</button>
      <a class="button" href="/log.csv" onclick="flashButton(this)">Download CSV</a>
      <a class="button" href="/events.csv" onclick="flashButton(this)">Download Events</a>
      <button onclick="flashButton(this); newLogFile()">New log file</button>
      <button class="secondary" onclick="flashButton(this); wifiOff()">Turn off Wi-Fi</button>
    </div>

    <div id="message"></div>

    <div class="card config">
      <div class="label">Configuration</div>
      <div class="row">
        <label>Site ID <input id="cfgSiteId" type="text" maxlength="32"></label>
        <label>Temp offset &deg;C <input id="cfgTempOffset" type="number" step="0.01"></label>
        <label>RH offset % <input id="cfgHumOffset" type="number" step="0.1"></label>
      </div>
      <div class="row">
        <label><input id="cfgDailyRotate" type="checkbox"> Daily file rotation</label>
        <label><input id="cfgHeater" type="checkbox"> Sensor heater cycling</label>
        <button onclick="flashButton(this); saveConfig()">Save configuration</button>
      </div>
    </div>

    <table>
      <thead>
        <tr>
          <th>Time</th>
          <th>Avg Temp C</th>
          <th>Avg Humidity %</th>
          <th>Samples</th>
        </tr>
      </thead>
      <tbody id="rows"></tbody>
    </table>

    <div class="card config">
      <div class="label">Files on SD card</div>
      <div class="row">
        <label>Type
          <select id="fileType" onchange="renderFiles()">
            <option value="all">All</option>
            <option value="sensor">Sensor</option>
            <option value="events">Events</option>
          </select>
        </label>
        <label>From <input id="fileFrom" type="date" onchange="renderFiles()"></label>
        <label>To <input id="fileTo" type="date" onchange="renderFiles()"></label>
        <button onclick="flashButton(this); refreshFiles()">Refresh list</button>
        <button onclick="flashButton(this); downloadMerged()">Download merged</button>
      </div>
      <table>
        <thead>
          <tr><th><input id="fileCheckAll" type="checkbox" onchange="toggleAllFiles(this)"></th><th>File</th><th>Type</th><th>Date</th><th>Size</th><th></th></tr>
        </thead>
        <tbody id="fileRows"></tbody>
      </table>
    </div>
  </main>

  <script>
    let configLoaded = false;

    function flashButton(button) {
      button.classList.add('pressed');

      setTimeout(() => {
        button.classList.remove('pressed');
      }, 2000);
    }

    async function refresh() {
      const status = await fetch('/api/status').then(r => r.json());

      document.getElementById('temp').textContent =
        status.latestValid ? `${status.temperatureC.toFixed(2)} C` : '--';

      document.getElementById('humidity').textContent =
        status.latestValid ? `${status.humidity.toFixed(2)} %` : '--';

      document.getElementById('rtcTime').textContent = status.rtcTime;

      // With no RTC we still show the live reading, but the timestamp would be
      // meaningless, so label it explicitly instead of printing a stale time.
      document.getElementById('latestTime').textContent =
        (status.latestValid && !status.rtcOk) ? 'Live reading (RTC not set)' : status.latestTimestamp;
      document.getElementById('sdStatus').textContent = status.sdOk ? 'Active' : 'No card';

      document.getElementById('sensorStatus').textContent =
        status.sensorOk
          ? (status.heaterActive ? 'OK / heater purge' : 'Found / OK')
          : 'Not found';

      document.getElementById('loggingStatus').textContent =
        status.loggingEnabled
          ? (status.rebootPending ? 'Logging / reboot pending' : 'Logging')
          : 'Stopped';

      document.getElementById('rtcBatteryStatus').textContent =
        status.rtcOk
          ? (status.rtcBatteryConcern ? 'Needs RTC sync / battery check' : 'OK')
          : 'RTC not found';

      document.getElementById('rtcTimezone').textContent =
        status.rtcWasSynced
          ? `${status.rtcSyncTimezone}, UTC${status.rtcSyncUtcOffsetMinutes >= 0 ? '+' : ''}${status.rtcSyncUtcOffsetMinutes / 60}`
          : 'Not synced';

      // Show free heap plus the largest contiguous block. If the two diverge
      // (lots of free heap but a small largest block) the heap is fragmenting.
      const freeKb = (status.freeHeap / 1024).toFixed(1);
      const largestKb = (status.largestFreeBlock / 1024).toFixed(1);
      document.getElementById('heapStatus').textContent = `${freeKb} kB (max block ${largestKb} kB)`;

      document.getElementById('resetReason').textContent = status.bootResetReason;

      document.getElementById('logFile').textContent =
        status.sdOk
          ? (status.logFileExists
              ? `${status.logFileName} (${(status.logFileSize / 1024).toFixed(1)} kB)`
              : `${status.logFileName} (missing)`)
          : 'No card';

      document.getElementById('sdSpace').textContent =
        status.sdOk ? `${status.sdFreeMb} MB free of ${status.sdTotalMb} MB` : 'No card';

      document.getElementById('siteIdView').textContent = status.siteId || 'Not set';

      document.getElementById('rtcTemp').textContent =
        status.rtcOk ? `${status.rtcTemperatureC.toFixed(2)} C` : '--';

      document.getElementById('powerStatus').textContent =
        status.powerSource === 'usb'
          ? `USB (${(status.vinMillivolts / 1000).toFixed(2)} V)`
          : `Battery ${(status.batteryMillivolts / 1000).toFixed(2)} V${status.batteryLow ? ' — LOW' : ''}`;

      // Populate the config form once, so typing is not overwritten by polling.
      if (!configLoaded) {
        configLoaded = true;
        document.getElementById('cfgSiteId').value = status.siteId;
        document.getElementById('cfgTempOffset').value = status.tempOffsetC;
        document.getElementById('cfgHumOffset').value = status.humidityOffset;
        document.getElementById('cfgDailyRotate').checked = status.dailyRotate;
        document.getElementById('cfgHeater').checked = status.heaterEnabled;
      }

      const log = await fetch('/api/log').then(r => r.json());

      document.getElementById('rows').innerHTML = log.rows.map(row =>
        `<tr><td>${row.time}</td><td>${row.avgTemperatureC.toFixed(2)}</td><td>${row.avgHumidity.toFixed(2)}</td><td>${row.sampleCount}</td></tr>`
      ).join('');
    }

    async function syncRtc() {
      const now = new Date();
      const timezone = Intl.DateTimeFormat().resolvedOptions().timeZone;
      const utcOffsetMinutes = -now.getTimezoneOffset();

      const params = new URLSearchParams({
        year: now.getFullYear(),
        month: now.getMonth() + 1,
        day: now.getDate(),
        hour: now.getHours(),
        minute: now.getMinutes(),
        second: now.getSeconds(),
        timezone: timezone,
        utcOffsetMinutes: utcOffsetMinutes
      });

      const response = await fetch('/api/sync', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: params
      });

      document.getElementById('message').textContent = await response.text();
      refresh();
    }

    async function startLogging() {
      const response = await fetch('/api/logging/start', { method: 'POST' });
      document.getElementById('message').textContent = await response.text();
      refresh();
    }

    async function stopLogging() {
      const response = await fetch('/api/logging/stop', { method: 'POST' });
      document.getElementById('message').textContent = await response.text();
      refresh();
    }

    async function saveConfig() {
      const params = new URLSearchParams({
        siteId: document.getElementById('cfgSiteId').value,
        tempOffsetC: document.getElementById('cfgTempOffset').value || '0',
        humidityOffset: document.getElementById('cfgHumOffset').value || '0',
        dailyRotate: document.getElementById('cfgDailyRotate').checked ? 'true' : 'false',
        heaterEnabled: document.getElementById('cfgHeater').checked ? 'true' : 'false'
      });

      const response = await fetch('/api/config', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: params
      });

      document.getElementById('message').textContent = await response.text();
      refresh();
    }

    async function newLogFile() {
      if (!confirm('Archive the current log file and start a new one?')) return;

      const response = await fetch('/api/logfile/new', { method: 'POST' });
      document.getElementById('message').textContent = await response.text();
      refresh();
      refreshFiles();
    }

    async function wifiOff() {
      if (!confirm('Turn off Wi-Fi? This page will disconnect. Use the BLE WIFI_ON command to bring it back.')) return;

      document.getElementById('message').textContent = 'Turning Wi-Fi off; this page will disconnect...';

      // The AP drops right after replying, so the fetch may not resolve; that
      // is expected and not an error.
      try {
        await fetch('/api/wifi/off', { method: 'POST' });
      } catch (e) {
        // Connection closed as Wi-Fi went down — intended.
      }
    }

    let allFiles = [];

    // Fetched on demand (page load, Refresh button, after rotation) rather than
    // on the 5 s poll, so we don't scan the SD directory every refresh.
    async function refreshFiles() {
      try {
        const data = await fetch('/api/files').then(r => r.json());
        allFiles = data.files || [];
        renderFiles();
      } catch (e) {
        document.getElementById('fileRows').innerHTML =
          '<tr><td colspan="6">Could not load file list.</td></tr>';
      }
    }

    // Client-side date-range filter. Files without an embedded date (the live
    // log, the event log, numbered fallbacks) always show.
    function renderFiles() {
      const type = document.getElementById('fileType').value;
      const from = document.getElementById('fileFrom').value;
      const to = document.getElementById('fileTo').value;

      const rows = allFiles.filter(f => {
        if (type !== 'all' && f.type !== type) return false;
        if (!f.date) return true;
        if (from && f.date < from) return false;
        if (to && f.date > to) return false;
        return true;
      }).sort((a, b) =>
        (b.date || '').localeCompare(a.date || '') || a.name.localeCompare(b.name)
      );

      document.getElementById('fileCheckAll').checked = false;

      document.getElementById('fileRows').innerHTML = rows.length
        ? rows.map(f =>
            `<tr><td><input type="checkbox" class="fileCheck" value="${f.name}" data-type="${f.type}"></td>` +
            `<td>${f.name}</td><td>${f.type}</td><td>${f.date || '—'}</td>` +
            `<td>${(f.size / 1024).toFixed(1)} kB</td>` +
            `<td><a class="button" href="/download?file=${encodeURIComponent(f.name)}">Download</a></td></tr>`
          ).join('')
        : '<tr><td colspan="6">No files match.</td></tr>';
    }

    function toggleAllFiles(master) {
      document.querySelectorAll('.fileCheck').forEach(c => { c.checked = master.checked; });
    }

    async function downloadMerged() {
      const checked = [...document.querySelectorAll('.fileCheck:checked')];
      if (checked.length === 0) {
        document.getElementById('message').textContent = 'Select files to merge first.';
        return;
      }

      const types = new Set(checked.map(c => c.dataset.type));
      if (types.size > 1) {
        document.getElementById('message').textContent =
          'Select files of a single type (sensor or events) to merge.';
        return;
      }

      const names = checked.map(c => c.value);
      const params = new URLSearchParams({ files: names.join(',') });

      document.getElementById('message').textContent = `Merging ${names.length} files...`;

      const response = await fetch('/download/merged', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: params
      });

      if (!response.ok) {
        document.getElementById('message').textContent = 'Merge failed: ' + await response.text();
        return;
      }

      // Stream arrives as one CSV; hand it to the browser as a download.
      const blob = await response.blob();
      const url = URL.createObjectURL(blob);
      const link = document.createElement('a');
      link.href = url;
      link.download = `merged_${[...types][0]}_${names.length}_files.csv`;
      document.body.appendChild(link);
      link.click();
      link.remove();
      URL.revokeObjectURL(url);

      document.getElementById('message').textContent = `Merged ${names.length} files.`;
    }

    refresh();
    refreshFiles();
    setInterval(refresh, 5000);
  </script>
</body>
</html>
)rawliteral";

  server.send_P(200, "text/html", html);
}

void handleStatus() {
  server.send(200, "application/json", statusJson());
}

void handleLogJson() {
  String json;
  json.reserve(4096);   // ~60 rows of JSON in one allocation, avoids realloc churn
  json = "{\"rows\":[";
  uint16_t rowsToShow = min<uint16_t>(logCount, 60);
  bool firstRow = true;

  for (uint16_t i = 0; i < rowsToShow; i++) {
    uint16_t index = (logWriteIndex + MAX_LOG_ROWS - 1 - i) % MAX_LOG_ROWS;
    if (!logRows[index].valid) continue;

    if (!firstRow) json += ",";
    firstRow = false;

    json += "{";
    json += "\"time\":\"" + formatDateTime(logRows[index].timestamp) + "\",";
    json += "\"timezone\":\"" + rtcSyncTimezone + "\",";
    json += "\"utcOffsetMinutes\":" + String(logRows[index].utcOffsetMinutes) + ",";
    json += "\"avgTemperatureC\":" + String(logRows[index].avgTemperatureC, 2) + ",";
    json += "\"minTemperatureC\":" + String(logRows[index].minTemperatureC, 2) + ",";
    json += "\"maxTemperatureC\":" + String(logRows[index].maxTemperatureC, 2) + ",";
    json += "\"avgHumidity\":" + String(logRows[index].avgHumidity, 2) + ",";
    json += "\"minHumidity\":" + String(logRows[index].minHumidity, 2) + ",";
    json += "\"maxHumidity\":" + String(logRows[index].maxHumidity, 2) + ",";
    json += "\"rtcTemperatureC\":" + String(logRows[index].rtcTemperatureC, 2) + ",";
    json += "\"sampleCount\":" + String(logRows[index].sampleCount);
    json += "}";
  }

  json += "]}";
  server.send(200, "application/json", json);
}

void handleLogCsv() {
  if (sdOk && SD.exists(LOG_FILENAME)) {
    File file = SD.open(LOG_FILENAME, FILE_READ);
    if (file) {
      server.sendHeader("Content-Disposition", "attachment; filename=esp32-temp-humidity-log.csv");
      server.streamFile(file, "text/csv");
      file.close();
      return;
    }
  }

  // No SD file: stream the in-RAM ring buffer one row at a time. Building the
  // whole CSV in a single String would be a ~130 KB allocation at full buffer,
  // which can fail outright on a fragmented heap.
  server.sendHeader("Content-Disposition", "attachment; filename=esp32-temp-humidity-log.csv");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");
  server.sendContent(String(LOG_CSV_HEADER) + "\n");

  String row;
  row.reserve(160);

  for (uint16_t i = 0; i < logCount; i++) {
    uint16_t index = (logWriteIndex + MAX_LOG_ROWS - logCount + i) % MAX_LOG_ROWS;
    if (!logRows[index].valid) continue;

    row = csvDateTime(logRows[index].timestamp);
    row += ",";
    row += csvEscape(rtcSyncTimezone);
    row += ",";
    row += String(logRows[index].utcOffsetMinutes);
    row += ",";
    row += csvEscape(siteId);
    row += ",";
    row += String(logRows[index].avgTemperatureC, 2);
    row += ",";
    row += String(logRows[index].minTemperatureC, 2);
    row += ",";
    row += String(logRows[index].maxTemperatureC, 2);
    row += ",";
    row += String(logRows[index].avgHumidity, 2);
    row += ",";
    row += String(logRows[index].minHumidity, 2);
    row += ",";
    row += String(logRows[index].maxHumidity, 2);
    row += ",";
    row += String(logRows[index].rtcTemperatureC, 2);
    row += ",";
    row += String(logRows[index].sampleCount);
    row += "\n";

    server.sendContent(row);
    esp_task_wdt_reset();   // keep long downloads from tripping the watchdog
  }

  server.sendContent("");   // terminate the chunked response
}

void handleEventCsv() {
  if (sdOk && SD.exists(EVENT_LOG_FILENAME)) {
    File file = SD.open(EVENT_LOG_FILENAME, FILE_READ);
    if (file) {
      server.sendHeader("Content-Disposition", "attachment; filename=logger-events.csv");
      server.streamFile(file, "text/csv");
      file.close();
      return;
    }
  }

  String csv = String(EVENT_CSV_HEADER) + "\n";
  server.sendHeader("Content-Disposition", "attachment; filename=logger-events.csv");
  server.send(200, "text/csv", csv);
}

// Whitelist of files the browser lists and the download route will serve: the
// measurement log and its timestamped archives, plus the event log. Anything
// else on the card is ignored.
bool isLogFileName(const String &name) {
  if (name == "logger_events.csv") return true;
  return name.startsWith("temp_humidity_1min") && name.endsWith(".csv");
}

// Category used by the file browser's type filter.
const char *logFileType(const String &name) {
  return (name == "logger_events.csv") ? "events" : "sensor";
}

// Extracts YYYY-MM-DD from an archive name (temp_humidity_1min_YYYYMMDD_...csv).
// Returns "" for the live file, the event log, and numbered fallbacks, which
// then always show regardless of the date-range filter.
String logFileDate(const String &name) {
  int marker = name.indexOf("1min_");
  if (marker < 0) return "";

  String rest = name.substring(marker + 5);
  if (rest.length() < 8) return "";
  for (int i = 0; i < 8; i++) {
    if (!isDigit(rest[i])) return "";
  }

  return rest.substring(0, 4) + "-" + rest.substring(4, 6) + "-" + rest.substring(6, 8);
}

void handleFilesJson() {
  String json;
  json.reserve(4096);
  json = "{\"files\":[";

  if (sdOk) {
    File root = SD.open("/");
    bool first = true;

    if (root && root.isDirectory()) {
      for (File entry = root.openNextFile(); entry; entry = root.openNextFile()) {
        if (!entry.isDirectory()) {
          String name = entry.name();
          int slash = name.lastIndexOf('/');
          if (slash >= 0) name = name.substring(slash + 1);  // strip any path

          if (isLogFileName(name)) {
            if (!first) json += ",";
            first = false;
            json += "{\"name\":\"" + name + "\",";
            json += "\"size\":" + String((uint32_t)entry.size()) + ",";
            json += "\"type\":\"" + String(logFileType(name)) + "\",";
            json += "\"date\":\"" + logFileDate(name) + "\"}";
          }
        }
        entry.close();
        esp_task_wdt_reset();  // dir scans on a full card shouldn't trip the dog
      }
      root.close();
    }
  }

  json += "]}";
  server.send(200, "application/json", json);
}

void handleDownloadFile() {
  if (!sdOk) {
    server.send(503, "text/plain", "No SD card.");
    return;
  }

  if (!server.hasArg("file")) {
    server.send(400, "text/plain", "Missing file name.");
    return;
  }

  String name = server.arg("file");

  // Reject path traversal and subpaths: only bare names in the SD root are
  // allowed, and only ones matching the log whitelist.
  if (name.length() == 0 ||
      name.indexOf('/') >= 0 ||
      name.indexOf('\\') >= 0 ||
      name.indexOf("..") >= 0 ||
      !isLogFileName(name)) {
    server.send(400, "text/plain", "Invalid file name.");
    return;
  }

  String path = "/" + name;
  if (!SD.exists(path)) {
    server.send(404, "text/plain", "File not found.");
    return;
  }

  File file = SD.open(path, FILE_READ);
  if (!file) {
    server.send(500, "text/plain", "Could not open file.");
    return;
  }

  server.sendHeader("Content-Disposition", "attachment; filename=" + name);
  server.streamFile(file, "text/csv");
  file.close();
}

// Validates one name from a merge selection: bare name, on the whitelist, and
// present on the card. Returns "" if ok, otherwise an error message.
String validateMergeName(const String &name) {
  if (name.length() == 0) return "empty name";
  if (name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 || name.indexOf("..") >= 0) {
    return "invalid name: " + name;
  }
  if (!isLogFileName(name)) return "not a log file: " + name;
  if (!SD.exists("/" + name)) return "not found: " + name;
  return "";
}

// Concatenates several same-type files into one CSV download: the shared header
// is emitted once, then each file's data rows (its own header line skipped).
// Streamed chunk by chunk so an arbitrarily large merge needs no big buffer.
void handleDownloadMerged() {
  if (!sdOk) {
    server.send(503, "text/plain", "No SD card.");
    return;
  }
  if (!server.hasArg("files")) {
    server.send(400, "text/plain", "No files selected.");
    return;
  }

  String list = server.arg("files");

  // Pass 1: validate everything and confirm a single common type, before any
  // response body is sent (so failures can still return an error status).
  String commonType = "";
  for (int start = 0; start <= list.length();) {
    int comma = list.indexOf(',', start);
    String name = (comma < 0) ? list.substring(start) : list.substring(start, comma);
    name.trim();

    if (name.length() > 0) {
      String err = validateMergeName(name);
      if (err.length() > 0) {
        server.send(400, "text/plain", "Merge rejected: " + err);
        return;
      }
      String t = logFileType(name);
      if (commonType.length() == 0) {
        commonType = t;
      } else if (commonType != t) {
        server.send(400, "text/plain", "Selected files must all be the same type.");
        return;
      }
    }

    if (comma < 0) break;
    start = comma + 1;
  }

  if (commonType.length() == 0) {
    server.send(400, "text/plain", "No files selected.");
    return;
  }

  server.sendHeader("Content-Disposition", "attachment; filename=merged_" + commonType + ".csv");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");
  server.sendContent(String(commonType == "events" ? EVENT_CSV_HEADER : LOG_CSV_HEADER) + "\n");

  // Pass 2: stream each file's rows, skipping its own header line.
  char buf[513];
  for (int start = 0; start <= list.length();) {
    int comma = list.indexOf(',', start);
    String name = (comma < 0) ? list.substring(start) : list.substring(start, comma);
    name.trim();

    if (name.length() > 0) {
      File file = SD.open("/" + name, FILE_READ);
      if (file) {
        file.readStringUntil('\n');  // discard the per-file header
        while (file.available()) {
          int n = file.read((uint8_t *)buf, sizeof(buf) - 1);
          if (n <= 0) break;
          buf[n] = '\0';
          server.sendContent(buf);
          esp_task_wdt_reset();  // large merges must not trip the watchdog
        }
        file.close();
      }
    }

    if (comma < 0) break;
    start = comma + 1;
  }

  server.sendContent("");  // terminate the chunked response
}

String buildArchiveName() {
  DateTime now;
  if (readRtcTime(now)) {
    return "/temp_humidity_1min_" + String(now.year()) +
           twoDigits(now.month()) + twoDigits(now.day()) + "_" +
           twoDigits(now.hour()) + twoDigits(now.minute()) +
           twoDigits(now.second()) + ".csv";
  }

  // No trustworthy clock: fall back to the first free numbered name.
  for (int i = 1; i < 1000; i++) {
    String candidate = "/temp_humidity_1min_old" + String(i) + ".csv";
    if (!SD.exists(candidate)) return candidate;
  }
  return "";
}

// Archives the current log file (if any) under a timestamped name and starts a
// fresh one at the fixed LOG_FILENAME with the current header. Clears the RAM
// ring buffer so the webpage table matches the new file. Deliberately does NOT
// touch the in-progress minute aggregate — callers decide that. Used by the
// web control, the optional daily rotation, and the boot-time schema check.
bool rotateLogFile(String &archiveNameOut) {
  archiveNameOut = "";
  if (!sdOk) return false;

  if (SD.exists(LOG_FILENAME)) {
    String archiveName = buildArchiveName();
    if (archiveName.length() == 0 || !SD.rename(LOG_FILENAME, archiveName)) {
      appendEventLog("sd_error", "log_rotate_failed", "could not archive current log file");
      return false;
    }
    archiveNameOut = archiveName;
  }

  File file = SD.open(LOG_FILENAME, FILE_WRITE);
  if (!file) {
    sdOk = false;
    return false;
  }
  file.println(LOG_CSV_HEADER);
  file.close();

  logCount = 0;
  logWriteIndex = 0;
  return true;
}

void handleNewLogFile() {
  if (!sdOk) {
    server.send(503, "text/plain", "No SD card; cannot start a new log file.");
    return;
  }

  String archiveName;
  if (!rotateLogFile(archiveName)) {
    server.send(500, "text/plain", "Could not start a new log file.");
    return;
  }

  // Discard the partial minute so the new file starts clean.
  resetAggregate();
  lastLogMillis = millis();

  appendEventLog(
    "log_file_rotated",
    "web_command",
    archiveName.length() > 0 ? ("previous file archived as " + archiveName)
                             : String("new log file started; no previous file")
  );

  server.send(200, "text/plain",
              archiveName.length() > 0
                ? ("New log file started. Previous file archived as " + archiveName)
                : String("New log file started."));
}

bool argInRange(const String &name, int minValue, int maxValue) {
  if (!server.hasArg(name)) return false;
  int value = server.arg(name).toInt();
  return value >= minValue && value <= maxValue;
}

void handleSyncRtc() {
  if (!rtcOk) {
    server.send(503, "text/plain", "RTC is not available.");
    return;
  }

  if (!argInRange("year", 2024, 2099) ||
      !argInRange("month", 1, 12) ||
      !argInRange("day", 1, 31) ||
      !argInRange("hour", 0, 23) ||
      !argInRange("minute", 0, 59) ||
      !argInRange("second", 0, 59)) {
    server.send(400, "text/plain", "Invalid time from browser.");
    return;
  }

  if (!server.hasArg("timezone") || !server.hasArg("utcOffsetMinutes")) {
    server.send(400, "text/plain", "Missing browser timezone information.");
    return;
  }

  DateTime browserTime(
    server.arg("year").toInt(),
    server.arg("month").toInt(),
    server.arg("day").toInt(),
    server.arg("hour").toInt(),
    server.arg("minute").toInt(),
    server.arg("second").toInt()
  );

  // Clock-drift measurement: compare the RTC to the browser before adjusting.
  // Positive drift = the RTC was behind. Together with the time since the last
  // sync this characterizes each unit's drift rate.
  long driftSeconds = 0;
  bool driftKnown = false;
  DateTime rtcBefore;
  if (!rtcBatteryConcern && readRtcTime(rtcBefore)) {
    driftSeconds = (long)browserTime.unixtime() - (long)rtcBefore.unixtime();
    driftKnown = true;
  }

  uint32_t secondsSinceLastSync = 0;
  if (rtcLastSyncUnix > 0 && browserTime.unixtime() > rtcLastSyncUnix) {
    secondsSinceLastSync = browserTime.unixtime() - rtcLastSyncUnix;
  }

  rtc.adjust(browserTime);
  rtcLastSyncUnix = browserTime.unixtime();

  rtcSyncTimezone = server.arg("timezone");
  rtcSyncUtcOffsetMinutes = server.arg("utcOffsetMinutes").toInt();
  rtcWasSynced = true;

  // Syncing makes the RTC usable for this session.
  // If the coin cell is actually bad, rtcLostPower will be detected again
  // after the next full power cycle.
  rtcLostPower = false;
  rtcTimeWasBadAtBoot = false;
  rtcBatteryConcern = false;

  saveLoggingConfig();
  sampleSensor();

  appendEventLog(
    "rtc_sync",
    "web_command",
    "RTC synced from browser device using timezone " + rtcSyncTimezone +
    ", driftSeconds=" + (driftKnown ? String(driftSeconds) : String("unknown")) +
    ", secondsSinceLastSync=" +
    (secondsSinceLastSync > 0 ? String(secondsSinceLastSync) : String("unknown"))
  );

  server.send(200, "text/plain", "RTC synced to this device: " + formatDateTime(browserTime));
}

void handleSetConfig() {
  if (server.hasArg("siteId")) {
    siteId = server.arg("siteId");
    siteId.trim();
    siteId.replace("\"", "");   // keep the status JSON well-formed
    siteId.replace("\\", "");
    if (siteId.length() > 32) siteId = siteId.substring(0, 32);
  }

  if (server.hasArg("tempOffsetC")) {
    tempOffsetC = constrain(server.arg("tempOffsetC").toFloat(), -10.0f, 10.0f);
  }

  if (server.hasArg("humidityOffset")) {
    humidityOffset = constrain(server.arg("humidityOffset").toFloat(), -20.0f, 20.0f);
  }

  if (server.hasArg("dailyRotate")) {
    dailyRotateEnabled = server.arg("dailyRotate") == "true";
  }

  if (server.hasArg("heaterEnabled")) {
    sensorHeaterEnabled = server.arg("heaterEnabled") == "true";
  }

  saveLoggingConfig();

  // Config changes are provenance: calibration offsets in particular must be
  // traceable, since they are applied to the values before logging.
  appendEventLog(
    "config_changed",
    "web_command",
    String("siteId=") + siteId +
    ", tempOffsetC=" + String(tempOffsetC, 2) +
    ", humidityOffset=" + String(humidityOffset, 1) +
    ", dailyRotate=" + (dailyRotateEnabled ? "true" : "false") +
    ", heater=" + (sensorHeaterEnabled ? "true" : "false")
  );

  server.send(200, "text/plain", "Configuration saved.");
}

// -------- Setup helpers --------
void setupRoutes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/log", HTTP_GET, handleLogJson);
  server.on("/log.csv", HTTP_GET, handleLogCsv);
  server.on("/events.csv", HTTP_GET, handleEventCsv);
  server.on("/api/files", HTTP_GET, handleFilesJson);
  server.on("/download", HTTP_GET, handleDownloadFile);
  server.on("/download/merged", HTTP_POST, handleDownloadMerged);
  server.on("/api/sync", HTTP_POST, handleSyncRtc);
  server.on("/api/logging/start", HTTP_POST, handleStartLogging);
  server.on("/api/logging/stop", HTTP_POST, handleStopLogging);
  server.on("/api/logfile/new", HTTP_POST, handleNewLogFile);
  server.on("/api/wifi/off", HTTP_POST, handleWifiOff);
  server.on("/api/config", HTTP_POST, handleSetConfig);

  // In the NimBLE variant, server.begin() is called only when Wi-Fi is enabled
  // inside WifiControl.cpp.
}

// If the existing log file was written with an older column layout, appending
// new-format rows would misalign the columns. Archive it and start fresh.
void verifyLogFileSchema() {
  if (!sdOk || !SD.exists(LOG_FILENAME)) return;

  File file = SD.open(LOG_FILENAME, FILE_READ);
  if (!file) return;
  String firstLine = file.readStringUntil('\n');
  file.close();
  firstLine.trim();

  if (firstLine == String(LOG_CSV_HEADER)) return;

  String archiveName;
  if (rotateLogFile(archiveName)) {
    Serial.println("Log file schema changed; archived old-format file.");
    appendEventLog("log_file_rotated", "schema_changed",
                   "old-format file archived as " + archiveName);
  }
}

void setupSdCard() {
  sdOk = SD.begin(SD_CS_PIN);
  if (!sdOk) return;

  if (!SD.exists(LOG_FILENAME)) {
    File file = SD.open(LOG_FILENAME, FILE_WRITE);
    if (file) {
      file.println(LOG_CSV_HEADER);
      file.close();
    } else {
      sdOk = false;
      return;
    }
  }

  if (!SD.exists(EVENT_LOG_FILENAME)) {
    File file = SD.open(EVENT_LOG_FILENAME, FILE_WRITE);
    if (file) {
      file.println(EVENT_CSV_HEADER);
      file.close();
    } else {
      Serial.println("Could not create event log file.");
    }
  }

  verifyLogFileSchema();
}

// Self-heal for the SD card, in the same spirit as ensureBleAdvertising():
// sdOk latches false on any failure, so without this only a reboot recovers
// from a flaky card seat or a card swapped in the field.
void ensureSdCard() {
  static unsigned long lastAttemptMillis = 0;

  if (sdOk) return;
  if (millis() - lastAttemptMillis < 30000) return;
  lastAttemptMillis = millis();

  SD.end();
  setupSdCard();

  if (sdOk) {
    Serial.println("microSD card remounted.");
    appendEventLog("sd_remounted", "auto_remount", "microSD card available again");
  }
}

// SHT-30 heater state machine: every HEATER_PERIOD_MS, heat for HEATER_ON_MS,
// then wait HEATER_SETTLE_MS before sampling resumes (sampleSensor() skips
// readings while heaterOn or heaterSettling).
void manageSensorHeater() {
  if (!sensorOk) return;

  if (!sensorHeaterEnabled) {
    if (heaterOn || heaterSettling) {
      sht31.heater(false);
      heaterOn = false;
      heaterSettling = false;
      heaterLastCycleEndMillis = millis();
    }
    return;
  }

  if (heaterOn) {
    if (millis() - heaterOnStartMillis >= HEATER_ON_MS) {
      sht31.heater(false);
      heaterOn = false;
      heaterSettling = true;
      heaterSettleStartMillis = millis();
      Serial.println("SHT-30 heater off; settling before sampling resumes.");
    }
    return;
  }

  if (heaterSettling) {
    if (millis() - heaterSettleStartMillis >= HEATER_SETTLE_MS) {
      heaterSettling = false;
      heaterLastCycleEndMillis = millis();
    }
    return;
  }

  if (millis() - heaterLastCycleEndMillis >= HEATER_PERIOD_MS) {
    sht31.heater(true);
    heaterOn = true;
    heaterOnStartMillis = millis();
    Serial.println("SHT-30 heater on (condensation purge).");
  }
}

// Reads BATT_VIN/3 on GPIO39, updates the power state, and on low battery
// gracefully closes the log (flushes the final partial minute, stops writing)
// so a dying LiPo can't corrupt a file mid-write. Resumes automatically if
// external power returns before the cell dies.
void checkPowerStatus() {
  if (millis() - lastPowerCheckMillis < POWER_CHECK_INTERVAL_MS) return;
  lastPowerCheckMillis = millis();

  // Average several samples; the 20k series resistor on GPIO39 makes a single
  // ADC read a touch noisy. analogReadMilliVolts applies eFuse calibration.
  uint32_t acc = 0;
  const int samples = 16;
  for (int i = 0; i < samples; i++) acc += analogReadMilliVolts(BATTERY_SENSE_PIN);

  vinMillivolts = (acc / samples) * 3;  // carrier divides the input rail by 3
  onUsbPower = vinMillivolts > USB_PRESENT_MV;
  batteryMillivolts = onUsbPower ? 0 : (vinMillivolts + SCHOTTKY_DROP_MV);

  // Latch low-battery with hysteresis so it can't chatter around the threshold.
  if (onUsbPower) {
    batteryLow = false;
  } else if (!batteryLow && batteryMillivolts <= BATTERY_LOW_MV) {
    batteryLow = true;
  } else if (batteryLow && batteryMillivolts >= BATTERY_RECOVER_MV) {
    batteryLow = false;
  }

  if (batteryLow && loggingEnabled && !loggingSuspendedLowBatt) {
    // Flush whatever partial minute we have, then stop writing. autoLoggingWanted
    // is deliberately left set, so a reboot after recharge auto-resumes.
    loggingSuspendedLowBatt = true;
    if (aggregateSampleCount > 0) logAggregate();
    loggingEnabled = false;
    resetAggregate();
    appendEventLog(
      "low_battery",
      "power_monitor",
      "battery ~" + String(batteryMillivolts) +
      " mV; final row flushed and logging suspended to protect the log file"
    );
  } else if (loggingSuspendedLowBatt && onUsbPower) {
    // External power restored before shutdown; resume if the RTC gate still passes.
    loggingSuspendedLowBatt = false;
    if (rtcOk && rtcWasSynced && !rtcBatteryConcern) {
      loggingEnabled = true;
      resetAggregate();
      lastLogMillis = millis();
      appendEventLog("logging_start", "power_restored", "external power restored; logging resumed");
    }
  }
}

void checkScheduledReboot() {
  if (millis() - bootMillis < REBOOT_INTERVAL_MS) return;

  if (loggingEnabled) {
    if (!rebootPending) {
      appendEventLog(
        "scheduled_reboot_pending",
        "seven_day_reboot",
        "waiting until after next completed log row"
      );
    }

    rebootPending = true;
    return;
  }

  appendEventLog(
    "scheduled_reboot",
    "seven_day_reboot",
    "rebooting while logging stopped"
  );

  Serial.println("Scheduled reboot while logging stopped.");
  delay(100);
  ESP.restart();
}

// Maps the ESP32 reset cause to a short slug for the boot event. "brownout"
// means the supply dipped below the safe threshold (a real power problem);
// "sw_restart" is the normal scheduled reboot / ESP.restart(); the watchdog
// and panic cases indicate a hang or crash rather than a power event.
const char *resetReasonText(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_EXT:       return "external_pin";
    case ESP_RST_SW:        return "sw_restart";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_watchdog";
    case ESP_RST_TASK_WDT:  return "task_watchdog";
    case ESP_RST_WDT:       return "other_watchdog";
    case ESP_RST_DEEPSLEEP: return "deep_sleep_wake";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
  }
}

// -------- Arduino setup / loop --------
void setup() {
  Serial.begin(115200);
  delay(500);

  bootMillis = millis();
  loadLoggingConfig();

  // Battery/power sense on GPIO39 (BATT_VIN/3). 11 dB attenuation covers the
  // ~1.1-1.7 V this pin sees. Force the first reading to happen immediately so
  // the webpage shows a power source right away.
  analogSetPinAttenuation(BATTERY_SENSE_PIN, ADC_11db);
  lastPowerCheckMillis = millis() - POWER_CHECK_INTERVAL_MS;

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);   // 100 kHz for reliable I2C with SHT-30 cable + RTC
  Wire.setTimeOut(100);    // Avoid long I2C lockups

  sensorOk = sht31.begin(0x44);
  rtcOk = rtc.begin();

  setupSdCard();

  Serial.println();
  Serial.println("ESP32 Temperature/Humidity Logger - NimBLE Wi-Fi Variant");
  Serial.println(sensorOk ? "SHT-30 found." : "SHT-30 not found. Check wiring/address.");
  Serial.println(rtcOk ? "DS3231 RTC found." : "DS3231 RTC not found. Check wiring.");
  Serial.println(sdOk ? "microSD logging enabled." : "microSD card not found; logging recent readings in RAM only.");

  if (rtcOk) {
    DateTime bootRtcTime;
    bool bootTimeReadable = readRtcTime(bootRtcTime);

    rtcLostPower = rtc.lostPower();
    rtcTimeWasBadAtBoot = !bootTimeReadable;  // covers year<2020 and garbage reads
    rtcBatteryConcern = rtcLostPower || rtcTimeWasBadAtBoot;

    if (rtcBatteryConcern) {
      Serial.println("RTC time invalid or lost power. Sync RTC and check/replace coin cell battery if this repeats.");
    }
  } else {
    rtcBatteryConcern = true;
  }

  bootResetReason = resetReasonText(esp_reset_reason());
  Serial.print("Reset reason: ");
  Serial.println(bootResetReason);

  appendEventLog(
    "boot",
    bootResetReason,
    String("sensor=") + (sensorOk ? "ok" : "bad") +
    ", rtc=" + (rtcOk ? "ok" : "bad") +
    ", sd=" + (sdOk ? "ok" : "bad") +
    ", resetReason=" + bootResetReason +
    ", rtcLostPower=" + (rtcLostPower ? "true" : "false") +
    ", rtcTimeWasBadAtBoot=" + (rtcTimeWasBadAtBoot ? "true" : "false")
  );

  setupWatchdog();

  if (autoLoggingWanted && rtcOk && !rtcBatteryConcern && rtcWasSynced) {
    loggingEnabled = true;
    resetAggregate();
    lastLogMillis = millis();
    Serial.println("Auto-resuming logging after reboot.");
    appendEventLog("logging_start", "auto_resume", "logging resumed after reboot");
  } else if (autoLoggingWanted && rtcOk && rtcWasSynced && !rtcTimeWasBadAtBoot) {
    // The RTC reports lost power (typically a dead/missing coin cell during a
    // brief supply blip) but its time still reads as valid and plausible. For
    // an unattended deployment, losing days of data to a momentary blip is the
    // worse failure, so resume logging and flag the concern loudly. Manual
    // start via the web page still requires a fresh sync.
    loggingEnabled = true;
    resetAggregate();
    lastLogMillis = millis();
    Serial.println("Auto-resuming logging DESPITE RTC lost-power flag (time still plausible).");
    Serial.println("Check/replace the RTC coin cell and re-sync when possible.");
    appendEventLog(
      "logging_start",
      "auto_resume_rtc_concern",
      "RTC lost-power flag set but time reads plausible; logging resumed - check/replace coin cell and re-sync"
    );
  } else {
    loggingEnabled = false;
  }

  setupRoutes();
  setupWifiControl(&server, AP_SSID, AP_PASSWORD);
  startBle(BLE_DEVICE_NAME);

  // Wi-Fi starts off by default in the NimBLE variant.
  // Send BLE command WIFI_ON to enable the webpage.

  resetAggregate();
  sampleSensor();

  lastSampleMillis = millis();
  lastLogMillis = millis();
}

void loop() {
  // Execute any BLE command on the main task (never on the NimBLE host task).
  String bleCommand;
  if (takeBleCommand(bleCommand)) {
    processBleCommand(bleCommand);
  }

  if (isWifiEnabled()) {
    server.handleClient();
  }

  unsigned long nowMillis = millis();

  if (nowMillis - lastSampleMillis >= SAMPLE_INTERVAL_MS) {
    lastSampleMillis = nowMillis;
    sampleSensor();
  }

  if (loggingEnabled && nowMillis - lastLogMillis >= LOG_INTERVAL_MS) {
    lastLogMillis = nowMillis;
    logAggregate();
  }

  if (wifiOffRequested) {
    wifiOffRequested = false;
    appendEventLog("wifi_off", "web_command", "WIFI_OFF from webpage");
    stopWifi();
  }

  checkWifiAutoOff();
  ensureBleAdvertising();
  ensureSdCard();
  manageSensorHeater();
  checkPowerStatus();
  checkScheduledReboot();

  esp_task_wdt_reset(); // reset the dog
}

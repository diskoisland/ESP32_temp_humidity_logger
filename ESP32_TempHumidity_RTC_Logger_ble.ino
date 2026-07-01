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
const char *BLE_DEVICE_NAME = "SHT30-Logger";

// -------- User settings --------
const unsigned long SAMPLE_INTERVAL_MS = 5000;      // Sensor sampling time, 5 sec
const unsigned long LOG_INTERVAL_MS = 60000;        // Logging interval, 1 minute
const unsigned long REBOOT_INTERVAL_MS = 7UL * 24UL * 60UL * 60UL * 1000UL;  // 7 days
const uint16_t MAX_LOG_ROWS = 1440;                 // 24 hours at one-minute averages
const uint32_t WATCHDOG_TIMEOUT_MS = 30000;         // 30 second watchdog

// SparkFun MicroMod ESP32 maps the primary MicroMod/Qwiic I2C bus to GPIO 21/22.
const int I2C_SDA_PIN = 21;
const int I2C_SCL_PIN = 22;

// SparkFun MicroMod ESP32 primary SPI mapping used by the Data Logging Carrier.
const int SD_CS_PIN = 5;
const char *LOG_FILENAME = "/temp_humidity_1min.csv";
const char *EVENT_LOG_FILENAME = "/logger_events.csv";

const char *LOG_CSV_HEADER =
  "timestamp,timezone,utc_offset_minutes,"
  "avg_temperature_c,min_temperature_c,max_temperature_c,"
  "avg_humidity_percent,min_humidity_percent,max_humidity_percent,sample_count";

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

bool rtcOk = false;
bool rtcLostPower = false;
bool rtcTimeWasBadAtBoot = false;
bool rtcBatteryConcern = false;
bool rtcWasSynced = false;
String rtcSyncTimezone = "Not synced";
int rtcSyncUtcOffsetMinutes = 0;

const char *bootResetReason = "unknown";  // cause of the most recent restart

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

void appendEventLog(const char *event, const char *reason, const String &details) {
  Serial.print("Event: ");
  Serial.print(event);
  Serial.print(" / ");
  Serial.print(reason);
  Serial.print(" / ");
  Serial.println(details);

  // Event rows need a reliable timestamp. If the SD card or RTC is missing,
  // the event is still printed to Serial but not written to the event CSV.
  if (!sdOk || !rtcOk) return;

  File file = SD.open(EVENT_LOG_FILENAME, FILE_APPEND);
  if (!file) {
    Serial.println("Could not open event log file.");
    sdOk = false;
    return;
  }

  if (file.size() == 0) {
    file.println(EVENT_CSV_HEADER);
  }

  DateTime now = rtc.now();

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

  if (command == "STATUS") {
    appendEventLog("status", "ble_command", "STATUS");
    sendBleResponse(bleStatusText());
    return;
  }

  appendEventLog("unknown_ble_command", "ble_command", command);
  sendBleResponse("ERROR Unknown command. Use WIFI_ON, WIFI_OFF, or STATUS.");
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
  prefs.end();
}

void loadLoggingConfig() {
  prefs.begin("logger", true);
  autoLoggingWanted = prefs.getBool("autoLog", false);
  rtcWasSynced = prefs.getBool("rtcSynced", false);
  rtcSyncTimezone = prefs.getString("tz", "Not synced");
  rtcSyncUtcOffsetMinutes = prefs.getInt("utcOffset", 0);
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

  float temperatureC = sht31.readTemperature();
  float humidity = sht31.readHumidity();

  if (isnan(temperatureC) || isnan(humidity)) {
    latestValid = false;
    appendEventLog("sensor_error", "sample_failed", "SHT-30 returned NaN");
    return;
  }

  latestTemperatureC = temperatureC;
  latestHumidity = humidity;
  if (rtcOk) latestTimestamp = rtc.now();
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

  DateTime now = rtc.now();
  float avgTemperatureC = sumTemperatureC / aggregateSampleCount;
  float avgHumidity = sumHumidity / aggregateSampleCount;

  logRows[logWriteIndex] = {
    now,
    (int16_t)rtcSyncUtcOffsetMinutes,
    avgTemperatureC,
    minTemperatureC,
    maxTemperatureC,
    avgHumidity,
    minHumidity,
    maxHumidity,
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
void handleStartLogging() {
  if (!rtcOk || !rtcWasSynced || rtcBatteryConcern) {
    appendEventLog(
      "logging_blocked",
      "web_command",
      "start requested but RTC was not synced, invalid, or battery concern was active"
    );

    server.send(400, "text/plain", "Sync RTC and confirm RTC is valid before starting logging.");
    return;
  }

  loggingEnabled = true;
  autoLoggingWanted = true;
  resetAggregate();
  lastLogMillis = millis();

  saveLoggingConfig();

  appendEventLog("logging_start", "web_command", "user pressed Start logging");

  server.send(200, "text/plain", "Logging started.");
}

void handleStopLogging() {
  loggingEnabled = false;
  autoLoggingWanted = false;
  rebootPending = false;
  resetAggregate();

  saveLoggingConfig();

  appendEventLog("logging_stop", "web_command", "user pressed Stop logging");

  server.send(200, "text/plain", "Logging stopped.");
}

String statusJson() {
  DateTime now = rtcOk ? rtc.now() : DateTime(2000, 1, 1, 0, 0, 0);

  // Heap health. largestFreeBlock / freeHeap is the fragmentation indicator:
  // when free heap stays high but the largest block shrinks, the heap is
  // fragmenting. Watch these over days of real use before optimizing further.
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t largestFreeBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

  String json;
  json.reserve(1024);   // one allocation instead of ~40 reallocs while building
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
  json += rtcOk ? formatDateTime(now) : String("RTC not found");
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
    </section>

    <div class="row">
      <button onclick="flashButton(this); syncRtc()">Sync RTC to this device</button>
      <button onclick="flashButton(this); startLogging()">Start logging</button>
      <button onclick="flashButton(this); stopLogging()">Stop logging</button>
      <a class="button" href="/log.csv" onclick="flashButton(this)">Download CSV</a>
      <a class="button" href="/events.csv" onclick="flashButton(this)">Download Events</a>
    </div>

    <div id="message"></div>

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
  </main>

  <script>
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
      document.getElementById('sensorStatus').textContent = status.sensorOk ? 'Found / OK' : 'Not found';

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

    refresh();
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

  rtc.adjust(browserTime);

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
    "RTC synced from browser device using timezone " + rtcSyncTimezone
  );

  server.send(200, "text/plain", "RTC synced to this device: " + formatDateTime(browserTime));
}

// -------- Setup helpers --------
void setupRoutes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/log", HTTP_GET, handleLogJson);
  server.on("/log.csv", HTTP_GET, handleLogCsv);
  server.on("/events.csv", HTTP_GET, handleEventCsv);
  server.on("/api/sync", HTTP_POST, handleSyncRtc);
  server.on("/api/logging/start", HTTP_POST, handleStartLogging);
  server.on("/api/logging/stop", HTTP_POST, handleStopLogging);

  // In the NimBLE variant, server.begin() is called only when Wi-Fi is enabled
  // inside WifiControl.cpp.
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
    DateTime bootRtcTime = rtc.now();

    rtcLostPower = rtc.lostPower();
    rtcTimeWasBadAtBoot = bootRtcTime.year() < 2020;
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

  checkWifiAutoOff();
  checkScheduledReboot();

  esp_task_wdt_reset(); // reset the dog
}

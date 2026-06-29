#include "WifiControl.h"

#include <WiFi.h>
#include <ESPmDNS.h>

static WebServer *server = nullptr;
static const char *ssid = nullptr;
static const char *password = nullptr;

static bool wifiEnabled = false;
static unsigned long wifiStartedMillis = 0;

static const unsigned long WIFI_AUTO_OFF_MS = 15UL * 60UL * 1000UL;  // 15 minutes

void setupWifiControl(WebServer *webServer, const char *apSsid, const char *apPassword) {
  server = webServer;
  ssid = apSsid;
  password = apPassword;
}

bool isWifiEnabled() {
  return wifiEnabled;
}

String currentIpAddress() {
  if (!wifiEnabled) return "Wi-Fi off";

  if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
    return WiFi.softAPIP().toString();
  }

  return WiFi.localIP().toString();
}

void startWifi() {
  if (wifiEnabled) {
    wifiStartedMillis = millis();
    Serial.println("Wi-Fi already running; timeout refreshed.");
    return;
  }

  if (server == nullptr || ssid == nullptr || password == nullptr) {
    Serial.println("Wi-Fi control not configured.");
    return;
  }

  WiFi.mode(WIFI_AP);

  bool apStarted = WiFi.softAP(ssid, password);

  if (apStarted) {
    wifiEnabled = true;
    wifiStartedMillis = millis();

    server->begin();

    Serial.print("Access point started. Connect to ");
    Serial.print(ssid);
    Serial.print(" and open http://");
    Serial.println(WiFi.softAPIP());

    if (MDNS.begin("datalogger")) {
      Serial.println("mDNS started. Try: http://datalogger.local");
    } else {
      Serial.println("mDNS failed to start.");
    }
  } else {
    wifiEnabled = false;
    Serial.println("Access point failed to start.");
  }
}

void stopWifi() {
  if (!wifiEnabled) {
    Serial.println("Wi-Fi already off.");
    return;
  }

  MDNS.end();

  if (server != nullptr) {
    server->stop();
  }

  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);

  wifiEnabled = false;

  Serial.println("Wi-Fi access point stopped.");
}

void checkWifiAutoOff() {
  if (!wifiEnabled) return;

  if (millis() - wifiStartedMillis >= WIFI_AUTO_OFF_MS) {
    Serial.println("Wi-Fi auto-off timeout reached.");
    stopWifi();
  }
}
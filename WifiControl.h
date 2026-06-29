#ifndef WIFI_CONTROL_H
#define WIFI_CONTROL_H

#include <Arduino.h>
#include <WebServer.h>

void setupWifiControl(WebServer *webServer, const char *apSsid, const char *apPassword);
void startWifi();
void stopWifi();
void checkWifiAutoOff();
String currentIpAddress();
bool isWifiEnabled();

#endif
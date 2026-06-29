#ifndef BLE_CONTROL_H
#define BLE_CONTROL_H

#include <Arduino.h>

typedef void (*BleCommandHandler)(const String &command);

void startBle(const char *deviceName, BleCommandHandler commandHandler);
void sendBleResponse(const String &response);

#endif
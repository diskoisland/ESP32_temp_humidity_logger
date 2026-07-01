#ifndef BLE_CONTROL_H
#define BLE_CONTROL_H

#include <Arduino.h>

void startBle(const char *deviceName);
void sendBleResponse(const String &response);

// Called from loop() on the main task. If a BLE write has arrived since the
// last call, copies it into commandOut and returns true. The command is
// captured in the NimBLE callback but executed on the main task so that all
// Wi-Fi / SD / I2C work stays on a single task.
bool takeBleCommand(String &commandOut);

#endif
#include "BleControl.h"

#include <NimBLEDevice.h>

#define BLE_SERVICE_UUID        "8b7e0001-7a1b-4c2d-9f5e-123456789abc"
#define BLE_COMMAND_CHAR_UUID   "8b7e0002-7a1b-4c2d-9f5e-123456789abc"

static NimBLECharacteristic *bleCommandCharacteristic = nullptr;

// Single-slot mailbox for a command received over BLE. The NimBLE callback
// runs on the NimBLE host task; loop() drains it on the main Arduino task.
// A critical section guards the shared String against concurrent access.
static portMUX_TYPE bleCommandMux = portMUX_INITIALIZER_UNLOCKED;
static String pendingBleCommand;
static volatile bool bleCommandPending = false;

class BleCommandCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &connInfo) override {
    std::string value = characteristic->getValue();

    String command = String(value.c_str());
    command.trim();
    command.toUpperCase();

    // Do the minimum here: stash the command and return. All Wi-Fi/SD/I2C work
    // happens later in loop() so it never runs on the NimBLE host task.
    portENTER_CRITICAL(&bleCommandMux);
    pendingBleCommand = command;
    bleCommandPending = true;
    portEXIT_CRITICAL(&bleCommandMux);
  }
};

bool takeBleCommand(String &commandOut) {
  bool hadCommand = false;

  portENTER_CRITICAL(&bleCommandMux);
  if (bleCommandPending) {
    commandOut = pendingBleCommand;
    bleCommandPending = false;
    hadCommand = true;
  }
  portEXIT_CRITICAL(&bleCommandMux);

  return hadCommand;
}

void sendBleResponse(const String &response) {
  if (bleCommandCharacteristic == nullptr) return;

  bleCommandCharacteristic->setValue(response.c_str());
  bleCommandCharacteristic->notify();

  Serial.print("BLE response: ");
  Serial.println(response);
}

void startBle(const char *deviceName) {
  NimBLEDevice::init(deviceName);

  NimBLEServer *bleServer = NimBLEDevice::createServer();
  NimBLEService *service = bleServer->createService(BLE_SERVICE_UUID);

  bleCommandCharacteristic = service->createCharacteristic(
    BLE_COMMAND_CHAR_UUID,
    NIMBLE_PROPERTY::READ |
    NIMBLE_PROPERTY::WRITE |
    NIMBLE_PROPERTY::NOTIFY
  );

  bleCommandCharacteristic->setCallbacks(new BleCommandCallbacks());
  bleCommandCharacteristic->setValue("READY. Commands: WIFI_ON, WIFI_OFF, STATUS");

  service->start();

  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(BLE_SERVICE_UUID);
  advertising->enableScanResponse(true);
  advertising->start();

  Serial.print("NimBLE started. Device name: ");
  Serial.println(deviceName);
}
#include "BleControl.h"

#include <NimBLEDevice.h>

#define BLE_SERVICE_UUID        "8b7e0001-7a1b-4c2d-9f5e-123456789abc"
#define BLE_COMMAND_CHAR_UUID   "8b7e0002-7a1b-4c2d-9f5e-123456789abc"

static NimBLECharacteristic *bleCommandCharacteristic = nullptr;
static BleCommandHandler bleCommandHandler = nullptr;

class BleCommandCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &connInfo) override {
    std::string value = characteristic->getValue();

    String command = String(value.c_str());
    command.trim();
    command.toUpperCase();

    if (bleCommandHandler != nullptr) {
      bleCommandHandler(command);
    }
  }
};

void sendBleResponse(const String &response) {
  if (bleCommandCharacteristic == nullptr) return;

  bleCommandCharacteristic->setValue(response.c_str());
  bleCommandCharacteristic->notify();

  Serial.print("BLE response: ");
  Serial.println(response);
}

void startBle(const char *deviceName, BleCommandHandler commandHandler) {
  bleCommandHandler = commandHandler;

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
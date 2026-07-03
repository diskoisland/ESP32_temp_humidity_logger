#include "BleControl.h"

#include <NimBLEDevice.h>

#define BLE_SERVICE_UUID        "8b7e0001-7a1b-4c2d-9f5e-123456789abc"
#define BLE_COMMAND_CHAR_UUID   "8b7e0002-7a1b-4c2d-9f5e-123456789abc"

static NimBLEServer *bleServer = nullptr;
static NimBLECharacteristic *bleCommandCharacteristic = nullptr;

// Idle-kick: a phone that connects and then goes silent (BLE app backgrounded)
// holds the connection and stops advertising, making the device invisible to
// every other scanner. Track last activity and drop clients that exceed this.
static const unsigned long BLE_IDLE_TIMEOUT_MS = 5UL * 60UL * 1000UL;  // 5 minutes
static volatile unsigned long lastBleActivityMillis = 0;

// Single-slot mailbox for a command received over BLE. The NimBLE callback
// runs on the NimBLE host task; loop() drains it on the main Arduino task.
// A critical section guards the shared String against concurrent access.
static portMUX_TYPE bleCommandMux = portMUX_INITIALIZER_UNLOCKED;
static String pendingBleCommand;
static volatile bool bleCommandPending = false;

class BleServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *server, NimBLEConnInfo &connInfo) override {
    lastBleActivityMillis = millis();
    Serial.println("BLE client connected.");
  }

  void onDisconnect(NimBLEServer *server, NimBLEConnInfo &connInfo, int reason) override {
    // Reason 8 (0x08) = supervision timeout, i.e. the client vanished without
    // a clean disconnect (out of range, app killed). Advertising must be
    // restarted explicitly or the device stays invisible until reboot.
    Serial.print("BLE client disconnected, reason ");
    Serial.println(reason);

    NimBLEDevice::getAdvertising()->start();
  }
};

class BleCommandCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &connInfo) override {
    std::string value = characteristic->getValue();

    String command = String(value.c_str());
    command.trim();
    command.toUpperCase();

    lastBleActivityMillis = millis();

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

  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new BleServerCallbacks());
  bleServer->advertiseOnDisconnect(true);

  NimBLEService *service = bleServer->createService(BLE_SERVICE_UUID);

  bleCommandCharacteristic = service->createCharacteristic(
    BLE_COMMAND_CHAR_UUID,
    NIMBLE_PROPERTY::READ |
    NIMBLE_PROPERTY::WRITE |
    NIMBLE_PROPERTY::NOTIFY
  );

  bleCommandCharacteristic->setCallbacks(new BleCommandCallbacks());
  bleCommandCharacteristic->setValue("READY. Commands: WIFI_ON, WIFI_OFF, LOG_ON, LOG_OFF, STATUS");

  service->start();

  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(BLE_SERVICE_UUID);
  advertising->enableScanResponse(true);
  advertising->start();

  Serial.print("NimBLE started. Device name: ");
  Serial.println(deviceName);
}

void ensureBleAdvertising() {
  // Self-heal: if no client is connected but advertising has stopped (unclean
  // disconnect, Wi-Fi/BLE coexistence hiccup), restart it. Checked on a slow
  // cadence from loop() so a stalled radio never requires a reboot.
  static unsigned long lastCheckMillis = 0;

  if (millis() - lastCheckMillis < 5000) return;
  lastCheckMillis = millis();

  if (bleServer == nullptr) return;

  uint8_t connectedCount = bleServer->getConnectedCount();
  if (connectedCount > 0) {
    // Idle-kick: drop clients that have been silent too long so the device
    // goes back to advertising. onDisconnect restarts advertising for us.
    if (millis() - lastBleActivityMillis >= BLE_IDLE_TIMEOUT_MS) {
      Serial.println("BLE client idle too long; disconnecting to resume advertising.");

      for (uint8_t i = 0; i < connectedCount; i++) {
        bleServer->disconnect(bleServer->getPeerInfo(i).getConnHandle());
      }

      lastBleActivityMillis = millis();  // don't re-kick while teardown completes
    }

    return;
  }

  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  if (advertising->isAdvertising()) return;

  if (advertising->start()) {
    Serial.println("BLE advertising restarted by watchdog check.");
  } else {
    Serial.println("BLE advertising restart failed; will retry.");
  }
}
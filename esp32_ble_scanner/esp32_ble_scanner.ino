#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLERemoteCharacteristic.h>

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------
static BLEUUID serviceUUID("ABCD1234-0000-467A-9538-01F0652C74E0");
static BLEUUID charUUID("ABCD1234-0001-467A-9538-01F0652C74E0");

static const char* TARGET_NAME = "MyBLE";

// -----------------------------------------------------------------------------
// Globals
// -----------------------------------------------------------------------------
BLEAdvertisedDevice* myDevice = nullptr;
BLERemoteCharacteristic* pRemoteCharacteristic = nullptr;

bool doConnect = false;
bool connected = false;

// -----------------------------------------------------------------------------
// Notification callback
// -----------------------------------------------------------------------------
void notifyCallback(
    BLERemoteCharacteristic* pChar,
    uint8_t* pData,
    size_t length,
    bool isNotify)
{
    // Serial.println("Notification received!");

    if(length != 2)
    {
        Serial.print("Unexpected length: ");
        Serial.println(length);
        return;
    }

    uint16_t value =
        pData[0] |
        (pData[1] << 8);

    Serial.print("Analog = ");
    Serial.println(value);
}

// -----------------------------------------------------------------------------
// Client callbacks
// -----------------------------------------------------------------------------
class MyClientCallbacks : public BLEClientCallbacks
{
  void onConnect(BLEClient* client)
  {
    Serial.println("Connected to MyBLE!");
  }

  void onDisconnect(BLEClient* client)
  {
    connected = false;
    Serial.println("Disconnected!");
    Serial.println("Restarting scan...");
  }
};

// -----------------------------------------------------------------------------
// Scan callbacks
// -----------------------------------------------------------------------------
class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks
{
  void onResult(BLEAdvertisedDevice advertisedDevice)
  {
    if (!advertisedDevice.haveName())
      return;

    if (advertisedDevice.getName() != TARGET_NAME)
      return;

    Serial.println("Found MyBLE!");

    BLEDevice::getScan()->stop();

    myDevice = new BLEAdvertisedDevice(advertisedDevice);

    doConnect = true;
  }
};

// -----------------------------------------------------------------------------
// Connect
// -----------------------------------------------------------------------------
bool connectToServer()
{
  BLEClient* pClient = BLEDevice::createClient();

  pClient->setClientCallbacks(new MyClientCallbacks());

  Serial.println("Connecting...");

  if (!pClient->connect(myDevice))
  {
    Serial.println("Connection failed.");
    return false;
  }

  Serial.println("Connected.");

  BLERemoteService* pRemoteService =
  pClient->getService(serviceUUID);

  if (pRemoteService == nullptr)
  {
    Serial.println("Service not found.");
    pClient->disconnect();
    return false;
  }

  pRemoteCharacteristic =
  pRemoteService->getCharacteristic(charUUID);

  if (pRemoteCharacteristic == nullptr)
  {
    Serial.println("Characteristic not found.");
    pClient->disconnect();
    return false;
  }

  if (pRemoteCharacteristic->canNotify())
  {
    pRemoteCharacteristic->registerForNotify(notifyCallback);

    Serial.println("Notifications enabled.");
  }

  connected = true;

  return true;
}

// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------
void setup()
{
  Serial.begin(115200);

  Serial.println("--------------------------------------");
  Serial.println("ESP32 BLE Analog Receiver");
  Serial.println("--------------------------------------");

  BLEDevice::init("");

  BLEScan* pScan = BLEDevice::getScan();

  pScan->setAdvertisedDeviceCallbacks(
    new MyAdvertisedDeviceCallbacks());

  pScan->setActiveScan(true);
  pScan->setInterval(100);
  pScan->setWindow(99);

  pScan->start(0, false);

  Serial.println("Scanning...");
}

// -----------------------------------------------------------------------------
// Loop
// -----------------------------------------------------------------------------
void loop()
{
  if (doConnect)
  {
    doConnect = false;

    if (connectToServer())
    {
      Serial.println("Ready.");
    }
    else
    {
      Serial.println("Retry scanning...");
      BLEDevice::getScan()->start(0, false);
    }
  }

  if (!connected)
  {
    static uint32_t lastRetry = 0;

    if (millis() - lastRetry > 50) // 3000
    {
      lastRetry = millis();

      BLEDevice::getScan()->start(0, false);
    }
  }

  delay(10);
}

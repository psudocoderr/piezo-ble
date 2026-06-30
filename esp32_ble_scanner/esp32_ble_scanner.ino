#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// == Configuration ============================================================
static const char* TARGET_NAME   = "MyBLE"; // Must match Bluefruit.setName()
static const int   SCAN_DURATION = 3;        // Seconds per scan session
static const int   SCAN_INTERVAL = 100;      // Scan interval  (BLE units x 0.625 ms = 62.5 ms)
static const int   SCAN_WINDOW   = 99;       // Scan window    (BLE units x 0.625 ms = 61.9 ms)
// =============================================================================

BLEScan* pBLEScan = nullptr;

/* Helper: pretty-print a raw byte buffer as "0xHH 0xHH ..." */
void printHex(const uint8_t* buf, size_t len) {
  for (size_t i = 0; i < len; i++) {
    Serial.print("0x");
    if (buf[i] < 0x10) Serial.print('0');
    Serial.print(buf[i], HEX);
    if (i < len - 1) Serial.print(' ');
  }
}

/* BLE advertised-device callback - fires for every advertisement received */
class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {

    // Ignore everything except "MyBLE"
    if (!advertisedDevice.haveName()) return;
    if (advertisedDevice.getName() != TARGET_NAME) return;

    // Read Manufacturer Specific Data
    if (advertisedDevice.haveManufacturerData()) {

      String mfrData = advertisedDevice.getManufacturerData();
      const uint8_t* raw = (const uint8_t*)mfrData.c_str();
      size_t rawLen = mfrData.length();

      if (rawLen >= 2) {

        uint16_t analogValue =
            (uint16_t)raw[0] |
            ((uint16_t)raw[1] << 8);

        Serial.print("[t=");
        Serial.print(millis());
        Serial.print(" ms] Analog A0 = ");
        Serial.print(analogValue);
        Serial.print("  |  Adv payload bytes: 0x");

        if (raw[0] < 0x10) Serial.print('0');
        Serial.print(raw[0], HEX);

        Serial.print(" 0x");

        if (raw[1] < 0x10) Serial.print('0');
        Serial.println(raw[1], HEX);
      }
    }
  }
};

/* setup */
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("============================================================");
  Serial.println("  ESP32 BLE Scanner - looking for MyBLE piezo advertiser");
  Serial.print  ("  Target name   : ");
  Serial.println(TARGET_NAME);
  Serial.print  ("  Scan duration : ");
  Serial.print  (SCAN_DURATION);
  Serial.println(" s per window");
  Serial.print  ("  Scan interval : ");
  Serial.print  (SCAN_INTERVAL * 0.625f, 1);
  Serial.println(" ms");
  Serial.print  ("  Scan window   : ");
  Serial.print  (SCAN_WINDOW * 0.625f, 1);
  Serial.println(" ms");
  Serial.println("============================================================");

  BLEDevice::init("ESP32_Scanner");

  pBLEScan = BLEDevice::getScan();
  // wantDuplicates = true so we get a callback on EVERY advertisement, not just the first
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks(), true);
  pBLEScan->setActiveScan(true);    // Request scan-response packets too
  pBLEScan->setInterval(SCAN_INTERVAL);
  pBLEScan->setWindow(SCAN_WINDOW);

  Serial.println("Scanning for MyBLE...");
}

/* loop */
void loop() {
  BLEScanResults* results = pBLEScan->start(SCAN_DURATION, false);
  pBLEScan->clearResults();
}
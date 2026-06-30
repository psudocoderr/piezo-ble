#include <bluefruit.h>
#include <Adafruit_TinyUSB.h>

// -----------------------------------------------------------------------------
// UUIDs (must match ESP32)
// -----------------------------------------------------------------------------
BLEService bleService("ABCD1234-0000-467A-9538-01F0652C74E0");

BLECharacteristic bleCharacteristic(
  "ABCD1234-0001-467A-9538-01F0652C74E0");

// 2-byte payload
uint8_t data[2];

// -----------------------------------------------------------------------------
// Start advertising
// -----------------------------------------------------------------------------
void startAdvertising()
{
  Bluefruit.Advertising.stop();
  Bluefruit.Advertising.clearData();

  Bluefruit.ScanResponse.clearData();
  Bluefruit.ScanResponse.addName();

  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addService(bleService);

  Bluefruit.Advertising.setInterval(160, 160);      // 100 ms
  Bluefruit.Advertising.setFastTimeout(30);
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.start(0);

  Serial.println("Advertising...");
}

// -----------------------------------------------------------------------------
// Connected
// -----------------------------------------------------------------------------
void connect_callback(uint16_t conn_handle)
{
  BLEConnection* connection = Bluefruit.Connection(conn_handle);

  // Request lower latency
  connection->requestConnectionParameter(6, 0, 200);

  Serial.print("[t=");
  Serial.print(millis());
  Serial.print(" ms] Connected. Handle=");
  Serial.println(conn_handle);
}

// -----------------------------------------------------------------------------
// Disconnected
// -----------------------------------------------------------------------------
void disconnect_callback(uint16_t conn_handle, uint8_t reason)
{
  Serial.print("[t=");
  Serial.print(millis());
  Serial.print(" ms] Disconnected. Reason=0x");
  Serial.println(reason, HEX);
}

// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------
void setup()
{
  Serial.begin(115200);

  Bluefruit.begin();
  Bluefruit.setTxPower(4);
  Bluefruit.setName("MyBLE");

  // Service
  bleService.begin();

  // Characteristic
  bleCharacteristic.setProperties(CHR_PROPS_NOTIFY);

  bleCharacteristic.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);

  bleCharacteristic.setFixedLen(2);

  bleCharacteristic.begin();

  Bluefruit.Periph.setConnectCallback(connect_callback);
  Bluefruit.Periph.setDisconnectCallback(disconnect_callback);

  startAdvertising();

  Serial.println("------------------------------------");
  Serial.println(" nRF BLE Analog Notifier");
  Serial.println(" Device : MyBLE");
  Serial.println(" Update : 100 ms");
  Serial.println("------------------------------------");
}

// -----------------------------------------------------------------------------
// Loop
// -----------------------------------------------------------------------------
void loop()
{
  static uint32_t lastUpdate = 0;

  if (millis() - lastUpdate >= 50)
  {
    lastUpdate = millis();

    uint16_t analogValue = analogRead(A0);

    data[0] = analogValue & 0xFF;
    data[1] = analogValue >> 8;

    // Send notification only if a client is connected
    if (Bluefruit.connected())
    {
      bleCharacteristic.notify(data, sizeof(data));
    }

    // Debug output
    Serial.print("[t=");
    Serial.print(millis());
    Serial.print(" ms] Analog A0 = ");
    Serial.print(analogValue);

    Serial.print(" | Notify bytes: 0x");

    if (data[0] < 0x10) Serial.print('0');
    Serial.print(data[0], HEX);

    Serial.print(" 0x");

    if (data[1] < 0x10) Serial.print('0');
    Serial.println(data[1], HEX);
  }
}

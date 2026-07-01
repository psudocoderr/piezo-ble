#include <bluefruit.h>
#include <Adafruit_TinyUSB.h>

// -----------------------------------------------------------------------------
// UUIDs (must match ESP32)
// -----------------------------------------------------------------------------
BLEService bleService("ABCD1234-0000-467A-9538-01F0652C74E0");
BLECharacteristic bleCharacteristic("ABCD1234-0001-467A-9538-01F0652C74E0");

// -----------------------------------------------------------------------------
// Packet buffer
// 10 samples × 2 bytes = 20-byte payload
// -----------------------------------------------------------------------------
uint16_t sampleBuffer[10];
uint8_t sampleIndex = 0;

// -----------------------------------------------------------------------------
// Advertising
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

  // Request ~7.5 ms connection interval
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

  sampleIndex = 0;
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

  bleService.begin();

  bleCharacteristic.setProperties(CHR_PROPS_NOTIFY);

  bleCharacteristic.setPermission(
    SECMODE_OPEN,
    SECMODE_NO_ACCESS);

  // 20-byte notification
  bleCharacteristic.setFixedLen(20);

  bleCharacteristic.begin();

  Bluefruit.Periph.setConnectCallback(connect_callback);
  Bluefruit.Periph.setDisconnectCallback(disconnect_callback);

  startAdvertising();

  Serial.println("------------------------------------");
  Serial.println("nRF52840 BLE Analog Stream");
  Serial.println("Sampling Rate : 200 Hz");
  Serial.println("Packet Size   : 10 samples");
  Serial.println("Payload       : 20 bytes");
  Serial.println("------------------------------------");
}

// -----------------------------------------------------------------------------
// Main Loop
// -----------------------------------------------------------------------------
void loop()
{
  static uint32_t lastSample = 0;

  // Sample every 5 ms (200 Hz)
  if (micros() - lastSample >= 5000)
  {
    lastSample += 5000;

    sampleBuffer[sampleIndex++] = analogRead(A0);
    // sampleBuffer[sampleIndex++] = 7000 + sampleIndex;

    // Buffer full?
    if (sampleIndex >= 10)
    {
      // Send one 20-byte notification
      if (Bluefruit.connected())
      {
        bleCharacteristic.notify(
          (uint8_t*)sampleBuffer,
                                 sizeof(sampleBuffer));
      }

        //Try to get know other methods to send data and data format. Eg- Indicate, write etc.

      // Debug output
      Serial.print("[");
      Serial.print(millis());
      Serial.print(" ms] ");

      for (int i = 0; i < 10; i++)
      {
        Serial.print(sampleBuffer[i]);

        if (i != 9)
          Serial.print(", ");
      }

      Serial.println();

      sampleIndex = 0;
    }
  }
}

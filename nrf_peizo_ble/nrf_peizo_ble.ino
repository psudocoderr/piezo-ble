#include <bluefruit.h>
#include <Adafruit_TinyUSB.h>

  uint8_t data[2];
BLEService bleService = BLEService(0x180D); // Custom BLE service
BLECharacteristic bleCharacteristic = BLECharacteristic(0x2A37); // Custom BLE characteristic

// Function to read analog value and start advertising
void startAdvertising() {
  Bluefruit.Advertising.clearData(); // Clear current advertising data
  Bluefruit.Advertising.addName();    // Add device name to advertisement

  // Read the analog value from pin A0 (adjust pin as needed)
  int analogValue = analogRead(A0);

  // Convert the analog value to a byte array (2 bytes for 16-bit value)
  data[0] = analogValue & 0xFF;           // Lower byte
  data[1] = (analogValue >> 8) & 0xFF;    // Upper byte

  // --- Serial output: print analog reading and payload bytes ---
  Serial.print("[t=");
  Serial.print(millis());
  Serial.print(" ms] Analog A0 = ");
  Serial.print(analogValue);
  Serial.print("  |  Adv payload bytes: 0x");
  if (data[0] < 0x10) Serial.print('0');
  Serial.print(data[0], HEX);
  Serial.print(" 0x");
  if (data[1] < 0x10) Serial.print('0');
  Serial.println(data[1], HEX);
  // -----------------------------------------------------------

  // Add the analog value to the advertisement data
  Bluefruit.Advertising.addData(BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA, data, sizeof(data));

  // Configure advertising interval and timeout
  Bluefruit.Advertising.setInterval(160, 160);  // Interval: 100ms
  Bluefruit.Advertising.setFastTimeout(500);    // Timeout: 1 second
  Bluefruit.Advertising.start(0);                 // Start advertising indefinitely
}

// Callback when a device connects
void connect_callback(uint16_t conn_handle) {
  Serial.print("[t=");
  Serial.print(millis());
  Serial.print(" ms] Connected! Handle=");
  Serial.println(conn_handle);
  // Send a notification with the current analog value encoded as ASCII for easy reading
  int analogValue = analogRead(A0);
  char notifBuf[16];
  snprintf(notifBuf, sizeof(notifBuf), "A0=%d", analogValue);
  Serial.print("  Notify sent: ");
  Serial.println(notifBuf);
  bleCharacteristic.notify(conn_handle, (uint8_t*)notifBuf, strlen(notifBuf));
}

// Callback when a device disconnects
void disconnect_callback(uint16_t conn_handle, uint8_t reason) {
  Serial.print("[t=");
  Serial.print(millis());
  Serial.print(" ms] Disconnected. Handle=");
  Serial.print(conn_handle);
  Serial.print("  Reason=0x");
  Serial.println(reason, HEX);
  Serial.println("  Restarting advertising...");
  startAdvertising(); // Restart advertising
}

void setup() {
  Serial.begin(115200);        // Initialize Serial for debugging
  Bluefruit.begin();           // Initialize Bluefruit module
  Bluefruit.setName("MyBLE"); // Set device name
  Serial.println("Bluefruit Begin!!!");
  // Set up the BLE service and characteristic
  bleService.begin();
  bleCharacteristic.setProperties(CHR_PROPS_NOTIFY); // Enable notifications
  bleCharacteristic.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS); // No read access
  bleCharacteristic.setFixedLen(11);  // Set fixed length for notifications
  bleCharacteristic.begin();

  // Set connection and disconnection callbacks
  Bluefruit.Periph.setConnectCallback(connect_callback);
  Bluefruit.Periph.setDisconnectCallback(disconnect_callback);

  startAdvertising(); // Start advertising
  Serial.println("Advertising Begin!!!");
  Serial.println("------------------------------------");
  Serial.println(" nRF BLE Piezo Advertiser Running");
  Serial.println(" Device name : MyBLE");
  Serial.println(" Interval    : 160 units (~100 ms)");
  Serial.println(" Data pin    : A0");
  Serial.println("------------------------------------");
}

void loop() {
  static unsigned long lastUpdate = 0;
  unsigned long currentMillis = millis();

  // Update advertisement payload every 100 ms and print to Serial
  if (currentMillis - lastUpdate >= 100) {
    lastUpdate = currentMillis;
    startAdvertising();
  }
}
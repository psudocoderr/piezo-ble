#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEClient.h>
#include <WiFi.h>
#include <time.h>
#include <sys/time.h>

const char* ssid = "YOUR_SSID";
const char* password = "YOUR_PASSWORD";

BLEUUID serviceUUID("ABCD1234-0000-467A-9538-01F0652C74E0");
BLEUUID charUUID("ABCD1234-0001-467A-9538-01F0652C74E0");

BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pCharacteristic = nullptr;

uint32_t sampleIndex = 0;
uint32_t totalSamples = 0;
uint32_t lastReport = 0;

// ----------------------------------------------------------------
// Flush gate: discard the first few notifications after every
// fresh subscription instead of trusting them immediately.
// ----------------------------------------------------------------
const uint8_t FLUSH_PACKETS = 3;
uint8_t flushCounter = 0;

// nRF52840 default ADC is 12-bit -> 0-4095. Anything outside that
// range cannot be a real analogRead() sample, so treat it as a
// corrupted packet rather than data.
const uint16_t ADC_MAX = 4095;

// ----------------------------------------------------------------
// Client callbacks (single reusable instance, no per-connect leak)
// ----------------------------------------------------------------
class ClientCallbacks : public BLEClientCallbacks {
    void onConnect(BLEClient* client) override {
        // no-op, connect() return value already tells us this
    }
    void onDisconnect(BLEClient* client) override {
        Serial.println("Disconnected (callback)");
    }
};
ClientCallbacks clientCallbacks;

//------------------------------------------------------------
// Notification callback
//------------------------------------------------------------
void notifyCallback(
    BLERemoteCharacteristic* pChar,
    uint8_t* pData,
    size_t length,
    bool isNotify)
{
    if (length != 20)
        return;

    if (flushCounter < FLUSH_PACKETS)
    {
        flushCounter++;
        Serial.println("Flushing stale packet...");
        return;
    }

    uint16_t samples[10];
    memcpy(samples, pData, sizeof(samples));

    // Validate packet
    for (int i = 0; i < 10; i++)
    {
        if (samples[i] > ADC_MAX)
        {
            Serial.println("Out-of-range packet discarded");
            return;
        }
    }

    String timestamp = getTimestamp();

    for (int i = 0; i < 10; i++)
    {
        Serial.print(timestamp);
        Serial.print(",");
        Serial.println(samples[i]);
    }

    totalSamples += 10;

    uint32_t now = millis();

    if (now - lastReport >= 1000)
    {
        float rate = totalSamples * 1000.0f / (now - lastReport);

        Serial.println("--------------------------------");
        Serial.print("Receive Rate : ");
        Serial.print(rate, 1);
        Serial.println(" samples/sec");
        Serial.println("--------------------------------");

        totalSamples = 0;
        lastReport = now;
    }
}

void cleanupClient()
{
    if (pClient != nullptr)
    {
        if (pClient->isConnected())
            pClient->disconnect();

        delete pClient;

        pClient = nullptr;
    }

    pCharacteristic = nullptr;
}

bool connectPeripheral()
{
    cleanupClient(); // always start an attempt from a clean slate

    BLEScan* scan = BLEDevice::getScan();
    scan->setActiveScan(true);

    Serial.println("Scanning...");

    BLEScanResults results = *scan->start(5);

    bool found = false;
    BLEAdvertisedDevice targetDevice;

    for (int i = 0; i < results.getCount(); i++)
        {
            BLEAdvertisedDevice device = results.getDevice(i);

            // Print every discovered BLE device
            Serial.print("Found: ");
            Serial.print(device.getAddress().toString().c_str());
            Serial.print("  Name: ");
            Serial.println(device.getName().c_str());

            if (device.isAdvertisingService(serviceUUID))
            {
                targetDevice = device;
                found = true;
                break;
            }
        }
        
    scan->clearResults(); // free scan buffer every cycle, don't let it accumulate

    if (!found)
        return false;

    Serial.print("Found: ");
    Serial.println(targetDevice.getAddress().toString().c_str());

    pClient = BLEDevice::createClient();
    pClient->setClientCallbacks(&clientCallbacks);

    if (!pClient->connect(&targetDevice))
    {
        Serial.println("Connection Failed");
        cleanupClient();
        return false;
    }

    Serial.println("Connected!");

    BLERemoteService* service = pClient->getService(serviceUUID);

    if (!service)
    {
        Serial.println("Service not found");
        cleanupClient();
        return false;
    }

    pCharacteristic = service->getCharacteristic(charUUID);

    if (!pCharacteristic)
    {
        Serial.println("Characteristic not found");
        cleanupClient();
        return false;
    }

    if (!pCharacteristic->canNotify())
    {
        Serial.println("Notify not supported");
        cleanupClient();
        return false;
    }

    flushCounter = 0; // reset the flush gate for this fresh connection
    pCharacteristic->registerForNotify(notifyCallback);

    BLERemoteDescriptor* cccd = pCharacteristic->getDescriptor(BLEUUID((uint16_t)0x2902));

    if (cccd != nullptr)
    {
        cccd->writeValue((uint8_t[]){0x01, 0x00}, 2, true);
    }
    else
    {
        Serial.println("Warning: CCCD descriptor not found, notify may not work");
    }

    Serial.println("Notifications Enabled");

    totalSamples = 0;
    lastReport = millis();

    return true;
}

//------------------------------------------------------------
// WiFi + NTP
//------------------------------------------------------------
void setupTime()
{
    Serial.print("Connecting to WiFi");

    WiFi.begin(ssid, password);

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }

    Serial.println("\nWiFi Connected");

    // IST (UTC+5:30)
    configTime(19800, 0, "pool.ntp.org", "time.nist.gov");

    struct tm timeinfo;

    Serial.print("Synchronizing time");

    while (!getLocalTime(&timeinfo))
    {
        Serial.print(".");
        delay(500);
    }

    Serial.println("\nTime synchronized");
}

String getTimestamp()
{
    struct tm timeinfo;

    if (!getLocalTime(&timeinfo))
        return "0000-00-00 00:00:00.000";

    struct timeval tv;
    gettimeofday(&tv, nullptr);

    char buffer[32];

    sprintf(buffer,
            "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
            timeinfo.tm_year + 1900,
            timeinfo.tm_mon + 1,
            timeinfo.tm_mday,
            timeinfo.tm_hour,
            timeinfo.tm_min,
            timeinfo.tm_sec,
            tv.tv_usec / 1000);

    return String(buffer);
}

//------------------------------------------------------------
// Setup
//------------------------------------------------------------
void setup()
{
    Serial.begin(115200);

    Serial.println("--------------------------------");
    Serial.println("ESP32 BLE Receiver");
    Serial.println("Expected Payload : 20 Bytes");
    Serial.println("10 Samples / Packet");
    Serial.println("--------------------------------");

    setupTime();

    BLEDevice::init("ESP32_Receiver");
    BLEDevice::setMTU(247);

    while (!connectPeripheral())
    {
        Serial.println("Retrying...");
        delay(1000);
    }
}

//------------------------------------------------------------
// Loop
//------------------------------------------------------------
void loop()
{
    if (pClient != nullptr && !pClient->isConnected())
    {
        Serial.println("Disconnected!");

        while (!connectPeripheral())
        {
            Serial.println("Retrying...");
            delay(1000);
        }
    }

    delay(10);
}

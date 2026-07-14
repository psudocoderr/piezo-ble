#include <NimBLEDevice.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <inttypes.h>
#include <time.h>
#include <sys/time.h>
#include "esp_heap_caps.h"
#include "secrets.h"

NimBLEUUID serviceUUID("ABCD1234-0000-467A-9538-01F0652C74E0");
NimBLEUUID charUUID("ABCD1234-0001-467A-9538-01F0652C74E0");

const char* TARGET_DEVICE_NAME = "MyBLE";

NimBLEClient* pClient = nullptr;
NimBLERemoteCharacteristic* pCharacteristic = nullptr;

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
// HTTP batch upload
// ----------------------------------------------------------------
struct NotifyBatch {          // one queue item per BLE notify (10 samples)
    time_t   epochSec;
    uint16_t ms;
    uint16_t values[10];
};

struct FlatSample {           // one entry in the upload buffer
    time_t   epochSec;
    uint16_t ms;
    uint16_t value;
};

enum class UploadResult : uint8_t
{
    Ok,
    Retry,
    Drop
};

const size_t BATCH_SIZE = 200; // samples per POST (20 notifies, ~1s @ 200Hz)
const TickType_t FLUSH_TIMEOUT = pdMS_TO_TICKS(1000); // partial-flush if no full batch in 1s
const size_t JSON_PAYLOAD_CAPACITY = 8192;

QueueHandle_t      sampleQueue;
WiFiClientSecure   netClient;

// Upload diagnostics
volatile uint32_t droppedBatches = 0;
uint32_t          droppedUploads = 0;
uint32_t          uploadedSamples = 0;
uint32_t          lastUploadReport = 0;

bool appendText(char* out, size_t cap, size_t& len, const char* text);
bool appendSampleJson(char* out, size_t cap, size_t& len, const FlatSample& sample);
bool buildPayload(char* out, size_t cap, FlatSample* buf, size_t count, size_t& len);
UploadResult postBatch(HTTPClient& http, FlatSample* buf, size_t count);

// ----------------------------------------------------------------
// Client callbacks (single reusable instance, no per-connect leak)
// NimBLEClientCallbacks::onDisconnect takes an extra `reason` code
// vs the Bluedroid version - that's new in NimBLE-Arduino v2.
// ----------------------------------------------------------------
class ClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* client) override {
        // no-op, connect() return value already tells us this
    }
    void onDisconnect(NimBLEClient* client, int reason) override {
        Serial.printf("Disconnected (callback), reason = %d\n", reason);
    }
};
ClientCallbacks clientCallbacks;

//------------------------------------------------------------
// Notification callback
//------------------------------------------------------------
void notifyCallback(
    NimBLERemoteCharacteristic* pChar,
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

    NotifyBatch nb;
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    nb.epochSec = tv.tv_sec;
    nb.ms = tv.tv_usec / 1000;
    memcpy(nb.values, samples, sizeof(samples));

    if (xQueueSend(sampleQueue, &nb, 0) != pdTRUE)
    {
        droppedBatches++;
        Serial.println("Upload queue full, batch dropped");
    }

    totalSamples += 10;

    uint32_t now = millis();

    if (now - lastReport >= 1000)
    {
        float rate = totalSamples * 1000.0f / (now - lastReport);

        Serial.println("--------------------------------");
        Serial.printf("BLE Rx Rate  : %.1f samples/sec\n", rate);
        Serial.printf("Queue Used   : %u / 100\n",
                      (unsigned)(100 - uxQueueSpacesAvailable(sampleQueue)));
        Serial.printf("Dropped      : %u batches\n", (unsigned)droppedBatches);
        Serial.printf("Free Heap    : %u bytes\n", (unsigned)ESP.getFreeHeap());
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
        // Reuse the same client object across reconnects, same as before -
        // NimBLE handles client teardown more cleanly than Bluedroid did,
        // but there's no need to churn allocations here either way.
    }

    pCharacteristic = nullptr;
}

bool connectPeripheral()
{
    cleanupClient(); // always start an attempt from a clean slate

    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);

    Serial.println("Scanning...");

    // NOTE: NimBLE's getResults() takes milliseconds, not seconds like
    // Bluedroid's start() did. This blocks for the full duration, same
    // behavior as the old scan->start(5) call.
    NimBLEScanResults results = scan->getResults(5000, false);

    bool found = false;
    const NimBLEAdvertisedDevice* targetDevice = nullptr;

    for (int i = 0; i < results.getCount(); i++)
    {
        const NimBLEAdvertisedDevice* device = results.getDevice(i);

        // Print every discovered BLE device
        Serial.print("Found: ");
        Serial.print(device->getAddress().toString().c_str());
        Serial.print("  Name: ");
        Serial.println(device->getName().c_str());

        if (device->getName() == TARGET_DEVICE_NAME &&
            device->isAdvertisingService(serviceUUID))
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
    Serial.println(targetDevice->getAddress().toString().c_str());

    if (pClient == nullptr)
    {
        pClient = NimBLEDevice::createClient();
        pClient->setClientCallbacks(&clientCallbacks);
    }

    if (!pClient->connect(targetDevice))
    {
        Serial.println("Connection Failed");
        cleanupClient();
        return false;
    }

    Serial.println("Connected!");

    NimBLERemoteService* service = pClient->getService(serviceUUID);

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

    // subscribe() replaces registerForNotify() + the manual CCCD write in
    // NimBLE v2 - it writes the 0x2902 descriptor internally, so the block
    // that used to look up BLERemoteDescriptor(0x2902) by hand is gone.
    if (!pCharacteristic->subscribe(true, notifyCallback))
    {
        Serial.println("Subscribe failed");
        cleanupClient();
        return false;
    }

    Serial.println("Notifications Enabled");

    totalSamples = 0;
    lastReport = millis();

    return true;
}

//------------------------------------------------------------
// HTTP batch upload
//------------------------------------------------------------
bool appendText(char* out, size_t cap, size_t& len, const char* text)
{
    size_t textLen = strlen(text);
    if (len + textLen >= cap)
        return false;

    memcpy(out + len, text, textLen);
    len += textLen;
    out[len] = '\0';
    return true;
}

bool appendSampleJson(char* out, size_t cap, size_t& len, const FlatSample& sample)
{
    int written = snprintf(out + len,
                           cap - len,
                           "{\"t\":%" PRId64 ",\"ms\":%u,\"v\":%u}",
                           (int64_t)sample.epochSec,
                           (unsigned)sample.ms,
                           (unsigned)sample.value);

    if (written < 0 || (size_t)written >= cap - len)
        return false;

    len += (size_t)written;
    return true;
}

bool buildPayload(char* out, size_t cap, FlatSample* buf, size_t count, size_t& len)
{
    len = 0;
    out[0] = '\0';

    if (!appendText(out, cap, len, "{\"device\":\""))
        return false;
    if (!appendText(out, cap, len, DEVICE_ID))
        return false;
    if (!appendText(out, cap, len, "\",\"samples\":["))
        return false;

    for (size_t i = 0; i < count; i++)
    {
        if (i > 0 && !appendText(out, cap, len, ","))
            return false;
        if (!appendSampleJson(out, cap, len, buf[i]))
            return false;
    }

    return appendText(out, cap, len, "]}");
}

UploadResult postBatch(HTTPClient& http, FlatSample* buf, size_t count)
{
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("WiFi down, skipping upload");
        return UploadResult::Retry;
    }

    static char payload[JSON_PAYLOAD_CAPACITY];
    size_t payloadLen = 0;

    if (!buildPayload(payload, sizeof(payload), buf, count, payloadLen))
    {
        Serial.printf("Upload payload too large for %u-byte buffer, count=%u\n",
                      (unsigned)sizeof(payload),
                      (unsigned)count);
        return UploadResult::Drop;
    }

    // Largest CONTIGUOUS block matters more than total free heap for TLS -
    // mbedTLS needs one big chunk (~16KB per direction), not scattered ones.
    Serial.printf("[HTTP] Payload: %u bytes | Free heap: %u | Largest block: %u bytes\n",
                  (unsigned)payloadLen,
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    if (!http.begin(netClient, SERVER_URL))
    {
        Serial.println("HTTP begin failed");
        return UploadResult::Retry;
    }

    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + AUTH_TOKEN);
    http.addHeader("User-Agent", "pulse-esp32-nimble/1.0");
    http.setTimeout(5000); // allow headroom for TLS + server write

    int code = http.POST((uint8_t*)payload, payloadLen);
    bool ok = (code >= 200 && code < 300);
    String response;

    if (ok)
    {
        Serial.printf("Uploaded %u samples (HTTP %d)\n", (unsigned)count, code);
    }
    else if (code < 0)
    {
        char errBuf[128] = {0};
        netClient.lastError(errBuf, sizeof(errBuf));
        Serial.printf("Upload failed: HTTP Code = %d (%s). SSL error: '%s'\n",
                      code, http.errorToString(code).c_str(), errBuf);
    }
    else
    {
        Serial.printf("Upload failed: HTTP Code = %d\n", code);
        response = http.getString();
        if (response.length() > 0)
        {
            Serial.print("Server response: ");
            Serial.println(response);
        }
    }

    http.end();   // always release the connection + TLS context, success or fail

    if (ok)
        return UploadResult::Ok;

    if (code == 530 && response.indexOf("1033") >= 0)
    {
        Serial.println("Cloudflare tunnel/origin error 1033; dropping this batch until server is fixed");
        return UploadResult::Drop;
    }

    if (code >= 400 && code < 500 && code != 408 && code != 429)
        return UploadResult::Drop;

    return UploadResult::Retry;
}

void uploadTask(void* param)
{
    static FlatSample buffer[BATCH_SIZE];
    size_t count = 0;

    HTTPClient http;
    http.setReuse(true);

    for (;;)
    {
        // ---- Greedy drain: pull everything available from the queue ----
        NotifyBatch nb;
        TickType_t wait = (count == 0) ? FLUSH_TIMEOUT : 0;  // block only if buffer empty

        while (count < BATCH_SIZE &&
               xQueueReceive(sampleQueue, &nb, wait) == pdTRUE)
        {
            for (int i = 0; i < 10 && count < BATCH_SIZE; i++)
            {
                time_t sampleSec = nb.epochSec;
                int32_t sampleMs = (int32_t)nb.ms - (9 - i) * 5;
                if (sampleMs < 0)
                {
                    sampleSec -= 1;
                    sampleMs += 1000;
                }
                buffer[count++] = { sampleSec, (uint16_t)sampleMs, nb.values[i] };
            }
            wait = 0; // subsequent receives are non-blocking
        }

        // ---- Partial flush: POST whatever we have (don't wait for full batch) ----
        if (count > 0)
        {
            UploadResult result = postBatch(http, buffer, count);

            if (result == UploadResult::Ok)
            {
                uploadedSamples += count;
                count = 0;
            }
            else if (result == UploadResult::Drop)
            {
                droppedUploads += count;
                Serial.printf("Dropping %u samples after non-retryable upload failure\n",
                              (unsigned)count);
                count = 0;
            }
            else
            {
                // Brief retry delay — short enough to not overflow the queue
                Serial.println("Retrying upload...");
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }

        // ---- Upload rate report (every 5 sec) ----
        uint32_t now = millis();
        if (now - lastUploadReport >= 5000)
        {
            Serial.printf("[Upload] sent=%u dropped=%u in last 5s (%.1f samples/sec)\n",
                          (unsigned)uploadedSamples,
                          (unsigned)droppedUploads,
                          uploadedSamples / 5.0f);
            uploadedSamples = 0;
            droppedUploads = 0;
            lastUploadReport = now;
        }
    }
}

//------------------------------------------------------------
// WiFi + NTP
//------------------------------------------------------------
void setupTime()
{
    Serial.print("Connecting to WiFi");
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        if (millis() - start > 20000) {
            Serial.println("\nWiFi timeout, restarting...");
            ESP.restart();
        }
    }
    Serial.println("\nWiFi Connected");

    configTime(19800, 0, "pool.ntp.org", "time.nist.gov");
    struct tm timeinfo;
    Serial.print("Synchronizing time");

    start = millis();
    while (!getLocalTime(&timeinfo)) {
        Serial.print(".");
        delay(500);
        if (millis() - start > 15000) {
            Serial.println("\nNTP timeout, retrying with backup server...");
            configTime(19800, 0, "time.google.com", "time.cloudflare.com");
            start = millis(); // give the new servers a fresh window
        }
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
    Serial.println("ESP32 BLE Receiver (NimBLE)");
    Serial.println("Expected Payload : 20 Bytes");
    Serial.println("10 Samples / Packet");
    Serial.print("HTTP URL         : ");
    Serial.println(SERVER_URL);
    Serial.println("--------------------------------");

    setupTime();

    // TLS requires setInsecure() or a pinned CA. Using setInsecure() for throughput.
    netClient.setInsecure();
    netClient.setHandshakeTimeout(15);

    sampleQueue = xQueueCreate(100, sizeof(NotifyBatch));  // ~5s headroom at 20 notifies/sec

    // Stack 16384: TLS handshake needs ~10 KB of stack on ESP32
    xTaskCreatePinnedToCore(uploadTask, "uploadTask", 16384, nullptr, 1, nullptr, 1);

    // No esp_bt_controller_mem_release() call needed here anymore - NimBLE
    // never compiles Classic BT in, so there's nothing to release at runtime.
    NimBLEDevice::init("ESP32_Receiver");
    NimBLEDevice::setMTU(247);

    Serial.printf("[Boot] Free heap after NimBLE init: %u | Largest block: %u bytes\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

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
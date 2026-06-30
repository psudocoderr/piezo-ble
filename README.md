# piezo-ble

Wireless piezo sensor streaming over BLE: an nRF52 peripheral samples a piezo sensor at 200 Hz and streams the data to an ESP32 central, which receives, validates, timestamps, and logs it over serial.

## Architecture

- **`nrf_peizo_ble/`** — nRF52 (Bluefruit) peripheral. Samples `A0` at 200 Hz, batches 10 samples into a 20-byte payload, and pushes it via BLE GATT notify.
- **`esp32_ble_scanner/`** — ESP32 central. Connects to the nRF52 as a GATT client, subscribes to notifications, validates and timestamps each sample, and prints it as CSV over serial. Also syncs wall-clock time over WiFi/NTP so timestamps are real dates, not just `millis()`.

Both sides share a custom BLE service/characteristic UUID pair so the ESP32 can find and subscribe to the right peripheral.

## Data flow

1. nRF52 samples the piezo sensor every 5 ms (200 Hz).
2. Every 10 samples (50 ms), it sends a single 20-byte BLE notification (`uint16_t[10]`).
3. ESP32 receives the notification and:
   - discards the first few packets after every fresh subscription (stale-data flush gate), since the BLE stack can deliver buffered/garbage data right after a notify subscription;
   - rejects any packet containing a value outside the nRF52's 12-bit ADC range (0–4095) as corrupted rather than treating it as real data;
   - timestamps each surviving sample using NTP-synced wall-clock time and prints `timestamp,value` lines over serial;
   - reports a rolling samples/sec rate once per second.
4. If the BLE link drops, the ESP32 tears down the client cleanly and retries the scan/connect loop until it reconnects.

## Version history

This repo's commit history tracks the project's evolution:

- **v1** — Broadcast/advertising based: nRF52 advertised the sensor reading as raw advertising payload; ESP32 scanned for it. Simple but limited in throughput and reliability.
- **v2** — Switched to a connection-based architecture: explicit service/characteristic UUIDs, ESP32 connects and subscribes to notifications instead of scanning broadcasts.
- **v3** — Batched sampling: 10 samples per 20-byte notification instead of one sample per packet, tighter ~7.5 ms connection interval, and ESP32-side auto-reconnect with sample/throughput reporting.
- **v4** (current) — Data quality and timestamping: WiFi/NTP time sync for real timestamps on each sample, a flush gate to discard stale packets right after subscribing, out-of-range packet validation, and cleaner client teardown/reconnect (no per-attempt object leaks) on the ESP32 side.

## Hardware

- nRF52 board (Bluefruit/Adafruit core) with a piezo sensor on `A0`
- ESP32 board (Bluedroid BLE stack) with WiFi access for NTP time sync

## Configuration

The ESP32 sketch needs WiFi credentials to sync time over NTP:

```cpp
const char* ssid = "your-network";
const char* password = "your-password";
```

> Don't commit real credentials to a public repo — pull them from a separate untracked header (e.g. `secrets.h` added to `.gitignore`) or environment-specific config instead of hardcoding them in the sketch.

## Building

Both sketches are standard Arduino `.ino` files:

1. Open `nrf_peizo_ble/nrf_peizo_ble.ino` in the Arduino IDE, select your nRF52 board, and flash it.
2. Open `esp32_ble_scanner/esp32_ble_scanner.ino`, set your WiFi credentials, select your ESP32 board, and flash it.
3. Power both boards — the ESP32 connects to WiFi, syncs time, then scans for and connects to the nRF52, printing timestamped CSV samples over serial (115200 baud).

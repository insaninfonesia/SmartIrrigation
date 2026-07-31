/*
 * ============================================================
 *  ESP32 - SENSOR NODE (Method 3: Auto Channel Scan)
 *  Soil Moisture ──ESP-NOW──► Collector (auto channel find)
 * ============================================================
 *
 *  Change per device:
 *    Node 1 → NODE_ID = 1
 *    Node 2 → NODE_ID = 2
 *    Node 3 → NODE_ID = 3
 *
 *  Paste the Collector MAC from its Serial Monitor into
 *  COLLECTOR_MAC[] below.
 *
 *  Startup sequence:
 *    1. Scan ch.1→13, send ping on each
 *    2. Collector replies with ChannelInfo broadcast
 *    3. Node locks to that channel permanently
 *    4. Normal sensor loop begins
 *
 *  Wiring (Resistive Soil Moisture Sensor):
 *    VCC → 3.3V  |  GND → GND  |  AO → GPIO 34
 *
 *  Libraries: esp_now, esp_wifi, WiFi (all built-in)
 * ============================================================
 */

#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>

// ── NODE CONFIGURATION ────────────────────────────────────
#define NODE_ID        1           // ← CHANGE: 1, 2, or 3
#define MOISTURE_PIN   34
#define SEND_INTERVAL  5000        // ms between sends

// ── Collector MAC ─────────────────────────────────────────
// !! Replace with collector MAC from its Serial Monitor !!
uint8_t COLLECTOR_MAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ── ADC Calibration ───────────────────────────────────────
const int ADC_DRY = 3200;
const int ADC_WET = 1200;

// ── Data Structures ───────────────────────────────────────
typedef struct SensorData {
  uint8_t  nodeId;
  int      moistureRaw;
  float    moisturePct;
  char     label[16];
} SensorData;

typedef struct ChannelInfo {
  uint8_t  magic;
  uint8_t  channel;
  uint8_t  collectorMac[6];
} ChannelInfo;

// ── State ─────────────────────────────────────────────────
SensorData payload;
volatile uint8_t  lockedChannel  = 0;   // 0 = not found yet
volatile bool     channelLocked  = false;
volatile bool     lastSendOK     = false;

// ── ESP-NOW Send Callback ─────────────────────────────────
// ESP32 Arduino core 3.x changed first param to wifi_tx_info_t*
void onDataSent(const wifi_tx_info_t* info, esp_now_send_status_t status) {
  lastSendOK = (status == ESP_NOW_SEND_SUCCESS);
  if (!channelLocked) {
    // During scan phase — used to detect collector ack
    Serial.printf("[SCAN] ch.%d send %s\n",
                  lockedChannel, lastSendOK ? "ACK ✓" : "no ack");
  } else {
    Serial.printf("[ESP-NOW] Send → %s\n", lastSendOK ? "OK" : "FAIL");
  }
}

// ── ESP-NOW Receive Callback ──────────────────────────────
void onDataReceive(const esp_now_recv_info_t* info,
                   const uint8_t* data, int len) {

  // Only care about ChannelInfo packets during scan phase
  if (!channelLocked && len == sizeof(ChannelInfo)) {
    ChannelInfo* ch = (ChannelInfo*)data;
    if (ch->magic == 0xAA) {
      lockedChannel = ch->channel;
      channelLocked = true;
      Serial.printf("[SCAN] Channel broadcast received: ch.%d\n", ch->channel);
    }
  }
}

// ── Re-register collector peer on given channel ───────────
void registerCollectorPeer(uint8_t channel) {
  // Remove old peer if exists
  if (esp_now_is_peer_exist(COLLECTOR_MAC)) {
    esp_now_del_peer(COLLECTOR_MAC);
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, COLLECTOR_MAC, 6);
  peer.channel = channel;
  peer.encrypt = false;

  if (esp_now_add_peer(&peer) == ESP_OK) {
    Serial.printf("[ESP-NOW] Collector peer registered on ch.%d\n", channel);
  } else {
    Serial.println("[ESP-NOW] ERROR: Failed to register peer!");
  }
}

// ── Channel Scan ──────────────────────────────────────────
//    Tries ch.1 to ch.13, sends a ping on each.
//    Waits for either a send ACK or a ChannelInfo broadcast.
//    Locks to first channel that gets a response.
bool scanForCollector() {
  Serial.println("\n[SCAN] Scanning for collector...");

  uint8_t pingMsg[] = {'P', 'I', 'N', 'G'};

  for (uint8_t ch = 1; ch <= 13; ch++) {
    Serial.printf("[SCAN] Trying ch.%d... ", ch);

    // Switch to this channel
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    lockedChannel = ch;   // temp — for logging in callbacks
    lastSendOK    = false;

    // Register peer on this channel
    registerCollectorPeer(ch);

    // Send ping
    esp_now_send(COLLECTOR_MAC, pingMsg, sizeof(pingMsg));

    // Wait up to 400ms for ACK or channel broadcast
    unsigned long t = millis();
    while (millis() - t < 400) {
      if (channelLocked || lastSendOK) break;
      delay(10);
    }

    if (channelLocked) {
      // Got channel broadcast — switch to the real channel
      Serial.printf("\n[SCAN] ✓ Collector found via broadcast → ch.%d\n",
                    lockedChannel);
      esp_wifi_set_channel(lockedChannel, WIFI_SECOND_CHAN_NONE);
      registerCollectorPeer(lockedChannel);
      return true;
    }

    if (lastSendOK) {
      // Got send ACK on this channel — lock here
      channelLocked = true;
      lockedChannel = ch;
      Serial.printf("\n[SCAN] ✓ Collector found via ACK → ch.%d\n", ch);
      return true;
    }

    Serial.println("no response");
    esp_now_del_peer(COLLECTOR_MAC);
  }

  Serial.println("[SCAN] ✗ Collector not found on any channel!");
  return false;
}

// ── Sensor Reading ────────────────────────────────────────
int readMoistureRaw() {
  long sum = 0;
  for (int i = 0; i < 10; i++) {
    sum += analogRead(MOISTURE_PIN);
    delay(5);
  }
  return (int)(sum / 10);
}

float rawToPercent(int raw) {
  float pct = map(raw, ADC_DRY, ADC_WET, 0, 100);
  return constrain(pct, 0.0f, 100.0f);
}

// ── Setup ─────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  payload.nodeId = NODE_ID;
  snprintf(payload.label, sizeof(payload.label), "Node-%d", NODE_ID);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  Serial.println("\n============================================");
  Serial.printf("  ESP32 Sensor Node — ID: %d\n", NODE_ID);
  Serial.println("  Auto Channel Scan Mode");
  Serial.println("============================================");
  Serial.print("Node MAC      : "); Serial.println(WiFi.macAddress());
  Serial.printf("Sensor PIN    : GPIO %d\n", MOISTURE_PIN);
  Serial.printf("Send interval : %d ms\n", SEND_INTERVAL);
  Serial.print("Collector MAC : ");
  for (int i = 0; i < 6; i++) {
    Serial.printf("%02X", COLLECTOR_MAC[i]);
    if (i < 5) Serial.print(":");
  }
  Serial.println("\n============================================");

  // Init ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] Init FAILED! Restarting...");
    delay(2000); ESP.restart();
  }
  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataReceive);

  // Scan until collector found — retry every 5s if not found
  while (!scanForCollector()) {
    Serial.println("[SCAN] Retrying in 5s...");
    delay(5000);
  }

  Serial.printf("\n[NODE] Ready — locked to ch.%d\n", lockedChannel);
  Serial.println("============================================\n");
}

// ── Loop ──────────────────────────────────────────────────
unsigned long lastSend        = 0;
unsigned long lastChannelCheck = 0;
int           failCount       = 0;

void loop() {
  // If too many consecutive send failures → rescan
  if (failCount >= 5) {
    Serial.println("[NODE] Too many failures — rescanning for collector...");
    channelLocked = false;
    lockedChannel = 0;
    failCount     = 0;
    while (!scanForCollector()) {
      Serial.println("[SCAN] Retrying in 5s...");
      delay(5000);
    }
  }

  unsigned long now = millis();
  if (now - lastSend >= SEND_INTERVAL) {
    lastSend = now;

    payload.moistureRaw = readMoistureRaw();
    payload.moisturePct = rawToPercent(payload.moistureRaw);

    Serial.printf("[Node-%d] Raw: %d | Moisture: %.1f%% | ch.%d\n",
                  NODE_ID, payload.moistureRaw,
                  payload.moisturePct, lockedChannel);

    lastSendOK = false;
    esp_err_t result = esp_now_send(COLLECTOR_MAC,
                                    (uint8_t*)&payload,
                                    sizeof(payload));
    if (result != ESP_OK) {
      Serial.printf("[ESP-NOW] Send error: %d\n", result);
      failCount++;
    }

    // Wait briefly for send callback to set lastSendOK
    delay(100);
    if (!lastSendOK) failCount++;
    else             failCount = 0;
  }
}

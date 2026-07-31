/*
 * ============================================================
 *  ESP32 - DATA COLLECTOR (Method 3: Auto Channel)
 *  ESP-NOW receiver  +  Channel Broadcast  +  HTTP POST
 * ============================================================
 *
 *  Flow:
 *    1. Connect to router (STA) → locks to router channel
 *    2. Broadcast own channel via ESP-NOW so nodes auto-tune
 *    3. Receive soil data from nodes via ESP-NOW
 *    4. Forward each packet to your REST API via HTTP POST
 *
 *  No hardcoded channel needed on nodes — they scan & find us.
 *
 *  IMPORTANT:
 *    1. Flash this collector first.
 *    2. Copy its MAC address from Serial Monitor.
 *    3. Paste that MAC into COLLECTOR_MAC[] in each node.
 *
 *  Libraries needed:
 *    - ArduinoJson by Benoit Blanchon (Library Manager)
 *    - esp_now, WiFi, HTTPClient, esp_wifi (all built-in)
 * ============================================================
 */

#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ── Router Credentials ────────────────────────────────────
const char* ROUTER_SSID     = "YOUR_ROUTER_SSID";      // ← change
const char* ROUTER_PASSWORD = "YOUR_ROUTER_PASSWORD";  // ← change

// ── Your REST API Endpoint ────────────────────────────────
const char* API_URL = "https://your-api.example.com/soil/data"; // ← change
const char* API_KEY = "";   // e.g. "Bearer mytoken123" — leave "" to skip

// ── Actuator Configuration ────────────────────────────────
#define ACTUATOR_PIN        26       // ← GPIO pin connected to relay/LED
#define DRY_THRESHOLD_PCT   30.0f   // ← trigger ON if average BELOW this (%)
#define HYSTERESIS_PCT       5.0f   // ← turn OFF only when avg exceeds
                                    //   DRY_THRESHOLD + HYSTERESIS (35%)
                                    //   prevents rapid ON/OFF switching
// Timing protection
#define ACTUATOR_MIN_ON_MS   5000UL  // ← minimum ON time  (5s)  — prevents flicker
#define ACTUATOR_MIN_OFF_MS  5000UL  // ← minimum OFF time (5s)  — protects pump/relay

// ── Data Structures ───────────────────────────────────────
typedef struct SensorData {
  uint8_t  nodeId;
  int      moistureRaw;
  float    moisturePct;
  char     label[16];
} SensorData;

// Channel info packet — sent as broadcast so nodes can tune
typedef struct ChannelInfo {
  uint8_t  magic;      // 0xAA = magic marker to distinguish from sensor data
  uint8_t  channel;
  uint8_t  collectorMac[6];
} ChannelInfo;

// ── Queue (decouple ESP-NOW callback from HTTP) ───────────
#define QUEUE_SIZE 10

struct QueueItem {
  SensorData data;
  int        rssi;
  bool       used;
};

QueueItem    queue[QUEUE_SIZE];
portMUX_TYPE queueMux = portMUX_INITIALIZER_UNLOCKED;

void enqueue(const SensorData& d, int rssi) {
  portENTER_CRITICAL(&queueMux);
  for (int i = 0; i < QUEUE_SIZE; i++) {
    if (!queue[i].used) {
      queue[i].data = d;
      queue[i].rssi = rssi;
      queue[i].used = true;
      break;
    }
  }
  portEXIT_CRITICAL(&queueMux);
}

bool dequeue(QueueItem& out) {
  portENTER_CRITICAL(&queueMux);
  for (int i = 0; i < QUEUE_SIZE; i++) {
    if (queue[i].used) {
      out          = queue[i];
      queue[i].used = false;
      portEXIT_CRITICAL(&queueMux);
      return true;
    }
  }
  portEXIT_CRITICAL(&queueMux);
  return false;
}

// ── Last Data Storage per Node ───────────────────────────
// Stores the most recent reading from each node with timestamp

#define MAX_DATA_AGE_MS  30000UL   // ← max age to consider data valid for average (30s)
                                   //   change to any value e.g. 60000 = 1 minute

struct NodeLastData {
  float         moisturePct;
  int           moistureRaw;
  unsigned long receivedAt;   // millis() when data arrived
  bool          hasData;      // true after first packet received
};

NodeLastData nodeData[3] = {};   // index 0 = Node-1, 1 = Node-2, 2 = Node-3

// ── Update last data when packet arrives ──────────────────
void updateNodeData(const SensorData& d) {
  int idx = d.nodeId - 1;
  if (idx < 0 || idx > 2) return;

  nodeData[idx].moisturePct = d.moisturePct;
  nodeData[idx].moistureRaw = d.moistureRaw;
  nodeData[idx].receivedAt  = millis();
  nodeData[idx].hasData     = true;
}

// ── Average of Node-1 and Node-2 (if both data are fresh) ─
struct AverageResult {
  bool  valid;          // false if either node data is missing or too old
  float avgPct;
  int   avgRaw;
  unsigned long ageNode1Ms;   // how old is node-1 data
  unsigned long ageNode2Ms;   // how old is node-2 data
  String reason;        // why invalid, if valid=false
};

AverageResult calcAverage() {
  AverageResult result = {};
  unsigned long now    = millis();

  // Check Node-1
  if (!nodeData[0].hasData) {
    result.valid  = false;
    result.reason = "Node-1 has no data yet";
    return result;
  }

  // Check Node-2
  if (!nodeData[1].hasData) {
    result.valid  = false;
    result.reason = "Node-2 has no data yet";
    return result;
  }

  result.ageNode1Ms = now - nodeData[0].receivedAt;
  result.ageNode2Ms = now - nodeData[1].receivedAt;

  // Check Node-1 freshness (rollover-safe subtraction)
  if (result.ageNode1Ms > MAX_DATA_AGE_MS) {
    result.valid  = false;
    result.reason = "Node-1 data too old (" + String(result.ageNode1Ms / 1000) + "s)";
    return result;
  }

  // Check Node-2 freshness
  if (result.ageNode2Ms > MAX_DATA_AGE_MS) {
    result.valid  = false;
    result.reason = "Node-2 data too old (" + String(result.ageNode2Ms / 1000) + "s)";
    return result;
  }

  // Both fresh — compute average
  result.valid   = true;
  result.avgPct  = (nodeData[0].moisturePct + nodeData[1].moisturePct) / 2.0f;
  result.avgRaw  = (nodeData[0].moistureRaw + nodeData[1].moistureRaw) / 2;
  result.reason  = "OK";
  return result;
}

// ── Actuator State ────────────────────────────────────────
bool          actuatorOn        = false;
unsigned long actuatorLastOn    = 0;   // millis() when last turned ON
unsigned long actuatorLastOff   = 0;   // millis() when last turned OFF

// ── Actuator Control ──────────────────────────────────────
void setActuator(bool on) {
  unsigned long now = millis();

  if (on && !actuatorOn) {
    // Want to turn ON — check minimum OFF time first
    if ((now - actuatorLastOff) < ACTUATOR_MIN_OFF_MS) {
      Serial.printf("[ACT] ON requested but min-OFF not elapsed (%lu ms left)\n",
                    ACTUATOR_MIN_OFF_MS - (now - actuatorLastOff));
      return;
    }
    actuatorOn      = true;
    actuatorLastOn  = now;
    digitalWrite(ACTUATOR_PIN, HIGH);
    Serial.println("[ACT] ⚡ ACTUATOR ON  — soil too dry, watering started");

  } else if (!on && actuatorOn) {
    // Want to turn OFF — check minimum ON time first
    if ((now - actuatorLastOn) < ACTUATOR_MIN_ON_MS) {
      Serial.printf("[ACT] OFF requested but min-ON not elapsed (%lu ms left)\n",
                    ACTUATOR_MIN_ON_MS - (now - actuatorLastOn));
      return;
    }
    actuatorOn      = false;
    actuatorLastOff = now;
    digitalWrite(ACTUATOR_PIN, LOW);
    Serial.println("[ACT] ✋ ACTUATOR OFF — moisture restored");
  }
}

// ── Evaluate actuator based on average moisture ───────────
//    Uses hysteresis to prevent rapid switching:
//      Turn ON  when avg < DRY_THRESHOLD
//      Turn OFF when avg > DRY_THRESHOLD + HYSTERESIS
void evaluateActuator(const AverageResult& avg) {
  if (!avg.valid) {
    // Safety: if data is stale/missing, turn actuator OFF
    if (actuatorOn) {
      Serial.printf("[ACT] Average invalid (%s) — turning OFF for safety\n",
                    avg.reason.c_str());
      setActuator(false);
    }
    return;
  }

  float offThreshold = DRY_THRESHOLD_PCT + HYSTERESIS_PCT;

  Serial.printf("[ACT] Avg: %.1f%% | ON threshold: <%.1f%% | OFF threshold: >%.1f%% | State: %s\n",
                avg.avgPct, DRY_THRESHOLD_PCT, offThreshold,
                actuatorOn ? "ON" : "OFF");

  if (!actuatorOn && avg.avgPct < DRY_THRESHOLD_PCT) {
    setActuator(true);   // too dry → water
  } else if (actuatorOn && avg.avgPct > offThreshold) {
    setActuator(false);  // wet enough → stop
  }
}

// ── Print average to Serial ───────────────────────────────
void printAverage() {
  AverageResult avg = calcAverage();

  Serial.println("\n┌─────────────────────────────────────┐");
  Serial.println("│         NODE-1 + NODE-2 AVERAGE     │");
  Serial.println("├─────────────────────────────────────┤");

  if (avg.valid) {
    Serial.printf("│ Avg Moisture : %.1f%%                 \n", avg.avgPct);
    Serial.printf("│ Avg Raw ADC  : %d                    \n", avg.avgRaw);
    Serial.printf("│ Node-1 age   : %lu ms                \n", avg.ageNode1Ms);
    Serial.printf("│ Node-2 age   : %lu ms                \n", avg.ageNode2Ms);
    Serial.printf("│ Max allowed  : %lu ms                \n", MAX_DATA_AGE_MS);
    Serial.printf("│ Dry threshold: %.1f%%                 \n", DRY_THRESHOLD_PCT);
    Serial.printf("│ Actuator     : %s                    \n", actuatorOn ? "ON ⚡" : "OFF ✋");
  } else {
    Serial.printf("│ ⚠ Average INVALID: %s\n", avg.reason.c_str());
  }

  Serial.println("└─────────────────────────────────────┘\n");
}

// ── Post average to API (separate endpoint) ───────────────
void postAverageToAPI() {
  AverageResult avg = calcAverage();

  // Always evaluate actuator first — even if WiFi is down
  evaluateActuator(avg);

  if (!avg.valid) {
    Serial.printf("[AVG] Skipping upload — %s\n", avg.reason.c_str());
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[AVG] No WiFi — skipping average upload.");
    return;
  }

  StaticJsonDocument<256> doc;
  doc["type"]         = "average";
  doc["nodeIds"]      = "1,2";
  doc["avgPct"]       = serialized(String(avg.avgPct, 1));
  doc["avgRaw"]       = avg.avgRaw;
  doc["node1Pct"]     = serialized(String(nodeData[0].moisturePct, 1));
  doc["node2Pct"]     = serialized(String(nodeData[1].moisturePct, 1));
  doc["node1AgeMs"]   = avg.ageNode1Ms;
  doc["node2AgeMs"]   = avg.ageNode2Ms;
  doc["maxAgeMs"]     = MAX_DATA_AGE_MS;
  doc["actuatorOn"]   = actuatorOn;                   // ← actuator state in payload
  doc["dryThreshold"] = DRY_THRESHOLD_PCT;
  doc["collectorMac"] = WiFi.macAddress();
  doc["timestamp"]    = millis();

  String body;
  serializeJson(doc, body);

  HTTPClient http;
  http.begin(API_URL);   // can use a different endpoint if needed
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(8000);
  if (strlen(API_KEY) > 0) http.addHeader("Authorization", API_KEY);

  Serial.printf("[AVG] Posting average: %.1f%%\n", avg.avgPct);
  int code = http.POST(body);
  if (code > 0) {
    Serial.printf("[AVG] Response %d → %s\n", code, http.getString().c_str());
  } else {
    Serial.printf("[AVG] Failed: %s\n", http.errorToString(code).c_str());
  }
  http.end();
}

// ── ESP-NOW Receive Callback ──────────────────────────────
void onDataReceive(const esp_now_recv_info_t* info,
                   const uint8_t* data, int len) {

  // Ignore our own channel broadcast echoes
  if (len == sizeof(ChannelInfo)) {
    ChannelInfo* ch = (ChannelInfo*)data;
    if (ch->magic == 0xAA) return;
  }

  if (len != sizeof(SensorData)) return;

  SensorData received;
  memcpy(&received, data, sizeof(received));

  int rssi = info->rx_ctrl->rssi;

  Serial.printf("[ESP-NOW] Node-%d | Raw: %d | %.1f%% | RSSI: %d dBm\n",
                received.nodeId, received.moistureRaw,
                received.moisturePct, rssi);

  // Store last data per node (for average calculation)
  updateNodeData(received);

  enqueue(received, rssi);
}

// ── Broadcast channel so nodes can auto-tune ──────────────
void broadcastChannel() {
  uint8_t broadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

  // Add broadcast as peer if not already added
  if (!esp_now_is_peer_exist(broadcast)) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, broadcast, 6);
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
  }

  ChannelInfo info;
  info.magic   = 0xAA;
  info.channel = (uint8_t)WiFi.channel();
  uint8_t mac[6];
  esp_wifi_get_mac(WIFI_IF_STA, mac);
  memcpy(info.collectorMac, mac, 6);

  esp_now_send(broadcast, (uint8_t*)&info, sizeof(info));
  Serial.printf("[ESP-NOW] Channel broadcast sent: ch.%d\n", info.channel);
}

// ── HTTP POST ─────────────────────────────────────────────
void postToAPI(const QueueItem& item) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] No WiFi — skipping.");
    return;
  }

  StaticJsonDocument<256> doc;
  doc["nodeId"]       = item.data.nodeId;
  doc["label"]        = item.data.label;
  doc["moistureRaw"]  = item.data.moistureRaw;
  doc["moisturePct"]  = serialized(String(item.data.moisturePct, 1));
  doc["collectorMac"] = WiFi.macAddress();
  doc["rssi"]         = item.rssi;
  doc["channel"]      = WiFi.channel();
  doc["timestamp"]    = millis();

  String body;
  serializeJson(doc, body);

  HTTPClient http;
  http.begin(API_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(8000);
  if (strlen(API_KEY) > 0) http.addHeader("Authorization", API_KEY);

  int code = http.POST(body);
  if (code > 0) {
    Serial.printf("[HTTP] %d → %s\n", code, http.getString().c_str());
  } else {
    Serial.printf("[HTTP] Failed: %s\n", http.errorToString(code).c_str());
  }
  http.end();
}

// ── WiFi + ESP-NOW safe reconnect ────────────────────────
void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.println("[WiFi] Lost connection — reconnecting...");

  // Deinit ESP-NOW before changing channel
  esp_now_deinit();

  WiFi.begin(ROUTER_SSID, ROUTER_PASSWORD);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500);
    Serial.print(".");
    tries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Reconnected on ch.%d — IP: %s\n",
                  WiFi.channel(), WiFi.localIP().toString().c_str());

    // Re-init ESP-NOW on new channel
    esp_now_init();
    esp_now_register_recv_cb(onDataReceive);

    // Re-broadcast new channel to nodes
    broadcastChannel();
  } else {
    Serial.println("\n[WiFi] Failed — will retry.");
  }
}

// ── Setup ─────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);

  // ── Actuator pin init ──────────────────────────────────
  pinMode(ACTUATOR_PIN, OUTPUT);
  digitalWrite(ACTUATOR_PIN, LOW);   // start OFF — safety default
  actuatorLastOff = millis();
  Serial.printf("[ACT] Actuator pin GPIO%d initialized (OFF)\n", ACTUATOR_PIN);

  memset(queue, 0, sizeof(queue));

  WiFi.mode(WIFI_STA);
  WiFi.begin(ROUTER_SSID, ROUTER_PASSWORD);

  Serial.println("\n============================================");
  Serial.println("  ESP32 Data Collector — Auto Channel Mode");
  Serial.println("============================================");
  Serial.print("Collector MAC : ");
  Serial.println(WiFi.macAddress());
  Serial.println(">>> COPY this MAC into each node's COLLECTOR_MAC[] <<<");
  Serial.println("============================================");

  Serial.print("[WiFi] Connecting");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500); Serial.print("."); tries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected! IP: %s | Channel: %d\n",
                  WiFi.localIP().toString().c_str(), WiFi.channel());
  } else {
    Serial.println("\n[WiFi] Router not reached — ESP-NOW still works.");
  }

  // Init ESP-NOW after WiFi (channel is now fixed)
  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] Init FAILED! Restarting...");
    delay(2000); ESP.restart();
  }
  esp_now_register_recv_cb(onDataReceive);
  Serial.println("[ESP-NOW] Initialized.");

  // Broadcast channel so nodes know where to find us
  // Repeat 5x to ensure nodes catch it during their scan
  for (int i = 0; i < 5; i++) {
    broadcastChannel();
    delay(300);
  }

  Serial.printf("[API] Endpoint: %s\n", API_URL);
  Serial.println("============================================\n");
}

// ── Loop ──────────────────────────────────────────────────
unsigned long lastWiFiCheck    = 0;
unsigned long lastBroadcast    = 0;
unsigned long lastAvgReport    = 0;
#define AVG_REPORT_INTERVAL_MS  10000UL   // ← print + post average every 10s

void loop() {
  // WiFi health check every 15s
  if (millis() - lastWiFiCheck > 15000) {
    lastWiFiCheck = millis();
    ensureWiFi();
  }

  // Re-broadcast channel every 30s for late-joining nodes
  if (millis() - lastBroadcast > 30000) {
    lastBroadcast = millis();
    broadcastChannel();
  }

  // Compute and post average every AVG_REPORT_INTERVAL_MS
  if (millis() - lastAvgReport > AVG_REPORT_INTERVAL_MS) {
    lastAvgReport = millis();
    printAverage();
    postAverageToAPI();
  }

  // Drain queue
  QueueItem item;
  if (dequeue(item)) {
    postToAPI(item);
  }

  delay(10);
}

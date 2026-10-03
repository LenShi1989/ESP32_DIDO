#include "mqtt_ctrl.h"
#include "app_config.h"
#include "io_ctrl.h"
#include <WiFi.h>
#include <PubSubClient.h>

static WiFiClient   wifiClient;
static PubSubClient mqtt(wifiClient);

static String   activeClientId = "";
static uint32_t lastAttempt    = 0;
static bool     needRestart    = false;
static bool     publishAllPending = false;   // 連線後補推一次完整現況
static uint32_t lastStatusAt   = 0;

static MqttMessage msgs[MQTT_MSG_MAX];
static uint8_t     msgCount = 0;

// PubSubClient 不是 thread-safe，但 net task (loop/connect)、io task (告警發佈)
// 與 AsyncTCP web task (網頁發佈) 都會碰到同一個 socket，必須互斥。
static SemaphoreHandle_t mqttLock = nullptr;
static SemaphoreHandle_t msgLock  = nullptr;

static bool lockTake(SemaphoreHandle_t h, uint32_t ms) {
  if (!h) return false;
  return xSemaphoreTake(h, ms / portTICK_PERIOD_MS) == pdTRUE;
}
static void lockGive(SemaphoreHandle_t h) { if (h) xSemaphoreGive(h); }

// 網頁 / io task 等待鎖的上限。net task 可能正在做會阻塞的 connect()，
// 等不到就回報忙碌，不讓 AsyncTCP task 卡住。
#define MQTT_LOCK_MS 2000

// PubSubClient::connected() 在偵測到斷線時會呼叫 _client->stop()，屬於寫入操作。
// display task 每 500ms 就要查一次狀態，走鎖會互相拖累，改用 net task 維護的快取旗標。
static volatile bool connectedFlag = false;
static volatile int  stateFlag     = 0;

static void pushMessage(const String &topic, const String &payload) {
  if (!lockTake(msgLock, 100)) return;
  uint8_t keep = (msgCount < MQTT_MSG_MAX) ? msgCount : (MQTT_MSG_MAX - 1);
  for (int i = keep; i > 0; i--) msgs[i] = msgs[i - 1];
  time_t now; time(&now);
  msgs[0].ts      = now;
  msgs[0].topic   = topic;
  msgs[0].payload = payload;
  if (msgCount < MQTT_MSG_MAX) msgCount++;
  lockGive(msgLock);
}

static void callback(char *topic, byte *payload, unsigned int len) {
  String msg;
  msg.reserve(len);
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  String t(topic);
  Serial.printf("[mqtt] 收到 [%s] %s\n", t.c_str(), msg.c_str());
  pushMessage(t, msg);

  // 訂閱主題可直接控制繼電器。
  // 格式："on" / "off" / "pulse" 作用於 CH1（相容舊用法）；
  //       加通道前綴則指定通道，例如 "2:on"、"2:pulse"。
  if (t == cfg.subTopic) {
    String m = msg;
    m.trim();
    m.toLowerCase();

    uint8_t ch = 0;
    int sep = m.indexOf(':');
    if (sep > 0) {
      int n = m.substring(0, sep).toInt();
      if (n >= 1 && n <= DO_COUNT) ch = n - 1;
      m = m.substring(sep + 1);
      m.trim();
    }

    if      (m == "on"  || m == "1") { doSet(ch, true);  mqttPublishDoState(); }
    else if (m == "off" || m == "0") { doSet(ch, false); mqttPublishDoState(); }
    else if (m == "pulse")           { doPulse(ch);      mqttPublishDoState(); }
  }
}

String mqttClientId() { return activeClientId; }
bool   mqttConnected() { return connectedFlag; }

static void buildClientId() {
  if (cfg.clientIdAuto || cfg.clientId.length() == 0) {
    uint64_t mac = ESP.getEfuseMac();
    char buf[32];
    sprintf(buf, "ESP32DIDO_%04X", (uint16_t)(mac >> 32));
    activeClientId = String(buf);
  } else {
    activeClientId = cfg.clientId;
  }
}

void mqttBegin() {
  if (!mqttLock) mqttLock = xSemaphoreCreateMutex();
  if (!msgLock)  msgLock  = xSemaphoreCreateMutex();
  buildClientId();
  mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
  mqtt.setCallback(callback);
  mqtt.setBufferSize(1024);
  mqtt.setKeepAlive(30);
}

void mqttRestart() {
  needRestart = true;
}

static void connectOnce() {
  if (!cfg.mqttEnabled || cfg.mqttHost.length() == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;

  bool ok;
  if (cfg.mqttUser.length() > 0) {
    ok = mqtt.connect(activeClientId.c_str(), cfg.mqttUser.c_str(), cfg.mqttPass.c_str());
  } else {
    ok = mqtt.connect(activeClientId.c_str());
  }
  Serial.printf("[mqtt] Client [%s] 連線 %s (state=%d)\n",
                activeClientId.c_str(), ok ? "成功" : "失敗", mqtt.state());
  if (ok && cfg.subTopic.length() > 0) {
    uint8_t qos = cfg.subQos > 1 ? 1 : cfg.subQos;   // PubSubClient 最高支援 QoS1
    mqtt.subscribe(cfg.subTopic.c_str(), qos);
    Serial.printf("[mqtt] 訂閱 %s (QoS%d)\n", cfg.subTopic.c_str(), qos);
  }
  if (ok) publishAllPending = true;        // 離開鎖之後再推，避免重入
}

void mqttLoop() {
  if (!lockTake(mqttLock, portMAX_DELAY)) return;

  if (needRestart) {
    needRestart = false;
    if (mqtt.connected()) mqtt.disconnect();
    buildClientId();
    mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
    lastAttempt = 0;
  }
  if (!cfg.mqttEnabled) {
    if (mqtt.connected()) mqtt.disconnect();
    connectedFlag = false;
    stateFlag     = mqtt.state();
    lockGive(mqttLock);
    return;
  }
  if (!mqtt.connected()) {
    if (millis() - lastAttempt >= 5000) {
      lastAttempt = millis();
      connectOnce();
    }
    connectedFlag = mqtt.connected();
    stateFlag     = mqtt.state();
    lockGive(mqttLock);
    return;
  }
  mqtt.loop();
  connectedFlag = mqtt.connected();
  stateFlag     = mqtt.state();
  lockGive(mqttLock);

  // 以下會各自取鎖，必須在放鎖之後才呼叫
  if (publishAllPending) {
    publishAllPending = false;
    lastStatusAt = millis();
    mqttPublishAll();
    return;
  }
  if (cfg.mqttStatusSec && millis() - lastStatusAt >= (uint32_t)cfg.mqttStatusSec * 1000) {
    lastStatusAt = millis();
    mqttPublishStatus();
  }
}

bool mqttPublish(const String &topic, const String &payload, bool retain) {
  if (topic.length() == 0) return false;
  if (!lockTake(mqttLock, MQTT_LOCK_MS)) {
    Serial.println(F("[mqtt] 取得鎖逾時，略過發佈"));
    return false;
  }
  bool ok = false;
  if (mqtt.connected()) {
    ok = mqtt.publish(topic.c_str(), (const uint8_t *)payload.c_str(),
                      payload.length(), retain);
    if (!ok) {
      Serial.printf("[mqtt] 發佈失敗 [%s] state=%d", topic.c_str(), mqtt.state());
      Serial.println();
    }
  } else {
    Serial.println(F("[mqtt] 尚未連線，無法發佈"));
  }
  lockGive(mqttLock);
  return ok;
}

void mqttPublishAlarm(uint8_t ch, bool isAlarm, const String &text) {
  if (cfg.pubTopic.length() == 0) return;
  JSON_DOC(doc, 512);
  doc["ch"]    = ch;
  doc["alarm"] = isAlarm;
  doc["text"]  = text;
  doc["time"]  = isoTime(time(nullptr));
  String body;
  serializeJson(doc, body);
  mqttPublish(cfg.pubTopic + "/alarm", body);
}

void mqttPublishDoState() {
  if (cfg.pubTopic.length() == 0) return;
  for (int c = 0; c < DO_COUNT; c++) {
    mqttPublish(cfg.pubTopic + "/do/" + String(c + 1),
                doState(c) ? "on" : "off", cfg.mqttRetain);
  }
}

void mqttPublishDiState(uint8_t ch) {
  if (cfg.pubTopic.length() == 0 || ch >= DI_COUNT) return;
  mqttPublish(cfg.pubTopic + "/di/" + String(ch + 1),
              diAlarm(ch) ? "on" : "off", cfg.mqttRetain);
}

// 整體狀態快照，方便 Node-RED / Home Assistant 之類一次取得全部資訊
void mqttPublishStatus() {
  if (cfg.pubTopic.length() == 0) return;

  JSON_DOC(doc, 1024);
  doc["time"]   = isoTime(time(nullptr));
  doc["uptime"] = (uint32_t)(millis() / 1000);
  doc["rssi"]   = WiFi.RSSI();
  doc["ip"]     = WiFi.localIP().toString();

  JsonArray di = JSON_SUB_ARR(doc, "di");
  for (int i = 0; i < DI_COUNT; i++) {
    JsonObject o = JSON_ADD_OBJ(di);
    o["ch"]    = i + 1;
    o["name"]  = cfg.di[i].name;
    o["level"] = diRaw(i) ? 1 : 0;
    o["alarm"] = diAlarm(i);
    o["en"]    = cfg.di[i].enabled;
  }

  JsonArray dout = JSON_SUB_ARR(doc, "do");
  for (int c = 0; c < DO_COUNT; c++) {
    JsonObject o = JSON_ADD_OBJ(dout);
    o["ch"]   = c + 1;
    o["name"] = cfg.doCh[c].name;
    o["on"]   = doState(c);
    o["mode"] = cfg.doCh[c].mode;
  }

  String body;
  serializeJson(doc, body);
  mqttPublish(cfg.pubTopic + "/status", body, cfg.mqttRetain);
}

void mqttPublishAll() {
  for (int i = 0; i < DI_COUNT; i++) mqttPublishDiState(i);
  mqttPublishDoState();
  mqttPublishStatus();
}

// PubSubClient 的 state() 代碼，網頁直接顯示文字比較好判斷
static const char *mqttStateText(int st) {
  switch (st) {
    case -4: return "連線逾時";
    case -3: return "連線中斷";
    case -2: return "無法連上伺服器 (網路/位址/埠號)";
    case -1: return "已離線";
    case  0: return "已連線";
    case  1: return "通訊協定版本不符";
    case  2: return "ClientID 被拒絕";
    case  3: return "伺服器無法使用";
    case  4: return "帳號或密碼錯誤";
    case  5: return "未授權";
    default: return "未知狀態";
  }
}

String mqttStatusJson() {
  JSON_DOC(doc, 768);
  doc["enabled"]   = cfg.mqttEnabled;
  doc["connected"] = connectedFlag;
  doc["state"]     = stateFlag;
  doc["stateText"] = mqttStateText(stateFlag);
  doc["clientId"]  = activeClientId;
  doc["host"]      = cfg.mqttHost;
  doc["port"]      = cfg.mqttPort;
  doc["idAuto"]    = cfg.clientIdAuto;
  doc["user"]      = cfg.mqttUser;
  doc["pubTopic"]  = cfg.pubTopic;
  doc["pubQos"]    = cfg.pubQos;
  doc["subTopic"]  = cfg.subTopic;
  doc["subQos"]    = cfg.subQos;
  doc["effPubQos"] = 0;                                  // PubSubClient 發佈固定 QoS0
  doc["effSubQos"] = cfg.subQos > 1 ? 1 : cfg.subQos;
  doc["retain"]    = cfg.mqttRetain;
  doc["statusSec"] = cfg.mqttStatusSec;
  String out;
  serializeJson(doc, out);
  return out;
}

String mqttMessagesJson() {
  JSON_DOC(doc, 4096);
  JsonArray arr = doc.to<JsonArray>();
  if (!lockTake(msgLock, 500)) return "[]";
  for (uint8_t i = 0; i < msgCount; i++) {
    JsonObject o = JSON_ADD_OBJ(arr);
    o["time"]    = isoTime(msgs[i].ts);
    o["topic"]   = msgs[i].topic;
    o["payload"] = msgs[i].payload;
  }
  lockGive(msgLock);
  String out;
  serializeJson(doc, out);
  return out;
}

void mqttClearMessages() {
  if (!lockTake(msgLock, 500)) return;
  msgCount = 0;
  lockGive(msgLock);
}

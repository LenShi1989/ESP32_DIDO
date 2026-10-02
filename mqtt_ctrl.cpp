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

static MqttMessage msgs[MQTT_MSG_MAX];
static uint8_t     msgCount = 0;

static void pushMessage(const String &topic, const String &payload) {
  uint8_t keep = (msgCount < MQTT_MSG_MAX) ? msgCount : (MQTT_MSG_MAX - 1);
  for (int i = keep; i > 0; i--) msgs[i] = msgs[i - 1];
  time_t now; time(&now);
  msgs[0].ts      = now;
  msgs[0].topic   = topic;
  msgs[0].payload = payload;
  if (msgCount < MQTT_MSG_MAX) msgCount++;
}

static void callback(char *topic, byte *payload, unsigned int len) {
  String msg;
  msg.reserve(len);
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  String t(topic);
  Serial.printf("[mqtt] 收到 [%s] %s\n", t.c_str(), msg.c_str());
  pushMessage(t, msg);

  // 訂閱主題可直接控制繼電器
  if (t == cfg.subTopic) {
    String m = msg;
    m.trim();
    m.toLowerCase();
    if (m == "on" || m == "1") {
      doSet(true);
      mqttPublishDoState();
    } else if (m == "off" || m == "0") {
      doSet(false);
      mqttPublishDoState();
    } else if (m == "pulse") {
      doPulse();
      mqttPublishDoState();
    }
  }
}

String mqttClientId() { return activeClientId; }
bool   mqttConnected() { return mqtt.connected(); }

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
}

void mqttLoop() {
  if (needRestart) {
    needRestart = false;
    if (mqtt.connected()) mqtt.disconnect();
    buildClientId();
    mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
    lastAttempt = 0;
  }
  if (!cfg.mqttEnabled) {
    if (mqtt.connected()) mqtt.disconnect();
    return;
  }
  if (!mqtt.connected()) {
    if (millis() - lastAttempt < 5000) return;
    lastAttempt = millis();
    connectOnce();
    return;
  }
  mqtt.loop();
}

bool mqttPublish(const String &topic, const String &payload, bool retain) {
  if (!mqtt.connected() || topic.length() == 0) return false;
  return mqtt.publish(topic.c_str(), (const uint8_t *)payload.c_str(),
                      payload.length(), retain);
}

void mqttPublishAlarm(uint8_t ch, bool isAlarm, const String &text) {
  if (!mqtt.connected() || cfg.pubTopic.length() == 0) return;
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
  if (!mqtt.connected() || cfg.pubTopic.length() == 0) return;
  mqttPublish(cfg.pubTopic + "/do", doState() ? "on" : "off");
}

String mqttStatusJson() {
  JSON_DOC(doc, 768);
  doc["enabled"]   = cfg.mqttEnabled;
  doc["connected"] = mqtt.connected();
  doc["state"]     = mqtt.state();
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
  String out;
  serializeJson(doc, out);
  return out;
}

String mqttMessagesJson() {
  JSON_DOC(doc, 4096);
  JsonArray arr = doc.to<JsonArray>();
  for (uint8_t i = 0; i < msgCount; i++) {
    JsonObject o = JSON_ADD_OBJ(arr);
    o["time"]    = isoTime(msgs[i].ts);
    o["topic"]   = msgs[i].topic;
    o["payload"] = msgs[i].payload;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

void mqttClearMessages() { msgCount = 0; }

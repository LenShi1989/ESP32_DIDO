#include "app_config.h"
#include <SPIFFS.h>
#include <FS.h>

Config      cfg;
AlarmRecord alarmLog[ALARM_MAX];
uint8_t     alarmCount = 0;

// SPIFFS 會被 web task 與 IO task 同時寫入，用 mutex 保護
static SemaphoreHandle_t fsLock = nullptr;

static void fsLockTake() {
  if (!fsLock) fsLock = xSemaphoreCreateMutex();
  xSemaphoreTake(fsLock, portMAX_DELAY);
}
static void fsLockGive() {
  if (fsLock) xSemaphoreGive(fsLock);
}

String isoTime(time_t t) {
  if (t < 1600000000) return String("--");          // 尚未校時
  struct tm tmInfo;
  localtime_r(&t, &tmInfo);
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmInfo);
  return String(buf);
}

void configSetDefaults() {
  cfg.wifiSsid = "";
  cfg.wifiPass = "";
  cfg.hostname = "esp32-dido";

  // 不預設帳密：未設定前網頁免登入，首次開啟會提示使用者設定
  cfg.authUser = "";
  cfg.authPass = "";

  for (int i = 0; i < DI_COUNT; i++) {
    cfg.di[i].name       = String("DI") + (i + 1);
    cfg.di[i].enabled    = true;
    cfg.di[i].activeLow  = true;
    cfg.di[i].alarmText  = String("DI") + (i + 1) + " 觸發告警";
    cfg.di[i].normalText = String("DI") + (i + 1) + " 恢復正常";
  }

  cfg.discordEnabled  = false;
  cfg.discordWebhook  = "";
  cfg.telegramEnabled = false;
  cfg.telegramToken   = "";
  cfg.telegramChatId  = "";

  cfg.doMode      = DO_MODE_MANUAL;
  cfg.doActiveLow = true;                           // 原始硬體：拉 LOW 觸發繼電器
  cfg.pulseMs     = 1000;
  for (int i = 0; i < SCHED_COUNT; i++) {
    cfg.sched[i].enabled = false;
    cfg.sched[i].days    = 0x7F;                    // 每天
    cfg.sched[i].onH  = 8;  cfg.sched[i].onM  = 0;
    cfg.sched[i].offH = 18; cfg.sched[i].offM = 0;
  }

  cfg.mqttEnabled  = true;
  cfg.mqttHost     = "MQTTGO.io";
  cfg.mqttPort     = 1883;
  cfg.clientIdAuto = true;
  cfg.clientId     = "";
  cfg.mqttUser     = "";
  cfg.mqttPass     = "";
  cfg.pubTopic     = "esp32/len";
  cfg.pubQos       = 0;
  cfg.subTopic     = "esp32/len/receive";
  cfg.subQos       = 2;

  cfg.tftInvert   = true;                           // ZJY 1.54" IPS 需要反相
  cfg.tftRotation = 0;
  cfg.tftBgr      = true;
  cfg.tftSpiMhz   = 40;

  cfg.tz  = "CST-8";                                // 台北時區
  cfg.ntp = "pool.ntp.org";
}

bool configLoad() {
  configSetDefaults();
  if (!SPIFFS.exists(CONFIG_FILE)) {
    Serial.println(F("[cfg] 無設定檔，使用預設值"));
    return false;
  }
  File f = SPIFFS.open(CONFIG_FILE, "r");
  if (!f) return false;

  JSON_DOC(doc, 4096);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.printf("[cfg] 設定檔解析失敗: %s\n", err.c_str());
    return false;
  }

  cfg.wifiSsid = doc["wifi"]["ssid"] | cfg.wifiSsid;
  cfg.wifiPass = doc["wifi"]["pass"] | cfg.wifiPass;
  cfg.hostname = doc["wifi"]["host"] | cfg.hostname;

  cfg.authUser = doc["auth"]["user"] | cfg.authUser;
  cfg.authPass = doc["auth"]["pass"] | cfg.authPass;

  for (int i = 0; i < DI_COUNT; i++) {
    JsonObject o = doc["di"][i];
    if (o.isNull()) continue;
    cfg.di[i].name       = o["name"]   | cfg.di[i].name;
    cfg.di[i].enabled    = o["en"]     | cfg.di[i].enabled;
    cfg.di[i].activeLow  = o["low"]    | cfg.di[i].activeLow;
    cfg.di[i].alarmText  = o["alarm"]  | cfg.di[i].alarmText;
    cfg.di[i].normalText = o["normal"] | cfg.di[i].normalText;
  }

  cfg.discordEnabled  = doc["notify"]["dcEn"]  | cfg.discordEnabled;
  cfg.discordWebhook  = doc["notify"]["dcUrl"] | cfg.discordWebhook;
  cfg.telegramEnabled = doc["notify"]["tgEn"]  | cfg.telegramEnabled;
  cfg.telegramToken   = doc["notify"]["tgTok"] | cfg.telegramToken;
  cfg.telegramChatId  = doc["notify"]["tgCid"] | cfg.telegramChatId;

  cfg.doMode      = doc["do"]["mode"]    | cfg.doMode;
  cfg.doActiveLow = doc["do"]["low"]     | cfg.doActiveLow;
  cfg.pulseMs     = doc["do"]["pulseMs"] | cfg.pulseMs;
  for (int i = 0; i < SCHED_COUNT; i++) {
    JsonObject o = doc["do"]["sched"][i];
    if (o.isNull()) continue;
    cfg.sched[i].enabled = o["en"]   | cfg.sched[i].enabled;
    cfg.sched[i].days    = o["days"] | cfg.sched[i].days;
    cfg.sched[i].onH     = o["onH"]  | cfg.sched[i].onH;
    cfg.sched[i].onM     = o["onM"]  | cfg.sched[i].onM;
    cfg.sched[i].offH    = o["offH"] | cfg.sched[i].offH;
    cfg.sched[i].offM    = o["offM"] | cfg.sched[i].offM;
  }

  cfg.mqttEnabled  = doc["mqtt"]["en"]       | cfg.mqttEnabled;
  cfg.mqttHost     = doc["mqtt"]["host"]     | cfg.mqttHost;
  cfg.mqttPort     = doc["mqtt"]["port"]     | cfg.mqttPort;
  cfg.clientIdAuto = doc["mqtt"]["idAuto"]   | cfg.clientIdAuto;
  cfg.clientId     = doc["mqtt"]["id"]       | cfg.clientId;
  cfg.mqttUser     = doc["mqtt"]["user"]     | cfg.mqttUser;
  cfg.mqttPass     = doc["mqtt"]["pass"]     | cfg.mqttPass;
  cfg.pubTopic     = doc["mqtt"]["pubTopic"] | cfg.pubTopic;
  cfg.pubQos       = doc["mqtt"]["pubQos"]   | cfg.pubQos;
  cfg.subTopic     = doc["mqtt"]["subTopic"] | cfg.subTopic;
  cfg.subQos       = doc["mqtt"]["subQos"]   | cfg.subQos;

  cfg.tftInvert   = doc["tft"]["inv"] | cfg.tftInvert;
  cfg.tftRotation = doc["tft"]["rot"] | cfg.tftRotation;
  cfg.tftBgr      = doc["tft"]["bgr"] | cfg.tftBgr;
  cfg.tftSpiMhz   = doc["tft"]["mhz"] | cfg.tftSpiMhz;

  cfg.tz  = doc["sys"]["tz"]  | cfg.tz;
  cfg.ntp = doc["sys"]["ntp"] | cfg.ntp;

  Serial.println(F("[cfg] 設定檔載入完成"));
  return true;
}

static void fillDoc(JsonDocument &doc, bool includeSecrets) {
  JsonObject w = JSON_SUB_OBJ(doc, "wifi");
  w["ssid"] = cfg.wifiSsid;
  w["host"] = cfg.hostname;
  if (includeSecrets) w["pass"] = cfg.wifiPass;

  JsonObject a = JSON_SUB_OBJ(doc, "auth");
  a["user"] = cfg.authUser;
  if (includeSecrets) a["pass"] = cfg.authPass;

  JsonArray di = JSON_SUB_ARR(doc, "di");
  for (int i = 0; i < DI_COUNT; i++) {
    JsonObject o = JSON_ADD_OBJ(di);
    o["name"]   = cfg.di[i].name;
    o["en"]     = cfg.di[i].enabled;
    o["low"]    = cfg.di[i].activeLow;
    o["alarm"]  = cfg.di[i].alarmText;
    o["normal"] = cfg.di[i].normalText;
  }

  JsonObject n = JSON_SUB_OBJ(doc, "notify");
  n["dcEn"]  = cfg.discordEnabled;
  n["tgEn"]  = cfg.telegramEnabled;
  n["tgCid"] = cfg.telegramChatId;
  if (includeSecrets) {
    n["dcUrl"] = cfg.discordWebhook;
    n["tgTok"] = cfg.telegramToken;
  } else {
    // 僅回報是否已設定，不外洩內容
    n["dcUrlSet"] = cfg.discordWebhook.length() > 0;
    n["tgTokSet"] = cfg.telegramToken.length() > 0;
  }

  JsonObject d = JSON_SUB_OBJ(doc, "do");
  d["mode"]    = cfg.doMode;
  d["low"]     = cfg.doActiveLow;
  d["pulseMs"] = cfg.pulseMs;
  JsonArray sc = JSON_SUB_ARR(d, "sched");
  for (int i = 0; i < SCHED_COUNT; i++) {
    JsonObject o = JSON_ADD_OBJ(sc);
    o["en"]   = cfg.sched[i].enabled;
    o["days"] = cfg.sched[i].days;
    o["onH"]  = cfg.sched[i].onH;
    o["onM"]  = cfg.sched[i].onM;
    o["offH"] = cfg.sched[i].offH;
    o["offM"] = cfg.sched[i].offM;
  }

  JsonObject m = JSON_SUB_OBJ(doc, "mqtt");
  m["en"]       = cfg.mqttEnabled;
  m["host"]     = cfg.mqttHost;
  m["port"]     = cfg.mqttPort;
  m["idAuto"]   = cfg.clientIdAuto;
  m["id"]       = cfg.clientId;
  m["user"]     = cfg.mqttUser;
  m["pubTopic"] = cfg.pubTopic;
  m["pubQos"]   = cfg.pubQos;
  m["subTopic"] = cfg.subTopic;
  m["subQos"]   = cfg.subQos;
  if (includeSecrets) m["pass"] = cfg.mqttPass;

  JsonObject t = JSON_SUB_OBJ(doc, "tft");
  t["inv"] = cfg.tftInvert;
  t["rot"] = cfg.tftRotation;
  t["bgr"] = cfg.tftBgr;
  t["mhz"] = cfg.tftSpiMhz;

  JsonObject s = JSON_SUB_OBJ(doc, "sys");
  s["tz"]  = cfg.tz;
  s["ntp"] = cfg.ntp;
}

bool configSave() {
  JSON_DOC(doc, 4096);
  fillDoc(doc, true);
  fsLockTake();
  File f = SPIFFS.open(CONFIG_FILE, "w");
  if (!f) {
    fsLockGive();
    Serial.println(F("[cfg] 無法寫入設定檔"));
    return false;
  }
  serializeJson(doc, f);
  f.close();
  fsLockGive();
  Serial.println(F("[cfg] 設定已儲存"));
  return true;
}

String configToJson(bool includeSecrets) {
  JSON_DOC(doc, 4096);
  fillDoc(doc, includeSecrets);
  String out;
  serializeJson(doc, out);
  return out;
}

// ------------------- 告警紀錄 -------------------

void alarmLoad() {
  alarmCount = 0;
  if (!SPIFFS.exists(ALARM_FILE)) return;
  File f = SPIFFS.open(ALARM_FILE, "r");
  if (!f) return;
  JSON_DOC(doc, 4096);
  if (deserializeJson(doc, f)) { f.close(); return; }
  f.close();

  for (JsonObject o : doc.as<JsonArray>()) {
    if (alarmCount >= ALARM_MAX) break;
    alarmLog[alarmCount].ts    = (time_t)(uint32_t)(o["ts"] | 0);
    alarmLog[alarmCount].ch    = o["ch"] | 0;
    alarmLog[alarmCount].alarm = o["a"]  | false;
    alarmLog[alarmCount].text  = o["t"].as<String>();
    alarmCount++;
  }
}

void alarmSave() {
  JSON_DOC(doc, 4096);
  JsonArray arr = doc.to<JsonArray>();
  for (uint8_t i = 0; i < alarmCount; i++) {
    JsonObject o = JSON_ADD_OBJ(arr);
    o["ts"] = (uint32_t)alarmLog[i].ts;
    o["ch"] = alarmLog[i].ch;
    o["a"]  = alarmLog[i].alarm;
    o["t"]  = alarmLog[i].text;
  }
  fsLockTake();
  File f = SPIFFS.open(ALARM_FILE, "w");
  if (f) {
    serializeJson(doc, f);
    f.close();
  }
  fsLockGive();
}

// 最新的放在索引 0，超過 ALARM_MAX 時循環刪除最舊一筆
void alarmAdd(uint8_t ch, bool isAlarm, const String &text) {
  uint8_t keep = (alarmCount < ALARM_MAX) ? alarmCount : (ALARM_MAX - 1);
  for (int i = keep; i > 0; i--) alarmLog[i] = alarmLog[i - 1];
  time_t now;
  time(&now);
  alarmLog[0].ts    = now;
  alarmLog[0].ch    = ch;
  alarmLog[0].alarm = isAlarm;
  alarmLog[0].text  = text;
  if (alarmCount < ALARM_MAX) alarmCount++;
  alarmSave();
}

void alarmClear() {
  alarmCount = 0;
  SPIFFS.remove(ALARM_FILE);
}

String alarmToJson() {
  JSON_DOC(doc, 4096);
  JsonArray arr = doc.to<JsonArray>();
  for (uint8_t i = 0; i < alarmCount; i++) {
    JsonObject o = JSON_ADD_OBJ(arr);
    o["ts"]   = (uint32_t)alarmLog[i].ts;
    o["time"] = isoTime(alarmLog[i].ts);
    o["ch"]   = alarmLog[i].ch;
    o["a"]    = alarmLog[i].alarm;
    o["t"]    = alarmLog[i].text;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

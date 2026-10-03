#include "web_portal.h"
#include "app_config.h"
#include "net_wifi.h"
#include "io_ctrl.h"
#include "mqtt_ctrl.h"
#include "notify.h"
#include "display_ui.h"

#include <WiFi.h>
#include <SPIFFS.h>
#include <FS.h>
#include <Update.h>

// 需安裝 ESP32Async/ESPAsyncWebServer + ESP32Async/AsyncTCP
// (Library Manager 搜尋 "ESP Async WebServer" / "Async TCP"，作者 ESP32Async)
#include <ESPAsyncWebServer.h>

static AsyncWebServer server(80);
static bool     rebootPending = false;
static uint32_t rebootAt      = 0;

bool webRebootPending() { return rebootPending && millis() > rebootAt; }

// ---------------------------------------------------------------- 工具

// 需要登入？回傳 true 表示已擋下 (已送出 401)
static bool guard(AsyncWebServerRequest *request) {
  // 帳號或密碼任一為空視為「尚未設定」，此時不做驗證
  if (cfg.authUser.length() == 0 || cfg.authPass.length() == 0) return false;
  if (request->authenticate(cfg.authUser.c_str(), cfg.authPass.c_str())) return false;
  request->requestAuthentication();
  return true;
}

static void sendJson(AsyncWebServerRequest *request, const String &json, int code = 200) {
  AsyncWebServerResponse *res = request->beginResponse(code, "application/json", json);
  res->addHeader("Cache-Control", "no-store");
  request->send(res);
}

static void sendOk(AsyncWebServerRequest *request, const String &msg = "OK") {
  sendJson(request, String("{\"ok\":true,\"msg\":\"") + msg + "\"}");
}

static void sendErr(AsyncWebServerRequest *request, const String &msg, int code = 400) {
  sendJson(request, String("{\"ok\":false,\"msg\":\"") + msg + "\"}", code);
}

// 讀取 POST 參數 (application/x-www-form-urlencoded)，不存在時回傳預設值
static String p(AsyncWebServerRequest *r, const char *name, const String &def = "") {
  if (r->hasParam(name, true))  return r->getParam(name, true)->value();
  if (r->hasParam(name, false)) return r->getParam(name, false)->value();
  return def;
}
static bool pBool(AsyncWebServerRequest *r, const char *name, bool def) {
  if (!r->hasParam(name, true) && !r->hasParam(name, false)) return def;
  String v = p(r, name);
  v.toLowerCase();
  return v == "1" || v == "true" || v == "on" || v == "yes";
}
static long pInt(AsyncWebServerRequest *r, const char *name, long def) {
  if (!r->hasParam(name, true) && !r->hasParam(name, false)) return def;
  return p(r, name).toInt();
}

// 請求是否從 AP 介面進來 (而非從區域網路以 STA IP 連入)
static bool isFromAp(AsyncWebServerRequest *r) {
  if (!r->client()) return false;
  return r->client()->localIP() == WiFi.softAPIP();
}

// Host 標頭是否就是本機 (AP IP / STA IP / hostname)，是的話代表使用者真的要連我們
static bool isOurHost(AsyncWebServerRequest *r) {
  String h = r->host();
  int colon = h.indexOf(':');
  if (colon >= 0) h = h.substring(0, colon);
  return h == WiFi.softAPIP().toString() ||
         h == WiFi.localIP().toString()  ||
         h.equalsIgnoreCase(cfg.hostname) ||
         h.equalsIgnoreCase(cfg.hostname + ".local");
}

static String contentTypeOf(const String &path) {
  if (path.endsWith(".html") || path.endsWith(".htm")) return "text/html";
  if (path.endsWith(".css"))  return "text/css";
  if (path.endsWith(".js"))   return "application/javascript";
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".png"))  return "image/png";
  if (path.endsWith(".jpg"))  return "image/jpeg";
  if (path.endsWith(".svg"))  return "image/svg+xml";
  if (path.endsWith(".ico"))  return "image/x-icon";
  if (path.endsWith(".woff2"))return "font/woff2";
  return "text/plain";
}

// 從 SPIFFS 送出檔案，支援 .gz
static bool serveFile(AsyncWebServerRequest *request, String path) {
  if (path.endsWith("/")) path += "index.html";
  String gz = path + ".gz";
  if (SPIFFS.exists(gz)) {
    AsyncWebServerResponse *res = request->beginResponse(SPIFFS, gz, contentTypeOf(path));
    res->addHeader("Content-Encoding", "gzip");
    request->send(res);
    return true;
  }
  if (SPIFFS.exists(path)) {
    request->send(SPIFFS, path, contentTypeOf(path));
    return true;
  }
  return false;
}

// ---------------------------------------------------------------- 系統狀態

static String systemStatusJson() {
  JSON_DOC(doc, 2048);
  doc["fw"]        = FW_VERSION;
  doc["build"]     = FW_BUILD;
  doc["uptime"]    = (uint32_t)(millis() / 1000);
  doc["heap"]      = ESP.getFreeHeap();
  doc["minHeap"]   = ESP.getMinFreeHeap();
  doc["chip"]      = ESP.getChipModel();
  doc["cores"]     = ESP.getChipCores();
  doc["cpuMhz"]    = ESP.getCpuFreqMHz();
  doc["flash"]     = ESP.getFlashChipSize();
  doc["sketch"]    = ESP.getSketchSize();
  doc["sketchFree"]= ESP.getFreeSketchSpace();
  doc["sdkVer"]    = ESP.getSdkVersion();
  doc["time"]      = isoTime(time(nullptr));
  doc["mode"]      = (int)cfg.doMode;

  JsonObject fs = JSON_SUB_OBJ(doc, "fs");
  fs["total"] = SPIFFS.totalBytes();
  fs["used"]  = SPIFFS.usedBytes();
  fs["free"]  = SPIFFS.totalBytes() - SPIFFS.usedBytes();

  String out;
  serializeJson(doc, out);
  return out;
}

// SPIFFS 目錄列表
static String fsListJson() {
  JSON_DOC(doc, 4096);
  doc["total"] = SPIFFS.totalBytes();
  doc["used"]  = SPIFFS.usedBytes();
  doc["free"]  = SPIFFS.totalBytes() - SPIFFS.usedBytes();
  JsonArray arr = JSON_SUB_ARR(doc, "files");

  File root = SPIFFS.open("/");
  if (root) {
    File f = root.openNextFile();
    while (f) {
      JsonObject o = JSON_ADD_OBJ(arr);
      o["name"] = String(f.name()).startsWith("/") ? String(f.name())
                                                   : "/" + String(f.name());
      o["size"] = (uint32_t)f.size();
      o["dir"]  = f.isDirectory();
      f = root.openNextFile();
    }
  }
  String out;
  serializeJson(doc, out);
  return out;
}

// ---------------------------------------------------------------- OTA

static bool otaError    = false;
static bool otaAuthFail = false;

static void handleOtaUpload(AsyncWebServerRequest *request, const String &filename,
                            size_t index, uint8_t *data, size_t len, bool final) {
  if (index == 0) {
    otaError    = false;
    otaAuthFail = false;
    if (cfg.authUser.length() && cfg.authPass.length() &&
        !request->authenticate(cfg.authUser.c_str(), cfg.authPass.c_str())) {
      otaAuthFail = true;
      otaError    = true;
      Serial.println(F("[ota] 驗證失敗，拒絕上傳"));
      return;
    }
    bool isFs = (p(request, "target") == "spiffs") || filename.indexOf("spiffs") >= 0;
    int cmd = isFs ? U_SPIFFS : U_FLASH;
    Serial.printf("[ota] 開始更新 %s (%s)\n", filename.c_str(), isFs ? "SPIFFS" : "FIRMWARE");
    displayMessage("OTA UPDATING...", filename);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, cmd)) {
      otaError = true;
      Update.printError(Serial);
    }
  }
  if (otaAuthFail) return;
  if (!otaError && len) {
    if (Update.write(data, len) != len) {
      otaError = true;
      Update.printError(Serial);
    }
  }
  if (final) {
    if (!otaError && Update.end(true)) {
      Serial.printf("[ota] 完成，共 %u bytes\n", (unsigned)(index + len));
    } else {
      otaError = true;
      Update.printError(Serial);
    }
  }
}

// ---------------------------------------------------------------- 路由

static void setupRoutes() {
  // 【註冊順序很重要】
  // ESPAsyncWebServer 預設的 URI 比對是 BackwardCompatible：
  //     (_value == path) || path.startsWith(_value + "/")
  // 也就是 "/api/wifi" 會連 "/api/wifi/scan" 一起吃掉，而且由先註冊者勝出。
  // 因此同一前綴下，**路徑較深的必須先註冊**，否則子路由永遠不會被呼叫
  // (症狀是回傳了另一個 API 的內容，而不是 404)。

  // ---- 系統狀態 ----
  server.on("/api/fs/delete", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    String path = p(r, "path");
    if (path.length() == 0) { sendErr(r, "缺少 path"); return; }
    if (path == CONFIG_FILE) { sendErr(r, "設定檔不可刪除"); return; }
    if (!SPIFFS.exists(path)) { sendErr(r, "檔案不存在", 404); return; }
    SPIFFS.remove(path);
    sendOk(r, "已刪除 " + path);
  });

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    sendJson(r, systemStatusJson());
  });

  server.on("/api/fs", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    sendJson(r, fsListJson());
  });

  server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    rebootPending = true;
    rebootAt = millis() + 800;
    sendOk(r, "重新啟動中");
  });

  // ---- WiFi ----
  // GET 取結果 / POST 啟動掃描 (前端先 POST 再輪詢 GET)
  server.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    sendJson(r, wifiScanJson());
  });

  server.on("/api/wifi/scan", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    wifiStartScan();
    sendOk(r, "掃描中");
  });

  server.on("/api/wifi/clear", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    wifiClearConfig();                 // AP 常開，不需重開機
    sendOk(r, "連線設定已清除，請改連 AP " + wifiApSsid() + " (" + wifiApIp() + ") 重新設定");
  });

  server.on("/api/wifi", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    sendJson(r, wifiStatusJson());
  });

  server.on("/api/wifi", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    String ssid = p(r, "ssid");
    if (ssid.length() == 0) { sendErr(r, "SSID 不可空白"); return; }
    cfg.hostname = p(r, "host", cfg.hostname);
    // 不重開機：AP 全程保持開啟，前端輪詢 /api/wifi 即可看到取得的 DHCP IP
    wifiApplyNew(ssid, p(r, "pass"));
    sendOk(r, "正在連線 " + ssid + " ...");
  });

  // ---- DI ----
  server.on("/api/alarms/clear", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    alarmClear();
    sendOk(r, "告警紀錄已清除");
  });

  server.on("/api/di", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    JSON_DOC(doc, 2048);
    JsonArray arr = JSON_SUB_ARR(doc, "ch");
    for (int i = 0; i < DI_COUNT; i++) {
      JsonObject o = JSON_ADD_OBJ(arr);
      o["ch"]     = i + 1;
      o["name"]   = cfg.di[i].name;
      o["en"]     = cfg.di[i].enabled;
      o["low"]    = cfg.di[i].activeLow;
      o["alarm"]  = cfg.di[i].alarmText;
      o["normal"] = cfg.di[i].normalText;
      o["level"]  = diRaw(i) ? 1 : 0;
      o["state"]  = diAlarm(i);
    }
    JsonObject n = JSON_SUB_OBJ(doc, "notify");
    n["dcEn"]     = cfg.discordEnabled;
    n["dcUrlSet"] = cfg.discordWebhook.length() > 0;
    n["tgEn"]     = cfg.telegramEnabled;
    n["tgTokSet"] = cfg.telegramToken.length() > 0;
    n["tgCid"]    = cfg.telegramChatId;
    String out;
    serializeJson(doc, out);
    sendJson(r, out);
  });

  server.on("/api/di", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    for (int i = 0; i < DI_COUNT; i++) {
      String k = String(i);
      cfg.di[i].name       = p(r, (String("name")   + k).c_str(), cfg.di[i].name);
      cfg.di[i].alarmText  = p(r, (String("alarm")  + k).c_str(), cfg.di[i].alarmText);
      cfg.di[i].normalText = p(r, (String("normal") + k).c_str(), cfg.di[i].normalText);
      cfg.di[i].enabled    = pBool(r, (String("en")  + k).c_str(), cfg.di[i].enabled);
      cfg.di[i].activeLow  = pBool(r, (String("low") + k).c_str(), cfg.di[i].activeLow);
    }
    ioReapplyConfig();
    configSave();
    sendOk(r, "DI 設定已儲存");
  });

  server.on("/api/alarms", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    sendJson(r, alarmToJson());
  });

  // ---- 推播 ----
  server.on("/api/notify/test", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    notifyPush(p(r, "msg", "ESP32 DIDO 測試推播"));
    sendOk(r, "已送出測試推播");
  });

  server.on("/api/notify/status", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    sendJson(r, notifyLastResultJson());
  });

  server.on("/api/notify", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    cfg.discordEnabled  = pBool(r, "dcEn", cfg.discordEnabled);
    cfg.telegramEnabled = pBool(r, "tgEn", cfg.telegramEnabled);
    cfg.telegramChatId  = p(r, "tgCid", cfg.telegramChatId);
    // 密鑰欄位留白表示沿用原值
    String dc = p(r, "dcUrl");
    if (dc.length()) cfg.discordWebhook = dc;
    String tg = p(r, "tgTok");
    if (tg.length()) cfg.telegramToken = tg;
    if (pBool(r, "dcClear", false)) cfg.discordWebhook = "";
    if (pBool(r, "tgClear", false)) cfg.telegramToken  = "";
    configSave();
    sendOk(r, "推播設定已儲存");
  });

  // ---- DO ----
  server.on("/api/do/set", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    String s = p(r, "state");
    s.toLowerCase();
    if (s == "toggle") doSet(!doState());
    else               doSet(s == "on" || s == "1" || s == "true");
    mqttPublishDoState();
    sendJson(r, doStatusJson());
  });

  server.on("/api/do/pulse", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    long ms = pInt(r, "ms", cfg.pulseMs);
    doPulse(constrain(ms, 100L, 600000L));
    mqttPublishDoState();
    sendJson(r, doStatusJson());
  });

  server.on("/api/do/state", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    sendJson(r, doStatusJson());
  });

  server.on("/api/do", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    JSON_DOC(doc, 1024);
    doc["on"]      = doState();
    doc["mode"]    = cfg.doMode;
    doc["low"]     = cfg.doActiveLow;
    doc["pulseMs"] = cfg.pulseMs;
    JsonArray sc = JSON_SUB_ARR(doc, "sched");
    for (int i = 0; i < SCHED_COUNT; i++) {
      JsonObject o = JSON_ADD_OBJ(sc);
      o["en"]   = cfg.sched[i].enabled;
      o["days"] = cfg.sched[i].days;
      o["onH"]  = cfg.sched[i].onH;
      o["onM"]  = cfg.sched[i].onM;
      o["offH"] = cfg.sched[i].offH;
      o["offM"] = cfg.sched[i].offM;
    }
    String out;
    serializeJson(doc, out);
    sendJson(r, out);
  });

  server.on("/api/do", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    cfg.doMode      = (uint8_t)pInt(r, "mode", cfg.doMode);
    cfg.doActiveLow = pBool(r, "low", cfg.doActiveLow);
    long ms = pInt(r, "pulseMs", cfg.pulseMs);
    cfg.pulseMs = constrain(ms, 100L, 600000L);
    for (int i = 0; i < SCHED_COUNT; i++) {
      String k = String(i);
      cfg.sched[i].enabled = pBool(r, (String("sEn")  + k).c_str(), cfg.sched[i].enabled);
      cfg.sched[i].days    = (uint8_t)pInt(r, (String("sDays") + k).c_str(), cfg.sched[i].days);
      cfg.sched[i].onH     = (uint8_t)pInt(r, (String("sOnH")  + k).c_str(), cfg.sched[i].onH);
      cfg.sched[i].onM     = (uint8_t)pInt(r, (String("sOnM")  + k).c_str(), cfg.sched[i].onM);
      cfg.sched[i].offH    = (uint8_t)pInt(r, (String("sOffH") + k).c_str(), cfg.sched[i].offH);
      cfg.sched[i].offM    = (uint8_t)pInt(r, (String("sOffM") + k).c_str(), cfg.sched[i].offM);
    }
    ioReapplyConfig();
    configSave();
    sendOk(r, "DO 設定已儲存");
  });

  // ---- MQTT ----
  server.on("/api/mqtt/messages/clear", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    mqttClearMessages();
    sendOk(r, "訊息已清除");
  });

  server.on("/api/mqtt/publish", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    String topic = p(r, "topic", cfg.pubTopic);
    String msg   = p(r, "msg");
    if (topic.length() == 0) { sendErr(r, "Topic 不可空白"); return; }
    if (!mqttConnected())    { sendErr(r, "MQTT 尚未連線", 503); return; }
    bool ok = mqttPublish(topic, msg, pBool(r, "retain", false));
    if (ok) sendOk(r, "已發佈至 " + topic);
    else    sendErr(r, "發佈失敗", 500);
  });

  server.on("/api/mqtt/messages", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    sendJson(r, mqttMessagesJson());
  });

  server.on("/api/mqtt", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    sendJson(r, mqttStatusJson());
  });

  server.on("/api/mqtt", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    // 只有連線相關欄位變動時才重新連線，避免存檔後立刻 Publish 失敗
    bool     oldEn    = cfg.mqttEnabled;
    String   oldHost  = cfg.mqttHost;
    uint16_t oldPort  = cfg.mqttPort;
    bool     oldAuto  = cfg.clientIdAuto;
    String   oldId    = cfg.clientId;
    String   oldUser  = cfg.mqttUser;
    String   oldPw    = cfg.mqttPass;
    String   oldSub   = cfg.subTopic;
    uint8_t  oldSubQ  = cfg.subQos;

    cfg.mqttEnabled  = pBool(r, "en", cfg.mqttEnabled);
    cfg.mqttHost     = p(r, "host", cfg.mqttHost);
    cfg.mqttPort     = (uint16_t)pInt(r, "port", cfg.mqttPort);
    cfg.clientIdAuto = pBool(r, "idAuto", cfg.clientIdAuto);
    cfg.clientId     = p(r, "id", cfg.clientId);
    cfg.mqttUser     = p(r, "user", cfg.mqttUser);
    String mp = p(r, "pass");
    if (mp.length()) cfg.mqttPass = mp;
    if (pBool(r, "passClear", false)) cfg.mqttPass = "";
    cfg.pubTopic = p(r, "pubTopic", cfg.pubTopic);
    cfg.pubQos   = (uint8_t)constrain(pInt(r, "pubQos", cfg.pubQos), 0L, 2L);
    cfg.subTopic = p(r, "subTopic", cfg.subTopic);
    cfg.subQos   = (uint8_t)constrain(pInt(r, "subQos", cfg.subQos), 0L, 2L);
    configSave();
    bool reconnect = oldEn != cfg.mqttEnabled || oldHost != cfg.mqttHost ||
                     oldPort != cfg.mqttPort || oldAuto != cfg.clientIdAuto ||
                     oldId != cfg.clientId || oldUser != cfg.mqttUser ||
                     oldPw != cfg.mqttPass || oldSub != cfg.subTopic ||
                     oldSubQ != cfg.subQos;
    if (reconnect) mqttRestart();
    sendOk(r, reconnect ? "MQTT 設定已儲存，重新連線中" : "MQTT 設定已儲存");
  });

  // ---- 顯示器 ----
  server.on("/api/display/test", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    displayTestPattern();
    sendOk(r, "已顯示測試圖，15 秒後自動回到狀態畫面");
  });

  server.on("/api/display", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    JSON_DOC(doc, 256);
    doc["invert"]   = cfg.tftInvert;
    doc["rotation"] = cfg.tftRotation;
    doc["bgr"]      = cfg.tftBgr;
    doc["mhz"]      = cfg.tftSpiMhz;
    String out;
    serializeJson(doc, out);
    sendJson(r, out);
  });

  server.on("/api/display", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    cfg.tftInvert   = pBool(r, "invert", cfg.tftInvert);
    cfg.tftRotation = (uint8_t)constrain(pInt(r, "rotation", cfg.tftRotation), 0L, 3L);
    cfg.tftBgr      = pBool(r, "bgr", cfg.tftBgr);
    cfg.tftSpiMhz   = (uint8_t)constrain(pInt(r, "mhz", cfg.tftSpiMhz), 4L, 80L);
    displayApplySettings();                 // 立即生效，不必重開機
    configSave();
    sendOk(r, "顯示設定已套用");
  });

  // ---- 使用者 ----
  server.on("/api/user", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    JSON_DOC(doc, 256);
    doc["user"]       = cfg.authUser;
    doc["configured"] = cfg.authUser.length() > 0 && cfg.authPass.length() > 0;
    String out;
    serializeJson(doc, out);
    sendJson(r, out);
  });

  server.on("/api/user", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    String oldPass = p(r, "oldPass");
    if (cfg.authPass.length() && oldPass != cfg.authPass) {
      sendErr(r, "原密碼錯誤", 403);
      return;
    }
    String u = p(r, "user");
    String np = p(r, "newPass");
    if (u.length() == 0)  { sendErr(r, "帳號不可空白"); return; }
    if (np.length() < 4)  { sendErr(r, "密碼至少 4 碼"); return; }
    cfg.authUser = u;
    cfg.authPass = np;
    configSave();
    sendOk(r, "帳號密碼已更新，請以新帳密重新登入");
  });

  // ---- OTA ----
  server.on("/api/ota", HTTP_POST,
    [](AsyncWebServerRequest *r) {
      // 上傳結束後的回應
      if (otaAuthFail) { r->requestAuthentication(); return; }
      bool ok = !otaError && !Update.hasError();
      AsyncWebServerResponse *res = r->beginResponse(ok ? 200 : 500, "application/json",
          ok ? "{\"ok\":true,\"msg\":\"更新成功，裝置即將重新啟動\"}"
             : "{\"ok\":false,\"msg\":\"更新失敗\"}");
      res->addHeader("Connection", "close");
      r->send(res);
      if (ok) {
        rebootPending = true;
        rebootAt = millis() + 1000;
      } else {
        displayForceRedraw();
      }
    },
    handleOtaUpload);

  // ---- 靜態網頁 (SPIFFS) + captive portal ----
  server.onNotFound([](AsyncWebServerRequest *r) {
    if (r->method() == HTTP_OPTIONS) { r->send(200); return; }

    // 連上 AP 的裝置若在探測網路 (或輸入了別的網址)，導向設定頁，
    // 手機偵測到被導向就會自動跳出登入頁。
    // 只處理從 AP 介面進來的請求，不影響從區域網路以 STA IP 連進來的人。
    if (isFromAp(r) && !isOurHost(r)) {
      AsyncWebServerResponse *res = r->beginResponse(302, "text/plain", "");
      res->addHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
      res->addHeader("Cache-Control", "no-store");
      r->send(res);
      return;
    }

    if (guard(r)) return;
    String path = r->url();
    if (path == "/") path = "/index.html";
    if (serveFile(r, path)) return;
    // SPA 行為：未知路徑回首頁
    if (path.indexOf('.') < 0 && serveFile(r, "/index.html")) return;
    r->send(404, "text/plain",
            "404 Not Found - " + path + "\n請先上傳 data/ 資料夾至 SPIFFS");
  });
}

void webBegin() {
  setupRoutes();
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  server.begin();
  Serial.println(F("[web] 網頁伺服器已啟動 :80"));
}

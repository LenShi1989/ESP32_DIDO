#include "web_portal.h"
#include "app_config.h"
#include "net_wifi.h"
#include "io_ctrl.h"
#include "mqtt_ctrl.h"
#include "notify.h"
#include "display_ui.h"
#include "modbus_rtu.h"

#include <WiFi.h>
#include <SPIFFS.h>
#include <FS.h>
#include <Update.h>
#include <esp_random.h>

// 需安裝 ESP32Async/ESPAsyncWebServer + ESP32Async/AsyncTCP
// (Library Manager 搜尋 "ESP Async WebServer" / "Async TCP"，作者 ESP32Async)
#include <ESPAsyncWebServer.h>

static AsyncWebServer server(80);
static bool     rebootPending = false;
static uint32_t rebootAt      = 0;

bool webRebootPending() { return rebootPending && millis() > rebootAt; }

// ---------------------------------------------------------------- 忘記密碼
//
// 復原碼顯示在 ST7789 面板上，看得到面板代表人就在裝置旁邊，
// 因此這兩支 API 不需登入即可呼叫（否則忘記密碼時根本進不來）。
// 防濫用：一次有效 120 秒、最多試 5 次，逾時或試完要重新產生。

#define RECOVER_TTL_SEC   120
#define RECOVER_MAX_TRY     5

static String   recoverCode  = "";
static uint32_t recoverUntil = 0;
static uint8_t  recoverTries = 0;

static bool recoverActive() {
  return recoverCode.length() && (int32_t)(millis() - recoverUntil) < 0;
}

static void recoverStart() {
  char buf[8];
  snprintf(buf, sizeof(buf), "%06u", (unsigned)(esp_random() % 1000000));
  recoverCode  = buf;
  recoverUntil = millis() + RECOVER_TTL_SEC * 1000UL;
  recoverTries = 0;
  displayRecoveryCode(recoverCode, RECOVER_TTL_SEC);
  Serial.printf("[auth] 復原碼 %s（%d 秒內有效）", recoverCode.c_str(), RECOVER_TTL_SEC);
  Serial.println();
}

static void recoverClear() {
  recoverCode  = "";
  recoverUntil = 0;
  recoverTries = 0;
}

// 供 Serial 指令使用：直接清除帳號密碼
void authClearCredentials() {
  cfg.authUser = "";
  cfg.authPass = "";
  configSave();
  recoverClear();
  Serial.println(F("[auth] 已清除帳號密碼，網頁目前免登入"));
}

// ---------------------------------------------------------------- 工具

// 需要登入？回傳 true 表示已擋下 (已送出 401)
static bool guard(AsyncWebServerRequest *request) {
  // 帳號或密碼任一為空視為「尚未設定」，此時不做驗證
  if (cfg.authUser.length() == 0 || cfg.authPass.length() == 0) return false;
  if (request->authenticate(cfg.authUser.c_str(), cfg.authPass.c_str())) return false;

  // 取消瀏覽器的登入對話框後會看到這個頁面，順手給復原入口
  const char *body =
    "<!DOCTYPE html><html lang=\"zh-Hant\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>需要登入</title><style>"
    "body{font-family:system-ui,-apple-system,'Microsoft JhengHei',sans-serif;"
    "background:#0f1419;color:#e6edf3;display:flex;min-height:100vh;margin:0;"
    "align-items:center;justify-content:center;padding:20px}"
    "div{max-width:420px}h1{font-size:20px;margin:0 0 12px}"
    "p{color:#8b98a5;font-size:14px;line-height:1.6}"
    "a{color:#2f81f7}</style></head><body><div>"
    "<h1>需要登入</h1>"
    "<p>請重新整理頁面並輸入帳號密碼。</p>"
    "<p>忘記密碼？<a href=\"/recover.html\">使用面板復原碼重設 &rarr;</a><br>"
    "復原碼會顯示在裝置的 ST7789 面板上，需要實際接觸裝置。</p>"
    "</div></body></html>";

  AsyncWebServerResponse *res = request->beginResponse(401, "text/html", body);
  res->addHeader("WWW-Authenticate", "Basic realm=\"ESP32 DIDO\"");
  request->send(res);
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

// 取出 1-based 的 DO 通道參數並轉成 0-based 索引
static uint8_t pCh(AsyncWebServerRequest *r) {
  long n = pInt(r, "ch", 1);
  if (n < 1 || n > DO_COUNT) n = 1;
  return (uint8_t)(n - 1);
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
      o["pin"]    = diPin(i);
      o["level"]  = diRaw(i) ? 1 : 0;
      o["state"]  = diAlarm(i);
    }
    doc["ioTicks"] = ioTickCount();          // 停住不動代表 taskIo 沒在跑
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
    uint8_t ch = pCh(r);
    String s = p(r, "state");
    s.toLowerCase();
    if (s == "toggle") doSet(ch, !doState(ch));
    else               doSet(ch, s == "on" || s == "1" || s == "true");
    mqttPublishDoState();
    sendJson(r, doStatusJson());
  });

  server.on("/api/do/selftest", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    doSelfTest(pCh(r));                 // 約 2.4 秒，會阻塞這個請求
    sendJson(r, doStatusJson());
  });

  server.on("/api/do/pulse", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    uint8_t ch = pCh(r);
    long ms = pInt(r, "ms", cfg.doCh[ch].pulseMs);
    doPulse(ch, constrain(ms, 100L, 600000L));
    mqttPublishDoState();
    sendJson(r, doStatusJson());
  });

  server.on("/api/do/state", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    sendJson(r, doStatusJson());
  });

  server.on("/api/do", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    JSON_DOC(doc, 2048);
    JsonArray arr = JSON_SUB_ARR(doc, "ch");
    for (int c = 0; c < DO_COUNT; c++) {
      JsonObject d = JSON_ADD_OBJ(arr);
      d["ch"]      = c + 1;
      d["name"]    = cfg.doCh[c].name;
      d["pin"]     = doPin(c);
      d["on"]      = doState(c);
      d["mode"]    = cfg.doCh[c].mode;
      d["low"]     = cfg.doCh[c].activeLow;
      d["pulseMs"] = cfg.doCh[c].pulseMs;
      d["linkDi"]  = cfg.doCh[c].linkDi;
      d["linkAct"] = cfg.doCh[c].linkAction;
      JsonArray sc = JSON_SUB_ARR(d, "sched");
      for (int i = 0; i < SCHED_COUNT; i++) {
        JsonObject o = JSON_ADD_OBJ(sc);
        o["en"]   = cfg.sched[c][i].enabled;
        o["days"] = cfg.sched[c][i].days;
        o["onH"]  = cfg.sched[c][i].onH;
        o["onM"]  = cfg.sched[c][i].onM;
        o["offH"] = cfg.sched[c][i].offH;
        o["offM"] = cfg.sched[c][i].offM;
      }
    }
    String out;
    serializeJson(doc, out);
    sendJson(r, out);
  });

  server.on("/api/do", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    // 欄位名稱格式：c<通道>_<欄位>，例如 c0_mode、c1_sOnH2
    for (int c = 0; c < DO_COUNT; c++) {
      String pre = String("c") + c + "_";
      cfg.doCh[c].name      = p(r, (pre + "name").c_str(), cfg.doCh[c].name);
      cfg.doCh[c].mode      = (uint8_t)pInt(r, (pre + "mode").c_str(), cfg.doCh[c].mode);
      cfg.doCh[c].activeLow = pBool(r, (pre + "low").c_str(), cfg.doCh[c].activeLow);
      cfg.doCh[c].pulseMs   = constrain(pInt(r, (pre + "pulseMs").c_str(),
                                             cfg.doCh[c].pulseMs), 100L, 600000L);
      cfg.doCh[c].linkDi     = (uint8_t)constrain(pInt(r, (pre + "linkDi").c_str(),
                                                       cfg.doCh[c].linkDi), 0L, (long)DI_COUNT - 1);
      cfg.doCh[c].linkAction = (uint8_t)constrain(pInt(r, (pre + "linkAct").c_str(),
                                                       cfg.doCh[c].linkAction), 0L, (long)LINK_PULSE);
      for (int i = 0; i < SCHED_COUNT; i++) {
        String k = pre + "s";
        cfg.sched[c][i].enabled = pBool(r, (k + "En"  + i).c_str(), cfg.sched[c][i].enabled);
        cfg.sched[c][i].days    = (uint8_t)pInt(r, (k + "Days" + i).c_str(), cfg.sched[c][i].days);
        cfg.sched[c][i].onH     = (uint8_t)pInt(r, (k + "OnH"  + i).c_str(), cfg.sched[c][i].onH);
        cfg.sched[c][i].onM     = (uint8_t)pInt(r, (k + "OnM"  + i).c_str(), cfg.sched[c][i].onM);
        cfg.sched[c][i].offH    = (uint8_t)pInt(r, (k + "OffH" + i).c_str(), cfg.sched[c][i].offH);
        cfg.sched[c][i].offM    = (uint8_t)pInt(r, (k + "OffM" + i).c_str(), cfg.sched[c][i].offM);
      }
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

  server.on("/api/mqtt/pushnow", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    if (!mqttConnected()) { sendErr(r, "MQTT 尚未連線", 503); return; }
    mqttPublishAll();
    sendOk(r, "已推播 DI / DO 狀態與整體快照");
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
    cfg.mqttRetain    = pBool(r, "retain", cfg.mqttRetain);
    cfg.mqttStatusSec = (uint16_t)constrain(pInt(r, "statusSec", cfg.mqttStatusSec), 0L, 3600L);
    configSave();
    bool reconnect = oldEn != cfg.mqttEnabled || oldHost != cfg.mqttHost ||
                     oldPort != cfg.mqttPort || oldAuto != cfg.clientIdAuto ||
                     oldId != cfg.clientId || oldUser != cfg.mqttUser ||
                     oldPw != cfg.mqttPass || oldSub != cfg.subTopic ||
                     oldSubQ != cfg.subQos;
    if (reconnect) mqttRestart();
    sendOk(r, reconnect ? "MQTT 設定已儲存，重新連線中" : "MQTT 設定已儲存");
  });

  // ---- RS-485 Modbus RTU ----
  server.on("/api/modbus/poll", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    sendJson(r, modbusPollJson());
  });

  server.on("/api/modbus/reset", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    modbusPollReset();
    sendOk(r, "統計與輪詢結果已清除");
  });

  server.on("/api/modbus", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    sendJson(r, modbusStatusJson());
  });

  server.on("/api/modbus", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    cfg.mbEnabled   = pBool(r, "en", cfg.mbEnabled);
    cfg.mbMode      = (uint8_t)constrain(pInt(r, "mode", cfg.mbMode), 0L, 1L);
    cfg.mbBaud      = (uint32_t)constrain(pInt(r, "baud", cfg.mbBaud), 1200L, 921600L);
    cfg.mbParity    = (uint8_t)constrain(pInt(r, "parity", cfg.mbParity), 0L, 2L);
    cfg.mbStopBits  = (uint8_t)constrain(pInt(r, "stopBits", cfg.mbStopBits), 1L, 2L);
    cfg.mbSlaveId   = (uint8_t)constrain(pInt(r, "slaveId", cfg.mbSlaveId), 1L, 247L);
    cfg.mbTimeoutMs = (uint16_t)constrain(pInt(r, "timeout", cfg.mbTimeoutMs), 50L, 5000L);
    cfg.mbPublish   = pBool(r, "publish", cfg.mbPublish);

    for (int i = 0; i < MB_POLL_MAX; i++) {
      String pre = String("p") + i + "_";
      cfg.mbPoll[i].enabled   = pBool(r, (pre + "en").c_str(), cfg.mbPoll[i].enabled);
      cfg.mbPoll[i].name      = p(r, (pre + "name").c_str(), cfg.mbPoll[i].name);
      cfg.mbPoll[i].slaveId   = (uint8_t)constrain(pInt(r, (pre + "id").c_str(),
                                                   cfg.mbPoll[i].slaveId), 1L, 247L);
      cfg.mbPoll[i].fc        = (uint8_t)constrain(pInt(r, (pre + "fc").c_str(),
                                                   cfg.mbPoll[i].fc), 1L, 4L);
      cfg.mbPoll[i].addr      = (uint16_t)constrain(pInt(r, (pre + "addr").c_str(),
                                                    cfg.mbPoll[i].addr), 0L, 65535L);
      cfg.mbPoll[i].count     = (uint16_t)constrain(pInt(r, (pre + "count").c_str(),
                                                    cfg.mbPoll[i].count), 1L, 16L);
      cfg.mbPoll[i].periodSec = (uint16_t)constrain(pInt(r, (pre + "period").c_str(),
                                                    cfg.mbPoll[i].periodSec), 0L, 3600L);
    }
    configSave();
    modbusRestart();
    sendOk(r, "Modbus 設定已套用");
  });

  // ---- 顯示器 ----
  server.on("/api/display/test", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    displayTestPattern();
    sendOk(r, "已顯示測試圖，15 秒後自動回到狀態畫面");
  });

  server.on("/api/display/qr", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    long sec = pInt(r, "sec", 0);
    displayQrScreen((uint32_t)constrain(sec, 0L, 3600L));
    sendOk(r, "已顯示 QR 畫面");
  });

  // 與原始 sketch 相同的單一 pushImage 路徑，用來比對雜訊來源
  server.on("/api/display/splash", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    displaySplashHold();
    sendOk(r, "已顯示開機圖，15 秒後自動回到狀態畫面");
  });

  server.on("/api/display/page", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    displaySetPage((uint8_t)constrain(pInt(r, "page", cfg.tftPage), 0L, 2L));
    configSave();
    const char *n[] = { "狀態畫面", "Modbus 數值", "自動輪替" };
    sendOk(r, String("ST7789 已切換為") + n[cfg.tftPage]);
  });

  server.on("/api/display", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (guard(r)) return;
    JSON_DOC(doc, 256);
    doc["invert"]   = cfg.tftInvert;
    doc["rotation"] = cfg.tftRotation;
    doc["bgr"]      = cfg.tftBgr;
    doc["mhz"]      = cfg.tftSpiMhz;
    doc["bl"]       = cfg.tftBacklight;
    doc["qrSec"]    = cfg.qrBootSec;
    doc["page"]     = cfg.tftPage;
    doc["pageSec"]  = cfg.tftPageSec;
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
    cfg.tftBacklight = (uint8_t)constrain(pInt(r, "bl", cfg.tftBacklight), 0L, 2L);
    cfg.qrBootSec    = (uint16_t)constrain(pInt(r, "qrSec", cfg.qrBootSec), 0L, 3600L);
    cfg.tftPageSec   = (uint16_t)constrain(pInt(r, "pageSec", cfg.tftPageSec), 3L, 600L);
    displaySetPage((uint8_t)constrain(pInt(r, "page", cfg.tftPage), 0L, 2L));
    displayApplySettings();                 // 立即生效，不必重開機
    configSave();
    sendOk(r, "顯示設定已套用");
  });

  // ---- 忘記密碼（不需登入，憑面板上的復原碼）----
  server.on("/api/user/recover/start", HTTP_POST, [](AsyncWebServerRequest *r) {
    recoverStart();
    sendOk(r, "復原碼已顯示在裝置面板上，有效 120 秒");
  });

  server.on("/api/user/recover", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!recoverActive()) { sendErr(r, "復原碼已失效，請重新產生", 403); return; }
    if (recoverTries >= RECOVER_MAX_TRY) {
      recoverClear();
      sendErr(r, "嘗試次數過多，請重新產生復原碼", 429);
      return;
    }
    recoverTries++;

    if (p(r, "code") != recoverCode) {
      sendErr(r, String("復原碼錯誤（還可嘗試 ") +
                 (RECOVER_MAX_TRY - recoverTries) + " 次）", 403);
      return;
    }

    String u  = p(r, "user");
    String np = p(r, "newPass");
    if (u.length() == 0) { sendErr(r, "帳號不可空白"); return; }
    if (np.length() < 4) { sendErr(r, "密碼至少 4 碼"); return; }

    cfg.authUser = u;
    cfg.authPass = np;
    configSave();
    recoverClear();
    displayForceRedraw();
    Serial.printf("[auth] 已由復原碼重設帳號為 %s", u.c_str());
    Serial.println();
    sendOk(r, "已重設，請以新帳號密碼登入");
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

    String path = r->url();
    if (path == "/") path = "/index.html";

    // 忘記密碼頁本身不需登入，否則永遠打不開
    if (path != "/recover.html" && guard(r)) return;
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

#include "net_wifi.h"
#include "app_config.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <DNSServer.h>

// AP 一律常開 (AP_STA)，STA 連上後也不關閉，方便隨時用 192.168.4.1 回到設定頁。
static String   apSsid       = "";
static uint32_t lastRetry    = 0;
static bool     staConnected = false;

// 連上 AP 後自動跳出設定頁 (captive portal)：把所有網域都解析到 AP 自己的 IP
static DNSServer dnsServer;
static bool      dnsRunning = false;

// 套用新的 WiFi 設定，不重開機，由 wifiLoop 處理並回報取得的 DHCP IP
static bool     applyPending = false;
static uint32_t applyStart   = 0;
#define APPLY_TIMEOUT_MS 20000

// --- 掃描狀態 ---
// scanComplete() 的 -2 (WIFI_SCAN_FAILED) 同時代表「真的失敗」與「尚未登記成 RUNNING」，
// 不能一看到 -2 就當作掃完，否則第一次輪詢就會誤判成沒有任何網路。
static bool     scanning     = false;
static uint32_t scanStart    = 0;
static uint8_t  scanRetry    = 0;
static String   scanCache    = "[]";   // 最近一次成功的結果，換頁回來仍看得到
static bool     scanLastFail = false;
static int16_t  scanStartRc  = 0;      // scanNetworks() 的回傳值，供診斷用
static int      scanLastCount = -1;    // 最近一次掃到的原始筆數

// scanCache 由 net task 寫、AsyncTCP web task 讀，寫入過程中 String 會反覆
// realloc，不加鎖會讀到半成品或已釋放的記憶體。
static SemaphoreHandle_t scanLock = nullptr;

static void scanLockTake() {
  if (!scanLock) scanLock = xSemaphoreCreateMutex();
  xSemaphoreTake(scanLock, portMAX_DELAY);
}
static void scanLockGive() { if (scanLock) xSemaphoreGive(scanLock); }

#define SCAN_SETTLE_MS   2500          // 起掃後的寬限期，期間的 -2 一律視為進行中
#define SCAN_TIMEOUT_MS 25000          // 總逾時
#define SCAN_MAX_RETRY      1

static String macSuffix() {
  uint64_t mac = ESP.getEfuseMac();
  char buf[8];
  sprintf(buf, "%04X", (uint16_t)(mac >> 32));
  return String(buf);
}

// 尚未取得 STA 連線時為 true，畫面與網頁用來判斷要不要提示去設定
bool   wifiIsAp()   { return !staConnected; }
String wifiApSsid() { return apSsid; }
bool   wifiStaConnected() { return staConnected; }
String wifiApIp()   { return WiFi.softAPIP().toString(); }

static void startAp() {
  apSsid = String("ESP32-DIDO-") + macSuffix();
  WiFi.softAP(apSsid.c_str());
  delay(100);                                  // 等 softAP 取得 IP 再啟動 DNS

  IPAddress apIp = WiFi.softAPIP();
  if (!dnsRunning) {
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    // "*" = 任何網域都解析到 AP 的 IP，手機的連線偵測被導向後就會自動跳出設定頁
    dnsRunning = dnsServer.start(53, "*", apIp);
  }
  Serial.printf("[wifi] AP 常開 SSID=%s IP=%s DNS=%s",
                apSsid.c_str(), apIp.toString().c_str(), dnsRunning ? "on" : "off");
  Serial.println();
}

static bool tryConnect(const String &ssid, const String &pass, uint16_t timeoutMs) {
  if (ssid.length() == 0) return false;
  Serial.printf("[wifi] 連線 %s ...\n", ssid.c_str());
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    delay(50);                                 // GPIO2 已改作 DO2，不再閃狀態燈
  }
  return WiFi.status() == WL_CONNECTED;
}

void wifiBegin() {
  WiFi.persistent(false);
  WiFi.setHostname(cfg.hostname.c_str());
  WiFi.mode(WIFI_AP_STA);                      // AP 與 STA 同時啟用，連線成功後 AP 也不關
  startAp();

  if (tryConnect(cfg.wifiSsid, cfg.wifiPass, 15000)) {
    staConnected = true;
    Serial.printf("[wifi] 已連線 %s  DHCP IP=%s  GW=%s  RSSI=%d",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(), WiFi.RSSI());
    Serial.println();
  } else {
    staConnected = false;
    Serial.println(F("[wifi] 尚未連上無線網路，請連 AP 進行設定"));
  }
}

void wifiLoop() {
  if (dnsRunning) dnsServer.processNextRequest();   // captive portal
  wifiScanLoop();

  bool now = WiFi.status() == WL_CONNECTED;

  // 套用新設定中：等待結果並印出取得的 DHCP IP
  if (applyPending) {
    if (now) {
      applyPending = false;
      staConnected = true;
      Serial.printf("[wifi] 新設定連線成功  DHCP IP=%s", WiFi.localIP().toString().c_str());
      Serial.println();
    } else if (millis() - applyStart >= APPLY_TIMEOUT_MS) {
      applyPending = false;
      staConnected = false;
      Serial.println(F("[wifi] 新設定連線逾時"));
    }
    return;
  }

  if (now != staConnected) {
    staConnected = now;
    if (now) {
      Serial.printf("[wifi] 已連線  DHCP IP=%s", WiFi.localIP().toString().c_str());
      Serial.println();
    } else {
      Serial.println(F("[wifi] 連線中斷"));
    }
  }
  if (now) return;
  if (cfg.wifiSsid.length() == 0) return;           // 還沒設定過，等使用者從 AP 設定
  if (millis() - lastRetry < 15000) return;
  lastRetry = millis();
  Serial.println(F("[wifi] 嘗試重新連線"));
  WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
}

// 真正送出掃描指令。連線中掃描會短暫影響吞吐，屬正常現象。
static void startScanHw() {
  WiFi.scanDelete();

  // AP 常開，固定維持 AP_STA：STA 介面沒啟用掃描必定失敗，
  // AP 介面則要留著讓使用者在設定過程中不會斷線
  if (WiFi.getMode() != WIFI_AP_STA) WiFi.mode(WIFI_AP_STA);

  // 每頻道停留 300ms (預設 120ms)，弱訊號 AP 比較掃得到
  int16_t rc = WiFi.scanNetworks(true /* async */, true /* show hidden */,
                                 false /* passive */, 300 /* ms per channel */);
  // 回傳 -1 (WIFI_SCAN_RUNNING) 才代表成功啟動；-2 表示驅動層拒絕
  Serial.printf("[wifi] scanNetworks rc=%d mode=%d", rc, (int)WiFi.getMode());
  Serial.println();
  scanStartRc  = rc;
  scanning     = true;
  scanStart    = millis();
  scanLastFail = false;
  Serial.printf("[wifi] 開始掃描 (第 %d 次)\n", scanRetry + 1);
}

void wifiStartScan() {
  if (scanning && millis() - scanStart < SCAN_TIMEOUT_MS) return;   // 已在掃描中
  scanRetry = 0;
  startScanHw();
}

static const char *encStr(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-EAP";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    default:                        return "?";
  }
}

// 同一個 SSID 可能在多個頻道出現 (Mesh / 雙頻)，只留訊號最強的那筆
static void buildScanCache(int n) {
  JSON_DOC(doc, 8192);
  JsonArray arr = doc.to<JsonArray>();

  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    int32_t rssi = WiFi.RSSI(i);

    bool merged = false;
    if (ssid.length()) {                       // 隱藏網路 (空 SSID) 不合併
      for (JsonObject o : arr) {
        if (o["ssid"].as<String>() != ssid) continue;
        if (rssi > o["rssi"].as<int32_t>()) {
          o["rssi"] = rssi;
          o["ch"]   = WiFi.channel(i);
          o["enc"]  = encStr(WiFi.encryptionType(i));
        }
        merged = true;
        break;
      }
    }
    if (merged) continue;
    if (arr.size() >= 40) continue;

    JsonObject o = JSON_ADD_OBJ(arr);
    o["ssid"]   = ssid;
    o["rssi"]   = rssi;
    o["ch"]     = WiFi.channel(i);
    o["enc"]    = encStr(WiFi.encryptionType(i));
    o["hidden"] = ssid.length() == 0;
  }

  String built;
  serializeJson(arr, built);

  scanLockTake();
  scanCache = built;
  scanLockGive();
  Serial.printf("[wifi] 掃描完成：%d 筆，合併後 %u 筆\n", n, (unsigned)arr.size());
}

// 由 wifiLoop() 週期呼叫：處理寬限期、重試與逾時
void wifiScanLoop() {
  if (!scanning) return;
  uint32_t elapsed = millis() - scanStart;
  int n = WiFi.scanComplete();

  if (n >= 0) {                                 // 掃描完成
    scanLastCount = n;
    buildScanCache(n);
    WiFi.scanDelete();
    scanning     = false;
    scanLastFail = false;
    return;
  }
  if (n == WIFI_SCAN_RUNNING) return;

  // n == WIFI_SCAN_FAILED
  if (elapsed < SCAN_SETTLE_MS) return;          // 寬限期內，還在啟動
  if (scanRetry < SCAN_MAX_RETRY) {
    Serial.println(F("[wifi] 掃描未啟動，重試一次"));
    scanRetry++;
    startScanHw();
    return;
  }
  Serial.println(F("[wifi] 掃描失敗"));
  scanning     = false;
  scanLastFail = true;
}

String wifiScanJson() {
  if (scanning && millis() - scanStart >= SCAN_TIMEOUT_MS) {
    Serial.println(F("[wifi] 掃描逾時"));
    scanning     = false;
    scanLastFail = true;
  }

  scanLockTake();
  String list = scanCache;          // 取一份快照再放鎖，序列化期間不擋 net task
  scanLockGive();
  if (list.length() == 0) list = "[]";

  // 直接組字串，避免巢狀 JsonDocument 之間的複製語意問題，也省下 8KB 配置
  String out;
  out.reserve(list.length() + 160);
  out  = "{\"scanning\":";
  out += scanning ? "true" : "false";
  out += ",\"failed\":";
  out += (scanLastFail && !scanning) ? "true" : "false";
  out += ",\"elapsed\":";
  out += scanning ? (uint32_t)(millis() - scanStart) : 0;
  out += ",\"ap\":";
  out += staConnected ? "false" : "true";
  out += ",\"startRc\":";
  out += scanStartRc;
  out += ",\"rawCount\":";
  out += scanLastCount;
  out += ",\"mode\":";
  out += (int)WiFi.getMode();
  out += ",\"list\":";
  out += list;
  out += "}";

  Serial.printf("[wifi] /api/wifi/scan -> scanning=%d list=%u bytes",
                scanning ? 1 : 0, (unsigned)list.length());
  Serial.println();
  return out;
}

// 不重開機：存檔後直接改連新的 SSID，由 wifiLoop 追蹤結果，
// 前端輪詢 /api/wifi 即可看到取得的 DHCP IP。AP 全程保持開啟，不會斷線。
void wifiApplyNew(const String &ssid, const String &pass) {
  cfg.wifiSsid = ssid;
  cfg.wifiPass = pass;
  configSave();

  staConnected = false;
  applyPending = true;
  applyStart   = millis();
  WiFi.disconnect();
  delay(100);
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.printf("[wifi] 套用新設定，連線 %s ...", ssid.c_str());
  Serial.println();
}

// AP 常開，清除後不必重開機也能繼續從 192.168.4.1 設定
void wifiClearConfig() {
  cfg.wifiSsid = "";
  cfg.wifiPass = "";
  configSave();
  applyPending = false;
  staConnected = false;
  WiFi.disconnect(false, true);                // 保留 AP，只斷開 STA
  Serial.println(F("[wifi] 已清除連線設定，AP 維持開啟"));
}

String wifiStatusJson() {
  bool conn = WiFi.status() == WL_CONNECTED;

  JSON_DOC(doc, 1024);
  // --- AP (常開) ---
  doc["ap"]        = !conn;                    // 尚未連上 STA，前端用來提示去設定
  doc["apAlways"]  = true;                     // AP 連線成功後也不關閉
  doc["apSsid"]    = apSsid;
  doc["apIp"]      = WiFi.softAPIP().toString();
  doc["apClients"] = WiFi.softAPgetStationNum();
  doc["apMac"]     = WiFi.softAPmacAddress();

  // --- STA ---
  doc["connected"] = conn;
  doc["applying"]  = applyPending;             // 正在套用新設定
  doc["dhcp"]      = true;                     // 位址由 DHCP 取得
  doc["ssid"]      = conn ? WiFi.SSID() : cfg.wifiSsid;
  doc["ip"]        = conn ? WiFi.localIP().toString()   : String("");
  doc["gw"]        = conn ? WiFi.gatewayIP().toString() : String("");
  doc["mask"]      = conn ? WiFi.subnetMask().toString(): String("");
  doc["dns"]       = conn ? WiFi.dnsIP().toString()     : String("");
  doc["mac"]       = WiFi.macAddress();
  doc["rssi"]      = conn ? WiFi.RSSI() : 0;
  doc["ch"]        = WiFi.channel();
  doc["host"]      = cfg.hostname;
  String out;
  serializeJson(doc, out);
  return out;
}

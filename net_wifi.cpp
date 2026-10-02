#include "net_wifi.h"
#include "app_config.h"
#include <WiFi.h>
#include <esp_wifi.h>

static bool     apMode      = false;
static String   apSsid      = "";
static uint32_t lastRetry   = 0;

// --- 掃描狀態 ---
// scanComplete() 的 -2 (WIFI_SCAN_FAILED) 同時代表「真的失敗」與「尚未登記成 RUNNING」，
// 不能一看到 -2 就當作掃完，否則第一次輪詢就會誤判成沒有任何網路。
static bool     scanning     = false;
static uint32_t scanStart    = 0;
static uint8_t  scanRetry    = 0;
static String   scanCache    = "[]";   // 最近一次成功的結果，換頁回來仍看得到
static bool     scanLastFail = false;

#define SCAN_SETTLE_MS   2500          // 起掃後的寬限期，期間的 -2 一律視為進行中
#define SCAN_TIMEOUT_MS 25000          // 總逾時
#define SCAN_MAX_RETRY      1

static String macSuffix() {
  uint64_t mac = ESP.getEfuseMac();
  char buf[8];
  sprintf(buf, "%04X", (uint16_t)(mac >> 32));
  return String(buf);
}

bool   wifiIsAp()   { return apMode; }
String wifiApSsid() { return apSsid; }

static void startAp() {
  apMode = true;
  apSsid = String("ESP32-DIDO-") + macSuffix();
  WiFi.mode(WIFI_AP_STA);                     // AP_STA 才能同時掃描 / 測試連線
  WiFi.softAP(apSsid.c_str());
  Serial.printf("[wifi] AP 設定模式 SSID=%s IP=%s\n",
                apSsid.c_str(), WiFi.softAPIP().toString().c_str());
}

static bool tryConnect(const String &ssid, const String &pass, uint16_t timeoutMs) {
  if (ssid.length() == 0) return false;
  Serial.printf("[wifi] 連線 %s ...\n", ssid.c_str());
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    digitalWrite(LED_BUILTIN, (millis() / 200) % 2);
    delay(50);
  }
  return WiFi.status() == WL_CONNECTED;
}

void wifiBegin() {
  WiFi.persistent(false);
  WiFi.setHostname(cfg.hostname.c_str());
  WiFi.mode(WIFI_STA);

  if (tryConnect(cfg.wifiSsid, cfg.wifiPass, 15000)) {
    apMode = false;
    Serial.printf("[wifi] 已連線 IP=%s RSSI=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    digitalWrite(LED_BUILTIN, LOW);
  } else {
    Serial.println(F("[wifi] 連線失敗，啟動 AP 設定模式"));
    startAp();
  }
}

void wifiLoop() {
  wifiScanLoop();
  if (apMode) return;
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastRetry < 15000) return;
  lastRetry = millis();
  Serial.println(F("[wifi] 斷線，嘗試重新連線"));
  WiFi.disconnect();
  WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
}

// 真正送出掃描指令。連線中掃描會短暫影響吞吐，屬正常現象。
static void startScanHw() {
  WiFi.scanDelete();

  // STA 介面沒開起來時掃描必定失敗；AP 設定模式下維持 AP_STA 才能邊掃邊供人連線
  wifi_mode_t m = WiFi.getMode();
  if (apMode) {
    if (m != WIFI_AP_STA) WiFi.mode(WIFI_AP_STA);
  } else if (m == WIFI_MODE_NULL || m == WIFI_AP) {
    WiFi.mode(WIFI_STA);
  }

  // 每頻道停留 300ms (預設 120ms)，弱訊號 AP 比較掃得到
  WiFi.scanNetworks(true /* async */, true /* show hidden */,
                    false /* passive */, 300 /* ms per channel */);
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

  scanCache = "";
  serializeJson(arr, scanCache);
  Serial.printf("[wifi] 掃描完成：%d 筆，合併後 %u 筆\n", n, (unsigned)arr.size());
}

// 由 wifiLoop() 週期呼叫：處理寬限期、重試與逾時
void wifiScanLoop() {
  if (!scanning) return;
  uint32_t elapsed = millis() - scanStart;
  int n = WiFi.scanComplete();

  if (n >= 0) {                                 // 掃描完成
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

  JSON_DOC(doc, 8192);
  doc["scanning"] = scanning;
  doc["failed"]   = scanLastFail && !scanning;
  doc["elapsed"]  = scanning ? (uint32_t)(millis() - scanStart) : 0;
  doc["ap"]       = apMode;
  // 掃描期間一併回傳上次的結果，畫面不會整個清空
  JsonDocument listDoc;
  deserializeJson(listDoc, scanCache);
  doc["list"] = listDoc.as<JsonArray>();

  String out;
  serializeJson(doc, out);
  return out;
}

void wifiApplyNew(const String &ssid, const String &pass) {
  cfg.wifiSsid = ssid;
  cfg.wifiPass = pass;
  configSave();
}

void wifiClearConfig() {
  cfg.wifiSsid = "";
  cfg.wifiPass = "";
  configSave();
  WiFi.disconnect(true, true);
  delay(500);
  ESP.restart();
}

String wifiStatusJson() {
  JSON_DOC(doc, 1024);
  doc["ap"]       = apMode;
  doc["apSsid"]   = apMode ? apSsid : String("");
  doc["apIp"]     = apMode ? WiFi.softAPIP().toString() : String("");
  doc["connected"] = WiFi.status() == WL_CONNECTED;
  doc["ssid"]     = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : cfg.wifiSsid;
  doc["ip"]       = WiFi.localIP().toString();
  doc["gw"]       = WiFi.gatewayIP().toString();
  doc["mask"]     = WiFi.subnetMask().toString();
  doc["dns"]      = WiFi.dnsIP().toString();
  doc["mac"]      = WiFi.macAddress();
  doc["rssi"]     = WiFi.RSSI();
  doc["ch"]       = WiFi.channel();
  doc["host"]     = cfg.hostname;
  String out;
  serializeJson(doc, out);
  return out;
}

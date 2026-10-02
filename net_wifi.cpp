#include "net_wifi.h"
#include "app_config.h"
#include <WiFi.h>
#include <esp_wifi.h>

static bool     apMode      = false;
static String   apSsid      = "";
static uint32_t lastRetry   = 0;
static bool     scanPending = false;

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
  if (apMode) return;
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastRetry < 15000) return;
  lastRetry = millis();
  Serial.println(F("[wifi] 斷線，嘗試重新連線"));
  WiFi.disconnect();
  WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
}

void wifiStartScan() {
  if (scanPending) return;
  // AP 模式下也可掃描 (WIFI_AP_STA)
  WiFi.scanDelete();
  WiFi.scanNetworks(true /* async */, true /* show hidden */);
  scanPending = true;
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

String wifiScanJson() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return "{\"scanning\":true,\"list\":[]}";
  if (n == WIFI_SCAN_FAILED) {
    scanPending = false;
    return "{\"scanning\":false,\"list\":[]}";
  }
  scanPending = false;

  JSON_DOC(doc, 4096);
  doc["scanning"] = false;
  JsonArray arr = JSON_SUB_ARR(doc, "list");
  for (int i = 0; i < n && i < 30; i++) {
    JsonObject o = JSON_ADD_OBJ(arr);
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
    o["ch"]   = WiFi.channel(i);
    o["enc"]  = encStr(WiFi.encryptionType(i));
  }
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

#include "display_ui.h"
#include "app_config.h"
#include "net_wifi.h"
#include "mqtt_ctrl.h"
#include "io_ctrl.h"
#include "ST7789.h"
#include "bitmap.h"
#include <WiFi.h>

static ST7789 tft = ST7789();

// 版面座標 (240x240，字型 2 = 16px 高)
#define HDR_H      22
#define ROW_H      20
#define LBL_X       6
#define VAL_X      74
#define FONT        2

#define C_BG       TFT_BLACK
#define C_HDR      0x001F        // 深藍
#define C_LABEL    TFT_CYAN
#define C_VALUE    TFT_WHITE
#define C_OK       TFT_GREEN
#define C_WARN     TFT_YELLOW
#define C_ALARM    TFT_RED
#define C_IDLE     TFT_DARKGREY

// 上次顯示的內容，用來判斷是否需要重畫
struct Shadow {
  String  ssid, ip, rssi, mqttTxt, apTxt;
  bool    mqttOk   = false;
  bool    diAlarm[DI_COUNT];
  bool    doOn     = false;
  bool    valid    = false;
};
static Shadow sh;
static uint32_t lastUpdate = 0;
static uint32_t testUntil  = 0;    // 測試圖保留到這個時間點，期間不被狀態頁蓋掉

#define TEST_HOLD_MS 15000

// 顯示 task 與 web task (OTA 訊息) 都會畫面，用 mutex 保護 SPI
static SemaphoreHandle_t tftLock = nullptr;
static void tftTake() {
  if (!tftLock) tftLock = xSemaphoreCreateMutex();
  xSemaphoreTake(tftLock, portMAX_DELAY);
}
static void tftGive() { if (tftLock) xSemaphoreGive(tftLock); }

static void drawRow(int y, const char *label, const String &value, uint16_t color) {
  tft.setTextColor(C_LABEL, C_BG);
  tft.drawString(label, LBL_X, y, FONT);
  tft.fillRect(VAL_X, y, 240 - VAL_X, ROW_H - 2, C_BG);
  tft.setTextColor(color, C_BG);
  tft.drawString(value, VAL_X, y, FONT);
}

static void drawFrame() {
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, 240, HDR_H, C_HDR);
  tft.setTextColor(TFT_WHITE, C_HDR);
  tft.drawString("ESP32 DIDO", 6, 3, FONT);
  tft.drawFastHLine(0, HDR_H, 240, C_IDLE);
  tft.drawFastHLine(0, 160, 240, C_IDLE);
}

void displayBegin() {
  if (!tftLock) tftLock = xSemaphoreCreateMutex();

  pinMode(TFT_BL_PIN, OUTPUT);
  digitalWrite(TFT_BL_PIN, HIGH);          // 開背光

#if (TFT_RST >= 0)
  // 這塊面板需要確實的硬體重置才會起來。驅動內部已修正條件會再打一次，
  // 這裡依原始 sketch 驗證過的時序先做一次，確保冷開機穩定。
  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_RST, HIGH);
  delay(100);
  digitalWrite(TFT_RST, LOW);
  delay(100);
  digitalWrite(TFT_RST, HIGH);
  delay(100);
#endif

  tft.begin();
  tft.setSwapBytes(false);
  displayApplySettings();
  tft.fillScreen(C_BG);
  Serial.printf("[tft] ST7789 就緒 invert=%d rotation=%d %s",
                cfg.tftInvert ? 1 : 0, cfg.tftRotation, cfg.tftBgr ? "BGR" : "RGB");
  Serial.println();
}

// IPS 面板多半需要 INVON；色序與旋轉則依模組而異，一併做成可調
void displayApplySettings() {
  tftTake();
  tft.madColorOrder = cfg.tftBgr ? TFT_MAD_BGR : TFT_MAD_RGB;
  tft.setRotation(cfg.tftRotation & 3);      // 內部會重寫 MADCTL，色序同時生效
  tft.invertDisplay(cfg.tftInvert);
  tftGive();
  sh.valid = false;                          // 下一輪重畫整頁
}

// 校正用測試圖。四角標記可確認原點與可視範圍，色塊可確認 RGB/BGR 是否顛倒。
void displayTestPattern() {
  tftTake();
  tft.fillScreen(TFT_BLACK);

  // 邊框：若看不到完整矩形，代表旋轉或位移不對
  tft.drawRect(0, 0, 240, 240, TFT_WHITE);
  tft.drawRect(1, 1, 238, 238, TFT_WHITE);

  // 左上角實心白塊 = 原點 (0,0)
  tft.fillRect(2, 2, 30, 30, TFT_WHITE);

  // 色塊，由左至右：紅 綠 藍。順序顛倒代表要切換 BGR/RGB
  tft.fillRect(10,  50, 70, 40, TFT_RED);
  tft.fillRect(85,  50, 70, 40, TFT_GREEN);
  tft.fillRect(160, 50, 70, 40, TFT_BLUE);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("R", 40,  60, FONT);
  tft.drawString("G", 115, 60, FONT);
  tft.drawString("B", 190, 60, FONT);

  // 灰階：若黑白顛倒代表反相設定相反
  for (int i = 0; i < 8; i++) {
    uint8_t v = i * 36;
    tft.fillRect(10 + i * 27, 110, 27, 30, tft.color565(v, v, v));
  }
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("BLACK", 12, 145, FONT);
  tft.setTextDatum(0);
  tft.drawString("WHITE", 180, 145, FONT);

  tft.drawString("TEST PATTERN", 60, 180, FONT);
  tft.drawString("0,0 = top-left", 55, 205, FONT);

  // 右下角標記，確認右下邊界沒有被裁掉
  tft.fillRect(208, 208, 30, 30, TFT_YELLOW);
  tftGive();
  testUntil = millis() + TEST_HOLD_MS;
  sh.valid  = false;                 // 時間到之後重畫整頁
}

void displaySplash() {
  tftTake();
  tft.pushImage(0, 0, 240, 238, tecom1);
  tftGive();
}

void displayForceRedraw() { sh.valid = false; }

void displayMessage(const String &line1, const String &line2) {
  tftTake();
  tft.fillScreen(C_BG);
  tft.setTextColor(C_WARN, C_BG);
  tft.drawString(line1, 10, 95, FONT);
  tft.setTextColor(C_VALUE, C_BG);
  tft.drawString(line2, 10, 120, FONT);
  tftGive();
  sh.valid = false;
}

void displayLoop() {
  if (testUntil) {                             // 測試圖顯示中，暫停狀態更新
    if ((int32_t)(millis() - testUntil) < 0) return;
    testUntil = 0;
  }
  if (millis() - lastUpdate < 500) return;
  lastUpdate = millis();

  // --- 取得目前狀態 ---
  // AP 常開，上半部顯示 STA (DHCP) 資訊，另闢一行顯示 AP
  bool   conn = WiFi.status() == WL_CONNECTED;
  String ssid = conn ? WiFi.SSID() : (cfg.wifiSsid.length() ? cfg.wifiSsid : String("---"));
  String ip   = conn ? WiFi.localIP().toString() : String("NO DHCP");
  String rssi = conn ? String(WiFi.RSSI()) + " dBm" : String("DISCONNECTED");
  String apTxt = wifiApIp() + " (" + String(WiFi.softAPgetStationNum()) + ")";
  bool   mqOk = mqttConnected();
  String mqTxt = cfg.mqttEnabled ? (mqOk ? String("CONNECTED") : String("OFFLINE"))
                                 : String("DISABLED");
  bool   dOn  = doState();

  bool changed = !sh.valid || sh.ssid != ssid || sh.ip != ip || sh.rssi != rssi ||
                 sh.mqttTxt != mqTxt || sh.mqttOk != mqOk || sh.doOn != dOn ||
                 sh.apTxt != apTxt;
  for (int i = 0; i < DI_COUNT && !changed; i++)
    if (sh.diAlarm[i] != diAlarm(i)) changed = true;

  // 底部時間每秒刷新，與其他欄位分開處理
  static String lastTimeStr;
  String tstr = isoTime(time(nullptr));
  if (!changed) {
    if (tstr == lastTimeStr) return;
    tftTake();
    tft.fillRect(0, 212, 240, 24, C_BG);
    tft.setTextColor(C_IDLE, C_BG);
    tft.drawString(tstr, 6, 214, FONT);
    tftGive();
    lastTimeStr = tstr;
    return;
  }

  tftTake();

  bool full = !sh.valid;
  if (full) drawFrame();

  int y = HDR_H + 4;
  if (full || sh.ssid != ssid) drawRow(y, "SSID", ssid.length() ? ssid : String("---"), C_VALUE);
  y += ROW_H;
  if (full || sh.ip != ip)     drawRow(y, "IP",   ip, conn ? C_OK : C_WARN);
  y += ROW_H;
  if (full || sh.rssi != rssi) drawRow(y, "RSSI", rssi, conn ? C_OK : C_WARN);
  y += ROW_H;
  if (full || sh.apTxt != apTxt) drawRow(y, "AP", apTxt, C_LABEL);
  y += ROW_H;
  if (full || sh.mqttTxt != mqTxt)
    drawRow(y, "MQTT", mqTxt, mqOk ? C_OK : (cfg.mqttEnabled ? C_WARN : C_IDLE));
  y += ROW_H;
  if (full) drawRow(y, "ID", mqttClientId(), C_VALUE);

  // --- DI / DO 區塊 ---
  int by = 168;
  for (int i = 0; i < DI_COUNT; i++) {
    if (!full && sh.diAlarm[i] == diAlarm(i)) continue;
    bool a = diAlarm(i);
    int bx = 6 + i * 78;
    tft.fillRoundRect(bx, by, 72, 30, 4, a ? C_ALARM : C_IDLE);
    tft.setTextColor(TFT_WHITE, a ? C_ALARM : C_IDLE);
    char buf[16];
    snprintf(buf, sizeof(buf), "DI%d %s", i + 1, a ? "ALM" : "OK");
    tft.drawString(buf, bx + 6, by + 7, FONT);
  }
  if (full || sh.doOn != dOn) {
    int bx = 6 + DI_COUNT * 78;
    tft.fillRoundRect(bx, by, 72, 30, 4, dOn ? C_OK : C_IDLE);
    tft.setTextColor(TFT_BLACK, dOn ? C_OK : C_IDLE);
    tft.drawString(dOn ? "DO ON" : "DO OFF", bx + 6, by + 7, FONT);
  }

  // --- 底部時間 ---
  tft.fillRect(0, 212, 240, 24, C_BG);
  tft.setTextColor(C_IDLE, C_BG);
  tft.drawString(tstr, 6, 214, FONT);
  tftGive();
  lastTimeStr = tstr;

  // --- 更新 shadow ---
  sh.ssid = ssid; sh.ip = ip; sh.rssi = rssi;
  sh.mqttTxt = mqTxt; sh.mqttOk = mqOk; sh.apTxt = apTxt;
  sh.doOn = dOn;
  for (int i = 0; i < DI_COUNT; i++) sh.diAlarm[i] = diAlarm(i);
  sh.valid = true;
}

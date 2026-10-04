#include "display_ui.h"
#include "app_config.h"
#include "net_wifi.h"
#include "mqtt_ctrl.h"
#include "io_ctrl.h"
#include "ST7789.h"
#include "bitmap.h"
#include "qrcode.h"
#include "modbus_rtu.h"
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
  String  ioTxt;                   // DI/DO 狀態的字串快照
  bool    valid    = false;
};
static Shadow sh;
static uint32_t lastUpdate = 0;
static uint32_t testUntil  = 0;    // 測試圖保留到這個時間點，期間不被狀態頁蓋掉
static uint32_t qrUntil    = 0;    // 開機 QR 畫面保留到這個時間點
static uint8_t  activePage = 0;    // 目前顯示中的頁（自動輪替時會自己切換）
static uint32_t pageSwitchAt = 0;
static String   mbShadow;          // Modbus 頁的內容快照，變了才重畫

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

  applyBacklight();

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
  Serial.printf("[tft] ST7789 就緒 invert=%d rotation=%d %s %uMHz",
                cfg.tftInvert ? 1 : 0, cfg.tftRotation,
                cfg.tftBgr ? "BGR" : "RGB", (unsigned)cfg.tftSpiMhz);
  Serial.println();
}

// 背光：預設不驅動，讓腳位維持高阻抗（原始 sketch 的行為，模組自己讓背光恆亮）。
// 若由 GPIO 直推 LED，大電流造成的地彈會干擾同一排針的 SPI 訊號。
void applyBacklight() {
  if (cfg.tftBacklight == 0) {
    pinMode(TFT_BL_PIN, INPUT);            // 高阻抗，不供電也不下拉
  } else {
    pinMode(TFT_BL_PIN, OUTPUT);
    digitalWrite(TFT_BL_PIN, cfg.tftBacklight == 1 ? HIGH : LOW);
  }
}

// IPS 面板多半需要 INVON；色序與旋轉則依模組而異，一併做成可調
void displayApplySettings() {
  tftTake();
  applyBacklight();
  tft.spiFrequency  = (uint32_t)constrain((int)cfg.tftSpiMhz, 4, 80) * 1000000UL;
  tft.madColorOrder = cfg.tftBgr ? TFT_MAD_BGR : TFT_MAD_RGB;
  tft.setRotation(cfg.tftRotation & 3);      // 內部會重寫 MADCTL，色序同時生效
  tft.invertDisplay(cfg.tftInvert);
  tftGive();
  sh.valid = false;                          // 下一輪重畫整頁
}

bool displayQrActive() { return qrUntil != 0; }

void displayQrDismiss() {
  if (!qrUntil) return;
  qrUntil  = 0;
  sh.valid = false;
}

// 開機 QR 畫面：掃描後直接開啟裝置網頁。
// 版本 3 (29x29 模組) 足以容納 "http://192.168.100.100/" 這類長度的網址。
void displayQrScreen(uint32_t holdSec) {
  if (holdSec == 0) holdSec = cfg.qrBootSec;
  if (holdSec == 0) return;                    // 設為 0 代表不顯示

  bool   conn = WiFi.status() == WL_CONNECTED;
  String host = conn ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  String url  = "http://" + host + "/";

  QRCode qr;
  const uint8_t version = 3;                   // 29x29
  uint8_t *buf = (uint8_t *)malloc(qrcode_getBufferSize(version));
  if (!buf) {
    Serial.println(F("[tft] QR 緩衝區配置失敗"));
    return;
  }
  if (qrcode_initText(&qr, buf, version, ECC_MEDIUM, url.c_str()) < 0) {
    Serial.println(F("[tft] QR 編碼失敗"));
    free(buf);
    return;
  }

  // 置中並留 4 模組的靜區（quiet zone），否則部分手機掃不到
  const int quiet = 4;
  const int total = qr.size + quiet * 2;
  const int scale = 160 / total;               // 盡量放大但不超過 160px
  const int side  = total * scale;
  const int ox    = (240 - side) / 2;
  const int oy    = 34;

  tftTake();
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, 240, HDR_H, C_HDR);
  tft.setTextColor(TFT_WHITE, C_HDR);
  tft.drawString("SCAN TO OPEN", 6, 3, FONT);

  // QR 必須畫在白底上才掃得到
  tft.fillRect(ox, oy, side, side, TFT_WHITE);
  for (uint8_t y = 0; y < qr.size; y++) {
    for (uint8_t x = 0; x < qr.size; x++) {
      if (!qrcode_getModule(&qr, x, y)) continue;
      tft.fillRect(ox + (x + quiet) * scale, oy + (y + quiet) * scale,
                   scale, scale, TFT_BLACK);
    }
  }
  free(buf);

  tft.setTextColor(TFT_CYAN, C_BG);
  tft.drawString(url, 6, oy + side + 8, FONT);
  tft.setTextColor(C_IDLE, C_BG);
  tft.drawString(conn ? "STA" : "AP MODE", 6, oy + side + 28, FONT);
  tftGive();

  qrUntil   = millis() + holdSec * 1000UL;
  testUntil = 0;
  sh.valid  = false;
  Serial.printf("[tft] QR 畫面 %s 停留 %u 秒", url.c_str(), (unsigned)holdSec);
  Serial.println();
}

// 忘記密碼的復原碼。刻意用最大字體，隔一段距離也看得清楚。
void displayRecoveryCode(const String &code, uint32_t holdSec) {
  tftTake();
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, 240, HDR_H, C_ALARM);
  tft.setTextColor(TFT_WHITE, C_ALARM);
  tft.drawString("PASSWORD RECOVERY", 6, 3, FONT);

  tft.setTextColor(C_LABEL, C_BG);
  tft.drawString("Recovery code:", 6, 50, FONT);

  // Font2 放大 3 倍，240px 寬剛好容得下 6 位數
  tft.setTextSize(3);
  tft.setTextColor(TFT_WHITE, C_BG);
  tft.drawString(code, 18, 86, FONT);
  tft.setTextSize(1);

  tft.setTextColor(C_WARN, C_BG);
  tft.drawString("Valid for " + String(holdSec) + " seconds", 6, 150, FONT);
  tft.setTextColor(C_IDLE, C_BG);
  tft.drawString("Enter it at /recover.html", 6, 176, FONT);
  tft.drawString("to set a new password", 6, 196, FONT);
  tftGive();

  qrUntil   = 0;
  testUntil = millis() + holdSec * 1000UL;   // 借用保留機制，時間到自動回狀態頁
  sh.valid  = false;
  mbShadow  = "";
}

// 校正用測試圖。四角標記可確認原點與可視範圍，色塊可確認 RGB/BGR 是否顛倒。
void displayTestPattern() {
  qrUntil = 0;                       // 手動操作優先於開機 QR 畫面
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

// 與原始 sketch 完全相同的繪圖路徑（單一 pushImage）。
// 用來區分雜訊來自 SPI 訊號本身，還是狀態頁的大量小筆繪圖。
void displaySplash() {
  tftTake();
  tft.pushImage(0, 0, 240, 238, tecom1);
  tftGive();
}

void displaySplashHold() {
  qrUntil = 0;                       // 手動操作優先於開機 QR 畫面
  displaySplash();
  testUntil = millis() + TEST_HOLD_MS;
  sh.valid  = false;
}

void displayForceRedraw() { sh.valid = false; mbShadow = ""; }

void displaySetPage(uint8_t page) {
  cfg.tftPage  = page > 2 ? 0 : page;
  activePage   = cfg.tftPage == 2 ? activePage : cfg.tftPage;
  pageSwitchAt = millis();
  sh.valid     = false;
  mbShadow     = "";
  qrUntil      = 0;                            // 手動切頁優先於開機 QR
}

void displayMessage(const String &line1, const String &line2) {
  qrUntil   = 0;
  testUntil = 0;
  tftTake();
  tft.fillScreen(C_BG);
  tft.setTextColor(C_WARN, C_BG);
  tft.drawString(line1, 10, 95, FONT);
  tft.setTextColor(C_VALUE, C_BG);
  tft.drawString(line2, 10, 120, FONT);
  tftGive();
  sh.valid = false;
}

// 面板字型 Font16 只有 ASCII 32~127，中文會整段畫不出來，
// 因此錯誤狀態在面板上一律轉成 ASCII 代碼。
static const char *mbErrAscii(const String &e) {
  if (e.length() == 0)          return "---";
  if (e.indexOf("逾時") >= 0)   return "TIMEOUT";
  if (e.indexOf("例外") >= 0)   return "EXCEPTION";
  if (e.indexOf("站號") >= 0)   return "BAD ID";
  if (e.indexOf("長度") >= 0)   return "BAD LEN";
  return "ERROR";
}

// Modbus Master 輪詢數值頁
static void drawModbusPage(bool full) {
  // 先把要顯示的內容組成字串，和上次比對，沒變就不重畫
  String snap;
  char line[7][40];
  int  rows = 0;

  bool master = mbIsMaster();
  if (!mbIsEnabled()) {
    snprintf(line[rows++], 40, "RS485 DISABLED");
  } else if (!master) {
    snprintf(line[rows++], 40, "SLAVE MODE");
    snprintf(line[rows++], 40, "ID %d", cfg.mbSlaveId);
  } else {
    for (uint8_t i = 0; i < MB_POLL_MAX && rows < 6; i++) {
      if (!mbPollEnabled(i)) continue;
      uint16_t v[4];
      uint8_t  n = mbPollValues(i, v, 4);
      String   nm = mbPollName(i);
      if (nm.length() > 7) nm = nm.substring(0, 7);

      if (n == 0) {
        snprintf(line[rows++], 40, "%-7s %s", nm.c_str(), mbErrAscii(mbPollError(i)));
      } else if (n == 1) {
        snprintf(line[rows++], 40, "%-7s %u", nm.c_str(), v[0]);
      } else if (n == 2) {
        snprintf(line[rows++], 40, "%-7s %u %u", nm.c_str(), v[0], v[1]);
      } else {
        snprintf(line[rows++], 40, "%-7s %u %u %u", nm.c_str(), v[0], v[1], v[2]);
      }
    }
    if (rows == 0) snprintf(line[rows++], 40, "NO POLL ENABLED");
  }

  uint32_t rx, tx, err;
  mbGetStats(rx, tx, err);
  snprintf(line[rows], 40, "RX%lu TX%lu ERR%lu",
           (unsigned long)rx, (unsigned long)tx, (unsigned long)err);
  int statRow = rows;

  for (int i = 0; i <= statRow; i++) { snap += line[i]; snap += '|'; }
  if (!full && snap == mbShadow) return;
  mbShadow = snap;

  tftTake();
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, 240, HDR_H, C_HDR);
  tft.setTextColor(TFT_WHITE, C_HDR);
  tft.drawString(mbIsEnabled() ? (master ? "MODBUS MASTER" : "MODBUS SLAVE")
                               : "MODBUS", 6, 3, FONT);

  int y = HDR_H + 6;
  for (int i = 0; i < statRow; i++) {
    // 讀不到值的那幾行用黃色標出來
    bool bad = strstr(line[i], "---") || strstr(line[i], "TIMEOUT") ||
               strstr(line[i], "EXCEPTION") || strstr(line[i], "ERROR") ||
               strstr(line[i], "BAD ");
    tft.setTextColor(bad ? C_WARN : C_VALUE, C_BG);
    tft.drawString(line[i], LBL_X, y, FONT);
    y += ROW_H;
  }

  tft.drawFastHLine(0, 196, 240, C_IDLE);
  tft.setTextColor(C_LABEL, C_BG);
  tft.drawString(line[statRow], LBL_X, 202, FONT);

  tft.setTextColor(C_IDLE, C_BG);
  tft.drawString(isoTime(time(nullptr)), LBL_X, 222, FONT);
  tftGive();
}

void displayLoop() {
  if (qrUntil) {                               // QR 畫面顯示中
    if ((int32_t)(millis() - qrUntil) < 0) return;
    qrUntil  = 0;
    sh.valid = false;                          // 時間到，重畫狀態頁
    Serial.println(F("[tft] QR 畫面結束，切換至狀態畫面"));
  }
  if (testUntil) {                             // 測試圖顯示中，暫停狀態更新
    if ((int32_t)(millis() - testUntil) < 0) return;
    testUntil = 0;
  }
  if (millis() - lastUpdate < 500) return;
  lastUpdate = millis();

  // --- 決定目前該顯示哪一頁 ---
  if (cfg.tftPage == 2) {                      // 自動輪替
    uint32_t iv = (cfg.tftPageSec ? cfg.tftPageSec : 10) * 1000UL;
    if (millis() - pageSwitchAt >= iv) {
      pageSwitchAt = millis();
      activePage   = activePage ? 0 : 1;
      sh.valid     = false;
      mbShadow     = "";
    }
  } else if (cfg.tftPage != activePage) {
    activePage = cfg.tftPage;
    sh.valid   = false;
    mbShadow   = "";
  }

  if (activePage == 1) {
    drawModbusPage(!sh.valid);
    sh.valid = true;                           // 借用同一個旗標表示「已畫過」
    return;
  }

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
  // DI / DO 狀態合併成一個字串，變動時才重畫下半部
  String ioTxt;
  for (int i = 0; i < DI_COUNT; i++) ioTxt += diAlarm(i) ? '1' : '0';
  for (int c = 0; c < DO_COUNT; c++) ioTxt += doState(c) ? '1' : '0';

  bool changed = !sh.valid || sh.ssid != ssid || sh.ip != ip || sh.rssi != rssi ||
                 sh.mqttTxt != mqTxt || sh.mqttOk != mqOk ||
                 sh.apTxt != apTxt || sh.ioTxt != ioTxt;

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

  // --- DI / DO 區塊：4 格並排 (DI1 DI2 DO1 DO2) ---
  const int by = 168, bw = 52, bh = 30, step = 58;
  for (int i = 0; i < DI_COUNT; i++) {
    bool a = diAlarm(i);
    int bx = 6 + i * step;
    tft.fillRoundRect(bx, by, bw, bh, 4, a ? C_ALARM : C_IDLE);
    tft.setTextColor(TFT_WHITE, a ? C_ALARM : C_IDLE);
    char buf[12];
    snprintf(buf, sizeof(buf), "I%d:%s", i + 1, a ? "AL" : "OK");
    tft.drawString(buf, bx + 5, by + 7, FONT);
  }
  for (int c = 0; c < DO_COUNT; c++) {
    bool on = doState(c);
    int bx = 6 + (DI_COUNT + c) * step;
    tft.fillRoundRect(bx, by, bw, bh, 4, on ? C_OK : C_IDLE);
    tft.setTextColor(on ? TFT_BLACK : TFT_WHITE, on ? C_OK : C_IDLE);
    char buf[12];
    snprintf(buf, sizeof(buf), "O%d:%s", c + 1, on ? "ON" : "--");
    tft.drawString(buf, bx + 5, by + 7, FONT);
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
  sh.ioTxt = ioTxt;
  sh.valid = true;
}

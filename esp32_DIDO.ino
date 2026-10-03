/*
 * ESP32 DIDO 模組        韌體版本 1.8.1
 * ==================================================================
 *  前端：SPIFFS 內的 data/index.html + css/js，側邊欄式設定介面
 *        系統狀態 / WiFi 設定 / DI 設定 / DO 設定 / MQTT 設定 / OTA / 使用者
 *  後端：ST7789 顯示 WiFi、MQTT、DI 告警與 DO 狀態
 *
 *  版本定義於 app_config.h 的 FW_VERSION，開機 Serial 與網頁「系統狀態」
 *  都會顯示版本與編譯時間，用來確認韌體與 SPIFFS 內的網頁是否同一次更新。
 *
 *  ==ESP32 D1 mini 接線==
 *    DI1  pin32     DI2  pin33      (INPUT_PULLUP，短接 GND 觸發)
 *    DO1  pin4      DO2  pin2       (DO2 原為 LED_BUILTIN，已改作輸出)
 *    RS-485: DI(TX) 17 / RO(RX) 16 / DE、RE 短路接 14
 *    ST7789: CS 5 / DC 19 / MOSI 23 / SCLK 18 / RST 0 / BLK 15
 *
 *  ==上傳步驟==
 *    1. Partition Scheme 選 "Minimal SPIFFS (Large APPS with OTA)"
 *       預設的 Default 只給 APP 1.2MB，放不下本韌體
 *    2. Upload Speed 設 115200（本板 921600 會失敗）
 *    3. 先上傳 data/ 到 SPIFFS（ESP32 Sketch Data Upload，
 *       或執行 tools\make-spiffs.ps1 產生 spiffs.bin 由網頁 OTA 上傳）
 *    4. 燒錄韌體
 *    5. 裝置一律開啟 AP：ESP32-DIDO-xxxx (192.168.4.1)，連上會自動跳出設定頁
 *    6. 無預設帳密，首次開啟免登入，請立即到「使用者設定」建立帳號密碼
 *
 *  ==相依函式庫==
 *    ESP32Async/ESPAsyncWebServer 3.2.0 以上  ← 舊 fork 無法在 core 3.x 編譯
 *    ESP32Async/AsyncTCP、PubSubClient、ArduinoJson (6 或 7)
 *    QR 編碼器 qrcode.c/h 已隨專案附帶（ricmoo/QRCode，MIT），不需另外安裝
 *    開發環境：Arduino IDE 1.8.19 + ESP32 core 3.3.10
 *
 *  ==版本沿革==
 *    1.8.1  新增忘記密碼復原（面板復原碼 + 序列埠指令）
 *    1.8.0  Modbus Master 輪詢數值可顯示於 ST7789，網頁切換畫面
 *    1.7.0  新增 RS-485 Modbus RTU 設定（Slave / Master 可切換）
 *    1.6.0  開機顯示 QR 畫面，掃描即可開啟裝置網頁（預設停留 2 分鐘）
 *    1.5.0  DI / DO 狀態納入 MQTT 推播（di/<n>、status 快照、retained）
 *    1.4.1  DI 診斷：腳位、即時電位、IO 任務心跳
 *    1.4.0  DO 改為雙通道，新增 GPIO2 控制
 *    1.3.3  繼電器自我測試與腳位準位回讀
 *    1.3.2  背光改回不驅動，新增開機圖按鈕用於判斷雜訊來源
 *    1.3.1  修正 TFT_MISO 與 TFT_DC 撞腳；SPI 時脈可於網頁調整
 *    1.3.0  ST7789 反相 / 色序 / 旋轉改為網頁可調；MQTT 預設改 MQTTGO.io
 *    1.2.1  修正 ST7789 無畫面（硬體重置被 TFT_RST > 0 條件跳過）
 *    1.2.0  AP 常開 + captive portal 自動跳轉，連線後顯示 DHCP IP
 *    1.1.1  修正路由註冊順序導致 9 支子路由被通用路由攔截
 *    1.1.0  移除預設帳密；修正 WiFi 掃描誤判為無結果
 *    1.0.0  改用 SPIFFS 網頁前端，後端拆分模組
 */

#include <Arduino.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <time.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#include "app_config.h"
#include "net_wifi.h"
#include "io_ctrl.h"
#include "mqtt_ctrl.h"
#include "notify.h"
#include "display_ui.h"
#include "web_portal.h"
#include "modbus_rtu.h"

TaskHandle_t hTaskIo;
TaskHandle_t hTaskNet;
TaskHandle_t hTaskNotify;
TaskHandle_t hTaskDisplay;
TaskHandle_t hTaskRs485;

// core 0：DI 取樣 / DO 點動與排程 (時間敏感，不被網路阻塞)
static void taskIo(void *) {
  for (;;) {
    ioLoop();
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// core 0：Modbus RTU。訊框邊界靠靜默時間判斷，週期要短才不會漏接。
static void taskRs485(void *) {
  for (;;) {
    modbusLoop();
    vTaskDelay(2 / portTICK_PERIOD_MS);
  }
}

// core 1：WiFi 重連 + MQTT
static void taskNet(void *) {
  for (;;) {
    wifiLoop();
    mqttLoop();
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// core 1：推播 (HTTPS 會阻塞，獨立 task)
static void taskNotify(void *) {
  for (;;) {
    notifyLoop();
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

// core 1：ST7789 畫面更新
static void taskDisplay(void *) {
  for (;;) {
    displayLoop();
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

static void setupTime() {
  configTzTime(cfg.tz.c_str(), cfg.ntp.c_str(), "time.google.com", "time.windows.com");
  Serial.printf("[time] NTP=%s TZ=%s\n", cfg.ntp.c_str(), cfg.tz.c_str());
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);     // 關閉 brownout 偵測
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.printf("=== ESP32 DIDO %s (build %s) ===", FW_VERSION, FW_BUILD);
  Serial.println();

  // GPIO2 (LED_BUILTIN) 已改作 DO2 輸出，由 ioBegin() 接管，此處不再動它

  // --- SPIFFS ---
  if (!SPIFFS.begin(true)) {                     // true = 掛載失敗時自動格式化
    Serial.println(F("[fs] SPIFFS 掛載失敗"));
  } else {
    Serial.printf("[fs] SPIFFS %u / %u bytes\n",
                  (unsigned)SPIFFS.usedBytes(), (unsigned)SPIFFS.totalBytes());
  }

  configLoad();
  alarmLoad();

  // --- 顯示器 ---
  displayBegin();
  displaySplash();

  // --- IO ---
  ioBegin();
  modbusBegin();
  notifyBegin();

  // --- 網路 ---
  wifiBegin();
  setupTime();
  mqttBegin();
  webBegin();

  // 等網路就緒、拿到 IP 之後才畫 QR，掃到的網址才是有效的
  displayQrScreen();
  displayForceRedraw();

  //                副程式,      任務名稱,  堆疊,  參數, 優先序, handle,      核心
  BaseType_t rc = xTaskCreatePinnedToCore(taskIo, "io", 8192, NULL, 2, &hTaskIo, 0);
  Serial.printf("[sys] io task 建立 %s", rc == pdPASS ? "成功" : "失敗");
  Serial.println();
  xTaskCreatePinnedToCore(taskNet,     "net",     8192,  NULL, 1, &hTaskNet,     1);
  xTaskCreatePinnedToCore(taskNotify,  "notify",  16384, NULL, 1, &hTaskNotify,  1);
  xTaskCreatePinnedToCore(taskDisplay, "display", 4096,  NULL, 1, &hTaskDisplay, 1);
  xTaskCreatePinnedToCore(taskRs485,   "rs485",   4096,  NULL, 2, &hTaskRs485,   0);

  Serial.printf("[sys] 就緒，網頁 http://%s/\n",
                wifiIsAp() ? WiFi.softAPIP().toString().c_str()
                           : WiFi.localIP().toString().c_str());
}

// 序列埠指令。面板看不到或網頁進不去時的救援管道，需實體接上 USB。
static void handleSerialCommand(const String &cmd) {
  if (cmd == "help") {
    Serial.println(F("可用指令："));
    Serial.println(F("  help         顯示本說明"));
    Serial.println(F("  info         顯示版本與連線資訊"));
    Serial.println(F("  reset-auth   清除登入帳號密碼（忘記密碼時使用）"));
    Serial.println(F("  reset-wifi   清除 WiFi 連線設定"));
    Serial.println(F("  reboot       重新啟動"));
  } else if (cmd == "info") {
    Serial.printf("版本 %s (build %s)", FW_VERSION, FW_BUILD);  Serial.println();
    Serial.printf("WiFi  %s  IP %s", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());           Serial.println();
    Serial.printf("AP    %s", WiFi.softAPIP().toString().c_str()); Serial.println();
    Serial.printf("帳號  %s", cfg.authUser.length() ? cfg.authUser.c_str() : "(未設定)");
    Serial.println();
    Serial.printf("可用記憶體 %u bytes", (unsigned)ESP.getFreeHeap()); Serial.println();
  } else if (cmd == "reset-auth") {
    authClearCredentials();
  } else if (cmd == "reset-wifi") {
    wifiClearConfig();
  } else if (cmd == "reboot") {
    Serial.println(F("重新啟動中..."));
    delay(200);
    ESP.restart();
  } else {
    Serial.printf("未知指令：%s（輸入 help 查看可用指令）", cmd.c_str());
    Serial.println();
  }
}

static void pollSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      line.trim();
      if (line.length()) handleSerialCommand(line);
      line = "";
    } else if (line.length() < 64) {
      line += c;
    }
  }
}

void loop() {
  pollSerial();

  if (webRebootPending()) {
    Serial.println(F("[sys] 重新啟動"));
    displayMessage("REBOOTING...", "");
    delay(300);
    ESP.restart();
  }
  delay(100);
}

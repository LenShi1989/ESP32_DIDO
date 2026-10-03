/*
 * ESP32 DIDO 模組
 * ------------------------------------------------------------------
 *  前端：SPIFFS 內的 data/index.html + css/js，側邊欄式設定介面
 *        系統狀態 / WiFi 設定 / DI 設定 / DO 設定 / MQTT 設定 / OTA / 使用者
 *  後端：ST7789 顯示 WiFi、MQTT、DI 告警與 DO 繼電器狀態
 *
 *  ==ESP32 D1 mini 接線==
 *    DI1   pin32      DI2   pin33      Relay pin4
 *    ST7789: CS 5 / DC 19 / MOSI 23 / SCLK 18 / RST 0 / BLK 15
 *
 *  ==上傳步驟==
 *    1. Arduino IDE 安裝 "ESP32 Sketch Data Upload" (或 PlatformIO uploadfs)
 *    2. 先上傳 data/ 資料夾到 SPIFFS，再燒錄韌體
 *    3. 首次開機若無 WiFi 設定會開 AP：ESP32-DIDO-xxxx (192.168.4.1)
 *    4. 預設網頁帳密 admin / admin
 *
 *  ==相依函式庫==
 *    ESPAsyncWebServer + AsyncTCP、PubSubClient、ArduinoJson (6 或 7)
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

TaskHandle_t hTaskIo;
TaskHandle_t hTaskNet;
TaskHandle_t hTaskNotify;
TaskHandle_t hTaskDisplay;

// core 0：DI 取樣 / DO 點動與排程 (時間敏感，不被網路阻塞)
static void taskIo(void *) {
  for (;;) {
    ioLoop();
    vTaskDelay(10 / portTICK_PERIOD_MS);
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

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

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
  notifyBegin();

  // --- 網路 ---
  wifiBegin();
  setupTime();
  mqttBegin();
  webBegin();

  displayForceRedraw();

  //                副程式,      任務名稱,  堆疊,  參數, 優先序, handle,      核心
  xTaskCreatePinnedToCore(taskIo,      "io",      8192,  NULL, 2, &hTaskIo,      0);
  xTaskCreatePinnedToCore(taskNet,     "net",     8192,  NULL, 1, &hTaskNet,     1);
  xTaskCreatePinnedToCore(taskNotify,  "notify",  16384, NULL, 1, &hTaskNotify,  1);
  xTaskCreatePinnedToCore(taskDisplay, "display", 4096,  NULL, 1, &hTaskDisplay, 1);

  Serial.printf("[sys] 就緒，網頁 http://%s/\n",
                wifiIsAp() ? WiFi.softAPIP().toString().c_str()
                           : WiFi.localIP().toString().c_str());
}

void loop() {
  if (webRebootPending()) {
    Serial.println(F("[sys] 重新啟動"));
    displayMessage("REBOOTING...", "");
    delay(300);
    ESP.restart();
  }
  delay(100);
}

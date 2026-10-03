/*
 * app_config.h - 設定資料結構與 SPIFFS 持久化
 *
 * 所有設定以 JSON 形式存放於 SPIFFS /config.json
 * 告警紀錄存放於 SPIFFS /alarms.json (最新 10 筆循環)
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <time.h>

// ---- ArduinoJson 6 / 7 相容巨集 ----
#if ARDUINOJSON_VERSION_MAJOR >= 7
#define JSON_DOC(name, cap)        JsonDocument name
#define JSON_ADD_OBJ(arr)          (arr).add<JsonObject>()
#define JSON_SUB_OBJ(parent, key)  (parent)[key].to<JsonObject>()
#define JSON_SUB_ARR(parent, key)  (parent)[key].to<JsonArray>()
#else
#define JSON_DOC(name, cap)        DynamicJsonDocument name(cap)
#define JSON_ADD_OBJ(arr)          (arr).createNestedObject()
#define JSON_SUB_OBJ(parent, key)  (parent).createNestedObject(key)
#define JSON_SUB_ARR(parent, key)  (parent).createNestedArray(key)
#endif

// 韌體版本。網頁「系統狀態」會一併顯示編譯時間，
// 可用來確認韌體與 SPIFFS 內的網頁是否為同一次更新。
#define FW_VERSION   "1.5.0"
#define FW_BUILD     __DATE__ " " __TIME__

// ---- 硬體腳位 ----
#define DI1_PIN      32          // DI 1
#define DI2_PIN      33          // DI 2
#define DO1_PIN       4          // DO 1 繼電器
#define DO2_PIN       2          // DO 2（GPIO2，本板的 LED_BUILTIN，已改作 DO 用途）
#define TFT_BL_PIN   15          // ST7789 背光

#define DI_COUNT      2
#define DO_COUNT      2
#define SCHED_COUNT   4          // 定時排程筆數
#define ALARM_MAX    10          // 告警紀錄保留筆數
#define MQTT_MSG_MAX 10          // 訂閱訊息保留筆數

#define CONFIG_FILE  "/config.json"
#define ALARM_FILE   "/alarms.json"

// DO 工作模式
enum DoMode : uint8_t {
  DO_MODE_MANUAL = 0,            // 手動 switch
  DO_MODE_SCHEDULE = 1,          // 定時
  DO_MODE_PULSE = 2              // 點動
};

struct DiConfig {
  String  name;                  // 顯示名稱
  bool    enabled;               // 是否啟用監控
  bool    activeLow;             // true = 接點短路(LOW)視為告警
  String  alarmText;             // 自定義觸發告警文字
  String  normalText;            // 自定義解除告警文字
};

struct DoConfig {
  String  name;                  // 顯示名稱
  bool    activeLow;             // true = 輸出 LOW 導通
  uint8_t mode;                  // DoMode
  uint32_t pulseMs;              // 點動保持時間 (ms)
};

struct ScheduleItem {
  bool    enabled;
  uint8_t days;                  // bit0=日 bit1=一 ... bit6=六
  uint8_t onH, onM;              // 開啟時間
  uint8_t offH, offM;            // 關閉時間
};

struct AlarmRecord {
  time_t  ts;                    // epoch 秒 (0 = 尚未校時)
  uint8_t ch;                    // DI 通道 1..DI_COUNT
  bool    alarm;                 // true=觸發 false=解除
  String  text;                  // 當下的告警/解除文字
};

struct MqttMessage {
  time_t  ts;
  String  topic;
  String  payload;
};

struct Config {
  // --- WiFi ---
  String   wifiSsid;
  String   wifiPass;
  String   hostname;

  // --- 使用者 ---
  String   authUser;
  String   authPass;

  // --- DI ---
  DiConfig di[DI_COUNT];

  // --- 推播 ---
  bool     discordEnabled;
  String   discordWebhook;
  bool     telegramEnabled;
  String   telegramToken;
  String   telegramChatId;

  // --- DO ---
  DoConfig doCh[DO_COUNT];
  ScheduleItem sched[DO_COUNT][SCHED_COUNT];

  // --- MQTT ---
  bool     mqttEnabled;
  String   mqttHost;
  uint16_t mqttPort;
  bool     clientIdAuto;
  String   clientId;
  String   mqttUser;
  String   mqttPass;
  String   pubTopic;
  uint8_t  pubQos;               // 預設 0
  String   subTopic;
  uint8_t  subQos;               // 預設 2 (PubSubClient 實際支援 0/1)
  bool     mqttRetain;           // 狀態主題是否保留 (retained)
  uint16_t mqttStatusSec;        // 定期推播整體狀態的秒數，0 = 關閉

  // --- 顯示器 ---
  // 面板差異很大，做成可在網頁即時調整並存檔，不必為了試參數反覆重燒
  bool     tftInvert;            // IPS 面板多半需要反相 (INVON)
  uint8_t  tftRotation;          // 0~3
  bool     tftBgr;               // true=BGR，false=RGB (紅藍顛倒時切換)
  uint8_t  tftSpiMhz;            // SPI 時脈 (MHz)，雜訊多時調低
  // 背光腳位驅動方式。0 = 不驅動 (保持高阻抗，與原始 sketch 相同)。
  // 模組多半自帶上拉讓背光恆亮；若由 GPIO 直推 LED，大電流會造成地彈干擾 SPI。
  uint8_t  tftBacklight;         // 0=不驅動 1=輸出HIGH 2=輸出LOW

  // --- 其他 ---
  String   tz;                   // POSIX TZ 字串
  String   ntp;                  // NTP 伺服器
};

extern Config cfg;

void   configSetDefaults();
bool   configLoad();
bool   configSave();
String configToJson(bool includeSecrets);

// 告警紀錄
extern AlarmRecord alarmLog[ALARM_MAX];
extern uint8_t     alarmCount;

void   alarmLoad();
void   alarmSave();
void   alarmAdd(uint8_t ch, bool isAlarm, const String &text);
void   alarmClear();
String alarmToJson();

String isoTime(time_t t);

#endif

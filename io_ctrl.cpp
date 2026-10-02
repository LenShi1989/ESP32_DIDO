#include "io_ctrl.h"
#include "app_config.h"
#include "notify.h"
#include "mqtt_ctrl.h"
#include <time.h>

static const uint8_t DI_PINS[DI_COUNT] = { DI1_PIN, DI2_PIN };

static bool     diLevel[DI_COUNT];        // 去彈跳後的電位
static bool     diLast[DI_COUNT];         // 上一次取樣電位
static uint32_t diChangeAt[DI_COUNT];     // 電位改變的時間
static bool     diAlarmState[DI_COUNT];   // 目前告警狀態

static bool     relayOn        = false;
static uint32_t pulseUntil     = 0;       // 點動自動關閉時間 (0 = 無)
static int      lastSchedMin   = -1;      // 已處理過的分鐘 (避免同一分鐘重複觸發)

#define DEBOUNCE_MS 50

void ioBegin() {
  for (int i = 0; i < DI_COUNT; i++) {
    pinMode(DI_PINS[i], INPUT_PULLUP);
    diLevel[i]      = digitalRead(DI_PINS[i]);
    diLast[i]       = diLevel[i];
    diChangeAt[i]   = millis();
    diAlarmState[i] = (diLevel[i] == LOW) == cfg.di[i].activeLow;
  }
  pinMode(RELAY_PIN, OUTPUT);
  doSet(false);
}

// 設定頁改了 activeLow 之後，重新依新極性推算告警狀態與輸出電位
void ioReapplyConfig() {
  for (int i = 0; i < DI_COUNT; i++) {
    bool isAlarm = (diLevel[i] == LOW) == cfg.di[i].activeLow;
    diAlarmState[i] = isAlarm;             // 視為設定變更，不重複寫入告警紀錄
  }
  doSet(relayOn);                          // 以新的 doActiveLow 重寫輸出腳位
}

// --------------------- DO ---------------------

bool doState() { return relayOn; }

void doSet(bool on) {
  relayOn = on;
  // doActiveLow = true 代表輸出 LOW 導通繼電器
  digitalWrite(RELAY_PIN, (on == cfg.doActiveLow) ? LOW : HIGH);
  if (!on) pulseUntil = 0;
}

void doPulse(uint32_t holdMs) {
  doSet(true);
  pulseUntil = millis() + holdMs;
  if (pulseUntil == 0) pulseUntil = 1;      // 避免 millis 溢位時剛好等於 0
}

void doPulse() { doPulse(cfg.pulseMs); }

String doStatusJson() {
  JSON_DOC(doc, 512);
  doc["on"]       = relayOn;
  doc["mode"]     = cfg.doMode;
  doc["pulseMs"]  = cfg.pulseMs;
  doc["pulsing"]  = pulseUntil != 0;
  doc["remainMs"] = pulseUntil ? (int32_t)(pulseUntil - millis()) : 0;
  String out;
  serializeJson(doc, out);
  return out;
}

// 定時排程：每到設定的分鐘就切換一次
static void scheduleCheck() {
  if (cfg.doMode != DO_MODE_SCHEDULE) return;

  time_t now;
  time(&now);
  if (now < 1600000000) return;             // 尚未校時，不執行排程

  struct tm t;
  localtime_r(&now, &t);
  int curMin = t.tm_hour * 60 + t.tm_min;
  if (curMin == lastSchedMin) return;
  lastSchedMin = curMin;

  for (int i = 0; i < SCHED_COUNT; i++) {
    ScheduleItem &s = cfg.sched[i];
    if (!s.enabled) continue;
    if (!(s.days & (1 << t.tm_wday))) continue;
    if (s.onH * 60 + s.onM == curMin) {
      Serial.printf("[do] 排程 %d 開啟\n", i + 1);
      doSet(true);
    }
    if (s.offH * 60 + s.offM == curMin) {
      Serial.printf("[do] 排程 %d 關閉\n", i + 1);
      doSet(false);
    }
  }
}

// --------------------- DI ---------------------

bool diRaw(uint8_t idx)   { return idx < DI_COUNT ? diLevel[idx] : true; }
bool diAlarm(uint8_t idx) { return idx < DI_COUNT ? diAlarmState[idx] : false; }

String diStatusJson() {
  JSON_DOC(doc, 1024);
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < DI_COUNT; i++) {
    JsonObject o = JSON_ADD_OBJ(arr);
    o["ch"]    = i + 1;
    o["name"]  = cfg.di[i].name;
    o["en"]    = cfg.di[i].enabled;
    o["level"] = diLevel[i] ? 1 : 0;
    o["alarm"] = diAlarmState[i];
  }
  String out;
  serializeJson(doc, out);
  return out;
}

static void onDiEdge(uint8_t i, bool isAlarm) {
  diAlarmState[i] = isAlarm;
  if (!cfg.di[i].enabled) return;

  String text = isAlarm ? cfg.di[i].alarmText : cfg.di[i].normalText;
  Serial.printf("[di] CH%d %s: %s\n", i + 1, isAlarm ? "告警" : "恢復", text.c_str());

  alarmAdd(i + 1, isAlarm, text);
  notifyPush(cfg.di[i].name + " " + text);
  mqttPublishAlarm(i + 1, isAlarm, text);
}

void ioLoop() {
  uint32_t now = millis();

  // DI 去彈跳取樣
  for (int i = 0; i < DI_COUNT; i++) {
    bool raw = digitalRead(DI_PINS[i]);
    if (raw != diLast[i]) {
      diLast[i]     = raw;
      diChangeAt[i] = now;
    } else if (raw != diLevel[i] && now - diChangeAt[i] >= DEBOUNCE_MS) {
      diLevel[i] = raw;
      bool isAlarm = (raw == LOW) == cfg.di[i].activeLow;
      if (isAlarm != diAlarmState[i]) onDiEdge(i, isAlarm);
    }
  }

  // 點動逾時關閉
  if (pulseUntil != 0 && (int32_t)(now - pulseUntil) >= 0) {
    pulseUntil = 0;
    doSet(false);
    Serial.println(F("[do] 點動時間到，關閉繼電器"));
  }

  scheduleCheck();
}

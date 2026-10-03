/*
 * mqtt_ctrl.h - MQTT 連線 / 發佈 / 訂閱
 *
 * 注意：PubSubClient 僅支援 publish QoS0、subscribe QoS0/1。
 *       網頁上可選到 QoS2，程式會自動降級並於狀態中回報 effSubQos。
 */
#ifndef MQTT_CTRL_H
#define MQTT_CTRL_H

#include <Arduino.h>

void   mqttBegin();
void   mqttLoop();
void   mqttRestart();                       // 設定變更後重新連線
bool   mqttConnected();
String mqttClientId();

bool   mqttPublish(const String &topic, const String &payload, bool retain = false);
void   mqttPublishAlarm(uint8_t ch, bool isAlarm, const String &text);

// 狀態主題（預設 retained，訂閱端一連上就能取得現況）
//   <pubTopic>/di/<n>   "on"=告警中  "off"=正常
//   <pubTopic>/do/<n>   "on"=導通    "off"=斷開
//   <pubTopic>/status   全體狀態 JSON 快照
void   mqttPublishDiState(uint8_t ch);     // ch 為 0-based
void   mqttPublishDoState();
void   mqttPublishStatus();
void   mqttPublishAll();                   // 連線成功後推送一次完整現況

String mqttStatusJson();
String mqttMessagesJson();
void   mqttClearMessages();

#endif

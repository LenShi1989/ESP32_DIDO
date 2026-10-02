/*
 * net_wifi.h - WiFi 連線 / AP 設定模式 / 掃描
 */
#ifndef NET_WIFI_H
#define NET_WIFI_H

#include <Arduino.h>

void   wifiBegin();                 // 依設定連線，失敗則開 AP
void   wifiLoop();                  // 斷線重連 (由 task 週期呼叫)
bool   wifiIsAp();                  // 目前是否為 AP 設定模式
String wifiApSsid();                // AP 模式的 SSID

void   wifiStartScan();             // 非同步啟動掃描
String wifiScanJson();              // 取得掃描結果 JSON (掃描中回 {"scanning":true})

void   wifiApplyNew(const String &ssid, const String &pass);  // 存檔後重新連線
void   wifiClearConfig();           // 清除連線設定並重開機

String wifiStatusJson();            // WiFi 連線資訊 JSON

#endif

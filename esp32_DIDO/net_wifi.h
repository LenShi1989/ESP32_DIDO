/*
 * net_wifi.h - WiFi 連線 / AP 設定模式 / 掃描
 */
#ifndef NET_WIFI_H
#define NET_WIFI_H

#include <Arduino.h>

void   wifiBegin();                 // 依設定連線，失敗則開 AP
void   wifiLoop();                  // 斷線重連 (由 task 週期呼叫)
bool   wifiIsAp();                  // 尚未取得 STA 連線 (AP 本身一律常開)
bool   wifiStaConnected();          // STA 是否已連線
String wifiApSsid();                // AP 的 SSID
String wifiApIp();                  // AP 的 IP (固定 192.168.4.1)

void   wifiStartScan();             // 非同步啟動掃描
void   wifiScanLoop();              // 掃描狀態機 (寬限期 / 重試 / 逾時)，由 wifiLoop 呼叫
String wifiScanJson();              // 取得掃描結果 JSON (掃描中回 scanning:true 與上次結果)

void   wifiApplyNew(const String &ssid, const String &pass);  // 存檔後直接重連，不重開機
void   wifiClearConfig();           // 清除連線設定 (AP 維持開啟)

String wifiStatusJson();            // WiFi 連線資訊 JSON

#endif

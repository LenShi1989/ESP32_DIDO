/*
 * display_ui.h - ST7789 240x240 狀態顯示
 *   WiFi 連線資訊 / MQTT 連線狀態 / DI 告警狀態 / DO 繼電器狀態
 */
#ifndef DISPLAY_UI_H
#define DISPLAY_UI_H

#include <Arduino.h>

void displayBegin();        // 初始化 + 開機圖
void displaySplash();       // 顯示開機 bitmap
void displayLoop();         // 由 task 週期呼叫 (內部自行限制更新頻率)
void displayForceRedraw();  // 強制重畫整頁 (例如 OTA 後)
void displayMessage(const String &line1, const String &line2);  // 全螢幕訊息 (OTA/重開機)

#endif

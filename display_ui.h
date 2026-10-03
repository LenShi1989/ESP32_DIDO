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

// 套用 cfg 內的反相 / 旋轉 / 色序設定（網頁改完即時生效，不必重開機）
void displayApplySettings();
// 校正用測試圖：色塊 + 邊框 + 角落標記，用來確認方向、色序與可視範圍
void displayTestPattern();
// 顯示開機圖並保留 15 秒。繪圖路徑與原始 sketch 相同，用來比對雜訊來源。
void displaySplashHold();
void applyBacklight();

#endif

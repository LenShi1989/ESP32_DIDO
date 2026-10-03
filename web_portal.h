/*
 * web_portal.h - AsyncWebServer：SPIFFS 靜態網頁 + REST API + OTA
 */
#ifndef WEB_PORTAL_H
#define WEB_PORTAL_H

#include <Arduino.h>

void webBegin();
bool webRebootPending();     // 有 API 要求重開機時為 true

// 供序列埠指令使用：清除登入帳號密碼（面板故障時的最後手段）
void authClearCredentials();

#endif

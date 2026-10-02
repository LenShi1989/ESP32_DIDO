/*
 * web_portal.h - AsyncWebServer：SPIFFS 靜態網頁 + REST API + OTA
 */
#ifndef WEB_PORTAL_H
#define WEB_PORTAL_H

#include <Arduino.h>

void webBegin();
bool webRebootPending();     // 有 API 要求重開機時為 true

#endif

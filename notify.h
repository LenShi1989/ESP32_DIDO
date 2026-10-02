/*
 * notify.h - Discord / Telegram 推播 (佇列 + 背景 task 送出，不阻塞 IO)
 */
#ifndef NOTIFY_H
#define NOTIFY_H

#include <Arduino.h>

#define NOTIFY_QUEUE_LEN 8

void   notifyBegin();
void   notifyPush(const String &msg);          // 丟進佇列，立即返回
void   notifyLoop();                           // 由背景 task 呼叫
String notifyLastResultJson();                 // 最近一次送出的結果

#endif

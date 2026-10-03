/*
 * io_ctrl.h - DI 告警監控 / DO 繼電器控制 (手動、定時、點動)
 */
#ifndef IO_CTRL_H
#define IO_CTRL_H

#include <Arduino.h>

void   ioBegin();
void   ioLoop();                      // 由 task 週期呼叫 (建議 10ms)
void   ioReapplyConfig();             // DI/DO 設定變更後重新套用極性

// --- DI ---
bool   diRaw(uint8_t idx);            // 原始腳位電位 (0/1)
bool   diAlarm(uint8_t idx);          // 是否處於告警狀態
String diStatusJson();

// --- DO ---
bool   doState();                     // 繼電器是否導通
void   doSet(bool on);                // 手動設定
void   doPulse();                     // 點動：導通後經過 pulseMs 自動關閉
void   doPulse(uint32_t holdMs);
String doStatusJson();

// 診斷用：直接對腳位做 ON/OFF 切換數次，繞過模式與排程邏輯。
// 聽得到繼電器咔噠聲即代表韌體與接線正常。
void   doSelfTest();

#endif

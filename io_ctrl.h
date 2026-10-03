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

// --- DO (ch 為 0-based 通道索引) ---
uint8_t doPin(uint8_t ch);            // 該通道的 GPIO 編號
bool   doState(uint8_t ch = 0);       // 是否導通
void   doSet(uint8_t ch, bool on);    // 手動設定
void   doPulse(uint8_t ch);           // 點動：導通後經過 pulseMs 自動關閉
void   doPulse(uint8_t ch, uint32_t holdMs);
String doStatusJson();                // 全通道狀態

// 診斷用：直接對腳位做 ON/OFF 切換數次，繞過模式與排程邏輯。
// 聽得到繼電器咔噠聲即代表韌體與接線正常。
void   doSelfTest(uint8_t ch);

#endif

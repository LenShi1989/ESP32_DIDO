/*
 * modbus_rtu.h - RS-485 Modbus RTU（Slave / Master 可切換）
 *
 *  接線：DI(TX) pin17 / RO(RX) pin16 / DE、RE 短路後接 pin14
 *
 *  ---- Slave 模式的位址對照（位址皆為 0-based，protocol address）----
 *    Coils            FC01 讀 / FC05、FC0F 寫
 *        0  DO1        1  DO2
 *    Discrete Inputs  FC02 讀
 *        0  DI1 告警    1  DI2 告警
 *    Input Registers  FC04 讀
 *        0  DI1 電位    1  DI2 電位
 *        2  DI1 告警    3  DI2 告警
 *        4  DO1 狀態    5  DO2 狀態
 *        6  RSSI (int16, dBm)
 *        7  運行秒數 高位    8  運行秒數 低位
 *        9  可用記憶體 (KB)
 *       10  WiFi 已連線 (0/1)
 *       11  MQTT 已連線 (0/1)
 *    Holding Registers FC03 讀 / FC06、FC10 寫
 *        0  DO1 (0/1)   1  DO2 (0/1)
 */
#ifndef MODBUS_RTU_H
#define MODBUS_RTU_H

#include <Arduino.h>

void   modbusBegin();
void   modbusLoop();                 // 由 taskRs485 週期呼叫（建議 2ms）
void   modbusRestart();              // 設定變更後重新套用序列埠參數

String modbusStatusJson();           // 模式、統計、序列埠參數
String modbusPollJson();             // Master 模式各筆輪詢的最新結果
void   modbusPollReset();            // 清除 Master 的統計與結果

#endif

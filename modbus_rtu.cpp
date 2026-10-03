#include "modbus_rtu.h"
#include "app_config.h"
#include "io_ctrl.h"
#include "mqtt_ctrl.h"
#include <WiFi.h>

#define MB_BUF_MAX   256
#define MB_MIN_FRAME   4          // 站號 + 功能碼 + CRC

// Modbus 例外碼
#define MB_EX_ILLEGAL_FUNCTION  0x01
#define MB_EX_ILLEGAL_ADDRESS   0x02
#define MB_EX_ILLEGAL_VALUE     0x03
#define MB_EX_SLAVE_FAILURE     0x04

static HardwareSerial &bus = Serial2;

static uint8_t  rxBuf[MB_BUF_MAX];
static uint16_t rxLen      = 0;
static uint32_t lastByteUs = 0;
static uint32_t frameGapUs = 4000;   // t3.5，依鮑率計算

static bool     started    = false;
static bool     needReinit = false;

// --- 統計 ---
static uint32_t statRx = 0, statTx = 0, statCrcErr = 0, statExc = 0, statTimeout = 0;

// --- Master 輪詢結果 ---
struct PollResult {
  bool     valid = false;
  uint32_t lastOkMs = 0;
  uint32_t okCount = 0, errCount = 0;
  uint16_t values[16];
  uint8_t  count = 0;
  String   err;
};
static PollResult pollRes[MB_POLL_MAX];
static uint32_t   pollNextAt[MB_POLL_MAX];
static int8_t     pollWaiting = -1;      // 等待回應中的輪詢索引
static uint32_t   pollSentAt  = 0;

// ---------------------------------------------------------------- 工具

static uint16_t crc16(const uint8_t *buf, uint16_t len) {
  uint16_t crc = 0xFFFF;
  for (uint16_t i = 0; i < len; i++) {
    crc ^= buf[i];
    for (uint8_t b = 0; b < 8; b++) {
      if (crc & 1) crc = (crc >> 1) ^ 0xA001;
      else         crc >>= 1;
    }
  }
  return crc;
}

static void setTx(bool on) {
  digitalWrite(RS485_DE_PIN, on ? HIGH : LOW);
}

static void sendFrame(uint8_t *buf, uint16_t len) {
  uint16_t crc = crc16(buf, len);
  buf[len++] = crc & 0xFF;              // CRC 低位在前
  buf[len++] = crc >> 8;

  setTx(true);
  delayMicroseconds(50);                // 等收發器切換完成
  bus.write(buf, len);
  bus.flush();                          // 等最後一個位元真的送出去
  delayMicroseconds(50);
  setTx(false);
  statTx++;
}

static void sendException(uint8_t id, uint8_t fc, uint8_t code) {
  uint8_t f[8];
  f[0] = id;
  f[1] = fc | 0x80;
  f[2] = code;
  sendFrame(f, 3);
  statExc++;
}

// ---------------------------------------------------------------- Slave 資料來源

// 線圈 = DO
static bool coilRead(uint16_t addr, bool &out) {
  if (addr >= DO_COUNT) return false;
  out = doState((uint8_t)addr);
  return true;
}
static bool coilWrite(uint16_t addr, bool on) {
  if (addr >= DO_COUNT) return false;
  doSet((uint8_t)addr, on);
  mqttPublishDoState();
  return true;
}

// 離散輸入 = DI 告警狀態
static bool discreteRead(uint16_t addr, bool &out) {
  if (addr >= DI_COUNT) return false;
  out = diAlarm((uint8_t)addr);
  return true;
}

// 輸入暫存器：狀態快照
static bool inputRegRead(uint16_t addr, uint16_t &out) {
  uint32_t up = millis() / 1000;
  switch (addr) {
    case 0: case 1:
      if (addr >= DI_COUNT) return false;
      out = diRaw((uint8_t)addr) ? 1 : 0; return true;
    case 2: case 3:
      if (addr - 2 >= DI_COUNT) return false;
      out = diAlarm((uint8_t)(addr - 2)) ? 1 : 0; return true;
    case 4: case 5:
      if (addr - 4 >= DO_COUNT) return false;
      out = doState((uint8_t)(addr - 4)) ? 1 : 0; return true;
    case 6:  out = (uint16_t)(int16_t)WiFi.RSSI();        return true;
    case 7:  out = (uint16_t)(up >> 16);                  return true;
    case 8:  out = (uint16_t)(up & 0xFFFF);               return true;
    case 9:  out = (uint16_t)(ESP.getFreeHeap() / 1024);  return true;
    case 10: out = WiFi.status() == WL_CONNECTED ? 1 : 0; return true;
    case 11: out = mqttConnected() ? 1 : 0;               return true;
    default: return false;
  }
}

// 保持暫存器：DO 狀態，可讀可寫
static bool holdRegRead(uint16_t addr, uint16_t &out) {
  if (addr >= DO_COUNT) return false;
  out = doState((uint8_t)addr) ? 1 : 0;
  return true;
}
static bool holdRegWrite(uint16_t addr, uint16_t val) {
  if (addr >= DO_COUNT) return false;
  doSet((uint8_t)addr, val != 0);
  mqttPublishDoState();
  return true;
}

// ---------------------------------------------------------------- Slave 處理

static void handleSlave(uint8_t *f, uint16_t len) {
  uint8_t id = f[0];
  uint8_t fc = f[1];

  // 站號 0 為廣播，處理但不回應
  bool broadcast = (id == 0);
  if (!broadcast && id != cfg.mbSlaveId) return;

  uint8_t  rsp[MB_BUF_MAX];
  uint16_t rlen = 0;
  rsp[rlen++] = cfg.mbSlaveId;
  rsp[rlen++] = fc;

  switch (fc) {
    // ---- 讀線圈 / 離散輸入 ----
    case 0x01:
    case 0x02: {
      if (len < 8) return;
      uint16_t addr = (f[2] << 8) | f[3];
      uint16_t qty  = (f[4] << 8) | f[5];
      if (qty < 1 || qty > 2000) { if (!broadcast) sendException(id, fc, MB_EX_ILLEGAL_VALUE); return; }

      uint8_t bytes = (qty + 7) / 8;
      rsp[rlen++] = bytes;
      for (uint8_t i = 0; i < bytes; i++) rsp[rlen + i] = 0;

      for (uint16_t i = 0; i < qty; i++) {
        bool v = false;
        bool ok = (fc == 0x01) ? coilRead(addr + i, v) : discreteRead(addr + i, v);
        if (!ok) { if (!broadcast) sendException(id, fc, MB_EX_ILLEGAL_ADDRESS); return; }
        if (v) rsp[rlen + i / 8] |= (1 << (i % 8));
      }
      rlen += bytes;
      break;
    }

    // ---- 讀保持暫存器 / 輸入暫存器 ----
    case 0x03:
    case 0x04: {
      if (len < 8) return;
      uint16_t addr = (f[2] << 8) | f[3];
      uint16_t qty  = (f[4] << 8) | f[5];
      if (qty < 1 || qty > 125) { if (!broadcast) sendException(id, fc, MB_EX_ILLEGAL_VALUE); return; }

      rsp[rlen++] = qty * 2;
      for (uint16_t i = 0; i < qty; i++) {
        uint16_t v = 0;
        bool ok = (fc == 0x03) ? holdRegRead(addr + i, v) : inputRegRead(addr + i, v);
        if (!ok) { if (!broadcast) sendException(id, fc, MB_EX_ILLEGAL_ADDRESS); return; }
        rsp[rlen++] = v >> 8;
        rsp[rlen++] = v & 0xFF;
      }
      break;
    }

    // ---- 寫單一線圈 ----
    case 0x05: {
      if (len < 8) return;
      uint16_t addr = (f[2] << 8) | f[3];
      uint16_t val  = (f[4] << 8) | f[5];
      if (val != 0x0000 && val != 0xFF00) { if (!broadcast) sendException(id, fc, MB_EX_ILLEGAL_VALUE); return; }
      if (!coilWrite(addr, val == 0xFF00)) { if (!broadcast) sendException(id, fc, MB_EX_ILLEGAL_ADDRESS); return; }
      for (uint8_t i = 2; i < 6; i++) rsp[rlen++] = f[i];   // 原樣回應
      break;
    }

    // ---- 寫單一保持暫存器 ----
    case 0x06: {
      if (len < 8) return;
      uint16_t addr = (f[2] << 8) | f[3];
      uint16_t val  = (f[4] << 8) | f[5];
      if (!holdRegWrite(addr, val)) { if (!broadcast) sendException(id, fc, MB_EX_ILLEGAL_ADDRESS); return; }
      for (uint8_t i = 2; i < 6; i++) rsp[rlen++] = f[i];
      break;
    }

    // ---- 寫多個線圈 ----
    case 0x0F: {
      if (len < 10) return;
      uint16_t addr  = (f[2] << 8) | f[3];
      uint16_t qty   = (f[4] << 8) | f[5];
      uint8_t  bytes = f[6];
      if (qty < 1 || bytes != (qty + 7) / 8 || len < 9 + bytes) {
        if (!broadcast) sendException(id, fc, MB_EX_ILLEGAL_VALUE);
        return;
      }
      for (uint16_t i = 0; i < qty; i++) {
        bool v = (f[7 + i / 8] >> (i % 8)) & 1;
        if (!coilWrite(addr + i, v)) { if (!broadcast) sendException(id, fc, MB_EX_ILLEGAL_ADDRESS); return; }
      }
      for (uint8_t i = 2; i < 6; i++) rsp[rlen++] = f[i];
      break;
    }

    // ---- 寫多個保持暫存器 ----
    case 0x10: {
      if (len < 11) return;
      uint16_t addr  = (f[2] << 8) | f[3];
      uint16_t qty   = (f[4] << 8) | f[5];
      uint8_t  bytes = f[6];
      if (qty < 1 || bytes != qty * 2 || len < 9 + bytes) {
        if (!broadcast) sendException(id, fc, MB_EX_ILLEGAL_VALUE);
        return;
      }
      for (uint16_t i = 0; i < qty; i++) {
        uint16_t v = (f[7 + i * 2] << 8) | f[8 + i * 2];
        if (!holdRegWrite(addr + i, v)) { if (!broadcast) sendException(id, fc, MB_EX_ILLEGAL_ADDRESS); return; }
      }
      for (uint8_t i = 2; i < 6; i++) rsp[rlen++] = f[i];
      break;
    }

    default:
      if (!broadcast) sendException(id, fc, MB_EX_ILLEGAL_FUNCTION);
      return;
  }

  if (!broadcast) sendFrame(rsp, rlen);
}

// ---------------------------------------------------------------- Master 處理

static void masterSend(int idx) {
  MbPollItem &it = cfg.mbPoll[idx];
  uint8_t f[8];
  f[0] = it.slaveId;
  f[1] = it.fc;
  f[2] = it.addr >> 8;
  f[3] = it.addr & 0xFF;
  f[4] = it.count >> 8;
  f[5] = it.count & 0xFF;
  sendFrame(f, 6);
  pollWaiting = idx;
  pollSentAt  = millis();
}

static void masterHandleResponse(uint8_t *f, uint16_t len) {
  if (pollWaiting < 0) return;
  int idx = pollWaiting;
  MbPollItem &it = cfg.mbPoll[idx];
  PollResult &pr = pollRes[idx];
  pollWaiting = -1;

  if (f[0] != it.slaveId) { pr.errCount++; pr.err = "站號不符"; return; }

  if (f[1] & 0x80) {                       // 例外回應
    pr.errCount++;
    pr.err = String("例外碼 ") + (len >= 3 ? f[2] : 0);
    pr.valid = false;
    return;
  }

  uint8_t n = 0;
  if (it.fc == 1 || it.fc == 2) {          // 位元類：每個位元展開成一個值
    uint8_t bytes = f[2];
    if (len < 3 + bytes + 2) { pr.errCount++; pr.err = "長度不足"; return; }
    for (uint16_t i = 0; i < it.count && n < 16; i++, n++)
      pr.values[n] = (f[3 + i / 8] >> (i % 8)) & 1;
  } else {                                 // 暫存器類
    uint8_t bytes = f[2];
    if (len < 3 + bytes + 2) { pr.errCount++; pr.err = "長度不足"; return; }
    for (uint16_t i = 0; i < it.count && n < 16; i++, n++)
      pr.values[n] = (f[3 + i * 2] << 8) | f[4 + i * 2];
  }
  pr.count    = n;
  pr.valid    = true;
  pr.lastOkMs = millis();
  pr.okCount++;
  pr.err      = "";

  if (cfg.mbPublish && cfg.pubTopic.length()) {
    JSON_DOC(doc, 512);
    doc["name"] = it.name;
    doc["id"]   = it.slaveId;
    doc["fc"]   = it.fc;
    doc["addr"] = it.addr;
    JsonArray a = JSON_SUB_ARR(doc, "values");
    for (uint8_t i = 0; i < n; i++) a.add(pr.values[i]);
    String body;
    serializeJson(doc, body);
    mqttPublish(cfg.pubTopic + "/modbus/" + String(idx + 1), body, cfg.mqttRetain);
  }
}

static void masterLoop() {
  uint32_t now = millis();

  // 等待回應中
  if (pollWaiting >= 0) {
    if (now - pollSentAt >= cfg.mbTimeoutMs) {
      PollResult &pr = pollRes[pollWaiting];
      pr.errCount++;
      pr.valid = false;
      pr.err   = "逾時無回應";
      statTimeout++;
      pollWaiting = -1;
    }
    return;                                 // 一次只跑一筆，避免匯流排衝突
  }

  for (int i = 0; i < MB_POLL_MAX; i++) {
    MbPollItem &it = cfg.mbPoll[i];
    if (!it.enabled || it.periodSec == 0) continue;
    if ((int32_t)(now - pollNextAt[i]) < 0) continue;
    pollNextAt[i] = now + it.periodSec * 1000UL;
    masterSend(i);
    return;
  }
}

// ---------------------------------------------------------------- 對外介面

static void applySerial() {
  uint32_t cfgBits;
  if (cfg.mbStopBits == 2) {
    cfgBits = cfg.mbParity == 1 ? SERIAL_8E2 : (cfg.mbParity == 2 ? SERIAL_8O2 : SERIAL_8N2);
  } else {
    cfgBits = cfg.mbParity == 1 ? SERIAL_8E1 : (cfg.mbParity == 2 ? SERIAL_8O1 : SERIAL_8N1);
  }

  if (started) bus.end();
  bus.begin(cfg.mbBaud, cfgBits, RS485_RX_PIN, RS485_TX_PIN);
  bus.setTimeout(0);
  started = true;

  // t3.5：3.5 個字元的靜默時間。高鮑率時下限取 2ms，避免 loop 週期追不上。
  frameGapUs = (uint32_t)(38500000UL / cfg.mbBaud);
  if (frameGapUs < 2000) frameGapUs = 2000;

  rxLen       = 0;
  pollWaiting = -1;
  for (int i = 0; i < MB_POLL_MAX; i++) pollNextAt[i] = millis() + 1000;

  Serial.printf("[mb] %s  %lu-8%c%d  站號%d  t3.5=%luus",
                cfg.mbMode == MB_MASTER ? "Master" : "Slave",
                (unsigned long)cfg.mbBaud,
                cfg.mbParity == 1 ? 'E' : (cfg.mbParity == 2 ? 'O' : 'N'),
                cfg.mbStopBits, cfg.mbSlaveId, (unsigned long)frameGapUs);
  Serial.println();
}

void modbusBegin() {
  pinMode(RS485_DE_PIN, OUTPUT);
  setTx(false);                             // 預設接收
  if (cfg.mbEnabled) applySerial();
}

void modbusRestart() { needReinit = true; }

void modbusLoop() {
  if (needReinit) {
    needReinit = false;
    if (cfg.mbEnabled) applySerial();
    else if (started) { bus.end(); started = false; Serial.println(F("[mb] 已停用")); }
  }
  if (!cfg.mbEnabled || !started) return;

  // 收位元組，以靜默時間判斷訊框結束
  while (bus.available()) {
    uint8_t b = bus.read();
    if (rxLen < MB_BUF_MAX) rxBuf[rxLen++] = b;
    lastByteUs = micros();
  }

  if (rxLen > 0 && (micros() - lastByteUs) >= frameGapUs) {
    uint16_t len = rxLen;
    rxLen = 0;

    if (len >= MB_MIN_FRAME) {
      uint16_t got  = (rxBuf[len - 1] << 8) | rxBuf[len - 2];
      uint16_t calc = crc16(rxBuf, len - 2);
      if (got != calc) {
        statCrcErr++;
      } else {
        statRx++;
        if (cfg.mbMode == MB_MASTER) masterHandleResponse(rxBuf, len);
        else                         handleSlave(rxBuf, len);
      }
    }
  }

  if (cfg.mbMode == MB_MASTER) masterLoop();
}

String modbusStatusJson() {
  JSON_DOC(doc, 768);
  doc["enabled"]  = cfg.mbEnabled;
  doc["mode"]     = cfg.mbMode;
  doc["baud"]     = cfg.mbBaud;
  doc["parity"]   = cfg.mbParity;
  doc["stopBits"] = cfg.mbStopBits;
  doc["slaveId"]  = cfg.mbSlaveId;
  doc["timeout"]  = cfg.mbTimeoutMs;
  doc["publish"]  = cfg.mbPublish;
  doc["txPin"]    = RS485_TX_PIN;
  doc["rxPin"]    = RS485_RX_PIN;
  doc["dePin"]    = RS485_DE_PIN;

  JsonObject st = JSON_SUB_OBJ(doc, "stat");
  st["rx"]      = statRx;
  st["tx"]      = statTx;
  st["crcErr"]  = statCrcErr;
  st["exc"]     = statExc;
  st["timeout"] = statTimeout;

  String out;
  serializeJson(doc, out);
  return out;
}

String modbusPollJson() {
  JSON_DOC(doc, 2048);
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < MB_POLL_MAX; i++) {
    JsonObject o = JSON_ADD_OBJ(arr);
    o["idx"]     = i;
    o["en"]      = cfg.mbPoll[i].enabled;
    o["name"]    = cfg.mbPoll[i].name;
    o["id"]      = cfg.mbPoll[i].slaveId;
    o["fc"]      = cfg.mbPoll[i].fc;
    o["addr"]    = cfg.mbPoll[i].addr;
    o["count"]   = cfg.mbPoll[i].count;
    o["period"]  = cfg.mbPoll[i].periodSec;
    o["valid"]   = pollRes[i].valid;
    o["ok"]      = pollRes[i].okCount;
    o["err"]     = pollRes[i].errCount;
    o["errMsg"]  = pollRes[i].err;
    o["ageMs"]   = pollRes[i].lastOkMs ? (uint32_t)(millis() - pollRes[i].lastOkMs) : 0;
    JsonArray v = JSON_SUB_ARR(o, "values");
    for (uint8_t k = 0; k < pollRes[i].count; k++) v.add(pollRes[i].values[k]);
  }
  String out;
  serializeJson(doc, out);
  return out;
}

void modbusPollReset() {
  statRx = statTx = statCrcErr = statExc = statTimeout = 0;
  for (int i = 0; i < MB_POLL_MAX; i++) {
    pollRes[i] = PollResult();
    pollNextAt[i] = millis();
  }
  pollWaiting = -1;
}

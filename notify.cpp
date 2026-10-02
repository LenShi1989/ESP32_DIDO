#include "notify.h"
#include "app_config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

static String        queueBuf[NOTIFY_QUEUE_LEN];
static volatile int  qHead = 0, qTail = 0;
static SemaphoreHandle_t qLock = nullptr;

static int    lastDiscordCode  = 0;
static int    lastTelegramCode = 0;
static String lastMessage      = "";

void notifyBegin() {
  if (!qLock) qLock = xSemaphoreCreateMutex();
}

void notifyPush(const String &msg) {
  if (!qLock) notifyBegin();
  xSemaphoreTake(qLock, portMAX_DELAY);
  int next = (qHead + 1) % NOTIFY_QUEUE_LEN;
  if (next != qTail) {                       // 佇列未滿
    queueBuf[qHead] = msg;
    qHead = next;
  } else {
    Serial.println(F("[notify] 佇列已滿，丟棄訊息"));
  }
  xSemaphoreGive(qLock);
}

static bool queuePop(String &out) {
  if (!qLock) return false;
  bool got = false;
  xSemaphoreTake(qLock, portMAX_DELAY);
  if (qTail != qHead) {
    out   = queueBuf[qTail];
    qTail = (qTail + 1) % NOTIFY_QUEUE_LEN;
    got   = true;
  }
  xSemaphoreGive(qLock);
  return got;
}

// JSON 字串跳脫
static String jsonEscape(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '"':  o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n";  break;
      case '\r': o += "\\r";  break;
      case '\t': o += "\\t";  break;
      default:   o += c;      break;
    }
  }
  return o;
}

static int postJson(const String &url, const String &body) {
  WiFiClientSecure client;
  client.setInsecure();                      // ESP32 上不驗證憑證，避免需要維護根憑證
  client.setTimeout(10);

  HTTPClient http;
  if (!http.begin(client, url)) return -1;
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);
  int code = http.POST(body);
  if (code <= 0) {
    Serial.printf("[notify] HTTP 失敗 %s\n", http.errorToString(code).c_str());
  }
  http.end();
  return code;
}

static void sendDiscord(const String &msg) {
  if (!cfg.discordEnabled || cfg.discordWebhook.length() == 0) return;
  String body = "{\"content\":\"" + jsonEscape(msg) + "\"}";
  lastDiscordCode = postJson(cfg.discordWebhook, body);
  Serial.printf("[notify] Discord -> %d\n", lastDiscordCode);
}

static void sendTelegram(const String &msg) {
  if (!cfg.telegramEnabled || cfg.telegramToken.length() == 0 ||
      cfg.telegramChatId.length() == 0) return;
  String url  = "https://api.telegram.org/bot" + cfg.telegramToken + "/sendMessage";
  String body = "{\"chat_id\":\"" + jsonEscape(cfg.telegramChatId) +
                "\",\"text\":\"" + jsonEscape(msg) + "\"}";
  lastTelegramCode = postJson(url, body);
  Serial.printf("[notify] Telegram -> %d\n", lastTelegramCode);
}

void notifyLoop() {
  String msg;
  if (!queuePop(msg)) return;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[notify] 無網路，略過推播"));
    return;
  }
  lastMessage = msg;
  sendDiscord(msg);
  sendTelegram(msg);
}

String notifyLastResultJson() {
  JSON_DOC(doc, 512);
  doc["msg"]      = lastMessage;
  doc["discord"]  = lastDiscordCode;
  doc["telegram"] = lastTelegramCode;
  doc["dcEn"]     = cfg.discordEnabled;
  doc["tgEn"]     = cfg.telegramEnabled;
  String out;
  serializeJson(doc, out);
  return out;
}

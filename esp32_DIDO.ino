/*
//==ESP32 D1 mini==
#define TFT_CS    5  
#define TFT_DC   19  
#define TFT_MOSI 23  
#define TFT_SCLK 18  
#define TFT_RST   0   
#define TFT_MISO 19  
//       3.3v        
//       Gnd         
*/


#include <Arduino.h>
#include "Guineapig.WiFiConfig.h"
#include "web-assets.h"
#include <PubSubClient.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "ST7789.h"
#include "bitmap.h"


#pragma region Async Web Server ***************************
#include <ESPAsyncWebServer.h>
#ifdef ESP32
#include <WiFi.h>
#define LED_ON_VAL HIGH
#define LED_OFF_VAL LOW
#else
#include <ESP8266WiFi.h>
#define LED_ON_VAL LOW
#define LED_OFF_VAL HIGH
#endif
AsyncWebServer webServer(80);
#pragma endregion *****************************************


//==button area==//
#define BUTTON_PIN1 32      // 按鍵的接腳
#define BUTTON_PIN2 33
#define Relay_PIN    4      // 繼電器的接腳
bool buttonState = 0;       // 按鈕的狀態

//==WiFi connect area==//
bool resetWifiFlag = false;
int curr_led_val = LED_ON_VAL;

//==MQTT area==
// ------ 以下修改成你MQTT設定 ------
const char* mqtt_server = "broker.mqttgo.io";    //註冊MQTT伺服器
const unsigned int mqtt_port = 1883;
#define MQTT_USER        "my_name"               //本案例未使用
#define MQTT_PASSWORD    "my_password"           //本案例未使用
#define MQTT_Topic_1     "esp32/len"             //topic 主題
#define MQTT_Topic_2     "esp32/len/receive"     //topic 主題
#define MQTT_Topic_3     "esp32/len/alarm"       //topic 主題

char clientId[50];
void mqtt_callback(char* topic, byte* payload, unsigned int msgLength);
WiFiClient wifiClient;
PubSubClient mqttClient(mqtt_server, mqtt_port, mqtt_callback, wifiClient);
int meg = 0;
long last_time = millis();
bool aflag = false;

void task_0(void*);
void task_1(void*);
void task_2(void*);

TaskHandle_t TaskHandle_0;
TaskHandle_t TaskHandle_1;
TaskHandle_t TaskHandle_2;


ST7789 tft = ST7789(); // Invoke library, pins defined in User_Setup.h

int time1 = 100;
int var = 0;

String processor(const String& var) {
  if (var == "LED_ONOFF") return String(curr_led_val == LED_ON_VAL ? "on" : "off");
  return String();
}
String procBulbOn(const String& var) {
  if (var == "COLOR1") return "#fedc94";
  else if (var == "COLOR2") return "#f2ce75";
  return String();
}
String procBulbOff(const String& var) {
  if (var == "COLOR1") return "#ccc";
  else if (var == "COLOR2") return "#ccc";
  return String();
}

void WiFiconnecting() {
  //嘗試連上無線網路，若成功傳回 true 繼續作業；若失敗則啟用 AP 模式讓使用者連上來設定網路
  if (WiFiConfig.connectWiFi())
  {
    pinMode(LED_BUILTIN, OUTPUT);
    //建立一個小網站讓使用者開關LED燈及清除網路設定(方便實驗觀察用，一般應用時不太需要)
    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send_P(200, "text/html", index_html, processor);
    });
    webServer.on("/bulb-on.svg", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send_P(200, "image/svg+xml", bulb_svg, procBulbOn);
    });
    webServer.on("/bulb-off.svg", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send_P(200, "image/svg+xml", bulb_svg, procBulbOff);
    });
    //點燈
    webServer.on("/led/on", HTTP_GET, [](AsyncWebServerRequest *request) {
      digitalWrite(LED_BUILTIN, LED_ON_VAL);
      curr_led_val = LED_ON_VAL;
      request->send(200, "text/plain", "OK");
    });
    //關燈
    webServer.on("/led/off", HTTP_GET, [](AsyncWebServerRequest *request) {
      digitalWrite(LED_BUILTIN, LED_OFF_VAL);
      curr_led_val = LED_OFF_VAL;
      request->send(200, "text/plain", "OK");
    });
    //重設網路
    webServer.on("/reset-wifi", HTTP_GET, [](AsyncWebServerRequest *request) {
      resetWifiFlag = true;
      request->send(200, "text/plain", "Reset and rebooting...");
    });
    webServer.begin();
    digitalWrite(LED_BUILTIN, curr_led_val);
  }
  delay(0);
}

void DI2DO() {
  buttonState = digitalRead(BUTTON_PIN1);                //讀取按鍵的狀態
  if(buttonState == LOW){                               //如果按鍵按了
    if (millis() - last_time >= 5000) {
      mqttClient.publish(MQTT_Topic_1, "on");           //發佈
      mqttClient.publish(MQTT_Topic_3, "推播緊急告警"); //發佈
      printf("推播緊急告警\n");
      aflag = true;
      last_time = millis();
    }
  }
  
  else if (buttonState == 1 and aflag == true) {        //如果按鍵是未按下
    mqttClient.publish(MQTT_Topic_1, "off");           //發佈
    mqttClient.publish(MQTT_Topic_3, "警報正常");      //發佈
    aflag = false;
  }
}

void ResetWiFi() {
  if (resetWifiFlag)
  {
    delay(3000);
    WiFiConfig.clearWiFiConfig();
  }
}

//重新連線MQTT Server
boolean mqtt_nonblock_reconnect() {
  boolean doConn = false;
  if (! mqttClient.connected()) {
    boolean isConn = mqttClient.connect(clientId);
    //boolean isConn = mqttClient.connect(clientId, MQTT_USER, MQTT_PASSWORD);
    char logConnected[100];
    sprintf(logConnected, "MQTT Client [%s] Connect %s !", clientId, (isConn ? "Successful" : "Failed"));
    Serial.println(logConnected);
    mqttClient.subscribe(MQTT_Topic_1);    //訂閱
  }
  return doConn;
}

//MQTT callback 需加 mqttClient.loop();
void mqtt_callback(char* topic, byte* payload, unsigned int msgLength){
  Serial.print("Payload arrived on topic: ");
  Serial.print(topic);
  Serial.print(". Payload: ");
  String message;
  
  for (int i = 0; i < msgLength; i++) {
    Serial.print((char)payload[i]);
    message += (char)payload[i];
  }
  Serial.println();
  
  if (String(topic) == MQTT_Topic_1) {
    Serial.print("Changing output to ");
    if(message == "on"){
      Serial.println("on");
      digitalWrite(Relay_PIN, LOW);
      mqttClient.publish(MQTT_Topic_2, "on");    //發佈
    }
    else if(message == "off"){
      Serial.println("off");
      digitalWrite(Relay_PIN, HIGH);
      mqttClient.publish(MQTT_Topic_2, "off");   //發佈
    }
  }
}

//MQTT傳遞訊息
void MQTT_TX() {  
  if (! mqttClient.connected()) {
    // client loses its connection
    Serial.printf("MQTT Client [%s] Connection LOST !\n", clientId);
    mqtt_nonblock_reconnect();
  }
  
  if (millis() - last_time >= 5000) {
    meg = 22753;
    //int to char
    String str_String = String(meg);
    char megString[50];
    itoa(meg, megString, 10);    //參數10進位
    Serial.print("meg: ");
    Serial.println(megString);
    mqttClient.publish(MQTT_Topic_1, megString);    //發佈
    last_time = millis();
  }
}

void tecom() {
  tft.pushImage(0,0,240,238,tecom1);
  delay(time1);
}

//core 1
void task_0(void *pvParameters){
  for(;;){
    ResetWiFi();
    vTaskDelay(10);
  }
}

//core 1
void task_1(void *pvParameters){
  for(;;){
    mqtt_nonblock_reconnect();
    // MQTT_TX();                   //用MQTT傳訊息
    mqttClient.loop();
    vTaskDelay(10);
  }
}

//core 0
void task_2(void *pvParameters){
  for(;;){
    DI2DO();
    vTaskDelay(10);
  }
}


void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); //disable brownout detector
  Serial.begin(115200);
  pinMode(BUTTON_PIN1, INPUT_PULLUP);   //設定按鈕的接腳為輸入，因為我們要讀取它的狀態
  pinMode(LED_BUILTIN, OUTPUT);     //設定LED的PIN腳為輸出
  pinMode(Relay_PIN, OUTPUT);       //設定繼電器的PIN腳為輸出
  digitalWrite(Relay_PIN, HIGH);    //拉LOW觸發繼電器
  Serial.print("ST7789 TFT Bitmap Test");
  pinMode(0, OUTPUT);
  digitalWrite(0, HIGH);
  delay(100);
  digitalWrite(0, LOW);
  delay(100);
  digitalWrite(0, HIGH);

  tft.begin();              // initialize a ST7789 chip
  tft.setSwapBytes(false);  // Swap the byte order for pushImage() - corrects endianness
  tft.fillScreen(TFT_BLACK);
  tecom();

  WiFiconnecting();                    //啟動WIFI連線
  sprintf(clientId, "ESP32CAM_%04X", random(0xffff));  // Create a random client ID
  mqtt_nonblock_reconnect();           //啟動MQTT連線
  last_time = millis();
  //副程式, 任務名稱, 堆疊區10000, 輸入值, 優先序：0代表最低, 任務handle變數, 核心編號                   
  xTaskCreatePinnedToCore(task_0, "wifi連線", 10000, NULL, 1, &TaskHandle_0, 1);  //core 1
  xTaskCreatePinnedToCore(task_1, "mqtt連線", 10000, NULL, 1, &TaskHandle_1, 1);  //core 1
  xTaskCreatePinnedToCore(task_2, "DIDO作動", 10000, NULL, 1, &TaskHandle_2, 0);  //core 0
}

void loop() {
  tecom();
  Serial.print("tecom= ");  Serial.println(millis());
  /*  
  var = var+1;
  if (var > 3 ){var = 0;}
  
  switch (var) {
      case 1:    
        Serial.print("mao= ");  Serial.println(millis());
        mao();
        break;
      case 2:
        Serial.print("miku= ");  Serial.println(millis());
        miku();
        break;
      default:
        Serial.print("maomiku= ");  Serial.println(millis());
        maomiku();
        break;
  }  
  delay(1000);
 */ 
}



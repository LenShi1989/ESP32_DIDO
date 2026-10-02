# ESP32_DIDO

ESP32 D1 mini 的 DI / DO 模組韌體。
網頁前端放在 **SPIFFS**（`data/`），後端 `.ino` 負責 IO、MQTT 與 **ST7789** 狀態顯示。

---

## 功能

### 前端（SPIFFS 內的網頁，側邊欄式介面）

| 側邊欄        | 內容                                                                       |
| :------------ | :------------------------------------------------------------------------- |
| 系統狀態      | WiFi 連線資訊、SPIFFS 檔案目錄與容量、系統資訊                             |
| WiFi 設定     | 網路掃描 SSID、手動輸入 SSID / 密碼、清除連線設定                          |
| DI 設定       | 自定義觸發／解除告警文字、告警紀錄（最新 10 筆循環）、Discord / Telegram 推播 |
| DO 設定       | Switch 控制繼電器 ON/OFF、定時（時間排程）、點動（保持時間）               |
| MQTT 設定     | Broker / Port / ClientID（自動或手動）、Publish（Topic/QoS/訊息）、Subscriptions |
| OTA 更新      | 網頁上傳韌體 `.bin` 或檔案系統 `spiffs.bin`                                |
| 使用者設定    | 設定登入帳號及密碼（HTTP Basic 驗證，預設 `admin` / `admin`）              |

### 後端（`esp32_DIDO.ino` + 模組）

ST7789 240x240 螢幕即時顯示：WiFi 連線資訊、MQTT 連線狀態、DI 告警狀態、DO 繼電器狀態。

---

## 檔案結構

```
esp32_DIDO.ino      主程式：初始化與 FreeRTOS 任務分配
app_config.*        設定結構、SPIFFS JSON 存取、告警紀錄（10 筆循環）
net_wifi.*          WiFi 連線、AP 設定模式、SSID 掃描
io_ctrl.*           DI 去彈跳與告警、DO 手動 / 定時 / 點動
notify.*            Discord / Telegram 推播（佇列 + 背景任務）
mqtt_ctrl.*         MQTT 連線、發佈、訂閱
display_ui.*        ST7789 狀態畫面
web_portal.*        AsyncWebServer：靜態網頁 + REST API + OTA
ST7789.*            顯示器驅動（TFT_eSPI 子集）
bitmap.h            開機圖
data/               SPIFFS 內容：index.html、css/style.css、js/app.js
legacy/             舊版 Guineapig WiFiConfig 與內嵌 HTML（已停用，不參與編譯）
```

任務分配：

| 任務      | 核心  | 工作                            |
| :-------- | :---- | :------------------------------ |
| `io`      | 0     | DI 取樣、DO 點動與排程（10 ms） |
| `net`     | 1     | WiFi 重連、MQTT                 |
| `notify`  | 1     | HTTPS 推播（會阻塞，獨立任務）  |
| `display` | 1     | ST7789 畫面更新                 |

---

## 相依函式庫

| 函式庫              | 來源 / 套件名稱                                      | 實測版本 | 版本要求           |
| :------------------ | :--------------------------------------------------- | :------- | :----------------- |
| ESP Async WebServer | [ESP32Async/ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer) | 3.12.1   | **3.2.0 以上**     |
| Async TCP           | [ESP32Async/AsyncTCP](https://github.com/ESP32Async/AsyncTCP) | 3.5.0    | 3.x                |
| PubSubClient        | knolleary                                            | 2.8      | 2.8                |
| ArduinoJson         | bblanchon                                            | 7.4.2    | 6.x 或 7.x 皆可    |

開發環境：Arduino IDE 1.8.19 + ESP32 core **3.3.10**，
板子 WEMOS D1 MINI ESP32（已實測通過編譯）。

### ⚠️ ESPAsyncWebServer 必須用 ESP32Async 版

ESP32 core 3.x 內建的是 **mbedTLS 3.x**，已移除 `mbedtls_md5_starts_ret()` 這組帶 `_ret`
後綴的舊 API。以下常見的舊 fork 仍在呼叫它們，一旦用到 HTTP Basic 驗證就會編譯失敗：

- `me-no-dev/ESPAsyncWebServer`（原版，已停更）
- `lacamera/ESPAsyncWebServer` 3.1.0
- `dvarrel/ESPAsyncWebSrv`（連標頭檔名都不同，是 `ESPAsyncWebSrv.h`）

錯誤訊息長這樣：

```
WebAuthentication.cpp:74:3: error: 'mbedtls_md5_starts_ret' was not declared in this scope;
   did you mean 'mbedtls_md5_starts'?
```

**解法**：把 `Arduino/libraries/` 底下舊的 `ESPAsyncWebServer` / `ESPAsyncWebSrv` /
`ESPAsyncTCP` / `AsyncTCP-master` 資料夾整個移走（不要只改名留在 `libraries/` 裡，
Arduino 仍會掃到而撞名），改安裝 ArduinoIDE 函式庫管理員中作者為 **ESP32Async** 的
「ESP Async WebServer」與「Async TCP」，裝完重開 IDE。

---

## 燒錄步驟

1. **Partition Scheme 必須改成 `Minimal SPIFFS (Large APPS with OTA)`**
   （`min_spiffs`：APP 1.875MB x2 + SPIFFS 128KB）。
   預設的 `Default` 只給 APP 1.2MB，放不下目前約 1.36MB 的韌體。
2. 用 *ESP32 Sketch Data Upload*（或 PlatformIO `uploadfs`）把 `data/` 上傳到 SPIFFS。
   `data/` 約 40KB，128KB 的 SPIFFS 分區夠用。
3. 燒錄韌體。
4. 首次開機若無 WiFi 設定，裝置會開 AP：`ESP32-DIDO-xxxx`，連上後開 `http://192.168.4.1/`。
5. 網頁預設帳密 `admin` / `admin`，請於「使用者設定」更改。

設定檔與告警紀錄存在 SPIFFS：`/config.json`、`/alarms.json`。

> 編譯後的用量參考（core 3.3.10 / d1_mini32 / min_spiffs）：
> 程式 **1360319 bytes（69%）**、全域變數 **51336 bytes（15%）**。

### Upload Speed 請設 115200

本板用 **921600 會燒錄失敗**。握手階段是 115200 完成的（晶片型號、MAC 都讀得到），
一切換到 921600 就在讀 SPI flash 時斷線：

```
Changing baud rate to 921600...
Changed.
...
read_spiflash_sfdp → A fatal error occurred: The chip stopped responding.
```

改成 `115200` 即可穩定上傳（1.36MB 約需 2 分鐘）。`460800` 通常也可以，但 115200 最保險。

若 115200 仍失敗，依序檢查：換一條有資料線的 USB 線 → 燒錄時先拔掉繼電器模組 VCC
（背光加繼電器會吃電流）→ 手動進下載模式（按住 BOOT → 點一下 EN/RST → 放開 BOOT）。

> **硬體注意**：ST7789 的 `RES` 接在 **pin0**，而 GPIO0 是 ESP32 判斷是否進入下載模式的
> bootstrap 腳。目前可正常燒錄，但顯示器端若有下拉或電容，偶爾會讓燒錄時好時壞。
> 若日後常傳不進去，可將 RES 改接其他腳位（例如 pin27），並同步修改 `ST7789.h` 的
> `#define TFT_RST`。

---

## REST API

| 方法   | 路徑                        | 說明                              |
| :----- | :-------------------------- | :-------------------------------- |
| GET    | `/api/status`               | 系統資訊 + SPIFFS 容量            |
| GET    | `/api/fs`                   | SPIFFS 目錄列表                   |
| POST   | `/api/fs/delete`            | 刪除檔案（`path`）                |
| POST   | `/api/reboot`               | 重新啟動                          |
| GET    | `/api/wifi`                 | WiFi 連線資訊                     |
| POST   | `/api/wifi`                 | 設定 SSID / 密碼 / 主機名稱       |
| POST   | `/api/wifi/scan`            | 啟動掃描                          |
| GET    | `/api/wifi/scan`            | 取得掃描結果                      |
| POST   | `/api/wifi/clear`           | 清除連線設定並重開                |
| GET    | `/api/di`                   | DI 設定與即時狀態                 |
| POST   | `/api/di`                   | 儲存 DI 設定                      |
| GET    | `/api/alarms`               | 告警紀錄（最新 10 筆）            |
| POST   | `/api/alarms/clear`         | 清除告警紀錄                      |
| POST   | `/api/notify`               | 推播設定                          |
| POST   | `/api/notify/test`          | 送出測試推播                      |
| GET    | `/api/do`                   | DO 設定與狀態                     |
| POST   | `/api/do`                   | 儲存 DO 設定（模式／點動／排程）  |
| POST   | `/api/do/set`               | `state=on\|off\|toggle`           |
| POST   | `/api/do/pulse`             | 點動一次（`ms`）                  |
| GET    | `/api/mqtt`                 | MQTT 設定與連線狀態               |
| POST   | `/api/mqtt`                 | 儲存 MQTT 設定                    |
| POST   | `/api/mqtt/publish`         | 發佈訊息（`topic`、`msg`）        |
| GET    | `/api/mqtt/messages`        | 已收到的訂閱訊息                  |
| POST   | `/api/user`                 | 變更登入帳號密碼                  |
| POST   | `/api/ota?target=firmware`  | 上傳韌體（multipart）             |
| POST   | `/api/ota?target=spiffs`    | 上傳檔案系統映像                  |

MQTT 訂閱主題收到 `on` / `off` / `pulse` 可直接控制繼電器；
DI 告警會發佈到 `<pubTopic>/alarm`，DO 狀態發佈到 `<pubTopic>/do`。

### 已知限制

PubSubClient 發佈固定為 QoS0、訂閱最高支援 QoS1。網頁上仍可選到 QoS2（設定會存檔），
實際使用的 QoS 顯示在「訂閱 Subscriptions」說明中。

---

## ESP32 D1 mini 接線對照表

### DI

| DI  | ESP32 D1 mini |
| :-- | :------------ |
| IN  | pin32         |
| GND | GND           |
| IN  | pin33         |
| GND | GND           |

### I2C

| I2C | ESP32 D1 mini |
| :-- | :------------ |
| SDA | pin21(SDA)    |
| SCL | pin22(SCL)    |

### POWER

| POWER     | ESP32 D1 mini |
| :-------- | :------------ |
| USB DC 5V | VCC           |
| GND       | GND           |

### Relay

| Relay | ESP32 D1 mini |
| :---- | :------------ |
| IN    | pin4          |
| VCC   | 3.3V          |
| GND   | GND           |

### RS-485

| RS-485       | ESP32 D1 mini |
| :----------- | :------------ |
| DI(TX)       | pin17(TX)     |
| RO(RX)       | pin16(RX)     |
| DE、RE(短路) | pin14         |
| VCC          | 3.3V          |
| GND          | GND           |

### ST7789 OLED(SPI)

| ST7789 OLED(SPI) | ESP32 D1 mini |
| :--------------- | :------------ |
| GND              | GND           |
| VCC              | 3.3V          |
| SCL              | pin18(VSCLK)  |
| SDA              | pin23(VMOSI)  |
| RES              | pin0          |
| DC               | pin19(VMISO)  |
| CS               | pin5(VSS)     |
| BLK              | pin15         |

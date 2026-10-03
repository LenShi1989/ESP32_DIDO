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
| 顯示器        | ST7789 反相 / 色序 / 旋轉即時調整與測試圖（於系統狀態頁）                  |
| OTA 更新      | 網頁上傳韌體 `.bin` 或檔案系統 `spiffs.bin`                                |
| 使用者設定    | 設定登入帳號及密碼（HTTP Basic 驗證，**無預設帳密**）                      |

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
   `data/` 約 43KB，128KB 的 SPIFFS 分區夠用。
   Arduino IDE 1.8.x 若沒有這個選單，可改用下方的 PowerShell 腳本。
3. 燒錄韌體。
4. 裝置一律開啟 AP `ESP32-DIDO-xxxx`（`192.168.4.1`），**連上 STA 後也不關閉**。
   手機連上此 AP 會自動跳出設定頁（captive portal）。
   ESP32 只支援 **2.4GHz**，5GHz 的 SSID 不會出現在掃描清單中。
5. **本韌體不設預設帳密**：首次開啟網頁免登入，頁首會顯示提醒，
   請立即到「使用者設定」建立帳號密碼。設定後即啟用 HTTP Basic 驗證。

設定檔與告警紀錄存在 SPIFFS：`/config.json`、`/alarms.json`。

> 編譯後的用量參考（core 3.3.10 / d1_mini32 / min_spiffs）：
> 程式 **1367203 bytes（69%）**、全域變數 **51368 bytes（15%）**。

### 用 PowerShell 產生 / 燒錄 spiffs.bin

`tools\make-spiffs.ps1` 會自動找出 ESP32 core 的 `mkspiffs.exe`，並從分區表
讀出 SPIFFS 的位移與大小，不必手動填路徑：

```powershell
# 只產生 build\spiffs.bin，供網頁「OTA 更新 → 檔案系統」上傳
.\tools\make-spiffs.ps1

# 產生後直接用序列埠燒錄
.\tools\make-spiffs.ps1 -Port COM8

# 換分區配置時指定（需與 Arduino IDE 的 Partition Scheme 一致）
.\tools\make-spiffs.ps1 -Scheme default
```

輸出範例：

```
core      : 3.3.10
分區      : min_spiffs  offset 0x3D0000  size 131,072 bytes
來源      : data  43,059 bytes
已產生 build\spiffs.bin（131,072 bytes，使用率 32.9%）
```

打包前會先檢查 `data/` 是否塞得進分區，超過會直接中止。
手動執行的話等同於：

```powershell
mkspiffs.exe -c data -b 4096 -p 256 -s 0x20000 build\spiffs.bin
esptool.exe --chip esp32 --port COM8 --baud 115200 write_flash -z 0x3D0000 build\spiffs.bin
```

> `-s` 與 `write_flash` 的位移必須對應所選分區配置。`min_spiffs` 是
> size `0x20000`、offset `0x3D0000`；改用其他配置請查
> `<core>\tools\partitions\<scheme>.csv`。

---

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

> **ST7789 的 RES 接在 GPIO0**：`ST7789.cpp` 原本的重置條件寫成 `#if (TFT_RST > 0)`，
> 由於本板 `TFT_RST` 正好是 `0`，硬體重置會被整段跳過、只剩軟體重置，面板因此不會亮。
> 已修正為 `>= 0`（`-1` 才代表沒接 RST 腳），並在 `displayBegin()` 另外補一次
> 原始 sketch 驗證過的重置時序。

> **硬體注意**：ST7789 的 `RES` 接在 **pin0**，而 GPIO0 是 ESP32 判斷是否進入下載模式的
> bootstrap 腳。目前可正常燒錄，但顯示器端若有下拉或電容，偶爾會讓燒錄時好時壞。
> 若日後常傳不進去，可將 RES 改接其他腳位（例如 pin27），並同步修改 `ST7789.h` 的
> `#define TFT_RST`。

---

## 繼電器不動作時

「DO 設定」頁的繼電器卡片會顯示 **輸出腳位 / 實際準位 / 導通準位**，
按「自我測試」會繞過模式與排程邏輯，直接推腳位切換 4 次（約 2.4 秒），
Serial 同步輸出每次的寫入值與回讀值：

```
[do] ON   GPIO4 寫入 LOW  回讀 LOW   (activeLow=1)
[do] OFF  GPIO4 寫入 HIGH 回讀 HIGH  (activeLow=1)
```

| 現象 | 判斷 |
| :--- | :--- |
| 回讀值跟著變，聽得到咔噠聲 | 正常 |
| 回讀值跟著變，但繼電器沒動作 | 模組供電或接線問題（多數模組線圈需 **5V**，3.3V 吸不動） |
| 回讀值不變 | 腳位被佔用或設定錯誤 |
| 動作方向相反 | 在 DO 設定頁切換「輸出低電位導通繼電器」 |

---

## 顯示器調整

面板個體差異大（反相、色序、方向），這三項做成**網頁上可即時切換並存檔**的設定，
位置在「系統狀態 → 顯示器 (ST7789)」，調完立刻套用，不必重新燒錄。

| 設定 | 預設 | 什麼時候要改 |
| :--- | :--- | :----------- |
| 反相顯示 | 開 | 黑白顛倒（背景應為黑卻是白）時關閉 |
| 色序 BGR | 開 | 紅藍顛倒時改為 RGB |
| 旋轉 | 0° | 方向不對或內容被裁切時依序試 |
| SPI 時脈 | 40 MHz | 畫面出現像素雜訊、橫向撕裂時往下調（27 / 20 / 10） |
| 背光腳位 | 不驅動 | 背光不亮時才改為輸出 HIGH 或 LOW |

按「顯示測試圖」會畫出校正圖並保留 15 秒：

- 左上角白色方塊＝原點 `(0,0)`，右下角黃色方塊＝邊界，看不到代表方向或位移不對
- 色塊由左至右應為 **紅 綠 藍**，顛倒則切換色序
- 灰階左端應為黑、右端為白，相反則切換反相

> ZJY 1.54" IPS 240×240 模組實測需要**開啟反相**，這也是目前的預設值。
> `ST7789.cpp` 的初始化送的是 `INVOFF`，IPS 面板因此會黑白顛倒。

### 判斷雜訊來源

「顯示開機圖」走的是與**原始 sketch 完全相同**的單一 `pushImage` 路徑，
「顯示測試圖」與狀態畫面則是大量小筆的 `fillRect` / `drawString`。兩者比對即可定位：

| 開機圖 | 狀態畫面 | 判斷 |
| :----- | :------- | :--- |
| 乾淨 | 有雜訊 | 問題在繪圖方式或時序，不是接線 |
| 有雜訊 | 有雜訊 | SPI 訊號本身不穩：調低時脈、縮短杜邦線、改用排線 |

> **背光腳位預設不驅動**。原始 sketch 從未碰過 pin15，模組自身會讓背光恆亮。
> 若由 GPIO 直接推 LED，20~40mA 的電流會造成地彈，干擾同一排針上的 SPI 訊號。
> 只有在背光真的不亮時才需要改成輸出。

### ⚠️ TFT_MISO 必須是 -1

`ST7789.h` 原本在 D1 mini 區塊把 `TFT_MISO` 定義成 **19**，與 `TFT_DC` **同一支腳**：

```cpp
#define TFT_DC   19
#define TFT_MISO 19   // ← 撞腳
```

`init()` 會執行 `SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, -1)`，把 GPIO19 經 GPIO matrix
接成 SPI 的 MISO **輸入**，而 `DC_C` / `DC_D` 巨集又要把同一支腳當**輸出**推。
兩者互相干擾，DC 準位在傳輸途中被拉扯，命令與資料的分界就會錯亂，
症狀是**文字勉強看得出來、但整片佈滿像素雜訊與橫向撕裂**。

本模組的排針是 `GND VCC SCL SDA RES DC CS BLK`，**沒有 MISO**，
因此正確值是 `-1`（不使用 MISO）。已修正。

---

## WiFi 行為

- **AP 常開**：模式固定為 `WIFI_AP_STA`，連上路由器後 AP 仍然開啟，
  隨時可用 `192.168.4.1` 回到設定頁，不怕改錯設定連不上。
- **自動跳轉**：AP 內建 DNS 伺服器把所有網域解析到 `192.168.4.1`，
  手機／筆電連上 AP 後，系統的連線偵測被導向就會自動彈出設定頁。
  只有從 AP 介面進來的請求會被導向，從區域網路以 STA IP 連入不受影響。
- **設定不重開機**：儲存 SSID 後直接重連，AP 全程不中斷，
  網頁會持續輪詢並顯示路由器配發的 **DHCP IP**，點一下即可切換到新位址。
- **清除設定**同樣不重開機，AP 維持開啟可立即重新設定。

> **安全性**：AP 為開放網路且無密碼。若尚未在「使用者設定」建立帳號密碼，
> 附近任何人都能連上 AP 操作繼電器。部署到現場前請務必設定登入帳密。

ST7789 畫面上半部顯示 STA 的 SSID / DHCP IP / RSSI，第四行 `AP` 顯示
AP 的 IP 與目前連線的裝置數。

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
| POST   | `/api/do/selftest`          | 繼電器自我測試（直接切換腳位 4 次）|
| GET    | `/api/mqtt`                 | MQTT 設定與連線狀態               |
| POST   | `/api/mqtt`                 | 儲存 MQTT 設定                    |
| POST   | `/api/mqtt/publish`         | 發佈訊息（`topic`、`msg`）        |
| GET    | `/api/mqtt/messages`        | 已收到的訂閱訊息                  |
| POST   | `/api/user`                 | 變更登入帳號密碼                  |
| POST   | `/api/ota?target=firmware`  | 上傳韌體（multipart）             |
| POST   | `/api/ota?target=spiffs`    | 上傳檔案系統映像                  |

`POST /api/wifi/scan` 只負責啟動掃描並立即回應，再用 `GET /api/wifi/scan` 輪詢結果：

```jsonc
{ "scanning": true,  "elapsed": 3200, "failed": false, "list": [ /* 上次結果 */ ] }
{ "scanning": false, "elapsed": 0,    "failed": false, "list": [ { "ssid": "...", "rssi": -52,
                                                                  "ch": 6, "enc": "WPA2",
                                                                  "hidden": false } ] }
```

掃描中仍會回傳上一次的清單，畫面不會整個清空；`failed` 為 true 才代表真的掃描失敗。

MQTT 訂閱主題收到 `on` / `off` / `pulse` 可直接控制繼電器；
DI 告警會發佈到 `<pubTopic>/alarm`，DO 狀態發佈到 `<pubTopic>/do`。

### 路由註冊順序

ESPAsyncWebServer 預設的 URI 比對是 `BackwardCompatible`：

```cpp
(_value == path) || path.startsWith(_value + "/")
```

因此 `/api/wifi` 會連 `/api/wifi/scan` 一併吃掉，且由**先註冊者勝出**。
`setupRoutes()` 中同一前綴下必須把**路徑較深的排在前面**，否則子路由永遠不會被呼叫，
症狀是該 API 回傳了另一支 API 的內容（而不是 404），很難一眼看出。

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

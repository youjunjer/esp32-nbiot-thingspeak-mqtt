# ESP32 NB303 ThingSpeak MQTT 專案交接摘要

此文件用於在其他電腦重新啟動本專案。內容整理自本次開發與實測紀錄，不包含 ThingSpeak MQTT 密鑰。

## GitHub Repo

- Repo：`youjunjer/esp32-nbiot-thingspeak-mqtt`
- URL：`https://github.com/youjunjer/esp32-nbiot-thingspeak-mqtt`
- 專案類型：ESP32 Arduino / PlatformIO
- 主要程式：`src/esp32_nbiot_thingspeak_mqtt.ino`

## 硬體

- 開發板：ESP32
- NB-IoT 模組：NB303
- 感測器：DHT11
- DHT 程式庫：SimpleDHT
- USB 序列埠：實測為 `COM11`

## 接線

### NB303

| ESP32 | NB303 |
| --- | --- |
| GPIO17 TX2 | RX |
| GPIO16 RX2 | TX |
| GPIO15 | NB303 重開機控制腳 |
| GPIO33 | NB303 重開機控制腳 |
| GND | GND |

GPIO15 與 GPIO33 會在開機與重新註冊逾時時拉低 5 秒，再拉高，用來觸發 NB303 重開機。

### DHT11

| ESP32 | DHT11 |
| --- | --- |
| GPIO25 | DATA |
| 3V3 | VCC |
| GND | GND |

## 目前程式流程

1. ESP32 開機後先拉低 GPIO15 / GPIO33 5 秒，再拉高，觸發 NB303 重開機。
2. 使用 `ATI` 確認 NB303 正常回應。
3. `ATI` 成功後送出 `AT+SM=LOCK_FOREVER`，避免 NB303 進入睡眠。
4. 在 `loop()` 中持續以 `AT+CEREG?` 檢查 NB303 網路註冊。
5. `+CEREG` 回應包含 `,1` 或 `,5` 時，視為完成註冊。
6. 每次真正上傳前都會先確認已註冊。
7. 若 5 分鐘內未註冊，重新觸發 NB303 重開機。
8. 註冊成功後立即上傳第一筆資料，後續每 60 秒上傳一次。

## ThingSpeak MQTT 設定

正式 repo 不保存密鑰。使用前需在程式中填入：

```cpp
String thingSpeakChannelId = "YOUR_CHANNEL_ID";
String thingSpeakMqttClientId = "YOUR_MQTT_CLIENT_ID";
String thingSpeakMqttUsername = "YOUR_MQTT_USERNAME";
String thingSpeakMqttPassword = "YOUR_MQTT_PASSWORD";
```

ThingSpeak MQTT 設定：

- Domain：`mqtt3.thingspeak.com`
- Port：`1883`
- Topic：`channels/<channelID>/publish`
- Payload：`field1=<temperature>&field2=<humidity>&status=NB303`
- Field 1：溫度
- Field 2：濕度

## 重要實測結論

NB303 的 `AT+EMQNEW` 實測使用 broker IP 才能讓 ThingSpeak 收到資料。程式目前保留：

```cpp
String thingSpeakDomain = "mqtt3.thingspeak.com";
String thingSpeakHost = "34.194.89.194";
```

流程上仍會先送：

```text
AT+EDNS="mqtt3.thingspeak.com"
```

但 `AT+EMQNEW` 使用 `thingSpeakHost` 的 IP：

```text
AT+EMQNEW=34.194.89.194,1883,60000,1024
```

若 ThingSpeak DNS 解析結果改變，需用 NB303 的 `AT+EDNS="mqtt3.thingspeak.com"` 查目前 IP，再更新 `thingSpeakHost`。

## 已實測成功的序列結果

成功測試時序列監控重點如下：

```text
+CEREG: 0,1
[STEP CEREG] REGISTERED
[DHT11] T=28.0C H=50.0%
[STEP EDNS] OK
[STEP EMQNEW] OK
[STEP EMQCON] OK
[STEP EMQPUB] OK
[TASK] ThingSpeak publish done
```

`EMQPUB OK` 加上 ThingSpeak 頁面收到資料，才視為完整成功。

## 在新電腦重新開始

1. Clone repo：

```powershell
git clone https://github.com/youjunjer/esp32-nbiot-thingspeak-mqtt.git
cd esp32-nbiot-thingspeak-mqtt
```

2. 安裝 PlatformIO 或使用 VS Code PlatformIO。

3. 確認 `platformio.ini`：

```ini
upload_port = COM11
monitor_port = COM11
monitor_speed = 115200
```

若序列埠不同，請改成實際 ESP32 的 COM port。

4. 填入自己的 ThingSpeak MQTT 參數。

5. 編譯與燒錄：

```powershell
python -m platformio run -t upload
```

6. 開啟監控：

```powershell
python -m platformio device monitor
```

## 帶參數交付版

曾另外產生一份帶 ThingSpeak 參數的交付版 zip，放在 repo 外，不推上公開 GitHub：

```text
esp32-nbiot-thingspeak-mqtt-with-params.zip
```

該 zip 內含 MQTT 密碼，只能私下傳遞，不能提交到公開 repo。

## PZEM 後續方向

若後續加入 PZEM 電力量測模組，建議規劃 ThingSpeak 欄位如下：

- Field 1：溫度
- Field 2：濕度
- Field 3：電壓
- Field 4：電流
- Field 5：功率
- Field 6：累積電能
- Field 7：頻率
- Field 8：功率因數

NB303 已使用 ESP32 `Serial2`，PZEM 若使用 UART，建議改用另一組 `HardwareSerial`，避免與 NB303 UART 衝突。

PZEM-004T 用於市電量測，接線需注意高壓安全與 CT 方向。

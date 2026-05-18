# ESP32 NB303 ThingSpeak 溫溼度 MQTT

這是 ESP32 Arduino / PlatformIO 專案。ESP32 透過 NB303 NB-IoT 模組連線，讀取 DHT11 溫溼度，並用 MQTT 發送到 ThingSpeak。

## 硬體

- 開發板：ESP32 NodeMCU-32S
- USB 序列監控：`COM11 @ 115200`
- NB303 UART：ESP32 `Serial2 @ 115200 8N1`
- 溫溼度感測器：`DHT11`
- DHT 程式庫：`SimpleDHT`

## 接線

### ESP32 與 NB303

| ESP32 | NB303 |
| --- | --- |
| GPIO17 TX2 | RX |
| GPIO16 RX2 | TX |
| GPIO15 | 重啟/控制腳 |
| GPIO33 | 重啟/控制腳 |
| GND | GND |

`GPIO15` 和 `GPIO33` 會先拉低 5 秒再拉高，用來觸發 NB303 重開機。

### ESP32 與 DHT11

| ESP32 | DHT11 |
| --- | --- |
| GPIO25 | DATA |
| 3V3 | VCC |
| GND | GND |

## ThingSpeak MQTT 設定

主要設定在 [src/main.cpp](src/main.cpp) 頂端：

```cpp
static const char *kThingSpeakHost = "mqtt3.thingspeak.com";
static const char *kThingSpeakPort = "1883";
static const char *kThingSpeakChannelId = "2925903";
static const char *kThingSpeakMqttClientId = "...";
static const char *kThingSpeakMqttUsername = "...";
static const char *kThingSpeakMqttPassword = "...";
```

MQTT 發送目標：

- Host：`mqtt3.thingspeak.com`
- Port：`1883`
- Topic：`channels/2925903/publish`
- Payload：`field1=<temperature>&field2=<humidity>&status=NB303`
- `field1`：溫度
- `field2`：濕度

## 執行流程

1. ESP32 觸發 NB303 重開機：`GPIO15`、`GPIO33` 拉低 5 秒後拉高。
2. ESP32 送 `ATI` 確認 NB303 正常回應。
3. `ATI` 正常後送 `AT+SM=LOCK_FOREVER`，避免 NB303 進入睡眠。
4. 執行任務前先送 `AT+CEREG?` 檢查註冊狀態。
5. `+CEREG` 回覆包含 `,1` 或 `,5` 才視為網路註冊完成。
6. 註冊完成後立即送第 1 筆 ThingSpeak 資料。
7. 後續每 1 分鐘送 1 筆資料。
8. 如果重開機後 5 分鐘內仍未完成註冊，會再次觸發 NB303 重開機。

## 編譯

```powershell
python -m platformio run
```

## 上傳

```powershell
python -m platformio run -t upload
```

## 監控

```powershell
python -m platformio device monitor --port COM11 --baud 115200
```

## 目前注意事項

如果序列輸出出現：

```text
[DHT11] read failed
```

表示還沒成功讀到 DHT11，請先檢查 `GPIO25`、VCC、GND、DATA 接線，以及 DHT11 是否需要上拉電阻。

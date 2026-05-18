# ESP32 NB303 ThingSpeak DHT MQTT

ESP32 Arduino firmware that reads temperature and humidity from a DHT sensor and publishes them to ThingSpeak through NB303 MQTT AT commands.

## Hardware

- Board: ESP32 NodeMCU-32S
- USB serial monitor: `COM11 @ 115200`
- NB303 UART: ESP32 `Serial2 @ 115200 8N1`
- DHT sensor: `DHT11` on `GPIO25`, using `SimpleDHT`

## Wiring

| ESP32 | NB303 |
| --- | --- |
| GPIO17 TX2 | RX |
| GPIO16 RX2 | TX |
| GPIO15 | reboot/control input |
| GPIO33 | reboot/control input |
| GND | GND |

| ESP32 | DHT11 |
| --- | --- |
| GPIO25 | DATA |
| 3V3 | VCC |
| GND | GND |

## ThingSpeak MQTT

Set these constants at the top of `src/main.cpp`:

```cpp
static const char *kThingSpeakChannelId = "YOUR_CHANNEL_ID";
static const char *kThingSpeakMqttClientId = "YOUR_MQTT_CLIENT_ID";
static const char *kThingSpeakMqttUsername = "YOUR_MQTT_USERNAME";
static const char *kThingSpeakMqttPassword = "YOUR_MQTT_PASSWORD";
```

Publish target:

- Host: `mqtt3.thingspeak.com`
- Port: `1883`
- Topic: `channels/<channelID>/publish`
- Payload: `field1=<temperature>&field2=<humidity>&status=NB303`
- Publish interval: send one data point immediately after registration, then one data point per minute

## Upload

```powershell
python -m platformio run -d .\13_esp32_nb303_thingspeak_dht_mqtt -t upload
python -m platformio device monitor --port COM11 --baud 115200
```

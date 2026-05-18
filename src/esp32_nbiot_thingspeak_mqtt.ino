#include <Arduino.h>
#include <SimpleDHT.h>
#include <math.h>
#include <string.h>


int nb303RxPin = 16; // NB303 UART 接收腳，ESP32 RX2，連接 NB303 TX。
int nb303TxPin = 17; // NB303 UART 傳送腳，ESP32 TX2，連接 NB303 RX。
int nb303BootPin1 = 15; // NB303 重開機控制腳 1。
int nb303BootPin2 = 33; // NB303 重開機控制腳 2。
int dhtPin = 25; // DHT11 DATA 腳位。
int debugBaud = 115200; // USB 序列監控 baud rate。
int nb303Baud = 115200; // NB303 UART baud rate。
int atiIntervalMs = 10000; // ATI 失敗時的重試間隔，單位毫秒。
int ceregIntervalMs = 5000; // AT+CEREG? 註冊狀態檢查間隔，單位毫秒。
int registrationTimeoutMs = 300000; // NB303 註冊等待上限，單位毫秒。
int nb303BootLowMs = 5000; // NB303 重開機控制腳拉低時間，單位毫秒。
int publishIntervalMs = 60000; // ThingSpeak 發送間隔，單位毫秒。
String thingSpeakHost = "mqtt3.thingspeak.com"; // ThingSpeak MQTT broker 主機。
String thingSpeakPort = "1883"; // ThingSpeak MQTT broker port。
String mqttTimeout = "60000"; // NB303 MQTT 連線 timeout 參數。
String mqttBuffer = "1024"; // NB303 MQTT buffer 大小。
String thingSpeakChannelId = "YOUR_CHANNEL_ID"; // ThingSpeak Channel ID。
String thingSpeakMqttClientId = "YOUR_MQTT_CLIENT_ID"; // ThingSpeak MQTT Device Client ID。
String thingSpeakMqttUsername = "YOUR_MQTT_USERNAME"; // ThingSpeak MQTT Device Username。
String thingSpeakMqttPassword = "YOUR_MQTT_PASSWORD"; // ThingSpeak MQTT Device Password。
bool g_sleepLocked = false; // 記錄是否已送出 AT+SM=LOCK_FOREVER。
bool g_networkRegistered = false; // 記錄 NB303 是否已完成網路註冊。
bool g_mqttConnected = false; // 記錄 NB303 MQTT session 是否已連線。
bool g_publishImmediatelyAfterRegister = true; // 註冊成功後是否要立即送第一筆資料。
unsigned long g_lastAtiMs = 0; // 上一次送 ATI 的時間。
unsigned long g_lastCeregMs = 0; // 上一次檢查 AT+CEREG? 的時間。
unsigned long g_lastPublishMs = 0; // 上一次發送 ThingSpeak 資料的時間。
unsigned long g_registrationWindowStartMs = 0; // 本次 NB303 註冊等待起始時間。
char g_cmdBuf[160]; // USB 序列監控手動輸入 AT 指令的暫存區。
size_t g_cmdLen = 0; // 手動 AT 指令目前輸入長度。
char g_lastRxBuf[1400]; // NB303 最新回應內容暫存區。
SimpleDHT11 dht(dhtPin); // DHT11 感測器物件。

void setup()
{
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  Serial.begin(debugBaud);
  delay(300);
  Serial.println();
  Serial.println("=== esp32_nb303_thingspeak_dht_mqtt ===");
  Serial.println("NB303 reboot trigger: GPIO15/GPIO33 LOW 5s, then HIGH");

  rebootNb303();
  Serial2.begin(nb303Baud, SERIAL_8N1, nb303RxPin, nb303TxPin);

  Serial.println("NB303 UART: RX2=GPIO16 <- NB303 TX, TX2=GPIO17 -> NB303 RX, 115200 8N1");
  Serial.println("DHT: GPIO25, DHT11, SimpleDHT");
  Serial.println("ThingSpeak MQTT: mqtt3.thingspeak.com:1883");
  Serial.print("> ");

  runAtiAndSleepLock();
  g_lastAtiMs = millis();
  g_registrationWindowStartMs = millis();
}

void loop()
{
  handleUsbConsole();
  mirrorNb303Urc();

  unsigned long now = millis();
  if (!g_sleepLocked && (now - g_lastAtiMs) >= atiIntervalMs) {
    g_lastAtiMs = now;
    runAtiAndSleepLock();
  }

  bool duePublish = g_lastPublishMs == 0 || (now - g_lastPublishMs) >= publishIntervalMs;
  if (g_publishImmediatelyAfterRegister) {
    duePublish = true;
  }
  bool dueCereg = g_sleepLocked && (now - g_lastCeregMs) >= ceregIntervalMs;

  if (duePublish || dueCereg) {
    g_lastCeregMs = now;
    if (ensureNetworkRegisteredBeforeTask()) {
      if (duePublish) {
        runPublishTask();
        g_lastPublishMs = millis();
        g_publishImmediatelyAfterRegister = false;
      }
    }
  }

  blinkStatusLed();
}

// 檢查 ThingSpeak 設定是否仍為佔位文字，避免未設定時嘗試上傳資料。
bool hasPlaceholderConfig()
{
  return thingSpeakChannelId == "YOUR_CHANNEL_ID" ||
         thingSpeakMqttClientId == "YOUR_MQTT_CLIENT_ID" ||
         thingSpeakMqttUsername == "YOUR_MQTT_USERNAME" ||
         thingSpeakMqttPassword == "YOUR_MQTT_PASSWORD";
}

// 清除 NB303 UART 接收緩衝，避免上一筆回應影響下一筆 AT 指令判斷。
void clearNb303Rx()
{
  while (Serial2.available() > 0) {
    Serial2.read();
  }
}

// 等待 NB303 回應，並將收到的內容輸出到 USB 序列監控。
bool waitNb303Response(unsigned long timeoutMs)
{
  memset(g_lastRxBuf, 0, sizeof(g_lastRxBuf));
  size_t idx = 0;
  bool gotAny = false;
  unsigned long start = millis();

  Serial.println("[NB303 RX]");
  while ((millis() - start) < timeoutMs) {
    while (Serial2.available() > 0) {
      int c = Serial2.read();
      if (c < 0) continue;

      gotAny = true;
      Serial.write((char)c);
      if (idx < sizeof(g_lastRxBuf) - 1U) {
        g_lastRxBuf[idx++] = (char)c;
      }
    }

    if (strstr(g_lastRxBuf, "\r\nOK\r\n") != nullptr ||
        strstr(g_lastRxBuf, "\nOK\r\n") != nullptr ||
        strstr(g_lastRxBuf, "\r\nERROR\r\n") != nullptr ||
        strstr(g_lastRxBuf, "\nERROR\r\n") != nullptr) {
      break;
    }
    delay(2);
  }

  if (!gotAny) {
    Serial.println("(no response)");
  } else {
    Serial.println();
  }

  return strstr(g_lastRxBuf, "OK") != nullptr;
}

// 送出一筆 NB303 AT 指令，並等待 OK 或 ERROR 回應。
bool sendNb303Command(const char *cmd, unsigned long timeoutMs)
{
  clearNb303Rx();
  Serial.print("[TX] ");
  Serial.println(cmd);
  Serial2.print(cmd);
  Serial2.print("\r\n");
  return waitNb303Response(timeoutMs);
}

// 判斷 AT+CEREG? 回應是否代表已完成 NB-IoT 網路註冊。
bool isNetworkRegistered()
{
  const char *p = strstr(g_lastRxBuf, "+CEREG:");
  if (p == nullptr) return false;
  return strstr(p, ",1") != nullptr || strstr(p, ",5") != nullptr;
}

// 查詢 NB303 網路註冊狀態，並更新目前註冊旗標。
bool checkNetworkRegistration()
{
  bool cmdOk = sendNb303Command("AT+CEREG?", 3000);
  bool registered = cmdOk && isNetworkRegistered();

  if (registered) {
    Serial.println(g_networkRegistered ? "[STEP CEREG] STILL REGISTERED" : "[STEP CEREG] REGISTERED");
  } else {
    Serial.println("[STEP CEREG] NOT REGISTERED");
  }

  if (registered && !g_networkRegistered) {
    g_publishImmediatelyAfterRegister = true;
  }
  g_networkRegistered = registered;
  return registered;
}

// 將 GPIO15 與 GPIO33 拉低 5 秒後拉高，用來觸發 NB303 重開機。
void rebootNb303()
{
  Serial.println("[PWR] NB303 reboot trigger: GPIO15/GPIO33 LOW for 5s");
  pinMode(nb303BootPin1, OUTPUT);
  pinMode(nb303BootPin2, OUTPUT);
  digitalWrite(nb303BootPin1, LOW);
  digitalWrite(nb303BootPin2, LOW);
  delay(nb303BootLowMs);

  Serial.println("[PWR] GPIO15/GPIO33 HIGH");
  digitalWrite(nb303BootPin1, HIGH);
  digitalWrite(nb303BootPin2, HIGH);
  delay(1000);
}

// 送出 ATI 確認 NB303 可回應，成功後送出 AT+SM=LOCK_FOREVER 關閉睡眠。
void runAtiAndSleepLock()
{
  bool atiOk = sendNb303Command("ATI", 3000);
  Serial.println(atiOk ? "[STEP ATI] OK" : "[STEP ATI] FAIL");

  if (atiOk && !g_sleepLocked) {
    bool lockOk = sendNb303Command("AT+SM=LOCK_FOREVER", 3000);
    Serial.println(lockOk ? "[STEP SM] LOCK_FOREVER OK" : "[STEP SM] LOCK_FOREVER FAIL");
    g_sleepLocked = lockOk;
  }
}

// 重啟 NB303 流程，並重新進行 ATI 與關閉睡眠設定。
void restartNb303Flow()
{
  g_sleepLocked = false;
  g_networkRegistered = false;
  g_mqttConnected = false;
  g_publishImmediatelyAfterRegister = true;
  g_lastPublishMs = 0;
  Serial2.end();
  rebootNb303();
  Serial2.begin(nb303Baud, SERIAL_8N1, nb303RxPin, nb303TxPin);
  delay(300);
  runAtiAndSleepLock();
  g_lastAtiMs = millis();
  g_lastCeregMs = 0;
  g_registrationWindowStartMs = millis();
}

// 任務執行前確認 NB303 已註冊網路，若 5 分鐘內未註冊則重啟 NB303。
bool ensureNetworkRegisteredBeforeTask()
{
  if (!g_sleepLocked) {
    runAtiAndSleepLock();
    g_lastAtiMs = millis();
  }

  if (!g_sleepLocked) return false;
  if (checkNetworkRegistration()) return true;

  unsigned long now = millis();
  unsigned long elapsed = now - g_registrationWindowStartMs;
  if (elapsed >= registrationTimeoutMs) {
    Serial.println("[RECOVER] Not registered for 300s, reboot NB303 before task");
    restartNb303Flow();
  } else {
    unsigned long remainSec = (registrationTimeoutMs - elapsed + 999) / 1000;
    Serial.print("[WAIT] Registration pending, retry before reboot in ");
    Serial.print(remainSec);
    Serial.println("s");
  }

  return false;
}

// 將 MQTT payload 轉成 NB303 EMQPUB 指令需要的十六進位字串。
void bytesToHex(const char *src, size_t len, char *hexOut, size_t hexOutSize)
{
  char table[] = "0123456789abcdef";
  size_t out = 0;
  for (size_t i = 0; i < len; i++) {
    if ((out + 2) >= hexOutSize) break;
    unsigned char b = (unsigned char)src[i];
    hexOut[out++] = table[(b >> 4) & 0x0F];
    hexOut[out++] = table[b & 0x0F];
  }
  hexOut[out] = '\0';
}

// 建立 NB303 MQTT session，並連線到 ThingSpeak MQTT broker。
bool mqttConnect()
{
  if (hasPlaceholderConfig()) {
    Serial.println("[CONFIG] Fill ThingSpeak MQTT constants before publishing");
    return false;
  }

  char cmd[360];

  snprintf(cmd, sizeof(cmd), "AT+EDNS=\"%s\"", thingSpeakHost.c_str());
  if (!sendNb303Command(cmd, 8000)) {
    Serial.println("[STEP EDNS] FAIL");
    return false;
  }
  Serial.println("[STEP EDNS] OK");

  snprintf(cmd, sizeof(cmd), "AT+EMQNEW=%s,%s,%s,%s",
           thingSpeakHost.c_str(), thingSpeakPort.c_str(), mqttTimeout.c_str(), mqttBuffer.c_str());
  if (!sendNb303Command(cmd, 8000)) {
    Serial.println("[STEP EMQNEW] FAIL");
    return false;
  }
  Serial.println("[STEP EMQNEW] OK");

  snprintf(cmd, sizeof(cmd), "AT+EMQCON=0,3.1,\"%s\",60000,1,0,\"%s\",\"%s\"",
           thingSpeakMqttClientId.c_str(), thingSpeakMqttUsername.c_str(), thingSpeakMqttPassword.c_str());
  if (!sendNb303Command(cmd, 10000)) {
    Serial.println("[STEP EMQCON] FAIL");
    return false;
  }

  Serial.println("[STEP EMQCON] OK");
  g_mqttConnected = true;
  return true;
}

// 讀取 DHT11 溫度與濕度，讀取失敗時輸出錯誤代碼。
bool readDht(float &temperature, float &humidity)
{
  int err = dht.read2(&temperature, &humidity, nullptr);
  if (err != SimpleDHTErrSuccess || isnan(temperature) || isnan(humidity)) {
    Serial.print("[DHT11] read failed, err=");
    Serial.println(err);
    return false;
  }

  Serial.print("[DHT11] T=");
  Serial.print(temperature, 1);
  Serial.print("C H=");
  Serial.print(humidity, 1);
  Serial.println("%");
  return true;
}

// 將溫度與濕度組成 ThingSpeak payload，透過 NB303 MQTT 發送。
bool publishThingSpeak(float temperature, float humidity)
{
  if (!g_mqttConnected && !mqttConnect()) {
    return false;
  }

  char topic[80];
  snprintf(topic, sizeof(topic), "channels/%s/publish", thingSpeakChannelId.c_str());

  char payload[120];
  snprintf(payload, sizeof(payload), "field1=%.1f&field2=%.1f&status=NB303", temperature, humidity);

  char hexPayload[260];
  bytesToHex(payload, strlen(payload), hexPayload, sizeof(hexPayload));

  char cmd[420];
  snprintf(cmd, sizeof(cmd), "AT+EMQPUB=0,%s,0,0,0,%d,%s", topic, (int)strlen(hexPayload), hexPayload);
  bool ok = sendNb303Command(cmd, 10000);
  Serial.println(ok ? "[STEP EMQPUB] OK" : "[STEP EMQPUB] FAIL");

  if (!ok) {
    g_mqttConnected = false;
  }
  return ok;
}

// 執行一次資料上傳任務，包含讀取 DHT11 與發送 ThingSpeak MQTT。
void runPublishTask()
{
  float temperature = 0;
  float humidity = 0;
  if (!readDht(temperature, humidity)) return;

  if (!publishThingSpeak(temperature, humidity)) {
    Serial.println("[TASK] ThingSpeak publish failed");
    return;
  }

  Serial.println("[TASK] ThingSpeak publish done");
}

// 讓 USB 序列監控可手動輸入 AT 指令，方便直接測試 NB303。
void handleUsbConsole()
{
  while (Serial.available() > 0) {
    int c = Serial.read();
    if (c < 0) continue;

    if (c == '\r' || c == '\n') {
      if (g_cmdLen > 0) {
        g_cmdBuf[g_cmdLen] = '\0';
        sendNb303Command(g_cmdBuf, 3000);
        g_cmdLen = 0;
        Serial.print("> ");
      }
      continue;
    }

    if ((c == 0x08 || c == 0x7F) && g_cmdLen > 0) {
      g_cmdLen--;
      Serial.print("\b \b");
      continue;
    }

    if (g_cmdLen < sizeof(g_cmdBuf) - 1U) {
      g_cmdBuf[g_cmdLen++] = (char)c;
      Serial.write((char)c);
    }
  }
}

// 將 NB303 主動輸出的 URC 訊息同步顯示到 USB 序列監控。
void mirrorNb303Urc()
{
  while (Serial2.available() > 0) {
    int c = Serial2.read();
    if (c >= 0) Serial.write((char)c);
  }
}

// 讓內建 LED 閃爍，表示 ESP32 主程式仍在執行。
void blinkStatusLed()
{
  unsigned long now = millis();
  static unsigned long lastBlinkMs = 0;
  if ((now - lastBlinkMs) >= 500) {
    lastBlinkMs = now;
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
  }
}

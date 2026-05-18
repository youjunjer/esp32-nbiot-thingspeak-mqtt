#include <Arduino.h>
#include <SimpleDHT.h>
#include <math.h>
#include <string.h>

#define DBG_PORT Serial
#define NB303_PORT Serial2

// NB303 使用 ESP32 的 Serial2，GPIO16/17 分別接 NB303 TX/RX。
int nb303RxPin = 16; // ESP32 RX2, connect to NB303 TX
int nb303TxPin = 17; // ESP32 TX2, connect to NB303 RX

// GPIO15 與 GPIO33 同時拉低 5 秒再拉高，用來觸發 NB303 重開機。
int nb303BootPin1 = 15;
int nb303BootPin2 = 33;

// DHT11 DATA 腳接 GPIO25，使用 SimpleDHT 程式庫讀取溫溼度。
int dhtPin = 25;

int debugBaud = 115200;
int nb303Baud = 115200;
int atiIntervalMs = 10000;
int ceregIntervalMs = 5000;

// NB303 重開機後最多等待 5 分鐘完成註冊，逾時才再次重開機。
int registrationTimeoutMs = 300000;
int nb303BootLowMs = 5000;
int publishIntervalMs = 60000; // 1 data point per minute

// ThingSpeak MQTT broker 與發送參數。
String thingSpeakHost = "mqtt3.thingspeak.com";
String thingSpeakPort = "1883";
String mqttTimeout = "60000";
String mqttBuffer = "1024";

// ThingSpeak MQTT Device credentials 與 Channel ID。
String thingSpeakChannelId = "2925903";
String thingSpeakMqttClientId = "YOUR_MQTT_CLIENT_ID";
String thingSpeakMqttUsername = "YOUR_MQTT_USERNAME";
String thingSpeakMqttPassword = "YOUR_MQTT_PASSWORD";

bool g_sleepLocked = false;
bool g_networkRegistered = false;
bool g_mqttConnected = false;
bool g_publishImmediatelyAfterRegister = true;
unsigned long g_lastAtiMs = 0;
unsigned long g_lastCeregMs = 0;
unsigned long g_lastPublishMs = 0;
unsigned long g_registrationWindowStartMs = 0;
char g_cmdBuf[160];
size_t g_cmdLen = 0;
char g_lastRxBuf[1400];
SimpleDHT11 dht(dhtPin);

// 若仍保留預設占位字串，就不要嘗試發布，避免送到錯誤的 MQTT topic。
static bool hasPlaceholderConfig()
{
  return thingSpeakChannelId == "YOUR_CHANNEL_ID" ||
         thingSpeakMqttClientId == "YOUR_MQTT_CLIENT_ID" ||
         thingSpeakMqttUsername == "YOUR_MQTT_USERNAME" ||
         thingSpeakMqttPassword == "YOUR_MQTT_PASSWORD";
}

static void clearNb303Rx()
{
  while (NB303_PORT.available() > 0) {
    NB303_PORT.read();
  }
}

static bool waitNb303Response(unsigned long timeoutMs)
{
  memset(g_lastRxBuf, 0, sizeof(g_lastRxBuf));
  size_t idx = 0;
  bool gotAny = false;
  unsigned long start = millis();

  DBG_PORT.println("[NB303 RX]");
  while ((millis() - start) < timeoutMs) {
    while (NB303_PORT.available() > 0) {
      int c = NB303_PORT.read();
      if (c < 0) continue;

      gotAny = true;
      DBG_PORT.write((char)c);
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
    DBG_PORT.println("(no response)");
  } else {
    DBG_PORT.println();
  }

  return strstr(g_lastRxBuf, "OK") != nullptr;
}

// 送出一行 NB303 AT 指令，並等待 OK/ERROR 或逾時。
static bool sendNb303Command(const char *cmd, unsigned long timeoutMs)
{
  clearNb303Rx();
  DBG_PORT.print("[TX] ");
  DBG_PORT.println(cmd);
  NB303_PORT.print(cmd);
  NB303_PORT.print("\r\n");
  return waitNb303Response(timeoutMs);
}

// NB-IoT 註冊狀態：,1 表示 home network，,5 表示 roaming，兩者都可視為已註冊。
static bool isNetworkRegistered()
{
  const char *p = strstr(g_lastRxBuf, "+CEREG:");
  if (p == nullptr) return false;
  return strstr(p, ",1") != nullptr || strstr(p, ",5") != nullptr;
}

// 執行任務前都先檢查 AT+CEREG?，確認 NB303 已完成網路註冊。
static bool checkNetworkRegistration()
{
  bool cmdOk = sendNb303Command("AT+CEREG?", 3000);
  bool registered = cmdOk && isNetworkRegistered();

  if (registered) {
    DBG_PORT.println(g_networkRegistered ? "[STEP CEREG] STILL REGISTERED" : "[STEP CEREG] REGISTERED");
  } else {
    DBG_PORT.println("[STEP CEREG] NOT REGISTERED");
  }

  if (registered && !g_networkRegistered) {
    g_publishImmediatelyAfterRegister = true;
  }
  g_networkRegistered = registered;
  return registered;
}

// 觸發 NB303 重開機：GPIO15/GPIO33 拉低 5 秒，再拉高。
static void rebootNb303()
{
  DBG_PORT.println("[PWR] NB303 reboot trigger: GPIO15/GPIO33 LOW for 5s");
  pinMode(nb303BootPin1, OUTPUT);
  pinMode(nb303BootPin2, OUTPUT);
  digitalWrite(nb303BootPin1, LOW);
  digitalWrite(nb303BootPin2, LOW);
  delay(nb303BootLowMs);

  DBG_PORT.println("[PWR] GPIO15/GPIO33 HIGH");
  digitalWrite(nb303BootPin1, HIGH);
  digitalWrite(nb303BootPin2, HIGH);
  delay(1000);
}

// NB303 可回應 ATI 後，立即鎖定睡眠，避免模組進入 sleep。
static void runAtiAndSleepLock()
{
  bool atiOk = sendNb303Command("ATI", 3000);
  DBG_PORT.println(atiOk ? "[STEP ATI] OK" : "[STEP ATI] FAIL");

  if (atiOk && !g_sleepLocked) {
    bool lockOk = sendNb303Command("AT+SM=LOCK_FOREVER", 3000);
    DBG_PORT.println(lockOk ? "[STEP SM] LOCK_FOREVER OK" : "[STEP SM] LOCK_FOREVER FAIL");
    g_sleepLocked = lockOk;
  }
}

// 註冊逾時或需要恢復時，重新啟動 NB303 並重跑 ATI / LOCK_FOREVER。
static void restartNb303Flow()
{
  g_sleepLocked = false;
  g_networkRegistered = false;
  g_mqttConnected = false;
  g_publishImmediatelyAfterRegister = true;
  g_lastPublishMs = 0;
  NB303_PORT.end();
  rebootNb303();
  NB303_PORT.begin(nb303Baud, SERIAL_8N1, nb303RxPin, nb303TxPin);
  delay(300);
  runAtiAndSleepLock();
  g_lastAtiMs = millis();
  g_lastCeregMs = 0;
  g_registrationWindowStartMs = millis();
}

// 任務真正執行前的保護流程：未註冊就等待，超過 5 分鐘才重啟 NB303。
static bool ensureNetworkRegisteredBeforeTask()
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
    DBG_PORT.println("[RECOVER] Not registered for 300s, reboot NB303 before task");
    restartNb303Flow();
  } else {
    unsigned long remainSec = (registrationTimeoutMs - elapsed + 999) / 1000;
    DBG_PORT.print("[WAIT] Registration pending, retry before reboot in ");
    DBG_PORT.print(remainSec);
    DBG_PORT.println("s");
  }

  return false;
}

// NB303 的 EMQPUB 需要十六進位 payload，所以先把 ThingSpeak payload 轉成 hex。
static void bytesToHex(const char *src, size_t len, char *hexOut, size_t hexOutSize)
{
  static const char table[] = "0123456789abcdef";
  size_t out = 0;
  for (size_t i = 0; i < len; i++) {
    if ((out + 2) >= hexOutSize) break;
    uint8_t b = (uint8_t)src[i];
    hexOut[out++] = table[(b >> 4) & 0x0F];
    hexOut[out++] = table[b & 0x0F];
  }
  hexOut[out] = '\0';
}

// 建立 NB303 內建 MQTT session，連到 ThingSpeak MQTT broker。
static bool mqttConnect()
{
  if (hasPlaceholderConfig()) {
    DBG_PORT.println("[CONFIG] Fill ThingSpeak MQTT constants before publishing");
    return false;
  }

  char cmd[360];

  snprintf(cmd, sizeof(cmd), "AT+EDNS=\"%s\"", thingSpeakHost.c_str());
  if (!sendNb303Command(cmd, 8000)) {
    DBG_PORT.println("[STEP EDNS] FAIL");
    return false;
  }
  DBG_PORT.println("[STEP EDNS] OK");

  snprintf(cmd, sizeof(cmd), "AT+EMQNEW=%s,%s,%s,%s",
           thingSpeakHost.c_str(), thingSpeakPort.c_str(), mqttTimeout.c_str(), mqttBuffer.c_str());
  if (!sendNb303Command(cmd, 8000)) {
    DBG_PORT.println("[STEP EMQNEW] FAIL");
    return false;
  }
  DBG_PORT.println("[STEP EMQNEW] OK");

  snprintf(cmd, sizeof(cmd), "AT+EMQCON=0,3.1,\"%s\",60000,1,0,\"%s\",\"%s\"",
           thingSpeakMqttClientId.c_str(), thingSpeakMqttUsername.c_str(), thingSpeakMqttPassword.c_str());
  if (!sendNb303Command(cmd, 10000)) {
    DBG_PORT.println("[STEP EMQCON] FAIL");
    return false;
  }

  DBG_PORT.println("[STEP EMQCON] OK");
  g_mqttConnected = true;
  return true;
}

// 讀取 DHT11 溫溼度，成功後回傳攝氏溫度與相對濕度。
static bool readDht(float &temperature, float &humidity)
{
  int err = dht.read2(&temperature, &humidity, nullptr);
  if (err != SimpleDHTErrSuccess || isnan(temperature) || isnan(humidity)) {
    DBG_PORT.print("[DHT11] read failed, err=");
    DBG_PORT.println(err);
    return false;
  }

  DBG_PORT.print("[DHT11] T=");
  DBG_PORT.print(temperature, 1);
  DBG_PORT.print("C H=");
  DBG_PORT.print(humidity, 1);
  DBG_PORT.println("%");
  return true;
}

// 將溫溼度送到 ThingSpeak：field1=溫度，field2=濕度。
static bool publishThingSpeak(float temperature, float humidity)
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
  DBG_PORT.println(ok ? "[STEP EMQPUB] OK" : "[STEP EMQPUB] FAIL");

  if (!ok) {
    g_mqttConnected = false;
  }
  return ok;
}

// 單次資料任務：讀 DHT11，然後透過 NB303 MQTT 發送到 ThingSpeak。
static void runPublishTask()
{
  float temperature = 0;
  float humidity = 0;
  if (!readDht(temperature, humidity)) return;

  if (!publishThingSpeak(temperature, humidity)) {
    DBG_PORT.println("[TASK] ThingSpeak publish failed");
    return;
  }

  DBG_PORT.println("[TASK] ThingSpeak publish done");
}

// USB 序列監控可手動輸入 AT 指令，方便現場除錯 NB303。
static void handleUsbConsole()
{
  while (DBG_PORT.available() > 0) {
    int c = DBG_PORT.read();
    if (c < 0) continue;

    if (c == '\r' || c == '\n') {
      if (g_cmdLen > 0) {
        g_cmdBuf[g_cmdLen] = '\0';
        sendNb303Command(g_cmdBuf, 3000);
        g_cmdLen = 0;
        DBG_PORT.print("> ");
      }
      continue;
    }

    if ((c == 0x08 || c == 0x7F) && g_cmdLen > 0) {
      g_cmdLen--;
      DBG_PORT.print("\b \b");
      continue;
    }

    if (g_cmdLen < sizeof(g_cmdBuf) - 1U) {
      g_cmdBuf[g_cmdLen++] = (char)c;
      DBG_PORT.write((char)c);
    }
  }
}

// 印出 NB303 主動回報，例如 +IP 或其他 URC。
static void mirrorNb303Urc()
{
  while (NB303_PORT.available() > 0) {
    int c = NB303_PORT.read();
    if (c >= 0) DBG_PORT.write((char)c);
  }
}

void setup()
{
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  DBG_PORT.begin(debugBaud);
  delay(300);
  DBG_PORT.println();
  DBG_PORT.println("=== 13_esp32_nb303_thingspeak_dht_mqtt ===");
  DBG_PORT.println("NB303 reboot trigger: GPIO15/GPIO33 LOW 5s, then HIGH");
  rebootNb303();

  NB303_PORT.begin(nb303Baud, SERIAL_8N1, nb303RxPin, nb303TxPin);

  DBG_PORT.println("NB303 UART: RX2=GPIO16 <- NB303 TX, TX2=GPIO17 -> NB303 RX, 115200 8N1");
  DBG_PORT.println("DHT: GPIO25, DHT11, SimpleDHT");
  DBG_PORT.println("ThingSpeak MQTT: mqtt3.thingspeak.com:1883");
  DBG_PORT.print("> ");

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

  // 註冊成功後立即送第一筆，之後每 1 分鐘送 1 筆。
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

  static unsigned long lastBlinkMs = 0;
  if ((now - lastBlinkMs) >= 500) {
    lastBlinkMs = now;
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
  }
}

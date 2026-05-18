#include <Arduino.h>
#include <SimpleDHT.h>
#include <math.h>
#include <string.h>

#define DBG_PORT Serial
#define NB303_PORT Serial2

static constexpr int kNb303RxPin = 16; // ESP32 RX2, connect to NB303 TX
static constexpr int kNb303TxPin = 17; // ESP32 TX2, connect to NB303 RX
static constexpr int kNb303BootPin1 = 15;
static constexpr int kNb303BootPin2 = 33;
static constexpr int kDhtPin = 25;

static constexpr uint32_t kDebugBaud = 115200;
static constexpr uint32_t kNb303Baud = 115200;
static constexpr uint32_t kAtiIntervalMs = 10000;
static constexpr uint32_t kCeregIntervalMs = 5000;
static constexpr uint32_t kRegistrationTimeoutMs = 300000;
static constexpr uint32_t kNb303BootLowMs = 5000;
static constexpr uint32_t kPublishIntervalMs = 60UL * 1000UL; // 1 data point per minute

static const char *kThingSpeakHost = "mqtt3.thingspeak.com";
static const char *kThingSpeakPort = "1883";
static const char *kMqttTimeout = "60000";
static const char *kMqttBuffer = "1024";

// Fill these with the MQTT device credentials from ThingSpeak.
static const char *kThingSpeakChannelId = "2925903";
static const char *kThingSpeakMqttClientId = "YOUR_MQTT_CLIENT_ID";
static const char *kThingSpeakMqttUsername = "YOUR_MQTT_CLIENT_ID";
static const char *kThingSpeakMqttPassword = "YOUR_MQTT_PASSWORD";

static bool g_sleepLocked = false;
static bool g_networkRegistered = false;
static bool g_mqttConnected = false;
static bool g_publishImmediatelyAfterRegister = true;
static uint32_t g_lastAtiMs = 0;
static uint32_t g_lastCeregMs = 0;
static uint32_t g_lastPublishMs = 0;
static uint32_t g_registrationWindowStartMs = 0;
static char g_cmdBuf[160];
static size_t g_cmdLen = 0;
static char g_lastRxBuf[1400];
static SimpleDHT11 dht(kDhtPin);

static bool hasPlaceholderConfig()
{
  return strcmp(kThingSpeakChannelId, "YOUR_CHANNEL_ID") == 0 ||
         strcmp(kThingSpeakMqttClientId, "YOUR_MQTT_CLIENT_ID") == 0 ||
         strcmp(kThingSpeakMqttUsername, "YOUR_MQTT_USERNAME") == 0 ||
         strcmp(kThingSpeakMqttPassword, "YOUR_MQTT_PASSWORD") == 0;
}

static void clearNb303Rx()
{
  while (NB303_PORT.available() > 0) {
    NB303_PORT.read();
  }
}

static bool waitNb303Response(uint32_t timeoutMs)
{
  memset(g_lastRxBuf, 0, sizeof(g_lastRxBuf));
  size_t idx = 0;
  bool gotAny = false;
  uint32_t start = millis();

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

static bool sendNb303Command(const char *cmd, uint32_t timeoutMs)
{
  clearNb303Rx();
  DBG_PORT.print("[TX] ");
  DBG_PORT.println(cmd);
  NB303_PORT.print(cmd);
  NB303_PORT.print("\r\n");
  return waitNb303Response(timeoutMs);
}

static bool isNetworkRegistered()
{
  const char *p = strstr(g_lastRxBuf, "+CEREG:");
  if (p == nullptr) return false;
  return strstr(p, ",1") != nullptr || strstr(p, ",5") != nullptr;
}

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

static void rebootNb303()
{
  DBG_PORT.println("[PWR] NB303 reboot trigger: GPIO15/GPIO33 LOW for 5s");
  pinMode(kNb303BootPin1, OUTPUT);
  pinMode(kNb303BootPin2, OUTPUT);
  digitalWrite(kNb303BootPin1, LOW);
  digitalWrite(kNb303BootPin2, LOW);
  delay(kNb303BootLowMs);

  DBG_PORT.println("[PWR] GPIO15/GPIO33 HIGH");
  digitalWrite(kNb303BootPin1, HIGH);
  digitalWrite(kNb303BootPin2, HIGH);
  delay(1000);
}

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

static void restartNb303Flow()
{
  g_sleepLocked = false;
  g_networkRegistered = false;
  g_mqttConnected = false;
  g_publishImmediatelyAfterRegister = true;
  g_lastPublishMs = 0;
  NB303_PORT.end();
  rebootNb303();
  NB303_PORT.begin(kNb303Baud, SERIAL_8N1, kNb303RxPin, kNb303TxPin);
  delay(300);
  runAtiAndSleepLock();
  g_lastAtiMs = millis();
  g_lastCeregMs = 0;
  g_registrationWindowStartMs = millis();
}

static bool ensureNetworkRegisteredBeforeTask()
{
  if (!g_sleepLocked) {
    runAtiAndSleepLock();
    g_lastAtiMs = millis();
  }

  if (!g_sleepLocked) return false;
  if (checkNetworkRegistration()) return true;

  uint32_t now = millis();
  uint32_t elapsed = now - g_registrationWindowStartMs;
  if (elapsed >= kRegistrationTimeoutMs) {
    DBG_PORT.println("[RECOVER] Not registered for 300s, reboot NB303 before task");
    restartNb303Flow();
  } else {
    uint32_t remainSec = (kRegistrationTimeoutMs - elapsed + 999) / 1000;
    DBG_PORT.print("[WAIT] Registration pending, retry before reboot in ");
    DBG_PORT.print(remainSec);
    DBG_PORT.println("s");
  }

  return false;
}

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

static bool mqttConnect()
{
  if (hasPlaceholderConfig()) {
    DBG_PORT.println("[CONFIG] Fill ThingSpeak MQTT constants before publishing");
    return false;
  }

  char cmd[360];

  snprintf(cmd, sizeof(cmd), "AT+EDNS=\"%s\"", kThingSpeakHost);
  if (!sendNb303Command(cmd, 8000)) {
    DBG_PORT.println("[STEP EDNS] FAIL");
    return false;
  }
  DBG_PORT.println("[STEP EDNS] OK");

  snprintf(cmd, sizeof(cmd), "AT+EMQNEW=%s,%s,%s,%s", kThingSpeakHost, kThingSpeakPort, kMqttTimeout, kMqttBuffer);
  if (!sendNb303Command(cmd, 8000)) {
    DBG_PORT.println("[STEP EMQNEW] FAIL");
    return false;
  }
  DBG_PORT.println("[STEP EMQNEW] OK");

  snprintf(cmd, sizeof(cmd), "AT+EMQCON=0,3.1,\"%s\",60000,1,0,\"%s\",\"%s\"",
           kThingSpeakMqttClientId, kThingSpeakMqttUsername, kThingSpeakMqttPassword);
  if (!sendNb303Command(cmd, 10000)) {
    DBG_PORT.println("[STEP EMQCON] FAIL");
    return false;
  }

  DBG_PORT.println("[STEP EMQCON] OK");
  g_mqttConnected = true;
  return true;
}

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

static bool publishThingSpeak(float temperature, float humidity)
{
  if (!g_mqttConnected && !mqttConnect()) {
    return false;
  }

  char topic[80];
  snprintf(topic, sizeof(topic), "channels/%s/publish", kThingSpeakChannelId);

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

  DBG_PORT.begin(kDebugBaud);
  delay(300);
  DBG_PORT.println();
  DBG_PORT.println("=== 13_esp32_nb303_thingspeak_dht_mqtt ===");
  DBG_PORT.println("NB303 reboot trigger: GPIO15/GPIO33 LOW 5s, then HIGH");
  rebootNb303();

  NB303_PORT.begin(kNb303Baud, SERIAL_8N1, kNb303RxPin, kNb303TxPin);

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

  uint32_t now = millis();
  if (!g_sleepLocked && (now - g_lastAtiMs) >= kAtiIntervalMs) {
    g_lastAtiMs = now;
    runAtiAndSleepLock();
  }

  bool duePublish = g_lastPublishMs == 0 || (now - g_lastPublishMs) >= kPublishIntervalMs;
  if (g_publishImmediatelyAfterRegister) {
    duePublish = true;
  }
  bool dueCereg = g_sleepLocked && (now - g_lastCeregMs) >= kCeregIntervalMs;

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

  static uint32_t lastBlinkMs = 0;
  if ((now - lastBlinkMs) >= 500) {
    lastBlinkMs = now;
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
  }
}

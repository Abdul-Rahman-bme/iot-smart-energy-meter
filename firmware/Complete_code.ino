#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <PZEM004Tv30.h>
#include <HardwareSerial.h>

#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// ======================================================
//                 USER CONFIG (FILL THESE)
// ======================================================
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Telegram (keep token private)
#define BOT_TOKEN "YOUR_TELEGRAM_BOT_TOKEN"

// MQTT (HiveMQ public broker)
const char* MQTT_HOST = "broker.hivemq.com";
const int   MQTT_PORT = 1883;

// Topics (match your dashboard)
const char* tV    = "unique/energy/voltage";
const char* tI    = "unique/energy/current";
const char* tP    = "unique/energy/power";
const char* tE    = "unique/energy/energy";
const char* tF    = "unique/energy/frequency";
const char* tPF   = "unique/energy/pf";
const char* tJSON = "unique/energy/data";

// ======================================================
//                 LCD + PZEM PINS
// ======================================================
LiquidCrystal_I2C lcd(0x27, 16, 2);   // change to 0x3F if needed
static const int I2C_SDA = 21;
static const int I2C_SCL = 22;

static const int PZEM_RX = 16; // ESP32 RX2 (connect to PZEM TX)
static const int PZEM_TX = 17; // ESP32 TX2 (connect to PZEM RX)

HardwareSerial PZEMSerial(2);
PZEM004Tv30 pzem(PZEMSerial, PZEM_RX, PZEM_TX);

// ======================================================
//                 NETWORK CLIENTS
// ======================================================
WiFiClient espClient;
PubSubClient mqtt(espClient);

WiFiClientSecure tgClient;
UniversalTelegramBot bot(BOT_TOKEN, tgClient);

Preferences prefs;

// ======================================================
//                 TIMING
// ======================================================
unsigned long lastReadMs  = 0;
unsigned long lastPubMs   = 0;
unsigned long lastLcdMs   = 0;
unsigned long lastAlertMs = 0;
unsigned long lastTgPollMs = 0;

const unsigned long READ_INTERVAL_MS  = 1000;
const unsigned long PUB_INTERVAL_MS   = 2000;
const unsigned long LCD_INTERVAL_MS   = 2000;
const unsigned long ALERT_CHECK_MS    = 1000;
const unsigned long TG_POLL_MS        = 2000;

// Set 5s while testing, 60s for real:
const unsigned long COOLDOWN_MS = 60UL * 1000UL;
// const unsigned long COOLDOWN_MS = 5UL * 1000UL;

int screenIndex = 0;

// ======================================================
//                 METER STATE
// ======================================================
enum DataState : uint8_t {
  DATA_OK = 0,
  DATA_MAINS_OFF,
  DATA_PZEM_COMMS_ERR
};

struct MeterData {
  float V, I, P, E, F, PF;
  float rawV;
  DataState state;
} d;

// ======================================================
//                 ALERT THRESHOLDS (EDIT)
// ======================================================
const float OVER_VOLT      = 255.0;
const float OVER_VOLT_CLR  = 250.0;

const float UNDER_VOLT     = 190.0;
const float UNDER_VOLT_CLR = 195.0;

const float OVER_CURRENT     = 10.0;  // A
const float OVER_CURRENT_CLR = 9.0;

const float OVER_POWER     = 2000.0;  // W
const float OVER_POWER_CLR = 1800.0;

const float FREQ_LOW   = 47.0;
const float FREQ_HIGH  = 53.0;
const float FREQ_CLR_L = 47.5;
const float FREQ_CLR_H = 52.5;

// ======================================================
//                 ALERT CONTROL + FLAGS
// ======================================================
bool alertsEnabled = true;
uint32_t muteUntilMs = 0;

bool isMuted() {
  if (muteUntilMs == 0) return false;
  return (int32_t)(millis() - muteUntilMs) < 0;
}

struct AlertFlags {
  bool mainsOff = false;
  bool commErr  = false;
  bool overV    = false;
  bool underV   = false;
  bool overI    = false;
  bool overP    = false;
  bool freqBad  = false;
  bool mqttDown = false;
} a;

unsigned long lastSent_mainsOff = 0;
unsigned long lastSent_commErr  = 0;
unsigned long lastSent_overV    = 0;
unsigned long lastSent_underV   = 0;
unsigned long lastSent_overI    = 0;
unsigned long lastSent_overP    = 0;
unsigned long lastSent_freqBad  = 0;
unsigned long lastSent_mqttDown = 0;
unsigned long lastSent_wifiUp   = 0;
unsigned long lastSent_boot     = 0;

bool bootMsgSent = false;
bool lastWifiConnected = false;

// ======================================================
//          TELEGRAM PAIRING (NO HARDCODE CHAT_ID)
// ======================================================
String authorizedChatId = "";   // stored in flash
bool   isPaired = false;
String pairingCode = "";

// Telegram duplicate-prevention
int tgLastUpdateId = 0;
bool tgPrimed = false;

// update_id conversion helper (works with both int and String library versions)
int updToInt(int v) { return v; }
int updToInt(long v) { return (int)v; }
int updToInt(unsigned long v) { return (int)v; }
int updToInt(const String& s) { return s.toInt(); }

// ======================================================
//                 SIMULATION MODE (SERIAL)
// ======================================================
bool simMode = false;

void resetAlertSystem() {
  a = AlertFlags();
  lastSent_mainsOff = lastSent_commErr = lastSent_overV = lastSent_underV = 0;
  lastSent_overI = lastSent_overP = lastSent_freqBad = lastSent_mqttDown = 0;
  lastSent_wifiUp = lastSent_boot = 0;
}

void setSimNormal() {
  simMode = true; resetAlertSystem();
  d.state = DATA_OK;
  d.V = 240.0; d.I = 0.50; d.P = 120.0; d.F = 50.0; d.PF = 0.90;
}
void setSimMainsOff() { simMode = true; resetAlertSystem(); d.state = DATA_MAINS_OFF; d.V=d.I=d.P=d.F=d.PF=0; }
void setSimCommErr()  { simMode = true; resetAlertSystem(); d.state = DATA_PZEM_COMMS_ERR; d.V=d.I=d.P=d.F=d.PF=0; }
void setSimOverV()    { simMode = true; resetAlertSystem(); d.state = DATA_OK; d.V = OVER_VOLT+5; d.I=0.5; d.P=200; d.F=50; d.PF=0.9; }
void setSimUnderV()   { simMode = true; resetAlertSystem(); d.state = DATA_OK; d.V = UNDER_VOLT-5; d.I=0.5; d.P=100; d.F=50; d.PF=0.9; }
void setSimOverI()    { simMode = true; resetAlertSystem(); d.state = DATA_OK; d.V=240; d.I=OVER_CURRENT+1; d.P=d.V*d.I; d.F=50; d.PF=0.9; }
void setSimOverP()    { simMode = true; resetAlertSystem(); d.state = DATA_OK; d.V=240; d.I=8; d.P=OVER_POWER+200; d.F=50; d.PF=0.9; }
void setSimFreqBad()  { simMode = true; resetAlertSystem(); d.state = DATA_OK; d.V=240; d.I=0.5; d.P=100; d.F=FREQ_HIGH+2; d.PF=0.9; }

void handleSerialCommands() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim(); cmd.toLowerCase();

  if (cmd == "help") {
    Serial.println("SIM commands:");
    Serial.println("  sim normal | sim mainsoff | sim commerr | sim overv | sim underv | sim overi | sim overp | sim freqbad");
    Serial.println("  sim off");
    return;
  }
  if (cmd == "sim off") { simMode = false; resetAlertSystem(); Serial.println("[SIM] OFF"); return; }
  if (cmd == "sim normal")   { setSimNormal();   Serial.println("[SIM] NORMAL"); return; }
  if (cmd == "sim mainsoff") { setSimMainsOff(); Serial.println("[SIM] MAINS OFF"); return; }
  if (cmd == "sim commerr")  { setSimCommErr();  Serial.println("[SIM] COMMS ERR"); return; }
  if (cmd == "sim overv")    { setSimOverV();    Serial.println("[SIM] OVER V"); return; }
  if (cmd == "sim underv")   { setSimUnderV();   Serial.println("[SIM] UNDER V"); return; }
  if (cmd == "sim overi")    { setSimOverI();    Serial.println("[SIM] OVER I"); return; }
  if (cmd == "sim overp")    { setSimOverP();    Serial.println("[SIM] OVER P"); return; }
  if (cmd == "sim freqbad")  { setSimFreqBad();  Serial.println("[SIM] FREQ BAD"); return; }

  Serial.println("Unknown. Type help");
}

// ======================================================
//                 CORE HELPERS
// ======================================================
void wifiEnsureConnected() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(200);
  }
}

void mqttEnsureConnected() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqtt.connected()) return;

  String cid = "ESP32Energy-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  mqtt.connect(cid.c_str());
}

bool sendTelegramTo(const String& chatId, const String& msg) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (chatId.length() == 0) return false;
  bool ok = bot.sendMessage(chatId, msg, "");
  Serial.println(ok ? "[TG] Sent" : "[TG] Failed");
  return ok;
}

// automatic alerts only (respects pairing + alertsEnabled + mute + cooldown)
bool trySendAlert(const String& msg, unsigned long &lastSentTs) {
  if (!isPaired) return false;
  if (!alertsEnabled) return false;
  if (isMuted()) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  unsigned long now = millis();
  if (now - lastSentTs < COOLDOWN_MS) return false;

  bool ok = bot.sendMessage(authorizedChatId, msg, "");
  if (ok) lastSentTs = now;
  return ok;
}

uint32_t parseDurationMs(String s) {
  s.trim(); s.toLowerCase();
  if (s.length() == 0) return 10UL * 60UL * 1000UL;

  if (s.endsWith("minutes")) s.replace("minutes","m");
  else if (s.endsWith("mins")) s.replace("mins","m");
  else if (s.endsWith("min")) s.replace("min","m");

  uint32_t mult = 60000UL;
  char last = s[s.length()-1];

  if (isAlpha((unsigned char)last)) {
    s.remove(s.length()-1);
    if (last=='s') mult = 1000UL;
    else if (last=='m') mult = 60000UL;
    else if (last=='h') mult = 3600000UL;
    else if (last=='d') mult = 86400000UL;
  }
  long val = s.toInt();
  if (val <= 0) val = 10;

  uint64_t dur = (uint64_t)val * (uint64_t)mult;
  const uint64_t CAP = 7ULL * 24ULL * 3600ULL * 1000ULL;
  if (dur > CAP) dur = CAP;
  return (uint32_t)dur;
}

String stateToText() {
  if (d.state == DATA_OK) return "OK";
  if (d.state == DATA_MAINS_OFF) return "MAINS OFF";
  return "PZEM ERROR";
}

String formatStatusMessage() {
  String msg = "📟 Energy Meter Status\n";
  msg += "State: " + stateToText() + "\n";
  msg += "WiFi: " + String(WiFi.status()==WL_CONNECTED ? "OK" : "DOWN") + "\n";
  msg += "MQTT: " + String(mqtt.connected() ? "OK" : "DOWN") + "\n";
  msg += "Alerts: " + String(alertsEnabled ? "ON" : "OFF") + "\n";
  msg += "Muted: " + String(isMuted() ? "YES" : "NO") + "\n\n";
  msg += "V: " + String(d.V,1) + " V\n";
  msg += "I: " + String(d.I,3) + " A\n";
  msg += "P: " + String(d.P,1) + " W\n";
  msg += "E: " + String(d.E,3) + " kWh\n";
  msg += "F: " + String(d.F,1) + " Hz\n";
  msg += "PF: " + String(d.PF,2) + "\n";
  if (simMode) msg += "\n⚙️ SIM MODE: ON";
  return msg;
}

// ======================================================
//                 PZEM READ
// ======================================================
void readPZEM() {
  d.rawV = pzem.voltage();
  float rawI  = pzem.current();
  float rawP  = pzem.power();
  float rawE  = pzem.energy();
  float rawF  = pzem.frequency();
  float rawPF = pzem.pf();

  if (isnan(d.rawV)) {
    d.state = DATA_PZEM_COMMS_ERR;
    d.V=d.I=d.P=d.E=d.F=d.PF=0.0;
    return;
  }

  if (d.rawV < 10.0) {
    d.state = DATA_MAINS_OFF;
    d.V=d.I=d.P=d.E=d.F=d.PF=0.0;
    return;
  }

  d.state = DATA_OK;
  d.V  = d.rawV;
  d.I  = isnan(rawI)  ? 0.0 : rawI;
  d.P  = isnan(rawP)  ? 0.0 : rawP;
  d.E  = isnan(rawE)  ? 0.0 : rawE;
  d.F  = isnan(rawF)  ? 0.0 : rawF;
  d.PF = isnan(rawPF) ? 0.0 : rawPF;
}

// ======================================================
//                 MQTT PUBLISH
// ======================================================
void publishMQTT() {
  if (!mqtt.connected()) return;

  mqtt.publish(tV,  String(d.V, 1).c_str(),  true);
  mqtt.publish(tI,  String(d.I, 3).c_str(),  true);
  mqtt.publish(tP,  String(d.P, 1).c_str(),  true);
  mqtt.publish(tE,  String(d.E, 3).c_str(),  true);
  mqtt.publish(tF,  String(d.F, 1).c_str(),  true);
  mqtt.publish(tPF, String(d.PF, 2).c_str(), true);

  String json = "{";
  json += "\"V\":"  + String(d.V,1)  + ",";
  json += "\"I\":"  + String(d.I,3)  + ",";
  json += "\"P\":"  + String(d.P,1)  + ",";
  json += "\"E\":"  + String(d.E,3)  + ",";
  json += "\"F\":"  + String(d.F,1)  + ",";
  json += "\"PF\":" + String(d.PF,2);
  json += "}";
  mqtt.publish(tJSON, json.c_str(), true);
}

// ======================================================
//                 LCD ROTATION
// ======================================================
void lcdShowScreen() {
  lcd.clear();

  // show pairing if not paired
  if (!isPaired) {
    lcd.setCursor(0,0); lcd.print("TG: /start");
    lcd.setCursor(0,1); lcd.print("Code: "); lcd.print(pairingCode);
    return;
  }

  if (d.state == DATA_MAINS_OFF) {
    lcd.setCursor(0,0); lcd.print("POWER CUT");
    lcd.setCursor(0,1); lcd.print("Mains OFF");
    return;
  }
  if (d.state == DATA_PZEM_COMMS_ERR) {
    lcd.setCursor(0,0); lcd.print("SENSOR ERROR");
    lcd.setCursor(0,1); lcd.print("PZEM no reply");
    return;
  }

  switch (screenIndex) {
    case 0:
      lcd.setCursor(0,0); lcd.print("Voltage");
      lcd.setCursor(0,1); lcd.print(d.V,1); lcd.print(" V");
      break;
    case 1:
      lcd.setCursor(0,0); lcd.print("Current");
      lcd.setCursor(0,1); lcd.print(d.I,3); lcd.print(" A");
      break;
    case 2:
      lcd.setCursor(0,0); lcd.print("Power");
      lcd.setCursor(0,1); lcd.print(d.P,1); lcd.print(" W");
      break;
    case 3:
      lcd.setCursor(0,0); lcd.print("Energy");
      lcd.setCursor(0,1); lcd.print(d.E,3); lcd.print(" kWh");
      break;
    case 4:
      lcd.setCursor(0,0); lcd.print("Frequency");
      lcd.setCursor(0,1); lcd.print(d.F,1); lcd.print(" Hz");
      break;
    case 5:
      lcd.setCursor(0,0); lcd.print("Power Factor");
      lcd.setCursor(0,1); lcd.print(d.PF,2);
      break;
  }

  screenIndex++;
  if (screenIndex > 5) screenIndex = 0;
}

// ======================================================
//                 ALERT CHECK
// ======================================================
void checkAlerts() {
  bool wifiNow = (WiFi.status() == WL_CONNECTED);

  if (wifiNow && !lastWifiConnected) {
    trySendAlert("✅ WiFi connected\nIP: " + WiFi.localIP().toString(), lastSent_wifiUp);
  }
  lastWifiConnected = wifiNow;

  if (wifiNow && isPaired && !bootMsgSent) {
    if (trySendAlert("✅ Energy meter ONLINE\nIP: " + WiFi.localIP().toString(), lastSent_boot)) {
      bootMsgSent = true;
    }
  }

  // MQTT down/up
  bool mqttOk = mqtt.connected();
  if (wifiNow && isPaired) {
    if (!mqttOk && !a.mqttDown) {
      a.mqttDown = true;
      trySendAlert("⚠️ MQTT disconnected\nBroker: " + String(MQTT_HOST), lastSent_mqttDown);
    }
    if (mqttOk && a.mqttDown) {
      a.mqttDown = false;
      trySendAlert("✅ MQTT reconnected", lastSent_mqttDown);
    }
  }

  // power cut / restored
  bool mainsOffNow = (d.state == DATA_MAINS_OFF);
  if (mainsOffNow && !a.mainsOff) {
    a.mainsOff = true;
    trySendAlert("🚨 POWER CUT detected\nMains OFF", lastSent_mainsOff);
  }
  if (!mainsOffNow && a.mainsOff && d.state == DATA_OK) {
    a.mainsOff = false;
    trySendAlert("✅ Power restored\nV=" + String(d.V,1) + "V, F=" + String(d.F,1) + "Hz", lastSent_mainsOff);
  }

  // PZEM comm error / recovered
  bool commErrNow = (d.state == DATA_PZEM_COMMS_ERR);
  if (commErrNow && !a.commErr) {
    a.commErr = true;
    trySendAlert("🚨 SENSOR ERROR\nPZEM not responding", lastSent_commErr);
  }
  if (!commErrNow && a.commErr) {
    a.commErr = false;
    trySendAlert("✅ Sensor OK\nPZEM responding again", lastSent_commErr);
  }

  if (d.state != DATA_OK) return;

  // Over V
  if (!a.overV && d.V >= OVER_VOLT) {
    a.overV = true;
    trySendAlert("🚨 OVER VOLTAGE\nV=" + String(d.V,1) + "V", lastSent_overV);
  } else if (a.overV && d.V <= OVER_VOLT_CLR) {
    a.overV = false;
    trySendAlert("✅ Voltage normal\nV=" + String(d.V,1) + "V", lastSent_overV);
  }

  // Under V
  if (!a.underV && d.V <= UNDER_VOLT) {
    a.underV = true;
    trySendAlert("🚨 UNDER VOLTAGE\nV=" + String(d.V,1) + "V", lastSent_underV);
  } else if (a.underV && d.V >= UNDER_VOLT_CLR) {
    a.underV = false;
    trySendAlert("✅ Voltage normal\nV=" + String(d.V,1) + "V", lastSent_underV);
  }

  // Over I
  if (!a.overI && d.I >= OVER_CURRENT) {
    a.overI = true;
    trySendAlert("🚨 OVER CURRENT\nI=" + String(d.I,3) + "A", lastSent_overI);
  } else if (a.overI && d.I <= OVER_CURRENT_CLR) {
    a.overI = false;
    trySendAlert("✅ Current normal\nI=" + String(d.I,3) + "A", lastSent_overI);
  }

  // Over P
  if (!a.overP && d.P >= OVER_POWER) {
    a.overP = true;
    trySendAlert("🚨 HIGH POWER\nP=" + String(d.P,1) + "W", lastSent_overP);
  } else if (a.overP && d.P <= OVER_POWER_CLR) {
    a.overP = false;
    trySendAlert("✅ Power normal\nP=" + String(d.P,1) + "W", lastSent_overP);
  }

  // Frequency abnormal
  bool freqBadNow = (d.F > 0.0) && (d.F < FREQ_LOW || d.F > FREQ_HIGH);
  if (!a.freqBad && freqBadNow) {
    a.freqBad = true;
    trySendAlert("🚨 FREQUENCY ABNORMAL\nF=" + String(d.F,1) + "Hz", lastSent_freqBad);
  } else if (a.freqBad && (d.F >= FREQ_CLR_L && d.F <= FREQ_CLR_H)) {
    a.freqBad = false;
    trySendAlert("✅ Frequency normal\nF=" + String(d.F,1) + "Hz", lastSent_freqBad);
  }
}

// ======================================================
//                 TELEGRAM COMMANDS + PAIRING
// ======================================================
String makePairingCode() {
  uint32_t r = (uint32_t)esp_random();
  uint32_t mac = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFF);
  uint32_t code = (r ^ mac) % 1000000UL;
  char buf[7];
  snprintf(buf, sizeof(buf), "%06lu", (unsigned long)code);
  return String(buf);
}

bool isAuthorizedChat(const String& chatId) {
  return isPaired && (chatId == authorizedChatId);
}

void handleTelegramCommand(const String& chatId, String text) {
  text.trim();
  String lower = text; lower.toLowerCase();

  // Not paired yet
  if (!isPaired) {
    if (lower == "/start") {
      String msg = "🔐 Device not paired.\nPair code: " + pairingCode + "\nSend: /pair " + pairingCode;
      sendTelegramTo(chatId, msg);
      return;
    }
    if (lower.startsWith("/pair")) {
      int sp = text.indexOf(' ');
      String code = (sp >= 0) ? text.substring(sp + 1) : "";
      code.trim();

      if (code == pairingCode) {
        authorizedChatId = chatId;
        isPaired = true;
        prefs.putString("chat_id", authorizedChatId);
        bootMsgSent = false; // allow online msg once paired
        sendTelegramTo(chatId, "✅ Paired!\nUse /help");
      } else {
        sendTelegramTo(chatId, "❌ Wrong code. Check LCD and send: /pair <CODE>");
      }
      return;
    }
    // ignore other commands
    return;
  }

  // Paired: ignore everyone else
  if (!isAuthorizedChat(chatId)) return;

  if (lower == "/help") {
    sendTelegramTo(chatId,
      "Commands:\n"
      "/status\n"
      "/alerts on | /alerts off\n"
      "/mute 10m (30s,2h,1d)\n"
      "/unmute\n"
      "/forget\n"
      "/help"
    );
    return;
  }

  if (lower == "/status") {
    sendTelegramTo(chatId, formatStatusMessage());
    return;
  }

  if (lower.startsWith("/alerts")) {
    if (lower.indexOf("on") >= 0) {
      alertsEnabled = true;
      sendTelegramTo(chatId, "✅ Alerts ENABLED");
    } else if (lower.indexOf("off") >= 0) {
      alertsEnabled = false;
      sendTelegramTo(chatId, "🔕 Alerts DISABLED (MQTT/LCD still run)");
    } else {
      sendTelegramTo(chatId, String("Alerts are ") + (alertsEnabled ? "ON" : "OFF"));
    }
    return;
  }

  if (lower.startsWith("/mute")) {
    String arg = "";
    int sp = text.indexOf(' ');
    if (sp >= 0) arg = text.substring(sp + 1);

    uint32_t dur = parseDurationMs(arg);
    muteUntilMs = millis() + dur;
    sendTelegramTo(chatId, "🔕 Muted alerts. Use /unmute to cancel.");
    return;
  }

  if (lower == "/unmute") {
    muteUntilMs = 0;
    sendTelegramTo(chatId, "✅ Unmuted.");
    return;
  }

  if (lower == "/forget") {
    prefs.remove("chat_id");
    authorizedChatId = "";
    isPaired = false;
    bootMsgSent = false;
    pairingCode = makePairingCode();
    sendTelegramTo(chatId, "🧹 Pairing cleared. Send /start to pair again.");
    return;
  }
}

void telegramPoll() {
  if (WiFi.status() != WL_CONNECTED) return;
  unsigned long now = millis();
  if (now - lastTgPollMs < TG_POLL_MS) return;
  lastTgPollMs = now;

  // Prime once after boot: drain old updates to avoid spam
  if (!tgPrimed) {
    int n = bot.getUpdates(0);
    if (n > 0) {
      tgLastUpdateId = updToInt(bot.messages[n - 1].update_id);
      prefs.putInt("last_uid", tgLastUpdateId);
    }
    tgPrimed = true;
    return;
  }

  int n = bot.getUpdates(tgLastUpdateId + 1);
  if (n <= 0) return;

  for (int i = 0; i < n; i++) {
    int uid = updToInt(bot.messages[i].update_id);
    if (uid <= tgLastUpdateId) continue;
    tgLastUpdateId = uid;

    String chatId = bot.messages[i].chat_id;
    String text   = bot.messages[i].text;
    handleTelegramCommand(chatId, text);
  }

  prefs.putInt("last_uid", tgLastUpdateId);
}

// ======================================================
//                 SETUP + LOOP
// ======================================================
void setup() {
  Serial.begin(115200);

  // I2C + LCD
  Wire.begin(I2C_SDA, I2C_SCL);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0,0); lcd.print("Energy Monitor");
  lcd.setCursor(0,1); lcd.print("Booting...");

  // PZEM Serial2
  PZEMSerial.begin(9600, SERIAL_8N1, PZEM_RX, PZEM_TX);

  // NVS
  prefs.begin("tg", false);
  authorizedChatId = prefs.getString("chat_id", "");
  isPaired = (authorizedChatId.length() > 0);
  tgLastUpdateId = prefs.getInt("last_uid", 0);

  pairingCode = makePairingCode();

  // WiFi
  wifiEnsureConnected();

  // Telegram TLS
  tgClient.setInsecure();
  tgClient.setTimeout(15000);

  // MQTT
  mqtt.setServer(MQTT_HOST, MQTT_PORT);

  delay(1200);
  lcd.clear();

  Serial.println("Ready. Serial sim: type 'help'. Telegram: /start (pairing) then /help.");
}

void loop() {
  handleSerialCommands();

  wifiEnsureConnected();
  mqttEnsureConnected();
  mqtt.loop();

  telegramPoll();

  unsigned long now = millis();

  if (now - lastReadMs >= READ_INTERVAL_MS) {
    lastReadMs = now;

    if (!simMode) readPZEM();

    Serial.printf("V=%.1f I=%.3f P=%.1f E=%.3f F=%.1f PF=%.2f state=%d paired=%d sim=%d\n",
                  d.V, d.I, d.P, d.E, d.F, d.PF, (int)d.state, (int)isPaired, (int)simMode);
  }

  if (now - lastPubMs >= PUB_INTERVAL_MS) {
    lastPubMs = now;
    publishMQTT();
  }

  if (now - lastLcdMs >= LCD_INTERVAL_MS) {
    lastLcdMs = now;
    lcdShowScreen();
  }

  if (now - lastAlertMs >= ALERT_CHECK_MS) {
    lastAlertMs = now;
    checkAlerts();
  }
}

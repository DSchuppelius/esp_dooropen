#include <Arduino.h>
#if defined(ESP32)
  #include <WiFi.h>
  #include <WebServer.h>
#else
  #include <ESP8266WiFi.h>
  #include <ESP8266WebServer.h>
#endif
#include <WiFiManager.h>
#include <EEPROM.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoSIP.h>
#include <PubSubClient.h>

#include "config.h"

// ------------------------------------------------------------
//  Globale Objekte
// ------------------------------------------------------------
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
#if defined(ESP32)
WebServer server(80);
#else
ESP8266WebServer server(80);
#endif

// SIP: Ein- und Ausgabepuffer + Client
char acSipIn[2048];
char acSipOut[2048];
Sip  aSip(acSipOut, sizeof(acSipOut));
String myIpStr;                      // eigene IP (muss fuer Sip.Init leben)

// Stabile Puffer fuer die SIP-Lib: sie merkt sich nur ZEIGER, keine Kopien.
// Deshalb duerfen diese Adressen sich nie aendern (kein String::c_str()!).
char sipServerBuf[41];
char sipUserBuf[25];
char sipPwBuf[33];
char myIpBuf[16];
char dialBuf[MAX_DIAL_LEN + 1];

// Laufzeit-Einstellungen (aus EEPROM; Defaults aus config.h)
uint8_t  buzzerSeconds     = DEFAULT_BUZZER_SECONDS;
bool     buzzerActive      = false;
uint32_t buzzerOffAt       = 0;      // millis-Zeitpunkt zum Abschalten
uint32_t buzzerTriggers    = 0;      // Zaehler

String   dialNr            = DEFAULT_DIAL_NR;  // Zielrufnummer
bool     callOnRing        = true;            // beim Klingeln automatisch anrufen
uint32_t callCount         = 0;
String   sipServer         = SIP_SERVER_IP;
uint16_t sipPort           = SIP_PORT;
String   sipUser           = SIP_USER;
String   sipPw             = SIP_PW;
uint32_t lastRegisterAt    = 0;

// Home Assistant / MQTT
WiFiClient   mqttNet;
PubSubClient mqtt(mqttNet);
String   mqttServer        = MQTT_SERVER;
uint16_t mqttPort          = MQTT_PORT;
String   mqttUser          = MQTT_USER;
String   mqttPw            = MQTT_PW;
char     mqttServerBuf[41];              // PubSubClient merkt sich nur den Zeiger
String   devId;                          // z.B. "tueroeffner_a1b2c3" (aus der MAC)
String   baseTopic;                      // z.B. "tueroeffner/a1b2c3"
uint32_t mqttLastTry       = 0;
uint32_t mqttRetryMs       = 5000;       // waechst bei Fehlschlag bis 60 s
bool     mqttDiscoveryDue  = false;      // Discovery (erneut) senden
bool     mqttStateDue      = false;      // Zustand sofort senden

bool     signalActive      = false;  // aktueller (entprellter) Zustand
bool     lastSignalRaw     = false;
uint32_t lastSignalChange  = 0;
uint32_t signalCount       = 0;
uint32_t lastRingAt        = 0;      // millis des letzten Klingelns
bool     hasDisplay        = false;  // OLED beim I2C-Scan gefunden?
bool     displayDirty      = true;
uint32_t lastActivityAt    = 0;      // fuer Bildschirmschoner
bool     displayOn         = true;

// ------------------------------------------------------------
//  EEPROM Persistenz (als Struct)
// ------------------------------------------------------------
static const uint8_t SETTINGS_MAGIC = 0x53;   // aendern, wenn sich Layout aendert
static const uint8_t MQTT_MAGIC     = 0x4D;   // Kennung fuer den angehaengten MQTT-Block
struct Settings {
  uint8_t  magic;
  uint8_t  buzzerSeconds;
  uint8_t  callOnRing;
  uint16_t sipPort;
  char     dialNr[MAX_DIAL_LEN + 1];
  char     sipServer[41];
  char     sipUser[25];
  char     sipPw[33];
  // Nachtraeglich HINTEN angehaengt, damit aeltere Einstellungen gueltig bleiben.
  // Fehlt der Block (mqttMagic falsch), gelten die Defaults aus config.h.
  uint8_t  mqttMagic;
  uint16_t mqttPort;
  char     mqttServer[41];
  char     mqttUser[33];
  char     mqttPw[33];
};
static const uint16_t EEPROM_SIZE = sizeof(Settings) + 8;

static void copyToField(char *dst, size_t dstSize, const String &src) {
  size_t n = src.length();
  if (n > dstSize - 1) n = dstSize - 1;
  memcpy(dst, src.c_str(), n);
  dst[n] = '\0';
}

void loadSettings() {
  EEPROM.begin(EEPROM_SIZE);
  Settings s;
  EEPROM.get(0, s);
  if (s.magic != SETTINGS_MAGIC) return;   // nichts gespeichert -> Defaults behalten
  if (s.buzzerSeconds >= MIN_BUZZER_SECONDS && s.buzzerSeconds <= MAX_BUZZER_SECONDS)
    buzzerSeconds = s.buzzerSeconds;
  callOnRing = s.callOnRing != 0;
  if (s.sipPort > 0) sipPort = s.sipPort;
  s.dialNr[sizeof(s.dialNr) - 1]       = '\0';
  s.sipServer[sizeof(s.sipServer) - 1] = '\0';
  s.sipUser[sizeof(s.sipUser) - 1]     = '\0';
  s.sipPw[sizeof(s.sipPw) - 1]         = '\0';
  dialNr    = String(s.dialNr);
  sipServer = String(s.sipServer);
  sipUser   = String(s.sipUser);
  sipPw     = String(s.sipPw);

  if (s.mqttMagic != MQTT_MAGIC) return;   // Einstellungen von vor dem MQTT-Update
  if (s.mqttPort > 0) mqttPort = s.mqttPort;
  s.mqttServer[sizeof(s.mqttServer) - 1] = '\0';
  s.mqttUser[sizeof(s.mqttUser) - 1]     = '\0';
  s.mqttPw[sizeof(s.mqttPw) - 1]         = '\0';
  mqttServer = String(s.mqttServer);
  mqttUser   = String(s.mqttUser);
  mqttPw     = String(s.mqttPw);
}

void saveSettings() {
  Settings s;
  memset(&s, 0, sizeof(s));
  s.magic         = SETTINGS_MAGIC;
  s.buzzerSeconds = buzzerSeconds;
  s.callOnRing    = callOnRing ? 1 : 0;
  s.sipPort       = sipPort;
  copyToField(s.dialNr,    sizeof(s.dialNr),    dialNr);
  copyToField(s.sipServer, sizeof(s.sipServer), sipServer);
  copyToField(s.sipUser,   sizeof(s.sipUser),   sipUser);
  copyToField(s.sipPw,     sizeof(s.sipPw),     sipPw);
  s.mqttMagic     = MQTT_MAGIC;
  s.mqttPort      = mqttPort;
  copyToField(s.mqttServer, sizeof(s.mqttServer), mqttServer);
  copyToField(s.mqttUser,   sizeof(s.mqttUser),   mqttUser);
  copyToField(s.mqttPw,     sizeof(s.mqttPw),     mqttPw);
  EEPROM.put(0, s);
  EEPROM.commit();
}

// SIP-Client mit den aktuellen Einstellungen (neu) initialisieren
void initSip() {
  if (myIpStr.length() == 0) myIpStr = WiFi.localIP().toString();
  copyToField(sipServerBuf, sizeof(sipServerBuf), sipServer);
  copyToField(sipUserBuf,   sizeof(sipUserBuf),   sipUser);
  copyToField(sipPwBuf,     sizeof(sipPwBuf),     sipPw);
  copyToField(myIpBuf,      sizeof(myIpBuf),      myIpStr);
  aSip.Init(sipServerBuf, sipPort, myIpBuf, sipPort,
            sipUserBuf, sipPwBuf, SIP_MAX_DIAL_SEC);
  aSip.SetBeepSeconds(SIP_BEEP_SECONDS);
  // Hier bewusst blockierend warten (Boot / neue Zugangsdaten), damit das
  // Ergebnis gleich feststeht. Erneuerungen laufen spaeter nebenher in loop().
  for (int i = 0; i < 3 && !aSip.IsRegistered(); i++) {   // erster Versuch nach Boot scheitert oft am Timing
    if (i > 0) delay(500);
    aSip.StartRegister(SIP_REG_EXPIRES);
    while (aSip.IsRegistering()) {
      aSip.Processing(acSipIn, sizeof(acSipIn));
      delay(5);
    }
  }
  lastRegisterAt = millis();
  Serial.printf("SIP-REGISTER: %s\n", aSip.IsRegistered() ? "OK" : "fehlgeschlagen");
}

// ------------------------------------------------------------
//  Relais / Summer
// ------------------------------------------------------------
void setRelay(bool on) {
  bool level = RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(PIN_RELAY, level ? HIGH : LOW);
  bool led = STATUS_LED_ACTIVE_LOW ? !on : on;
  digitalWrite(PIN_STATUS_LED, led ? HIGH : LOW);
}

// Aktivitaet melden -> Bildschirmschoner-Timer zuruecksetzen und Display wecken
void markActivity() {
  lastActivityAt = millis();
  if (hasDisplay && !displayOn) {
    display.ssd1306_command(SSD1306_DISPLAYON);
    displayOn = true;
    displayDirty = true;
  }
}

void startBuzzer() {
  buzzerActive = true;
  buzzerOffAt  = millis() + (uint32_t)buzzerSeconds * 1000UL;
  buzzerTriggers++;
  setRelay(true);
  markActivity();
  displayDirty = true;
}

void stopBuzzer() {
  buzzerActive = false;
  setRelay(false);
  displayDirty = true;
}

// SIP-Anruf an die Zielrufnummer ausloesen (nur Signalisierung, kein Audio)
bool placeCall() {
  if (dialNr.length() == 0) return false;
  copyToField(dialBuf, sizeof(dialBuf), dialNr);
  if (!aSip.Dial(dialBuf, SIP_CALLER_NAME)) return false;   // Anruf laeuft schon
  callCount++;
  markActivity();
  return true;
}

// ------------------------------------------------------------
//  Display
// ------------------------------------------------------------
// Wird der "Es klingelt"-Hinweis gerade angezeigt?
bool isRinging() {
  return lastRingAt != 0 && (millis() - lastRingAt) < RING_NOTIFY_MS;
}

void drawDisplay() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Auffaelliger Klingel-Screen
  if (isRinging()) {
    display.setTextSize(2);
    display.setCursor(8, 8);
    display.println(F("ES"));
    display.setCursor(8, 28);
    display.println(F("KLINGELT!"));
    display.setTextSize(1);
    display.setCursor(0, 52);
    display.print(F("Anzahl: "));
    display.println(signalCount);
    display.display();
    return;
  }

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(F("Tueroeffner"));
  display.drawFastHLine(0, 10, OLED_WIDTH, SSD1306_WHITE);

  display.setCursor(0, 16);
  if (WiFi.status() == WL_CONNECTED) {
    display.print(F("IP "));
    display.println(WiFi.localIP().toString());
  } else {
    display.println(F("WLAN: offline"));
  }

  display.setCursor(0, 28);
  display.print(F("Signal: "));
  display.println(signalActive ? F("AKTIV") : F("ruhig"));

  display.setCursor(0, 40);
  display.print(F("Summer: "));
  display.println(buzzerActive ? F("AN") : F("aus"));

  display.setCursor(0, 52);
  display.print(F("Dauer: "));
  display.print(buzzerSeconds);
  display.println(F("s"));

  display.display();
}

// ------------------------------------------------------------
//  Home Assistant (MQTT mit Auto-Discovery)
// ------------------------------------------------------------
// Eine Entitaet per Discovery anlegen; cfg = entitaetsspezifische JSON-Felder
void mqttDiscover(const char *component, const char *object, const String &cfg) {
  String topic = String(MQTT_DISCOVERY_PREFIX) + "/" + component + "/" + devId + "/" + object + "/config";
  String p = "{" + cfg;
  p += ",\"unique_id\":\"" + devId + "_" + object + "\"";
  p += ",\"availability_topic\":\"" + baseTopic + "/status\"";
  p += ",\"device\":{\"identifiers\":[\"" + devId + "\"],\"name\":\"Türöffner\","
       "\"manufacturer\":\"DIY\",\"model\":\"ESP SIP-Türöffner\","
       "\"configuration_url\":\"http://" + WiFi.localIP().toString() + "\"}}";
  mqtt.publish(topic.c_str(), p.c_str(), true);
}

void mqttPublishDiscovery() {
  const String st  = ",\"state_topic\":\"" + baseTopic + "/state\"";
  const String cmd = ",\"command_topic\":\"" + baseTopic + "/";

  mqttDiscover("button", "open",
    "\"name\":\"Tür öffnen\",\"icon\":\"mdi:door-open\"" + cmd + "open/set\"");
  mqttDiscover("event", "doorbell",
    "\"name\":\"Klingel\",\"device_class\":\"doorbell\",\"event_types\":[\"ring\"]"
    ",\"state_topic\":\"" + baseTopic + "/doorbell\"");
  mqttDiscover("binary_sensor", "ringing",
    "\"name\":\"Es klingelt\",\"icon\":\"mdi:bell-ring\"" + st +
    ",\"value_template\":\"{{ 'ON' if value_json.ringing else 'OFF' }}\"");
  mqttDiscover("binary_sensor", "buzzer",
    "\"name\":\"Summer\",\"device_class\":\"lock\"" + st +
    ",\"value_template\":\"{{ 'ON' if value_json.buzzer else 'OFF' }}\"");
  mqttDiscover("binary_sensor", "sip",
    "\"name\":\"SIP registriert\",\"device_class\":\"connectivity\",\"entity_category\":\"diagnostic\"" + st +
    ",\"value_template\":\"{{ 'ON' if value_json.sip else 'OFF' }}\"");
  mqttDiscover("switch", "callonring",
    "\"name\":\"Beim Klingeln anrufen\",\"icon\":\"mdi:phone-ring\",\"entity_category\":\"config\"" + st + cmd +
    "callonring/set\",\"value_template\":\"{{ 'ON' if value_json.callonring else 'OFF' }}\"");
  mqttDiscover("number", "duration",
    "\"name\":\"Summer-Dauer\",\"icon\":\"mdi:timer-outline\",\"entity_category\":\"config\""
    ",\"min\":" + String(MIN_BUZZER_SECONDS) + ",\"max\":" + String(MAX_BUZZER_SECONDS) +
    ",\"mode\":\"box\",\"unit_of_measurement\":\"s\"" + st + cmd +
    "duration/set\",\"value_template\":\"{{ value_json.seconds }}\"");
  mqttDiscover("sensor", "rings",
    "\"name\":\"Klingeln\",\"icon\":\"mdi:bell\",\"state_class\":\"total_increasing\",\"entity_category\":\"diagnostic\"" + st +
    ",\"value_template\":\"{{ value_json.rings }}\"");
  mqttDiscover("sensor", "openings",
    "\"name\":\"Öffnungen\",\"icon\":\"mdi:door\",\"state_class\":\"total_increasing\",\"entity_category\":\"diagnostic\"" + st +
    ",\"value_template\":\"{{ value_json.openings }}\"");
  mqttDiscover("sensor", "calls",
    "\"name\":\"Anrufe\",\"icon\":\"mdi:phone\",\"state_class\":\"total_increasing\",\"entity_category\":\"diagnostic\"" + st +
    ",\"value_template\":\"{{ value_json.calls }}\"");
}

String mqttStateJson() {
  String j = "{\"ringing\":";
  j += isRinging() ? "true" : "false";
  j += ",\"buzzer\":";     j += buzzerActive ? "true" : "false";
  j += ",\"sip\":";        j += aSip.IsRegistered() ? "true" : "false";
  j += ",\"callonring\":"; j += callOnRing ? "true" : "false";
  j += ",\"seconds\":";    j += buzzerSeconds;
  j += ",\"rings\":";      j += signalCount;
  j += ",\"openings\":";   j += buzzerTriggers;
  j += ",\"calls\":";      j += callCount;
  j += "}";
  return j;
}

// Klingel-Ereignis fuer Automationen (Event-Entitaet, nicht retained)
void mqttRing() {
  if (mqtt.connected())
    mqtt.publish((baseTopic + "/doorbell").c_str(), "{\"event_type\":\"ring\"}");
  mqttStateDue = true;
}

void mqttCallback(char *topic, byte *payload, unsigned int len) {
  String t(topic);
  String p;
  p.reserve(len);
  for (unsigned int i = 0; i < len; i++) p += (char)payload[i];

  if (t == MQTT_DISCOVERY_PREFIX "/status") {        // HA neu gestartet
    if (p == "online") mqttDiscoveryDue = true;
    return;
  }
  if (t == baseTopic + "/open/set") {
    startBuzzer();
    aSip.Hangup();   // wie in der Weboberflaeche: Tuer auf -> Anruf beenden
  } else if (t == baseTopic + "/callonring/set") {
    callOnRing = (p == "ON");
    saveSettings();
  } else if (t == baseTopic + "/duration/set") {
    int v = (int)p.toFloat();   // HA sendet Zahlen evtl. als "5.0"
    if (v >= MIN_BUZZER_SECONDS && v <= MAX_BUZZER_SECONDS) {
      buzzerSeconds = (uint8_t)v;
      saveSettings();
      displayDirty = true;
    }
  }
  mqttStateDue = true;
}

// Broker-Einstellungen uebernehmen (Start und nach Aenderung im Web)
void initMqtt() {
  if (mqtt.connected()) {
    mqtt.publish((baseTopic + "/status").c_str(), "offline", true);
    mqtt.disconnect();
  }
  copyToField(mqttServerBuf, sizeof(mqttServerBuf), mqttServer);
  mqtt.setServer(mqttServerBuf, mqttPort);
  mqttRetryMs = 5000;
  mqttLastTry = millis() - mqttRetryMs;   // sofort verbinden
}

void mqttLoop() {
  if (mqttServerBuf[0] == 0) return;   // kein Broker eingestellt -> MQTT aus

  if (!mqtt.connected()) {
    // Verbindungsaufbau blockiert kurz -> nicht waehrend eines Anrufs
    if (aSip.IsBusy() || WiFi.status() != WL_CONNECTED) return;
    if (millis() - mqttLastTry < mqttRetryMs) return;
    mqttLastTry = millis();

    String will = baseTopic + "/status";
    bool ok = mqtt.connect(devId.c_str(),
                           mqttUser.length() ? mqttUser.c_str() : nullptr,
                           mqttPw.length()   ? mqttPw.c_str()   : nullptr,
                           will.c_str(), 0, true, "offline");
    if (!ok) {
      Serial.printf("MQTT: Verbindung fehlgeschlagen (rc=%d)\n", mqtt.state());
      mqttRetryMs = min<uint32_t>(mqttRetryMs * 2, 60000);
      return;
    }
    Serial.println(F("MQTT: verbunden"));
    mqttRetryMs = 5000;
    mqtt.publish(will.c_str(), "online", true);
    mqtt.subscribe((baseTopic + "/+/set").c_str());
    mqtt.subscribe(MQTT_DISCOVERY_PREFIX "/status");
    mqttDiscoveryDue = true;
  }

  mqtt.loop();

  if (mqttDiscoveryDue) {
    mqttDiscoveryDue = false;
    mqttPublishDiscovery();
    mqttStateDue = true;
  }

  // Zustand bei Aenderung sofort, sonst alle 60 s senden. Vergleich nur alle
  // 200 ms, damit nicht jede Loop einen String baut.
  static uint32_t lastCheck = 0, lastSent = 0;
  static String   lastState;
  if (mqttStateDue || millis() - lastCheck > 200) {
    lastCheck = millis();
    String s = mqttStateJson();
    if (mqttStateDue || s != lastState || millis() - lastSent > 60000) {
      mqttStateDue = false;
      if (mqtt.publish((baseTopic + "/state").c_str(), s.c_str(), true)) {
        lastState = s;
        lastSent  = millis();
      }
    }
  }
}

// ------------------------------------------------------------
//  Signal-Eingang (entprellt)
// ------------------------------------------------------------
void handleSignalInput() {
  int raw = digitalRead(PIN_SIGNAL);
  bool pressed = SIGNAL_ACTIVE_LOW ? (raw == LOW) : (raw == HIGH);

  if (pressed != lastSignalRaw) {
    lastSignalRaw    = pressed;
    lastSignalChange = millis();
  }

  if ((millis() - lastSignalChange) > SIGNAL_DEBOUNCE_MS) {
    if (pressed != signalActive) {
      signalActive = pressed;
      if (signalActive) {
        signalCount++;
        lastRingAt = millis();
        markActivity();
        mqttRing();
        if (callOnRing) placeCall();
      }
      displayDirty = true;
    }
  }
}

// ------------------------------------------------------------
//  Webserver
// ------------------------------------------------------------
const char PAGE_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="de"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="theme-color" content="#0f1115">
<title>Türöffner</title>
<style>
:root{--bg:#f2f3f7;--card:#fff;--tile:#f6f7fa;--fg:#15171c;--mut:#6b7280;--line:#e4e6eb;
 --acc:#2563eb;--ok:#16a34a;--warn:#f59e0b;--bad:#dc2626;--sh:0 10px 30px rgba(15,17,21,.08)}
@media(prefers-color-scheme:dark){:root{--bg:#0f1115;--card:#171a21;--tile:#1f232c;--fg:#eef0f4;
 --mut:#8b93a1;--line:#2a2f3a;--acc:#3b82f6;--ok:#22c55e;--sh:0 10px 30px rgba(0,0,0,.4)}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.4 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
 display:flex;justify-content:center;padding:24px 16px}
.app{width:100%;max-width:400px}
header{display:flex;align-items:center;justify-content:space-between;margin-bottom:16px}
h1{font-size:22px;margin:0;letter-spacing:-.02em}
.pill{display:flex;align-items:center;gap:6px;font-size:12px;color:var(--mut);background:var(--card);
 padding:6px 10px;border-radius:99px;box-shadow:var(--sh)}
.dot{width:8px;height:8px;border-radius:50%;background:var(--mut)}
.dot.on{background:var(--ok);box-shadow:0 0 0 3px color-mix(in srgb,var(--ok) 25%,transparent)}
.dot.off{background:var(--bad)}
.card{background:var(--card);border-radius:20px;padding:20px;box-shadow:var(--sh);margin-bottom:14px}
.seg{display:flex;background:var(--card);border-radius:14px;padding:4px;margin-bottom:14px;box-shadow:var(--sh)}
.seg button{flex:1;border:0;background:none;color:var(--mut);padding:10px 4px;border-radius:10px;font:inherit;font-size:14px;font-weight:600;cursor:pointer;white-space:nowrap}
.seg button.active{background:var(--acc);color:#fff}
.tab{display:none}.tab.show{display:block}
.ringing .ring .bg{stroke:var(--warn);animation:glow 1.2s ease-in-out infinite}
@keyframes glow{50%{opacity:.35}}
.ringing .hint{color:var(--warn);font-size:17px;font-weight:700}
.door{display:flex;flex-direction:column;align-items:center;padding:8px 0 4px}
.ring{position:relative;width:170px;height:170px}
.ring>svg{position:absolute;inset:0;transform:rotate(-90deg)}
.ring circle{fill:none;stroke-width:8}
.ring .bg{stroke:var(--line)}
.ring .pr{stroke:var(--ok);stroke-linecap:round;stroke-dasharray:490;stroke-dashoffset:490;transition:stroke-dashoffset .25s linear}
.open{position:absolute;inset:16px;border:0;border-radius:50%;background:var(--ok);color:#fff;cursor:pointer;
 font:inherit;font-size:17px;font-weight:700;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:6px;
 box-shadow:0 8px 24px color-mix(in srgb,var(--ok) 40%,transparent);transition:transform .1s}
.open:active{transform:scale(.95)}
.open svg{width:38px;height:38px}
.open.active{background:color-mix(in srgb,var(--ok) 80%,#000)}
.hint{color:var(--mut);font-size:13px;margin-top:10px;height:24px;display:flex;align-items:center}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}
.tile{background:var(--tile);border-radius:14px;padding:10px 12px}
.tile small{display:block;color:var(--mut);font-size:11px;text-transform:uppercase;letter-spacing:.04em}
.tile b{font-size:18px}
.on{color:var(--ok)}.off{color:var(--mut)}
.row2{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:12px}
.btn{border:0;border-radius:12px;padding:12px;font:inherit;font-weight:600;cursor:pointer;background:var(--tile);color:var(--fg)}
.btn:active{opacity:.7}
.btn.pri{background:var(--acc);color:#fff;width:100%;margin-top:14px}
h2{font-size:13px;text-transform:uppercase;letter-spacing:.05em;color:var(--mut);margin:0 0 12px}
label{display:block;font-size:13px;color:var(--mut);margin:10px 0 4px}
input[type=text],input[type=number],input[type=password]{width:100%;padding:11px 12px;border-radius:10px;
 border:1px solid var(--line);background:var(--tile);color:var(--fg);font:inherit}
input:focus{outline:2px solid var(--acc);outline-offset:-1px}
.two{display:grid;grid-template-columns:2fr 1fr;gap:8px}
.sw{display:flex;align-items:center;justify-content:space-between;margin-top:14px}
.sw input{appearance:none;width:46px;height:28px;border-radius:99px;background:var(--line);position:relative;cursor:pointer;transition:.2s;margin:0}
.sw input::after{content:"";position:absolute;top:3px;left:3px;width:22px;height:22px;border-radius:50%;background:#fff;transition:.2s}
.sw input:checked{background:var(--ok)}
.sw input:checked::after{left:21px}
.toast{position:fixed;left:50%;bottom:24px;transform:translate(-50%,120px);background:var(--fg);color:var(--bg);
 padding:12px 18px;border-radius:12px;font-weight:600;transition:transform .25s;box-shadow:var(--sh)}
.toast.show{transform:translate(-50%,0)}
.toast.err{background:var(--bad);color:#fff}
.kv{display:flex;justify-content:space-between;gap:8px;padding:8px 0;border-bottom:1px solid var(--line);font-size:14px}
.kv span{color:var(--mut)}
code{font:13px ui-monospace,Consolas,monospace;word-break:break-all}
.note{color:var(--mut);font-size:13px;margin:12px 0 0}
</style></head><body>
<div class="app">
 <header>
  <h1>Türöffner</h1>
  <div class="pill"><span class="dot" id="conn"></span><span id="connt">verbinde…</span></div>
 </header>

 <div class="seg">
  <button id="tb0" class="active" onclick="tab(0)">Steuerung</button>
  <button id="tb1" onclick="tab(1)">Einstellungen</button>
  <button id="tb2" onclick="tab(2)">Home Assistant</button>
 </div>

 <div id="t0" class="tab show">
  <div class="card" id="doorc">
   <div class="door">
    <div class="ring">
     <svg viewBox="0 0 170 170"><circle class="bg" cx="85" cy="85" r="78"/><circle class="pr" id="prog" cx="85" cy="85" r="78"/></svg>
     <button class="open" id="openb" onclick="openDoor()">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="11" width="18" height="11" rx="2"/><path d="M7 11V7a5 5 0 0 1 9.9-1"/></svg>
      <span id="openl">Öffnen</span>
     </button>
    </div>
    <div class="hint" id="hint">&nbsp;</div>
   </div>
  </div>

  <div class="card">
   <div class="grid">
    <div class="tile"><small>Signal</small><b id="sig">–</b></div>
    <div class="tile"><small>Summer</small><b id="buz">–</b></div>
    <div class="tile"><small>SIP</small><b id="reg">–</b></div>
    <div class="tile"><small>Klingeln</small><b id="rings">–</b></div>
    <div class="tile"><small>Öffnungen</small><b id="cnt">–</b></div>
    <div class="tile"><small>Anrufe</small><b id="calls">–</b></div>
   </div>
   <div class="row2">
    <button class="btn" onclick="testCall()">📞 Test-Anruf</button>
    <button class="btn" id="snd" onclick="initAudio();beep()">🔇 Ton testen</button>
   </div>
  </div>
 </div>

 <div id="t1" class="tab">
  <div class="card">
   <h2>Türsummer</h2>
   <label for="dur">Summer-Dauer (Sekunden)</label>
   <input id="dur" type="number" min="1" max="30">
   <button class="btn pri" onclick="saveDur()">Speichern</button>
  </div>
  <div class="card">
   <h2>Anruf beim Klingeln</h2>
   <label for="dial">Ziel (Nebenstelle)</label>
   <input id="dial" type="text" inputmode="tel">
   <div class="sw"><span>Beim Klingeln anrufen</span><input id="auto" type="checkbox"></div>
   <button class="btn pri" onclick="saveDial()">Speichern</button>
  </div>
  <div class="card">
   <h2>SIP-Zugang</h2>
   <div class="two">
    <div><label for="sipserver">Server</label><input id="sipserver" type="text"></div>
    <div><label for="sipport">Port</label><input id="sipport" type="number" min="1" max="65535"></div>
   </div>
   <label for="sipuser">Benutzer</label>
   <input id="sipuser" type="text" autocomplete="off">
   <label for="sippw">Passwort</label>
   <input id="sippw" type="password" placeholder="unverändert" autocomplete="new-password">
   <button class="btn pri" onclick="saveSip()">Speichern &amp; neu registrieren</button>
  </div>
 </div>

 <div id="t2" class="tab">
  <div class="card">
   <h2>MQTT-Broker <b id="mqst" class="off" style="float:right;text-transform:none">–</b></h2>
   <div class="two">
    <div><label for="mqserver">Broker</label><input id="mqserver" type="text" placeholder="leer = aus"></div>
    <div><label for="mqport">Port</label><input id="mqport" type="number" min="1" max="65535"></div>
   </div>
   <label for="mquser">Benutzer</label>
   <input id="mquser" type="text" autocomplete="off">
   <label for="mqpw">Passwort</label>
   <input id="mqpw" type="password" placeholder="unverändert" autocomplete="new-password">
   <button class="btn pri" onclick="saveMqtt()">Speichern &amp; verbinden</button>
  </div>
  <div class="card">
   <h2>Gerät</h2>
   <div class="kv"><span>Geräte-ID</span><code id="mqid">–</code></div>
   <div class="kv"><span>Basis-Topic</span><code id="mqbase">–</code></div>
   <p class="note">Das Gerät erscheint automatisch unter <b>Einstellungen → Geräte &amp; Dienste → MQTT</b>
   mit „Tür öffnen“, „Klingel“ (Ereignis für Automationen), Summer, SIP-Status und Zählern.
   Voraussetzung: Mosquitto-Add-on und MQTT-Integration.</p>
  </div>
 </div>
</div>
<div class="toast" id="toast"></div>
<script>
let audioCtx=null,soundOn=false,wasRinging=false,buzEnd=0,buzDur=1,tt;
const $=id=>document.getElementById(id);
function tab(n){for(let i=0;i<3;i++){$('t'+i).className=i==n?'tab show':'tab';$('tb'+i).className=i==n?'active':'';}}
function toast(msg,err){const t=$('toast');t.textContent=msg;t.className='toast show'+(err?' err':'');
 clearTimeout(tt);tt=setTimeout(()=>t.className='toast'+(err?' err':''),2500);}
function initAudio(){
 if(!audioCtx)audioCtx=new(window.AudioContext||window.webkitAudioContext)();
 if(audioCtx.state==='suspended')audioCtx.resume();
 soundOn=true;$('snd').textContent='🔊 Ton an';
}
function beep(){
 if(!audioCtx)return;const t=audioCtx.currentTime;
 for(let i=0;i<3;i++){const o=audioCtx.createOscillator(),g=audioCtx.createGain();
  o.type='square';o.frequency.value=880;o.connect(g);g.connect(audioCtx.destination);
  const s=t+i*.35;g.gain.setValueAtTime(.001,s);g.gain.exponentialRampToValueAtTime(.3,s+.02);
  g.gain.exponentialRampToValueAtTime(.001,s+.25);o.start(s);o.stop(s+.26);}
}
document.addEventListener('click',initAudio,{once:true});
function setVal(id,v){const e=$(id);if(document.activeElement!==e)e.value=v;}
function flag(id,on,a,b){const e=$(id);e.textContent=on?a:b;e.className=on?'on':'off';}
async function post(url,okMsg){
 try{const r=await fetch(url,{method:'POST'});let j={};try{j=await r.json();}catch(e){}
  if(r.ok){if(okMsg)toast(okMsg);}else toast(j.err||'Fehler',true);}
 catch(e){toast('Keine Verbindung',true);}
 refresh();
}
function anim(){
 const left=Math.max(0,buzEnd-Date.now()),f=left/(buzDur*1000);
 $('prog').style.strokeDashoffset=490*(1-f);
 const act=left>0;$('openb').className=act?'open active':'open';
 $('openl').textContent=act?Math.ceil(left/1000)+' s':'Öffnen';
 if(act)requestAnimationFrame(anim);
}
async function refresh(){
 try{
  const s=await(await fetch('/status')).json();
  $('conn').className='dot on';$('connt').textContent='online';
  flag('sig',s.signal,'aktiv','ruhig');flag('buz',s.buzzer,'an','aus');flag('reg',s.registered,'ok','nein');
  $('rings').textContent=s.signals;$('cnt').textContent=s.triggers;$('calls').textContent=s.calls;
  $('doorc').className=s.ringing?'card ringing':'card';
  if(s.ringing&&!wasRinging&&soundOn)beep();wasRinging=s.ringing;
  buzDur=s.seconds;
  if(s.buzzer&&buzEnd<Date.now()){buzEnd=Date.now()+s.seconds*1000;anim();}
  if(!s.buzzer&&buzEnd>Date.now()){buzEnd=0;anim();}
  $('hint').textContent=s.ringing?'🔔 Es klingelt!':s.dial?(s.callonring?'Klingeln ruft '+s.dial+' an – * am Telefon öffnet':'Anruf beim Klingeln ist aus'):'Keine Zielnummer eingestellt';
  setVal('dur',s.seconds);setVal('dial',s.dial);
  if(document.activeElement!==$('auto'))$('auto').checked=s.callonring;
  setVal('sipserver',s.sipserver);setVal('sipport',s.sipport);setVal('sipuser',s.sipuser);
  $('sippw').placeholder=s.haspw?'unverändert':'nicht gesetzt';
  setVal('mqserver',s.mqttserver);setVal('mqport',s.mqttport);setVal('mquser',s.mqttuser);
  $('mqpw').placeholder=s.hasmqttpw?'unverändert':'nicht gesetzt';
  $('mqid').textContent=s.mqttid;$('mqbase').textContent=s.mqttbase;
  if(s.mqttserver)flag('mqst',s.mqtt,'verbunden','getrennt');else{$('mqst').textContent='aus';$('mqst').className='off';}
 }catch(e){$('conn').className='dot off';$('connt').textContent='offline';}
}
function openDoor(){buzEnd=Date.now()+buzDur*1000;anim();post('/open','Tür wird geöffnet');}
function saveDur(){post('/setduration?s='+$('dur').value,'Gespeichert');}
function saveDial(){post('/setdial?nr='+encodeURIComponent($('dial').value)+'&auto='+($('auto').checked?'1':'0'),'Gespeichert');}
function testCall(){post('/call','Anruf wird aufgebaut');}
function saveSip(){
 const q=new URLSearchParams({server:$('sipserver').value,port:$('sipport').value,user:$('sipuser').value,pw:$('sippw').value});
 $('sippw').value='';post('/setsip?'+q,'Gespeichert – registriere neu');
}
function saveMqtt(){
 const q=new URLSearchParams({server:$('mqserver').value,port:$('mqport').value,user:$('mquser').value,pw:$('mqpw').value});
 $('mqpw').value='';post('/setmqtt?'+q,'Gespeichert – verbinde');
}
setInterval(refresh,1000);refresh();
</script>
</body></html>
)HTML";

void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", PAGE_HTML);
}

void handleStatus() {
  String json = "{";
  json += "\"signal\":"   + String(signalActive ? "true" : "false");
  json += ",\"ringing\":" + String(isRinging() ? "true" : "false");
  json += ",\"buzzer\":"  + String(buzzerActive ? "true" : "false");
  json += ",\"seconds\":" + String(buzzerSeconds);
  json += ",\"triggers\":" + String(buzzerTriggers);
  json += ",\"signals\":"  + String(signalCount);
  json += ",\"dial\":\""   + dialNr + "\"";
  json += ",\"callonring\":" + String(callOnRing ? "true" : "false");
  json += ",\"calls\":"    + String(callCount);
  json += ",\"sipserver\":\"" + sipServer + "\"";
  json += ",\"sipport\":"  + String(sipPort);
  json += ",\"sipuser\":\"" + sipUser + "\"";
  json += ",\"haspw\":"    + String(sipPw.length() > 0 ? "true" : "false");
  json += ",\"registered\":" + String(aSip.IsRegistered() ? "true" : "false");
  json += ",\"mqttserver\":\"" + mqttServer + "\"";
  json += ",\"mqttport\":"  + String(mqttPort);
  json += ",\"mqttuser\":\"" + mqttUser + "\"";
  json += ",\"hasmqttpw\":" + String(mqttPw.length() > 0 ? "true" : "false");
  json += ",\"mqtt\":"      + String(mqtt.connected() ? "true" : "false");
  json += ",\"mqttid\":\""  + devId + "\"";
  json += ",\"mqttbase\":\"" + baseTopic + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleOpen() {
  startBuzzer();
  aSip.Hangup();   // Tuer ist auf -> laufenden Anruf beenden (klingelnd: CANCEL, sonst BYE)
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleSetDuration() {
  if (server.hasArg("s")) {
    int v = server.arg("s").toInt();
    if (v >= MIN_BUZZER_SECONDS && v <= MAX_BUZZER_SECONDS) {
      buzzerSeconds = (uint8_t)v;
      saveSettings();
      displayDirty = true;
      server.send(200, "application/json", "{\"ok\":true}");
      return;
    }
  }
  server.send(400, "application/json", "{\"ok\":false}");
}

void handleCall() {
  if (dialNr.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"err\":\"keine Zielnummer\"}");
    return;
  }
  bool ok = placeCall();
  server.send(ok ? 200 : 409, "application/json",
              ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"Anruf laeuft bereits\"}");
}

void handleSetDial() {
  if (server.hasArg("nr")) {
    String nr = server.arg("nr");
    if (nr.length() <= MAX_DIAL_LEN) {
      dialNr = nr;
      if (server.hasArg("auto")) callOnRing = server.arg("auto") == "1";
      saveSettings();
      server.send(200, "application/json", "{\"ok\":true}");
      return;
    }
  }
  server.send(400, "application/json", "{\"ok\":false}");
}

void handleSetSip() {
  if (server.hasArg("server")) sipServer = server.arg("server");
  if (server.hasArg("port")) {
    int p = server.arg("port").toInt();
    if (p > 0 && p <= 65535) sipPort = (uint16_t)p;
  }
  if (server.hasArg("user")) sipUser = server.arg("user");
  // Passwort nur uebernehmen, wenn ein neues angegeben wurde
  if (server.hasArg("pw") && server.arg("pw").length() > 0) sipPw = server.arg("pw");
  saveSettings();
  initSip();   // mit neuen Daten neu registrieren
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleSetMqtt() {
  if (server.hasArg("server")) {
    String s = server.arg("server");
    s.trim();
    if (s.length() < sizeof(mqttServerBuf)) mqttServer = s;
  }
  if (server.hasArg("port")) {
    int p = server.arg("port").toInt();
    if (p > 0 && p <= 65535) mqttPort = (uint16_t)p;
  }
  if (server.hasArg("user")) mqttUser = server.arg("user");
  // Passwort nur uebernehmen, wenn ein neues angegeben wurde
  if (server.hasArg("pw") && server.arg("pw").length() > 0) mqttPw = server.arg("pw");
  saveSettings();
  initMqtt();   // Verbindung mit neuen Daten aufbauen (in mqttLoop)
  server.send(200, "application/json", "{\"ok\":true}");
}

// ------------------------------------------------------------
//  Setup / Loop
// ------------------------------------------------------------
// Wird aufgerufen, sobald das WLAN-Konfig-Portal (AP) startet.
void configModeCallback(WiFiManager *wm) {
  if (!hasDisplay) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(F("WLAN einrichten:"));
  display.drawFastHLine(0, 10, OLED_WIDTH, SSD1306_WHITE);
  display.setCursor(0, 14);
  display.println(F("1) WLAN waehlen:"));
  display.print(F("   "));
  display.println(F(WIFI_AP_NAME));
  display.setCursor(0, 38);
  display.println(F("2) Browser:"));
  display.print(F("   http://"));
  display.println(WiFi.softAPIP().toString());
  display.display();
}

void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_STATUS_LED, OUTPUT);
  setRelay(false);
  pinMode(PIN_SIGNAL, INPUT_PULLUP);

  loadSettings();

  Wire.begin(PIN_SDA, PIN_SCL);

  // I2C-Bus scannen und gefundene Adressen ausgeben (Diagnose)
  Serial.println(F("I2C-Scan:"));
  uint8_t foundAddr = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  gefunden: 0x%02X\n", addr);
      if (addr == 0x3C || addr == 0x3D) foundAddr = addr;
    }
  }
  if (foundAddr == 0) {
    // Ohne OLED alle Display-Zugriffe weglassen (spart I2C-Zeit in der Loop)
    Serial.println(F("  Kein OLED gefunden! (HW-364A: OLED an D5/D6)"));
  } else if (!display.begin(SSD1306_SWITCHCAPVCC, foundAddr)) {
    Serial.printf("SSD1306 Init fehlgeschlagen (Adresse 0x%02X)!\n", foundAddr);
  } else {
    hasDisplay = true;
    Serial.printf("Display OK auf 0x%02X\n", foundAddr);
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(F("Starte..."));
    display.println(F("WLAN verbinden"));
    display.display();
  }

  WiFiManager wm;
  wm.setAPCallback(configModeCallback);
  wm.setConfigPortalTimeout(300);
  if (!wm.autoConnect(WIFI_AP_NAME)) {
    Serial.println(F("WLAN fehlgeschlagen, Neustart"));
    ESP.restart();
  }
  // Modem-Sleep aus: sonst verschluckt der ESP Unicast-Pakete (Ping/HTTP)
#if defined(ESP32)
  WiFi.setSleep(false);
#else
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
#endif
  Serial.print(F("IP: "));
  Serial.println(WiFi.localIP());

  // SIP-Client initialisieren (eigene IP muss als String erhalten bleiben)
  myIpStr = WiFi.localIP().toString();
  initSip();

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/open", HTTP_POST, handleOpen);
  server.on("/setduration", HTTP_POST, handleSetDuration);
  server.on("/call", HTTP_POST, handleCall);
  server.on("/setdial", HTTP_POST, handleSetDial);
  server.on("/setsip", HTTP_POST, handleSetSip);
  server.on("/setmqtt", HTTP_POST, handleSetMqtt);
  server.begin();

  // Home Assistant / MQTT: eindeutige Kennung aus den letzten 3 MAC-Bytes
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  mac.toLowerCase();
  devId     = "tueroeffner_" + mac.substring(6);
  baseTopic = "tueroeffner/" + mac.substring(6);
  mqttNet.setTimeout(MQTT_CONNECT_TIMEOUT_MS);
  mqtt.setBufferSize(1024);     // Discovery-Nachrichten sind laenger als die 256-Byte-Voreinstellung
  mqtt.setSocketTimeout(2);     // max. Wartezeit auf CONNACK (s)
  mqtt.setCallback(mqttCallback);
  initMqtt();

  lastActivityAt = millis();
  displayDirty = true;
}

void updateDisplay();

void loop() {
  server.handleClient();
  aSip.Processing(acSipIn, sizeof(acSipIn));
  handleSignalInput();
  mqttLoop();

  // DTMF vom Telefon: '*' oeffnet die Tuer (loest den Summer aus) und legt auf
  char dtmf = aSip.ReadDtmf();
  if (dtmf) Serial.printf("DTMF empfangen: %c\n", dtmf);
  if (dtmf == '*') {
    startBuzzer();
    aSip.Hangup();
  }

  // SIP-Registrierung rechtzeitig erneuern (laeuft nebenher ueber Processing).
  // Ist sie fehlgeschlagen, schon nach SIP_REG_RETRY_SEC erneut versuchen.
  uint32_t regInterval = aSip.IsRegistered() ? (uint32_t)(SIP_REG_EXPIRES - 30) * 1000UL
                                             : (uint32_t)SIP_REG_RETRY_SEC * 1000UL;
  if (!aSip.IsRegistering() && millis() - lastRegisterAt > regInterval) {
    lastRegisterAt = millis();
    aSip.StartRegister(SIP_REG_EXPIRES);
  }

  if (buzzerActive && (int32_t)(millis() - buzzerOffAt) >= 0) {
    stopBuzzer();
  }

  if (hasDisplay) updateDisplay();
}

// Display nur neu zeichnen, wenn sich etwas Sichtbares geaendert hat
void updateDisplay() {
  // Bildschirmschoner: OLED nach Inaktivitaet ausschalten
  if (SCREEN_TIMEOUT_SECONDS > 0 && displayOn &&
      (millis() - lastActivityAt) > (uint32_t)SCREEN_TIMEOUT_SECONDS * 1000UL) {
    display.clearDisplay();
    display.display();
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    displayOn = false;
  }

  // Zustaende, die sich ohne eigenes Ereignis aendern: Ablauf des
  // Klingel-Hinweises und WLAN-Verbindung
  static bool lastRinging = false;
  static bool lastWifi    = false;
  bool ringing = isRinging();
  bool wifi    = WiFi.status() == WL_CONNECTED;
  if (ringing != lastRinging || wifi != lastWifi) {
    lastRinging  = ringing;
    lastWifi     = wifi;
    displayDirty = true;
  }

  // Nicht waehrend eines Anrufs zeichnen: das I2C-Update blockiert und
  // stoert den RTP-Takt. displayDirty bleibt gesetzt -> wird danach nachgeholt.
  if (displayOn && displayDirty && !aSip.IsBusy()) {
    displayDirty = false;
    drawDisplay();
  }
}

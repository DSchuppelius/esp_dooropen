// Home Assistant (MQTT mit Auto-Discovery)
#include "app.h"
#include <PubSubClient.h>

static WiFiClient   mqttNet;
static PubSubClient mqtt(mqttNet);
static char     mqttServerBuf[41];              // PubSubClient merkt sich nur den Zeiger
String   devId;                                 // z.B. "tueroeffner_a1b2c3" (aus der MAC)
String   baseTopic;                             // z.B. "tueroeffner/a1b2c3"
static uint32_t mqttLastTry  = 0;
static uint32_t mqttRetryMs  = 5000;            // waechst bei Fehlschlag bis 60 s
static uint32_t connectedAt  = 0;
bool     mqttDiscoveryDue    = false;           // Discovery (erneut) senden
bool     mqttStateDue        = false;           // Zustand sofort senden

// Befehle kurz nach dem Verbinden ignorieren: so loest eine gespeicherte
// (retained) Nachricht nicht bei jedem Neustart die Tuer aus
static const uint32_t COMMAND_GRACE_MS = 2000;
static const char *const COMMANDS[] = { "open", "open2", "lock", "callonring", "quiet", "praxis", "duration", "incoming" };

bool mqttConnected() {
  return mqtt.connected();
}

static String discoveryTopic(const char *component, const char *object) {
  return String(MQTT_DISCOVERY_PREFIX) + "/" + component + "/" + devId + "/" + object + "/config";
}

// Eine Entitaet per Discovery anlegen. tmpl (im Flash) enthaelt die
// entitaetsspezifischen JSON-Felder; Platzhalter:
//   $S = ,"state_topic":"<basis>/state"   $C = ,"command_topic":"<basis>/
static void mqttDiscover(const char *component, const char *object, const __FlashStringHelper *tmpl) {
  String cfg(tmpl);
  cfg.replace("$S", ",\"state_topic\":\"" + baseTopic + "/state\"");
  cfg.replace("$C", ",\"command_topic\":\"" + baseTopic + "/");
  cfg.replace("$B", baseTopic);
  cfg.replace("$N", jsonEsc(relay2Name));
  String p;
  p.reserve(cfg.length() + 300);
  p = "{" + cfg;
  p += F(",\"unique_id\":\""); p += devId; p += '_'; p += object; p += '"';
  p += F(",\"availability_topic\":\""); p += baseTopic; p += F("/status\"");
  p += F(",\"device\":{\"identifiers\":[\""); p += devId;
  p += F("\"],\"name\":\"Türöffner\",\"manufacturer\":\"DIY\",\"model\":\"ESP SIP-Türöffner\","
         "\"sw_version\":\"" FW_VERSION "\",\"configuration_url\":\"http://");
  p += netIP().toString();
  p += F("\"}}");
  mqtt.publish(discoveryTopic(component, object).c_str(), p.c_str(), true);
}

// Entitaet wieder entfernen (leere Konfiguration)
static void mqttForget(const char *component, const char *object) {
  mqtt.publish(discoveryTopic(component, object).c_str(), "", true);
}

#define STR_(x) #x
#define STR(x)  STR_(x)

static void mqttPublishDiscovery() {
  mqttDiscover("button", "open", F(
    "\"name\":\"Tür öffnen\",\"icon\":\"mdi:door-open\"$Copen/set\""));
  // Schloss: "Aufschliessen"/"Oeffnen" loest den Summer aus, Zustand = Summer
  mqttDiscover("lock", "lock", F(
    "\"name\":\"Türsummer\"$S$Clock/set\""
    ",\"value_template\":\"{{ 'UNLOCKED' if value_json.buzzer else 'LOCKED' }}\""
    ",\"payload_lock\":\"LOCK\",\"payload_unlock\":\"UNLOCK\",\"payload_open\":\"OPEN\""
    ",\"state_locked\":\"LOCKED\",\"state_unlocked\":\"UNLOCKED\""));
  mqttDiscover("event", "doorbell", F(
    "\"name\":\"Klingel\",\"device_class\":\"doorbell\",\"event_types\":[\"ring\"]"
    ",\"state_topic\":\"$B/doorbell\""));
  mqttDiscover("binary_sensor", "ringing", F(
    "\"name\":\"Es klingelt\",\"icon\":\"mdi:bell-ring\"$S"
    ",\"value_template\":\"{{ 'ON' if value_json.ringing else 'OFF' }}\""));
  mqttDiscover("binary_sensor", "buzzer", F(
    "\"name\":\"Summer\",\"device_class\":\"lock\"$S"
    ",\"value_template\":\"{{ 'ON' if value_json.buzzer else 'OFF' }}\""));
  mqttDiscover("binary_sensor", "sip", F(
    "\"name\":\"SIP registriert\",\"device_class\":\"connectivity\",\"entity_category\":\"diagnostic\"$S"
    ",\"value_template\":\"{{ 'ON' if value_json.sip else 'OFF' }}\""));
  mqttDiscover("sensor", "sipstate", F(
    "\"name\":\"SIP-Status\",\"icon\":\"mdi:phone-check\",\"entity_category\":\"diagnostic\"$S"
    ",\"value_template\":\"{{ value_json.sipstate }}\""));
  mqttDiscover("sensor", "last", F(
    "\"name\":\"Letztes Ereignis\",\"icon\":\"mdi:history\"$S"
    ",\"value_template\":\"{{ value_json.last }}\""));
  mqttDiscover("switch", "callonring", F(
    "\"name\":\"Beim Klingeln anrufen\",\"icon\":\"mdi:phone-ring\",\"entity_category\":\"config\"$S"
    "$Ccallonring/set\",\"value_template\":\"{{ 'ON' if value_json.callonring else 'OFF' }}\""));
  mqttDiscover("switch", "quiet", F(
    "\"name\":\"Nachtruhe\",\"icon\":\"mdi:sleep\",\"entity_category\":\"config\"$S"
    "$Cquiet/set\",\"value_template\":\"{{ 'ON' if value_json.quiet else 'OFF' }}\""));
  mqttDiscover("switch", "praxis", F(
    "\"name\":\"Praxis-Modus\",\"icon\":\"mdi:door-sliding-open\",\"entity_category\":\"config\"$S"
    "$Cpraxis/set\",\"value_template\":\"{{ 'ON' if value_json.praxis else 'OFF' }}\""));
  mqttDiscover("switch", "incoming", F(
    "\"name\":\"Anrufe annehmen\",\"icon\":\"mdi:phone-incoming\",\"entity_category\":\"config\"$S"
    "$Cincoming/set\",\"value_template\":\"{{ 'ON' if value_json.incoming else 'OFF' }}\""));
  mqttDiscover("number", "duration", F(
    "\"name\":\"Summer-Dauer\",\"icon\":\"mdi:timer-outline\",\"entity_category\":\"config\""
    ",\"min\":" STR(MIN_BUZZER_SECONDS) ",\"max\":" STR(MAX_BUZZER_SECONDS)
    ",\"mode\":\"box\",\"unit_of_measurement\":\"s\"$S"
    "$Cduration/set\",\"value_template\":\"{{ value_json.seconds }}\""));
  mqttDiscover("sensor", "rings", F(
    "\"name\":\"Klingeln\",\"icon\":\"mdi:bell\",\"state_class\":\"total_increasing\",\"entity_category\":\"diagnostic\"$S"
    ",\"value_template\":\"{{ value_json.rings }}\""));
  mqttDiscover("sensor", "openings", F(
    "\"name\":\"Öffnungen\",\"icon\":\"mdi:door\",\"state_class\":\"total_increasing\",\"entity_category\":\"diagnostic\"$S"
    ",\"value_template\":\"{{ value_json.openings }}\""));
  mqttDiscover("sensor", "calls", F(
    "\"name\":\"Anrufe\",\"icon\":\"mdi:phone\",\"state_class\":\"total_increasing\",\"entity_category\":\"diagnostic\"$S"
    ",\"value_template\":\"{{ value_json.calls }}\""));
  if (relay2On) {
    mqttDiscover("button", "open2", F(
      "\"name\":\"$N öffnen\",\"icon\":\"mdi:garage-open\"$Copen2/set\""));
  } else {
    mqttForget("button", "open2");
  }
  if (doorOn) {
    mqttDiscover("binary_sensor", "door", F(
      "\"name\":\"Tür\",\"device_class\":\"door\"$S"
      ",\"value_template\":\"{{ 'ON' if value_json.door else 'OFF' }}\""));
    mqttDiscover("binary_sensor", "dooralert", F(
      "\"name\":\"Tür zu lange offen\",\"device_class\":\"problem\"$S"
      ",\"value_template\":\"{{ 'ON' if value_json.dooralert else 'OFF' }}\""));
  } else {
    mqttForget("binary_sensor", "door");
    mqttForget("binary_sensor", "dooralert");
  }
}

static String mqttStateJson() {
  String j;
  j.reserve(360);
  j = "{\"ringing\":";
  j += isRinging() ? "true" : "false";
  j += ",\"buzzer\":";     j += buzzerActive ? "true" : "false";
  j += ",\"relay2\":";     j += relay2Active ? "true" : "false";
  j += ",\"sip\":";        j += aSip.IsRegistered() ? "true" : "false";
  j += ",\"sipstate\":\""; j += sipStateText(); j += '"';
  j += ",\"callonring\":"; j += callOnRing ? "true" : "false";
  j += ",\"quiet\":";      j += quietOn ? "true" : "false";
  j += ",\"praxis\":";     j += praxisOn ? "true" : "false";
  j += ",\"incoming\":";   j += incomingOn ? "true" : "false";
  j += ",\"door\":";       j += doorOpen ? "true" : "false";
  j += ",\"dooralert\":";  j += doorAlerted ? "true" : "false";
  j += ",\"seconds\":";    j += buzzerSeconds;
  j += ",\"rings\":";      j += signalCount;
  j += ",\"openings\":";   j += buzzerTriggers;
  j += ",\"calls\":";      j += callCount;
  j += ",\"last\":\"";     j += jsonEsc(lastEventText()); j += '"';
  j += "}";
  return j;
}

// Klingel-Ereignis fuer Automationen (Event-Entitaet, nicht retained)
void mqttRing() {
  if (mqtt.connected())
    mqtt.publish((baseTopic + "/doorbell").c_str(), "{\"event_type\":\"ring\"}");
  mqttStateDue = true;
}

// Jedes Protokoll-Ereignis auch per MQTT (Topic .../event, nicht retained)
void mqttEvent(EventType type, const String &detail) {
  mqttStateDue = true;
  if (!mqtt.connected()) return;
  String j = "{\"type\":" + String(type) + ",\"event\":\"" + eventName(type) +
             "\",\"detail\":\"" + jsonEsc(detail) + "\"}";
  mqtt.publish((baseTopic + "/event").c_str(), j.c_str());
}

static bool isOn(const String &p) { return p == "ON" || p == "1" || p == "true"; }

static void mqttCallback(char *topic, byte *payload, unsigned int len) {
  String t(topic);
  String p;
  p.reserve(len);
  for (unsigned int i = 0; i < len; i++) p += (char)payload[i];

  if (t == MQTT_DISCOVERY_PREFIX "/status") {        // HA neu gestartet
    if (p == "online") mqttDiscoveryDue = true;
    return;
  }
  if (len == 0) return;   // geloeschte retained-Nachricht
  if (millis() - connectedAt < COMMAND_GRACE_MS) {
    logMsg("MQTT: Befehl kurz nach dem Verbinden ignoriert (%s)", topic);
    return;
  }

  if (t == baseTopic + "/open/set") {
    // HA-Button sendet "PRESS"; beliebige Nachrichten oeffnen NICHT
    if (p == "PRESS" || p == "OPEN" || p == "UNLOCK" || p == "ON" || p == "1") {
      startBuzzer(EV_OPEN_HA);
      aSip.Hangup();   // wie in der Weboberflaeche: Tuer auf -> Anruf beenden
    }
  } else if (t == baseTopic + "/open2/set") {
    if (p == "PRESS" || p == "OPEN" || p == "ON" || p == "1") startRelay2("Home Assistant");
  } else if (t == baseTopic + "/lock/set") {
    if (p == "UNLOCK" || p == "OPEN") {
      startBuzzer(EV_OPEN_HA);
      aSip.Hangup();
    } else if (p == "LOCK" && buzzerActive) {
      stopBuzzer();
    }
  } else if (t == baseTopic + "/callonring/set") {
    callOnRing = isOn(p);
    saveSettings();
  } else if (t == baseTopic + "/quiet/set") {
    quietOn = isOn(p);
    saveSettings();
  } else if (t == baseTopic + "/praxis/set") {
    praxisOn = isOn(p);
    saveSettings();
  } else if (t == baseTopic + "/incoming/set") {
    incomingOn = isOn(p);
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

void mqttBegin() {
  // Eindeutige Kennung aus den letzten 3 MAC-Bytes
  String mac = netMac();
  mac.replace(":", "");
  mac.toLowerCase();
  devId     = "tueroeffner_" + mac.substring(6);
  baseTopic = "tueroeffner/" + mac.substring(6);
  mqttNet.setTimeout(MQTT_CONNECT_TIMEOUT_MS);
  mqtt.setBufferSize(1024);     // Discovery-Nachrichten sind laenger als die 256-Byte-Voreinstellung
  mqtt.setSocketTimeout(2);     // max. Wartezeit auf CONNACK (s)
  mqtt.setCallback(mqttCallback);
  initMqtt();
}

// Broker-Einstellungen uebernehmen (Start und nach Aenderung im Web)
void initMqtt() {
  if (mqtt.connected()) {
    mqtt.publish((baseTopic + "/status").c_str(), "offline", true);
    mqtt.disconnect();
  }
  size_t n = mqttServer.length() < sizeof(mqttServerBuf) ? mqttServer.length() : sizeof(mqttServerBuf) - 1;
  memcpy(mqttServerBuf, mqttServer.c_str(), n);
  mqttServerBuf[n] = 0;
  mqtt.setServer(mqttServerBuf, mqttPort);
  mqttRetryMs = 5000;
  mqttLastTry = millis() - mqttRetryMs;   // sofort verbinden
}

void mqttLoop() {
  if (mqttServerBuf[0] == 0) return;   // kein Broker eingestellt -> MQTT aus

  if (!mqtt.connected()) {
    // Verbindungsaufbau blockiert kurz -> nicht waehrend eines Anrufs oder Summens
    if (aSip.IsBusy() || buzzerActive || !netUp()) return;
    if (millis() - mqttLastTry < mqttRetryMs) return;
    mqttLastTry = millis();

    String will = baseTopic + "/status";
    bool ok = mqtt.connect(devId.c_str(),
                           mqttUser.length() ? mqttUser.c_str() : nullptr,
                           mqttPw.length()   ? mqttPw.c_str()   : nullptr,
                           will.c_str(), 0, true, "offline");
    if (!ok) {
      logMsg("MQTT: Verbindung fehlgeschlagen (rc=%d)", mqtt.state());
      mqttRetryMs = min<uint32_t>(mqttRetryMs * 2, 60000);
      return;
    }
    logMsg("MQTT: verbunden");
    connectedAt = millis();
    mqttRetryMs = 5000;
    mqtt.publish(will.c_str(), "online", true);
    mqtt.subscribe((baseTopic + "/+/set").c_str());
    mqtt.subscribe(MQTT_DISCOVERY_PREFIX "/status");
    // Versehentlich gespeicherte (retained) Befehle loeschen
    for (const char *c : COMMANDS)
      mqtt.publish((baseTopic + "/" + c + "/set").c_str(), "", true);
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

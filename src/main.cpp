#include <Arduino.h>
#if defined(ESP32)
  #include <WiFi.h>
  #include <WebServer.h>
  #include <ESPmDNS.h>
  #include <Update.h>
  #include <esp_timer.h>
  #include <esp_task_wdt.h>
  #include <esp_system.h>
  #include <esp_tls.h>          // HTTPS ueber ESP-IDF (mit Zertifikatsbuendel)
  #include <esp_crt_bundle.h>
#else
  #include <ESP8266WiFi.h>
  #include <ESP8266WebServer.h>
  #include <ESP8266mDNS.h>
  #include <WiFiClientSecure.h> // BearSSL
  #include <Updater.h>
#endif
#include <ArduinoOTA.h>
#include <time.h>
#include <WiFiManager.h>
#include <EEPROM.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoSIP.h>
#include <PubSubClient.h>

#include "config.h"

#define FW_VERSION "1.3.0"

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

String   dialList          = DEFAULT_DIAL_NR;  // Rufkette, z.B. "100, 101"
String   dtmfPin           = "";               // Oeffnungs-Code am Telefon (leer = '*')
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
uint32_t lastSignalOffAt   = 0;      // millis, als das Signal zuletzt endete
bool     hasDisplay        = false;  // OLED beim I2C-Scan gefunden?
bool     displayDirty      = true;
uint32_t lastActivityAt    = 0;      // fuer Bildschirmschoner
bool     displayOn         = true;

// Zugang zur Weboberflaeche (leeres Passwort = kein Schutz).
// Admin darf alles, der Bedien-Zugang nur oeffnen und den Verlauf sehen.
String   webUser           = "admin";
String   webPw             = "";
String   opUser            = "tuer";
String   opPw              = "";

// Nachtruhe: in diesem Zeitfenster beim Klingeln nicht anrufen
bool     quietOn           = false;
uint16_t quietFrom         = 22 * 60;   // Minuten seit Mitternacht
uint16_t quietTo           = 7 * 60;

// Praxis-Modus: in diesem Zeitfenster oeffnet Klingeln die Tuer automatisch
bool     praxisOn          = false;
uint8_t  praxisDays        = 0x1F;      // Bit 0 = Montag ... Bit 6 = Sonntag
uint16_t praxisFrom        = 8 * 60;
uint16_t praxisTo          = 12 * 60;

// Tuerkontakt
bool     doorOn            = false;     // Kontakt angeschlossen?
bool     doorInvert        = false;     // Kontakt umgekehrt (offen = geschlossen)
uint16_t doorAlertMin      = 0;         // Meldung, wenn so lange offen (0 = aus)
bool     doorOpen          = false;
uint32_t doorOpenedAt      = 0;
bool     doorAlerted       = false;

// Push-Mitteilungen
enum PushType : uint8_t { PUSH_OFF, PUSH_NTFY, PUSH_TELEGRAM };
static const uint8_t PUSH_EV_RING = 1, PUSH_EV_OPEN = 2, PUSH_EV_DOOR = 4;
uint8_t  pushType          = PUSH_OFF;
uint8_t  pushEvents        = PUSH_EV_RING | PUSH_EV_DOOR;
String   pushServer        = "https://ntfy.sh";
String   pushTopic         = "";        // ntfy-Topic bzw. Telegram-Chat-ID
String   pushToken         = "";        // ntfy-Token (optional) bzw. Telegram-Bot-Token
volatile int lastPushCode  = 0;         // HTTP-Status der letzten Mitteilung (-1 = Fehler)

// Netzwerk: feste IP statt DHCP
bool      staticIp         = false;
IPAddress ipAddr, ipGw, ipMask(255, 255, 255, 0), ipDns;

// Zaehler verzoegert speichern (Flash schonen)
bool     countersDirty     = false;
uint32_t lastSettingsSave  = 0;

// Geplanter Neustart / WLAN-Reset (erst nach dem Senden der HTTP-Antwort)
uint32_t restartAt         = 0;
uint8_t  restartReason     = 0;
bool     wifiResetDue      = false;

// Rufkette: Index der gerade angerufenen Nummer, -1 = keine Kette aktiv
int8_t   chainIdx          = -1;
String   lastCallNr;
String   dtmfBuf;                       // eingegebene Ziffern fuer den Code

// Selbstheilung
uint32_t wifiLostAt        = 0;
String   bootReason;

// Ereignisprotokoll (Ringpuffer). Neue Typen nur HINTEN anhaengen (Weboberflaeche).
enum EventType : uint8_t {
  EV_RING, EV_RING_QUIET, EV_CALL, EV_OPEN_WEB, EV_OPEN_BTN, EV_OPEN_PHONE, EV_OPEN_HA, EV_BOOT,
  EV_OPEN_AUTO, EV_NOANSWER, EV_DOOR_ALERT
};
struct LogEntry {
  uint32_t  at;      // millis()
  time_t    t;       // Unix-Zeit, 0 = Uhr war noch nicht gestellt
  EventType type;
};
LogEntry eventLog[LOG_SIZE];
uint8_t  logHead           = 0;       // naechster Schreibplatz
uint8_t  logCount          = 0;
uint32_t logTotal          = 0;       // fuer die Weboberflaeche: hat sich etwas getan?

// ------------------------------------------------------------
//  EEPROM Persistenz (als Struct)
// ------------------------------------------------------------
static const uint8_t SETTINGS_MAGIC = 0x53;   // aendern, wenn sich Layout aendert
static const uint8_t MQTT_MAGIC     = 0x4D;   // Kennung fuer den angehaengten MQTT-Block
static const uint8_t EXT_MAGIC      = 0x45;   // Kennung fuer Zaehler/Zugang/Nachtruhe
static const uint8_t EXT2_MAGIC     = 0x32;   // Kennung fuer Rufkette/Praxis/Push/Netz
struct Settings {
  uint8_t  magic;
  uint8_t  buzzerSeconds;
  uint8_t  callOnRing;
  uint16_t sipPort;
  char     dialNr[MAX_DIAL_LEN + 1];   // alt; enthaelt nur noch den Anfang der Rufkette
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
  // Dritter Block, ebenfalls hinten angehaengt
  uint8_t  extMagic;
  uint32_t signalCount;
  uint32_t buzzerTriggers;
  uint32_t callCount;
  char     webUser[25];
  char     webPw[33];
  uint8_t  quietOn;
  uint16_t quietFrom;
  uint16_t quietTo;
  // Vierter Block
  uint8_t  ext2Magic;
  char     dialList[MAX_DIAL_LIST + 1];
  char     dtmfPin[9];
  char     opUser[25];
  char     opPw[33];
  uint8_t  praxisOn;
  uint8_t  praxisDays;
  uint16_t praxisFrom;
  uint16_t praxisTo;
  uint8_t  doorOn;
  uint8_t  doorInvert;
  uint16_t doorAlertMin;
  uint8_t  pushType;
  uint8_t  pushEvents;
  char     pushServer[65];
  char     pushTopic[65];
  char     pushToken[65];
  uint8_t  staticIp;
  uint32_t ipAddr;
  uint32_t ipGw;
  uint32_t ipMask;
  uint32_t ipDns;
};
static const uint16_t EEPROM_SIZE = sizeof(Settings) + 8;

static void copyToField(char *dst, size_t dstSize, const String &src) {
  size_t n = src.length();
  if (n > dstSize - 1) n = dstSize - 1;
  memcpy(dst, src.c_str(), n);
  dst[n] = '\0';
}

// Zeichenkette aus dem EEPROM sicher abschliessen und uebernehmen
#define LOAD_STR(field, var) do { s.field[sizeof(s.field) - 1] = '\0'; var = String(s.field); } while (0)

void loadSettings() {
  EEPROM.begin(EEPROM_SIZE);
  Settings s;
  EEPROM.get(0, s);
  if (s.magic != SETTINGS_MAGIC) return;   // nichts gespeichert -> Defaults behalten
  if (s.buzzerSeconds >= MIN_BUZZER_SECONDS && s.buzzerSeconds <= MAX_BUZZER_SECONDS)
    buzzerSeconds = s.buzzerSeconds;
  callOnRing = s.callOnRing != 0;
  if (s.sipPort > 0) sipPort = s.sipPort;
  LOAD_STR(dialNr,    dialList);   // Fallback, falls Block 4 noch fehlt
  LOAD_STR(sipServer, sipServer);
  LOAD_STR(sipUser,   sipUser);
  LOAD_STR(sipPw,     sipPw);

  if (s.mqttMagic != MQTT_MAGIC) return;   // Einstellungen von vor dem MQTT-Update
  if (s.mqttPort > 0) mqttPort = s.mqttPort;
  LOAD_STR(mqttServer, mqttServer);
  LOAD_STR(mqttUser,   mqttUser);
  LOAD_STR(mqttPw,     mqttPw);

  if (s.extMagic != EXT_MAGIC) return;     // Einstellungen von vor Version 1.2
  signalCount    = s.signalCount;
  buzzerTriggers = s.buzzerTriggers;
  callCount      = s.callCount;
  s.webUser[sizeof(s.webUser) - 1] = '\0';
  if (s.webUser[0]) webUser = String(s.webUser);
  LOAD_STR(webPw, webPw);
  quietOn = s.quietOn != 0;
  if (s.quietFrom < 24 * 60) quietFrom = s.quietFrom;
  if (s.quietTo   < 24 * 60) quietTo   = s.quietTo;

  if (s.ext2Magic != EXT2_MAGIC) return;   // Einstellungen von vor Version 1.3
  LOAD_STR(dialList, dialList);
  LOAD_STR(dtmfPin,  dtmfPin);
  s.opUser[sizeof(s.opUser) - 1] = '\0';
  if (s.opUser[0]) opUser = String(s.opUser);
  LOAD_STR(opPw, opPw);
  praxisOn   = s.praxisOn != 0;
  praxisDays = s.praxisDays & 0x7F;
  if (s.praxisFrom < 24 * 60) praxisFrom = s.praxisFrom;
  if (s.praxisTo   < 24 * 60) praxisTo   = s.praxisTo;
  doorOn       = s.doorOn != 0;
  doorInvert   = s.doorInvert != 0;
  doorAlertMin = s.doorAlertMin;
  if (s.pushType <= PUSH_TELEGRAM) pushType = s.pushType;
  pushEvents = s.pushEvents;
  LOAD_STR(pushServer, pushServer);
  LOAD_STR(pushTopic,  pushTopic);
  LOAD_STR(pushToken,  pushToken);
  staticIp = s.staticIp != 0;
  ipAddr = IPAddress(s.ipAddr);
  ipGw   = IPAddress(s.ipGw);
  ipMask = IPAddress(s.ipMask);
  ipDns  = IPAddress(s.ipDns);
}

void saveSettings() {
  Settings s;
  memset(&s, 0, sizeof(s));
  s.magic         = SETTINGS_MAGIC;
  s.buzzerSeconds = buzzerSeconds;
  s.callOnRing    = callOnRing ? 1 : 0;
  s.sipPort       = sipPort;
  copyToField(s.dialNr,    sizeof(s.dialNr),    dialList);
  copyToField(s.sipServer, sizeof(s.sipServer), sipServer);
  copyToField(s.sipUser,   sizeof(s.sipUser),   sipUser);
  copyToField(s.sipPw,     sizeof(s.sipPw),     sipPw);
  s.mqttMagic     = MQTT_MAGIC;
  s.mqttPort      = mqttPort;
  copyToField(s.mqttServer, sizeof(s.mqttServer), mqttServer);
  copyToField(s.mqttUser,   sizeof(s.mqttUser),   mqttUser);
  copyToField(s.mqttPw,     sizeof(s.mqttPw),     mqttPw);
  s.extMagic       = EXT_MAGIC;
  s.signalCount    = signalCount;
  s.buzzerTriggers = buzzerTriggers;
  s.callCount      = callCount;
  copyToField(s.webUser, sizeof(s.webUser), webUser);
  copyToField(s.webPw,   sizeof(s.webPw),   webPw);
  s.quietOn        = quietOn ? 1 : 0;
  s.quietFrom      = quietFrom;
  s.quietTo        = quietTo;
  s.ext2Magic      = EXT2_MAGIC;
  copyToField(s.dialList, sizeof(s.dialList), dialList);
  copyToField(s.dtmfPin,  sizeof(s.dtmfPin),  dtmfPin);
  copyToField(s.opUser,   sizeof(s.opUser),   opUser);
  copyToField(s.opPw,     sizeof(s.opPw),     opPw);
  s.praxisOn       = praxisOn ? 1 : 0;
  s.praxisDays     = praxisDays;
  s.praxisFrom     = praxisFrom;
  s.praxisTo       = praxisTo;
  s.doorOn         = doorOn ? 1 : 0;
  s.doorInvert     = doorInvert ? 1 : 0;
  s.doorAlertMin   = doorAlertMin;
  s.pushType       = pushType;
  s.pushEvents     = pushEvents;
  copyToField(s.pushServer, sizeof(s.pushServer), pushServer);
  copyToField(s.pushTopic,  sizeof(s.pushTopic),  pushTopic);
  copyToField(s.pushToken,  sizeof(s.pushToken),  pushToken);
  s.staticIp       = staticIp ? 1 : 0;
  s.ipAddr         = (uint32_t)ipAddr;
  s.ipGw           = (uint32_t)ipGw;
  s.ipMask         = (uint32_t)ipMask;
  s.ipDns          = (uint32_t)ipDns;
  EEPROM.put(0, s);
  EEPROM.commit();
  countersDirty    = false;
  lastSettingsSave = millis();
}

// Zaehler haben sich geaendert -> spaeter in loop() speichern
void countersChanged() {
  countersDirty = true;
}

// ------------------------------------------------------------
//  Neustart mit Grund (ueberlebt den Neustart im RTC-Speicher)
// ------------------------------------------------------------
enum RestartReason : uint8_t {
  RR_NONE, RR_USER, RR_UPDATE, RR_WIFI_LOST, RR_LOW_HEAP, RR_WIFI_RESET, RR_RESTORE, RR_NETWORK
};
static const uint32_t RR_MAGIC = 0xA5C30000;

#if defined(ESP32)
RTC_NOINIT_ATTR uint32_t rtcReason;
void writeRestartReason(uint8_t r) { rtcReason = RR_MAGIC | r; }
uint8_t readRestartReason() {
  uint32_t v = rtcReason;
  rtcReason = 0;
  return (v & 0xFFFF0000) == RR_MAGIC ? (uint8_t)(v & 0xFF) : RR_NONE;
}
#else
static const uint32_t RTC_REASON_BLOCK = 64;   // hinter dem vom OTA genutzten Bereich
void writeRestartReason(uint8_t r) {
  uint32_t v = RR_MAGIC | r;
  ESP.rtcUserMemoryWrite(RTC_REASON_BLOCK, &v, sizeof(v));
}
uint8_t readRestartReason() {
  uint32_t v = 0;
  ESP.rtcUserMemoryRead(RTC_REASON_BLOCK, &v, sizeof(v));
  uint32_t zero = 0;
  ESP.rtcUserMemoryWrite(RTC_REASON_BLOCK, &zero, sizeof(zero));
  return (v & 0xFFFF0000) == RR_MAGIC ? (uint8_t)(v & 0xFF) : RR_NONE;
}
#endif

// Warum ist das Geraet zuletzt gestartet? (fuer die Weboberflaeche)
String describeBootReason() {
  switch (readRestartReason()) {
    case RR_USER:       return "Neustart über die Weboberfläche";
    case RR_UPDATE:     return "Firmware-Update";
    case RR_WIFI_LOST:  return "Selbstheilung: WLAN war weg";
    case RR_LOW_HEAP:   return "Selbstheilung: Speicher knapp";
    case RR_WIFI_RESET: return "WLAN zurückgesetzt";
    case RR_RESTORE:    return "Sicherung eingespielt";
    case RR_NETWORK:    return "Netzwerk geändert";
    default: break;
  }
#if defined(ESP32)
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "Einschalten";
    case ESP_RST_EXT:      return "Reset-Taste";
    case ESP_RST_SW:       return "Software-Neustart";
    case ESP_RST_PANIC:    return "Absturz";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "Watchdog (Programm hing)";
    case ESP_RST_BROWNOUT: return "Unterspannung";
    default:               return "unbekannt";
  }
#else
  switch (ESP.getResetInfoPtr()->reason) {
    case REASON_DEFAULT_RST:      return "Einschalten";
    case REASON_EXT_SYS_RST:      return "Reset-Taste";
    case REASON_SOFT_RESTART:     return "Software-Neustart";
    case REASON_EXCEPTION_RST:    return "Absturz";
    case REASON_WDT_RST:
    case REASON_SOFT_WDT_RST:     return "Watchdog (Programm hing)";
    default:                      return "unbekannt";
  }
#endif
}

void stopBuzzer();

void restartNow(uint8_t reason) {
  stopBuzzer();
  if (countersDirty) saveSettings();
  writeRestartReason(reason);
  delay(50);
  ESP.restart();
}

void feedWatchdog() {
#if defined(ESP32)
  esp_task_wdt_reset();
#else
  ESP.wdtFeed();
#endif
}

// ------------------------------------------------------------
//  Uhrzeit, Protokoll, Zeitfenster
// ------------------------------------------------------------
bool timeValid() {
  return time(nullptr) > 1600000000;   // nach NTP-Abgleich (vorher ~1970)
}

// Laufzeit in Sekunden (64 Bit, laeuft nicht nach 49 Tagen ueber)
uint32_t uptimeSeconds() {
#if defined(ESP32)
  return (uint32_t)(esp_timer_get_time() / 1000000ULL);
#else
  return (uint32_t)(micros64() / 1000000ULL);
#endif
}

void logEvent(EventType type) {
  LogEntry &e = eventLog[logHead];
  e.at   = millis();
  e.t    = timeValid() ? time(nullptr) : 0;
  e.type = type;
  logHead = (logHead + 1) % LOG_SIZE;
  if (logCount < LOG_SIZE) logCount++;
  logTotal++;
}

// Liegt die aktuelle Uhrzeit im Fenster from..to (Minuten) an einem der Tage
// (Bit 0 = Montag)? Ohne gueltige Uhrzeit: nein.
bool inWindow(uint16_t from, uint16_t to, uint8_t days) {
  if (!timeValid()) return false;
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  if (!(days & (1 << ((lt.tm_wday + 6) % 7)))) return false;   // tm_wday: 0 = Sonntag
  uint16_t m = lt.tm_hour * 60 + lt.tm_min;
  if (from <= to) return m >= from && m < to;
  return m >= from || m < to;   // ueber Mitternacht, z.B. 22:00-07:00
}

bool isQuiet()  { return quietOn  && inWindow(quietFrom,  quietTo,  0x7F); }
bool isPraxis() { return praxisOn && inWindow(praxisFrom, praxisTo, praxisDays); }

// ------------------------------------------------------------
//  Push-Mitteilungen (ntfy / Telegram)
//  ESP32: eigener Task, blockiert die Loop nicht.
//  ESP8266: wird in der Loop gesendet, wenn kein Anruf laeuft.
// ------------------------------------------------------------
struct PushMsg {
  uint8_t type;
  char    server[65];
  char    topic[65];
  char    token[65];
  char    text[120];
};

String urlEncode(const char *s) {
  static const char hex[] = "0123456789ABCDEF";
  String r;
  for (; *s; s++) {
    uint8_t c = (uint8_t)*s;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') r += (char)c;
    else { r += '%'; r += hex[c >> 4]; r += hex[c & 15]; }
  }
  return r;
}

// "https://host:port/pfad" zerlegen
bool splitUrl(const String &url, bool &https, String &host, uint16_t &port, String &path) {
  int p;
  if (url.startsWith("https://"))     { https = true;  p = 8; port = 443; }
  else if (url.startsWith("http://")) { https = false; p = 7; port = 80; }
  else return false;
  int slash = url.indexOf('/', p);
  if (slash < 0) slash = url.length();
  host = url.substring(p, slash);
  path = slash < (int)url.length() ? url.substring(slash) : String("/");
  int colon = host.indexOf(':');
  if (colon >= 0) {
    port = (uint16_t)host.substring(colon + 1).toInt();
    host = host.substring(0, colon);
  }
  return host.length() > 0 && port > 0;
}

// HTTP-Statuscode aus "HTTP/1.1 200 OK" lesen
int parseStatus(const char *line) {
  return strncmp(line, "HTTP/", 5) == 0 && strchr(line, ' ') ? atoi(strchr(line, ' ') + 1) : -1;
}

// Kleiner HTTP(S)-POST; liefert den Statuscode oder -1. Blockiert bis PUSH_TIMEOUT_MS.
int httpPost(const String &url, const String &headers, const String &body) {
  bool https;
  String host, path;
  uint16_t port;
  if (!splitUrl(url, https, host, port, path)) return -1;
  String req = "POST " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n" + headers +
               "Content-Length: " + String(body.length()) + "\r\n\r\n" + body;
  char line[64] = {0};
#if defined(ESP32)
  if (https) {
    esp_tls_cfg_t cfg = {};
    cfg.crt_bundle_attach = esp_crt_bundle_attach;   // Server-Zertifikat pruefen
    cfg.timeout_ms        = PUSH_TIMEOUT_MS;
    esp_tls_t *tls = esp_tls_init();
    if (!tls) return -1;
    int code = -1;
    if (esp_tls_conn_new_sync(host.c_str(), host.length(), port, &cfg, tls) == 1) {
      const char *p = req.c_str();
      size_t left = req.length();
      while (left > 0) {
        int n = esp_tls_conn_write(tls, p, left);
        if (n <= 0) break;
        p += n; left -= n;
      }
      if (left == 0) {
        int n = esp_tls_conn_read(tls, line, sizeof(line) - 1);
        if (n > 0) { line[n] = 0; code = parseStatus(line); }
      }
    }
    esp_tls_conn_destroy(tls);
    return code;
  }
  WiFiClient c;
  if (!c.connect(host.c_str(), port, PUSH_TIMEOUT_MS)) return -1;
#else
  BearSSL::WiFiClientSecure tls;
  WiFiClient plain;
  WiFiClient *cp = &plain;
  if (https) { tls.setInsecure(); cp = &tls; }   // BearSSL: kein Zertifikatsspeicher
  WiFiClient &c = *cp;
  c.setTimeout(PUSH_TIMEOUT_MS);
  if (!c.connect(host.c_str(), port)) return -1;
#endif
  c.print(req);
  uint32_t t0 = millis();
  while (!c.available() && c.connected() && millis() - t0 < PUSH_TIMEOUT_MS) delay(10);
  size_t n = c.readBytesUntil('\n', line, sizeof(line) - 1);
  line[n] = 0;
  c.stop();
  return parseStatus(line);
}

void sendPushNow(const PushMsg &m) {
  if (WiFi.status() != WL_CONNECTED) { lastPushCode = -1; return; }
  String url, headers, body;
  if (m.type == PUSH_NTFY) {
    url = m.server;
    if (!url.endsWith("/")) url += "/";
    url += m.topic;
    headers = "Content-Type: text/plain; charset=utf-8\r\nTitle: Tueroeffner\r\nTags: bell\r\n";
    if (m.token[0]) headers += String("Authorization: Bearer ") + m.token + "\r\n";
    body = m.text;
  } else {
    url     = String("https://api.telegram.org/bot") + m.token + "/sendMessage";
    headers = "Content-Type: application/x-www-form-urlencoded\r\n";
    body    = "chat_id=" + urlEncode(m.topic) + "&text=" + urlEncode(m.text);
  }
  lastPushCode = httpPost(url, headers, body);
  Serial.printf("Push: HTTP %d\n", lastPushCode);
}

#if defined(ESP32)
QueueHandle_t pushQueue = nullptr;

void pushTask(void *) {
  PushMsg m;
  for (;;) {
    if (xQueueReceive(pushQueue, &m, portMAX_DELAY) == pdTRUE) sendPushNow(m);
  }
}
#else
PushMsg pendingPush;
bool    pendingPushDue = false;
#endif

// Mitteilung einreihen; ev = PUSH_EV_* (0 = immer, z.B. Test)
void queuePush(uint8_t ev, const String &text) {
  if (pushType == PUSH_OFF || pushTopic.length() == 0) return;
  if (ev && !(pushEvents & ev)) return;
  PushMsg m;
  m.type = pushType;
  copyToField(m.server, sizeof(m.server), pushServer);
  copyToField(m.topic,  sizeof(m.topic),  pushTopic);
  copyToField(m.token,  sizeof(m.token),  pushToken);
  copyToField(m.text,   sizeof(m.text),   text);
#if defined(ESP32)
  if (pushQueue) xQueueSend(pushQueue, &m, 0);
#else
  pendingPush    = m;
  pendingPushDue = true;
#endif
}

// ESP8266: wartende Mitteilung senden (nicht waehrend eines Anrufs)
void pushLoop() {
#if !defined(ESP32)
  if (pendingPushDue && !aSip.IsBusy()) {
    pendingPushDue = false;
    sendPushNow(pendingPush);
  }
#endif
}

// ------------------------------------------------------------
//  SIP
// ------------------------------------------------------------
// Mit Oeffnungs-Code bleibt das Gespraech laenger offen, damit man ihn tippen kann
void applyCallSeconds() {
  aSip.SetCallSeconds(dtmfPin.length() ? SIP_PIN_CALL_SECONDS : 0);
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
  applyCallSeconds();
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
  Serial.printf("SIP-REGISTER: %s (%d)\n", aSip.IsRegistered() ? "OK" : "fehlgeschlagen",
                aSip.RegisterStatus());
}

// n-te Nummer der Rufkette ("100, 101; 102") -> false, wenn es sie nicht gibt
bool dialTarget(uint8_t n, String &out) {
  uint8_t idx = 0;
  int i = 0, len = dialList.length();
  while (i < len) {
    while (i < len && (dialList[i] == ',' || dialList[i] == ';' || dialList[i] == ' ')) i++;
    int start = i;
    while (i < len && dialList[i] != ',' && dialList[i] != ';' && dialList[i] != ' ') i++;
    if (i > start) {
      if (idx == n) { out = dialList.substring(start, i); return true; }
      idx++;
    }
  }
  return false;
}

// Rufkette pruefen: jede Nummer hoechstens MAX_DIAL_LEN Zeichen
bool dialListValid(const String &list) {
  if (list.length() > MAX_DIAL_LIST) return false;
  String saved = dialList, nr;
  dialList = list;
  bool ok = true;
  for (uint8_t i = 0; dialTarget(i, nr); i++)
    if (nr.length() > MAX_DIAL_LEN) ok = false;
  dialList = saved;
  return ok;
}

void markActivity();

// Eine Nummer der Rufkette anrufen (nur Signalisierung + Beep)
bool dialIndex(uint8_t idx) {
  String nr;
  if (!dialTarget(idx, nr)) return false;
  copyToField(dialBuf, sizeof(dialBuf), nr);
  if (!aSip.Dial(dialBuf, SIP_CALLER_NAME)) return false;   // Anruf laeuft schon
  lastCallNr = nr;
  dtmfBuf    = "";
  callCount++;
  countersChanged();
  logEvent(EV_CALL);
  markActivity();
  return true;
}

// Rufkette von vorne starten
bool startChain() {
  if (aSip.IsBusy() || !dialIndex(0)) return false;
  chainIdx = 0;
  return true;
}

void stopChain() {
  chainIdx = -1;
}

// Nach jedem Anruf: angenommen -> fertig, sonst (nach kurzer Pause) naechste Nummer
void chainLoop() {
  static uint32_t idleSince = 0;
  if (chainIdx < 0 || aSip.IsBusy()) { idleSince = 0; return; }
  if (aSip.LastCallResult() == Sip::CALL_ANSWERED) { stopChain(); return; }
  if (!idleSince) idleSince = millis();
  if (millis() - idleSince < 1000) return;   // Anlage den vorigen Anruf abschliessen lassen
  idleSince = 0;
  String nr;
  if (!dialTarget(chainIdx + 1, nr)) {   // Ende der Kette, niemand hat abgenommen
    logEvent(EV_NOANSWER);
    stopChain();
    return;
  }
  chainIdx++;
  if (!dialIndex(chainIdx)) stopChain();
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

const char *openSourceName(EventType source) {
  switch (source) {
    case EV_OPEN_WEB:   return "Web";
    case EV_OPEN_BTN:   return "Taster";
    case EV_OPEN_PHONE: return "Telefon";
    case EV_OPEN_HA:    return "Home Assistant";
    default:            return "automatisch";
  }
}

// Tuer oeffnen; source = EV_OPEN_* (wer hat geoeffnet, fuers Protokoll)
void startBuzzer(EventType source) {
  stopChain();   // Tuer ist auf -> niemanden mehr anrufen
  buzzerActive = true;
  buzzerOffAt  = millis() + (uint32_t)buzzerSeconds * 1000UL;
  buzzerTriggers++;
  countersChanged();
  logEvent(source);
  setRelay(true);
  markActivity();
  displayDirty = true;
  if (source != EV_OPEN_AUTO)
    queuePush(PUSH_EV_OPEN, String("Tür geöffnet (") + openSourceName(source) + ")");
}

void stopBuzzer() {
  buzzerActive = false;
  setRelay(false);
  displayDirty = true;
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
  if (doorOn) {
    display.print(F("Tuer: "));
    display.println(doorOpen ? F("OFFEN") : F("zu"));
  } else {
    display.print(F("Dauer: "));
    display.print(buzzerSeconds);
    display.println(F("s"));
  }

  display.display();
}

// ------------------------------------------------------------
//  Home Assistant (MQTT mit Auto-Discovery)
// ------------------------------------------------------------
String discoveryTopic(const char *component, const char *object) {
  return String(MQTT_DISCOVERY_PREFIX) + "/" + component + "/" + devId + "/" + object + "/config";
}

// Eine Entitaet per Discovery anlegen; cfg = entitaetsspezifische JSON-Felder
void mqttDiscover(const char *component, const char *object, const String &cfg) {
  String p = "{" + cfg;
  p += ",\"unique_id\":\"" + devId + "_" + object + "\"";
  p += ",\"availability_topic\":\"" + baseTopic + "/status\"";
  p += ",\"device\":{\"identifiers\":[\"" + devId + "\"],\"name\":\"Türöffner\","
       "\"manufacturer\":\"DIY\",\"model\":\"ESP SIP-Türöffner\",\"sw_version\":\"" FW_VERSION "\","
       "\"configuration_url\":\"http://" + WiFi.localIP().toString() + "\"}}";
  mqtt.publish(discoveryTopic(component, object).c_str(), p.c_str(), true);
}

// Entitaet wieder entfernen (leere Konfiguration)
void mqttForget(const char *component, const char *object) {
  mqtt.publish(discoveryTopic(component, object).c_str(), "", true);
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
  mqttDiscover("switch", "quiet",
    "\"name\":\"Nachtruhe\",\"icon\":\"mdi:sleep\",\"entity_category\":\"config\"" + st + cmd +
    "quiet/set\",\"value_template\":\"{{ 'ON' if value_json.quiet else 'OFF' }}\"");
  mqttDiscover("switch", "praxis",
    "\"name\":\"Praxis-Modus\",\"icon\":\"mdi:door-sliding-open\",\"entity_category\":\"config\"" + st + cmd +
    "praxis/set\",\"value_template\":\"{{ 'ON' if value_json.praxis else 'OFF' }}\"");
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
  if (doorOn) {
    mqttDiscover("binary_sensor", "door",
      "\"name\":\"Tür\",\"device_class\":\"door\"" + st +
      ",\"value_template\":\"{{ 'ON' if value_json.door else 'OFF' }}\"");
    mqttDiscover("binary_sensor", "dooralert",
      "\"name\":\"Tür zu lange offen\",\"device_class\":\"problem\"" + st +
      ",\"value_template\":\"{{ 'ON' if value_json.dooralert else 'OFF' }}\"");
  } else {
    mqttForget("binary_sensor", "door");
    mqttForget("binary_sensor", "dooralert");
  }
}

String mqttStateJson() {
  String j = "{\"ringing\":";
  j += isRinging() ? "true" : "false";
  j += ",\"buzzer\":";     j += buzzerActive ? "true" : "false";
  j += ",\"sip\":";        j += aSip.IsRegistered() ? "true" : "false";
  j += ",\"callonring\":"; j += callOnRing ? "true" : "false";
  j += ",\"quiet\":";      j += quietOn ? "true" : "false";
  j += ",\"praxis\":";     j += praxisOn ? "true" : "false";
  j += ",\"door\":";       j += doorOpen ? "true" : "false";
  j += ",\"dooralert\":";  j += doorAlerted ? "true" : "false";
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
    startBuzzer(EV_OPEN_HA);
    aSip.Hangup();   // wie in der Weboberflaeche: Tuer auf -> Anruf beenden
  } else if (t == baseTopic + "/callonring/set") {
    callOnRing = (p == "ON");
    saveSettings();
  } else if (t == baseTopic + "/quiet/set") {
    quietOn = (p == "ON");
    saveSettings();
  } else if (t == baseTopic + "/praxis/set") {
    praxisOn = (p == "ON");
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
//  Eingaenge (entprellt)
// ------------------------------------------------------------
// Klingeln auswerten: Praxis-Modus oeffnet, sonst Rufkette (ausser Nachtruhe)
void onRing() {
  mqttRing();
  if (isPraxis()) {
    logEvent(EV_RING);
    startBuzzer(EV_OPEN_AUTO);
    queuePush(PUSH_EV_RING, "Es hat geklingelt – Tür automatisch geöffnet (Praxis-Modus)");
    return;
  }
  bool quiet = isQuiet();
  logEvent(quiet ? EV_RING_QUIET : EV_RING);
  queuePush(PUSH_EV_RING, quiet ? "Es klingelt an der Tür (Nachtruhe)" : "Es klingelt an der Tür");
  pushLoop();   // ESP8266: Mitteilung vor dem Anruf raus, sonst erst nach dem Anruf
  if (callOnRing && !quiet) startChain();
}

// Klingel-Taster wirkt wie ein zweiter Kontakt parallel zum Klingelsignal
void handleSignalInput() {
  int raw = digitalRead(PIN_SIGNAL);
  bool pressed = (SIGNAL_ACTIVE_LOW ? (raw == LOW) : (raw == HIGH)) ||
                 digitalRead(PIN_BTN_RING) == LOW;

  if (pressed != lastSignalRaw) {
    lastSignalRaw    = pressed;
    lastSignalChange = millis();
  }

  if ((millis() - lastSignalChange) > SIGNAL_DEBOUNCE_MS) {
    if (pressed != signalActive) {
      signalActive = pressed;
      if (signalActive) {
        // Sturmklingeln: kurz hintereinander nur zaehlen, nicht erneut melden/anrufen
        bool cooldown = lastRingAt != 0 && millis() - lastRingAt < RING_COOLDOWN_MS;
        signalCount++;
        countersChanged();
        lastRingAt = millis();
        markActivity();
        if (!cooldown) onRing();
      } else {
        lastSignalOffAt = millis();
      }
      displayDirty = true;
    }
  }
}

// Summer-Taster (entprellt): jeder Druck oeffnet wie der Web-Button
void handleBuzzerButton() {
  static bool     lastRaw   = false;
  static bool     state     = false;
  static uint32_t changedAt = 0;
  bool pressed = digitalRead(PIN_BTN_BUZZER) == LOW;
  if (pressed != lastRaw) {
    lastRaw   = pressed;
    changedAt = millis();
  }
  if ((millis() - changedAt) > SIGNAL_DEBOUNCE_MS && pressed != state) {
    state = pressed;
    if (state) {
      startBuzzer(EV_OPEN_BTN);
      aSip.Hangup();   // Tuer ist auf -> laufenden Anruf beenden
    }
  }
}

// Tuerkontakt (Reed): geschlossen = LOW. Offen zu lange -> Meldung.
void handleDoorContact() {
  static bool     lastRaw   = false;
  static uint32_t changedAt = 0;
  if (!doorOn) {
    if (doorOpen || doorAlerted) { doorOpen = doorAlerted = false; mqttStateDue = true; }
    return;
  }
  bool open = digitalRead(PIN_DOOR) == HIGH;
  if (doorInvert) open = !open;
  if (open != lastRaw) {
    lastRaw   = open;
    changedAt = millis();
  }
  if ((millis() - changedAt) > SIGNAL_DEBOUNCE_MS && open != doorOpen) {
    doorOpen = open;
    if (open) doorOpenedAt = millis();
    doorAlerted  = false;
    mqttStateDue = true;
    displayDirty = true;
  }
  if (doorOpen && doorAlertMin && !doorAlerted &&
      millis() - doorOpenedAt > (uint32_t)doorAlertMin * 60000UL) {
    doorAlerted = true;
    logEvent(EV_DOOR_ALERT);
    queuePush(PUSH_EV_DOOR, "Die Tür steht seit " + String(doorAlertMin) + " min offen");
    mqttStateDue = true;
  }
}

// Ziffern vom Telefon: ohne Code oeffnet '*', mit Code die richtige Ziffernfolge
void handleDtmf() {
  char d = aSip.ReadDtmf();
  if (!d) return;
  Serial.printf("DTMF empfangen: %c\n", d);
  bool open = false;
  if (dtmfPin.length() == 0) {
    open = d == '*';
  } else if (d == '*' || d == '#') {
    dtmfBuf = "";   // neu anfangen
  } else {
    dtmfBuf += d;
    if (dtmfBuf.length() > 16) dtmfBuf.remove(0, dtmfBuf.length() - 16);
    open = dtmfBuf.endsWith(dtmfPin);
  }
  if (open) {
    dtmfBuf = "";
    startBuzzer(EV_OPEN_PHONE);
    aSip.Hangup();
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
.seg button{flex:1;border:0;background:none;color:var(--mut);padding:10px 4px;border-radius:10px;font:inherit;font-size:13px;font-weight:600;cursor:pointer;white-space:nowrap}
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
.hint{color:var(--mut);font-size:13px;margin-top:10px;min-height:24px;display:flex;align-items:center;text-align:center}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}
.tile{background:var(--tile);border-radius:14px;padding:10px 12px;min-width:0}
.tile small{display:block;color:var(--mut);font-size:11px;text-transform:uppercase;letter-spacing:.04em}
.tile b{font-size:18px;display:block;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.tile b.sm{font-size:14px;line-height:25px}
.on{color:var(--ok)}.off{color:var(--mut)}.warn{color:var(--warn)}.bad{color:var(--bad)}
.row2{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:12px}
.btn{border:0;border-radius:12px;padding:12px;font:inherit;font-weight:600;cursor:pointer;background:var(--tile);color:var(--fg)}
.btn:active{opacity:.7}
.btn.pri{background:var(--acc);color:#fff;width:100%;margin-top:14px}
.btn.full{width:100%;margin-top:8px}
h2{font-size:13px;text-transform:uppercase;letter-spacing:.05em;color:var(--mut);margin:0 0 12px}
h2 b{float:right;text-transform:none}
label{display:block;font-size:13px;color:var(--mut);margin:10px 0 4px}
input[type=text],input[type=number],input[type=password],input[type=time],select{width:100%;padding:11px 12px;border-radius:10px;
 border:1px solid var(--line);background:var(--tile);color:var(--fg);font:inherit}
input:focus,select:focus{outline:2px solid var(--acc);outline-offset:-1px}
input.dirty,select.dirty{border-color:var(--warn)}
.btn.pri.due{box-shadow:0 0 0 3px color-mix(in srgb,var(--warn) 55%,transparent)}
.two{display:grid;grid-template-columns:2fr 1fr;gap:8px}
.half{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.sw{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-top:14px}
.sw input{appearance:none;flex:none;width:46px;height:28px;border-radius:99px;background:var(--line);position:relative;cursor:pointer;transition:.2s;margin:0}
.sw input::after{content:"";position:absolute;top:3px;left:3px;width:22px;height:22px;border-radius:50%;background:#fff;transition:.2s}
.sw input:checked{background:var(--ok)}
.sw input:checked::after{left:21px}
.days{display:flex;gap:4px;margin-top:4px}
.days label{flex:1;margin:0}
.days input{position:absolute;opacity:0;pointer-events:none}
.days span{display:block;text-align:center;padding:8px 0;border-radius:8px;background:var(--tile);font-size:13px;font-weight:600;color:var(--mut);cursor:pointer}
.days input:checked+span{background:var(--acc);color:#fff}
.days input.dirty+span{box-shadow:inset 0 0 0 2px var(--warn)}
.toast{position:fixed;left:50%;bottom:24px;transform:translate(-50%,160px);background:var(--fg);color:var(--bg);
 padding:12px 18px;border-radius:12px;font-weight:600;transition:transform .25s;box-shadow:var(--sh);max-width:90vw}
.toast.show{transform:translate(-50%,0)}
.toast.err{background:var(--bad);color:#fff}
.kv{display:flex;justify-content:space-between;gap:8px;padding:8px 0;border-bottom:1px solid var(--line);font-size:14px}
.kv span{color:var(--mut)}
.kv b,.kv code{text-align:right}
code{font:13px ui-monospace,Consolas,monospace;word-break:break-all}
.note{color:var(--mut);font-size:13px;margin:12px 0 0}
.log{list-style:none;margin:0;padding:0}
.log li{display:flex;justify-content:space-between;gap:8px;padding:8px 0;border-bottom:1px solid var(--line);font-size:14px}
.log li:last-child{border-bottom:0}
.log time{color:var(--mut);font-size:13px;white-space:nowrap}
.btn.bad{background:var(--bad);color:#fff}
input[type=file]{width:100%;font:inherit;font-size:13px;color:var(--mut);margin-top:4px}
.bar{height:6px;background:var(--line);border-radius:3px;overflow:hidden;margin-top:12px;display:none}
.bar i{display:block;height:100%;width:0;background:var(--acc);transition:width .2s}
.hide{display:none!important}
</style></head><body>
<div class="app">
 <header>
  <h1>Türöffner</h1>
  <div class="pill"><span class="dot" id="conn"></span><span id="connt">verbinde…</span></div>
 </header>

 <div class="seg" id="seg">
  <button id="tb0" class="active" onclick="tab(0)">Tür</button>
  <button id="tb1" onclick="tab(1)">Einstellungen</button>
  <button id="tb2" onclick="tab(2)">Dienste</button>
  <button id="tb3" onclick="tab(3)">System</button>
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
    <div class="tile"><small>Tür</small><b id="doorst">–</b></div>
    <div class="tile"><small>Modus</small><b id="mode" class="sm">–</b></div>
    <div class="tile"><small>Letzter Anruf</small><b id="lastc" class="sm">–</b></div>
   </div>
   <div class="row2">
    <button class="btn" onclick="testCall()">📞 Test-Anruf</button>
    <button class="btn" id="snd" onclick="initAudio();beep()">🔇 Ton testen</button>
   </div>
  </div>
  <div class="card">
   <h2>Verlauf</h2>
   <ul class="log" id="log"><li><span>Noch keine Ereignisse</span></li></ul>
  </div>
 </div>

 <div id="t1" class="tab">
  <div class="card">
   <h2>Türsummer &amp; Anruf</h2>
   <label for="dur">Summer-Dauer (Sekunden)</label>
   <input id="dur" type="number" min="1" max="30">
   <label for="dial">Anrufen beim Klingeln (Rufkette)</label>
   <input id="dial" type="text" inputmode="tel" placeholder="z. B. 100, 101">
   <label for="pin">Öffnen-Code am Telefon</label>
   <input id="pin" type="text" inputmode="numeric" maxlength="8" placeholder="leer = Taste *" autocomplete="off">
   <div class="sw"><span>Beim Klingeln anrufen</span><input id="auto" type="checkbox"></div>
   <p class="note">Mehrere Nummern werden nacheinander angerufen, bis jemand abhebt (je 15 s).
   Mit Code bleibt das Gespräch 30 s offen; „*“ oder „#“ löscht die Eingabe.</p>
   <button class="btn pri" onclick="saveGeneral()">Speichern</button>
  </div>
  <div class="card">
   <h2>Nachtruhe</h2>
   <div class="sw" style="margin-top:0"><span>Nachts nicht anrufen</span><input id="qon" type="checkbox"></div>
   <div class="half">
    <div><label for="qfrom">von</label><input id="qfrom" type="time"></div>
    <div><label for="qto">bis</label><input id="qto" type="time"></div>
   </div>
   <p class="note" id="qnote">Klingeln wird weiter gezählt, protokolliert und gemeldet.</p>
   <button class="btn pri" onclick="saveQuiet()">Speichern</button>
  </div>
  <div class="card">
   <h2>Praxis-Modus</h2>
   <div class="sw" style="margin-top:0"><span>Beim Klingeln automatisch öffnen</span><input id="pon" type="checkbox"></div>
   <label>Tage</label>
   <div class="days" id="pdays"></div>
   <div class="half">
    <div><label for="pfrom">von</label><input id="pfrom" type="time"></div>
    <div><label for="pto">bis</label><input id="pto" type="time"></div>
   </div>
   <p class="note">In diesem Zeitfenster öffnet Klingeln sofort die Tür, ohne Anruf.</p>
   <button class="btn pri" onclick="savePraxis()">Speichern</button>
  </div>
  <div class="card">
   <h2>Türkontakt</h2>
   <div class="sw" style="margin-top:0"><span>Türkontakt angeschlossen</span><input id="don" type="checkbox"></div>
   <div class="sw"><span>Kontakt umgekehrt</span><input id="dinv" type="checkbox"></div>
   <label for="dalert">Melden, wenn länger offen als (Minuten, 0 = nie)</label>
   <input id="dalert" type="number" min="0" max="1440">
   <p class="note" id="dnote">Reed-Kontakt zwischen Pin und GND, geschlossen = Tür zu.</p>
   <button class="btn pri" onclick="saveDoor()">Speichern</button>
  </div>
  <div class="card">
   <h2>SIP-Zugang <b id="sipst" class="off">–</b></h2>
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
   <h2>Push-Mitteilungen <b id="pushst" class="off">–</b></h2>
   <label for="ptype">Dienst</label>
   <select id="ptype" onchange="pushForm()"><option value="0">aus</option><option value="1">ntfy</option><option value="2">Telegram</option></select>
   <div id="pbox">
    <div id="pserverbox"><label for="pserver">ntfy-Server</label><input id="pserver" type="text" placeholder="https://ntfy.sh"></div>
    <label for="ptopic" id="ptopicl">Topic</label><input id="ptopic" type="text" autocomplete="off">
    <label for="ptoken" id="ptokenl">Zugangs-Token (optional)</label><input id="ptoken" type="password" placeholder="unverändert" autocomplete="new-password">
    <div class="sw"><span>Klingeln melden</span><input id="pev1" type="checkbox"></div>
    <div class="sw"><span>Öffnen melden</span><input id="pev2" type="checkbox"></div>
    <div class="sw"><span>Tür zu lange offen melden</span><input id="pev4" type="checkbox"></div>
   </div>
   <p class="note" id="pnote"></p>
   <button class="btn pri" onclick="savePush()">Speichern</button>
   <button class="btn full" onclick="testPush()">Test-Mitteilung senden</button>
  </div>
  <div class="card">
   <h2>Home Assistant (MQTT) <b id="mqst" class="off">–</b></h2>
   <div class="two">
    <div><label for="mqserver">Broker</label><input id="mqserver" type="text" placeholder="leer = aus"></div>
    <div><label for="mqport">Port</label><input id="mqport" type="number" min="1" max="65535"></div>
   </div>
   <label for="mquser">Benutzer</label>
   <input id="mquser" type="text" autocomplete="off">
   <label for="mqpw">Passwort</label>
   <input id="mqpw" type="password" placeholder="unverändert" autocomplete="new-password">
   <button class="btn pri" onclick="saveMqtt()">Speichern &amp; verbinden</button>
   <div class="kv" style="margin-top:12px"><span>Geräte-ID</span><code id="mqid">–</code></div>
   <div class="kv"><span>Basis-Topic</span><code id="mqbase">–</code></div>
   <p class="note">Das Gerät erscheint automatisch unter <b>Einstellungen → Geräte &amp; Dienste → MQTT</b>.
   Voraussetzung: Mosquitto-Add-on und MQTT-Integration.</p>
  </div>
 </div>

 <div id="t3" class="tab">
  <div class="card">
   <h2>Gerät</h2>
   <div class="kv"><span>Adresse</span><code id="sysaddr">–</code></div>
   <div class="kv"><span>Firmware</span><code id="sysver">–</code></div>
   <div class="kv"><span>Laufzeit</span><b id="sysup">–</b></div>
   <div class="kv"><span>Letzter Start</span><b id="sysboot">–</b></div>
   <div class="kv"><span>Uhrzeit</span><b id="systime">–</b></div>
   <div class="kv"><span>WLAN-Signal</span><b id="sysrssi">–</b></div>
   <div class="kv"><span>Freier Speicher</span><b id="sysheap">–</b></div>
   <div class="row2">
    <button class="btn" onclick="restart()">↻ Neustart</button>
    <button class="btn" onclick="resetCounters()">Zähler auf 0</button>
   </div>
   <button class="btn bad full" onclick="wifiReset()">WLAN zurücksetzen</button>
  </div>
  <div class="card">
   <h2>Netzwerk</h2>
   <div class="sw" style="margin-top:0"><span>Feste IP-Adresse</span><input id="nstatic" type="checkbox" onchange="netForm()"></div>
   <div id="nbox">
    <div class="half">
     <div><label for="nip">IP-Adresse</label><input id="nip" type="text" inputmode="decimal"></div>
     <div><label for="nmask">Subnetzmaske</label><input id="nmask" type="text" inputmode="decimal"></div>
     <div><label for="ngw">Gateway</label><input id="ngw" type="text" inputmode="decimal"></div>
     <div><label for="ndns">DNS</label><input id="ndns" type="text" inputmode="decimal"></div>
    </div>
   </div>
   <p class="note">Das Gerät startet danach neu. Falsche Werte? Klingel-Taster beim Einschalten
   5 s halten – dann gilt wieder DHCP.</p>
   <button class="btn pri" onclick="saveNet()">Speichern &amp; neu starten</button>
  </div>
  <div class="card">
   <h2>Admin-Zugang <b id="webst" class="off">–</b></h2>
   <label for="webuser">Benutzer</label>
   <input id="webuser" type="text" autocomplete="off">
   <label for="webpw">Neues Passwort</label>
   <input id="webpw" type="password" autocomplete="new-password">
   <label for="webpw2">Passwort wiederholen</label>
   <input id="webpw2" type="password" autocomplete="new-password">
   <button class="btn pri" onclick="saveWeb()">Passwort setzen</button>
   <button class="btn full" id="weboff" onclick="webOff()">Passwortschutz entfernen</button>
   <p class="note">Darf alles. Gilt auch für Updates (aus PlatformIO erst nach einem Neustart).</p>
  </div>
  <div class="card">
   <h2>Bedien-Zugang <b id="opst" class="off">–</b></h2>
   <label for="opuser">Benutzer</label>
   <input id="opuser" type="text" autocomplete="off">
   <label for="oppw">Neues Passwort</label>
   <input id="oppw" type="password" autocomplete="new-password">
   <button class="btn pri" onclick="saveOp()">Passwort setzen</button>
   <button class="btn full" id="opoff" onclick="opOff()">Bedien-Zugang entfernen</button>
   <p class="note" id="opnote">Darf nur öffnen und den Verlauf sehen – z. B. für den Kunden.
   Passwort vergessen? Klingel-Taster beim Einschalten 5 s halten – dann sind beide Passwörter weg.</p>
  </div>
  <div class="card">
   <h2>Sicherung</h2>
   <button class="btn full" style="margin-top:0" onclick="location.href='/backup'">⬇ Einstellungen herunterladen</button>
   <label for="rs">Sicherung einspielen</label>
   <input id="rs" type="file" accept=".json">
   <button class="btn pri" onclick="restore()">Einspielen &amp; neu starten</button>
   <p class="note">Die Datei enthält alle Passwörter – sicher aufbewahren.</p>
  </div>
  <div class="card">
   <h2>Firmware-Update</h2>
   <input id="fw" type="file" accept=".bin">
   <div class="bar" id="fwbar"><i id="fwprog"></i></div>
   <button class="btn pri" onclick="upload()">Hochladen &amp; installieren</button>
   <p class="note">Datei: <code>.pio/build/&lt;board&gt;/firmware.bin</code>. Einstellungen bleiben erhalten.</p>
  </div>
 </div>
</div>
<div class="toast" id="toast"></div>
<script>
let audioCtx=null,soundOn=false,wasRinging=false,buzEnd=0,buzDur=1,tt,logN=-1,role=2;
const $=id=>document.getElementById(id);
function tab(n){for(let i=0;i<4;i++){$('t'+i).className=i==n?'tab show':'tab';$('tb'+i).className=i==n?'active':'';}}
function toast(msg,err){const t=$('toast');t.textContent=msg;t.className='toast show'+(err?' err':'');
 clearTimeout(tt);tt=setTimeout(()=>t.className='toast'+(err?' err':''),3000);}
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
// Wochentage fuer den Praxis-Modus
['Mo','Di','Mi','Do','Fr','Sa','So'].forEach((d,i)=>{const l=document.createElement('label');
 l.innerHTML='<input type="checkbox" id="pd'+i+'"><span>'+d+'</span>';$('pdays').append(l);});
// Vom Nutzer geaenderte, noch nicht gespeicherte Felder: refresh() laesst sie in Ruhe
const dirty=new Set();let gen=0;
const isField=c=>c.id&&(c.tagName==='INPUT'||c.tagName==='SELECT')&&c.type!=='file';
function markDirty(e){const c=e.target;if(!isField(c))return;
 dirty.add(c.id);c.classList.add('dirty');const b=c.closest('.card').querySelector('.pri');if(b)b.classList.add('due');}
document.addEventListener('input',markDirty);document.addEventListener('change',markDirty);
// Enter in einem Feld = Speichern-Button der Karte
document.addEventListener('keydown',e=>{if(e.key==='Enter'&&e.target.tagName==='INPUT'){
 const b=e.target.closest('.card').querySelector('.pri');if(b){e.preventDefault();b.click();}}});
function clean(ids){for(const id of ids){dirty.delete(id);const c=$(id);c.classList.remove('dirty');
 const b=c.closest('.card').querySelector('.pri');if(b&&!c.closest('.card').querySelector('.dirty'))b.classList.remove('due');}}
function setVal(id,v){const e=$(id);if(!dirty.has(id)&&document.activeElement!==e)e.value=v;}
function setChk(id,v){const e=$(id);if(!dirty.has(id))e.checked=v;}
function flag(id,on,a,b){const e=$(id);e.textContent=on?a:b;e.className=on?'on':'off';}
function txt(id,t,c){const e=$(id);e.textContent=t;e.className=(e.classList.contains('sm')?'sm ':'')+(c||'');}
async function post(url,okMsg,ids,body){
 gen++;   // laufende refresh()-Antworten mit alten Werten verwerfen
 let ok=false;
 try{const r=await fetch(url,{method:'POST',body});let j={};try{j=await r.json();}catch(e){}
  ok=r.ok;if(r.ok){if(ids)clean(ids);if(okMsg)toast(okMsg);}else toast(j.err||'Fehler',true);}
 catch(e){toast('Keine Verbindung',true);}
 refresh();return ok;
}
const hm=m=>String(m/60|0).padStart(2,'0')+':'+String(m%60).padStart(2,'0');
const mins=v=>{const p=(v||'0:0').split(':');return p[0]*60+ +p[1];};
function dur(s){if(s<60)return s+' s';if(s<3600)return(s/60|0)+' min';
 if(s<86400)return(s/3600|0)+' h '+(s%3600/60|0)+' min';return(s/86400|0)+' T '+(s%86400/3600|0)+' h';}
const EV=['🔔 Klingeln','🔔 Klingeln (Nachtruhe)','📞 Anruf ausgelöst','🔓 Geöffnet (Web)',
 '🔓 Geöffnet (Taster)','🔓 Geöffnet (Telefon)','🔓 Geöffnet (Home Assistant)','⚡ Gerät gestartet',
 '🔓 Automatisch geöffnet (Praxis)','📵 Niemand hat abgehoben','⚠️ Tür zu lange offen'];
function when(e){
 if(!e.t)return'vor '+dur(e.a);
 const d=new Date(e.t*1000);
 return d.toDateString()==new Date().toDateString()?d.toLocaleTimeString('de-DE',{hour:'2-digit',minute:'2-digit'})
  :d.toLocaleString('de-DE',{day:'2-digit',month:'2-digit',hour:'2-digit',minute:'2-digit'});
}
async function loadLog(){
 try{const j=await(await fetch('/log')).json(),ul=$('log');ul.textContent='';
  if(!j.ev.length){const li=document.createElement('li');li.textContent='Noch keine Ereignisse';ul.append(li);return;}
  for(const e of j.ev){const li=document.createElement('li'),a=document.createElement('span'),t=document.createElement('time');
   a.textContent=EV[e.e]||'?';t.textContent=when(e);li.append(a,t);ul.append(li);}
 }catch(e){}
}
function anim(){
 const left=Math.max(0,buzEnd-Date.now()),f=left/(buzDur*1000);
 $('prog').style.strokeDashoffset=490*(1-f);
 const act=left>0;$('openb').className=act?'open active':'open';
 $('openl').textContent=act?Math.ceil(left/1000)+' s':'Öffnen';
 if(act)requestAnimationFrame(anim);
}
// SIP-Anmeldung im Klartext
function sipText(s){
 if(!s.sipserver&&role==2)return['kein Server','off'];
 const c=s.sipst;
 if(c==200)return['angemeldet','on'];if(c==-1)return['verbinde …','off'];
 if(c==0)return['Anlage antwortet nicht','bad'];
 if(c==401||c==407)return['Benutzer/Passwort falsch','bad'];
 if(c==403)return['abgelehnt (403)','bad'];if(c==404)return['Benutzer unbekannt (404)','bad'];
 return['Fehler '+c,'bad'];
}
function callText(s){
 if(s.chain)return['läuft …','warn'];
 return[['–','off'],['angenommen','on'],['nicht angenommen','warn'],['besetzt','warn'],['abgelehnt','warn'],['Fehler '+s.lastcode,'bad']][s.lastcall]||['–','off'];
}
async function refresh(){
 try{
  const g=gen;
  const s=await(await fetch('/status')).json();
  if(g!==gen)return;   // waehrenddessen gespeichert -> Antwort ist veraltet
  role=s.role;$('seg').classList.toggle('hide',role<2);if(role<2)tab(0);
  $('conn').className='dot on';$('connt').textContent='online';
  flag('sig',s.signal,'aktiv','ruhig');flag('buz',s.buzzer,'an','aus');flag('reg',s.registered,'ok','nein');
  $('reg').title=sipText(s)[0];
  $('rings').textContent=s.signals;$('cnt').textContent=s.triggers;$('calls').textContent=s.calls;
  if(!s.door)txt('doorst','–','off');else if(s.door==2)txt('doorst',s.dooralert?'offen!':'offen',s.dooralert?'bad':'warn');else txt('doorst','zu','on');
  txt('mode',s.praxisnow?'Praxis':s.quietnow?'Nachtruhe':'Normal',s.praxisnow||s.quietnow?'warn':'');
  const lc=callText(s);txt('lastc',lc[0],lc[1]);if(s.lastnr)$('lastc').title='an '+s.lastnr;
  $('doorc').className=s.ringing?'card ringing':'card';
  if(s.ringing&&!wasRinging&&soundOn)beep();wasRinging=s.ringing;
  buzDur=s.seconds;
  if(s.buzzer&&buzEnd<Date.now()){buzEnd=Date.now()+s.seconds*1000;anim();}
  if(!s.buzzer&&buzEnd>Date.now()){buzEnd=0;anim();}
  const nrs=s.dial.split(/[,; ]+/).filter(x=>x).join(' → ');
  $('hint').textContent=s.ringing?'🔔 Es klingelt!':s.praxisnow?'🏥 Praxis-Modus – Klingeln öffnet automatisch'
   :!nrs?'Keine Zielnummer eingestellt':!s.callonring?'Anruf beim Klingeln ist aus'
   :s.quietnow?'🌙 Nachtruhe – Klingeln ruft nicht an':'Klingeln ruft '+nrs+' an – '+(s.haspin?'Code':'*')+' am Telefon öffnet';
  if(s.logn!==logN){logN=s.logn;loadLog();}
  if(role<2)return;   // Bedien-Zugang: keine Einstellungen
  setVal('dur',s.seconds);setVal('dial',s.dial);setVal('pin',s.pin);setChk('auto',s.callonring);
  setChk('qon',s.quiet);setVal('qfrom',hm(s.qfrom));setVal('qto',hm(s.qto));
  $('qnote').textContent=!s.ntp?'Uhrzeit noch nicht abgeglichen – greift erst danach.'
   :(s.quietnow?'Gerade aktiv. ':'')+'Klingeln wird weiter gezählt, protokolliert und gemeldet.';
  setChk('pon',s.praxis);setVal('pfrom',hm(s.pfrom));setVal('pto',hm(s.pto));
  for(let i=0;i<7;i++)setChk('pd'+i,!!(s.pdays&(1<<i)));
  setChk('don',s.dooron);setChk('dinv',s.doorinv);setVal('dalert',s.dooralertmin);
  $('dnote').textContent='Reed-Kontakt zwischen '+s.doorpin+' und GND, geschlossen = Tür zu.'+(s.dooron?' Aktuell: '+(s.door==2?'offen':'zu')+'.':'');
  const st=sipText(s);txt('sipst',st[0],st[1]);
  setVal('sipserver',s.sipserver);setVal('sipport',s.sipport);setVal('sipuser',s.sipuser);
  $('sippw').placeholder=s.haspw?'unverändert':'nicht gesetzt';
  setVal('ptype',s.ptype);setVal('pserver',s.pserver);setVal('ptopic',s.ptopic);
  $('ptoken').placeholder=s.haspt?'unverändert':'nicht gesetzt';
  for(const b of[1,2,4])setChk('pev'+b,!!(s.pev&b));pushForm();
  txt('pushst',!s.ptype?'aus':s.pcode==0?'bereit':s.pcode>=200&&s.pcode<300?'zuletzt ok':'Fehler '+s.pcode,
   !s.ptype||s.pcode==0?'off':s.pcode>=200&&s.pcode<300?'on':'bad');
  setVal('mqserver',s.mqttserver);setVal('mqport',s.mqttport);setVal('mquser',s.mqttuser);
  $('mqpw').placeholder=s.hasmqttpw?'unverändert':'nicht gesetzt';
  $('mqid').textContent=s.mqttid;$('mqbase').textContent=s.mqttbase;
  if(s.mqttserver)flag('mqst',s.mqtt,'verbunden','getrennt');else txt('mqst','aus','off');
  $('sysaddr').textContent=s.host+'.local · '+s.ip;$('sysver').textContent=s.ver;$('sysup').textContent=dur(s.up);
  $('sysboot').textContent=s.boot;
  $('systime').textContent=s.ntp?new Date(s.now*1000).toLocaleString('de-DE'):'nicht abgeglichen';
  $('sysrssi').textContent=s.rssi+' dBm ('+(s.rssi>-60?'gut':s.rssi>-75?'mittel':'schwach')+')';
  $('sysheap').textContent=Math.round(s.heap/1024)+' KB';
  setChk('nstatic',s.nstatic);setVal('nip',s.nip);setVal('nmask',s.nmask);setVal('ngw',s.ngw);setVal('ndns',s.ndns);netForm();
  setVal('webuser',s.webuser);flag('webst',s.hasweb,'geschützt','offen');$('weboff').classList.toggle('hide',!s.hasweb);
  setVal('opuser',s.opuser);flag('opst',s.hasop,'aktiv','aus');$('opoff').classList.toggle('hide',!s.hasop);
 }catch(e){$('conn').className='dot off';$('connt').textContent='offline';}
}
function pushForm(){const t=$('ptype').value;$('pbox').classList.toggle('hide',t=='0');
 $('pserverbox').classList.toggle('hide',t!='1');
 $('ptopicl').textContent=t=='2'?'Chat-ID':'Topic';
 $('ptokenl').textContent=t=='2'?'Bot-Token':'Zugangs-Token (optional)';
 $('pnote').textContent=t=='1'?'App „ntfy“ installieren und dasselbe Topic abonnieren. Topic schwer erratbar wählen.'
  :t=='2'?'Bot über @BotFather anlegen; Chat-ID z. B. über @userinfobot.':'';}
function netForm(){$('nbox').classList.toggle('hide',!$('nstatic').checked);}
function openDoor(){buzEnd=Date.now()+buzDur*1000;anim();post('/open','Tür wird geöffnet');}
const Q=o=>new URLSearchParams(o);
function saveGeneral(){
 post('/setdial?'+Q({s:$('dur').value,nr:$('dial').value,pin:$('pin').value,auto:$('auto').checked?1:0}),
  'Gespeichert',['dur','dial','pin','auto']);
}
function testCall(){post('/call','Anruf wird aufgebaut');}
function saveSip(){
 const q=Q({server:$('sipserver').value,port:$('sipport').value,user:$('sipuser').value,pw:$('sippw').value});
 $('sippw').value='';post('/setsip?'+q,'Gespeichert – registriere neu',['sipserver','sipport','sipuser','sippw']);
}
function saveMqtt(){
 const q=Q({server:$('mqserver').value,port:$('mqport').value,user:$('mquser').value,pw:$('mqpw').value});
 $('mqpw').value='';post('/setmqtt?'+q,'Gespeichert – verbinde',['mqserver','mqport','mquser','mqpw']);
}
function saveQuiet(){
 post('/setquiet?'+Q({on:$('qon').checked?1:0,from:mins($('qfrom').value),to:mins($('qto').value)}),'Gespeichert',['qon','qfrom','qto']);
}
function savePraxis(){
 let d=0;for(let i=0;i<7;i++)if($('pd'+i).checked)d|=1<<i;
 post('/setpraxis?'+Q({on:$('pon').checked?1:0,days:d,from:mins($('pfrom').value),to:mins($('pto').value)}),'Gespeichert',
  ['pon','pfrom','pto','pd0','pd1','pd2','pd3','pd4','pd5','pd6']);
}
function saveDoor(){
 post('/setdoor?'+Q({on:$('don').checked?1:0,inv:$('dinv').checked?1:0,alert:$('dalert').value||0}),'Gespeichert',['don','dinv','dalert']);
}
function savePush(){
 let ev=0;for(const b of[1,2,4])if($('pev'+b).checked)ev|=b;
 const q=Q({type:$('ptype').value,server:$('pserver').value,topic:$('ptopic').value,token:$('ptoken').value,ev});
 $('ptoken').value='';post('/setpush?'+q,'Gespeichert',['ptype','pserver','ptopic','ptoken','pev1','pev2','pev4']);
}
function testPush(){post('/testpush','Test-Mitteilung wird gesendet');setTimeout(refresh,4000);}
function saveNet(){
 const st=$('nstatic').checked,ip=$('nip').value;
 if(!confirm(st?'Feste IP '+ip+' übernehmen und neu starten?\nDanach ist das Gerät unter http://'+ip+' erreichbar.'
  :'Wieder DHCP verwenden und neu starten?\nDie Adresse kann sich ändern – dann über tueroeffner.local aufrufen.'))return;
 post('/setnet?'+Q({static:st?1:0,ip,mask:$('nmask').value,gw:$('ngw').value,dns:$('ndns').value}),'Startet neu …',
  ['nstatic','nip','nmask','ngw','ndns']).then(ok=>{if(ok)setTimeout(()=>location.href=st?'http://'+ip+'/':'/',15000);});
}
function saveWeb(){
 const p=$('webpw').value;
 if(!$('webuser').value||!p){toast('Benutzer und Passwort eingeben',true);return;}
 if(p!==$('webpw2').value){toast('Passwörter sind verschieden',true);return;}
 const q=Q({user:$('webuser').value,pw:p});
 $('webpw').value=$('webpw2').value='';
 post('/setweb?'+q,'Passwort gesetzt – bitte neu anmelden',['webuser','webpw','webpw2']);
}
function webOff(){if(confirm('Passwortschutz wirklich entfernen?\nDer Bedien-Zugang wird dabei ebenfalls entfernt.'))post('/setweb?off=1','Passwortschutz entfernt');}
function saveOp(){
 const p=$('oppw').value;
 if(!$('opuser').value||!p){toast('Benutzer und Passwort eingeben',true);return;}
 const q=Q({user:$('opuser').value,pw:p});$('oppw').value='';
 post('/setop?'+q,'Bedien-Zugang gesetzt',['opuser','oppw']);
}
function opOff(){if(confirm('Bedien-Zugang entfernen?'))post('/setop?off=1','Bedien-Zugang entfernt');}
function restart(){if(confirm('Gerät neu starten?')){post('/restart','Startet neu …');setTimeout(()=>location.reload(),12000);}}
function resetCounters(){if(confirm('Zähler für Klingeln, Öffnungen und Anrufe auf 0 setzen?'))post('/resetcounters','Zähler zurückgesetzt');}
function wifiReset(){
 if(confirm('WLAN-Zugangsdaten löschen?\nDanach öffnet das Gerät das WLAN „Tueroeffner-Setup“ zur Neueinrichtung.'))
  post('/wifireset','WLAN wird zurückgesetzt');
}
async function restore(){
 const f=$('rs').files[0];if(!f){toast('Bitte Sicherungsdatei auswählen',true);return;}
 let j;try{j=JSON.parse(await f.text());}catch(e){toast('Keine gültige Sicherung',true);return;}
 if(!j.tueroeffner){toast('Keine Sicherung vom Türöffner',true);return;}
 if(!confirm('Einstellungen aus der Sicherung übernehmen?\nDas Gerät startet danach neu.'))return;
 const b=new URLSearchParams();for(const k in j)if(k!='tueroeffner'&&k!='fw')b.append(k,j[k]);
 if(await post('/restore','Eingespielt – startet neu …',null,b))setTimeout(()=>location.reload(),15000);
}
function upload(){
 const f=$('fw').files[0];if(!f){toast('Bitte firmware.bin auswählen',true);return;}
 const fd=new FormData(),x=new XMLHttpRequest();fd.append('firmware',f,f.name);
 $('fwbar').style.display='block';$('fwprog').style.width='0';
 x.upload.onprogress=e=>{if(e.lengthComputable)$('fwprog').style.width=(e.loaded/e.total*100)+'%';};
 x.onload=()=>{
  if(x.status==200){toast('Update installiert – Neustart …');setTimeout(()=>location.reload(),15000);return;}
  let m='Update fehlgeschlagen';try{m=JSON.parse(x.responseText).err||m;}catch(e){}
  toast(m,true);$('fwbar').style.display='none';
 };
 x.onerror=()=>{toast('Upload abgebrochen',true);$('fwbar').style.display='none';};
 x.open('POST','/update');x.send(fd);
}
setInterval(refresh,1000);refresh();
setInterval(loadLog,60000);   // relative Zeitangaben aktuell halten
</script>
</body></html>
)HTML";

// ------------------------------------------------------------
//  Anmeldung: Admin darf alles, Bedien-Zugang nur oeffnen/ansehen
// ------------------------------------------------------------
enum Role : uint8_t { ROLE_NONE, ROLE_USER, ROLE_ADMIN };

uint8_t currentRole() {
  if (webPw.length() == 0) return ROLE_ADMIN;   // kein Schutz eingerichtet
  if (server.authenticate(webUser.c_str(), webPw.c_str())) return ROLE_ADMIN;
  if (opPw.length() && server.authenticate(opUser.c_str(), opPw.c_str())) return ROLE_USER;
  return ROLE_NONE;
}

// HTTP-Basic-Auth; fordert bei fehlender Berechtigung (neu) zur Anmeldung auf
bool requireRole(uint8_t needed) {
  if (currentRole() >= needed) return true;
  server.requestAuthentication(BASIC_AUTH, "Tueroeffner");
  return false;
}

#define NEED_USER  if (!requireRole(ROLE_USER))  return
#define NEED_ADMIN if (!requireRole(ROLE_ADMIN)) return

// Eingegebenen Text sicher in einen JSON-String packen
String jsonEsc(const String &s) {
  String r;
  r.reserve(s.length() + 4);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { r += '\\'; r += c; }
    else if ((uint8_t)c >= 0x20) r += c;   // Steuerzeichen weglassen
  }
  return r;
}

// JSON-Bausteine
String jStr(const char *key, const String &v) { return String(",\"") + key + "\":\"" + jsonEsc(v) + "\""; }
String jNum(const char *key, long v)          { return String(",\"") + key + "\":" + String(v); }
String jBool(const char *key, bool v)         { return String(",\"") + key + "\":" + (v ? "true" : "false"); }

void sendOk() {
  server.send(200, "application/json", "{\"ok\":true}");
}

void sendErr(int code, const char *msg) {
  server.send(code, "application/json", String("{\"ok\":false,\"err\":\"") + msg + "\"}");
}

// Hilfen zum Uebernehmen von Formularwerten
bool argInt(const char *key, long lo, long hi, long &out) {
  if (!server.hasArg(key)) return false;
  long v = server.arg(key).toInt();
  if (v < lo || v > hi) return false;
  out = v;
  return true;
}

bool argIp(const char *key, IPAddress &out) {
  IPAddress ip;
  if (!server.hasArg(key) || !ip.fromString(server.arg(key))) return false;
  out = ip;
  return true;
}

bool pinValid(const String &p) {
  if (p.length() > 8) return false;
  for (size_t i = 0; i < p.length(); i++) if (!isdigit((uint8_t)p[i])) return false;
  return true;
}

void handleRoot() {
  NEED_USER;
  server.send_P(200, "text/html; charset=utf-8", PAGE_HTML);
}

void handleStatus() {
  uint8_t role = currentRole();
  if (role == ROLE_NONE) { server.requestAuthentication(BASIC_AUTH, "Tueroeffner"); return; }

  // Kurze Tastendruecke etwas nachhalten, damit die 1-s-Abfrage sie sieht
  bool sigShown = signalActive ||
                  (lastSignalOffAt != 0 && millis() - lastSignalOffAt < SIGNAL_HOLD_MS);
  String json = "{\"role\":" + String(role);
  json += jBool("signal",    sigShown);
  json += jBool("ringing",   isRinging());
  json += jBool("buzzer",    buzzerActive);
  json += jNum ("seconds",   buzzerSeconds);
  json += jNum ("triggers",  buzzerTriggers);
  json += jNum ("signals",   signalCount);
  json += jNum ("calls",     callCount);
  json += jStr ("dial",      dialList);
  json += jBool("haspin",    dtmfPin.length() > 0);
  json += jBool("callonring", callOnRing);
  json += jBool("registered", aSip.IsRegistered());
  json += jNum ("sipst",     aSip.RegisterStatus());
  json += jBool("chain",     chainIdx >= 0);
  json += jNum ("lastcall",  aSip.LastCallResult());
  json += jNum ("lastcode",  aSip.LastCallCode());
  json += jStr ("lastnr",    lastCallNr);
  json += jBool("quietnow",  isQuiet());
  json += jBool("praxisnow", isPraxis());
  json += jNum ("door",      doorOn ? (doorOpen ? 2 : 1) : 0);
  json += jBool("dooralert", doorAlerted);
  json += jBool("ntp",       timeValid());
  json += jNum ("now",       (long)time(nullptr));
  json += jNum ("logn",      logTotal);

  if (role == ROLE_ADMIN) {
    json += jStr ("pin",        dtmfPin);
    json += jStr ("sipserver",  sipServer);
    json += jNum ("sipport",    sipPort);
    json += jStr ("sipuser",    sipUser);
    json += jBool("haspw",      sipPw.length() > 0);
    json += jStr ("mqttserver", mqttServer);
    json += jNum ("mqttport",   mqttPort);
    json += jStr ("mqttuser",   mqttUser);
    json += jBool("hasmqttpw",  mqttPw.length() > 0);
    json += jBool("mqtt",       mqtt.connected());
    json += jStr ("mqttid",     devId);
    json += jStr ("mqttbase",   baseTopic);
    json += jBool("quiet",      quietOn);
    json += jNum ("qfrom",      quietFrom);
    json += jNum ("qto",        quietTo);
    json += jBool("praxis",     praxisOn);
    json += jNum ("pdays",      praxisDays);
    json += jNum ("pfrom",      praxisFrom);
    json += jNum ("pto",        praxisTo);
    json += jBool("dooron",     doorOn);
    json += jBool("doorinv",    doorInvert);
    json += jNum ("dooralertmin", doorAlertMin);
#if defined(DOOR_ON_RX_PIN)
    json += jStr ("doorpin",    "RX (GPIO3)");
#else
    json += jStr ("doorpin",    "GPIO" + String(PIN_DOOR));
#endif
    json += jNum ("ptype",      pushType);
    json += jStr ("pserver",    pushServer);
    json += jStr ("ptopic",     pushTopic);
    json += jBool("haspt",      pushToken.length() > 0);
    json += jNum ("pev",        pushEvents);
    json += jNum ("pcode",      lastPushCode);
    json += jBool("hasweb",     webPw.length() > 0);
    json += jStr ("webuser",    webUser);
    json += jBool("hasop",      opPw.length() > 0);
    json += jStr ("opuser",     opUser);
    json += jBool("nstatic",    staticIp);
    // Bei DHCP die aktuellen Werte vorschlagen
    json += jStr ("nip",   (staticIp ? ipAddr : WiFi.localIP()).toString());
    json += jStr ("nmask", (staticIp ? ipMask : WiFi.subnetMask()).toString());
    json += jStr ("ngw",   (staticIp ? ipGw   : WiFi.gatewayIP()).toString());
    json += jStr ("ndns",  (staticIp ? ipDns  : WiFi.dnsIP()).toString());
    json += jStr ("host",  HOSTNAME);
    json += jStr ("ip",    WiFi.localIP().toString());
    json += jStr ("ver",   FW_VERSION " (" __DATE__ ")");
    json += jStr ("boot",  bootReason);
    json += jNum ("up",    uptimeSeconds());
    json += jNum ("rssi",  WiFi.RSSI());
    json += jNum ("heap",  ESP.getFreeHeap());
  }
  json += "}";
  server.send(200, "application/json", json);
}

// Ereignisprotokoll, neuestes zuerst. t = Unix-Zeit (0 = unbekannt), a = Alter in s
void handleLog() {
  NEED_USER;
  bool   synced = timeValid();
  time_t now    = time(nullptr);
  String json   = "{\"ev\":[";
  for (uint8_t i = 0; i < logCount; i++) {
    const LogEntry &e = eventLog[(logHead + LOG_SIZE - 1 - i) % LOG_SIZE];
    uint32_t age = (millis() - e.at) / 1000;
    time_t   t   = e.t ? e.t : (synced ? now - age : 0);   // vor dem NTP-Abgleich: nachrechnen
    if (i) json += ",";
    json += "{\"e\":" + String(e.type) + ",\"t\":" + String((uint32_t)t) + ",\"a\":" + String(age) + "}";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleOpen() {
  NEED_USER;
  startBuzzer(EV_OPEN_WEB);
  aSip.Hangup();   // Tuer ist auf -> laufenden Anruf beenden (klingelnd: CANCEL, sonst BYE)
  sendOk();
}

void handleCall() {
  NEED_USER;
  String nr;
  if (!dialTarget(0, nr)) { sendErr(400, "keine Zielnummer"); return; }
  if (!startChain())      { sendErr(409, "Anruf laeuft bereits"); return; }
  sendOk();
}

void handleSetDuration() {
  NEED_ADMIN;
  long v;
  if (!argInt("s", MIN_BUZZER_SECONDS, MAX_BUZZER_SECONDS, v)) { sendErr(400, "Summer-Dauer ungueltig"); return; }
  buzzerSeconds = (uint8_t)v;
  saveSettings();
  displayDirty = true;
  sendOk();
}

// Rufkette, Code, Anruf-Schalter und Summer-Dauer.
// Erst alles pruefen, dann uebernehmen -> bei Fehler bleibt alles unveraendert.
void handleSetDial() {
  NEED_ADMIN;
  String list = server.arg("nr");
  list.trim();
  if (!server.hasArg("nr") || !dialListValid(list)) {
    sendErr(400, "Rufkette zu lang (je Nummer max. 20 Zeichen)");
    return;
  }
  String pin = server.hasArg("pin") ? server.arg("pin") : dtmfPin;
  pin.trim();
  if (!pinValid(pin)) { sendErr(400, "Code: nur Ziffern, max. 8"); return; }
  long dur = buzzerSeconds;
  if (server.hasArg("s") && !argInt("s", MIN_BUZZER_SECONDS, MAX_BUZZER_SECONDS, dur)) {
    sendErr(400, "Summer-Dauer ungueltig");
    return;
  }
  dialList = list;
  dtmfPin  = pin;
  if (server.hasArg("auto")) callOnRing = server.arg("auto") == "1";
  buzzerSeconds = (uint8_t)dur;
  applyCallSeconds();
  saveSettings();
  displayDirty = true;
  mqttStateDue = true;
  sendOk();
}

void handleSetSip() {
  NEED_ADMIN;
  if (server.hasArg("server")) sipServer = server.arg("server");
  long p;
  if (argInt("port", 1, 65535, p)) sipPort = (uint16_t)p;
  if (server.hasArg("user")) sipUser = server.arg("user");
  // Passwort nur uebernehmen, wenn ein neues angegeben wurde
  if (server.hasArg("pw") && server.arg("pw").length() > 0) sipPw = server.arg("pw");
  saveSettings();
  initSip();   // mit neuen Daten neu registrieren
  sendOk();
}

void handleSetMqtt() {
  NEED_ADMIN;
  if (server.hasArg("server")) {
    String s = server.arg("server");
    s.trim();
    if (s.length() < sizeof(mqttServerBuf)) mqttServer = s;
  }
  long p;
  if (argInt("port", 1, 65535, p)) mqttPort = (uint16_t)p;
  if (server.hasArg("user")) mqttUser = server.arg("user");
  // Passwort nur uebernehmen, wenn ein neues angegeben wurde
  if (server.hasArg("pw") && server.arg("pw").length() > 0) mqttPw = server.arg("pw");
  saveSettings();
  initMqtt();   // Verbindung mit neuen Daten aufbauen (in mqttLoop)
  sendOk();
}

void handleSetQuiet() {
  NEED_ADMIN;
  long from, to;
  if (!argInt("from", 0, 24 * 60 - 1, from) || !argInt("to", 0, 24 * 60 - 1, to)) {
    sendErr(400, "Uhrzeit ungueltig");
    return;
  }
  quietOn   = server.arg("on") == "1";
  quietFrom = (uint16_t)from;
  quietTo   = (uint16_t)to;
  saveSettings();
  mqttStateDue = true;
  sendOk();
}

void handleSetPraxis() {
  NEED_ADMIN;
  long from, to, days;
  if (!argInt("from", 0, 24 * 60 - 1, from) || !argInt("to", 0, 24 * 60 - 1, to) ||
      !argInt("days", 0, 0x7F, days)) {
    sendErr(400, "Uhrzeit oder Tage ungueltig");
    return;
  }
  praxisOn   = server.arg("on") == "1";
  praxisDays = (uint8_t)days;
  praxisFrom = (uint16_t)from;
  praxisTo   = (uint16_t)to;
  saveSettings();
  mqttStateDue = true;
  sendOk();
}

void handleSetDoor() {
  NEED_ADMIN;
  long alert;
  if (!argInt("alert", 0, 1440, alert)) { sendErr(400, "Minuten ungueltig (0-1440)"); return; }
  bool wasOn   = doorOn;
  doorOn       = server.arg("on") == "1";
  doorInvert   = server.arg("inv") == "1";
  doorAlertMin = (uint16_t)alert;
  doorAlerted  = false;
  doorOpenedAt = millis();   // Frist neu beginnen
  saveSettings();
  if (wasOn != doorOn) mqttDiscoveryDue = true;   // Tuer-Entitaeten anlegen/entfernen
  mqttStateDue = true;
  sendOk();
}

void handleSetPush() {
  NEED_ADMIN;
  long type, ev;
  if (!argInt("type", PUSH_OFF, PUSH_TELEGRAM, type) || !argInt("ev", 0, 7, ev)) {
    sendErr(400, "Eingaben ungueltig");
    return;
  }
  String srv = server.arg("server"), topic = server.arg("topic");
  srv.trim();
  topic.trim();
  if (srv.length() > 64 || topic.length() > 64 || server.arg("token").length() > 64) {
    sendErr(400, "Eingabe zu lang (max. 64 Zeichen)");
    return;
  }
  if (type != PUSH_OFF && topic.length() == 0) {
    sendErr(400, type == PUSH_TELEGRAM ? "Chat-ID fehlt" : "Topic fehlt");
    return;
  }
  pushType   = (uint8_t)type;
  pushEvents = (uint8_t)ev;
  if (srv.length()) pushServer = srv;
  pushTopic = topic;
  // Token nur uebernehmen, wenn ein neues angegeben wurde
  if (server.arg("token").length() > 0) pushToken = server.arg("token");
  lastPushCode = 0;
  saveSettings();
  sendOk();
}

void handleTestPush() {
  NEED_ADMIN;
  if (pushType == PUSH_OFF || pushTopic.length() == 0) { sendErr(400, "Push ist nicht eingerichtet"); return; }
  sendOk();   // Antwort zuerst: auf dem ESP8266 blockiert das Senden kurz
  queuePush(0, "Test-Mitteilung vom Türöffner");
}

// Admin-Zugang setzen (user + pw) oder Schutz entfernen (off=1)
void handleSetWeb() {
  NEED_ADMIN;
  if (server.arg("off") == "1") {
    webPw = "";
    opPw  = "";   // ohne Admin-Passwort ist ohnehin alles offen
  } else {
    String u = server.arg("user");
    String p = server.arg("pw");
    u.trim();
    if (u.length() == 0 || u.length() > 24 || p.length() == 0 || p.length() > 32) {
      sendErr(400, "Benutzer 1-24, Passwort 1-32 Zeichen");
      return;
    }
    if (opPw.length() && u == opUser) { sendErr(400, "Benutzer ist schon der Bedien-Zugang"); return; }
    webUser = u;
    webPw   = p;
  }
  saveSettings();
  // ArduinoOTA uebernimmt das Passwort nur beim Start -> gilt dort nach Neustart
  sendOk();
}

// Bedien-Zugang setzen oder entfernen (off=1)
void handleSetOp() {
  NEED_ADMIN;
  if (server.arg("off") == "1") {
    opPw = "";
  } else {
    if (webPw.length() == 0) { sendErr(400, "Zuerst ein Admin-Passwort setzen"); return; }
    String u = server.arg("user");
    String p = server.arg("pw");
    u.trim();
    if (u.length() == 0 || u.length() > 24 || p.length() == 0 || p.length() > 32) {
      sendErr(400, "Benutzer 1-24, Passwort 1-32 Zeichen");
      return;
    }
    if (u == webUser) { sendErr(400, "Benutzer muss sich vom Admin unterscheiden"); return; }
    opUser = u;
    opPw   = p;
  }
  saveSettings();
  sendOk();
}

void handleSetNet() {
  NEED_ADMIN;
  bool st = server.arg("static") == "1";
  if (st) {
    IPAddress ip, mask, gw, dns;
    if (!argIp("ip", ip) || !argIp("mask", mask) || !argIp("gw", gw)) {
      sendErr(400, "IP, Subnetzmaske oder Gateway ungueltig");
      return;
    }
    if (!argIp("dns", dns)) dns = gw;
    ipAddr = ip; ipMask = mask; ipGw = gw; ipDns = dns;
  }
  staticIp = st;
  saveSettings();
  sendOk();
  restartReason = RR_NETWORK;
  restartAt     = millis() + 500;
}

void handleResetCounters() {
  NEED_ADMIN;
  signalCount = buzzerTriggers = callCount = 0;
  saveSettings();
  mqttStateDue = true;
  displayDirty = true;
  sendOk();
}

void handleRestart() {
  NEED_ADMIN;
  sendOk();
  restartReason = RR_USER;
  restartAt     = millis() + 500;   // erst Antwort rausschicken
}

void handleWifiReset() {
  NEED_ADMIN;
  sendOk();
  wifiResetDue  = true;
  restartReason = RR_WIFI_RESET;
  restartAt     = millis() + 500;
}

// Alle Einstellungen als JSON-Datei (inkl. Passwoerter, ohne Zaehler)
void handleBackup() {
  NEED_ADMIN;
  String j = "{\"tueroeffner\":1";
  j += jStr("fw", FW_VERSION);
  j += jNum("dur", buzzerSeconds);
  j += jStr("dial", dialList);
  j += jStr("pin", dtmfPin);
  j += jNum("auto", callOnRing);
  j += jStr("sipserver", sipServer);
  j += jNum("sipport", sipPort);
  j += jStr("sipuser", sipUser);
  j += jStr("sippw", sipPw);
  j += jStr("mqttserver", mqttServer);
  j += jNum("mqttport", mqttPort);
  j += jStr("mqttuser", mqttUser);
  j += jStr("mqttpw", mqttPw);
  j += jStr("webuser", webUser);
  j += jStr("webpw", webPw);
  j += jStr("opuser", opUser);
  j += jStr("oppw", opPw);
  j += jNum("quiet", quietOn);
  j += jNum("qfrom", quietFrom);
  j += jNum("qto", quietTo);
  j += jNum("praxis", praxisOn);
  j += jNum("pdays", praxisDays);
  j += jNum("pfrom", praxisFrom);
  j += jNum("pto", praxisTo);
  j += jNum("door", doorOn);
  j += jNum("doorinv", doorInvert);
  j += jNum("dooralert", doorAlertMin);
  j += jNum("pushtype", pushType);
  j += jNum("pushev", pushEvents);
  j += jStr("pushserver", pushServer);
  j += jStr("pushtopic", pushTopic);
  j += jStr("pushtoken", pushToken);
  j += jNum("staticip", staticIp);
  j += jStr("ip", ipAddr.toString());
  j += jStr("mask", ipMask.toString());
  j += jStr("gw", ipGw.toString());
  j += jStr("dns", ipDns.toString());
  j += "}";
  server.sendHeader("Content-Disposition", "attachment; filename=\"tueroeffner-" + devId + ".json\"");
  server.send(200, "application/json", j);
}

// Sicherung einspielen: Felder kommen als Formular (aus der JSON-Datei)
void handleRestore() {
  NEED_ADMIN;
  long v;
  if (argInt("dur", MIN_BUZZER_SECONDS, MAX_BUZZER_SECONDS, v)) buzzerSeconds = (uint8_t)v;
  if (server.hasArg("dial") && dialListValid(server.arg("dial"))) dialList = server.arg("dial");
  if (server.hasArg("pin") && pinValid(server.arg("pin"))) dtmfPin = server.arg("pin");
  if (argInt("auto", 0, 1, v)) callOnRing = v;
  if (server.hasArg("sipserver")) sipServer = server.arg("sipserver").substring(0, 40);
  if (argInt("sipport", 1, 65535, v)) sipPort = (uint16_t)v;
  if (server.hasArg("sipuser")) sipUser = server.arg("sipuser").substring(0, 24);
  if (server.hasArg("sippw"))   sipPw   = server.arg("sippw").substring(0, 32);
  if (server.hasArg("mqttserver")) mqttServer = server.arg("mqttserver").substring(0, 40);
  if (argInt("mqttport", 1, 65535, v)) mqttPort = (uint16_t)v;
  if (server.hasArg("mqttuser")) mqttUser = server.arg("mqttuser").substring(0, 32);
  if (server.hasArg("mqttpw"))   mqttPw   = server.arg("mqttpw").substring(0, 32);
  if (server.arg("webuser").length()) webUser = server.arg("webuser").substring(0, 24);
  if (server.hasArg("webpw")) webPw = server.arg("webpw").substring(0, 32);
  if (server.arg("opuser").length())  opUser  = server.arg("opuser").substring(0, 24);
  if (server.hasArg("oppw"))  opPw  = server.arg("oppw").substring(0, 32);
  if (argInt("quiet", 0, 1, v)) quietOn = v;
  if (argInt("qfrom", 0, 24 * 60 - 1, v)) quietFrom = (uint16_t)v;
  if (argInt("qto",   0, 24 * 60 - 1, v)) quietTo   = (uint16_t)v;
  if (argInt("praxis", 0, 1, v)) praxisOn = v;
  if (argInt("pdays", 0, 0x7F, v)) praxisDays = (uint8_t)v;
  if (argInt("pfrom", 0, 24 * 60 - 1, v)) praxisFrom = (uint16_t)v;
  if (argInt("pto",   0, 24 * 60 - 1, v)) praxisTo   = (uint16_t)v;
  if (argInt("door", 0, 1, v)) doorOn = v;
  if (argInt("doorinv", 0, 1, v)) doorInvert = v;
  if (argInt("dooralert", 0, 1440, v)) doorAlertMin = (uint16_t)v;
  if (argInt("pushtype", PUSH_OFF, PUSH_TELEGRAM, v)) pushType = (uint8_t)v;
  if (argInt("pushev", 0, 7, v)) pushEvents = (uint8_t)v;
  if (server.hasArg("pushserver")) pushServer = server.arg("pushserver").substring(0, 64);
  if (server.hasArg("pushtopic"))  pushTopic  = server.arg("pushtopic").substring(0, 64);
  if (server.hasArg("pushtoken"))  pushToken  = server.arg("pushtoken").substring(0, 64);
  IPAddress ip;
  if (argIp("ip", ip))   ipAddr = ip;
  if (argIp("mask", ip)) ipMask = ip;
  if (argIp("gw", ip))   ipGw   = ip;
  if (argIp("dns", ip))  ipDns  = ip;
  // feste IP nur mit vollstaendigen Angaben
  if (argInt("staticip", 0, 1, v)) staticIp = v && (uint32_t)ipAddr && (uint32_t)ipGw && (uint32_t)ipMask;
  if (webPw.length() == 0) opPw = "";
  saveSettings();
  sendOk();
  restartReason = RR_RESTORE;
  restartAt     = millis() + 500;
}

// Firmware-Upload: Daten kommen in Stuecken ueber handleUpdateUpload()
bool updateAuthOk = false;

void handleUpdateUpload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    updateAuthOk = currentRole() == ROLE_ADMIN;
    if (!updateAuthOk) return;
    Serial.printf("Update: %s\n", up.filename.c_str());
    stopBuzzer();
    stopChain();
    aSip.Hangup();
#if defined(ESP32)
    Update.begin(UPDATE_SIZE_UNKNOWN);
#else
    Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000);
#endif
  } else if (!updateAuthOk) {
    return;
  } else if (up.status == UPLOAD_FILE_WRITE) {
    feedWatchdog();
    if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_END) {
    if (!Update.end(true)) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.end();
  }
}

void handleUpdateDone() {
  if (!updateAuthOk) {
    server.requestAuthentication(BASIC_AUTH, "Tueroeffner");
    return;
  }
  if (Update.hasError() || !Update.isFinished()) {
    sendErr(500, "Update fehlgeschlagen (falsche Datei?)");
    return;
  }
  saveSettings();   // Zaehler sichern
  sendOk();
  restartReason = RR_UPDATE;
  restartAt     = millis() + 500;
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

void checkEmergencyReset();

void setup() {
#if defined(DOOR_ON_RX_PIN)
  Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);   // RX wird zum Tuerkontakt
#else
  Serial.begin(115200);
#endif
  delay(100);
  bootReason = describeBootReason();
  Serial.printf("Start: %s\n", bootReason.c_str());

  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_STATUS_LED, OUTPUT);
  setRelay(false);
  pinMode(PIN_SIGNAL, INPUT_PULLUP);
  pinMode(PIN_BTN_RING, INPUT_PULLUP);
  pinMode(PIN_BTN_BUZZER, INPUT_PULLUP);
  pinMode(PIN_DOOR, INPUT_PULLUP);

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

  checkEmergencyReset();

  WiFiManager wm;
  wm.setHostname(HOSTNAME);
  wm.setAPCallback(configModeCallback);
  wm.setConfigPortalTimeout(300);
  if (staticIp) wm.setSTAStaticIPConfig(ipAddr, ipGw, ipMask, ipDns);
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
  server.on("/log", handleLog);
  server.on("/open", HTTP_POST, handleOpen);
  server.on("/call", HTTP_POST, handleCall);
  server.on("/setduration", HTTP_POST, handleSetDuration);
  server.on("/setdial", HTTP_POST, handleSetDial);
  server.on("/setsip", HTTP_POST, handleSetSip);
  server.on("/setmqtt", HTTP_POST, handleSetMqtt);
  server.on("/setquiet", HTTP_POST, handleSetQuiet);
  server.on("/setpraxis", HTTP_POST, handleSetPraxis);
  server.on("/setdoor", HTTP_POST, handleSetDoor);
  server.on("/setpush", HTTP_POST, handleSetPush);
  server.on("/testpush", HTTP_POST, handleTestPush);
  server.on("/setweb", HTTP_POST, handleSetWeb);
  server.on("/setop", HTTP_POST, handleSetOp);
  server.on("/setnet", HTTP_POST, handleSetNet);
  server.on("/resetcounters", HTTP_POST, handleResetCounters);
  server.on("/restart", HTTP_POST, handleRestart);
  server.on("/wifireset", HTTP_POST, handleWifiReset);
  server.on("/backup", handleBackup);
  server.on("/restore", HTTP_POST, handleRestore);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.begin();

  // Uhrzeit per NTP (laeuft im Hintergrund; bis dahin ist timeValid() false)
#if defined(ESP32)
  configTzTime(TIME_ZONE, NTP_SERVER);
#else
  configTime(TIME_ZONE, NTP_SERVER);
#endif

  // OTA-Update aus PlatformIO (espota); startet auch mDNS -> http://tueroeffner.local
  ArduinoOTA.setHostname(HOSTNAME);
  // Nur setzen, wenn vorhanden: ein leeres Passwort wuerde als MD5("") trotzdem
  // eine Anmeldung verlangen. Aenderungen gelten fuer OTA erst nach Neustart.
  if (webPw.length()) ArduinoOTA.setPassword(webPw.c_str());
  ArduinoOTA.onStart([]() {
    stopBuzzer();
    stopChain();
    aSip.Hangup();
    saveSettings();   // Zaehler sichern
    writeRestartReason(RR_UPDATE);
  });
  ArduinoOTA.onProgress([](unsigned int, unsigned int) { feedWatchdog(); });
  ArduinoOTA.begin();
  MDNS.addService("http", "tcp", 80);

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

#if defined(ESP32)
  // Push-Mitteilungen in eigenem Task (TLS blockiert sonst die Loop)
  pushQueue = xQueueCreate(4, sizeof(PushMsg));
  xTaskCreatePinnedToCore(pushTask, "push", 12288, nullptr, 1, nullptr, 0);

  // Watchdog fuer die Loop: haengt sie laenger als LOOP_WATCHDOG_SEC -> Neustart
  esp_task_wdt_config_t wdt = { LOOP_WATCHDOG_SEC * 1000, 1, true };   // timeout, idle_core_mask, panic
  if (esp_task_wdt_reconfigure(&wdt) != ESP_OK) esp_task_wdt_init(&wdt);
  esp_task_wdt_add(nullptr);
#endif

  logEvent(EV_BOOT);
  lastActivityAt = millis();
  displayDirty = true;
}

// Notfall-Zugang: Klingel-Taster beim Einschalten PW_RESET_HOLD_MS halten
// -> Passwoerter der Weboberflaeche weg und wieder DHCP
void checkEmergencyReset() {
  if (digitalRead(PIN_BTN_RING) != LOW) return;
  Serial.println(F("Klingel-Taster gedrueckt: halten fuer Notfall-Reset (Passwoerter, DHCP)"));
  if (hasDisplay) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println(F("Taster halten:"));
    display.println(F("Passwoerter und"));
    display.println(F("feste IP werden"));
    display.println(F("geloescht"));
    display.display();
  }
  uint32_t t0 = millis();
  while (digitalRead(PIN_BTN_RING) == LOW) {
    if (millis() - t0 >= PW_RESET_HOLD_MS) {
      webPw    = "";
      opPw     = "";
      staticIp = false;
      saveSettings();
      Serial.println(F("Notfall-Reset: Passwoerter geloescht, DHCP aktiv"));
      if (hasDisplay) {
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println(F("Passwoerter weg,"));
        display.println(F("DHCP aktiv."));
        display.display();
      }
      delay(1500);
      return;
    }
    delay(10);
  }
}

// Selbstheilung: WLAN zu lange weg oder Speicher knapp -> Neustart
void selfHeal() {
  if (WiFi.status() != WL_CONNECTED) {
    if (!wifiLostAt) wifiLostAt = millis();
    else if (millis() - wifiLostAt > (uint32_t)WIFI_LOST_RESTART_MIN * 60000UL) {
      Serial.println(F("Selbstheilung: WLAN zu lange weg -> Neustart"));
      restartNow(RR_WIFI_LOST);
    }
  } else {
    wifiLostAt = 0;
  }
  if (ESP.getFreeHeap() < MIN_FREE_HEAP && !aSip.IsBusy() && !buzzerActive) {
    Serial.println(F("Selbstheilung: Speicher knapp -> Neustart"));
    restartNow(RR_LOW_HEAP);
  }
}

void updateDisplay();

void loop() {
  feedWatchdog();
  server.handleClient();
  ArduinoOTA.handle();   // auf dem ESP8266 auch mDNS
  aSip.Processing(acSipIn, sizeof(acSipIn));
  handleSignalInput();
  handleBuzzerButton();
  handleDoorContact();
  handleDtmf();
  chainLoop();
  mqttLoop();
  pushLoop();
  selfHeal();

  // Zaehler verzoegert sichern; nicht waehrend eines Anrufs (Flash-Schreiben
  // blockiert kurz und wuerde den Piepton stoeren)
  if (countersDirty && !aSip.IsBusy() && millis() - lastSettingsSave > COUNTER_SAVE_MS) {
    saveSettings();
  }

  // Geplanter Neustart (nach Update, Neustart-Button, WLAN-Reset, Sicherung, Netzwerk)
  if (restartAt && (int32_t)(millis() - restartAt) >= 0) {
    if (wifiResetDue) {
      WiFiManager wm;
      wm.resetSettings();
    }
    restartNow(restartReason);
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

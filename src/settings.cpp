// Einstellungen, Zaehler und Protokoll dauerhaft speichern.
//
// Alle Einstellungen stehen in EINER Tabelle (SETTINGS). Daraus entstehen:
//   - die Datei /settings.txt im LittleFS ("schluessel=wert" je Zeile),
//   - die Sicherung (JSON) und das Einspielen einer Sicherung.
// Neue Einstellungen: Variable anlegen, eine Zeile in SETTINGS ergaenzen - fertig.
// Zaehler und Protokoll liegen getrennt in /state.txt (werden oefter geschrieben).
// Geraete mit aelterer Firmware (EEPROM-Struct) werden beim ersten Start uebernommen.
#include "app.h"
#include <EEPROM.h>
#include <LittleFS.h>

// ------------------------------------------------------------
//  Laufzeit-Einstellungen (Defaults aus config.h)
// ------------------------------------------------------------
uint8_t  buzzerSeconds = DEFAULT_BUZZER_SECONDS;
String   dialList      = DEFAULT_DIAL_NR;
String   dtmfPin       = "";
bool     callOnRing    = true;
String   sipServer     = SIP_SERVER_IP;
uint16_t sipPort       = SIP_PORT;
String   sipUser       = SIP_USER;
String   sipPw         = SIP_PW;
String   mqttServer    = MQTT_SERVER;
uint16_t mqttPort      = MQTT_PORT;
String   mqttUser      = MQTT_USER;
String   mqttPw        = MQTT_PW;
String   webUser       = "admin";
String   webPw         = "";
String   opUser        = "tuer";
String   opPw          = "";
bool     quietOn       = false;
uint16_t quietFrom     = 22 * 60;     // Minuten seit Mitternacht
uint16_t quietTo       = 7 * 60;
bool     praxisOn      = false;
uint8_t  praxisDays    = 0x1F;        // Bit 0 = Montag ... Bit 6 = Sonntag
uint16_t praxisFrom    = 8 * 60;
uint16_t praxisTo      = 12 * 60;
uint16_t praxisFrom2   = 0;           // zweites Fenster, von == bis = aus
uint16_t praxisTo2     = 0;
String   praxisFree    = "";
bool     doorOn        = false;
bool     doorInvert    = false;
uint16_t doorAlertMin  = 0;
uint8_t  pushType      = PUSH_OFF;
uint8_t  pushEvents    = PUSH_EV_RING | PUSH_EV_DOOR;
String   pushServer    = "https://ntfy.sh";
String   pushTopic     = "";
String   pushToken     = "";
bool      staticIp     = false;
IPAddress ipAddr, ipGw, ipMask(255, 255, 255, 0), ipDns;
bool     incomingOn    = false;
String   guestCodes    = "";
String   syslogServer  = "";
String   apPw          = WIFI_AP_PASSWORD;

uint32_t signalCount = 0, buzzerTriggers = 0, callCount = 0;

static bool     fsOk          = false;
static bool     stateDirty    = false;
static uint32_t lastStateSave = 0;

static const char *SETTINGS_FILE = "/settings.txt";
static const char *SETTINGS_TMP  = "/settings.tmp";
static const char *STATE_FILE    = "/state.txt";
static const char *STATE_TMP     = "/state.tmp";

// ------------------------------------------------------------
//  Pruefungen
// ------------------------------------------------------------
bool pinValid(const String &p) {
  if (p.length() > 8) return false;
  for (size_t i = 0; i < p.length(); i++) if (!isdigit((uint8_t)p[i])) return false;
  return true;
}

static bool isSep(char c) { return c == ',' || c == ';' || c == ' '; }

// n-te Nummer der Rufkette ("100, 101; 102") -> false, wenn es sie nicht gibt
bool dialTarget(const String &list, uint8_t n, String &out) {
  uint8_t idx = 0;
  int i = 0, len = list.length();
  while (i < len) {
    while (i < len && isSep(list[i])) i++;
    int start = i;
    while (i < len && !isSep(list[i])) i++;
    if (i > start) {
      if (idx == n) { out = list.substring(start, i); return true; }
      idx++;
    }
  }
  return false;
}

// Rufkette pruefen: jede Nummer hoechstens MAX_DIAL_LEN Zeichen
bool dialListValid(const String &list) {
  if (list.length() > MAX_DIAL_LIST) return false;
  String nr;
  for (uint8_t i = 0; dialTarget(list, i, nr); i++)
    if (nr.length() > MAX_DIAL_LEN) return false;
  return true;
}

// Ein Eintrag der Gaestecodes: "Code:bisUnix:einmalig:Name"
bool parseGuest(const String &e, String &code, uint32_t &until, bool &once, String &name) {
  int a = e.indexOf(':'), b = e.indexOf(':', a + 1), c = e.indexOf(':', b + 1);
  if (a < 0 || b < 0 || c < 0) return false;
  code = e.substring(0, a);
  String u = e.substring(a + 1, b), o = e.substring(b + 1, c);
  name = e.substring(c + 1);
  if (code.length() < 4 || !pinValid(code)) return false;
  for (size_t i = 0; i < u.length(); i++) if (!isdigit((uint8_t)u[i])) return false;
  if (u.length() == 0 || u.length() > 10 || (o != "0" && o != "1")) return false;
  if (name.length() > 32 || name.indexOf(';') >= 0 || name.indexOf(':') >= 0) return false;
  until = strtoul(u.c_str(), nullptr, 10);
  once  = o == "1";
  return true;
}

bool guestsValid(const String &g) {
  if (g.length() > 300) return false;
  int n = 0, start = 0;
  while (start < (int)g.length()) {
    int end = g.indexOf(';', start);
    if (end < 0) end = g.length();
    String code, name; uint32_t until; bool once;
    if (!parseGuest(g.substring(start, end), code, until, once, name)) return false;
    if (++n > GUEST_CODES_MAX) return false;
    start = end + 1;
  }
  return true;
}

// "24.12." / "24.12" (jedes Jahr) oder "3.10.2026" (einmalig)
static bool parseFreeDay(const String &t, uint8_t &d, uint8_t &m, uint16_t &y) {
  int p1 = t.indexOf('.');
  if (p1 <= 0) return false;
  int p2 = t.indexOf('.', p1 + 1);
  d = t.substring(0, p1).toInt();
  m = (p2 < 0 ? t.substring(p1 + 1) : t.substring(p1 + 1, p2)).toInt();
  y = p2 < 0 || p2 + 1 >= (int)t.length() ? 0 : t.substring(p2 + 1).toInt();
  if (p2 >= 0 && p2 + 1 < (int)t.length() && (y < 2000 || y > 2099)) return false;
  return d >= 1 && d <= 31 && m >= 1 && m <= 12;
}

// Ruft fn fuer jeden Eintrag der Ausnahmetage auf; false bei Formatfehler
template <typename F> static bool forEachFreeDay(const String &s, F fn) {
  int i = 0, len = s.length(), n = 0;
  while (i < len) {
    while (i < len && isSep(s[i])) i++;
    int start = i;
    while (i < len && !isSep(s[i])) i++;
    if (i > start) {
      uint8_t d, m; uint16_t y;
      if (!parseFreeDay(s.substring(start, i), d, m, y) || ++n > 24) return false;
      if (fn(d, m, y)) return true;
    }
  }
  return true;
}

bool freeDaysValid(const String &s) {
  return s.length() <= 160 && forEachFreeDay(s, [](uint8_t, uint8_t, uint16_t) { return false; });
}

bool isFreeDay(int d, int m, int y) {
  bool hit = false;
  forEachFreeDay(praxisFree, [&](uint8_t fd, uint8_t fm, uint16_t fy) {
    hit = fd == d && fm == m && (fy == 0 || fy == y);
    return hit;
  });
  return hit;
}

// Einrichtungs-WLAN: leer = offen, sonst 8-32 Zeichen (WPA2)
bool apPwValid(const String &p) {
  return p.length() == 0 || (p.length() >= 8 && p.length() <= 32);
}

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

// ------------------------------------------------------------
//  Tabelle aller Einstellungen
// ------------------------------------------------------------
enum SetType : uint8_t { ST_STR, ST_U8, ST_U16, ST_BOOL, ST_IP };
struct SettingDef {
  const char *key;
  SetType     type;
  void       *ptr;
  long        lo, hi;                     // Zahlen: Bereich; Texte: Laenge von..bis
  bool      (*valid)(const String &);     // zusaetzliche Pruefung oder nullptr
};

static const SettingDef SETTINGS[] = {
  {"dur",        ST_U8,   &buzzerSeconds, MIN_BUZZER_SECONDS, MAX_BUZZER_SECONDS, nullptr},
  {"dial",       ST_STR,  &dialList,      0, MAX_DIAL_LIST, dialListValid},
  {"pin",        ST_STR,  &dtmfPin,       0, 8,     pinValid},
  {"auto",       ST_BOOL, &callOnRing,    0, 1,     nullptr},
  {"sipserver",  ST_STR,  &sipServer,     0, 40,    nullptr},
  {"sipport",    ST_U16,  &sipPort,       1, 65535, nullptr},
  {"sipuser",    ST_STR,  &sipUser,       0, 24,    nullptr},
  {"sippw",      ST_STR,  &sipPw,         0, 32,    nullptr},
  {"mqttserver", ST_STR,  &mqttServer,    0, 40,    nullptr},
  {"mqttport",   ST_U16,  &mqttPort,      1, 65535, nullptr},
  {"mqttuser",   ST_STR,  &mqttUser,      0, 32,    nullptr},
  {"mqttpw",     ST_STR,  &mqttPw,        0, 32,    nullptr},
  {"webuser",    ST_STR,  &webUser,       1, 24,    nullptr},
  {"webpw",      ST_STR,  &webPw,         0, 32,    nullptr},
  {"opuser",     ST_STR,  &opUser,        1, 24,    nullptr},
  {"oppw",       ST_STR,  &opPw,          0, 32,    nullptr},
  {"quiet",      ST_BOOL, &quietOn,       0, 1,     nullptr},
  {"qfrom",      ST_U16,  &quietFrom,     0, 1439,  nullptr},
  {"qto",        ST_U16,  &quietTo,       0, 1439,  nullptr},
  {"praxis",     ST_BOOL, &praxisOn,      0, 1,     nullptr},
  {"pdays",      ST_U8,   &praxisDays,    0, 0x7F,  nullptr},
  {"pfrom",      ST_U16,  &praxisFrom,    0, 1439,  nullptr},
  {"pto",        ST_U16,  &praxisTo,      0, 1439,  nullptr},
  {"pfrom2",     ST_U16,  &praxisFrom2,   0, 1439,  nullptr},
  {"pto2",       ST_U16,  &praxisTo2,     0, 1439,  nullptr},
  {"pfree",      ST_STR,  &praxisFree,    0, 160,   freeDaysValid},
  {"door",       ST_BOOL, &doorOn,        0, 1,     nullptr},
  {"doorinv",    ST_BOOL, &doorInvert,    0, 1,     nullptr},
  {"dooralert",  ST_U16,  &doorAlertMin,  0, 1440,  nullptr},
  {"pushtype",   ST_U8,   &pushType,      PUSH_OFF, PUSH_TELEGRAM, nullptr},
  {"pushev",     ST_U8,   &pushEvents,    0, 7,     nullptr},
  {"pushserver", ST_STR,  &pushServer,    0, 64,    nullptr},
  {"pushtopic",  ST_STR,  &pushTopic,     0, 64,    nullptr},
  {"pushtoken",  ST_STR,  &pushToken,     0, 64,    nullptr},
  {"staticip",   ST_BOOL, &staticIp,      0, 1,     nullptr},
  {"ip",         ST_IP,   &ipAddr,        0, 0,     nullptr},
  {"mask",       ST_IP,   &ipMask,        0, 0,     nullptr},
  {"gw",         ST_IP,   &ipGw,          0, 0,     nullptr},
  {"dns",        ST_IP,   &ipDns,         0, 0,     nullptr},
  {"incoming",   ST_BOOL, &incomingOn,    0, 1,     nullptr},
  {"guests",     ST_STR,  &guestCodes,    0, 300,   guestsValid},
  {"syslog",     ST_STR,  &syslogServer,  0, 40,    nullptr},
  {"appw",       ST_STR,  &apPw,          0, 32,    apPwValid},
};

// Wert pruefen und (wenn apply) uebernehmen
static bool applySetting(const SettingDef &d, const String &v, bool apply) {
  switch (d.type) {
    case ST_STR:
      if ((long)v.length() < d.lo || (long)v.length() > d.hi) return false;
      if (d.valid && !d.valid(v)) return false;
      if (apply) *(String *)d.ptr = v;
      return true;
    case ST_BOOL:
      if (v != "0" && v != "1" && v != "true" && v != "false") return false;
      if (apply) *(bool *)d.ptr = (v == "1" || v == "true");
      return true;
    case ST_U8:
    case ST_U16: {
      if (v.length() == 0 || v.length() > 6) return false;
      for (size_t i = 0; i < v.length(); i++) if (!isdigit((uint8_t)v[i])) return false;
      long n = v.toInt();
      if (n < d.lo || n > d.hi) return false;
      if (apply) {
        if (d.type == ST_U8) *(uint8_t *)d.ptr = (uint8_t)n;
        else                 *(uint16_t *)d.ptr = (uint16_t)n;
      }
      return true;
    }
    case ST_IP: {
      IPAddress ip;
      if (!ip.fromString(v)) return false;
      if (apply) *(IPAddress *)d.ptr = ip;
      return true;
    }
  }
  return false;
}

static String settingValue(const SettingDef &d) {
  switch (d.type) {
    case ST_STR:  return *(String *)d.ptr;
    case ST_BOOL: return *(bool *)d.ptr ? "1" : "0";
    case ST_U8:   return String(*(uint8_t *)d.ptr);
    case ST_U16:  return String(*(uint16_t *)d.ptr);
    case ST_IP:   return ((IPAddress *)d.ptr)->toString();
  }
  return String();
}

// Abhaengigkeiten zwischen Einstellungen herstellen
static void sanitize() {
  if (webPw.length() == 0) opPw = "";                 // ohne Admin-Passwort ist ohnehin alles offen
  if (opUser == webUser) opPw = "";
  if (staticIp && (!(uint32_t)ipAddr || !(uint32_t)ipGw || !(uint32_t)ipMask)) staticIp = false;
  if (!(uint32_t)ipDns) ipDns = ipGw;
  if (pushServer.length() == 0) pushServer = "https://ntfy.sh";
}

// ------------------------------------------------------------
//  Datei-Format: "schluessel=wert" je Zeile, % \r \n im Wert maskiert
// ------------------------------------------------------------
static void writeKv(File &f, const char *key, const String &v) {
  f.print(key);
  f.print('=');
  for (size_t i = 0; i < v.length(); i++) {
    char c = v[i];
    if (c == '%')       f.print("%25");
    else if (c == '\n') f.print("%0A");
    else if (c == '\r') f.print("%0D");
    else                f.print(c);
  }
  f.print('\n');
}

static String unescape(const String &v) {
  String r;
  r.reserve(v.length());
  for (size_t i = 0; i < v.length(); i++) {
    if (v[i] == '%' && i + 2 < v.length()) {
      r += (char)strtol(v.substring(i + 1, i + 3).c_str(), nullptr, 16);
      i += 2;
    } else {
      r += v[i];
    }
  }
  return r;
}

// Datei zeilenweise lesen und fn(key, value) aufrufen
template <typename F> static bool readKvFile(const char *path, F fn) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    int eq = line.indexOf('=');
    if (eq <= 0) continue;
    fn(line.substring(0, eq), unescape(line.substring(eq + 1)));
  }
  f.close();
  return true;
}

// Sicher schreiben: erst Hilfsdatei, dann umbenennen (Stromausfall-fest)
template <typename F> static bool writeKvFile(const char *path, const char *tmp, F fn) {
  if (!fsOk) return false;
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;
  fn(f);
  f.close();
  LittleFS.remove(path);
  return LittleFS.rename(tmp, path);
}

// Fehlt die Hauptdatei (Stromausfall beim Umbenennen), gilt die Hilfsdatei
static const char *existingFile(const char *path, const char *tmp) {
  if (LittleFS.exists(path)) return path;
  if (LittleFS.exists(tmp))  return tmp;
  return nullptr;
}

void saveSettings() {
  bool ok = writeKvFile(SETTINGS_FILE, SETTINGS_TMP, [](File &f) {
    for (const SettingDef &d : SETTINGS) writeKv(f, d.key, settingValue(d));
  });
  if (!ok) logMsg("Einstellungen: Speichern fehlgeschlagen");
}

void saveState() {
  // Eintraege ohne Uhrzeit (vor dem NTP-Abgleich) jetzt nachrechnen, sonst gehen sie verloren
  bool   synced = timeValid();
  time_t now    = time(nullptr);
  String lg;
  for (uint8_t i = 0; i < logCount; i++) {
    const LogEntry &e = eventLog[(logHead + LOG_SIZE - logCount + i) % LOG_SIZE];   // aelteste zuerst
    uint32_t t = e.t ? e.t : (synced ? (uint32_t)(now - (millis() - e.at) / 1000) : 0);
    if (!t) continue;
    if (lg.length()) lg += ',';
    lg += String(t) + ':' + String(e.type) + ':' + e.detail;   // Detail ohne ',' und ':'
  }
  writeKvFile(STATE_FILE, STATE_TMP, [&](File &f) {
    writeKv(f, "rings",    String(signalCount));
    writeKv(f, "openings", String(buzzerTriggers));
    writeKv(f, "calls",    String(callCount));
    writeKv(f, "log",      lg);
  });
  stateDirty    = false;
  lastStateSave = millis();
}

void stateChanged() {
  stateDirty = true;
}

// Zaehler/Protokoll verzoegert sichern; nicht waehrend eines Anrufs (Flash-Schreiben
// blockiert kurz und wuerde den Piepton stoeren)
void settingsLoop() {
  if (stateDirty && !aSip.IsBusy() && millis() - lastStateSave > COUNTER_SAVE_MS) saveState();
}

static void loadState() {
  const char *path = existingFile(STATE_FILE, STATE_TMP);
  if (!path) return;
  readKvFile(path, [](const String &k, const String &v) {
    if (k == "rings")         signalCount    = strtoul(v.c_str(), nullptr, 10);
    else if (k == "openings") buzzerTriggers = strtoul(v.c_str(), nullptr, 10);
    else if (k == "calls")    callCount      = strtoul(v.c_str(), nullptr, 10);
    else if (k == "log") {
      int start = 0;
      while (start < (int)v.length()) {
        int end = v.indexOf(',', start);
        if (end < 0) end = v.length();
        // "zeit:typ:detail"
        int c1 = v.indexOf(':', start);
        int c2 = c1 < 0 ? -1 : v.indexOf(':', c1 + 1);
        if (c1 > start && c1 < end) {
          bool hasDetail = c2 > c1 && c2 < end;
          logRestore(strtoul(v.substring(start, c1).c_str(), nullptr, 10),
                     (uint8_t)v.substring(c1 + 1, hasDetail ? c2 : end).toInt(),
                     hasDetail ? v.substring(c2 + 1, end) : String());
        }
        start = end + 1;
      }
    }
  });
}

// ------------------------------------------------------------
//  Uebernahme aus dem EEPROM-Struct (Firmware bis 1.3)
// ------------------------------------------------------------
namespace legacy {
static const uint8_t SETTINGS_MAGIC = 0x53, MQTT_MAGIC = 0x4D, EXT_MAGIC = 0x45, EXT2_MAGIC = 0x32;
struct Settings {
  uint8_t  magic;
  uint8_t  buzzerSeconds;
  uint8_t  callOnRing;
  uint16_t sipPort;
  char     dialNr[20 + 1];
  char     sipServer[41];
  char     sipUser[25];
  char     sipPw[33];
  uint8_t  mqttMagic;
  uint16_t mqttPort;
  char     mqttServer[41];
  char     mqttUser[33];
  char     mqttPw[33];
  uint8_t  extMagic;
  uint32_t signalCount;
  uint32_t buzzerTriggers;
  uint32_t callCount;
  char     webUser[25];
  char     webPw[33];
  uint8_t  quietOn;
  uint16_t quietFrom;
  uint16_t quietTo;
  uint8_t  ext2Magic;
  char     dialList[64 + 1];
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

#define LOAD_STR(field, var) do { s.field[sizeof(s.field) - 1] = '\0'; var = String(s.field); } while (0)

static bool load() {
  EEPROM.begin(sizeof(Settings) + 8);
  Settings s;
  EEPROM.get(0, s);
  EEPROM.end();
  if (s.magic != SETTINGS_MAGIC) return false;
  if (s.buzzerSeconds >= MIN_BUZZER_SECONDS && s.buzzerSeconds <= MAX_BUZZER_SECONDS)
    buzzerSeconds = s.buzzerSeconds;
  callOnRing = s.callOnRing != 0;
  if (s.sipPort > 0) sipPort = s.sipPort;
  LOAD_STR(dialNr,    dialList);
  LOAD_STR(sipServer, sipServer);
  LOAD_STR(sipUser,   sipUser);
  LOAD_STR(sipPw,     sipPw);
  if (s.mqttMagic != MQTT_MAGIC) return true;
  if (s.mqttPort > 0) mqttPort = s.mqttPort;
  LOAD_STR(mqttServer, mqttServer);
  LOAD_STR(mqttUser,   mqttUser);
  LOAD_STR(mqttPw,     mqttPw);
  if (s.extMagic != EXT_MAGIC) return true;
  signalCount    = s.signalCount;
  buzzerTriggers = s.buzzerTriggers;
  callCount      = s.callCount;
  s.webUser[sizeof(s.webUser) - 1] = '\0';
  if (s.webUser[0]) webUser = String(s.webUser);
  LOAD_STR(webPw, webPw);
  quietOn = s.quietOn != 0;
  if (s.quietFrom < 24 * 60) quietFrom = s.quietFrom;
  if (s.quietTo   < 24 * 60) quietTo   = s.quietTo;
  if (s.ext2Magic != EXT2_MAGIC) return true;
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
  doorAlertMin = s.doorAlertMin <= 1440 ? s.doorAlertMin : 0;
  if (s.pushType <= PUSH_TELEGRAM) pushType = s.pushType;
  pushEvents = s.pushEvents & 7;
  LOAD_STR(pushServer, pushServer);
  LOAD_STR(pushTopic,  pushTopic);
  LOAD_STR(pushToken,  pushToken);
  staticIp = s.staticIp != 0;
  ipAddr = IPAddress(s.ipAddr);
  ipGw   = IPAddress(s.ipGw);
  ipMask = IPAddress(s.ipMask);
  ipDns  = IPAddress(s.ipDns);
  return true;
}
#undef LOAD_STR
}  // namespace legacy

// ------------------------------------------------------------
//  Start
// ------------------------------------------------------------
void settingsBegin() {
#if defined(ESP32)
  fsOk = LittleFS.begin(true, "/littlefs", 10, "spiffs");   // Partition "spiffs" aus min_spiffs.csv
#else
  fsOk = LittleFS.begin();
  if (!fsOk && LittleFS.format()) fsOk = LittleFS.begin();
#endif
  if (!fsOk) {
    logMsg("Dateisystem nicht verfuegbar - Einstellungen aus dem EEPROM, Aenderungen gehen verloren");
    legacy::load();
    sanitize();
    return;
  }

  const char *path = existingFile(SETTINGS_FILE, SETTINGS_TMP);
  if (path) {
    readKvFile(path, [](const String &k, const String &v) {
      for (const SettingDef &d : SETTINGS)
        if (k == d.key) { applySetting(d, v, true); break; }
    });
    sanitize();
    loadState();
    return;
  }

  // Erster Start mit dieser Firmware: alte Einstellungen uebernehmen
  if (legacy::load()) logMsg("Einstellungen aus dem EEPROM uebernommen");
  sanitize();
  saveSettings();
  saveState();
}

// Alle Einstellungen als JSON (Sicherung, inkl. Passwoerter, ohne Zaehler)
String settingsJson() {
  String j;
  j.reserve(1400);
  j = "{\"tueroeffner\":1,\"fw\":\"" FW_VERSION "\"";
  for (const SettingDef &d : SETTINGS) {
    j += ",\"";
    j += d.key;
    j += "\":";
    bool num = d.type == ST_U8 || d.type == ST_U16 || d.type == ST_BOOL;
    if (num) j += settingValue(d);
    else     { j += '"'; j += jsonEsc(settingValue(d)); j += '"'; }
  }
  j += "}";
  return j;
}

// Sicherung einspielen: gueltige Felder uebernehmen, ungueltige in err auflisten
bool restoreFromArgs(WEB_SERVER_CLASS &srv, String &err) {
  int n = 0;
  for (const SettingDef &d : SETTINGS) {
    if (!srv.hasArg(d.key)) continue;
    if (applySetting(d, srv.arg(d.key), true)) n++;
    else { if (err.length()) err += ", "; err += d.key; }
  }
  sanitize();
  saveSettings();
  return n > 0;
}

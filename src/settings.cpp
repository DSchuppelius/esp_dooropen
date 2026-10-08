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
String   ringCallers   = "";
String   praxisHoliday = "";
bool     relay2On      = false;
String   relay2Name    = "Tor";
uint8_t  relay2Seconds = DEFAULT_BUZZER_SECONDS;
bool     relay2User    = false;
String   users         = "";
bool     tgOpen        = false;
String   tgChats       = "";
bool     wgOn          = false;
String   wgCards       = "";
bool     hkOn          = false;
String   hkCode        = "";

uint32_t signalCount = 0, buzzerTriggers = 0, callCount = 0;

static bool     fsOk          = false;
static uint8_t  fsFlags       = 0;       // FS_* (app.h): Formatiert, Notbetrieb, Fehler
static bool     stateDirty    = false;
static bool     settingsDirty = false;   // Speichern fehlgeschlagen -> spaeter erneut
static uint32_t lastStateSave = 0;
static uint32_t lastSettingsTry = 0;

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

// Bundesland fuer Feiertage: leer oder ein Kuerzel
static bool holidayStateValid(const String &s) {
  static const char *const STATES[] = { "BW", "BY", "BE", "BB", "HB", "HH", "HE", "MV",
                                        "NI", "NW", "RP", "SL", "SN", "ST", "SH", "TH" };
  if (s.length() == 0) return true;
  for (const char *st : STATES) if (s == st) return true;
  return false;
}

// Klingel-Rufnummern: Liste wie die Rufkette oder "*"
static bool ringCallersValid(const String &s) {
  return s == "*" || dialListValid(s);
}

// Feld fuer Listen (Benutzer): % : ; als %XX
String urlEncodeField(const String &v) {
  static const char hex[] = "0123456789ABCDEF";
  String r;
  for (size_t i = 0; i < v.length(); i++) {
    char c = v[i];
    if (c == '%' || c == ':' || c == ';' || (uint8_t)c < 0x20) { r += '%'; r += hex[(uint8_t)c >> 4]; r += hex[c & 15]; }
    else r += c;
  }
  return r;
}

String urlDecode(const String &v) {
  String r;
  r.reserve(v.length());
  for (size_t i = 0; i < v.length(); i++) {
    if (v[i] == '%' && i + 2 < v.length() && isxdigit((uint8_t)v[i + 1]) && isxdigit((uint8_t)v[i + 2])) {
      r += (char)strtol(v.substring(i + 1, i + 3).c_str(), nullptr, 16);
      i += 2;
    } else {
      r += v[i];
    }
  }
  return r;
}

// Weitere Benutzer: "Name:Passwort:Rolle:Tage:Von:Bis;..." (Felder %-kodiert).
// Rolle 1 = Tuer, 2 = Admin; Tage Bit 0 = Montag (0 = alle); Von == Bis = immer.
static bool parseUser(const String &e, UserEntry &u) {
  String f[6];
  int start = 0;
  for (int i = 0; i < 6; i++) {
    int end = i < 5 ? e.indexOf(':', start) : e.length();
    if (end < 0) return false;
    f[i] = e.substring(start, end);
    start = end + 1;
  }
  u.name = urlDecode(f[0]);
  u.pw   = urlDecode(f[1]);
  long role = f[2].toInt(), days = f[3].toInt(), from = f[4].toInt(), to = f[5].toInt();
  if (u.name.length() < 1 || u.name.length() > 24 || u.pw.length() < 1 || u.pw.length() > 32) return false;
  if ((role != 1 && role != 2) || days < 0 || days > 127 || from < 0 || from > 1439 || to < 0 || to > 1439) return false;
  u.role = (uint8_t)role;
  u.days = (uint8_t)days;
  u.from = (uint16_t)from;
  u.to   = (uint16_t)to;
  return true;
}

bool userAt(uint8_t idx, UserEntry &u) {
  int start = 0;
  uint8_t i = 0;
  while (start < (int)users.length()) {
    int end = users.indexOf(';', start);
    if (end < 0) end = users.length();
    if (i == idx) return parseUser(users.substring(start, end), u);
    i++;
    start = end + 1;
  }
  return false;
}

bool usersValid(const String &s) {
  if (s.length() > 900) return false;
  int start = 0, n = 0;
  String names = ";";
  while (start < (int)s.length()) {
    int end = s.indexOf(';', start);
    if (end < 0) end = s.length();
    UserEntry u;
    if (!parseUser(s.substring(start, end), u) || ++n > USERS_MAX) return false;
    String key = ";" + u.name + ";";
    if (names.indexOf(key) >= 0) return false;   // Namen eindeutig
    names += u.name + ";";
    start = end + 1;
  }
  return true;
}

// Karten fuer den RFID-Leser: "Nummer:Name;..."
bool cardsValid(const String &s) {
  if (s.length() > 700) return false;
  int start = 0, n = 0;
  while (start < (int)s.length()) {
    int end = s.indexOf(';', start);
    if (end < 0) end = s.length();
    String e = s.substring(start, end);
    int c = e.indexOf(':');
    if (c < 1 || c > 10 || ++n > 25) return false;
    for (int i = 0; i < c; i++) if (!isdigit((uint8_t)e[i])) return false;
    if (e.length() - c - 1 > 24 || e.indexOf(':', c + 1) >= 0) return false;
    start = end + 1;
  }
  return true;
}

// HomeKit-Kopplungscode: 8 Ziffern, keine trivialen Folgen (verbietet Apple)
static bool hkCodeValid(const String &c) {
  if (c.length() == 0) return true;
  if (c.length() != 8 || !pinValid(c)) return false;
  static const char *const BAD[] = { "00000000", "11111111", "22222222", "33333333", "44444444", "55555555",
                                     "66666666", "77777777", "88888888", "99999999", "12345678", "87654321" };
  for (const char *b : BAD) if (c == b) return false;
  return true;
}

// Feste IP: passt alles zusammen? Sonst ist das Geraet nach dem Neustart nicht erreichbar.
static uint32_t ipHost(const IPAddress &a) {   // Bytes in Rechenreihenfolge (a[0] oben)
  return (uint32_t)a[0] << 24 | (uint32_t)a[1] << 16 | (uint32_t)a[2] << 8 | a[3];
}

static bool ipUnicast(uint32_t a) {   // nicht 0.x, 127.x, Multicast oder Broadcast
  uint8_t first = a >> 24;
  return first != 0 && first != 127 && first < 224;
}

const __FlashStringHelper *staticIpError(const IPAddress &ip, const IPAddress &mask,
                                         const IPAddress &gw, const IPAddress &dns) {
  uint32_t i = ipHost(ip), m = ipHost(mask), g = ipHost(gw), host = ~m;
  // Maske zusammenhaengend (Einsen vorne), mindestens 2 Adressen fuer Geraete (/30)
  if (m == 0 || (host & (host + 1)) || host < 3) return F("Subnetzmaske ungültig (z. B. 255.255.255.0)");
  if (!ipUnicast(i)) return F("IP-Adresse ungültig");
  if ((i & host) == 0 || (i & host) == host) return F("IP-Adresse ist die Netz- oder Broadcast-Adresse");
  if (!ipUnicast(g) || (g & host) == 0 || (g & host) == host) return F("Gateway ungültig");
  if ((i & m) != (g & m)) return F("Gateway liegt nicht im Netz der IP-Adresse (Subnetzmaske?)");
  if (i == g) return F("IP-Adresse und Gateway sind gleich");
  if (!ipUnicast(ipHost(dns))) return F("DNS-Server ungültig");
  return nullptr;
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

static const SettingDef SETTINGS[] PROGMEM = {   // im Flash (RAM sparen, ESP8266)
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
  {"pushev",     ST_U8,   &pushEvents,    0, 15,    nullptr},
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
  {"ringcall",   ST_STR,  &ringCallers,   0, 64,    ringCallersValid},
  {"pholiday",   ST_STR,  &praxisHoliday, 0, 2,     holidayStateValid},
  {"r2on",       ST_BOOL, &relay2On,      0, 1,     nullptr},
  {"r2name",     ST_STR,  &relay2Name,    1, 16,    nullptr},
  {"r2dur",      ST_U8,   &relay2Seconds, MIN_BUZZER_SECONDS, MAX_BUZZER_SECONDS, nullptr},
  {"r2user",     ST_BOOL, &relay2User,    0, 1,     nullptr},
  {"users",      ST_STR,  &users,         0, 900,   usersValid},
  {"tgopen",     ST_BOOL, &tgOpen,        0, 1,     nullptr},
  {"tgchats",    ST_STR,  &tgChats,       0, 100,   nullptr},
  {"wgon",       ST_BOOL, &wgOn,          0, 1,     nullptr},
  {"cards",      ST_STR,  &wgCards,       0, 700,   cardsValid},
  {"hkon",       ST_BOOL, &hkOn,          0, 1,     nullptr},
  {"hkcode",     ST_STR,  &hkCode,        0, 8,     hkCodeValid},
};

// Tabelle aus dem Flash lesen: for (SettingIter it; it.next(d);) ...
static const size_t SETTINGS_N = sizeof(SETTINGS) / sizeof(SETTINGS[0]);
struct SettingIter {
  size_t i = 0;
  bool next(SettingDef &d) {
    if (i >= SETTINGS_N) return false;
    memcpy_P(&d, &SETTINGS[i++], sizeof(d));
    return true;
  }
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
// Zieldatei beim Schreiben; zaehlt die Bytes, die print() angenommen hat
struct KvOut {
  File   f;
  size_t len = 0;
  bool   ok  = true;    // alle Zeilen vollstaendig angenommen?
};

static void writeKv(KvOut &o, const char *key, const String &v) {
  String line;
  line.reserve(strlen(key) + v.length() + 8);
  line = key;
  line += '=';
  for (size_t i = 0; i < v.length(); i++) {
    char c = v[i];
    if (c == '%')       line += F("%25");
    else if (c == '\n') line += F("%0A");
    else if (c == '\r') line += F("%0D");
    else                line += c;
  }
  line += '\n';
  size_t n = o.f.print(line);
  o.len += n;
  if (n != line.length()) o.ok = false;
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
  if (!LittleFS.exists(path)) return false;
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

// Sicher schreiben: erst Hilfsdatei, pruefen, dann umbenennen. rename() ersetzt die alte
// Datei in einem Schritt (beide Cores rufen lfs_rename auf) - nach einem Stromausfall gilt
// also die alte oder die neue Datei, nie eine halbe. Geht beim Schreiben etwas schief
// (Speicher voll, Flash-Fehler), bleibt die alte Datei unangetastet.
template <typename F> static bool writeKvFileOnce(const char *path, const char *tmp, F fn) {
  if (LittleFS.exists(tmp)) LittleFS.remove(tmp);   // Reste koennten zufaellig gleich gross sein
  KvOut o;
  o.f = LittleFS.open(tmp, "w");
  if (!o.f) {
    logMsg("Speichern: %s laesst sich nicht anlegen", tmp);
    return false;
  }
  fn(o);
  o.f.close();   // erst hier landet der Rest im Flash, Fehler meldet close() nicht ...
  File chk = LittleFS.open(tmp, "r");   // ... darum die Groesse nachpruefen
  size_t size = chk ? chk.size() : 0;
  if (chk) chk.close();
  if (o.ok && o.len && size == o.len && LittleFS.rename(tmp, path)) return true;
  LittleFS.remove(tmp);
  logMsg("Speichern von %s fehlgeschlagen (%u von %u Bytes) - alte Datei bleibt",
         path, (unsigned)size, (unsigned)o.len);
  return false;
}

template <typename F> static bool writeKvFile(const char *path, const char *tmp, F fn) {
  if (!fsOk) return false;
  if (writeKvFileOnce(path, tmp, fn)) return true;
  // Einstellungen gehen vor: aelteren Verlauf loeschen und noch einmal versuchen
  return eventsFreeSpace() && writeKvFileOnce(path, tmp, fn);
}

// Hauptdatei laden; fehlt sie oder ist sie leer/kaputt (Stromausfall, Speicherfehler),
// die Hilfsdatei - die wird dann gleich zur Hauptdatei. load() liefert die Anzahl
// gueltiger Eintraege. Ergebnis: etwas geladen?
static bool loadKvFile(const char *path, const char *tmp, int (*load)(const char *)) {
  if (load(path) > 0) return true;
  if (load(tmp) <= 0) return false;
  logMsg("%s fehlt oder ist leer - Hilfsdatei %s verwendet", path, tmp);
  LittleFS.rename(tmp, path);
  return true;
}

bool saveSettings() {
  bool ok = writeKvFile(SETTINGS_FILE, SETTINGS_TMP, [](KvOut &o) {
    SettingIter it;
    SettingDef  d;
    while (it.next(d)) writeKv(o, d.key, settingValue(d));
  });
  settingsDirty   = !ok && fsOk;   // im RAM gilt die Aenderung - spaeter erneut versuchen
  lastSettingsTry = millis();
  if (ok) fsFlags &= ~FS_SAVE_ERR;
  else    { fsFlags |= FS_SAVE_ERR; logMsg("Einstellungen: Speichern fehlgeschlagen"); }
  return ok;
}

void saveState() {
  eventsFlush(true);   // langen Verlauf mitsichern (auch vor Neustart und Update)
  String lg;
  for (uint8_t i = 0; i < logCount; i++) {
    const LogEntry &e = eventLog[(logHead + LOG_SIZE - logCount + i) % LOG_SIZE];   // aelteste zuerst
    // Eintraege ohne Uhrzeit (vor dem NTP-Abgleich) jetzt nachrechnen; ohne NTP mit 0
    // sichern, nach einem Neustart erscheinen sie als "ohne Uhrzeit"
    if (lg.length()) lg += ',';
    lg += String(logEntryTime(e)) + ':' + String(e.type) + ':' + e.detail;   // Detail ohne ',' und ':'
  }
  bool ok = writeKvFile(STATE_FILE, STATE_TMP, [&](KvOut &o) {
    writeKv(o, "rings",    String(signalCount));
    writeKv(o, "openings", String(buzzerTriggers));
    writeKv(o, "calls",    String(callCount));
    writeKv(o, "log",      lg);
  });
  stateDirty    = !ok && fsOk;   // fehlgeschlagen: nach COUNTER_SAVE_MS erneut
  lastStateSave = millis();
}

void stateChanged() {
  stateDirty = true;
}

// Zaehler/Protokoll verzoegert sichern; nicht waehrend eines Anrufs (Flash-Schreiben
// blockiert kurz und wuerde den Piepton stoeren)
void settingsLoop() {
  if (stateDirty && !aSip.IsBusy() && millis() - lastStateSave > COUNTER_SAVE_MS) saveState();
  if (settingsDirty && !aSip.IsBusy() && millis() - lastSettingsTry > COUNTER_SAVE_MS) saveSettings();
  eventsFlush();   // langer Verlauf (gebuendelt, nicht waehrend eines Anrufs)
}

uint8_t fsStatus() {
  return fsFlags;
}

// Probleme mit dem Speicher melden (beim Start seriell, spaeter auch per Syslog)
void fsReport() {
  if (fsFlags & FS_FAILED)    logMsg("Speicher: Dateisystem nicht verfuegbar - Notbetrieb, Aenderungen gehen beim Neustart verloren");
  if (fsFlags & FS_FORMATTED) logMsg("Speicher: Dateisystem war nicht lesbar und wurde beim Start formatiert");
  if (fsFlags & FS_DAMAGED)   logMsg("Speicher: Einstellungen nicht lesbar - Standardwerte aktiv");
  if (fsFlags & FS_SAVE_ERR)  logMsg("Speicher: Speichern fehlgeschlagen (voll oder defekt)");
}

// Zaehler und Protokoll aus einer Datei; Ergebnis = Anzahl erkannter Eintraege
static int loadState(const char *path) {
  int n = 0;
  readKvFile(path, [&n](const String &k, const String &v) {
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
    } else {
      return;   // unbekannter Schluessel
    }
    n++;
  });
  return n;
}

// Einstellungen aus einer Datei; Ergebnis = Anzahl gueltiger Eintraege
static int loadSettingsFile(const char *path) {
  int n = 0;
  readKvFile(path, [&n](const String &k, const String &v) {
    SettingIter it;
    SettingDef  d;
    while (it.next(d))
      if (k == d.key) { if (applySetting(d, v, true)) n++; break; }
  });
  return n;
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
  Settings s = {};
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

// Uebernahme erledigt: Kennung im EEPROM ungueltig machen, damit die alten Daten nie
// wieder geladen werden (z.B. wenn das Dateisystem einmal formatiert werden musste)
static void retire() {
  EEPROM.begin(sizeof(Settings) + 8);   // gleiche Groesse wie load() (ESP32: sonst neu angelegt)
  if (EEPROM.read(0) == SETTINGS_MAGIC) {
    EEPROM.write(0, 0);
    if (EEPROM.commit()) logMsg("EEPROM: alte Einstellungen (Firmware bis 1.3) stillgelegt");
  }
  EEPROM.end();
}
}  // namespace legacy

// ------------------------------------------------------------
//  Start
// ------------------------------------------------------------
void settingsBegin() {
  // Erst ohne Formatieren einbinden, damit ein Formatieren erkannt und gemeldet wird
#if defined(ESP32)
  fsOk = LittleFS.begin(false, "/littlefs", 10, "spiffs");   // Partition "spiffs" aus min_spiffs.csv
  if (!fsOk) {
    fsFlags |= FS_FORMATTED;
    fsOk = LittleFS.begin(true, "/littlefs", 10, "spiffs");
  }
#else
  fsOk = LittleFS.begin();
  if (!fsOk) {
    fsFlags |= FS_FORMATTED;
    fsOk = LittleFS.format() && LittleFS.begin();
  }
#endif
  if (!fsOk) {
    // Notbetrieb: Einstellungen aus dem EEPROM (falls noch vorhanden), sonst Standardwerte
    fsFlags |= FS_FAILED;
    legacy::load();
    sanitize();
    fsReport();
    return;
  }

  bool hasFile = LittleFS.exists(SETTINGS_FILE) || LittleFS.exists(SETTINGS_TMP);
  if (loadKvFile(SETTINGS_FILE, SETTINGS_TMP, loadSettingsFile)) {
    sanitize();
    loadKvFile(STATE_FILE, STATE_TMP, loadState);
    legacy::retire();   // laengst uebernommen (aeltere Firmware hat das nicht erledigt)
    fsReport();
    return;
  }
  if (hasFile) {
    // Dateien da, aber nicht lesbar: Standardwerte - KEINE uralten EEPROM-Daten
    fsFlags |= FS_DAMAGED;
    sanitize();
    loadKvFile(STATE_FILE, STATE_TMP, loadState);
    fsReport();
    return;
  }

  // Erster Start mit dieser Firmware: alte Einstellungen uebernehmen
  bool legacyOk = legacy::load();
  if (legacyOk) logMsg("Einstellungen aus dem EEPROM uebernommen");
  sanitize();
  if (saveSettings() && legacyOk) legacy::retire();
  saveState();
  fsReport();
}

// Alle Einstellungen als JSON (Sicherung, inkl. Passwoerter, ohne Zaehler)
String settingsJson() {
  String j;
  j.reserve(1400);
  j = "{\"tueroeffner\":1,\"fw\":\"" FW_VERSION "\"";
  SettingIter it;
  SettingDef  d;
  while (it.next(d)) {
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
  SettingIter it;
  SettingDef  d;
  while (it.next(d)) {
    if (!srv.hasArg(d.key)) continue;
    if (applySetting(d, srv.arg(d.key), true)) n++;
    else { if (err.length()) err += ", "; err += d.key; }
  }
  sanitize();
  // Feste IP, die nicht zusammenpasst, wuerde das Geraet nach dem Neustart aussperren
  if (staticIp && staticIpError(ipAddr, ipMask, ipGw, ipDns)) {
    staticIp = false;
    if (err.length()) err += ", ";
    err += F("staticip (DHCP)");
  }
  saveSettings();
  return n > 0;
}

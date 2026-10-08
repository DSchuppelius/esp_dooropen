// Weboberflaeche und HTTP-API.
//
// Schutz:
//  - Anmeldung ueber die eigene Anmeldeseite (Sitzungs-Cookie) mit zwei Rollen (Admin,
//    Tuer-Zugang; ohne Tuer-Passwort ist die Tuer offen); Basic-Auth fuer Werkzeuge,
//  - Sperre einer Adresse nach mehreren falschen Anmeldungen,
//  - Aenderungen nur per POST von der eigenen Seite (Origin / Sec-Fetch-Site),
//    damit fremde Webseiten im Browser keine Tuer oeffnen koennen (CSRF),
//  - ohne Passwort nur Aufrufe ueber IP-Adresse oder Geraetenamen (DNS-Rebinding).
#include "app.h"
#if defined(ESP32)
  #include <Update.h>
  #include <lwip/sockets.h>
  #include <errno.h>
#else
  #include <Updater.h>
#endif
#include <time.h>
#include <LittleFS.h>
#include "index_html.h"   // erzeugt von tools/embed_html.py

WEB_SERVER_CLASS server(80);

enum Role : uint8_t { ROLE_NONE, ROLE_USER, ROLE_ADMIN };

// ------------------------------------------------------------
//  Sperre nach falschen Anmeldungen (je Adresse)
// ------------------------------------------------------------
struct AuthSlot {
  uint32_t ip;
  uint32_t lastHash;    // gleiche falsche Daten (z.B. Abfrage im Sekundentakt) zaehlen nur einmal
  uint8_t  fails;
  uint32_t lockedAt;
  uint32_t lockMs;
};
static AuthSlot authSlots[8];

static uint32_t fnv(const String &s) {
  uint32_t h = 2166136261UL;
  for (size_t i = 0; i < s.length(); i++) { h ^= (uint8_t)s[i]; h *= 16777619UL; }
  return h;
}

static AuthSlot *findSlot(uint32_t ip, bool create) {
  AuthSlot *free = nullptr;
  for (AuthSlot &s : authSlots) {
    if (s.ip == ip && s.ip) return &s;
    bool locked = s.lockMs && millis() - s.lockedAt < s.lockMs;
    if (!free || (!locked && (s.fails < free->fails || !s.ip))) free = &s;
  }
  if (!create) return nullptr;
  memset(free, 0, sizeof(*free));
  free->ip = ip;
  return free;
}

// Restliche Sperrzeit (s) fuer diese Adresse, 0 = frei
static uint32_t lockRemaining(uint32_t ip) {
  AuthSlot *s = findSlot(ip, false);
  if (!s || !s->lockMs) return 0;
  uint32_t passed = millis() - s->lockedAt;
  if (passed >= s->lockMs) { s->lockMs = 0; return 0; }
  return (s->lockMs - passed) / 1000 + 1;
}

static void authFailed(uint32_t ip, const String &authHeader) {
  AuthSlot *s = findSlot(ip, true);
  uint32_t h = fnv(authHeader);
  if (h == s->lastHash) return;
  s->lastHash = h;
  if (++s->fails >= AUTH_MAX_FAILS) {
    uint8_t extra = s->fails - AUTH_MAX_FAILS;
    uint32_t ms = (uint32_t)AUTH_LOCK_SEC * 1000UL << (extra > 4 ? 4 : extra);
    s->lockMs   = ms > 900000UL ? 900000UL : ms;
    s->lockedAt = millis();
    logMsg("Web: %s nach %u falschen Anmeldungen fuer %lu s gesperrt",
           IPAddress(ip).toString().c_str(), s->fails, (unsigned long)(s->lockMs / 1000));
  }
}

static void authOk(uint32_t ip) {
  AuthSlot *s = findSlot(ip, false);
  if (s) memset(s, 0, sizeof(*s));
}

static uint32_t clientIp() {
  return (uint32_t)server.client().remoteIP();
}

// ------------------------------------------------------------
//  Sitzungen (Anmeldung ueber die eigene Anmeldeseite, Cookie "sid")
//  "Angemeldet bleiben" -> 30 Tage, im LittleFS gesichert (uebersteht Neustarts);
//  sonst bis zum Schliessen des Browsers bzw. 12 h ohne Aktivitaet.
//  Werkzeuge (curl) koennen weiterhin Basic-Auth nutzen.
// ------------------------------------------------------------
struct WebSession {
  char     token[33];   // 128 Bit zufaellig, hex
  uint8_t  role;
  char     name[25];    // angemeldeter Benutzer (fuer Verlauf und Pruefung)
  bool     keep;        // angemeldet bleiben
  uint32_t created;     // Unix-Zeit (0 = unbekannt)
  uint32_t lastUsed;    // uptimeSeconds() - laeuft anders als millis() nicht nach 49 Tagen ueber
};
static WebSession sessions[8];
static const char    *SESSION_FILE    = "/sessions.txt";
static const uint32_t SESSION_IDLE_S  = 12UL * 3600UL;
static const uint32_t SESSION_KEEP_S  = 30UL * 86400UL;

static bool   loggedIn = false;   // letzte Anfrage mit gueltiger Anmeldung?
static String currentUser;        // Name des Benutzers der laufenden Anfrage (oder leer)

static uint32_t randomWord() {
#if defined(ESP32)
  return esp_random();
#else
  return ESP.random();
#endif
}

static bool sessionValid(WebSession &s) {
  if (!s.token[0]) return false;
  if (s.keep) {
    if (!timeValid()) return true;
    uint32_t now = (uint32_t)time(nullptr);
    if (!s.created) s.created = now;   // vor dem Zeitabgleich angelegt: Frist laeuft ab jetzt
    return now - s.created <= SESSION_KEEP_S;
  }
  return uptimeSeconds() - s.lastUsed < SESSION_IDLE_S;
}

static void sessionsSave() {
  File f = LittleFS.open(SESSION_FILE, "w");
  if (!f) return;
  for (WebSession &s : sessions)
    if (s.keep && sessionValid(s))
      f.printf("%s %u %lu %s\n", s.token, s.role, (unsigned long)s.created, urlEncodeField(s.name).c_str());
  f.close();
}

// Abgelaufene Sitzungen loeschen
static void sessionsPurge() {
  bool kept = false;
  for (WebSession &s : sessions)
    if (s.token[0] && !sessionValid(s)) { kept |= s.keep; memset(&s, 0, sizeof(s)); }
  if (kept) sessionsSave();
}

static void sessionsLoad() {
  if (!LittleFS.exists(SESSION_FILE)) return;   // ESP32 meldet sonst einen Fehler
  File f = LittleFS.open(SESSION_FILE, "r");
  if (!f) return;
  for (WebSession &s : sessions) {
    if (!f.available()) break;
    String line = f.readStringUntil('\n');
    char tok[40];
    unsigned role;
    unsigned long created;
    int  rest = 0;
    if (sscanf(line.c_str(), "%39s %u %lu %n", tok, &role, &created, &rest) != 3 || strlen(tok) != 32) continue;
    memcpy(s.token, tok, 33);
    String nm = urlDecode(line.substring(rest));
    nm.trim();
    strlcpy(s.name, nm.c_str(), sizeof(s.name));
    s.role     = (uint8_t)role;
    s.keep     = true;
    s.created  = created;
    s.lastUsed = uptimeSeconds();
  }
  f.close();
}

// Alle Sitzungen einer Rolle beenden (nach Passwortwechsel); ROLE_NONE = alle
void sessionsClear(uint8_t role) {
  for (WebSession &s : sessions)
    if (role == ROLE_NONE || s.role == role) memset(&s, 0, sizeof(s));
  sessionsSave();
}

// Sitzung aus dem Cookie der Anfrage
static WebSession *sessionFind() {
  String c = server.header("Cookie");
  int p = c.indexOf("sid=");
  if (p < 0 || (p > 0 && c[p - 1] != ' ' && c[p - 1] != ';')) return nullptr;
  String tok = c.substring(p + 4, p + 4 + 32);
  if (tok.length() != 32) return nullptr;
  sessionsPurge();
  for (WebSession &s : sessions)
    if (s.token[0] && tok == s.token) return &s;
  return nullptr;
}

static void setSessionCookie(const char *token, bool keep) {
  String c = String("sid=") + token + "; Path=/; HttpOnly; SameSite=Strict";
  if (keep) c += "; Max-Age=" + String(SESSION_KEEP_S);
  server.sendHeader("Set-Cookie", c);
}

static void sessionCreate(uint8_t role, bool keep, const String &name) {
  sessionsPurge();
  WebSession *slot = nullptr;
  uint32_t now = uptimeSeconds(), idle = 0;
  for (WebSession &s : sessions) {
    if (!s.token[0]) { slot = &s; break; }
    // sonst die am laengsten unbenutzte ersetzen
    if (!slot || now - s.lastUsed > idle) { slot = &s; idle = now - s.lastUsed; }
  }
  for (int i = 0; i < 4; i++) snprintf(slot->token + i * 8, 9, "%08lx", (unsigned long)randomWord());
  slot->role     = role;
  strlcpy(slot->name, name.c_str(), sizeof(slot->name));
  slot->keep     = keep;
  slot->created  = timeValid() ? (uint32_t)time(nullptr) : 0;
  slot->lastUsed = now;
  if (keep) sessionsSave();
  setSessionCookie(slot->token, keep);
}

// Gibt es das Konto noch mit dieser Rolle, und darf es sich jetzt anmelden?
// (Passwortwechsel und geloeschte Benutzer beenden so auch bestehende Sitzungen,
// Zeitfenster gelten auch waehrend einer Sitzung.)
static bool accountActive(const String &name, uint8_t role) {
  if (role == ROLE_ADMIN && name == webUser && webPw.length()) return true;
  if (role == ROLE_USER && name == opUser && opPw.length()) return true;
  UserEntry u;
  for (uint8_t i = 0; userAt(i, u); i++)
    if (u.name == name) return u.role == role && userWindowOk(u);
  return false;
}

// Zugangsdaten pruefen (Anmeldeseite): Rolle oder ROLE_NONE; name = Kontoname
static uint8_t checkLogin(const String &user, const String &pw, String &name) {
  name = user;
  if (webPw.length() && user == webUser && pw == webPw) return ROLE_ADMIN;
  if (opPw.length() && user == opUser && pw == opPw)    return ROLE_USER;
  UserEntry u;
  for (uint8_t i = 0; userAt(i, u); i++)
    if (u.name == user && u.pw == pw) return userWindowOk(u) ? u.role : ROLE_NONE;
  return ROLE_NONE;
}

// Admin-Passwort schuetzt die Einstellungen, das Tuer-Passwort (Bedien-Zugang) die Tuer.
// Ohne Tuer-Passwort darf jeder oeffnen und den Verlauf sehen.
static uint8_t currentRole() {
  currentUser = "";
  loggedIn = false;
  if (webPw.length() == 0) return ROLE_ADMIN;   // kein Schutz eingerichtet
  WebSession *s = sessionFind();
  if (s && accountActive(s->name, s->role)) {
    s->lastUsed = uptimeSeconds();
    loggedIn    = true;
    currentUser = s->name;
    return s->role;
  }
  // Basic-Auth nur fuer Werkzeuge (curl -u ...). Anfragen der eigenen Seite tragen
  // "X-Tueroeffner" - dort zaehlt allein die Sitzung, sonst bliebe man nach dem
  // Abmelden ueber eine vom Browser gemerkte Basic-Anmeldung weiter angemeldet.
  // (Sec-Fetch-Site senden Browser nur bei HTTPS, darum der eigene Header.)
  bool fromPage = server.hasHeader("X-Tueroeffner") || server.header("Sec-Fetch-Site").length();
  if (server.hasHeader("Authorization") && !fromPage) {
    if (server.authenticate(webUser.c_str(), webPw.c_str())) {
      authOk(clientIp()); loggedIn = true; currentUser = webUser; return ROLE_ADMIN;
    }
    if (opPw.length() && server.authenticate(opUser.c_str(), opPw.c_str())) {
      authOk(clientIp()); loggedIn = true; currentUser = opUser; return ROLE_USER;
    }
    UserEntry u;
    for (uint8_t i = 0; userAt(i, u); i++)
      if (userWindowOk(u) && server.authenticate(u.name.c_str(), u.pw.c_str())) {
        authOk(clientIp()); loggedIn = true; currentUser = u.name; return u.role;
      }
    authFailed(clientIp(), server.header("Authorization"));
  }
  if (opPw.length() == 0) return ROLE_USER;     // Tuer ohne Anmeldung
  return ROLE_NONE;
}

// ------------------------------------------------------------
//  Herkunft der Anfrage pruefen
// ------------------------------------------------------------
// Ist etwas ohne Anmeldung erreichbar (kein Admin- oder kein Tuer-Passwort): nur ueber
// IP-Adresse oder Geraetenamen (sonst koennte eine fremde Webseite per DNS-Rebinding
// die Tuer oeffnen oder die Oberflaeche samt Sicherung auslesen)
static bool hostAllowed() {
  if (webPw.length() && opPw.length()) return true;   // Basic-Auth schuetzt bereits
  String host = server.hostHeader();
  int colon = host.indexOf(':');
  if (colon >= 0) host = host.substring(0, colon);
  host.toLowerCase();
  IPAddress ip;
  if (host.length() == 0 || ip.fromString(host)) return true;
  return host == HOSTNAME || host == HOSTNAME ".local" || host.startsWith(HOSTNAME ".");
}

// Aenderungen nur von der eigenen Seite (oder Werkzeugen wie curl ohne Origin)
static bool sameOrigin() {
  String sfs = server.header("Sec-Fetch-Site");
  if (sfs.length() && sfs != "same-origin" && sfs != "none") return false;
  String origin = server.header("Origin");
  if (origin.length() && origin != "http://" + server.hostHeader()) return false;
  return true;
}

void sendOk() {
  server.send(200, "application/json", "{\"ok\":true}");
}

void sendErr(int code, const String &msg) {
  server.send(code, "application/json", "{\"ok\":false,\"err\":\"" + jsonEsc(msg) + "\"}");
}

// Antwort nach saveSettings(): ging das Schreiben schief, gilt die Aenderung nur bis
// zum Neustart - das soll die Oberflaeche sagen statt "Gespeichert"
static void replySaved(bool ok) {
  if (ok) sendOk();
  else sendErr(500, F("Übernommen, aber nicht dauerhaft gespeichert (Speicher voll oder defekt) – gilt nur bis zum Neustart"));
}

// Antwortdaten selbst senden, hoechstens bis deadline (millis). Die Loop steht solange;
// liest die Gegenstelle nicht mehr (Handy gesperrt, WLAN weg), wartet write() lange
// (ESP32: bis 10 x 1 s je Aufruf) - in der Zeit keine Klingel, kein SIP.
// false = abgebrochen (Verbindung weg, Gegenstelle liest nicht, Frist um).
bool webSend(const uint8_t *data, size_t len, uint32_t deadline, bool progmem) {
#if defined(ESP32)
  (void)progmem;   // Flash ist beim ESP32 direkt lesbar
  int fd = server.client().fd();
  while (len) {
    int32_t left = (int32_t)(deadline - millis());
    if (fd < 0 || left <= 0) return false;
    // kurz auf Platz im Sendepuffer warten, dann ohne Blockieren senden
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = (left < 50 ? left : 50) * 1000L;
    int r = select(fd + 1, nullptr, &set, nullptr, &tv);
    if (r < 0) return false;
    if (r == 0) continue;
    r = send(fd, data, len, MSG_DONTWAIT);
    if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return false;
    if (r > 0) { data += r; len -= r; }
  }
  return true;
#else
  // ESP8266: write() wartet hoechstens setTimeout() lang ohne Fortschritt
  WiFiClient &c = server.client();
  uint8_t buf[256];
  while (len) {
    int32_t left = (int32_t)(deadline - millis());
    if (left <= 0 || !c.connected()) return false;
    size_t n = len < 1024 ? len : 1024;
    const uint8_t *p = data;
    if (progmem) {   // aus dem Flash nur ueber memcpy_P lesen
      if (n > sizeof(buf)) n = sizeof(buf);
      memcpy_P(buf, data, n);
      p = buf;
    }
    c.setTimeout(left);
    if (c.write(p, n) != n) return false;
    data += n;
    len  -= n;
  }
  return true;
#endif
}

// Zugang pruefen; liefert die Rolle oder ROLE_NONE (Antwort ist dann schon gesendet)
static uint8_t gate(uint8_t needed) {
  if (!hostAllowed()) { sendErr(403, F("Aufruf nur über IP-Adresse oder " HOSTNAME ".local")); return ROLE_NONE; }
  uint32_t lock = lockRemaining(clientIp());
  if (lock) {
    server.sendHeader("Retry-After", String(lock));
    sendErr(429, "Zu viele falsche Anmeldungen – bitte " + String(lock) + " s warten");
    return ROLE_NONE;
  }
  uint8_t role = currentRole();
  if (role < needed) {
    // Kein WWW-Authenticate: der Browser soll kein eigenes Anmeldefenster zeigen,
    // die Seite blendet ihre Anmeldung ein
    sendErr(401, needed == ROLE_ADMIN && role == ROLE_USER ? F("Admin-Anmeldung erforderlich")
                                                           : F("Anmeldung erforderlich"));
    return ROLE_NONE;
  }
  if (server.method() == HTTP_POST && !sameOrigin()) {
    logMsg("Web: Anfrage von fremder Seite abgelehnt (%s)", server.uri().c_str());
    sendErr(403, F("Anfrage von fremder Seite abgelehnt"));
    return ROLE_NONE;
  }
  return role;
}

#define NEED_USER  if (!gate(ROLE_USER))  return
#define NEED_ADMIN if (!gate(ROLE_ADMIN)) return

// ------------------------------------------------------------
//  Hilfen
// ------------------------------------------------------------
// JSON-Bausteine; Schluessel per F("...") im Flash
typedef const __FlashStringHelper *FStr;
static String jStr(FStr key, const String &v) { String r(F(",\"")); r += key; r += F("\":\""); r += jsonEsc(v); r += '"'; return r; }
static String jNum(FStr key, long v)          { String r(F(",\"")); r += key; r += F("\":"); r += v; return r; }
static String jBool(FStr key, bool v)         { String r(F(",\"")); r += key; r += v ? F("\":true") : F("\":false"); return r; }

bool argInt(const char *key, long lo, long hi, long &out) {
  if (!server.hasArg(key)) return false;
  String s = server.arg(key);
  if (s.length() == 0) return false;
  for (size_t i = 0; i < s.length(); i++) if (!isdigit((uint8_t)s[i])) return false;
  long v = s.toInt();
  if (v < lo || v > hi) return false;
  out = v;
  return true;
}

static String argTrim(const char *key) {
  String s = server.arg(key);
  s.trim();
  return s;
}

bool argIp(const char *key, IPAddress &out) {
  IPAddress ip;
  if (!server.hasArg(key) || !ip.fromString(argTrim(key))) return false;
  out = ip;
  return true;
}

// ------------------------------------------------------------
//  Seiten und Abfragen
// ------------------------------------------------------------
// Anmelden ueber die Anmeldeseite: user, pw, keep (angemeldet bleiben)
void handleApiLogin() {
  if (!hostAllowed()) { sendErr(403, F("Aufruf nur über IP-Adresse oder " HOSTNAME ".local")); return; }
  uint32_t lock = lockRemaining(clientIp());
  if (lock) {
    server.sendHeader("Retry-After", String(lock));
    sendErr(429, "Zu viele falsche Anmeldungen – bitte " + String(lock) + " s warten");
    return;
  }
  if (!sameOrigin()) { sendErr(403, F("Anfrage von fremder Seite abgelehnt")); return; }
  String u = argTrim("user"), p = server.arg("pw"), name;
  uint8_t role = checkLogin(u, p, name);
  if (!role) {
    authFailed(clientIp(), u + ":" + p);
    uint32_t l = lockRemaining(clientIp());
    sendErr(l ? 429 : 401, l ? "Zu viele falsche Anmeldungen – bitte " + String(l) + " s warten"
                             : String(F("Benutzer oder Passwort falsch")));
    return;
  }
  authOk(clientIp());
  sessionCreate(role, server.arg("keep") == "1", name);
  logMsg("Web: %s angemeldet als %s (%s)", name.c_str(), role == ROLE_ADMIN ? "Admin" : "Tuer-Zugang",
         IPAddress(clientIp()).toString().c_str());
  server.send(200, "application/json", "{\"ok\":true,\"role\":" + String(role) + "}");
}

// Abmelden: Sitzung beenden und Cookie loeschen
void handleLogout() {
  WebSession *s = sessionFind();
  if (s) {
    bool keep = s->keep;
    memset(s, 0, sizeof(*s));
    if (keep) sessionsSave();
  }
  server.sendHeader("Set-Cookie", "sid=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
  sendOk();
}

// Frueherer Anmelde-Link -> Anmeldeseite
void handleLogin() {
  server.sendHeader("Location", "/#anmelden");
  server.send(302, "text/plain", "");
}

// Die Seite selbst enthaelt keine Daten; Anmeldung erfolgt in der Seite
void handleRoot() {
  if (!hostAllowed()) { sendErr(403, F("Aufruf nur über IP-Adresse oder " HOSTNAME ".local")); return; }
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", "no-cache");
  // Kopf ueber den Server, die ~20 KB selbst mit Frist (siehe webSend)
  server.setContentLength(INDEX_HTML_GZ_LEN);
  server.send(200, "text/html; charset=utf-8", "");
  if (!webSend(INDEX_HTML_GZ, INDEX_HTML_GZ_LEN, millis() + WEB_SEND_MAX_MS, true)) {
    logMsg("Web: Startseite abgebrochen (Gegenstelle liest nicht oder zu langsam)");
    server.client().stop();
  }
}

// Live-Zustand (jede Sekunde abgefragt -> klein halten)
void handleStatus() {
  uint8_t role = gate(ROLE_USER);
  if (!role) return;

  // Kurze Tastendruecke etwas nachhalten, damit die 1-s-Abfrage sie sieht
  bool sigShown = signalActive ||
                  (lastSignalOffAt != 0 && millis() - lastSignalOffAt < SIGNAL_HOLD_MS);
  String json;
  json.reserve(640);
  json = "{\"role\":" + String(role);
  json += jBool(F("hasweb"),    webPw.length() > 0);
  json += jBool(F("hasop"),     opPw.length() > 0);
  json += jBool(F("auth"),      loggedIn);   // mit Passwort angemeldet (Abmelden-Knopf)
  json += jBool(F("signal"),    sigShown);
  json += jBool(F("ringing"),   isRinging());
  json += jBool(F("buzzer"),    buzzerActive);
  json += jNum(F("seconds"),   buzzerSeconds);
  json += jNum(F("triggers"),  buzzerTriggers);
  json += jNum(F("signals"),   signalCount);
  json += jNum(F("calls"),     callCount);
  json += jStr(F("dial"),      dialList);
  json += jBool(F("haspin"),    dtmfPin.length() > 0);
  json += jBool(F("callonring"), callOnRing);
  json += jBool(F("sipcfg"),    sipServer.length() > 0);
  json += jBool(F("registered"), aSip.IsRegistered());
  json += jNum(F("sipst"),     aSip.RegisterStatus());
  json += jBool(F("chain"),     chainIdx >= 0);
  json += jBool(F("incall"),    aSip.IsIncoming());
  json += jNum(F("lastcall"),  aSip.LastCallResult());
  json += jNum(F("lastcode"),  aSip.LastCallCode());
  json += jStr(F("lastnr"),    lastCallNr);
  json += jBool(F("quiet"),     quietOn);
  json += jBool(F("quietnow"),  isQuiet());
  json += jBool(F("praxis"),    praxisOn);
  json += jBool(F("praxisnow"), isPraxis());
  json += jBool(F("incoming"),  incomingOn);
  json += jBool(F("inready"),   incomingReady());
  json += jNum(F("inlock"),    incomingLockedSec());
  json += jNum(F("door"),      doorOn ? (doorOpen ? 2 : 1) : 0);
  json += jBool(F("dooralert"), doorAlerted);
  json += jBool(F("ntp"),       timeValid());
  json += jNum(F("now"),       (long)time(nullptr));
  json += jNum(F("logn"),      logTotal);
  json += jStr(F("user"),      currentUser);
  json += jBool(F("r2"),       relay2On && (relay2User || role == ROLE_ADMIN));
  json += jStr(F("r2name"),    relay2Name);
  json += jBool(F("r2act"),    relay2Active);
  json += jBool(F("doorpass"), doorOn);
  const char *hol = holidayToday();
  json += jStr(F("holiday"),   hol ? hol : "");
  json += jNum(F("fs"),        fsStatus());   // Speicherprobleme (FS_* in app.h)
  if (role == ROLE_ADMIN) {
    json += jStr(F("lastcaller"), aSip.LastCaller());
    json += jStr(F("wglast"),     keypadLastCard());
    json += jNum(F("ptype"),   pushType);
    json += jNum(F("pcode"),   lastPushCode);
    json += jBool(F("mqttcfg"), mqttServer.length() > 0);
    json += jBool(F("mqtt"),    mqttConnected());
    json += jNum(F("up"),      uptimeSeconds());
    json += jNum(F("rssi"),    netRssi());
    json += jNum(F("heap"),    ESP.getFreeHeap());
    json += jNum(F("maxblk"),  maxFreeBlock());
  }
  json += "}";
  server.send(200, "application/json", json);
}

// Einstellungen fuer die Formulare (nur Admin, selten abgefragt)
void handleConfig() {
  NEED_ADMIN;
  String json;
  json.reserve(1500);
  json = "{\"pin\":\"" + jsonEsc(dtmfPin) + "\"";
  json += jNum(F("qfrom"),      quietFrom);
  json += jNum(F("qto"),        quietTo);
  json += jNum(F("pdays"),      praxisDays);
  json += jNum(F("pfrom"),      praxisFrom);
  json += jNum(F("pto"),        praxisTo);
  json += jNum(F("pfrom2"),     praxisFrom2);
  json += jNum(F("pto2"),       praxisTo2);
  json += jStr(F("pfree"),      praxisFree);
  json += jStr(F("guests"),     guestCodes);
  json += jBool(F("dooron"),     doorOn);
  json += jBool(F("doorinv"),    doorInvert);
  json += jNum(F("dooralertmin"), doorAlertMin);
#if defined(DOOR_ON_RX_PIN)
  json += jStr(F("doorpin"),    "RX (GPIO3)");
#else
  json += jStr(F("doorpin"),    "GPIO" + String(PIN_DOOR));
#endif
  json += jStr(F("sipserver"),  sipServer);
  json += jNum(F("sipport"),    sipPort);
  json += jStr(F("sipuser"),    sipUser);
  json += jBool(F("haspw"),      sipPw.length() > 0);
  json += jNum(F("ptype"),      pushType);
  json += jStr(F("pserver"),    pushServer);
  json += jStr(F("ptopic"),     pushTopic);
  json += jBool(F("haspt"),      pushToken.length() > 0);
  json += jNum(F("pev"),        pushEvents);
  json += jStr(F("mqttserver"), mqttServer);
  json += jNum(F("mqttport"),   mqttPort);
  json += jStr(F("mqttuser"),   mqttUser);
  json += jBool(F("hasmqttpw"),  mqttPw.length() > 0);
  json += jStr(F("mqttid"),     devId);
  json += jStr(F("mqttbase"),   baseTopic);
  json += jStr(F("syslog"),     syslogServer);
  json += jStr(F("host"),       HOSTNAME);
  json += jStr(F("ip"),         netIP().toString());
  json += jStr(F("ver"),        FW_VERSION " (" __DATE__ ")");
  json += jStr(F("boot"),       bootReason);
  json += jBool(F("nstatic"),    staticIp);
  // Bei DHCP die aktuellen Werte vorschlagen
  json += jStr(F("nip"),   (staticIp ? ipAddr : netIP()).toString());
  json += jStr(F("nmask"), (staticIp ? ipMask : netMask()).toString());
  json += jStr(F("ngw"),   (staticIp ? ipGw   : netGateway()).toString());
  json += jStr(F("ndns"),  (staticIp ? ipDns  : netDns()).toString());
  json += jStr(F("apname"),     WIFI_AP_NAME);
  json += jBool(F("apset"),      apPw.length() > 0);
  json += jBool(F("apdefault"),  apPw == WIFI_AP_PASSWORD);
  json += jStr(F("webuser"),    webUser);
  json += jBool(F("hasweb"),     webPw.length() > 0);
  json += jBool(F("ota"),        otaActive());
  json += jStr(F("opuser"),     opUser);
  json += jBool(F("hasop"),      opPw.length() > 0);
  json += jStr(F("ringcall"),    ringCallers);
  json += jStr(F("pholiday"),    praxisHoliday);
  json += jBool(F("r2on"),       relay2On);
  json += jStr(F("r2name"),      relay2Name);
  json += jNum(F("r2dur"),       relay2Seconds);
  json += jBool(F("r2user"),     relay2User);
  // Benutzer ohne Passwoerter: "Name:Rolle:Tage:Von:Bis;..."
  {
    String ul;
    UserEntry u;
    for (uint8_t i = 0; userAt(i, u); i++) {
      if (i) ul += ';';
      ul += urlEncodeField(u.name) + ':' + String(u.role) + ':' + String(u.days) + ':' +
            String(u.from) + ':' + String(u.to);
    }
    json += jStr(F("users"), ul);
  }
#if defined(ESP32)
  json += jBool(F("tgavail"),    true);
#else
  json += jBool(F("tgavail"),    false);
#endif
  json += jBool(F("tgopen"),     tgOpen);
  json += jStr(F("tgchats"),     tgChats);
  json += jBool(F("wgavail"),    keypadAvailable());
  json += jBool(F("wgon"),       wgOn);
  json += jStr(F("cards"),       wgCards);
  json += jBool(F("hkavail"),    homekitAvailable());
  json += jBool(F("hkon"),       hkOn);
  json += jStr(F("hkstatus"),    homekitStatus());
  json += jBool(F("eth"),        netIsEthernet());
  json += "}";
  server.send(200, "application/json", json);
}

// Ereignisprotokoll, neuestes zuerst. t = Unix-Zeit (0 = unbekannt), a = Alter in s
// (-1 = unbekannt: ohne Uhrzeit gesichert, vor einem Neustart), d = Detail
void handleLog() {
  NEED_USER;
  bool   synced = timeValid();
  time_t now    = time(nullptr);
  String json;
  json.reserve(60 * logCount + 16);
  json = "{\"ev\":[";
  for (uint8_t i = 0; i < logCount; i++) {
    const LogEntry &e = eventLog[(logHead + LOG_SIZE - 1 - i) % LOG_SIZE];
    uint32_t t   = logEntryTime(e);
    long     age = e.t == LOG_T_UNKNOWN ? -1
                 : e.t && synced ? (long)(now - e.t) : (long)((millis() - e.at) / 1000);
    if (i) json += ",";
    json += "{\"e\":" + String(e.type) + ",\"t\":" + String(t) + ",\"a\":" + String(age);
    if (e.detail[0]) json += ",\"d\":\"" + jsonEsc(e.detail) + "\"";
    json += "}";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

// ------------------------------------------------------------
//  Aktionen
// ------------------------------------------------------------
void handleOpen() {
  NEED_USER;
  startBuzzer(EV_OPEN_WEB, currentUser);
  aSip.Hangup();   // Tuer ist auf -> laufenden Anruf beenden (klingelnd: CANCEL, sonst BYE)
  sendOk();
}

// Zweites Relais (Tor o.ae.): Tuer-Zugang nur, wenn freigegeben
void handleOpen2() {
  if (!gate(relay2User ? ROLE_USER : ROLE_ADMIN)) return;
  if (!startRelay2("Web" + (currentUser.length() ? " " + currentUser : String()))) {
    sendErr(400, F("Zweites Relais ist ausgeschaltet"));
    return;
  }
  sendOk();
}

// Langer Verlauf als CSV-Datei
void handleLogCsv() {
  NEED_USER;
  eventsCsv(server);
}

void handleCall() {
  NEED_USER;
  String nr;
  if (!dialTarget(dialList, 0, nr)) { sendErr(400, F("keine Zielnummer")); return; }
  if (!startChain())                { sendErr(409, F("Anruf laeuft bereits")); return; }
  sendOk();
}

void handleSetDuration() {
  NEED_ADMIN;
  long v;
  if (!argInt("s", MIN_BUZZER_SECONDS, MAX_BUZZER_SECONDS, v)) { sendErr(400, F("Summer-Dauer ungueltig")); return; }
  buzzerSeconds = (uint8_t)v;
  bool saved = saveSettings();
  displayDirty = true;
  mqttStateDue = true;
  replySaved(saved);
}

// Rufkette, Code, Anruf-Schalter und Summer-Dauer.
// Erst alles pruefen, dann uebernehmen -> bei Fehler bleibt alles unveraendert.
void handleSetDial() {
  NEED_ADMIN;
  String list = argTrim("nr");
  if (!server.hasArg("nr") || !dialListValid(list)) {
    sendErr(400, F("Rufkette zu lang (je Nummer max. 20 Zeichen)"));
    return;
  }
  String pin = server.hasArg("pin") ? argTrim("pin") : dtmfPin;
  if (!pinValid(pin)) { sendErr(400, F("Code: nur Ziffern, max. 8")); return; }
  long dur = buzzerSeconds;
  if (server.hasArg("s") && !argInt("s", MIN_BUZZER_SECONDS, MAX_BUZZER_SECONDS, dur)) {
    sendErr(400, F("Summer-Dauer ungueltig"));
    return;
  }
  dialList = list;
  dtmfPin  = pin;
  if (server.hasArg("auto")) callOnRing = server.arg("auto") == "1";
  buzzerSeconds = (uint8_t)dur;
  applyCallSeconds();
  bool saved = saveSettings();
  displayDirty = true;
  mqttStateDue = true;
  replySaved(saved);
}

void handleSetSip() {
  NEED_ADMIN;
  String srv = server.hasArg("server") ? argTrim("server") : sipServer;
  String usr = server.hasArg("user") ? argTrim("user") : sipUser;
  String pw  = server.arg("pw");
  long p = sipPort;
  if (server.hasArg("port") && !argInt("port", 1, 65535, p)) { sendErr(400, F("Port ungueltig")); return; }
  if (srv.length() > 40 || usr.length() > 24 || pw.length() > 32) {
    sendErr(400, F("Zu lang (Server 40, Benutzer 24, Passwort 32 Zeichen)"));
    return;
  }
  sipServer = srv;
  sipUser   = usr;
  sipPort   = (uint16_t)p;
  if (pw.length() > 0) sipPw = pw;   // Passwort nur uebernehmen, wenn ein neues angegeben wurde
  bool saved = saveSettings();
  initSip();   // mit neuen Daten neu registrieren (laeuft nebenher)
  replySaved(saved);
}

void handleSetMqtt() {
  NEED_ADMIN;
  String srv = server.hasArg("server") ? argTrim("server") : mqttServer;
  String usr = server.hasArg("user") ? argTrim("user") : mqttUser;
  String pw  = server.arg("pw");
  long p = mqttPort;
  if (server.hasArg("port") && !argInt("port", 1, 65535, p)) { sendErr(400, F("Port ungueltig")); return; }
  if (srv.length() > 40 || usr.length() > 32 || pw.length() > 32) {
    sendErr(400, F("Zu lang (Broker 40, Benutzer/Passwort 32 Zeichen)"));
    return;
  }
  mqttServer = srv;
  mqttUser   = usr;
  mqttPort   = (uint16_t)p;
  if (pw.length() > 0) mqttPw = pw;
  bool saved = saveSettings();
  initMqtt();   // Verbindung mit neuen Daten aufbauen (in mqttLoop)
  replySaved(saved);
}

void handleSetSyslog() {
  NEED_ADMIN;
  String s = argTrim("server");
  if (s.length() > 40) { sendErr(400, F("Name zu lang (max. 40 Zeichen)")); return; }
  syslogServer = s;
  bool saved = saveSettings();
  syslogBegin();
  logMsg("Syslog aktiv (Firmware " FW_VERSION ")");
  replySaved(saved);
}

void handleSetQuiet() {
  NEED_ADMIN;
  long from, to;
  if (!argInt("from", 0, 24 * 60 - 1, from) || !argInt("to", 0, 24 * 60 - 1, to)) {
    sendErr(400, F("Uhrzeit ungueltig"));
    return;
  }
  quietOn   = server.arg("on") == "1";
  quietFrom = (uint16_t)from;
  quietTo   = (uint16_t)to;
  bool saved = saveSettings();
  mqttStateDue = true;
  replySaved(saved);
}

static bool holidays_known(const String &h);

void handleSetPraxis() {
  NEED_ADMIN;
  long from, to, days, from2 = 0, to2 = 0;
  if (!argInt("from", 0, 24 * 60 - 1, from) || !argInt("to", 0, 24 * 60 - 1, to) ||
      !argInt("days", 0, 0x7F, days)) {
    sendErr(400, F("Uhrzeit oder Tage ungueltig"));
    return;
  }
  if ((server.hasArg("from2") && !argInt("from2", 0, 24 * 60 - 1, from2)) ||
      (server.hasArg("to2")   && !argInt("to2",   0, 24 * 60 - 1, to2))) {
    sendErr(400, F("Zweites Zeitfenster ungueltig"));
    return;
  }
  String freeDays = server.hasArg("free") ? argTrim("free") : praxisFree;
  String holiday = server.hasArg("holiday") ? argTrim("holiday") : praxisHoliday;
  if (holiday.length() && (holiday.length() != 2 || !holidays_known(holiday))) {
    sendErr(400, F("Bundesland ungültig"));
    return;
  }
  if (!freeDaysValid(freeDays)) { sendErr(400, F("Ausnahmetage: z. B. 24.12, 31.12, 3.10.2026")); return; }
  praxisOn    = server.arg("on") == "1";
  praxisDays  = (uint8_t)days;
  praxisFrom  = (uint16_t)from;
  praxisTo    = (uint16_t)to;
  praxisFrom2 = (uint16_t)from2;
  praxisTo2   = (uint16_t)to2;
  praxisFree  = freeDays;
  praxisHoliday = holiday;
  bool saved = saveSettings();
  mqttStateDue = true;
  replySaved(saved);
}

void handleSetDoor() {
  NEED_ADMIN;
  long alert;
  if (!argInt("alert", 0, 1440, alert)) { sendErr(400, F("Minuten ungueltig (0-1440)")); return; }
  bool wasOn   = doorOn;
  doorOn       = server.arg("on") == "1";
  doorInvert   = server.arg("inv") == "1";
  doorAlertMin = (uint16_t)alert;
  doorAlerted  = false;
  bool saved = saveSettings();
  if (wasOn != doorOn) mqttDiscoveryDue = true;   // Tuer-Entitaeten anlegen/entfernen
  mqttStateDue = true;
  replySaved(saved);
}

// Eingehende Anrufe ein/aus und Gaestecodes
void handleSetIncoming() {
  NEED_ADMIN;
  String g = server.hasArg("guests") ? argTrim("guests") : guestCodes;
  if (!guestsValid(g)) {
    sendErr(400, "Gästecodes ungültig (max. " + String(GUEST_CODES_MAX) + ", je 4–8 Ziffern)");
    return;
  }
  if (server.hasArg("on")) incomingOn = server.arg("on") == "1";
  guestCodes = g;
  bool saved = saveSettings();
  mqttStateDue = true;
  replySaved(saved);
}

// Bundesland-Kuerzel fuer Feiertage bekannt?
static bool holidays_known(const String &h) {
  static const char *const ST = "BW BY BE BB HB HH HE MV NI NW RP SL SN ST SH TH";
  return strstr(ST, h.c_str()) != nullptr && h.indexOf(' ') < 0;
}

// Klingeln per Anruf: Nummern ("*" = jeder Anrufer, leer = aus)
void handleSetRing() {
  NEED_ADMIN;
  String c = argTrim("callers");
  if (c != "*" && !dialListValid(c)) { sendErr(400, F("Rufnummern ungültig (je max. 20 Zeichen)")); return; }
  ringCallers = c;
  replySaved(saveSettings());
}

// Zweites Relais
void handleSetRelay2() {
  NEED_ADMIN;
  long dur;
  String name = argTrim("name");
  if (!argInt("dur", MIN_BUZZER_SECONDS, MAX_BUZZER_SECONDS, dur) || name.length() < 1 || name.length() > 16) {
    sendErr(400, F("Name 1-16 Zeichen, Dauer 1-30 s"));
    return;
  }
  bool was    = relay2On;
  relay2On    = server.arg("on") == "1";
  relay2User  = server.arg("user") == "1";
  relay2Name  = name;
  relay2Seconds = (uint8_t)dur;
  if (!relay2On && relay2Active) stopRelay2();
  bool saved = saveSettings();
  mqttDiscoveryDue = true;   // Knopf in Home Assistant anlegen/entfernen/umbenennen
  (void)was;
  replySaved(saved);
}

// Weitere Benutzer. Kommt fuer einen bestehenden Namen ein leeres Passwort,
// bleibt dessen bisheriges Passwort erhalten.
void handleSetUsers() {
  NEED_ADMIN;
  String in = server.arg("users"), out;
  int start = 0;
  while (start < (int)in.length()) {
    int end = in.indexOf(';', start);
    if (end < 0) end = in.length();
    String e = in.substring(start, end);
    int c1 = e.indexOf(':'), c2 = c1 < 0 ? -1 : e.indexOf(':', c1 + 1);
    if (c1 < 0 || c2 < 0) { sendErr(400, F("Benutzerliste ungültig")); return; }
    String name = urlDecode(e.substring(0, c1)), pw = e.substring(c1 + 1, c2);
    if (name == webUser || name == opUser) {
      sendErr(400, "Name „" + name + "“ ist schon Admin bzw. Tür-Zugang");
      return;
    }
    if (pw.length() == 0) {   // bisheriges Passwort uebernehmen
      UserEntry u;
      bool found = false;
      for (uint8_t i = 0; userAt(i, u); i++)
        if (u.name == name) { pw = urlEncodeField(u.pw); found = true; break; }
      if (!found) { sendErr(400, "Passwort für „" + name + "“ fehlt"); return; }
    }
    if (out.length()) out += ';';
    out += e.substring(0, c1 + 1) + pw + e.substring(c2);
    start = end + 1;
  }
  if (!usersValid(out)) {
    sendErr(400, "Benutzer ungültig (max. " + String(USERS_MAX) + ", Namen eindeutig, Passwort 1-32 Zeichen)");
    return;
  }
  users = out;
  replySaved(saveSettings());
}

// Telegram-Bot: Oeffnen per Knopf erlauben, weitere Chats
void handleSetTelegram() {
  NEED_ADMIN;
  String chats = argTrim("chats");
  if (chats.length() > 100) { sendErr(400, F("Chat-IDs zu lang")); return; }
  tgOpen  = server.arg("open") == "1";
  tgChats = chats;
  replySaved(saveSettings());
}

// Tastenfeld / RFID
void handleSetKeypad() {
  NEED_ADMIN;
  String cards = argTrim("cards");
  if (!cardsValid(cards)) { sendErr(400, F("Kartenliste ungültig (Nummer:Name, max. 25)")); return; }
  wgOn    = server.arg("on") == "1";
  wgCards = cards;
  replySaved(saveSettings());
}

// Apple Home: Ein/Aus wirkt nach Neustart; "reset=1" erzeugt neuen Code
void handleSetHomekit() {
  NEED_ADMIN;
  bool on = server.arg("on") == "1";
  bool changed = on != hkOn;
  hkOn = on;
  if (server.arg("reset") == "1") hkCode = "";
  if (!saveSettings()) { replySaved(false); return; }
  server.send(200, "application/json", String("{\"ok\":true,\"restart\":") + (changed ? "true" : "false") + "}");
}

void handleSetPush() {
  NEED_ADMIN;
  long type, ev;
  if (!argInt("type", PUSH_OFF, PUSH_TELEGRAM, type) || !argInt("ev", 0, 15, ev)) {
    sendErr(400, F("Eingaben ungueltig"));
    return;
  }
  String srv = argTrim("server"), topic = argTrim("topic");
  if (srv.length() > 64 || topic.length() > 64 || server.arg("token").length() > 64) {
    sendErr(400, F("Eingabe zu lang (max. 64 Zeichen)"));
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
  replySaved(saveSettings());
}

void handleTestPush() {
  NEED_ADMIN;
  if (pushType == PUSH_OFF || pushTopic.length() == 0) { sendErr(400, F("Push ist nicht eingerichtet")); return; }
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
    String u = argTrim("user");
    String p = server.arg("pw");
    if (u.length() == 0 || u.length() > 24 || p.length() == 0 || p.length() > 32) {
      sendErr(400, F("Benutzer 1-24, Passwort 1-32 Zeichen"));
      return;
    }
    if (opPw.length() && u == opUser) { sendErr(400, F("Benutzer ist schon der Tür-Zugang")); return; }
    webUser = u;
    webPw   = p;
  }
  bool saved = saveSettings();
  sessionsClear(ROLE_NONE);   // alte Sitzungen gelten nicht mehr
  // Wer das Passwort gerade gesetzt hat, bleibt angemeldet
  if (webPw.length()) sessionCreate(ROLE_ADMIN, false, webUser);
  // ArduinoOTA uebernimmt das Passwort nur beim Start -> gilt dort nach Neustart
  replySaved(saved);
}

// Tuer-Zugang (Bedien-Zugang) setzen oder entfernen (off=1)
void handleSetOp() {
  NEED_ADMIN;
  if (server.arg("off") == "1") {
    opPw = "";
  } else {
    if (webPw.length() == 0) { sendErr(400, F("Zuerst ein Admin-Passwort setzen")); return; }
    String u = argTrim("user");
    String p = server.arg("pw");
    if (u.length() == 0 || u.length() > 24 || p.length() == 0 || p.length() > 32) {
      sendErr(400, F("Benutzer 1-24, Passwort 1-32 Zeichen"));
      return;
    }
    if (u == webUser) { sendErr(400, F("Benutzer muss sich vom Admin unterscheiden")); return; }
    opUser = u;
    opPw   = p;
  }
  bool saved = saveSettings();
  sessionsClear(ROLE_USER);   // Tuer-Sitzungen mit altem Passwort beenden
  replySaved(saved);
}

// Passwort des Einrichtungs-WLANs (leer = offen)
void handleSetAp() {
  NEED_ADMIN;
  String p = server.arg("pw");
  if (!apPwValid(p)) { sendErr(400, F("Mindestens 8, höchstens 32 Zeichen")); return; }
  apPw = p;
  replySaved(saveSettings());
}

void handleSetNet() {
  NEED_ADMIN;
  bool st = server.arg("static") == "1";
  if (st) {
    IPAddress ip, mask, gw, dns;
    if (!argIp("ip", ip) || !argIp("mask", mask) || !argIp("gw", gw)) {
      sendErr(400, F("IP, Subnetzmaske oder Gateway ungueltig"));
      return;
    }
    if (argTrim("dns").length() == 0) dns = gw;
    else if (!argIp("dns", dns)) { sendErr(400, F("DNS-Server ungültig")); return; }
    // Passt alles zusammen? Sonst waere das Geraet nach dem Neustart nicht erreichbar
    FStr e = staticIpError(ip, mask, gw, dns);
    if (e) { sendErr(400, e); return; }
    ipAddr = ip; ipMask = mask; ipGw = gw; ipDns = dns;
  }
  staticIp = st;
  // Nicht gespeichert -> kein Neustart (die Aenderung waere danach weg)
  if (!saveSettings()) { replySaved(false); return; }
  sendOk();
  scheduleRestart(RR_NETWORK);
}

void handleResetCounters() {
  NEED_ADMIN;
  signalCount = buzzerTriggers = callCount = 0;
  saveState();
  mqttStateDue = true;
  displayDirty = true;
  sendOk();
}

void handleRestart() {
  NEED_ADMIN;
  sendOk();
  scheduleRestart(RR_USER);   // erst Antwort rausschicken
}

void handleWifiReset() {
  NEED_ADMIN;
  sendOk();
  scheduleRestart(RR_WIFI_RESET, true);
}

// Alle Einstellungen als JSON-Datei (inkl. Passwoerter, ohne Zaehler)
void handleBackup() {
  NEED_ADMIN;
  server.sendHeader("Content-Disposition", "attachment; filename=\"tueroeffner-" + devId + ".json\"");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", settingsJson());
}

// Sicherung einspielen: Felder kommen als Formular (aus der JSON-Datei)
void handleRestore() {
  NEED_ADMIN;
  String skipped;
  if (!restoreFromArgs(server, skipped)) { sendErr(400, F("Keine gültigen Einstellungen in der Sicherung")); return; }
  if (skipped.length()) logMsg("Sicherung: ungueltige Felder uebersprungen: %s", skipped.c_str());
  sessionsClear(ROLE_NONE);   // Passwoerter koennen sich geaendert haben
  // Nicht gespeichert -> kein Neustart (die Sicherung waere danach weg)
  if (fsStatus() & FS_SAVE_ERR) { replySaved(false); return; }
  server.send(200, "application/json", "{\"ok\":true,\"skipped\":\"" + jsonEsc(skipped) + "\"}");
  scheduleRestart(RR_RESTORE);
}

// Firmware-Upload: Daten kommen in Stuecken ueber handleUpdateUpload()
static bool updateAuthOk = false;

void handleUpdateUpload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    updateAuthOk = hostAllowed() && !lockRemaining(clientIp()) &&
                   currentRole() == ROLE_ADMIN && sameOrigin();
    if (!updateAuthOk) {
      // Nicht berechtigt: gleich antworten und die Verbindung trennen - sonst liest der
      // Server die ganze Datei (beliebig gross/langsam), und die Loop stuende so lange
      logMsg("Update abgelehnt (%s)", IPAddress(clientIp()).toString().c_str());
      if (gate(ROLE_ADMIN)) sendErr(403, F("Update abgelehnt"));
      server.client().stop();
      return;
    }
    logMsg("Update: %s", up.filename.c_str());
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
  bool authOk = updateAuthOk;
  updateAuthOk = false;   // gilt nur fuer diesen Upload
  if (!authOk) {
    // gate() sendet die passende Fehlermeldung (z.B. Anmeldung erforderlich)
    if (gate(ROLE_ADMIN)) sendErr(403, F("Update abgelehnt"));
    return;
  }
  if (Update.hasError() || !Update.isFinished()) {
    sendErr(500, F("Update fehlgeschlagen (falsche Datei?)"));
    return;
  }
  saveState();   // Zaehler und Protokoll sichern
  sendOk();
  scheduleRestart(RR_UPDATE);
}

void webBegin() {
  // Anfrage-Header fuer Herkunftspruefung und Sitzung (Authorization sammelt der Server selbst)
#if defined(ESP32)
  static const char *headers[] = { "Origin", "Sec-Fetch-Site", "Cookie", "X-Tueroeffner" };
  server.collectHeaders(headers, 4);
#else
  server.collectHeaders("Origin", "Sec-Fetch-Site", "Cookie", "X-Tueroeffner");
#endif
  sessionsLoad();

  server.on("/", handleRoot);
  server.on("/login", handleLogin);
  server.on("/logout", HTTP_POST, handleLogout);
  server.on("/api/login", HTTP_POST, handleApiLogin);
  server.on("/status", handleStatus);
  server.on("/config", handleConfig);
  server.on("/log", handleLog);
  server.on("/log.csv", handleLogCsv);
  server.on("/open2", HTTP_POST, handleOpen2);
  server.on("/setring", HTTP_POST, handleSetRing);
  server.on("/setrelay2", HTTP_POST, handleSetRelay2);
  server.on("/setusers", HTTP_POST, handleSetUsers);
  server.on("/settelegram", HTTP_POST, handleSetTelegram);
  server.on("/setkeypad", HTTP_POST, handleSetKeypad);
  server.on("/sethomekit", HTTP_POST, handleSetHomekit);
  server.on("/open", HTTP_POST, handleOpen);
  server.on("/call", HTTP_POST, handleCall);
  server.on("/setduration", HTTP_POST, handleSetDuration);
  server.on("/setdial", HTTP_POST, handleSetDial);
  server.on("/setsip", HTTP_POST, handleSetSip);
  server.on("/setmqtt", HTTP_POST, handleSetMqtt);
  server.on("/setsyslog", HTTP_POST, handleSetSyslog);
  server.on("/setquiet", HTTP_POST, handleSetQuiet);
  server.on("/setpraxis", HTTP_POST, handleSetPraxis);
  server.on("/setdoor", HTTP_POST, handleSetDoor);
  server.on("/setincoming", HTTP_POST, handleSetIncoming);
  server.on("/setpush", HTTP_POST, handleSetPush);
  server.on("/testpush", HTTP_POST, handleTestPush);
  server.on("/setweb", HTTP_POST, handleSetWeb);
  server.on("/setop", HTTP_POST, handleSetOp);
  server.on("/setap", HTTP_POST, handleSetAp);
  server.on("/setnet", HTTP_POST, handleSetNet);
  server.on("/resetcounters", HTTP_POST, handleResetCounters);
  server.on("/restart", HTTP_POST, handleRestart);
  server.on("/wifireset", HTTP_POST, handleWifiReset);
  server.on("/backup", handleBackup);
  server.on("/restore", HTTP_POST, handleRestore);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.begin();
}

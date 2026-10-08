// Ereignisprotokoll (Ringpuffer), Meldungen auf Seriell und Syslog.
#include "app.h"
#include <WiFiUdp.h>
#include <LittleFS.h>
#include <time.h>

LogEntry eventLog[LOG_SIZE];
uint8_t  logHead  = 0;       // naechster Schreibplatz
uint8_t  logCount = 0;
uint32_t logTotal = 0;       // fuer die Weboberflaeche: hat sich etwas getan?

static WiFiUDP   syslogUdp;
static IPAddress syslogIp;
#if defined(ESP32)
// logMsg kommt auch aus anderen Tasks (Telegram-Bot): Syslog nur von einem Task zugleich.
// Beim ersten Aufruf angelegt (statisch, ohne Heap; C++ sichert das Anlegen ab).
static SemaphoreHandle_t syslogLock() {
  static StaticSemaphore_t buf;
  static SemaphoreHandle_t lock = xSemaphoreCreateMutexStatic(&buf);
  return lock;
}
#endif

// Langer Verlauf: neue Zeilen erst sammeln, dann gebuendelt ausserhalb von Anrufen schreiben
static String   pendingLines;
static uint32_t pendingSince = 0;    // millis() der aeltesten wartenden Zeile
static const char *EVENTS_FILE = "/events.csv";
static const char *EVENTS_OLD  = "/events.old.csv";

bool timeValid() {
  return time(nullptr) > 1600000000;   // nach NTP-Abgleich (vorher ~1970)
}

// Vor dem NTP-Abgleich erfasst (millis): Uhrzeit nachrechnen, sobald sie bekannt ist
static uint32_t timeFromMillis(uint32_t at) {
  return timeValid() ? (uint32_t)time(nullptr) - (millis() - at) / 1000 : 0;
}

uint32_t logEntryTime(const LogEntry &e) {
  if (e.t == LOG_T_UNKNOWN) return 0;
  return e.t ? e.t : timeFromMillis(e.at);
}

// Ereignisnamen im Flash (spart RAM auf dem ESP8266)
static const char EVN0[] PROGMEM = "Klingeln";
static const char EVN1[] PROGMEM = "Klingeln (Nachtruhe)";
static const char EVN2[] PROGMEM = "Anruf ausgelöst";
static const char EVN3[] PROGMEM = "Geöffnet (Web)";
static const char EVN4[] PROGMEM = "Geöffnet (Taster)";
static const char EVN5[] PROGMEM = "Geöffnet (Telefon)";
static const char EVN6[] PROGMEM = "Geöffnet (Home Assistant)";
static const char EVN7[] PROGMEM = "Gerät gestartet";
static const char EVN8[] PROGMEM = "Automatisch geöffnet (Praxis)";
static const char EVN9[] PROGMEM = "Niemand hat abgehoben";
static const char EVN10[] PROGMEM = "Tür zu lange offen";
static const char EVN11[] PROGMEM = "Anruf an den Türöffner";
static const char EVN12[] PROGMEM = "Geöffnet (Gästecode)";
static const char EVN13[] PROGMEM = "Falscher Code";
static const char EVN14[] PROGMEM = "Tür nach Summer geöffnet";
static const char EVN15[] PROGMEM = "Summer, Tür blieb zu";
static const char EVN16[] PROGMEM = "Tür geöffnet (ohne Summer)";
static const char EVN17[] PROGMEM = "Zweites Relais";
static const char EVN18[] PROGMEM = "Geöffnet (Telegram)";
static const char EVN19[] PROGMEM = "Geöffnet (Tastenfeld)";
static const char EVN20[] PROGMEM = "Geöffnet (Karte)";
static const char EVN21[] PROGMEM = "Falscher Code am Tastenfeld";
static const char EVN22[] PROGMEM = "Unbekannte Karte";
static const char EVN23[] PROGMEM = "Geöffnet (Apple Home)";
static const char *const EV_NAMES[] PROGMEM = {
  EVN0, EVN1, EVN2, EVN3, EVN4, EVN5, EVN6, EVN7, EVN8, EVN9, EVN10, EVN11, EVN12, EVN13, EVN14, EVN15, EVN16, EVN17, EVN18, EVN19, EVN20, EVN21, EVN22, EVN23
};
static_assert(sizeof(EV_NAMES) / sizeof(EV_NAMES[0]) == EV_COUNT, "Ereignisnamen unvollstaendig");

String eventName(EventType t) {
  if (t >= EV_COUNT) return "?";
  return String(FPSTR((const char *)pgm_read_ptr(&EV_NAMES[t])));
}

String lastEventText() {
  if (!logCount) return String();
  const LogEntry &e = eventLog[(logHead + LOG_SIZE - 1) % LOG_SIZE];
  String s = eventName(e.type);
  if (e.detail[0]) { s += " – "; s += e.detail; }
  return s;
}

// Detail fuer Protokoll/Datei: kurz und ohne Trennzeichen der Datei
static void setDetail(LogEntry &e, const String &detail) {
  size_t n = 0;
  for (size_t i = 0; i < detail.length() && n < sizeof(e.detail) - 1; i++) {
    char c = detail[i];
    e.detail[n++] = (c == ',' || c == ':' || (uint8_t)c < 0x20) ? ' ' : c;
  }
  e.detail[n] = 0;
}

static LogEntry &nextEntry() {
  LogEntry &e = eventLog[logHead];
  logHead = (logHead + 1) % LOG_SIZE;
  if (logCount < LOG_SIZE) logCount++;
  return e;
}

void logEvent(EventType type, const String &detail) {
  LogEntry &e = nextEntry();
  e.at   = millis();
  e.t    = timeValid() ? (uint32_t)time(nullptr) : 0;
  e.type = type;
  setDetail(e, detail);
  logTotal++;
  stateChanged();
  // "Unix-Zeit;Typ;Detail" - Detail ohne ';' (setDetail ersetzt Trennzeichen).
  // Ohne Uhrzeit vorlaeufig "m<millis>", wird beim Schreiben umgerechnet.
  if (pendingLines.length() < 1200) {
    if (!pendingLines.length()) pendingSince = millis();
    pendingLines += (e.t ? String(e.t) : "m" + String(e.at)) + ';' + String(type) + ';' + e.detail + '\n';
  }
  if (detail.length()) logMsg("Ereignis: %s (%s)", eventName(type).c_str(), detail.c_str());
  else                 logMsg("Ereignis: %s", eventName(type).c_str());
  mqttEvent(type, detail);
}

void logRestore(uint32_t t, uint8_t type, const String &detail) {
  if (type >= EV_COUNT) return;
  LogEntry &e = nextEntry();
  e.at   = millis();
  e.t    = t ? t : LOG_T_UNKNOWN;   // ohne Uhrzeit gesichert: bleibt unbekannt
  e.type = (EventType)type;
  setDetail(e, detail);
  logTotal++;
}

// Freier Platz im LittleFS (Bytes)
static size_t fsFree() {
#if defined(ESP32)
  size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
#else
  FSInfo i;
  if (!LittleFS.info(i)) return 0;
  size_t total = i.totalBytes, used = i.usedBytes;
#endif
  return used < total ? total - used : 0;
}

bool eventsFreeSpace() {
  if (!LittleFS.exists(EVENTS_OLD)) return false;
  LittleFS.remove(EVENTS_OLD);
  logMsg("Speicher knapp: aelterer Verlauf geloescht");
  return true;
}

// Gesammelte Zeilen anhaengen - gebuendelt, hoechstens alle EVENT_FLUSH_MS (Flash schonen);
// Datei zu gross -> wird zur ".old"-Datei. Eintraege ohne Uhrzeit ("m<millis>") bekommen
// sie nachtraeglich, sobald die Zeit bekannt ist; ohne Zeitabgleich nach 10 min mit
// Zeit 0 ("ohne Uhrzeit"). force: sofort und alles (vor Neustart/Update).
void eventsFlush(bool force) {
  if (!pendingLines.length()) return;
  if (fsStatus() & FS_FAILED) { pendingLines = ""; return; }
  if (!force && (aSip.IsBusy() || (millis() - pendingSince < EVENT_FLUSH_MS && pendingLines.length() < 900)))
    return;
  bool hasUntimed = pendingLines.startsWith("m") || pendingLines.indexOf("\nm") >= 0;
  if (!force && hasUntimed && !timeValid() && millis() < 600000UL) return;
  String out;
  if (hasUntimed) {
    int start = 0;
    while (start < (int)pendingLines.length()) {
      int end = pendingLines.indexOf('\n', start);
      if (end < 0) end = pendingLines.length();
      String line = pendingLines.substring(start, end);
      if (line.startsWith("m")) {
        int semi = line.indexOf(';');
        line = String(timeFromMillis(strtoul(line.c_str() + 1, nullptr, 10))) + line.substring(semi);
      }
      out += line + '\n';
      start = end + 1;
    }
  } else {
    out = pendingLines;
  }
  pendingLines = "";
  // Einstellungen brauchen immer Platz: notfalls den aelteren Verlauf opfern
  size_t need = out.length() + FS_RESERVE_BYTES;
  if (fsFree() < need && (!eventsFreeSpace() || fsFree() < need)) {
    logMsg("Verlauf: Speicher knapp - %u Bytes nicht gespeichert", (unsigned)out.length());
    return;
  }
  File f = LittleFS.open(EVENTS_FILE, "a");
  if (!f) return;
  f.print(out);
  size_t size = f.size();
  f.close();
  if (size > EVENT_FILE_MAX) LittleFS.rename(EVENTS_FILE, EVENTS_OLD);   // ersetzt die alte ".old"
}

// Unix-Zeit -> "2026-10-02 08:46:12" (Ortszeit), 0 -> ""
String formatTime(uint32_t t) {
  if (!t) return String();
  time_t tt = t;
  struct tm lt;
  localtime_r(&tt, &lt);
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &lt);
  return buf;
}

// Zeile "zeit;typ;detail" (aus der Datei oder noch wartend) als CSV-Zeile anhaengen
static void csvLine(String &out, const String &line) {
  int a = line.indexOf(';'), b = a < 0 ? -1 : line.indexOf(';', a + 1);
  if (b < 0) return;
  uint32_t t = line[0] == 'm' ? timeFromMillis(strtoul(line.c_str() + 1, nullptr, 10))
                              : strtoul(line.c_str(), nullptr, 10);
  int type   = atoi(line.c_str() + a + 1);
  out += t ? formatTime(t) : String(F("ohne Uhrzeit"));
  out += ';';
  out += type >= 0 && type < EV_COUNT ? eventName((EventType)type) : String("?");
  out += ';';
  out += line.c_str() + b + 1;
  out += "\r\n";
}

// Gesammelte CSV-Zeilen als einen Chunk senden. out beginnt mit dem Platzhalter
// "0000\r\n" fuer die Laenge (HTTP chunked, fuehrende Nullen sind erlaubt).
static bool csvChunk(String &out, uint32_t deadline) {
  static const char hexDigit[] = "0123456789abcdef";
  size_t n = out.length() - 6;
  bool ok = true;
  if (n) {
    for (uint8_t i = 0; i < 4; i++) out.setCharAt(3 - i, hexDigit[(n >> (4 * i)) & 15]);
    out += "\r\n";
    ok = webSend((const uint8_t *)out.c_str(), out.length(), deadline);
  }
  out = F("0000\r\n");
  yield();   // ESP8266: WLAN-Stack und Software-Watchdog bedienen
  return ok;
}

// Datei blockweise lesen (schneller als readStringUntil) und zeilenweise umwandeln;
// false = Senden abgebrochen
static bool csvFile(const char *path, String &out, uint32_t deadline) {
  if (!LittleFS.exists(path)) return true;
  File f = LittleFS.open(path, "r");
  if (!f) return true;
  char   buf[128];
  String line;
  bool   ok = true;
  int    n;
  while (ok && (n = (int)f.read((uint8_t *)buf, sizeof(buf))) > 0) {
    for (int i = 0; i < n && ok; i++) {
      if (buf[i] != '\n') { if (line.length() < 100) line += buf[i]; continue; }
      csvLine(out, line);
      line = "";
      if (out.length() > 1200) ok = csvChunk(out, deadline);
    }
  }
  f.close();
  return ok;
}

// Langer Verlauf als CSV (Semikolon, UTF-8 mit BOM fuer Excel), aelteste zuerst.
// Die Loop steht waehrenddessen (Klingel, SIP): darum direkt ueber die Verbindung senden
// (webSend) und abbrechen, sobald die Gegenstelle nicht mehr liest oder WEB_SEND_MAX_MS
// um sind. Antwort "chunked" - ein Abbruch faellt im Browser als Fehler auf.
void eventsCsv(WEB_SERVER_CLASS &srv) {
  uint32_t deadline = millis() + WEB_SEND_MAX_MS;
  String out;
  out.reserve(1400);
  out = F("HTTP/1.1 200 OK\r\n"
          "Content-Type: text/csv; charset=utf-8\r\n"
          "Content-Disposition: attachment; filename=\"tueroeffner-verlauf.csv\"\r\n"
          "Cache-Control: no-store\r\n"
          "Transfer-Encoding: chunked\r\n"
          "Connection: close\r\n\r\n");
  bool ok = webSend((const uint8_t *)out.c_str(), out.length(), deadline);
  out = F("0000\r\n\xEF\xBB\xBFZeit;Ereignis;Detail\r\n");
  for (const char *path : { EVENTS_OLD, EVENTS_FILE })
    if (ok) ok = csvFile(path, out, deadline);
  // noch nicht geschriebene Zeilen (werden gebuendelt gespeichert) mitsenden
  for (int start = 0; ok && start < (int)pendingLines.length();) {
    int end = pendingLines.indexOf('\n', start);
    if (end < 0) end = pendingLines.length();
    csvLine(out, pendingLines.substring(start, end));
    start = end + 1;
  }
  if (ok) ok = csvChunk(out, deadline);
  if (ok) ok = webSend((const uint8_t *)"0\r\n\r\n", 5, deadline);
  if (!ok) {
    logMsg("Web: CSV-Export abgebrochen (Gegenstelle liest nicht oder zu langsam)");
    srv.client().stop();
  }
}

// Syslog-Ziel aufloesen (beim Start und nach Aenderung)
void syslogBegin() {
  IPAddress ip((uint32_t)0);
  if (syslogServer.length() && netUp()) {
    IPAddress a;
    if (a.fromString(syslogServer) || WiFi.hostByName(syslogServer.c_str(), a) == 1) ip = a;
  }
#if defined(ESP32)
  xSemaphoreTake(syslogLock(), portMAX_DELAY);
#endif
  syslogIp = ip;
#if defined(ESP32)
  xSemaphoreGive(syslogLock());
#endif
  if ((uint32_t)ip) fsReport();   // Speicherprobleme vom Start auch an den Syslog-Server
}

// Meldung auf Seriell und (falls eingestellt) per Syslog (RFC 5424, UDP)
void logMsgP(const char *fmtP, ...) {
  char buf[200];
  va_list ap;
  va_start(ap, fmtP);
#if defined(ESP32)
  vsnprintf(buf, sizeof(buf), fmtP, ap);
#else
  vsnprintf_P(buf, sizeof(buf), fmtP, ap);
#endif
  va_end(ap);
  Serial.println(buf);
#if defined(ESP32)
  // Syslog gerade von einem anderen Task belegt: kurz warten, sonst nur seriell
  if (xSemaphoreTake(syslogLock(), pdMS_TO_TICKS(100)) != pdTRUE) return;
#endif
  if ((uint32_t)syslogIp && netUp()) {
    // <134> = local0.info; Zeitstempel "-" (setzt der Server)
    syslogUdp.beginPacket(syslogIp, SYSLOG_PORT);
    syslogUdp.print("<134>1 - " HOSTNAME " tueroeffner - - - ");
    syslogUdp.print(buf);
    syslogUdp.endPacket();
  }
#if defined(ESP32)
  xSemaphoreGive(syslogLock());
#endif
}

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

// Langer Verlauf: neue Zeilen erst sammeln, dann ausserhalb von Anrufen schreiben
static String   pendingLines;
static const char *EVENTS_FILE = "/events.csv";
static const char *EVENTS_OLD  = "/events.old.csv";

bool timeValid() {
  return time(nullptr) > 1600000000;   // nach NTP-Abgleich (vorher ~1970)
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
  e.t    = t;
  e.type = (EventType)type;
  setDetail(e, detail);
  logTotal++;
}

// Gesammelte Zeilen anhaengen; Datei zu gross -> wird zur ".old"-Datei.
// Eintraege ohne Uhrzeit ("m<millis>") bekommen sie nachtraeglich, sobald die
// Zeit bekannt ist; ohne Zeitabgleich nach 10 min mit Zeit 0 (unbekannt).
void eventsFlush() {
  if (!pendingLines.length() || aSip.IsBusy()) return;
  bool hasUntimed = pendingLines.startsWith("m") || pendingLines.indexOf("\nm") >= 0;
  if (hasUntimed && !timeValid() && millis() < 600000UL) return;
  String out;
  if (hasUntimed) {
    uint32_t now = timeValid() ? (uint32_t)time(nullptr) : 0;
    int start = 0;
    while (start < (int)pendingLines.length()) {
      int end = pendingLines.indexOf('\n', start);
      if (end < 0) end = pendingLines.length();
      String line = pendingLines.substring(start, end);
      if (line.startsWith("m")) {
        int semi = line.indexOf(';');
        uint32_t at = strtoul(line.substring(1, semi).c_str(), nullptr, 10);
        uint32_t t  = now ? now - (millis() - at) / 1000 : 0;
        line = String(t) + line.substring(semi);
      }
      out += line + '\n';
      start = end + 1;
    }
  } else {
    out = pendingLines;
  }
  File f = LittleFS.open(EVENTS_FILE, "a");
  if (!f) { pendingLines = ""; return; }
  f.print(out);
  size_t size = f.size();
  f.close();
  pendingLines = "";
  if (size > EVENT_FILE_MAX) {
    LittleFS.remove(EVENTS_OLD);
    LittleFS.rename(EVENTS_FILE, EVENTS_OLD);
  }
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

// Langer Verlauf als CSV (Semikolon, UTF-8 mit BOM fuer Excel), aelteste zuerst
void eventsCsv(WEB_SERVER_CLASS &srv) {
  eventsFlush();
  srv.sendHeader("Content-Disposition", "attachment; filename=\"tueroeffner-verlauf.csv\"");
  srv.sendHeader("Cache-Control", "no-store");
  srv.setContentLength(CONTENT_LENGTH_UNKNOWN);
  srv.send(200, "text/csv; charset=utf-8", "");
  srv.sendContent("\xEF\xBB\xBFZeit;Ereignis;Detail\r\n");
  for (const char *path : { EVENTS_OLD, EVENTS_FILE }) {
    File f = LittleFS.open(path, "r");
    if (!f) continue;
    String out;
    while (f.available()) {
      String line = f.readStringUntil('\n');
      int a = line.indexOf(';'), b = a < 0 ? -1 : line.indexOf(';', a + 1);
      if (b < 0) continue;
      uint32_t t = strtoul(line.substring(0, a).c_str(), nullptr, 10);
      int type   = line.substring(a + 1, b).toInt();
      out += formatTime(t);
      out += ';';
      out += type >= 0 && type < EV_COUNT ? eventName((EventType)type) : String("?");
      out += ';';
      out += line.substring(b + 1);
      out += "\r\n";
      if (out.length() > 1000) { srv.sendContent(out); out = ""; feedWatchdog(); }
    }
    if (out.length()) srv.sendContent(out);
    f.close();
  }
  srv.sendContent("");
}

// Syslog-Ziel aufloesen (beim Start und nach Aenderung)
void syslogBegin() {
  syslogIp = IPAddress((uint32_t)0);
  if (syslogServer.length() == 0 || !netUp()) return;
  if (!syslogIp.fromString(syslogServer)) {
    IPAddress a;
    if (WiFi.hostByName(syslogServer.c_str(), a) == 1) syslogIp = a;
  }
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
  if (!(uint32_t)syslogIp || !netUp()) return;
  // <134> = local0.info; Zeitstempel "-" (setzt der Server)
  syslogUdp.beginPacket(syslogIp, SYSLOG_PORT);
  syslogUdp.print("<134>1 - " HOSTNAME " tueroeffner - - - ");
  syslogUdp.print(buf);
  syslogUdp.endPacket();
}

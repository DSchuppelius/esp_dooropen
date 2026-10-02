// Ereignisprotokoll (Ringpuffer), Meldungen auf Seriell und Syslog.
#include "app.h"
#include <WiFiUdp.h>
#include <time.h>

LogEntry eventLog[LOG_SIZE];
uint8_t  logHead  = 0;       // naechster Schreibplatz
uint8_t  logCount = 0;
uint32_t logTotal = 0;       // fuer die Weboberflaeche: hat sich etwas getan?

static WiFiUDP   syslogUdp;
static IPAddress syslogIp;

bool timeValid() {
  return time(nullptr) > 1600000000;   // nach NTP-Abgleich (vorher ~1970)
}

const char *eventName(EventType t) {
  static const char *const NAMES[EV_COUNT] = {
    "Klingeln", "Klingeln (Nachtruhe)", "Anruf ausgelöst", "Geöffnet (Web)",
    "Geöffnet (Taster)", "Geöffnet (Telefon)", "Geöffnet (Home Assistant)", "Gerät gestartet",
    "Automatisch geöffnet (Praxis)", "Niemand hat abgehoben", "Tür zu lange offen",
    "Anruf an den Türöffner", "Geöffnet (Gästecode)", "Falscher Code"
  };
  return t < EV_COUNT ? NAMES[t] : "?";
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
  if (detail.length()) logMsg("Ereignis: %s (%s)", eventName(type), detail.c_str());
  else                 logMsg("Ereignis: %s", eventName(type));
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

// Syslog-Ziel aufloesen (beim Start und nach Aenderung)
void syslogBegin() {
  syslogIp = IPAddress((uint32_t)0);
  if (syslogServer.length() == 0 || WiFi.status() != WL_CONNECTED) return;
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
  if (!(uint32_t)syslogIp || WiFi.status() != WL_CONNECTED) return;
  // <134> = local0.info; Zeitstempel "-" (setzt der Server)
  syslogUdp.beginPacket(syslogIp, SYSLOG_PORT);
  syslogUdp.print("<134>1 - " HOSTNAME " tueroeffner - - - ");
  syslogUdp.print(buf);
  syslogUdp.endPacket();
}

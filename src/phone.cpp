// SIP: Anmeldung, Rufkette beim Klingeln, Oeffnen-Code per DTMF,
// eingehende Anrufe mit Oeffnen-Code oder Gaestecode.
#include "app.h"
#include <time.h>

// SIP: Ein- und Ausgabepuffer + Client. SIP ueber UDP bleibt praktisch immer
// unter der MTU (1500); groessere Pakete werden abgeschnitten.
static char acSipIn[1600];
static char acSipOut[1500];
Sip aSip(acSipOut, sizeof(acSipOut));

// Stabile Puffer fuer die SIP-Lib: sie merkt sich nur ZEIGER, keine Kopien.
// Deshalb duerfen diese Adressen sich nie aendern (kein String::c_str()!).
static char sipServerBuf[41];
static char sipUserBuf[25];
static char sipPwBuf[33];
static char myIpBuf[16];
static char dialBuf[MAX_DIAL_LEN + 1];

int8_t   chainIdx = -1;          // Rufkette: Index der gerade angerufenen Nummer, -1 = keine
String   lastCallNr;
static String   dtmfBuf;         // eingegebene Ziffern fuer den Code
static bool     sipInited      = false;
static uint32_t lastRegisterAt = 0;
static uint8_t  regFastTries   = 0;   // schnelle Wiederholungen nach Init
static int      lastRegStatus  = -2;

// Eingehende Anrufe: Fehlversuche und Sperre
static uint8_t  callTries   = 0;      // falsche Codes im laufenden Anruf
static uint8_t  failCount   = 0;      // falsche Codes im Zeitfenster
static uint32_t firstFailAt = 0;
static bool     locked      = false;
static uint32_t lockStart   = 0;

static void copyToField(char *dst, size_t dstSize, const String &src) {
  size_t n = src.length();
  if (n > dstSize - 1) n = dstSize - 1;
  memcpy(dst, src.c_str(), n);
  dst[n] = '\0';
}

// Mit Oeffnungs-Code bleibt das Gespraech laenger offen, damit man ihn tippen kann
void applyCallSeconds() {
  aSip.SetCallSeconds(dtmfPin.length() ? SIP_PIN_CALL_SECONDS : 0);
}

// Klingeln per Anruf: ist der Anrufer eine der Klingel-Nummern ("*" = jeder)?
static bool ringFilter(const char *caller) {
  if (ringCallers.length() == 0) return false;
  if (ringCallers == "*") return true;
  String nr;
  for (uint8_t i = 0; dialTarget(ringCallers, i, nr); i++)
    if (nr == caller) return true;
  return false;
}

// SIP-Client mit den aktuellen Einstellungen (neu) initialisieren. Blockiert
// nicht: die Anmeldung laeuft danach nebenher in phoneLoop().
void initSip() {
  aSip.Hangup();   // laufenden Anruf sauber beenden, bevor sich die Daten aendern
  stopChain();
  copyToField(sipServerBuf, sizeof(sipServerBuf), sipServer);
  copyToField(sipUserBuf,   sizeof(sipUserBuf),   sipUser);
  copyToField(sipPwBuf,     sizeof(sipPwBuf),     sipPw);
  copyToField(myIpBuf,      sizeof(myIpBuf),      myIpStr);
  aSip.Init(sipServerBuf, sipPort, myIpBuf, sipPort,
            sipUserBuf, sipPwBuf, SIP_MAX_DIAL_SEC);
  aSip.SetBeepSeconds(SIP_BEEP_SECONDS);
  aSip.SetIncomingSeconds(INCOMING_CALL_SECONDS);
  aSip.SetRingFilter(ringFilter);
  applyCallSeconds();
  sipInited      = true;
  regFastTries   = 3;   // erster Versuch nach Boot scheitert oft am Timing
  lastRegisterAt = millis() - 3600000UL;   // sofort anmelden
  lastRegStatus  = -2;
}

// SIP-Anmeldung im Klartext (Web, Home Assistant)
const char *sipStateText() {
  if (sipServer.length() == 0) return "nicht eingerichtet";
  switch (aSip.RegisterStatus()) {
    case 200: return "angemeldet";
    case -1:  return "verbinde";
    case 0:   return "Anlage antwortet nicht";
    case 401:
    case 407: return "Zugangsdaten falsch";
    case 403: return "abgelehnt";
    case 404: return "Benutzer unbekannt";
    default:  return "Fehler";
  }
}

// Registrierung rechtzeitig erneuern; ist sie fehlgeschlagen, bald erneut versuchen
static void registerLoop() {
  if (!netUp() || sipServer.length() == 0 || aSip.IsRegistering()) return;

  int st = aSip.RegisterStatus();
  if (st != lastRegStatus) {
    lastRegStatus = st;
    if (st == 200)    logMsg("SIP: angemeldet");
    else if (st >= 0) logMsg("SIP: Anmeldung fehlgeschlagen (%d, %s)", st, sipStateText());
    mqttStateDue = true;
  }

  uint32_t interval;
  if (aSip.IsRegistered()) {
    int exp = aSip.RegisterExpires();   // der Registrar darf kuerzen
    if (exp <= 0 || exp > SIP_REG_EXPIRES) exp = SIP_REG_EXPIRES;
    interval = (uint32_t)(exp > 60 ? exp - 30 : exp / 2) * 1000UL;
  } else {
    interval = regFastTries ? 1000UL : (uint32_t)SIP_REG_RETRY_SEC * 1000UL;
  }
  if (millis() - lastRegisterAt < interval) return;
  if (!aSip.IsRegistered() && regFastTries) regFastTries--;
  lastRegisterAt = millis();
  aSip.StartRegister(SIP_REG_EXPIRES);
}

// ------------------------------------------------------------
//  Rufkette
// ------------------------------------------------------------
// Eine Nummer der Rufkette anrufen (nur Signalisierung + Beep)
static bool dialIndex(uint8_t idx) {
  String nr;
  if (!dialTarget(dialList, idx, nr)) return false;
  copyToField(dialBuf, sizeof(dialBuf), nr);
  if (!aSip.Dial(dialBuf, SIP_CALLER_NAME)) return false;   // Anruf laeuft schon
  lastCallNr = nr;
  dtmfBuf    = "";
  callCount++;
  logEvent(EV_CALL, nr);
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
static void chainLoop() {
  static uint32_t idleSince = 0;
  if (chainIdx < 0 || aSip.IsBusy()) { idleSince = 0; return; }
  if (aSip.LastCallResult() == Sip::CALL_ANSWERED) { stopChain(); return; }
  if (!idleSince) idleSince = millis();
  if (millis() - idleSince < 1000) return;   // Anlage den vorigen Anruf abschliessen lassen
  idleSince = 0;
  String nr;
  if (!dialTarget(dialList, chainIdx + 1, nr)) {   // Ende der Kette, niemand hat abgenommen
    logEvent(EV_NOANSWER);
    stopChain();
    return;
  }
  chainIdx++;
  if (!dialIndex(chainIdx)) stopChain();
}

// ------------------------------------------------------------
//  Gaestecodes
// ------------------------------------------------------------
static bool guestUsable(uint32_t until) {
  return until == 0 || (timeValid() && (uint32_t)time(nullptr) < until);
}

// Ruft fn(code, until, once, name, start, end) fuer jeden gueltigen Eintrag auf;
// fn liefert true zum Abbrechen
template <typename F> static void forEachGuest(F fn) {
  int start = 0;
  while (start < (int)guestCodes.length()) {
    int end = guestCodes.indexOf(';', start);
    if (end < 0) end = guestCodes.length();
    String code, name; uint32_t until; bool once;
    if (parseGuest(guestCodes.substring(start, end), code, until, once, name) &&
        fn(code, until, once, name, start, end)) return;
    start = end + 1;
  }
}

static bool hasActiveGuest() {
  bool any = false;
  forEachGuest([&](const String &, uint32_t until, bool, const String &, int, int) {
    any = guestUsable(until);
    return any;
  });
  return any;
}

// Laengster Code, der gerade gilt -> ab dieser Laenge ist eine Eingabe falsch
static size_t maxCodeLen() {
  size_t m = dtmfPin.length();
  forEachGuest([&](const String &code, uint32_t until, bool, const String &, int, int) {
    if (guestUsable(until) && code.length() > m) m = code.length();
    return false;
  });
  return m < 4 ? 4 : m;
}

// Passt die Eingabe zu einem gueltigen Gaestecode? Einmal-Codes werden verbraucht.
static bool useGuest(const String &input, String &name) {
  int rmStart = -1, rmEnd = -1;
  bool hit = false;
  forEachGuest([&](const String &code, uint32_t until, bool once, const String &n, int s, int e) {
    if (code != input || !guestUsable(until)) return false;
    hit  = true;
    name = n.length() ? n : String("Gast");
    if (once) { rmStart = s; rmEnd = e; }
    return true;
  });
  if (rmStart >= 0) {
    // Eintrag samt Trennzeichen entfernen
    if (rmEnd < (int)guestCodes.length())  guestCodes.remove(rmStart, rmEnd - rmStart + 1);
    else if (rmStart > 0)                  guestCodes.remove(rmStart - 1);
    else                                   guestCodes = "";
    saveSettings();
  }
  return hit;
}

// ------------------------------------------------------------
//  Eingehende Anrufe
// ------------------------------------------------------------
uint32_t incomingLockedSec() {
  if (!locked) return 0;
  uint32_t passed = millis() - lockStart;
  uint32_t total  = (uint32_t)INCOMING_LOCK_MIN * 60000UL;
  if (passed >= total) { locked = false; failCount = 0; return 0; }
  return (total - passed) / 1000 + 1;
}

bool incomingReady() {
  return incomingOn && sipInited && aSip.IsRegistered() && incomingLockedSec() == 0 &&
         (dtmfPin.length() > 0 || hasActiveGuest());
}

static void codeFailed() {
  uint32_t window = (uint32_t)INCOMING_LOCK_MIN * 60000UL;
  if (failCount == 0 || millis() - firstFailAt > window) { failCount = 0; firstFailAt = millis(); }
  failCount++;
  callTries++;
  logEvent(EV_CODE_WRONG, aSip.IncomingFrom());
  if (failCount >= INCOMING_LOCK_FAILS && !locked) {
    locked    = true;
    lockStart = millis();
    logMsg("Eingehende Anrufe fuer %d min gesperrt (zu viele falsche Codes)", INCOMING_LOCK_MIN);
    queuePush(0, "Zu viele falsche Codes am Telefon – Anrufe für " + String(INCOMING_LOCK_MIN) +
                 " min gesperrt");
  }
  if (callTries >= INCOMING_MAX_TRIES || locked) aSip.Hangup();
}

static void openByPhone(EventType ev, const String &detail) {
  dtmfBuf = "";
  startBuzzer(ev, detail);
  aSip.Hangup();
}

// Ziffern bei einem eingehenden Anruf: ganzer Code muss stimmen, * oder # loescht
// Oeffnen-Code oder gueltiger Gaestecode (Telefon, Tastenfeld)
bool checkDoorCode(const String &code, EventType &ev, String &name) {
  if (dtmfPin.length() && code == dtmfPin) { ev = EV_OPEN_PHONE; name = ""; return true; }
  if (useGuest(code, name))                { ev = EV_OPEN_GUEST; return true; }
  return false;
}

static void incomingDigit(char d) {
  if (d == '*' || d == '#') { dtmfBuf = ""; return; }
  dtmfBuf += d;
  String name;
  if (dtmfPin.length() && dtmfBuf == dtmfPin) { openByPhone(EV_OPEN_PHONE, aSip.IncomingFrom()); return; }
  if (useGuest(dtmfBuf, name))                { openByPhone(EV_OPEN_GUEST, name); return; }
  if (dtmfBuf.length() >= maxCodeLen()) {
    dtmfBuf = "";
    codeFailed();
  }
}

// Ziffern vom Telefon. Ausgehender Anruf (Bewohner hat abgehoben): ohne Code
// oeffnet '*', mit Code die richtige Ziffernfolge.
static void handleDtmf() {
  char d = aSip.ReadDtmf();
  if (!d) return;
  Serial.println(F("DTMF empfangen"));   // Ziffer nicht ausgeben (Code)
  if (aSip.IsIncoming()) { incomingDigit(d); return; }
  if (dtmfPin.length() == 0) {
    if (d == '*') openByPhone(EV_OPEN_PHONE, "");
    return;
  }
  if (d == '*' || d == '#') { dtmfBuf = ""; return; }
  dtmfBuf += d;
  if (dtmfBuf.length() > 16) dtmfBuf.remove(0, dtmfBuf.length() - 16);
  if (dtmfBuf.endsWith(dtmfPin)) openByPhone(EV_OPEN_PHONE, "");
}

void phoneLoop() {
  // Erst nach initSip(): vorher ist der UDP-Socket nicht offen (ESP32 meldet sonst
  // bei jedem Aufruf einen Fehler)
  if (!sipInited) return;
  aSip.Processing(acSipIn, sizeof(acSipIn));
  registerLoop();

  // Annahme eingehender Anrufe nachfuehren (Einstellung, Sperre, Codes, Ablauf)
  static uint32_t lastCheck = 0;
  if (millis() - lastCheck > 1000) {
    lastCheck = millis();
    aSip.SetAcceptIncoming(incomingReady());
  }
  if (aSip.RingCall()) triggerRing(String("Anruf ") + aSip.RingCaller());
  if (aSip.NewIncoming()) {
    callTries = 0;
    dtmfBuf   = "";
    logEvent(EV_INCOMING, aSip.IncomingFrom());
    markActivity();
  }

  handleDtmf();
  chainLoop();
}

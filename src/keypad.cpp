// Tastenfeld / RFID-Leser mit Wiegand-Schnittstelle (ESP32).
//
// Ein Leser sendet ueber zwei Leitungen (D0 = 0-Bit, D1 = 1-Bit, je kurzer
// LOW-Impuls) entweder Tastendruecke (4 Bit, bzw. 8 Bit mit Pruefnibble) oder
// Kartennummern (26 Bit mit Paritaet, 34 Bit). Ein Datenpaket ist zu Ende, wenn
// 25 ms kein Bit mehr kommt.
//
// Tastenfeld: Ziffern, '#' bestaetigt, '*' loescht. Gueltig sind Oeffnen-Code und
// Gaestecodes (wie am Telefon). Karten: Nummern aus der Kartenliste im Web.
#include "app.h"

#if defined(PIN_WG_D0) && defined(ESP32)

static volatile uint64_t wgBits   = 0;
static volatile uint8_t  wgCount  = 0;
static volatile uint32_t wgLastUs = 0;
static String   keyBuf;
static uint32_t keyAt      = 0;
static String   lastCard;
static uint8_t  wrongCount = 0;
static uint32_t firstWrongAt = 0, lockedAt = 0;
static bool     locked     = false;

static void IRAM_ATTR wgD0() {
  if (wgCount < 64) { wgBits <<= 1; wgCount++; }
  wgLastUs = micros();
}
static void IRAM_ATTR wgD1() {
  if (wgCount < 64) { wgBits = (wgBits << 1) | 1; wgCount++; }
  wgLastUs = micros();
}

bool keypadAvailable() { return true; }
String keypadLastCard() { return lastCard; }

void keypadBegin() {
  pinMode(PIN_WG_D0, INPUT_PULLUP);
  pinMode(PIN_WG_D1, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_WG_D0), wgD0, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_WG_D1), wgD1, FALLING);
}

static bool isLocked() {
  if (locked && millis() - lockedAt > (uint32_t)KEYPAD_LOCK_MIN * 60000UL) { locked = false; wrongCount = 0; }
  return locked;
}

static void wrong(EventType ev, const String &detail) {
  logEvent(ev, detail);
  uint32_t window = (uint32_t)KEYPAD_LOCK_MIN * 60000UL;
  if (wrongCount == 0 || millis() - firstWrongAt > window) { wrongCount = 0; firstWrongAt = millis(); }
  if (++wrongCount >= KEYPAD_LOCK_FAILS && !locked) {
    locked   = true;
    lockedAt = millis();
    logMsg("Tastenfeld/Leser fuer %d min gesperrt (zu viele Fehlversuche)", KEYPAD_LOCK_MIN);
    queuePush(0, "Zu viele Fehlversuche am Tastenfeld – " + String(KEYPAD_LOCK_MIN) + " min gesperrt");
  }
}

static void submitCode() {
  String code = keyBuf;
  keyBuf = "";
  if (!code.length()) return;
  if (isLocked()) { logMsg("Tastenfeld gesperrt - Eingabe ignoriert"); return; }
  EventType ev;
  String name;
  if (checkDoorCode(code, ev, name)) {
    wrongCount = 0;
    startBuzzer(EV_OPEN_KEYPAD, ev == EV_OPEN_GUEST ? name : String());
    aSip.Hangup();
  } else {
    wrong(EV_KEYPAD_WRONG, String());
  }
}

static void cardRead(uint32_t nr) {
  lastCard = String(nr);
  if (isLocked()) { logMsg("Leser gesperrt - Karte %s ignoriert", lastCard.c_str()); return; }
  // Kartenliste "Nummer:Name;..."
  int start = 0;
  while (start < (int)wgCards.length()) {
    int end = wgCards.indexOf(';', start);
    if (end < 0) end = wgCards.length();
    String e = wgCards.substring(start, end);
    int c = e.indexOf(':');
    if (c > 0 && e.substring(0, c) == lastCard) {
      wrongCount = 0;
      startBuzzer(EV_OPEN_CARD, e.substring(c + 1));
      aSip.Hangup();
      return;
    }
    start = end + 1;
  }
  wrong(EV_CARD_UNKNOWN, lastCard);
}

// Paritaet fuer 26 Bit: Bit 25 gerade ueber 24..13, Bit 0 ungerade ueber 12..1
static bool parity26(uint32_t v) {
  int even = 0, odd = 0;
  for (int i = 13; i <= 24; i++) even += (v >> i) & 1;
  for (int i = 1; i <= 12; i++)  odd  += (v >> i) & 1;
  return ((even + ((v >> 25) & 1)) % 2 == 0) && ((odd + (v & 1)) % 2 == 1);
}

void keypadLoop() {
  if (!wgOn) { wgCount = 0; keyBuf = ""; return; }
  // Unvollstaendige Eingabe nach 10 s verwerfen
  if (keyBuf.length() && millis() - keyAt > 10000) keyBuf = "";
  // Abgelaufene Sperre und Fehlversuche auch ohne neue Eingabe vergessen (sonst
  // gaelten sie nach dem millis-Ueberlauf nach 49,7 Tagen wieder)
  isLocked();
  if (wrongCount && !locked && millis() - firstWrongAt > (uint32_t)KEYPAD_LOCK_MIN * 60000UL) wrongCount = 0;
  if (!wgCount || micros() - wgLastUs < 25000) return;

  noInterrupts();
  uint64_t bits  = wgBits;
  uint8_t  count = wgCount;
  wgBits = 0;
  wgCount = 0;
  interrupts();

  int key = -1;
  if (count == 4) {
    key = bits & 0x0F;
  } else if (count == 8) {
    // oberes Nibble = invertiertes unteres
    if (((bits >> 4) & 0x0F) == (~bits & 0x0F)) key = bits & 0x0F;
  } else if (count == 26) {
    if (parity26((uint32_t)bits)) cardRead((uint32_t)((bits >> 1) & 0xFFFFFF));
    return;
  } else if (count == 34) {
    cardRead((uint32_t)((bits >> 1) & 0xFFFFFFFFUL));
    return;
  } else {
    return;   // unbekanntes Format
  }
  if (key < 0) return;
  markActivity();
  keyAt = millis();
  if (key <= 9) {
    if (keyBuf.length() < 12) keyBuf += (char)('0' + key);
  } else if (key == 10) {   // '*'
    keyBuf = "";
  } else if (key == 11) {   // '#'
    submitCode();
  }
}

#else   // kein Wiegand auf diesem Board (ESP8266)

bool   keypadAvailable() { return false; }
String keypadLastCard()  { return String(); }
void   keypadBegin()     {}
void   keypadLoop()      {}

#endif

// Relais/Summer, Eingaenge (Klingel, Taster, Tuerkontakt), Zeitfenster,
// OLED-Anzeige und Status-LED.
#include "app.h"
#include <Ticker.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>
#include "holidays.h"
#include "presswatch.h"
#include "timewin.h"

static Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

bool     buzzerActive    = false;
static uint32_t buzzerOffAt = 0;          // millis-Zeitpunkt zum Abschalten
static uint32_t buzzerEndedAt = 0;        // millis, als der Summer zuletzt ausging (Klingel-Sperre)
static Ticker   buzzerTicker;             // schaltet das Relais auch ab, wenn die Loop haengt

bool     signalActive    = false;         // aktueller (entprellter) Zustand
uint32_t lastRingAt      = 0;             // millis des letzten Klingelns
uint32_t lastSignalOffAt = 0;             // millis, als das Signal zuletzt endete

bool     doorOpen        = false;
bool     doorAlerted     = false;
static uint32_t doorOpenedAt = 0;
static uint32_t passWatchUntil = 0;       // nach dem Summer: Tuer muss bis dahin aufgehen

bool     relay2Active    = false;
static uint32_t relay2OffAt = 0;
static Ticker   relay2Ticker;

bool     hasDisplay      = false;         // OLED beim I2C-Scan gefunden?
bool     displayDirty    = true;
static bool     displayOn      = true;
static uint32_t lastActivityAt = 0;       // fuer Bildschirmschoner

// ------------------------------------------------------------
//  Zeitfenster
// ------------------------------------------------------------
static bool localNow(struct tm &lt) {
  if (!timeValid()) return false;
  time_t now = time(nullptr);
  localtime_r(&now, &lt);
  return true;
}

static timewin::Day dayOf(const struct tm &lt) {
  return { lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_wday };
}

// Ohne gueltige Uhrzeit: weder Nachtruhe noch Praxis-Modus (sicherer Zustand)
bool isQuiet() {
  struct tm lt;
  return quietOn && localNow(lt) && timewin::inRange(lt.tm_hour * 60 + lt.tm_min, quietFrom, quietTo);
}

// Gesetzlicher Feiertag heute im eingestellten Bundesland (sonst nullptr)
const char *holidayToday() {
  struct tm lt;
  if (!praxisHoliday.length() || !localNow(lt)) return nullptr;
  return holidays::name(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, praxisHoliday.c_str());
}

// Gilt der Praxis-Modus an diesem Tag? (Wochentag, kein Ausnahme- oder Feiertag)
static bool praxisDay(const timewin::Day &d) {
  if (!(praxisDays & timewin::dayBit(d.wday))) return false;
  if (isFreeDay(d.d, d.m, d.y)) return false;
  return !holidays::name(d.y, d.m, d.d, praxisHoliday.c_str());
}

// Fenster ueber Mitternacht (z.B. 22:00-02:00): der Teil danach gehoert zum Vortag
bool isPraxis() {
  struct tm lt;
  if (!praxisOn || !localNow(lt)) return false;
  timewin::Day t = dayOf(lt);
  uint16_t m = lt.tm_hour * 60 + lt.tm_min;
  return timewin::inWindow(t, m, praxisFrom, praxisTo, praxisDay) ||
         timewin::inWindow(t, m, praxisFrom2, praxisTo2, praxisDay);
}

// Darf sich ein Benutzer jetzt anmelden? Tage 0 = alle, Von == Bis = ganztags.
// Ohne gueltige Uhrzeit nur, wenn kein Zeitfenster eingestellt ist.
bool userWindowOk(const UserEntry &u) {
  if (u.days == 0 && u.from == u.to) return true;
  struct tm lt;
  if (!localNow(lt)) return false;
  auto dayOk = [&u](const timewin::Day &d) { return !u.days || (u.days & timewin::dayBit(d.wday)); };
  timewin::Day t = dayOf(lt);
  if (u.from == u.to) return dayOk(t);
  return timewin::inWindow(t, lt.tm_hour * 60 + lt.tm_min, u.from, u.to, dayOk);
}

// ------------------------------------------------------------
//  Relais / Summer
// ------------------------------------------------------------
static void relayPin(bool on) {
  digitalWrite(PIN_RELAY, (RELAY_ACTIVE_LOW ? !on : on) ? HIGH : LOW);
}

// Laeuft im Timer-Kontext: nur den Pin schalten, den Rest macht die Loop.
// Ticker mit Funktionszeiger statt std::function: auf dem ESP32 laeuft der Timer
// im esp_timer-Task (Core 0) und laese sonst dasselbe Funktionsobjekt, das die Loop
// beim Neustellen (once_ms) oder Abschalten (detach) gerade ersetzt.
static void buzzerTimeout(void *) {
  relayPin(false);
}

static const char *openSourceName(EventType source) {
  switch (source) {
    case EV_OPEN_WEB:   return "Web";
    case EV_OPEN_BTN:   return "Taster";
    case EV_OPEN_PHONE: return "Telefon";
    case EV_OPEN_HA:    return "Home Assistant";
    case EV_OPEN_GUEST: return "Gästecode";
    case EV_OPEN_TELEGRAM: return "Telegram";
    case EV_OPEN_KEYPAD:   return "Tastenfeld";
    case EV_OPEN_CARD:     return "Karte";
    case EV_OPEN_HOMEKIT:  return "Apple Home";
    default:            return "automatisch";
  }
}

// Tuer oeffnen; source = EV_OPEN_* (wer hat geoeffnet, fuers Protokoll).
// Laeuft der Summer schon (z.B. doppelter Klick in der Weboberflaeche), wird er nur
// verlaengert: kein neuer Zaehler, kein Protokolleintrag, keine Mitteilung.
void startBuzzer(EventType source, const String &detail) {
  stopChain();   // Tuer ist auf -> niemanden mehr anrufen
  uint32_t ms = (uint32_t)buzzerSeconds * 1000UL;
  // Erst den Timer (neu) stellen, dann das Relais: ein gerade ablaufender alter
  // Timer schaltet das verlaengerte Oeffnen so nicht ab
  buzzerTicker.once_ms(ms, buzzerTimeout, (void *)nullptr);
  relayPin(true);
  buzzerOffAt  = millis() + ms;
  markActivity();
  displayDirty = true;
  mqttStateDue = true;
  // Mit Tuerkontakt: geht die Tuer danach wirklich auf?
  uint32_t passUntil = (millis() + ((uint32_t)buzzerSeconds + DOOR_PASS_WINDOW_SEC) * 1000UL) | 1;
  if (buzzerActive) {
    if (passWatchUntil) passWatchUntil = passUntil;
    return;
  }
  buzzerActive   = true;
  passWatchUntil = doorOn && !doorOpen ? passUntil : 0;
  buzzerTriggers++;
  logEvent(source, detail);
  if (source != EV_OPEN_AUTO) {
    String who = openSourceName(source);
    if (detail.length()) who += " " + detail;
    queuePush(PUSH_EV_OPEN, "Tür geöffnet (" + who + ")");
  }
}

void stopBuzzer() {
  buzzerTicker.detach();
  if (buzzerActive) buzzerEndedAt = millis() | 1;   // Klingel-Sperre (Einkopplung) laeuft ab jetzt
  buzzerActive = false;
  relayPin(false);
  displayDirty = true;
  mqttStateDue = true;
}

// ------------------------------------------------------------
//  Zweites Relais (z.B. Tor, zweite Tuer)
// ------------------------------------------------------------
static void relay2Pin(bool on) {
  digitalWrite(PIN_RELAY2, (RELAY2_ACTIVE_LOW ? !on : on) ? HIGH : LOW);
}

static void relay2Timeout(void *) {
  relay2Pin(false);
}

// who = Quelle fuer das Protokoll ("Web Anna", "Home Assistant", ...).
// Laeuft es schon: nur verlaengern (wie beim Summer).
bool startRelay2(const String &who) {
  if (!relay2On) return false;
  uint32_t ms = (uint32_t)relay2Seconds * 1000UL;
  relay2Ticker.once_ms(ms, relay2Timeout, (void *)nullptr);
  relay2Pin(true);
  relay2OffAt  = millis() + ms;
  markActivity();
  mqttStateDue = true;
  if (relay2Active) return true;
  relay2Active = true;
  logEvent(EV_OPEN2, relay2Name + (who.length() ? " – " + who : String()));
  queuePush(PUSH_EV_OPEN, relay2Name + " geöffnet" + (who.length() ? " (" + who + ")" : String()));
  return true;
}

void stopRelay2() {
  relay2Ticker.detach();
  relay2Active = false;
  relay2Pin(false);
  mqttStateDue = true;
}

// ------------------------------------------------------------
//  Display
// ------------------------------------------------------------
// Wird der "Es klingelt"-Hinweis gerade angezeigt?
bool isRinging() {
  return lastRingAt != 0 && (millis() - lastRingAt) < RING_NOTIFY_MS;
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

void displayBegin() {
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
    return;
  }
  if (!display.begin(SSD1306_SWITCHCAPVCC, foundAddr)) {
    Serial.printf("SSD1306 Init fehlgeschlagen (Adresse 0x%02X)!\n", foundAddr);
    return;
  }
  hasDisplay = true;
  Serial.printf("Display OK auf 0x%02X\n", foundAddr);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("Starte..."));
  display.println(F("WLAN verbinden"));
  display.display();
  lastActivityAt = millis();
}

static void drawDisplay() {
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
  if (netUp()) {
    display.print(F("IP "));
    display.println(netIP().toString());
  } else if (portalActive) {
    display.println(F("WLAN: Einrichtung"));
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

// Wird aufgerufen, sobald das WLAN-Einrichtungsportal (AP) startet
void showPortalInfo(const char *ip) {
  if (!hasDisplay) return;
  markActivity();
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(F("WLAN einrichten:"));
  display.drawFastHLine(0, 10, OLED_WIDTH, SSD1306_WHITE);
  display.setCursor(0, 14);
  display.println(F(WIFI_AP_NAME));
  display.print(F("PW: "));
  display.println(apPw.length() ? apPw : String("(offen)"));
  display.setCursor(0, 42);
  display.println(F("Browser:"));
  display.print(F("http://"));
  display.println(ip);
  display.display();
  displayDirty = false;
}

// Display nur neu zeichnen, wenn sich etwas Sichtbares geaendert hat
static void updateDisplay() {
  // Bildschirmschoner: OLED nach Inaktivitaet ausschalten (nicht bei offenem Portal)
  if (SCREEN_TIMEOUT_SECONDS > 0 && displayOn && !portalActive &&
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
  bool wifi    = netUp();
  if (ringing != lastRinging || wifi != lastWifi) {
    lastRinging  = ringing;
    lastWifi     = wifi;
    displayDirty = true;
  }

  // Nicht waehrend eines Anrufs zeichnen: das I2C-Update blockiert und
  // stoert den RTP-Takt. displayDirty bleibt gesetzt -> wird danach nachgeholt.
  // Bei offenem Portal bleibt dessen Hinweis stehen.
  if (displayOn && displayDirty && !aSip.IsBusy() && !portalActive) {
    displayDirty = false;
    drawDisplay();
  }
}

// ------------------------------------------------------------
//  Status-LED: Summer an = Dauerlicht, Einrichtungs-WLAN = schnelles Blinken,
//  kein WLAN = langsames Blinken, SIP nicht angemeldet = kurzer Blitz alle 2 s
// ------------------------------------------------------------
static void ledLoop() {
  uint32_t t = millis();
  bool on;
  if (buzzerActive)                                      on = true;
  else if (portalActive)                                 on = (t / 100) % 2;
  else if (!netUp())                on = (t / 500) % 2;
  else if (sipServer.length() && !aSip.IsRegistered())   on = (t % 2000) < 100;
  else                                                   on = false;
  static int8_t last = -1;
  if (on != last) {
    last = on;
    digitalWrite(PIN_STATUS_LED, (STATUS_LED_ACTIVE_LOW ? !on : on) ? HIGH : LOW);
  }
}

// ------------------------------------------------------------
//  Eingaenge (entprellt)
// ------------------------------------------------------------
// Klingeln auswerten: Praxis-Modus oeffnet, sonst Rufkette (ausser Nachtruhe).
// Zeitkritisches zuerst (Oeffnen bzw. Anruf, Home Assistant, Apple Home). Die
// Mitteilung wird nur eingereiht: gesendet blockiert sie auf dem ESP8266 Sekunden
// (bei gestoertem Internet deutlich laenger) - das Klingeln kaeme dann zu spaet an.
static void onRing(const String &detail) {
  if (isPraxis()) {
    startBuzzer(EV_OPEN_AUTO);
    mqttRing();
    homekitRing();
    logEvent(EV_RING, detail);
    queuePush(PUSH_EV_RING, "Es hat geklingelt – Tür automatisch geöffnet (Praxis-Modus)");
    return;
  }
  bool quiet = isQuiet();
  if (callOnRing && !quiet) startChain();
  mqttRing();
  homekitRing();
  logEvent(quiet ? EV_RING_QUIET : EV_RING, detail);
  // Telegram: Mitteilung mit "Oeffnen"-Knopf (falls erlaubt)
  queuePush(PUSH_EV_RING, quiet ? "Es klingelt an der Tür (Nachtruhe)" : "Es klingelt an der Tür", true);
}

// Klingeln zaehlen und auswerten; Sturmklingeln (kurz hintereinander) nur zaehlen
static void ringNow(const String &detail) {
  bool cooldown = lastRingAt != 0 && millis() - lastRingAt < RING_COOLDOWN_MS;
  signalCount++;
  stateChanged();
  lastRingAt = millis();
  markActivity();
  displayDirty = true;
  if (!cooldown) onRing(detail);
}

// Klingeln aus einer anderen Quelle (z.B. Anruf der TK-Anlage)
void triggerRing(const String &detail) {
  ringNow(detail);
  lastSignalOffAt = millis();   // Weboberflaeche: "Signal aktiv" kurz anzeigen
}

// Verpasste Druecke (Loop hing): Interrupt misst die aktiven Strecken mit, siehe
// presswatch.h. Wechselspannung und Stoerspitzen laufen nur ueber das Polling.
static presswatch::Watch ringWatch, btnWatch;

// Klingel: Signal und Taster zusammen (wie beim Polling)
static void IRAM_ATTR ringEdge() {
  bool sig = digitalRead(PIN_SIGNAL) == (SIGNAL_ACTIVE_LOW ? LOW : HIGH);
  presswatch::edge(ringWatch, sig || digitalRead(PIN_BTN_RING) == LOW, millis(), SIGNAL_DEBOUNCE_MS);
}

static void IRAM_ATTR buzzerBtnEdge() {
  presswatch::edge(btnWatch, digitalRead(PIN_BTN_BUZZER) == LOW, millis(), SIGNAL_DEBOUNCE_MS);
}

// Pin mit Interrupt? (ESP8266: GPIO16 nicht - dort bleibt es beim Polling)
static bool hasIrq(uint8_t pin) {
  return digitalPinToInterrupt(pin) != NOT_AN_INTERRUPT;
}

// Bisherige Phasen verwerfen (z.B. Druck fuer den Notfall-Reset)
static void watchReset(presswatch::Watch &w) {
  noInterrupts();
  presswatch::reset(w);
  interrupts();
}

// seenNow: das Polling hat den Druck erkannt und der Eingang ist gerade aktiv.
// true = der Interrupt hat einen Druck gesehen, den das Polling verpasst hat.
static bool missedPress(presswatch::Watch &w, bool seenNow) {
  noInterrupts();
  bool hit = presswatch::poll(w, seenNow, millis(), SIGNAL_DEBOUNCE_MS);
  interrupts();
  return hit;
}

// Einkopplung vom Tueroeffner auf die Klingelleitung: solange der Summer an ist und
// RING_BUZZER_GUARD_MS danach zaehlt Klingeln nicht
static bool ringGuard() {
  return buzzerActive || (buzzerEndedAt && millis() - buzzerEndedAt < RING_BUZZER_GUARD_MS);
}

// Klingel-Taster wirkt wie ein zweiter Kontakt parallel zum Klingelsignal.
// Klingelsignal ueber Optokoppler an Wechselspannung (z.B. Sensor-Eingang einer
// TK-Anlage): der Ausgang pulst dann mit 50 Hz. Darum gilt das Signal als aktiv,
// solange es in den letzten SIGNAL_AC_HOLD_MS aktiv war (bei Gleichspannung bzw.
// potentialfreiem Kontakt aendert das nichts, ausser ~40 ms laengerem Nachlauf).
// Damit dabei nicht schon zwei Stoerspitzen reichen, braucht das Signal allein
// SIGNAL_MIN_SAMPLES aktive Abtastungen.
// Ein Druck zaehlt erst, nachdem der Eingang seit dem Start einmal frei war: ein
// dauerhaft aktiver Eingang (Feuchte, klemmender Taster) klingelt nicht bei jedem
// Start (im Praxis-Modus wuerde das die Tuer oeffnen).
static void handleSignalInput() {
  static bool     lastRaw      = false;
  static uint32_t changedAt    = 0;
  static uint32_t lastActiveAt = 0;
  static bool     everActive   = false;
  static uint8_t  sigSamples   = 0;       // aktive Abtastungen des Signals seit changedAt
  static bool     armed        = false;   // war der Eingang seit dem Start einmal frei?
  static bool     stuckLogged  = false;
  bool sigRaw = digitalRead(PIN_SIGNAL) == (SIGNAL_ACTIVE_LOW ? LOW : HIGH);
  if (sigRaw) {
    lastActiveAt = millis();
    everActive   = true;
  }
  bool sig     = everActive && millis() - lastActiveAt < SIGNAL_AC_HOLD_MS;
  bool btn     = digitalRead(PIN_BTN_RING) == LOW;
  bool pressed = sig || btn;

  if (pressed != lastRaw) {
    lastRaw    = pressed;
    changedAt  = millis();
    sigSamples = 0;
  }
  if (sigRaw && sigSamples < 255) sigSamples++;

  if ((millis() - changedAt) > SIGNAL_DEBOUNCE_MS) {
    // Erstmals frei: was der Interrupt waehrend des Starts gesehen hat, verwerfen -
    // sonst kaeme ein Druck aus der Startphase danach als "verpasst" doch noch an
    if (!pressed && !armed) { armed = true; watchReset(ringWatch); }
    bool on = pressed && (btn || sigSamples >= SIGNAL_MIN_SAMPLES);
    if (on != signalActive) {
      signalActive = on;
      if (!on) {
        lastSignalOffAt = millis();
      } else if (!armed) {
        if (!stuckLogged) logMsg("Klingel-Eingang seit dem Start aktiv - ignoriert, bis er einmal frei war");
        stuckLogged = true;
      } else if (ringGuard()) {
        logMsg("Klingeln waehrend/kurz nach dem Summer ignoriert (Einkopplung)");
      } else {
        ringNow(String());
      }
      displayDirty = true;
    }
  }

  if (missedPress(ringWatch, signalActive && pressed) && armed && !ringGuard()) {
    logMsg("Klingeln per Interrupt erkannt (Loop war blockiert)");
    ringNow(String());
    lastSignalOffAt = millis();   // Weboberflaeche: "Signal aktiv" kurz anzeigen
  }
}

// Summer-Taster (entprellt): jeder Druck oeffnet wie der Web-Button - erst, nachdem
// der Taster seit dem Start einmal losgelassen war (sonst oeffnete ein klemmender
// Taster bei jedem Start die Tuer)
static void buzzerButtonPressed() {
  startBuzzer(EV_OPEN_BTN);
  aSip.Hangup();   // Tuer ist auf -> laufenden Anruf beenden
}

static void handleBuzzerButton() {
  static bool     lastRaw     = false;
  static bool     state       = false;
  static uint32_t changedAt   = 0;
  static bool     armed       = false;
  static bool     stuckLogged = false;
  bool pressed = digitalRead(PIN_BTN_BUZZER) == LOW;
  if (pressed != lastRaw) {
    lastRaw   = pressed;
    changedAt = millis();
  }
  if ((millis() - changedAt) > SIGNAL_DEBOUNCE_MS) {
    if (!pressed && !armed) { armed = true; watchReset(btnWatch); }   // siehe handleSignalInput
    if (pressed != state) {
      state = pressed;
      if (state && armed) {
        buzzerButtonPressed();
      } else if (state && !stuckLogged) {
        logMsg("Summer-Taster seit dem Start gedrueckt - ignoriert, bis er einmal losgelassen war");
        stuckLogged = true;
      }
    }
  }
  if (missedPress(btnWatch, state && pressed) && armed) {
    logMsg("Summer-Taster per Interrupt erkannt (Loop war blockiert)");
    buzzerButtonPressed();
  }
}

// Tuerkontakt (Reed): geschlossen = LOW. Offen zu lange -> Meldung.
static void handleDoorContact() {
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
    if (open) {
      doorOpenedAt = millis();
      if (passWatchUntil) { passWatchUntil = 0; logEvent(EV_DOOR_PASSED); }
      else if (!buzzerActive) logEvent(EV_DOOR_OPENED);
    }
    doorAlerted  = false;
    mqttStateDue = true;
    displayDirty = true;
  }
  // Summer war an, Tuer ging aber nicht auf (niemand hereingekommen)
  if (passWatchUntil && (int32_t)(millis() - passWatchUntil) >= 0) {
    passWatchUntil = 0;
    logEvent(EV_DOOR_UNUSED);
    queuePush(PUSH_EV_UNUSED, "Summer ausgelöst, aber die Tür blieb zu");
  }
  if (doorOpen && doorAlertMin && !doorAlerted &&
      millis() - doorOpenedAt > (uint32_t)doorAlertMin * 60000UL) {
    doorAlerted = true;
    logEvent(EV_DOOR_ALERT);
    queuePush(PUSH_EV_DOOR, "Die Tür steht seit " + String(doorAlertMin) + " min offen");
    mqttStateDue = true;
  }
}

void doorBegin() {
#if defined(ESP32)
  // ESP32-Core 3.x verwirft digitalWrite() vor pinMode(). Den Pegel daher direkt
  // nach dem Umschalten setzen (Mikrosekunden, ein Relais zieht dabei nicht an).
  pinMode(PIN_RELAY, OUTPUT);
  relayPin(false);
  pinMode(PIN_RELAY2, OUTPUT);
  relay2Pin(false);
  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, STATUS_LED_ACTIVE_LOW ? HIGH : LOW);
#else
  // Erst den Ruhepegel setzen, dann auf Ausgang schalten: sonst zieht ein
  // active-low-Relais beim Start kurz an.
  relayPin(false);
  pinMode(PIN_RELAY, OUTPUT);
  relay2Pin(false);
  pinMode(PIN_RELAY2, OUTPUT);
  digitalWrite(PIN_STATUS_LED, STATUS_LED_ACTIVE_LOW ? HIGH : LOW);
  pinMode(PIN_STATUS_LED, OUTPUT);
#endif
  pinMode(PIN_SIGNAL, INPUT_PULLUP);
  pinMode(PIN_BTN_RING, INPUT_PULLUP);
  pinMode(PIN_BTN_BUZZER, INPUT_PULLUP);
  pinMode(PIN_DOOR, INPUT_PULLUP);
  // Verpasste Druecke per Interrupt (Klingel nur, wenn beide Pins einen haben)
  if (hasIrq(PIN_SIGNAL) && hasIrq(PIN_BTN_RING)) {
    attachInterrupt(digitalPinToInterrupt(PIN_SIGNAL), ringEdge, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_BTN_RING), ringEdge, CHANGE);
  }
  if (hasIrq(PIN_BTN_BUZZER)) attachInterrupt(digitalPinToInterrupt(PIN_BTN_BUZZER), buzzerBtnEdge, CHANGE);
}

void doorLoop() {
  handleSignalInput();
  handleBuzzerButton();
  handleDoorContact();
  if (buzzerActive && (int32_t)(millis() - buzzerOffAt) >= 0) stopBuzzer();
  if (relay2Active && (int32_t)(millis() - relay2OffAt) >= 0) stopRelay2();
  // millis laeuft nach 49,7 Tagen ueber: abgelaufene Zeitpunkte loeschen, sonst
  // gaelten sie danach wieder (z.B. 10 s "Es klingelt" ohne Klingeln, danach
  // faellt echtes Klingeln in die Sperre gegen Sturmklingeln)
  uint32_t now = millis();
  if (lastRingAt && now - lastRingAt >= RING_NOTIFY_MS && now - lastRingAt >= RING_COOLDOWN_MS) lastRingAt = 0;
  if (lastSignalOffAt && now - lastSignalOffAt >= SIGNAL_HOLD_MS) lastSignalOffAt = 0;
  if (buzzerEndedAt && now - buzzerEndedAt >= RING_BUZZER_GUARD_MS) buzzerEndedAt = 0;
  ledLoop();
  if (hasDisplay) updateDisplay();
}

// Notfall-Zugang: Klingel-Taster beim Einschalten PW_RESET_HOLD_MS halten und dann
// loslassen -> Passwoerter der Weboberflaeche weg und wieder DHCP. Nur nach dem
// Einschalten oder der Reset-Taste (nicht nach Software- oder Watchdog-Neustart);
// ein dauerhaft aktiver Eingang (Feuchte, klemmender Taster) loest nie aus.
// true = Reset ausgefuehrt (Mitteilung schickt main.cpp, sobald das Netz steht).
bool checkEmergencyReset() {
#if defined(ESP32)
  esp_reset_reason_t why = esp_reset_reason();
  if (why != ESP_RST_POWERON && why != ESP_RST_EXT) return false;
#else
  uint32_t why = ESP.getResetInfoPtr()->reason;
  if (why != REASON_DEFAULT_RST && why != REASON_EXT_SYS_RST) return false;
#endif
  if (digitalRead(PIN_BTN_RING) != LOW) return false;
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
  uint32_t t0 = millis(), releasedAt = 0;
  bool held = false;
  for (;;) {
    uint32_t now = millis();
    bool pressed = digitalRead(PIN_BTN_RING) == LOW;
    if (pressed) releasedAt = 0;
    else if (!releasedAt) releasedAt = now | 1;
    if (releasedAt && now - releasedAt > SIGNAL_DEBOUNCE_MS) break;   // losgelassen
    if (!held && pressed && now - t0 >= PW_RESET_HOLD_MS) {
      held = true;
      Serial.println(F("Jetzt loslassen"));
      if (hasDisplay) {
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println(F("Jetzt loslassen"));
        display.display();
      }
    }
    if (now - t0 >= PW_RESET_HOLD_MS + PW_RESET_RELEASE_MS) {
      logMsg("Klingel-Taster nicht losgelassen - kein Notfall-Reset (Eingang dauerhaft aktiv?)");
      held = false;
      break;
    }
    delay(10);
  }
  watchReset(ringWatch);   // der Druck ist kein Klingeln
  if (!held) return false;

  webPw    = "";
  opPw     = "";
  staticIp = false;
  saveSettings();
  logMsg("Notfall-Reset: Passwoerter geloescht, DHCP aktiv");
  logEvent(EV_BOOT, F("Notfall-Reset"));
  if (hasDisplay) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println(F("Passwoerter weg,"));
    display.println(F("DHCP aktiv."));
    display.display();
  }
  delay(1500);
  return true;
}

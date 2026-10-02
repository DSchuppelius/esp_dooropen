// Relais/Summer, Eingaenge (Klingel, Taster, Tuerkontakt), Zeitfenster,
// OLED-Anzeige und Status-LED.
#include "app.h"
#include <Ticker.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>

static Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

bool     buzzerActive    = false;
static uint32_t buzzerOffAt = 0;          // millis-Zeitpunkt zum Abschalten
static Ticker   buzzerTicker;             // schaltet das Relais auch ab, wenn die Loop haengt

bool     signalActive    = false;         // aktueller (entprellter) Zustand
uint32_t lastRingAt      = 0;             // millis des letzten Klingelns
uint32_t lastSignalOffAt = 0;             // millis, als das Signal zuletzt endete

bool     doorOpen        = false;
bool     doorAlerted     = false;
static uint32_t doorOpenedAt = 0;

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

// m im Fenster from..to (Minuten)? from == to = Fenster aus. Ueber Mitternacht moeglich.
static bool inRange(uint16_t m, uint16_t from, uint16_t to) {
  if (from == to) return false;
  if (from < to) return m >= from && m < to;
  return m >= from || m < to;   // z.B. 22:00-07:00
}

// Ohne gueltige Uhrzeit: weder Nachtruhe noch Praxis-Modus (sicherer Zustand)
bool isQuiet() {
  struct tm lt;
  return quietOn && localNow(lt) && inRange(lt.tm_hour * 60 + lt.tm_min, quietFrom, quietTo);
}

bool isPraxis() {
  struct tm lt;
  if (!praxisOn || !localNow(lt)) return false;
  if (!(praxisDays & (1 << ((lt.tm_wday + 6) % 7)))) return false;   // tm_wday: 0 = Sonntag
  if (isFreeDay(lt.tm_mday, lt.tm_mon + 1, lt.tm_year + 1900)) return false;
  uint16_t m = lt.tm_hour * 60 + lt.tm_min;
  return inRange(m, praxisFrom, praxisTo) || inRange(m, praxisFrom2, praxisTo2);
}

// ------------------------------------------------------------
//  Relais / Summer
// ------------------------------------------------------------
static void relayPin(bool on) {
  digitalWrite(PIN_RELAY, (RELAY_ACTIVE_LOW ? !on : on) ? HIGH : LOW);
}

// Laeuft im Timer-Kontext: nur den Pin schalten, den Rest macht die Loop
static void buzzerTimeout() {
  relayPin(false);
}

static const char *openSourceName(EventType source) {
  switch (source) {
    case EV_OPEN_WEB:   return "Web";
    case EV_OPEN_BTN:   return "Taster";
    case EV_OPEN_PHONE: return "Telefon";
    case EV_OPEN_HA:    return "Home Assistant";
    case EV_OPEN_GUEST: return "Gästecode";
    default:            return "automatisch";
  }
}

// Tuer oeffnen; source = EV_OPEN_* (wer hat geoeffnet, fuers Protokoll)
void startBuzzer(EventType source, const String &detail) {
  stopChain();   // Tuer ist auf -> niemanden mehr anrufen
  buzzerActive = true;
  buzzerOffAt  = millis() + (uint32_t)buzzerSeconds * 1000UL;
  buzzerTriggers++;
  logEvent(source, detail);
  relayPin(true);
  buzzerTicker.once_ms((uint32_t)buzzerSeconds * 1000UL, buzzerTimeout);
  markActivity();
  displayDirty = true;
  mqttStateDue = true;
  if (source != EV_OPEN_AUTO) {
    String who = openSourceName(source);
    if (detail.length()) who += " " + detail;
    queuePush(PUSH_EV_OPEN, "Tür geöffnet (" + who + ")");
  }
}

void stopBuzzer() {
  buzzerTicker.detach();
  buzzerActive = false;
  relayPin(false);
  displayDirty = true;
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
  if (WiFi.status() == WL_CONNECTED) {
    display.print(F("IP "));
    display.println(WiFi.localIP().toString());
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
  bool wifi    = WiFi.status() == WL_CONNECTED;
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
  else if (WiFi.status() != WL_CONNECTED)                on = (t / 500) % 2;
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
// Klingeln auswerten: Praxis-Modus oeffnet, sonst Rufkette (ausser Nachtruhe)
static void onRing() {
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

// Klingel-Taster wirkt wie ein zweiter Kontakt parallel zum Klingelsignal.
// Klingelsignal ueber Optokoppler an Wechselspannung (z.B. Sensor-Eingang einer
// TK-Anlage): der Ausgang pulst dann mit 50 Hz. Darum gilt das Signal als aktiv,
// solange es in den letzten SIGNAL_AC_HOLD_MS aktiv war (bei Gleichspannung bzw.
// potentialfreiem Kontakt aendert das nichts, ausser ~40 ms laengerem Nachlauf).
static void handleSignalInput() {
  static bool     lastRaw      = false;
  static uint32_t changedAt    = 0;
  static uint32_t lastActiveAt = 0;
  static bool     everActive   = false;
  int raw = digitalRead(PIN_SIGNAL);
  if (SIGNAL_ACTIVE_LOW ? (raw == LOW) : (raw == HIGH)) {
    lastActiveAt = millis();
    everActive   = true;
  }
  bool sig     = everActive && millis() - lastActiveAt < SIGNAL_AC_HOLD_MS;
  bool pressed = sig || digitalRead(PIN_BTN_RING) == LOW;

  if (pressed != lastRaw) {
    lastRaw   = pressed;
    changedAt = millis();
  }

  if ((millis() - changedAt) > SIGNAL_DEBOUNCE_MS && pressed != signalActive) {
    signalActive = pressed;
    if (signalActive) {
      // Sturmklingeln: kurz hintereinander nur zaehlen, nicht erneut melden/anrufen
      bool cooldown = lastRingAt != 0 && millis() - lastRingAt < RING_COOLDOWN_MS;
      signalCount++;
      stateChanged();
      lastRingAt = millis();
      markActivity();
      if (!cooldown) onRing();
    } else {
      lastSignalOffAt = millis();
    }
    displayDirty = true;
  }
}

// Summer-Taster (entprellt): jeder Druck oeffnet wie der Web-Button
static void handleBuzzerButton() {
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

void doorBegin() {
#if defined(ESP32)
  // ESP32-Core 3.x verwirft digitalWrite() vor pinMode(). Den Pegel daher direkt
  // nach dem Umschalten setzen (Mikrosekunden, ein Relais zieht dabei nicht an).
  pinMode(PIN_RELAY, OUTPUT);
  relayPin(false);
  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, STATUS_LED_ACTIVE_LOW ? HIGH : LOW);
#else
  // Erst den Ruhepegel setzen, dann auf Ausgang schalten: sonst zieht ein
  // active-low-Relais beim Start kurz an.
  relayPin(false);
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_STATUS_LED, STATUS_LED_ACTIVE_LOW ? HIGH : LOW);
  pinMode(PIN_STATUS_LED, OUTPUT);
#endif
  pinMode(PIN_SIGNAL, INPUT_PULLUP);
  pinMode(PIN_BTN_RING, INPUT_PULLUP);
  pinMode(PIN_BTN_BUZZER, INPUT_PULLUP);
  pinMode(PIN_DOOR, INPUT_PULLUP);
}

void doorLoop() {
  handleSignalInput();
  handleBuzzerButton();
  handleDoorContact();
  if (buzzerActive && (int32_t)(millis() - buzzerOffAt) >= 0) stopBuzzer();
  ledLoop();
  if (hasDisplay) updateDisplay();
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
      logMsg("Notfall-Reset: Passwoerter geloescht, DHCP aktiv");
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

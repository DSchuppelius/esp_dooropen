#pragma once
// Gemeinsame Deklarationen aller Module des Tueroeffners.
//
//   main.cpp      Start, Loop, WLAN/Portal, Netzdienste, Selbstheilung
//   settings.cpp  Einstellungen (LittleFS), Zaehler/Protokoll sichern, Sicherung
//   events.cpp    Ereignisprotokoll, Syslog, Meldungen
//   door.cpp      Relais/Summer, Eingaenge, Zeitfenster, Display, Status-LED
//   phone.cpp     SIP: Anmeldung, Rufkette, Codes, eingehende Anrufe
//   push.cpp      Push-Mitteilungen (ntfy / Telegram), Telegram-Bot (ESP32)
//   keypad.cpp    Tastenfeld / RFID-Leser (Wiegand, ESP32)
//   homekit.cpp   Apple Home (HomeSpan, ESP32)
//   net.cpp       Netzwerk-Abstraktion (WLAN oder Ethernet)
//   mqtt.cpp      Home Assistant (MQTT mit Auto-Discovery)
//   web.cpp       Weboberflaeche, HTTP-API, Anmeldung

#include <Arduino.h>
#if defined(ESP32)
  #include <WiFi.h>
  #include <WebServer.h>
  #define WEB_SERVER_CLASS WebServer
#else
  #include <ESP8266WiFi.h>
  #include <ESP8266WebServer.h>
  #define WEB_SERVER_CLASS ESP8266WebServer
#endif
#include <ArduinoSIP.h>

#include "config.h"

#define FW_VERSION "1.5.0"

// ------------------------------------------------------------
//  Einstellungen (settings.cpp)
// ------------------------------------------------------------
extern uint8_t  buzzerSeconds;
extern String   dialList, dtmfPin;
extern bool     callOnRing;
extern String   sipServer, sipUser, sipPw;
extern uint16_t sipPort;
extern String   mqttServer, mqttUser, mqttPw;
extern uint16_t mqttPort;
extern String   webUser, webPw, opUser, opPw;
extern bool     quietOn;
extern uint16_t quietFrom, quietTo;
extern bool     praxisOn;
extern uint8_t  praxisDays;
extern uint16_t praxisFrom, praxisTo, praxisFrom2, praxisTo2;
extern String   praxisFree;                  // Ausnahmetage, z.B. "24.12, 31.12, 3.10.2026"
extern bool     doorOn, doorInvert;
extern uint16_t doorAlertMin;
extern uint8_t  pushType, pushEvents;
extern String   pushServer, pushTopic, pushToken;
extern bool     staticIp;
extern IPAddress ipAddr, ipGw, ipMask, ipDns;
extern bool     incomingOn;                  // Anrufe an den Tueroeffner annehmen
extern String   guestCodes;                  // "Code:bisUnix:einmalig:Name;..."
extern String   syslogServer;
extern String   apPw;                        // Passwort des Einrichtungs-WLANs
extern String   ringCallers;                 // Klingeln per Anruf: Nummern ("*" = jeder)
extern String   praxisHoliday;               // Feiertage des Bundeslands (z.B. "NW"), leer = aus
extern bool     relay2On;                    // zweites Relais aktiv
extern String   relay2Name;
extern uint8_t  relay2Seconds;
extern bool     relay2User;                  // darf auch der Tuer-Zugang schalten
extern String   users;                       // weitere Benutzer (siehe settings.cpp)
extern bool     tgOpen;                      // Telegram: Oeffnen per Knopf erlaubt
extern String   tgChats;                     // weitere erlaubte Telegram-Chat-IDs
extern bool     wgOn;                        // Tastenfeld/RFID (Wiegand) aktiv
extern String   wgCards;                     // Karten "Nummer:Name;..."
extern bool     hkOn;                        // Apple Home (HomeKit) aktiv
extern String   hkCode;                      // HomeKit-Kopplungscode (8 Ziffern)

// Zaehler (werden verzoegert gesichert)
extern uint32_t signalCount, buzzerTriggers, callCount;

void   settingsBegin();          // Dateisystem, Laden bzw. Uebernahme aus dem EEPROM
void   saveSettings();
void   saveState();              // Zaehler + Protokoll
void   stateChanged();           // -> spaeter speichern
void   settingsLoop();
String settingsJson();           // Sicherung
bool   restoreFromArgs(WEB_SERVER_CLASS &srv, String &err);

// Pruefungen (auch fuer die Weboberflaeche)
bool   pinValid(const String &p);
bool   dialListValid(const String &list);
bool   dialTarget(const String &list, uint8_t n, String &out);
bool   guestsValid(const String &g);
bool   parseGuest(const String &entry, String &code, uint32_t &until, bool &once, String &name);
bool   freeDaysValid(const String &s);
bool   isFreeDay(int d, int m, int y);
bool   apPwValid(const String &p);
bool   cardsValid(const String &s);
bool   usersValid(const String &s);
// Weitere Benutzer: i-ten Eintrag lesen (false = gibt es nicht)
struct UserEntry { String name, pw; uint8_t role; uint8_t days; uint16_t from, to; };
bool   userAt(uint8_t i, UserEntry &u);
bool   userWindowOk(const UserEntry &u);    // Zeitfenster jetzt erlaubt?
String urlDecode(const String &s);
String urlEncodeField(const String &s);
String jsonEsc(const String &s);

// ------------------------------------------------------------
//  Ereignisse (events.cpp). Neue Typen nur HINTEN anhaengen (Weboberflaeche, Dateien).
// ------------------------------------------------------------
enum EventType : uint8_t {
  EV_RING, EV_RING_QUIET, EV_CALL, EV_OPEN_WEB, EV_OPEN_BTN, EV_OPEN_PHONE, EV_OPEN_HA, EV_BOOT,
  EV_OPEN_AUTO, EV_NOANSWER, EV_DOOR_ALERT, EV_INCOMING, EV_OPEN_GUEST, EV_CODE_WRONG,
  EV_DOOR_PASSED, EV_DOOR_UNUSED, EV_DOOR_OPENED, EV_OPEN2, EV_OPEN_TELEGRAM, EV_OPEN_KEYPAD,
  EV_OPEN_CARD, EV_KEYPAD_WRONG, EV_CARD_UNKNOWN, EV_OPEN_HOMEKIT,
  EV_COUNT
};
struct LogEntry {
  uint32_t  at;          // millis()
  uint32_t  t;           // Unix-Zeit, 0 = Uhr war noch nicht gestellt
  EventType type;
  char      detail[17];  // z.B. Name des Gaestecodes, Nummer des Anrufers
};
extern LogEntry eventLog[LOG_SIZE];
extern uint8_t  logHead, logCount;
extern uint32_t logTotal;

bool        timeValid();
String      eventName(EventType t);
String      lastEventText();                        // juengster Eintrag, z.B. "Geöffnet (Web)"
void        logEvent(EventType type, const String &detail = String());
void        logRestore(uint32_t t, uint8_t type, const String &detail);   // beim Laden
// Meldung auf Seriell und Syslog. Das Format liegt im Flash (spart RAM auf dem ESP8266).
void        logMsgP(const char *fmtP, ...);
#define     logMsg(fmt, ...) logMsgP(PSTR(fmt), ##__VA_ARGS__)
void        syslogBegin();                          // Ziel (neu) aufloesen
void        eventsFlush();                          // langen Verlauf in die Datei schreiben
void        eventsCsv(WEB_SERVER_CLASS &srv);       // langen Verlauf als CSV senden
String      formatTime(uint32_t t);                 // "2026-10-02 08:46:12" (Ortszeit)

// ------------------------------------------------------------
//  Tuer, Eingaenge, Anzeige (door.cpp)
// ------------------------------------------------------------
extern bool     buzzerActive;
extern bool     signalActive;
extern uint32_t lastRingAt, lastSignalOffAt;
extern bool     doorOpen, doorAlerted;
extern bool     hasDisplay;
extern bool     displayDirty;

void doorBegin();                 // Pins
void displayBegin();
void startBuzzer(EventType source, const String &detail = String());
void stopBuzzer();
extern bool relay2Active;
bool startRelay2(const String &who);         // false = zweites Relais aus
void stopRelay2();
void triggerRing(const String &detail);      // Klingeln aus anderer Quelle (Anruf)
const char *holidayToday();                  // Feiertag heute (Praxis-Bundesland) oder nullptr
void doorLoop();                  // Eingaenge, Summer-Ende, LED, Display
void markActivity();
bool isRinging();
bool isQuiet();
bool isPraxis();
void showPortalInfo(const char *ip);
void checkEmergencyReset();

// ------------------------------------------------------------
//  Telefon (phone.cpp)
// ------------------------------------------------------------
extern Sip     aSip;
extern int8_t  chainIdx;
extern String  lastCallNr;

void initSip();                   // nach Aenderung der Zugangsdaten / neuer IP
void applyCallSeconds();          // nach Aenderung des Oeffnen-Codes
const char *sipStateText();
void phoneLoop();
bool startChain();
void stopChain();
bool incomingReady();             // werden eingehende Anrufe gerade angenommen?
uint32_t incomingLockedSec();     // Restdauer der Sperre nach falschen Codes
// Oeffnen-Code oder Gaestecode pruefen (Telefon, Tastenfeld). Treffer: ev und name
// gesetzt (Einmal-Gaestecodes werden verbraucht).
bool checkDoorCode(const String &code, EventType &ev, String &name);

// ------------------------------------------------------------
//  Push (push.cpp)
// ------------------------------------------------------------
enum PushType : uint8_t { PUSH_OFF, PUSH_NTFY, PUSH_TELEGRAM };
static const uint8_t PUSH_EV_RING = 1, PUSH_EV_OPEN = 2, PUSH_EV_DOOR = 4, PUSH_EV_UNUSED = 8;
extern volatile int lastPushCode;

void pushBegin();
void queuePush(uint8_t ev, const String &text, bool openButton = false);
void pushLoop();
bool telegramActive();            // Telegram-Bot laeuft (ESP32, Push = Telegram)

// ------------------------------------------------------------
//  Tastenfeld / RFID (keypad.cpp), Apple Home (homekit.cpp)
// ------------------------------------------------------------
void     keypadBegin();
void     keypadLoop();
String   keypadLastCard();         // zuletzt gelesene Karte (zum Anlernen)
bool     keypadAvailable();        // auf diesem Board vorhanden?
void     homekitBegin();
void     homekitLoop();
void     homekitRing();            // Klingel-Mitteilung an Apple Home
bool     homekitAvailable();
String   homekitStatus();          // z.B. "gekoppelt", "bereit zum Koppeln"

// ------------------------------------------------------------
//  Home Assistant (mqtt.cpp)
// ------------------------------------------------------------
extern String devId, baseTopic;
extern bool   mqttDiscoveryDue, mqttStateDue;

void mqttBegin();
void initMqtt();
void mqttLoop();
bool mqttConnected();
void mqttRing();
void mqttEvent(EventType type, const String &detail);

// ------------------------------------------------------------
//  Weboberflaeche (web.cpp)
// ------------------------------------------------------------
extern WEB_SERVER_CLASS server;
void webBegin();

// ------------------------------------------------------------
//  Netzwerk (net.cpp): WLAN oder Ethernet (WT32-ETH01)
// ------------------------------------------------------------
bool      netUp();                 // verbunden mit IP-Adresse?
IPAddress netIP();
IPAddress netMask();
IPAddress netGateway();
IPAddress netDns();
String    netMac();
int       netRssi();               // WLAN-Signal, bei Ethernet 0
bool      netIsEthernet();

// ------------------------------------------------------------
//  System (main.cpp)
// ------------------------------------------------------------
enum RestartReason : uint8_t {
  RR_NONE, RR_USER, RR_UPDATE, RR_WIFI_LOST, RR_LOW_HEAP, RR_WIFI_RESET, RR_RESTORE, RR_NETWORK,
  RR_LOOP_WDT
};
extern String   bootReason;
extern bool     portalActive;
extern String   myIpStr;

bool     otaActive();             // OTA aus PlatformIO nur mit Admin-Passwort
void     scheduleRestart(uint8_t reason, bool wifiReset = false);
void     restartNow(uint8_t reason);
void     writeRestartReason(uint8_t r);
void     feedWatchdog();
uint32_t uptimeSeconds();
uint32_t maxFreeBlock();

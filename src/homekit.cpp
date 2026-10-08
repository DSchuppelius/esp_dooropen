// Apple Home (HomeKit) ueber die Bibliothek HomeSpan - nur ESP32.
//
// Das Geraet erscheint als Tuerschloss mit Tuerklingel:
//  - "Aufschliessen" in der Home-App loest den Summer aus; das Schloss zeigt
//    "offen", solange der Summer laeuft, danach wieder "verriegelt".
//  - Klingeln erzeugt eine Klingel-Mitteilung auf iPhone/Watch/HomePod.
//
// HomeSpan nutzt unser WLAN und lauscht auf Port 1201. Ein-/Ausschalten wirkt erst
// nach einem Neustart.
#include "app.h"

#if defined(ESP32) && defined(HOMEKIT_ENABLED)
#include <HomeSpan.h>
#include <sodium.h>   // vor HAP.h noetig (Schluessellaengen)
#include <HAP.h>

static bool hkStarted = false;
static bool hkServing = false;   // HAP-Server und mDNS (_hap) laufen
static SpanCharacteristic *bellEvent = nullptr;

// Schloss: Ziel "offen" -> Summer; Zustand folgt dem Summer
struct DoorLock : Service::LockMechanism {
  SpanCharacteristic *current, *target;
  bool lastBuzzer = false;

  DoorLock() : Service::LockMechanism() {
    current = new Characteristic::LockCurrentState(1);   // 1 = verriegelt
    target  = new Characteristic::LockTargetState(1);
  }

  boolean update() override {
    if (target->getNewVal() == 0) {   // 0 = aufschliessen
      startBuzzer(EV_OPEN_HOMEKIT);
      aSip.Hangup();
    }
    return true;
  }

  void loop() override {
    if (buzzerActive == lastBuzzer) return;
    lastBuzzer = buzzerActive;
    current->setVal(buzzerActive ? 0 : 1);
    if (!buzzerActive && target->getVal() != 1) target->setVal(1);
    if (buzzerActive && target->getVal() != 0) target->setVal(0);
  }
};

bool homekitAvailable() { return true; }

// Kopplungscode fuer die Home-App im Format 123-45-678
static String prettyCode(const String &c) {
  return c.length() == 8 ? c.substring(0, 3) + "-" + c.substring(3, 5) + "-" + c.substring(5) : c;
}

String homekitStatus() {
  if (!hkOn) return "aus";
  if (!hkStarted) return "startet nach Neustart";
  if (!hkServing) return "wartet auf WLAN";
  if (!netUp()) return "WLAN getrennt";
  if (HAPClient::nAdminControllers() > 0) return "gekoppelt";
  return "bereit zum Koppeln – Code " + prettyCode(hkCode);
}

// Klingel-Mitteilung an Apple Home
void homekitRing() {
  if (hkStarted && bellEvent) bellEvent->setVal(0);   // 0 = einmal gedrueckt
}

void homekitBegin() {
  if (!hkOn) return;
  if (hkCode.length() != 8) {
    // Zufaelligen Kopplungscode erzeugen (keine trivialen Folgen)
    char c[9];
    snprintf(c, sizeof(c), "%08lu", (unsigned long)(esp_random() % 100000000UL));
    hkCode = c;
    if (hkCode == "00000000" || hkCode == "12345678") hkCode = "47110815";
    saveSettings();
  }
  homeSpan.setPortNum(1201);               // Port 80 gehoert der Weboberflaeche
  homeSpan.setHostNameSuffix("");          // gleicher Name wie das Geraet: tueroeffner.local
  homeSpan.setLogLevel(-1);                // keine eigenen seriellen Ausgaben
  homeSpan.setSerialInputDisable(true);    // keine seriellen Befehle ("E" loescht den NVS,
                                           // ohne Zeilenende haengt das Lesen)
  homeSpan.setPairingCode(hkCode.c_str());
  homeSpan.setQRID("TUER");
  // HomeSpan 2.0 startet HAP-Server und mDNS nur mit eigenen WLAN-Zugangsdaten - ohne
  // sie waere das Geraet in Apple Home unsichtbar. Es bekommt die unseres WLANs (stehen
  // ohnehin im NVS). homekitLoop() ruft poll() nur bei bestehender Verbindung auf, so
  // ruft HomeSpan nicht selbst WiFi.begin() auf (hoechstens einmal mit denselben Daten,
  // falls das WLAN genau waehrend poll() wegfaellt) - das WLAN verwalten weiter wir.
  homeSpan.setWifiCredentials(WiFi.SSID().c_str(), WiFi.psk().c_str());
  homeSpan.setWifiCallback([]() { hkServing = true; });   // nach hapServer->begin()
  homeSpan.begin(Category::Locks, "Türöffner", HOSTNAME, "ESP SIP-Türöffner");   // wartet fest 2 s

  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("Türöffner");
      new Characteristic::Manufacturer("DIY");
      new Characteristic::Model("ESP SIP-Türöffner");
      new Characteristic::FirmwareRevision(FW_VERSION);
    new DoorLock();
    new Service::Doorbell();
      bellEvent = new Characteristic::ProgrammableSwitchEvent();
  hkStarted = true;
  logMsg("Apple Home aktiv (Code %s)", prettyCode(hkCode).c_str());
}

void homekitLoop() {
  if (hkStarted && netUp()) homeSpan.poll();
}

#else

bool   homekitAvailable() { return false; }
String homekitStatus()    { return "nicht verfügbar"; }
void   homekitRing()      {}
void   homekitBegin()     {}
void   homekitLoop()      {}

#endif

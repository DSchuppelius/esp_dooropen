// Apple Home (HomeKit) ueber die Bibliothek HomeSpan - nur ESP32.
//
// Das Geraet erscheint als Tuerschloss mit Tuerklingel:
//  - "Aufschliessen" in der Home-App loest den Summer aus; das Schloss zeigt
//    "offen", solange der Summer laeuft, danach wieder "verriegelt".
//  - Klingeln erzeugt eine Klingel-Mitteilung auf iPhone/Watch/HomePod.
//
// HomeSpan nutzt unser WLAN (bekommt keine eigenen Zugangsdaten) und lauscht auf
// Port 1201. Ein-/Ausschalten wirkt erst nach einem Neustart.
#include "app.h"

#if defined(ESP32) && defined(HOMEKIT_ENABLED)
#include <HomeSpan.h>
#include <sodium.h>   // vor HAP.h noetig (Schluessellaengen)
#include <HAP.h>

static bool hkStarted = false;
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
  homeSpan.setLogLevel(-1);                // keine eigenen seriellen Ausgaben/Befehle
  homeSpan.setPairingCode(hkCode.c_str());
  homeSpan.setQRID("TUER");
  homeSpan.begin(Category::Locks, "Türöffner", HOSTNAME, "ESP SIP-Türöffner");
  // HomeSpan schaltet das automatische Wiederverbinden ab - wir verwalten das WLAN
  WiFi.setAutoReconnect(true);

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
  if (hkStarted) homeSpan.poll();
}

#else

bool   homekitAvailable() { return false; }
String homekitStatus()    { return "nicht verfügbar"; }
void   homekitRing()      {}
void   homekitBegin()     {}
void   homekitLoop()      {}

#endif

// SIP-Tueroeffner fuer ESP32 / ESP8266 - Start, Loop, WLAN/Ethernet und Selbstheilung.
// Uebersicht der Module: siehe app.h
#include "app.h"
#if defined(ESP32)
  #include <ESPmDNS.h>
  #include <esp_timer.h>
  #include <esp_task_wdt.h>
  #include <esp_system.h>
  #include <esp_heap_caps.h>
#else
  #include <ESP8266mDNS.h>
  #include <Ticker.h>
#endif
#include <ArduinoOTA.h>
#include <WiFiManager.h>
#if defined(USE_ETHERNET)
  #include <ETH.h>
#endif
#include <time.h>

String bootReason;
bool   portalActive = false;          // Einrichtungs-WLAN offen?
String myIpStr;                       // eigene IP (fuer SIP)

// WiFiManager schaltet beim Oeffnen des Portals eine nicht verbundene Station ab
// (_disableSTAConn; in 2.0.17 ohne Setter, aber protected). Beim spaeten Portal
// soll sie weiter verbinden -> kleine Unterklasse.
class DoorWiFiManager : public WiFiManager {
 public:
  void keepStation(bool keep) { _disableSTAConn = !keep; }
};

static DoorWiFiManager wm;
static bool     servicesUp    = false;   // Webserver, SIP, MQTT ... gestartet?
static bool     otaOn         = false;
static uint32_t restartAt     = 0;       // geplanter Neustart (nach der HTTP-Antwort)
static uint8_t  restartReason = 0;
static bool     wifiResetDue  = false;
static uint32_t netDownAt     = 0;       // seit wann ohne Verbindung (millis, 0 = verbunden)
static uint32_t wifiSavedAt   = 0;       // Zugangsdaten im Portal gespeichert (millis)
static uint32_t wifiRetryAt   = 0;       // letzter eigener Verbindungsversuch (millis)
static bool     wifiCreds     = false;   // WLAN-Zugangsdaten gespeichert? (je Versuch aktualisiert)
static bool     portalTried   = false;   // spaetes Einrichtungs-WLAN in dieser Offline-Phase schon offen
static bool     pwResetDone   = false;   // Notfall-Reset beim Start -> Mitteilung, sobald das Netz steht
#if defined(USE_ETHERNET)
static bool     ethStarted    = false;   // Ethernet-Treiber gestartet?
static uint32_t ethStuckAt    = 0;       // seit wann Link, aber keine IP (millis)
#endif

#if !defined(ESP32)
static Ticker            loopWdt;     // Loop-Watchdog (ESP32: Task-Watchdog)
static volatile uint32_t lastLoopAt = 0;
#endif

// ------------------------------------------------------------
//  Neustart mit Grund (ueberlebt den Neustart im RTC-Speicher)
// ------------------------------------------------------------
static const uint32_t RR_MAGIC = 0xA5C30000;

#if defined(ESP32)
RTC_NOINIT_ATTR uint32_t rtcReason;
void writeRestartReason(uint8_t r) { rtcReason = RR_MAGIC | r; }
static uint8_t readRestartReason() {
  uint32_t v = rtcReason;
  rtcReason = 0;
  return (v & 0xFFFF0000) == RR_MAGIC ? (uint8_t)(v & 0xFF) : RR_NONE;
}
#else
static const uint32_t RTC_REASON_BLOCK = 64;   // hinter dem vom OTA genutzten Bereich
void writeRestartReason(uint8_t r) {
  uint32_t v = RR_MAGIC | r;
  ESP.rtcUserMemoryWrite(RTC_REASON_BLOCK, &v, sizeof(v));
}
static uint8_t readRestartReason() {
  uint32_t v = 0;
  ESP.rtcUserMemoryRead(RTC_REASON_BLOCK, &v, sizeof(v));
  uint32_t zero = 0;
  ESP.rtcUserMemoryWrite(RTC_REASON_BLOCK, &zero, sizeof(zero));
  return (v & 0xFFFF0000) == RR_MAGIC ? (uint8_t)(v & 0xFF) : RR_NONE;
}
#endif

// Warum ist das Geraet zuletzt gestartet? (fuer die Weboberflaeche)
static String describeBootReason() {
  switch (readRestartReason()) {
    case RR_USER:       return "Neustart über die Weboberfläche";
    case RR_UPDATE:     return "Firmware-Update";
    case RR_WIFI_LOST:  return "Selbstheilung: WLAN war weg";
    case RR_LOW_HEAP:   return "Selbstheilung: Speicher knapp";
    case RR_WIFI_RESET: return "WLAN zurückgesetzt";
    case RR_RESTORE:    return "Sicherung eingespielt";
    case RR_NETWORK:    return "Netzwerk geändert";
    case RR_LOOP_WDT:   return "Watchdog (Programm hing)";
    default: break;
  }
#if defined(ESP32)
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "Einschalten";
    case ESP_RST_EXT:      return "Reset-Taste";
    case ESP_RST_SW:       return "Software-Neustart";
    case ESP_RST_PANIC:    return "Absturz";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "Watchdog (Programm hing)";
    case ESP_RST_BROWNOUT: return "Unterspannung";
    default:               return "unbekannt";
  }
#else
  switch (ESP.getResetInfoPtr()->reason) {
    case REASON_DEFAULT_RST:      return "Einschalten";
    case REASON_EXT_SYS_RST:      return "Reset-Taste";
    case REASON_SOFT_RESTART:     return "Software-Neustart";
    case REASON_EXCEPTION_RST:    return "Absturz";
    case REASON_WDT_RST:
    case REASON_SOFT_WDT_RST:     return "Watchdog (Programm hing)";
    default:                      return "unbekannt";
  }
#endif
}

void restartNow(uint8_t reason) {
  stopBuzzer();
  saveState();
  writeRestartReason(reason);
  delay(50);
  ESP.restart();
}

void scheduleRestart(uint8_t reason, bool wifiReset) {
  restartReason = reason;
  wifiResetDue |= wifiReset;
  restartAt     = (millis() + 500) | 1;   // erst Antwort rausschicken; 0 = keiner geplant
}

void feedWatchdog() {
#if defined(ESP32)
  esp_task_wdt_reset();
#else
  lastLoopAt = millis();
  ESP.wdtFeed();
#endif
}

#if !defined(ESP32)
// Laeuft im Timer-Kontext: haengt die Loop (z.B. in einer Warteschleife mit
// delay()), greift der Hardware-Watchdog nicht -> hier neu starten
static void loopWdtCheck() {
  if (millis() - lastLoopAt > (uint32_t)LOOP_WATCHDOG_SEC * 1000UL) {
    writeRestartReason(RR_LOOP_WDT);
    ESP.reset();
  }
}
#endif

// Laufzeit in Sekunden (64 Bit, laeuft nicht nach 49 Tagen ueber)
uint32_t uptimeSeconds() {
#if defined(ESP32)
  return (uint32_t)(esp_timer_get_time() / 1000000ULL);
#else
  return (uint32_t)(micros64() / 1000000ULL);
#endif
}

// Groesster zusammenhaengender freier Block (Zerstueckelung des Speichers)
uint32_t maxFreeBlock() {
#if defined(ESP32)
  return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
#else
  return ESP.getMaxFreeBlockSize();
#endif
}

bool otaActive() {
  return otaOn;
}

// ------------------------------------------------------------
//  WLAN und Netzdienste
// ------------------------------------------------------------
// Wird aufgerufen, sobald das WLAN-Einrichtungsportal (AP) startet
static void configModeCallback(WiFiManager *) {
  portalActive = true;
  String ip = WiFi.softAPIP().toString();
  logMsg("WLAN-Einrichtung offen: %s (%s), http://%s", WIFI_AP_NAME,
         apPw.length() ? "mit Passwort" : "OFFEN", ip.c_str());
  showPortalInfo(ip.c_str());
}

// Uhrzeit per NTP (laeuft im Hintergrund; bis dahin ist timeValid() false). Ohne
// Internet hilft das Gateway (Router/TK-Anlage bieten oft NTP an), dazu ein zweiter
// Pool-Server. sntp merkt sich nur Zeiger -> statische Puffer, beim Wechsel
// abwechselnd (der alte bleibt gueltig, bis sntp umgestellt ist).
static void startNtp() {
  static char    gwBuf[2][16];
  static uint8_t gwIdx = 0;
  const char *gw = nullptr;
  IPAddress g = netGateway();
  if ((uint32_t)g != 0) {
    gwIdx ^= 1;
    strlcpy(gwBuf[gwIdx], g.toString().c_str(), sizeof(gwBuf[gwIdx]));
    gw = gwBuf[gwIdx];
  }
#if defined(ESP32)
  configTzTime(TIME_ZONE, NTP_SERVER, gw, NTP_SERVER2);
#else
  configTime(TIME_ZONE, NTP_SERVER, gw, NTP_SERVER2);
#endif
}

// Einmalig, sobald das WLAN steht (und das Einrichtungsportal zu ist)
static void startServices() {
  myIpStr = netIP().toString();
  logMsg("%s verbunden, IP %s", netIsEthernet() ? "Ethernet" : "WLAN", myIpStr.c_str());

  // Modem-Sleep aus: sonst verschluckt der ESP Unicast-Pakete (Ping/HTTP)
#if defined(USE_ETHERNET)
#elif defined(ESP32)
  WiFi.setSleep(false);
#else
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
#endif

  startNtp();

  // Name im Netz: http://tueroeffner.local
  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);

  // OTA-Update aus PlatformIO (espota) nur mit Admin-Passwort - sonst koennte
  // jeder im Netz eine fremde Firmware aufspielen. Aenderungen gelten nach Neustart.
  if (webPw.length()) {
    ArduinoOTA.setHostname(HOSTNAME);
    ArduinoOTA.setPassword(webPw.c_str());
    ArduinoOTA.onStart([]() {
      stopBuzzer();
      stopChain();
      aSip.Hangup();
      saveState();   // Zaehler und Protokoll sichern
      writeRestartReason(RR_UPDATE);
    });
    ArduinoOTA.onProgress([](unsigned int, unsigned int) { feedWatchdog(); });
#if defined(ESP32)
    ArduinoOTA.setMdnsEnabled(false);   // mDNS laeuft schon
    ArduinoOTA.begin();
#else
    ArduinoOTA.begin(false);
#endif
    otaOn = true;
  } else {
    logMsg("OTA aus: erst ein Admin-Passwort setzen");
  }

  syslogBegin();
  mqttBegin();
  webBegin();
  initSip();
  homekitBegin();   // Apple Home (falls eingeschaltet)
  servicesUp = true;
  logMsg("Tueroeffner " FW_VERSION " bereit (%s)", bootReason.c_str());
  if (pwResetDone) {
    pwResetDone = false;
    queuePush(0, F("Notfall-Reset am Gerät: Passwörter gelöscht, DHCP aktiv"));
  }
}

// Zugangsdaten wurden im Portal gespeichert (WiFiManager-Rueckruf)
static void wifiSavedCallback() {
  wifiSavedAt = millis() | 1;
  wifiCreds   = true;
  logMsg("WLAN-Zugangsdaten gespeichert - verbinde");
}

#if !defined(USE_ETHERNET)
// Station mit den gespeicherten Zugangsdaten neu verbinden
static void staReconnect() {
  logMsg("WLAN: neuer Verbindungsversuch");
#if defined(ESP32)
  WiFi.disconnect();               // trennt eine halbe Verbindung (verbunden ohne IP)
#else
  WiFi.disconnect(false, false);   // ESP8266: WiFi.disconnect() loescht die Zugangsdaten!
#endif
  WiFi.begin();
  wifiRetryAt = millis();
}

// Portal zu (Zeit abgelaufen, "Exit" oder verbunden): Station sicher wieder starten.
// WiFiManager verbindet am Ende nur bei WL_IDLE_STATUS neu, Core 3.x meldet dann
// aber WL_DISCONNECTED - die Station bliebe sonst bis zur Selbstheilung untaetig.
static void portalClosed() {
  WiFi.setAutoReconnect(true);
  if (servicesUp) server.begin();   // eigene Weboberflaeche wieder auf Port 80
  displayDirty = true;              // Hinweis zur Einrichtung vom Display nehmen
  wifiCreds = wm.getWiFiIsSaved();
  if (!netUp() && wifiCreds) staReconnect();
}

// Bekanntes WLAN zu lange weg: Einrichtungs-WLAN oeffnen (z.B. neues Router-Passwort).
// Die Station bleibt an (AP+STA); neu verbunden wird nur im Minutentakt
// (wifiKeepTrying) - staendige Suchlaeufe machten das Einrichtungs-WLAN unbrauchbar.
static void openLatePortal() {
  logMsg("WLAN seit %u min weg - Einrichtungs-WLAN wird geoeffnet", (unsigned)WIFI_PORTAL_AFTER_MIN);
  if (servicesUp) server.stop();    // Port 80 gehoert solange dem Portal
  WiFi.setAutoReconnect(false);
  wm.keepStation(true);
  wm.setConfigPortalTimeout(WIFI_PORTAL_SECONDS);
  wm.startConfigPortal(WIFI_AP_NAME, apPw.length() ? apPw.c_str() : nullptr);
  portalActive = wm.getConfigPortalActive();
  if (!portalActive) {                // Start fehlgeschlagen
    WiFi.setAutoReconnect(true);
    if (servicesUp) server.begin();
  }
}

// WLAN weg: nach WIFI_RETRY_SEC und dann im selben Takt neu verbinden. Der Core 3.x
// verbindet bei manchen Trennungsgruenden nicht selbst neu (u.a. 202 AUTH_FAIL, wenn
// der Router nach einem Stromausfall noch startet). Nicht, solange das Portal
// gerade neue Zugangsdaten verbindet (wifiSavedAt).
static void wifiKeepTrying() {
  uint32_t now = millis();
  if (wifiSavedAt || now - netDownAt < (uint32_t)WIFI_RETRY_SEC * 1000UL ||
      now - wifiRetryAt < (uint32_t)WIFI_RETRY_SEC * 1000UL) return;
  wifiRetryAt = now;
  wifiCreds   = wm.getWiFiIsSaved();
  if (!wifiCreds) return;              // Ersteinrichtung: das Portal ist schon offen
  if (!portalActive && !portalTried && now - netDownAt >= (uint32_t)WIFI_PORTAL_AFTER_MIN * 60000UL) {
    portalTried = true;
    openLatePortal();
  }
  staReconnect();
}
#endif

// Verbindung ueberwachen: Dienste starten, bei neuer IP SIP neu anmelden
static void netLoop() {
#if defined(USE_ETHERNET)
  portalActive = false;
  bool connected = netUp();
#else
  bool wasPortal = portalActive;
  portalActive = wm.getConfigPortalActive();
  bool connected = netUp();
  if (wasPortal && !portalActive) portalClosed();   // Zeit abgelaufen oder "Exit"
#endif
  if (connected) {
    netDownAt   = 0;
    portalTried = false;
  } else if (!netDownAt) {
    netDownAt = millis() | 1;
  }

#if !defined(USE_ETHERNET)
  // Verbunden, aber Portal noch offen (z.B. erster Versuch nach dem Speichern
  // scheiterte, die automatische Wiederverbindung klappte dann doch) -> schliessen
  if (portalActive && connected) {
    logMsg("WLAN verbunden - Einrichtung wird beendet");
    wm.stopConfigPortal();
    portalActive = false;
    portalClosed();
  }
#endif
  // Gespeichert, aber keine Verbindung: der Verbindungsaufbau beim Start ist
  // zuverlaessiger als aus dem laufenden Portal heraus -> neu starten
  if (wifiSavedAt && !connected && millis() - wifiSavedAt > 45000UL) {
    logMsg("WLAN nach dem Speichern nicht verbunden -> Neustart");
    restartNow(RR_NETWORK);
  }
  if (connected) wifiSavedAt = 0;
#if !defined(USE_ETHERNET)
  if (!connected) wifiKeepTrying();
#endif

  if (portalActive || !connected) return;
  if (!servicesUp) {
    startServices();
    return;
  }
  String ip = netIP().toString();
  if (ip != myIpStr) {
    logMsg("Neue IP-Adresse %s (vorher %s) -> SIP neu anmelden", ip.c_str(), myIpStr.c_str());
    myIpStr = ip;
    initSip();
    syslogBegin();
    startNtp();                // Gateway (Zeitserver) kann sich geaendert haben
    mqttDiscoveryDue = true;   // configuration_url aktualisieren
  }
}

// Selbstheilung: Netzwerk zu lange weg oder Speicher knapp -> Neustart
static void selfHeal() {
#if defined(USE_ETHERNET)
  // Nur, wenn ein Link da ist, aber keine IP kommt (oder der Treiber nicht startete).
  // Bei gezogenem Kabel hilft kein Neustart (und Relais 2 an IO5 koennte zucken).
  if (netUp() || (ethStarted && !ETH.linkUp())) {
    ethStuckAt = 0;
  } else if (!ethStuckAt) {
    ethStuckAt = millis() | 1;
  } else if (millis() - ethStuckAt > (uint32_t)ETH_NO_IP_RESTART_MIN * 60000UL) {
    logMsg("Selbstheilung: Ethernet ohne IP-Adresse -> Neustart");
    restartNow(RR_WIFI_LOST);
  }
#else
  // Letzte Stufe nach den Verbindungsversuchen und dem Einrichtungs-WLAN. Ist das
  // Portal offen, erst nach der doppelten Zeit (vielleicht traegt gerade jemand das
  // WLAN ein) - ohne gespeicherte Zugangsdaten (Ersteinrichtung) gar nicht.
  uint32_t limit = (uint32_t)WIFI_LOST_RESTART_MIN * 60000UL;
  uint32_t down  = netDownAt ? millis() - netDownAt : 0;
  if (down > limit && (!portalActive || (down > 2 * limit && wifiCreds))) {
    logMsg("Selbstheilung: Netzwerk zu lange weg -> Neustart");
    restartNow(RR_WIFI_LOST);
  }
#endif
  if ((ESP.getFreeHeap() < MIN_FREE_HEAP || maxFreeBlock() < MIN_FREE_BLOCK) &&
      !aSip.IsBusy() && !buzzerActive) {
    logMsg("Selbstheilung: Speicher knapp (%u frei, Block %u) -> Neustart",
           (unsigned)ESP.getFreeHeap(), (unsigned)maxFreeBlock());
    restartNow(RR_LOW_HEAP);
  }
}

// ------------------------------------------------------------
//  Setup / Loop
// ------------------------------------------------------------
void setup() {
  // Als Erstes die Relais in Ruhelage (vor Serial und delay): Relais 2 liegt beim
  // WT32 auf IO5 (Strapping-Pin, Pull-up beim Reset), beim ESP8266 auf GPIO16 -
  // bis hier kann es kurz anziehen (Abhilfe siehe README "Relais beim Start")
  doorBegin();
#if defined(DOOR_ON_RX_PIN)
  Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);   // RX wird zum Tuerkontakt
#else
  Serial.begin(115200);
#endif
  delay(100);
  bootReason = describeBootReason();
  Serial.printf("Start: %s\n", bootReason.c_str());

  settingsBegin();
  displayBegin();
  pushBegin();
  keypadBegin();
  logEvent(EV_BOOT, bootReason);
  pwResetDone = checkEmergencyReset();   // Mitteilung folgt, sobald das Netz steht

#if defined(USE_ETHERNET)
  // Ethernet (WT32-ETH01): kein WLAN, keine Einrichtung per Portal
  WiFi.mode(WIFI_OFF);
  ETH.setHostname(HOSTNAME);
  ethStarted = ETH.begin(ETH_PHY_LAN8720, ETH_PHY_ADDR_CFG, ETH_PHY_MDC_PIN, ETH_PHY_MDIO_PIN,
                         ETH_PHY_POWER_PIN, ETH_CLK_MODE_CFG);
  if (staticIp) ETH.config(ipAddr, ipGw, ipMask, ipDns);
  if (ethStarted) logMsg("Ethernet gestartet - warte auf Verbindung");
  else            logMsg("Ethernet startet nicht - Neustart in %d min", ETH_NO_IP_RESTART_MIN);
#else
  // WLAN: bekanntes Netz versuchen; sonst Einrichtungsportal, das NICHT blockiert -
  // Klingel, Taster und Summer funktionieren waehrenddessen weiter.
  WiFi.mode(WIFI_STA);
  wifiCreds = wm.getWiFiIsSaved();
  wm.setHostname(HOSTNAME);
  wm.setAPCallback(configModeCallback);
  wm.setSaveConfigCallback(wifiSavedCallback);
  wm.setConfigPortalBlocking(false);
  wm.setConfigPortalTimeout(wifiCreds ? WIFI_PORTAL_SECONDS : 0);
  // Mit gespeicherten Zugangsdaten kein Portal beim Start: nach einem Stromausfall
  // startet der Router oft langsamer als der ESP, und das Portal schaltet die
  // Station ab. netLoop verbindet weiter und oeffnet es erst spaet.
  wm.setEnableConfigPortal(!wifiCreds);
  wm.setConnectTimeout(20);
  // Im Portal nur WLAN-Auswahl anbieten (kein Firmware-Upload ohne Anmeldung)
  std::vector<const char *> menu = { "wifi", "exit" };
  wm.setMenu(menu);
  wm.setShowInfoUpdate(false);
  wm.setShowInfoErase(false);
  if (staticIp) wm.setSTAStaticIPConfig(ipAddr, ipGw, ipMask, ipDns);
  if (!wm.autoConnect(WIFI_AP_NAME, apPw.length() ? apPw.c_str() : nullptr)) {
    if (wifiCreds) logMsg("WLAN nicht verbunden - neuer Versuch im Hintergrund");
    else           logMsg("WLAN nicht verbunden - Einrichtung laeuft im Hintergrund");
  }
#endif

#if defined(ESP32)
  // Watchdog fuer die Loop: haengt sie laenger als LOOP_WATCHDOG_SEC -> Neustart
  esp_task_wdt_config_t wdt = { LOOP_WATCHDOG_SEC * 1000, 1, true };   // timeout, idle_core_mask, panic
  if (esp_task_wdt_reconfigure(&wdt) != ESP_OK) esp_task_wdt_init(&wdt);
  esp_task_wdt_add(nullptr);
#else
  lastLoopAt = millis();
  loopWdt.attach(5, loopWdtCheck);
#endif
}

void loop() {
  feedWatchdog();
#if !defined(USE_ETHERNET)
  wm.process();
#endif
  netLoop();
  if (servicesUp) {
    server.handleClient();
    if (otaOn) ArduinoOTA.handle();
#if !defined(ESP32)
    MDNS.update();
#endif
  }
  phoneLoop();
  doorLoop();
  keypadLoop();
  homekitLoop();
  mqttLoop();
  pushLoop();
  settingsLoop();
  selfHeal();

  // Geplanter Neustart (nach Update, Neustart-Button, WLAN-Reset, Sicherung, Netzwerk)
  if (restartAt && (int32_t)(millis() - restartAt) >= 0) {
#if !defined(USE_ETHERNET)
    if (wifiResetDue) wm.resetSettings();
#endif
    restartNow(restartReason);
  }
}

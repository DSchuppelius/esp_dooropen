#pragma once

// ============================================================
//  Pin-Belegung
// ============================================================
#if defined(ETH_BOARD_WT32)
// ---- WT32-ETH01 (ESP32 mit Ethernet, LAN8720) -----------------
// Verfuegbare Pins am Stecker: IO2 IO4 IO12 IO14 IO15 IO17 IO32 IO33 IO35 IO36 IO39.
// IO35/36/39 sind reine Eingaenge OHNE internen Pull-up.
#define USE_ETHERNET
#define ETH_PHY_ADDR_CFG   1
#define ETH_PHY_POWER_PIN  16
#define ETH_PHY_MDC_PIN    23
#define ETH_PHY_MDIO_PIN   18
#define ETH_CLK_MODE_CFG   ETH_CLOCK_GPIO0_IN

#define PIN_SDA        15   // I2C (nur falls ein OLED angeschlossen wird)
#define PIN_SCL        14
#define PIN_RELAY      4    // Relais Tuersummer (active-high)
#define RELAY_ACTIVE_LOW  false
#define PIN_SIGNAL     32   // Klingel-Kontakt gegen GND
#define SIGNAL_ACTIVE_LOW true
#define PIN_BTN_RING   33   // Klingel-Taster gegen GND
#define PIN_BTN_BUZZER 12   // Summer-Taster gegen GND (Strapping-Pin: nie extern auf 3,3 V ziehen)
#define PIN_DOOR       2    // Tuerkontakt gegen GND
#define PIN_STATUS_LED    17
#define STATUS_LED_ACTIVE_LOW false
// Zweites Relais und Tastenfeld/RFID (Wiegand) auf den reinen Eingaengen bzw. frei:
#define PIN_RELAY2     5    // RXD-Pin des Moduls (UART2), frei nutzbar
#define RELAY2_ACTIVE_LOW false
#define PIN_WG_D0      35   // Wiegand D0 (Pull-up/Pegelwandler noetig, siehe README)
#define PIN_WG_D1      36   // Wiegand D1

#elif defined(ESP32)
// ---- ESP32 DevKit (WROOM-32) --------------------------------
// Kein Onboard-OLED: I2C liegt auf den Standard-Pins (falls extern genutzt).
#define PIN_SDA        21   // GPIO21 (I2C SDA)
#define PIN_SCL        22   // GPIO22 (I2C SCL)

// Ausgang zum Relais-Modul (schaltet den Summer)
#define PIN_RELAY      26   // GPIO26
// KY-019 (Aufdruck HW-482) schaltet bei HIGH ein -> active-high.
#define RELAY_ACTIVE_LOW  false

// Eingang fuer die Signalisierung (potentialfreier Kontakt)
// Eine Ader an diesen Pin, die andere an GND.
#define PIN_SIGNAL     27   // GPIO27
// Kontakt schliesst gegen GND -> gedrueckt = LOW.
#define SIGNAL_ACTIVE_LOW true

// Taster (je eine Ader an den Pin, die andere an GND; gedrueckt = LOW)
#define PIN_BTN_RING   32   // GPIO32: Klingel-Taster (wirkt wie das Klingelsignal)
#define PIN_BTN_BUZZER 33   // GPIO33: Summer-Taster (oeffnet die Tuer)
// Tuerkontakt (Reed-Kontakt gegen GND), im Web aktivieren
#define PIN_DOOR       25   // GPIO25

// Onboard-LED (GPIO2): Summer an = Dauerlicht, Blinkmuster zeigen WLAN-/SIP-Probleme.
// Manche DevKits haben dort keine LED (nur die rote Betriebs-LED) -> externe LED
// mit 330 Ohm von GPIO2 nach GND anschliessen.
#define PIN_STATUS_LED    2       // GPIO2 (Onboard-LED, active-high)
#define STATUS_LED_ACTIVE_LOW false

// Zweites Relais (z.B. Garage, zweite Tuer), im Web aktivieren
#define PIN_RELAY2     13   // GPIO13
#define RELAY2_ACTIVE_LOW false

// Tastenfeld / RFID-Leser mit Wiegand-Schnittstelle (D0/D1), im Web aktivieren.
// Leser mit 5-V-Ausgaengen brauchen einen Pegelwandler (siehe README).
#define PIN_WG_D0      18   // GPIO18
#define PIN_WG_D1      19   // GPIO19

#else
// ---- ESP8266 HW-364A (NodeMCU mit fest verbautem OLED) -------
// I2C fuer das ONBOARD-OLED (fest verdrahtet, NICHT aenderbar!)
#define PIN_SDA        D5   // GPIO14  (Onboard-OLED)
#define PIN_SCL        D6   // GPIO12  (Onboard-OLED)

// Ausgang zum Relais-Modul (schaltet den Summer)
#define PIN_RELAY      D1   // GPIO5
// KY-019 schaltet bei HIGH ein -> active-high, daher false.
// (Blaue Standard-Relaismodule waeren true.)
#define RELAY_ACTIVE_LOW  false

// Eingang fuer die Signalisierung (potentialfreier Kontakt)
// Eine Ader an diesen Pin, die andere an GND.
#define PIN_SIGNAL     D2   // GPIO4
// Kontakt schliesst gegen GND -> gedrueckt = LOW.
#define SIGNAL_ACTIVE_LOW true

// Taster (je eine Ader an den Pin, die andere an GND; gedrueckt = LOW)
#define PIN_BTN_RING   D7   // GPIO13: Klingel-Taster (wirkt wie das Klingelsignal)
// GPIO0 ist Boot-Pin: beim Einschalten NICHT gedrueckt halten (sonst Flash-Modus)
#define PIN_BTN_BUZZER D3   // GPIO0:  Summer-Taster (oeffnet die Tuer)
// Tuerkontakt (Reed-Kontakt gegen GND) auf RX: die serielle Schnittstelle
// laeuft dann nur noch als Ausgabe. Beim Flashen per USB Kontakt offen lassen.
#define PIN_DOOR       3    // GPIO3 (RX)
#define DOOR_ON_RX_PIN

// Onboard-LED (blaue LED, GPIO2): Summer an = Dauerlicht, Blinkmuster fuer WLAN/SIP.
#define PIN_STATUS_LED    LED_BUILTIN
#define STATUS_LED_ACTIVE_LOW true

// Zweites Relais auf D0 (GPIO16). Tastenfeld/RFID (Wiegand) gibt es auf dem
// ESP8266 nicht - es sind keine Pins mehr frei.
#define PIN_RELAY2     D0   // GPIO16
#define RELAY2_ACTIVE_LOW false
#endif

// ============================================================
//  Display
// ============================================================
#define OLED_WIDTH     128
#define OLED_HEIGHT    64
// Adresse wird per I2C-Scan ermittelt (0x3C oder 0x3D); ohne Fund kein Display.

// ============================================================
//  Verhalten
// ============================================================
#define DEFAULT_BUZZER_SECONDS   3
#define MIN_BUZZER_SECONDS       1
#define MAX_BUZZER_SECONDS       30
#define SIGNAL_DEBOUNCE_MS       50
// Klingelsignal ueber Optokoppler an Wechselspannung pulst mit 50 Hz (alle 20 ms):
// so lange nach dem letzten aktiven Pegel gilt das Signal noch als aktiv (ms)
#define SIGNAL_AC_HOLD_MS        40
// Das Klingelsignal allein (ohne Taster) zaehlt erst ab so vielen aktiven Abtastungen
// im Entprellfenster - sonst reichten zwei Stoerspitzen (je SIGNAL_AC_HOLD_MS gehalten)
#define SIGNAL_MIN_SAMPLES       3
// Klingeln waehrend des Summers und so lange danach (ms) ignorieren: Einkopplung vom
// Tueroeffner auf die Klingelleitung (im Praxis-Modus oeffnete das sonst erneut)
#define RING_BUZZER_GUARD_MS     300
// Weboberflaeche: "Signal aktiv" nach dem Loslassen noch so lange zeigen (ms),
// sonst verpasst die 1-s-Abfrage kurze Tastendruecke
#define SIGNAL_HOLD_MS           3000
// Wie lange "Es klingelt" nach dem Tastendruck angezeigt bleibt (ms)
#define RING_NOTIFY_MS           10000
// Sperre gegen Sturmklingeln: weiteres Klingeln innerhalb dieser Zeit (ms) zaehlt,
// loest aber keinen neuen Anruf, kein HA-Ereignis und keinen Protokolleintrag aus
#define RING_COOLDOWN_MS         5000
#define WIFI_AP_NAME             "Tueroeffner-Setup"
// Passwort des Einrichtungs-WLANs (mind. 8 Zeichen; im Web aenderbar, leer = offen).
// Bitte je Installation aendern - dieser Wert steht oeffentlich im Quelltext.
#define WIFI_AP_PASSWORD         "tuer-einrichten"
// Wie lange das Einrichtungs-WLAN offen bleibt, wenn das bekannte WLAN fehlt (s).
// Waehrenddessen laufen Klingel, Taster und Summer normal weiter.
#define WIFI_PORTAL_SECONDS      300
// Sind Zugangsdaten gespeichert, oeffnet das Einrichtungs-WLAN erst nach so vielen
// Minuten ohne Verbindung (nach Stromausfall startet der Router oft langsamer als
// der ESP). Die Station verbindet dabei weiter (WLAN-Einrichtung und Station parallel).
#define WIFI_PORTAL_AFTER_MIN    15
// WLAN weg: so lange (s) warten, dann alle WIFI_RETRY_SEC neu verbinden (der
// ESP32-Core gibt bei manchen Trennungsgruenden auf, z.B. falsches Passwort)
#define WIFI_RETRY_SEC           60
// Name im Netz: http://tueroeffner.local (mDNS), auch fuer OTA-Updates
#define HOSTNAME                 "tueroeffner"

// ============================================================
//  Uhrzeit (NTP) fuer Protokoll und Nachtruhe
// ============================================================
#define NTP_SERVER   "pool.ntp.org"
// Ausweich-Server. Dazwischen wird das Gateway gefragt (Router/TK-Anlage bieten oft
// NTP an) - so gibt es auch ohne Internet eine gueltige Uhrzeit.
#define NTP_SERVER2  "de.pool.ntp.org"
#define TIME_ZONE    "CET-1CEST,M3.5.0,M10.5.0/3"   // Mitteleuropa mit Sommerzeit

// ============================================================
//  Zaehler dauerhaft speichern
//  Flash nicht bei jedem Ereignis beschreiben: hoechstens alle X ms.
// ============================================================
#define COUNTER_SAVE_MS  300000UL

// Ereignisprotokoll (wird mit den Zaehlern gesichert und uebersteht Neustarts)
#define LOG_SIZE         30

// Notfall: Klingel-Taster beim Einschalten so lange halten (ms)
// -> Passwoerter weg und wieder DHCP statt fester IP
#define PW_RESET_HOLD_MS 5000
// ... und danach innerhalb dieser Zeit (ms) loslassen. Bleibt er gedrueckt
// (klemmender Taster, Feuchte), gibt es keinen Reset.
#define PW_RESET_RELEASE_MS 10000

// ============================================================
//  Selbstheilung
// ============================================================
// Neustart, wenn das WLAN so lange (Minuten) weg ist. Letzte Stufe: vorher
// verbindet die Station alle WIFI_RETRY_SEC neu, nach WIFI_PORTAL_AFTER_MIN oeffnet
// das Einrichtungs-WLAN. Ist es gerade offen, erst nach der doppelten Zeit.
#define WIFI_LOST_RESTART_MIN   30
// Ethernet: Neustart nur, wenn ein Link da ist, aber so lange (Minuten) keine IP
// kommt. Bei gezogenem Kabel hilft ein Neustart nicht.
#define ETH_NO_IP_RESTART_MIN   10
// Neustart, wenn der freie Speicher darunter faellt (Bytes)
#define MIN_FREE_HEAP           8000
// ... oder der groesste zusammenhaengende Block (Zerstueckelung, v.a. ESP8266)
#define MIN_FREE_BLOCK          3000
// Watchdog, falls die Loop haengt (Sekunden). Grosszuegig wegen blockierender
// Schritte (Updates, Push auf dem ESP8266). ESP32: Task-Watchdog, ESP8266: Ticker.
#define LOOP_WATCHDOG_SEC       60

// ============================================================
//  Schutz der Weboberflaeche
// ============================================================
// Nach so vielen falschen Anmeldungen wird die Adresse gesperrt ...
#define AUTH_MAX_FAILS          5
// ... zunaechst fuer so viele Sekunden, bei weiteren Fehlern laenger (max. 15 min)
#define AUTH_LOCK_SEC           60
// Groessere Antworten (Startseite, CSV-Export) hoechstens so lange senden (ms): die
// Loop (Klingel, SIP) steht waehrenddessen. Liest die Gegenstelle nicht -> Abbruch.
#define WEB_SEND_MAX_MS         5000

// ============================================================
//  Push-Mitteilungen (ntfy / Telegram), im Web einstellbar
// ============================================================
// ESP32: Frist fuer Verbindung und Antwort (eigener Task). ESP8266: Zeitbudget je
// Mitteilung insgesamt (DNS, Verbindung, TLS, Antwort) - die Loop steht so lange.
#define PUSH_TIMEOUT_MS         5000
// ESP8266: davon hoechstens fuer die Namensaufloesung (ms)
#define PUSH_DNS_TIMEOUT_MS     2000
// ESP8266: HTTPS nur mit so viel freiem Speicher, sonst wird die Mitteilung
// uebersprungen (BearSSL: 16 KB Empfangspuffer am Stueck, 6 KB Stack, 3 KB Kontext;
// scheitert eine Reservierung, bricht der Core mit abort() ab)
#define PUSH_TLS_MIN_BLOCK      17000
#define PUSH_TLS_MIN_HEAP       27000
// ESP8266: so viele Mitteilungen warten hoechstens (sonst faellt die aelteste weg)
#define PUSH_QUEUE_LEN          3

// Wie lange ein Anruf mit Oeffnungs-Code nach dem Abheben offen bleibt (s)
#define SIP_PIN_CALL_SECONDS    30

// Syslog (UDP) fuer die Fehlersuche aus der Ferne; Server im Web einstellbar
#define SYSLOG_PORT             514

// ============================================================
//  Eingehende Anrufe und Gaestecodes
//  Wer den Tueroeffner anruft, hoert einen kurzen Ton und tippt einen Code
//  (Oeffnen-Code oder Gaestecode). Nur aktiv, wenn im Web eingeschaltet.
// ============================================================
#define INCOMING_CALL_SECONDS   30     // so lange bleibt der Anruf offen
#define INCOMING_MAX_TRIES      3      // falsche Codes je Anruf, dann auflegen
#define INCOMING_LOCK_FAILS     6      // falsche Codes insgesamt ...
#define INCOMING_LOCK_MIN       15     // ... innerhalb/fuer so viele Minuten -> Sperre
#define GUEST_CODES_MAX         5      // Anzahl Gaestecodes

// ============================================================
//  Erweiterungen
// ============================================================
// Tuerkontakt: so lange nach dem Summer-Ende muss die Tuer aufgehen, sonst
// "Tuer blieb zu" (s)
#define DOOR_PASS_WINDOW_SEC    10
// Langer Verlauf im LittleFS (CSV-Export): je Datei hoechstens so viele Bytes,
// danach wird sie zur ".old"-Datei (also insgesamt etwa das Doppelte).
// ESP32: nur 128 KB LittleFS (min_spiffs.csv) -> kleiner, Platz fuer Einstellungen
#if defined(ESP32)
#define EVENT_FILE_MAX          24000
#else
#define EVENT_FILE_MAX          48000
#endif
// So viel muss im LittleFS frei bleiben (Einstellungen, Zaehler, Sitzungen samt
// Hilfsdateien); sonst wird zuerst der aeltere Verlauf geloescht
#define FS_RESERVE_BYTES        24576
// Langen Verlauf gebuendelt schreiben: hoechstens einmal je so viele ms (Flash schonen)
#define EVENT_FLUSH_MS          60000UL
// Weitere Benutzer (zusaetzlich zu Admin und Tuer-Zugang)
#define USERS_MAX               8
// Tastenfeld: falsche Codes bis zur Sperre und deren Dauer
#define KEYPAD_LOCK_FAILS       5
#define KEYPAD_LOCK_MIN         5
// Telegram: so lange ist der "Oeffnen"-Knopf einer Klingel-Mitteilung gueltig (s)
#define TELEGRAM_BUTTON_SEC     180

// ============================================================
//  VoIP / SIP (ansitel = Asterisk)
//  Zugangsdaten werden ueber die Weboberflaeche gesetzt und im
//  EEPROM gespeichert. Diese Defaults nur fuer den Erststart /
//  nach EEPROM-Loeschung. KEINE echten Secrets hier eintragen.
// ============================================================
#define SIP_SERVER_IP    ""
#define SIP_PORT         5060
#define SIP_USER         ""
#define SIP_PW           ""
#define SIP_CALLER_NAME  "Tueroeffner"
#define SIP_MAX_DIAL_SEC 15
// Registrierungsdauer in Sekunden; es wird rechtzeitig erneuert.
// (LANCOM begrenzt auf 120s -> hier klein halten.)
#define SIP_REG_EXPIRES  120
// Wartezeit bis zum naechsten Versuch, wenn die Registrierung fehlgeschlagen ist.
#define SIP_REG_RETRY_SEC 10
// Beep-Dauer im Hoerer nach dem Abheben (Sekunden); 0 = nur klingeln, kein Audio.
#define SIP_BEEP_SECONDS 6

// ============================================================
//  Home Assistant (MQTT mit Auto-Discovery)
//  Broker wird ueber die Weboberflaeche gesetzt; leerer Server = MQTT aus.
// ============================================================
#define MQTT_SERVER            ""
#define MQTT_PORT              1883
#define MQTT_USER              ""
#define MQTT_PW                ""
#define MQTT_DISCOVERY_PREFIX  "homeassistant"
// Max. Wartezeit beim Verbindungsaufbau; die Loop steht solange (ms).
#define MQTT_CONNECT_TIMEOUT_MS 1000
// Broker-Name: so lange auf die DNS-Antwort warten (ms; blockiert die Loop nicht)
#define MQTT_DNS_TIMEOUT_MS     5000
// Pause zwischen Verbindungsversuchen: verdoppelt sich je Fehlschlag bis hierhin (ms)
#define MQTT_RETRY_MAX_MS       300000UL

// Bildschirmschoner: OLED nach X Sekunden ohne Aktivitaet ausschalten (0 = nie).
#define SCREEN_TIMEOUT_SECONDS 60
// Zielrufnummer, die beim Klingeln angerufen wird (im Web aenderbar).
#define DEFAULT_DIAL_NR  ""
#define MAX_DIAL_LEN     20     // eine Nummer
#define MAX_DIAL_LIST    64     // Rufkette, z.B. "100, 101, 0171..."

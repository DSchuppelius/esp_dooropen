#pragma once

// ============================================================
//  Pin-Belegung
// ============================================================
#if defined(ESP32)
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

// Onboard-LED (GPIO2) spiegelt den Summer-Zustand -> Test ohne Relais.
#define PIN_STATUS_LED    2       // GPIO2 (Onboard-LED, active-high)
#define STATUS_LED_ACTIVE_LOW false

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

// Onboard-LED (blaue LED, GPIO2) spiegelt den Summer-Zustand -> Test ohne Relais.
#define PIN_STATUS_LED    LED_BUILTIN
#define STATUS_LED_ACTIVE_LOW true
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
// Wie lange "Es klingelt" nach dem Tastendruck angezeigt bleibt (ms)
#define RING_NOTIFY_MS           10000
#define WIFI_AP_NAME             "Tueroeffner-Setup"

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

// Bildschirmschoner: OLED nach X Sekunden ohne Aktivitaet ausschalten (0 = nie).
#define SCREEN_TIMEOUT_SECONDS 60
// Zielrufnummer, die beim Klingeln angerufen wird (im Web aenderbar).
#define DEFAULT_DIAL_NR  ""
#define MAX_DIAL_LEN     20

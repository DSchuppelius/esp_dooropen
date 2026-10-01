# esp_dooropen

SIP-Türöffner für **ESP32 DevKit (WROOM-32)** oder **ESP8266 HW-364A** (NodeMCU mit
fest verbautem OLED). Klingelt es, ruft der ESP per SIP ein Telefon an; mit **`*`** am
Telefon, über die **Weboberfläche** oder aus **Home Assistant** wird die Tür geöffnet.

Die Zielplattform wird über die PlatformIO-Umgebung gewählt (`esp32dev` bzw.
`nodemcuv2`); der Code passt sich per Präprozessor-Weiche automatisch an.

## Funktionen

- **Klingel-Eingang** (potentialfreier Kontakt, entprellt) mit Zähler und Hinweis
  „Es klingelt!“ im Web, auf dem Display und in Home Assistant.
- **Türsummer** über ein Relais, Dauer einstellbar (1–30 s).
- **SIP-Anruf beim Klingeln** an eine einstellbare Nebenstelle (getestet mit
  Asterisk, REGISTER mit Digest-Auth, auch LANCOM-kompatibel):
  - nach dem Abheben ein Piepton im Hörer (Beep, G.711 µ-law),
  - **`*` am Telefon** (DTMF per RFC 4733 oder SIP INFO) öffnet die Tür und legt auf,
  - wird nicht abgehoben, wird der Anruf nach 15 s per `CANCEL` zurückgezogen.
- **Weboberfläche** mit hellem/dunklem Design, großem Öffnen-Knopf mit
  Countdown-Ring, Statuskacheln, Test-Anruf und Ton-Benachrichtigung im Browser.
- **Home Assistant** per MQTT mit Auto-Discovery (Button, Klingel-Ereignis,
  Sensoren, Einstellungen).
- **OLED SSD1306** (optional) zeigt IP, Signal- und Summer-Status; wird kein
  Display gefunden, läuft alles ohne.
- **WiFiManager**: Beim ersten Start öffnet der ESP das WLAN `Tueroeffner-Setup`
  zum Eintragen der Zugangsdaten.
- Alle Einstellungen werden im EEPROM gespeichert und überstehen Neustarts und
  Firmware-Updates.

## Verdrahtung

### ESP32 DevKit (WROOM-32)

Hier gelten echte GPIO-Nummern. Ein OLED ist optional.

| Funktion             | Pin      | Hinweis                                  |
|----------------------|----------|------------------------------------------|
| Relais S (Signal)    | GPIO26   | schaltet das Relais (active-high)        |
| Relais + / -         | 5V / GND | Relais-Modul aus 5 V versorgen           |
| Klingel-Kontakt      | GPIO27   | eine Ader hier, andere Ader an GND       |
| Klingel-Taster       | GPIO32   | Taster gegen GND, wirkt wie Klingeln     |
| Summer-Taster        | GPIO33   | Taster gegen GND, öffnet die Tür         |
| Status-LED (onboard) | GPIO2    | spiegelt den Summer-Zustand              |
| OLED SDA (optional)  | GPIO21   | nur falls ein Display genutzt wird       |
| OLED SCL (optional)  | GPIO22   | nur falls ein Display genutzt wird       |

### ESP8266 HW-364A

Das OLED ist auf dem HW-364A **fest verdrahtet** an D5/D6.

| Funktion            | Pin         | Hinweis                                  |
|---------------------|-------------|------------------------------------------|
| OLED SDA (onboard)  | D5 (GPIO14) | fest verbaut, nicht belegen              |
| OLED SCL (onboard)  | D6 (GPIO12) | fest verbaut, nicht belegen              |
| KY-019 S (Signal)   | D1 (GPIO5)  | schaltet das Relais (active-high)        |
| KY-019 + (VCC)      | 5V / VU     | 5 V vom USB-Pin (nicht 3V3!)             |
| KY-019 - (GND)      | G           | gemeinsame Masse                         |
| Klingel-Kontakt     | D2 (GPIO4)  | eine Ader hier, andere Ader an GND       |
| Klingel-Taster      | D7 (GPIO13) | Taster gegen GND, wirkt wie Klingeln     |
| Summer-Taster       | D3 (GPIO0)  | gegen GND; Boot-Pin, nicht beim Start    |

### Türöffner und Klingel

Der Türöffner hängt am **Relaiskontakt** (COM + NO) zusammen mit einem
**Klingeltrafo** (8–12 V AC): Trafo → COM, NO → Türöffner → zurück zum Trafo.

**Wichtig:**

- Der **Klingel-Eingang** ist für einen *potentialfreien Kontakt* ausgelegt
  (`INPUT_PULLUP`, aktiv = gegen GND). Liegt dort **Fremdspannung** an (z. B. die
  Klingelspannung selbst), muss ein **Optokoppler (z. B. PC817)** dazwischen –
  sonst wird der ESP beschädigt.
- Der **Summer** wird **nicht** vom ESP versorgt. Der ESP schaltet nur das Relais,
  der Summer hat seinen eigenen Stromkreis.
- Die Voreinstellung passt für das **KY-019** (schaltet bei HIGH). Für Relais-Module,
  die bei LOW schalten (viele blaue Module), in `src/config.h`
  `RELAY_ACTIVE_LOW` auf `true` setzen.

Alle Pins stehen in [src/config.h](src/config.h) und lassen sich dort anpassen.

## Bauen & Flashen (PlatformIO)

```powershell
pio run -e esp32dev  -t upload   # ESP32 DevKit
pio run -e nodemcuv2 -t upload   # ESP8266 HW-364A

pio device monitor -b 115200     # serielle Ausgabe
```

**ESP32-Flashen:** Manche ESP32-Boards wechseln nicht automatisch in den
Download-Modus (`Wrong boot mode detected (0x13)` bzw. `No serial data received`).
Dann manuell: **BOOT** gedrückt halten → **EN/RST** kurz drücken → **BOOT**
loslassen, danach den Upload starten. Die Umgebung `esp32dev` ist mit
`board_upload.before_reset = no_reset` dafür vorbereitet.

## Inbetriebnahme

1. Nach dem ersten Start mit dem WLAN `Tueroeffner-Setup` verbinden und das
   Heim-WLAN eintragen.
2. Die IP-Adresse erscheint auf dem Display bzw. in der seriellen Ausgabe.
3. Im Browser die IP öffnen und unter **Einstellungen** eintragen:
   - **SIP-Zugang**: Server, Port, Benutzer, Passwort (eine eigene Nebenstelle für
     den Türöffner in der Telefonanlage anlegen),
   - **Anruf beim Klingeln**: Ziel-Nebenstelle und Schalter „Beim Klingeln anrufen“,
   - **Türsummer**: Dauer in Sekunden.
4. Mit **Test-Anruf** prüfen: Telefon klingelt → abheben → Piepton → `*` drücken →
   Summer schaltet, Anruf endet.

## Weboberfläche

| Tab                | Inhalt                                                          |
|--------------------|-----------------------------------------------------------------|
| **Steuerung**      | Öffnen-Knopf mit Countdown, Klingel-Hinweis, Status, Test-Anruf |
| **Einstellungen**  | Summer-Dauer, Anruf-Ziel, SIP-Zugang                            |
| **Home Assistant** | MQTT-Broker, Verbindungsstatus, Geräte-ID und Basis-Topic       |

Ein Druck auf „Öffnen“ beendet auch einen gerade laufenden Anruf. Nach einem Klick
auf die Seite spielt der Browser beim Klingeln einen Ton ab.

### HTTP-API

| Methode | Pfad           | Parameter                          | Funktion                  |
|---------|----------------|------------------------------------|---------------------------|
| GET     | `/status`      | –                                  | Zustand als JSON          |
| POST    | `/open`        | –                                  | Tür öffnen                |
| POST    | `/call`        | –                                  | Test-Anruf                |
| POST    | `/setduration` | `s`                                | Summer-Dauer (Sekunden)   |
| POST    | `/setdial`     | `nr`, `auto` (`0`/`1`)             | Anruf-Ziel                |
| POST    | `/setsip`      | `server`, `port`, `user`, `pw`     | SIP-Zugang                |
| POST    | `/setmqtt`     | `server`, `port`, `user`, `pw`     | MQTT-Broker               |

Beispiel: `curl -X POST http://<ip>/open`

> Die Weboberfläche hat **keine Anmeldung**. Den ESP nur in einem vertrauenswürdigen
> Netz betreiben (kein Port-Forwarding ins Internet).

## Home Assistant

Voraussetzung: das Add-on **Mosquitto broker** und die **MQTT-Integration**.

1. In der Weboberfläche im Tab **Home Assistant** Broker (IP von Home Assistant),
   Port `1883` sowie Benutzer und Passwort eintragen → „Speichern & verbinden“.
2. Das Gerät **Türöffner** erscheint automatisch unter
   *Einstellungen → Geräte & Dienste → MQTT*.

| Entität                       | Typ         | Funktion                                         |
|-------------------------------|-------------|--------------------------------------------------|
| Tür öffnen                    | Button      | Summer auslösen (beendet laufenden Anruf)        |
| Klingel                       | Event       | Ereignis `ring` bei jedem Klingeln (Türklingel)  |
| Es klingelt                   | Binärsensor | an, solange der Klingel-Hinweis aktiv ist (10 s) |
| Summer                        | Binärsensor | an, solange der Summer schaltet                  |
| Beim Klingeln anrufen         | Schalter    | Anruf beim Klingeln ein/aus (Konfiguration)      |
| Summer-Dauer                  | Zahl        | 1–30 s (Konfiguration)                           |
| SIP registriert               | Binärsensor | Verbindung zur Telefonanlage (Diagnose)          |
| Klingeln / Öffnungen / Anrufe | Sensor      | Zähler seit dem letzten Neustart (Diagnose)      |

Fällt der ESP aus, werden alle Entitäten als *nicht verfügbar* angezeigt.
Nach einem Neustart von Home Assistant meldet sich der ESP automatisch neu an.

Beispiel-Automation (Push mit Öffnen-Knopf):

```yaml
automation:
  - alias: Klingel Push
    trigger:
      - platform: state
        entity_id: event.turoffner_klingel
    action:
      - service: notify.mobile_app_dein_handy
        data:
          message: "Es klingelt an der Tür"
          data:
            actions:
              - action: TUER_AUF
                title: "Öffnen"

  - alias: Tür per Push öffnen
    trigger:
      - platform: event
        event_type: mobile_app_notification_action
        event_data:
          action: TUER_AUF
    action:
      - service: button.press
        target:
          entity_id: button.turoffner_tur_offnen
```

Die genauen Entity-IDs zeigt Home Assistant beim Gerät an.

## Anpassen

Grundeinstellungen in [src/config.h](src/config.h), u. a.:

| Einstellung              | Standard        | Bedeutung                                          |
|--------------------------|-----------------|----------------------------------------------------|
| `SIP_MAX_DIAL_SEC`       | 15              | Anruf wird nach dieser Zeit beendet                |
| `SIP_BEEP_SECONDS`       | 6               | Dauer des Pieptons nach dem Abheben (0 = kein Ton) |
| `SIP_REG_EXPIRES`        | 120             | Gültigkeit der SIP-Registrierung (s)               |
| `RING_NOTIFY_MS`         | 10000           | Anzeigedauer „Es klingelt!“                        |
| `SCREEN_TIMEOUT_SECONDS` | 60              | OLED-Bildschirmschoner (0 = aus)                   |
| `MQTT_DISCOVERY_PREFIX`  | `homeassistant` | Discovery-Prefix von Home Assistant                |

**Fehlersuche SIP:** In [platformio.ini](platformio.ini) `build_flags = -DDEBUGLOG`
einkommentieren – dann werden alle SIP-Pakete seriell ausgegeben.

## Lizenz

Dieses Projekt steht unter der [Apache License 2.0](LICENSE).
Die mitgelieferte Bibliothek [lib/ArduinoSIP](lib/ArduinoSIP) stammt von
Juergen Liegner und Thorsten Godau, wurde für dieses Projekt erweitert und steht
weiterhin unter der BSD-3-Clause-Lizenz (siehe [NOTICE](NOTICE) und den Kopf von
`ArduinoSIP.h`).

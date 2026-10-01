# esp_dooropen

SIP-Türöffner für **ESP32 DevKit (WROOM-32)** oder **ESP8266 HW-364A** (NodeMCU mit
fest verbautem OLED). Klingelt es, ruft der ESP per SIP ein Telefon an; mit **`*`** (oder einem Code) am
Telefon, über die **Weboberfläche**, per **Taster** oder aus **Home Assistant** wird die
Tür geöffnet.

Die Zielplattform wird über die PlatformIO-Umgebung gewählt (`esp32dev` bzw.
`nodemcuv2`); der Code passt sich per Präprozessor-Weiche automatisch an.

## Funktionen

- **Klingel-Eingang** (potentialfreier Kontakt, entprellt) mit Zähler und Hinweis
  „Es klingelt!“ im Web, auf dem Display und in Home Assistant.
- **Türsummer** über ein Relais, Dauer einstellbar (1–30 s).
- **SIP-Anruf beim Klingeln** (getestet mit Asterisk, REGISTER mit Digest-Auth, auch
  LANCOM-kompatibel):
  - **Rufkette**: mehrere Nummern nacheinander (z. B. `100, 101`), bis jemand abhebt,
  - nach dem Abheben ein Piepton im Hörer (Beep, G.711 µ-law),
  - **`*` am Telefon** (DTMF per RFC 4733 oder SIP INFO) öffnet die Tür und legt auf –
    oder ein **Öffnen-Code** (z. B. `1234`), dann bleibt das Gespräch 30 s offen,
  - wird nicht abgehoben, wird der Anruf nach 15 s per `CANCEL` zurückgezogen,
  - **SIP-Status im Klartext** („Anlage antwortet nicht“, „Passwort falsch“, …) und
    Ergebnis des letzten Anrufs (angenommen, besetzt, nicht angenommen, …).
- **Weboberfläche** mit hellem/dunklem Design, großem Öffnen-Knopf mit
  Countdown-Ring, Statuskacheln, Test-Anruf und Ton-Benachrichtigung im Browser.
- **Home Assistant** per MQTT mit Auto-Discovery (Button, Klingel-Ereignis,
  Sensoren, Einstellungen).
- **OLED SSD1306** (optional) zeigt IP, Signal- und Summer-Status; wird kein
  Display gefunden, läuft alles ohne.
- **Taster** vor Ort: Klingel-Taster (wirkt wie Klingeln) und Summer-Taster (öffnet).
- **Zwei Zugänge**: Admin (alles) und Bedien-Zugang (nur öffnen und Verlauf sehen,
  z. B. für den Kunden). Ohne Admin-Passwort ist die Oberfläche offen.
- **Firmware-Update über WLAN**: per Browser-Upload oder direkt aus PlatformIO (OTA).
- **Erreichbar unter `http://tueroeffner.local`** (mDNS).
- **Verlauf** der letzten 20 Ereignisse mit Uhrzeit (NTP): Klingeln, Anrufe und
  Öffnungen mit Quelle (Web, Taster, Telefon, Home Assistant).
- **Nachtruhe**: in einem Zeitfenster beim Klingeln nicht anrufen.
- **Praxis-Modus**: an gewählten Wochentagen in einem Zeitfenster öffnet Klingeln
  die Tür automatisch (ohne Anruf).
- **Türkontakt** (Reed): Tür offen/zu im Web und in Home Assistant, Meldung, wenn die
  Tür zu lange offen steht.
- **Push-Mitteilungen ohne Home Assistant** über **ntfy** oder **Telegram**
  (Klingeln, Öffnen, Tür zu lange offen).
- **Selbstheilung**: Neustart, wenn das WLAN 10 min weg ist, der Speicher knapp wird
  oder das Programm hängt (Watchdog); der Grund des letzten Starts wird angezeigt.
- **Sicherung**: alle Einstellungen als Datei herunterladen und wieder einspielen.
- **Feste IP-Adresse** wahlweise statt DHCP.
- **Sperre gegen Sturmklingeln**: Klingeln innerhalb von 5 s löst keinen neuen
  Anruf aus.
- **WiFiManager**: Beim ersten Start öffnet der ESP das WLAN `Tueroeffner-Setup`
  zum Eintragen der Zugangsdaten; im Web lässt sich das WLAN zurücksetzen.
- Alle Einstellungen und die Zähler werden im EEPROM gespeichert und überstehen
  Neustarts und Firmware-Updates (Zähler werden höchstens alle 5 min geschrieben,
  um den Flash zu schonen).

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
| Türkontakt (optional)| GPIO25   | Reed-Kontakt gegen GND, geschlossen = zu |
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
| Türkontakt (opt.)   | RX (GPIO3)  | Reed gegen GND; beim USB-Flashen offen   |

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

**ESP32-Plattform:** `esp32dev` ist fest auf die offizielle
[pioarduino](https://github.com/pioarduino/platform-espressif32)-Plattform
(Arduino-Core 3.1.3) eingestellt. Die gleichnamige Tasmota-Variante enthält kein TLS,
das für Push-Mitteilungen (HTTPS) nötig ist. Die Flash-Aufteilung `min_spiffs.csv`
schafft Platz für die größere Firmware (2× 1,9 MB für OTA). **Wer von Version 1.2
kommt, muss einmal per USB flashen** – eine neue Flash-Aufteilung lässt sich nicht
per OTA übertragen. Die Einstellungen bleiben dabei erhalten.

### Update über WLAN

Ab Version 1.2 ist kein USB-Kabel mehr nötig:

- **Browser:** Tab **System → Firmware-Update**, Datei
  `.pio/build/esp32dev/firmware.bin` (bzw. `nodemcuv2`) auswählen, hochladen.
- **PlatformIO:**

  ```powershell
  pio run -e esp32dev_ota  -t upload   # ESP32
  pio run -e nodemcuv2_ota -t upload   # ESP8266
  ```

  Ist ein Web-Passwort gesetzt, in [platformio.ini](platformio.ini) bei der
  OTA-Umgebung `upload_flags = --auth=PASSWORT` einkommentieren.

## Inbetriebnahme

1. Nach dem ersten Start mit dem WLAN `Tueroeffner-Setup` verbinden und das
   Heim-WLAN eintragen.
2. Die IP-Adresse erscheint auf dem Display bzw. in der seriellen Ausgabe.
3. Im Browser die IP öffnen und unter **Einstellungen** eintragen:
   - **SIP-Zugang**: Server, Port, Benutzer, Passwort (eine eigene Nebenstelle für
     den Türöffner in der Telefonanlage anlegen),
   - **Türsummer & Anruf**: Dauer, Rufkette (z. B. `100, 101`), optional ein
     Öffnen-Code, Schalter „Beim Klingeln anrufen“.
4. Mit **Test-Anruf** prüfen: Telefon klingelt → abheben → Piepton → `*` (bzw. Code)
   drücken → Summer schaltet, Anruf endet.
5. Unter **System** ein Admin-Passwort setzen, bei Bedarf einen Bedien-Zugang für
   den Kunden anlegen und eine **Sicherung** herunterladen.

## Weboberfläche

| Tab                | Inhalt                                                          |
|--------------------|-----------------------------------------------------------------|
| **Tür**            | Öffnen-Knopf, Status (SIP, Tür, Modus, letzter Anruf), Verlauf  |
| **Einstellungen**  | Summer, Rufkette, Code, Nachtruhe, Praxis-Modus, Türkontakt, SIP |
| **Dienste**        | Push-Mitteilungen (ntfy/Telegram), Home Assistant (MQTT)        |
| **System**         | Geräte-Info, Netzwerk, Zugänge, Sicherung, Firmware-Update      |

Mit dem **Bedien-Zugang** angemeldet sieht man nur den Tab **Tür**.

Ein Druck auf „Öffnen“ beendet auch einen gerade laufenden Anruf. Nach einem Klick
auf die Seite spielt der Browser beim Klingeln einen Ton ab.

### HTTP-API

| Methode | Pfad           | Parameter                          | Funktion                  |
|---------|----------------|------------------------------------|---------------------------|
| GET     | `/status`      | –                                  | Zustand als JSON          |
| POST    | `/open`        | –                                  | Tür öffnen                |
| POST    | `/call`        | –                                  | Test-Anruf                |
| GET     | `/log`         | –                                  | Verlauf als JSON          |
| POST    | `/setduration` | `s`                                | Summer-Dauer (Sekunden)   |
| POST    | `/setdial`     | `nr`, `pin`, `auto`, `s`           | Rufkette, Code, Dauer     |
| POST    | `/setsip`      | `server`, `port`, `user`, `pw`     | SIP-Zugang                |
| POST    | `/setmqtt`     | `server`, `port`, `user`, `pw`     | MQTT-Broker               |
| POST    | `/setquiet`    | `on`, `from`, `to` (Minuten)       | Nachtruhe                 |
| POST    | `/setpraxis`   | `on`, `days` (Bits Mo–So), `from`, `to` | Praxis-Modus         |
| POST    | `/setdoor`     | `on`, `inv`, `alert` (Minuten)     | Türkontakt                |
| POST    | `/setpush`     | `type`, `server`, `topic`, `token`, `ev` | Push-Mitteilungen   |
| POST    | `/testpush`    | –                                  | Test-Mitteilung           |
| POST    | `/setweb`      | `user`, `pw` oder `off=1`          | Admin-Zugang              |
| POST    | `/setop`       | `user`, `pw` oder `off=1`          | Bedien-Zugang             |
| POST    | `/setnet`      | `static`, `ip`, `mask`, `gw`, `dns` | Netzwerk (Neustart)      |
| POST    | `/resetcounters` | –                                | Zähler auf 0              |
| GET     | `/backup`      | –                                  | Sicherung herunterladen   |
| POST    | `/restore`     | Felder der Sicherung (Formular)    | Sicherung einspielen      |
| POST    | `/restart`     | –                                  | Neustart                  |
| POST    | `/wifireset`   | –                                  | WLAN-Daten löschen        |
| POST    | `/update`      | Datei (multipart)                  | Firmware-Update           |

Der Bedien-Zugang darf nur `/`, `/status`, `/log`, `/open` und `/call`.

Beispiel: `curl -X POST -u admin:PASSWORT http://tueroeffner.local/open`

> **Passwortschutz:** Unter **System → Admin-Zugang** lässt sich ein Passwort setzen
> (HTTP-Basic-Auth). Ohne Passwort ist die Oberfläche offen. Die Übertragung ist
> unverschlüsselt – den ESP nur im eigenen Netz betreiben (kein Port-Forwarding).
> **Notfall-Reset:** Klingel-Taster beim Einschalten 5 s gedrückt halten – dann sind
> beide Passwörter gelöscht und das Gerät nutzt wieder DHCP statt einer festen IP.

## Push-Mitteilungen

Unter **Dienste → Push-Mitteilungen**:

- **ntfy**: App „ntfy“ installieren, ein schwer erratbares Topic abonnieren (z. B.
  `tuer-k7x2p9`) und dasselbe Topic eintragen. Eigener ntfy-Server und Zugangs-Token
  sind möglich.
- **Telegram**: Bot über `@BotFather` anlegen, Bot-Token und Chat-ID eintragen.

Wählbar sind Meldungen bei Klingeln, Öffnen und „Tür zu lange offen“. Auf dem ESP32
laufen sie in einem eigenen Task und prüfen das Server-Zertifikat. Auf dem ESP8266
werden sie vor dem Anruf gesendet (verzögert den Anruf um 1–2 s) und ohne
Zertifikatsprüfung.

## Home Assistant

Voraussetzung: das Add-on **Mosquitto broker** und die **MQTT-Integration**.

1. In der Weboberfläche im Tab **Dienste** Broker (IP von Home Assistant),
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
| Nachtruhe                     | Schalter    | Nachtruhe ein/aus (Konfiguration)                |
| Praxis-Modus                  | Schalter    | Praxis-Modus ein/aus (Konfiguration)             |
| Tür / Tür zu lange offen      | Binärsensor | nur wenn der Türkontakt aktiviert ist            |
| Summer-Dauer                  | Zahl        | 1–30 s (Konfiguration)                           |
| SIP registriert               | Binärsensor | Verbindung zur Telefonanlage (Diagnose)          |
| Klingeln / Öffnungen / Anrufe | Sensor      | Zähler, bleiben über Neustarts erhalten          |

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
| `RING_COOLDOWN_MS`       | 5000            | Sperre gegen Sturmklingeln                         |
| `SIP_PIN_CALL_SECONDS`   | 30              | Gesprächsdauer mit Öffnen-Code                     |
| `WIFI_LOST_RESTART_MIN`  | 10              | Neustart, wenn das WLAN so lange weg ist           |
| `LOOP_WATCHDOG_SEC`      | 60              | ESP32-Watchdog, falls das Programm hängt           |
| `HOSTNAME`               | `tueroeffner`   | Name im Netz (`http://tueroeffner.local`)          |
| `TIME_ZONE`              | MEZ/MESZ        | Zeitzone für Verlauf und Nachtruhe                 |
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

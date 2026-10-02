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
- **Türsummer** über ein Relais, Dauer einstellbar (1–30 s). Ein Hardware-Timer
  schaltet das Relais auch dann pünktlich ab, wenn das Programm gerade hängt.
- **SIP-Anruf beim Klingeln** (getestet mit Asterisk, REGISTER mit Digest-Auth
  inkl. `qop=auth`/`opaque`, auch LANCOM-kompatibel):
  - **Rufkette**: mehrere Nummern nacheinander (z. B. `100, 101`), bis jemand abhebt,
  - nach dem Abheben ein Piepton im Hörer (G.711 A-law/µ-law),
  - **`*` am Telefon** (DTMF per RFC 4733 oder SIP INFO) öffnet die Tür und legt auf –
    oder ein **Öffnen-Code** (z. B. `1234`), dann bleibt das Gespräch 30 s offen,
  - wird nicht abgehoben, wird der Anruf nach 15 s per `CANCEL` zurückgezogen,
  - **SIP-Status im Klartext** und Ergebnis des letzten Anrufs.
- **Anrufe an den Türöffner** (optional): Wer die Nebenstelle des Türöffners anruft,
  hört einen kurzen Ton und tippt den Öffnen-Code oder einen **Gästecode**
  (bis zu 5, mit Name, Ablaufdatum und „einmalig“). Nach mehreren falschen Codes
  werden Anrufe für 15 min gesperrt.
- **Weboberfläche** mit hellem/dunklem Design, großem Öffnen-Knopf mit
  Countdown-Ring, Statuskacheln, Test-Anruf und Ton-Benachrichtigung im Browser.
- **Home Assistant** per MQTT mit Auto-Discovery (Schloss, Button, Klingel-Ereignis,
  Sensoren, letztes Ereignis, Einstellungen).
- **OLED SSD1306** (optional) zeigt IP, Signal- und Summer-Status; wird kein
  Display gefunden, läuft alles ohne.
- **Status-LED** mit Blinkmustern (Summer, WLAN-Einrichtung, kein WLAN, SIP fehlt).
- **Taster** vor Ort: Klingel-Taster (wirkt wie Klingeln) und Summer-Taster (öffnet).
- **Zwei Zugänge** mit eigener Anmeldeseite: Admin (alles) und Tür-Zugang (nur öffnen und
  Verlauf sehen, z. B. für den Kunden; ohne Tür-Passwort ist die Tür offen). Ohne
  Admin-Passwort warnt die Oberfläche deutlich.
- **Firmware-Update über WLAN**: per Browser-Upload oder aus PlatformIO (OTA, nur
  mit Admin-Passwort).
- **Erreichbar unter `http://tueroeffner.local`** (mDNS).
- **Verlauf** der letzten 30 Ereignisse mit Uhrzeit (NTP) und Detail (z. B. Name des
  Gästecodes, Nummer des Anrufers); **übersteht Neustarts**.
- **Nachtruhe**: in einem Zeitfenster beim Klingeln nicht anrufen.
- **Praxis-Modus**: an gewählten Wochentagen in **zwei Zeitfenstern** öffnet Klingeln
  die Tür automatisch (ohne Anruf); **Ausnahmetage** (Feiertage, Urlaub) einstellbar.
- **Türkontakt** (Reed): Tür offen/zu im Web und in Home Assistant, Meldung, wenn die
  Tür zu lange offen steht.
- **Push-Mitteilungen ohne Home Assistant** über **ntfy** oder **Telegram**
  (Klingeln, Öffnen, Tür zu lange offen, Sperre nach falschen Codes).
- **Syslog**: Meldungen und Ereignisse an einen Syslog-Server (Fehlersuche aus der Ferne).
- **Selbstheilung**: Neustart, wenn das WLAN 10 min weg ist, der Speicher knapp oder
  zerstückelt ist oder das Programm hängt (Watchdog auf beiden Boards); der Grund des
  letzten Starts wird angezeigt.
- **Neue IP per DHCP** wird erkannt, SIP meldet sich dann sofort neu an.
- **Sicherung**: alle Einstellungen als Datei herunterladen und wieder einspielen.
- **Feste IP-Adresse** wahlweise statt DHCP.
- **Sperre gegen Sturmklingeln**: Klingeln innerhalb von 5 s löst keinen neuen
  Anruf aus.
- **WiFiManager**: Fehlt das WLAN, öffnet der ESP das passwortgeschützte WLAN
  `Tueroeffner-Setup` zum Eintragen der Zugangsdaten – **ohne zu blockieren**: Klingel,
  Taster und Summer funktionieren währenddessen weiter.
- Einstellungen, Zähler und Verlauf liegen im **LittleFS** (Zähler/Verlauf werden
  höchstens alle 5 min geschrieben, um den Flash zu schonen).

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
| Status-LED           | GPIO2    | onboard oder extern, siehe [Status-LED](#status-led) |
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

- Der **Klingel-Eingang** verträgt nur einen *potentialfreien Kontakt* oder einen
  Optokoppler-Ausgang – **niemals die Klingelspannung direkt** (siehe
  [Klingel-Eingang anschließen](#klingel-eingang-anschließen)).
- Der **Summer** wird **nicht** vom ESP versorgt. Der ESP schaltet nur das Relais,
  der Summer hat seinen eigenen Stromkreis.
- Die Voreinstellung passt für das **KY-019** (schaltet bei HIGH). Für Relais-Module,
  die bei LOW schalten (viele blaue Module), in `src/config.h`
  `RELAY_ACTIVE_LOW` auf `true` setzen.

- **Zusätzliche Absicherung (empfohlen):** Das Relais wird per Hardware-Timer
  abgeschaltet, auch wenn das Programm hängt. Gegen einen *Defekt* des ESP (Pin bleibt
  auf HIGH) hilft das nicht. Wer das ausschließen will, schaltet den Türöffner über ein
  **Zeitrelais/Monoflop** (z. B. NE555-Schaltung oder fertiges „Delay Off“-Modul mit
  max. 30 s), das der ESP nur anstößt.

### Klingel-Eingang anschließen

Der Klingel-Eingang (ESP32: **GPIO27**, ESP8266: **D2**) arbeitet mit internem Pull-up:
**aktiv = gegen GND gezogen**. Erkannt werden sowohl Dauersignale als auch mit 50 Hz
pulsierende Signale (Optokoppler an Wechselspannung) – das Signal gilt als aktiv,
solange es in den letzten 40 ms aktiv war (`SIGNAL_AC_HOLD_MS`).

Welche Variante passt?

| Was ist vorhanden? | Variante |
| --- | --- |
| freier Schaltkontakt (Relaisausgang einer Sprechanlage, zweiter Kontakt am Taster) | **A** – direkt |
| klassische Klingel mit Klingeltrafo (8–12 V~) und Gong | **B** – Optokoppler parallel zum Gong |
| Klingeltaster an einer **TK-Anlage** (Sensor-Eingang, z. B. Aastra/Mitel OpenCom) | **C** – Optokoppler parallel zum Sensor |
| Klingel mit Gleichspannung (z. B. 12 V⎓) | **D** – Optokoppler, Polung beachten |
| Bus-Sprechanlage (2-Draht: Siedle, Ritto, Busch-Jaeger, TCS …) | **E** – über deren Schaltaktor |

**Vorher messen:** Messgerät auf **V~** (bei D auf V⎓), an die beiden Adern halten und
klingeln lassen. In Ruhe ~0 V, beim Klingeln die Klingelspannung → Variante B/C/D.
Liegt schon *ohne* Klingeln eine Spannung an (z. B. 15–30 V⎓) und ändert sich beim
Drücken nur wenig, ist es vermutlich ein Bus (E). Auf Gleichspannung gemessen zeigt
eine Wechselspannung nur wackelige Werte nahe 0 V.

#### A) Potentialfreier Kontakt

Der Kontakt hat keine eigene Spannung (nur „zu“ oder „offen“) – direkt anschließen:

```text
Kontakt ──── GPIO27 (ESP8266: D2)
Kontakt ──── GND
```

Keine weiteren Bauteile. Gilt genauso für den Klingel-Taster am Gerät selbst
(GPIO32 / D7).

#### B) Klingeltrafo und Gong (Wechselspannung)

Der Optokoppler kommt **parallel zum Gong** (an dessen zwei Klemmen): Nur beim
Klingeln liegt dort Spannung an. Der Gong klingelt weiter wie bisher.

```text
Gong-Klemme 1 ──[ Vorwiderstand ]──┬──── IN+  (Optokoppler-Modul)
                                   │
                                1N4148   (Strich/Kathode zu IN+)
                                   │
Gong-Klemme 2 ─────────────────────┴──── IN−
```

- **Zusätzlicher Vorwiderstand** ist nötig, auch wenn das Modul schon einen hat (der
  ist meist für 3–5 V⎓ ausgelegt, z. B. 330 Ω, Aufdruck `331`). Bei Wechselspannung
  zählt die Spitze (≈ 1,4 × Nennwert).
- **Diode 1N4148 antiparallel** ist Pflicht: In der negativen Halbwelle läge sonst die
  volle Spannung rückwärts an der LED im Optokoppler (PC817 verträgt nur 6 V).

| Klingelspannung | Spitze | zusätzlicher Vorwiderstand | LED-Strom (mit 330 Ω auf dem Modul) |
| --- | --- | --- | --- |
| 6–8 V~  | 8–11 V  | 680 Ω – 1 kΩ  | ~6–10 mA |
| 12 V~   | 17 V    | **1 kΩ**      | ~12 mA |
| 24 V~   | 34 V    | **2,2 kΩ**    | ~14 mA |

¼-W-Widerstände reichen. **1 kΩ deckt 6–12 V~ sicher ab.** Nicht parallel zum
*Taster* anschließen: Dort liegt die Spannung in Ruhe an, beim Drücken nicht – das
wird nicht unterstützt und belastet den Gong dauerhaft.

#### C) Sensor-Eingang einer TK-Anlage (z. B. OpenCom 130/131/150 mit TFE-Karte)

Hier schaltet der Klingeltaster die Wechselspannung eines Klingeltrafos auf den
Sensor-Eingang der Anlage (OpenCom M100-TFE/TFE-2: Sensor für 6–24 V~). Die Anlage ruft
daraufhin die eingestellten Telefone an.

Den Optokoppler **parallel zum Sensor-Eingang** anschließen – an die beiden Adern, die
auf die Sensor-Klemme der Anlage gehen (bei RJ45-Verkabelung das entsprechende
Aderpaar). Beschaltung wie bei **B** (Vorwiderstand + 1N4148). Die Anlage merkt davon
nichts, der Optokoppler zieht nur ~10 mA aus dem Klingeltrafo.

Gegenprobe auf **V~** an diesen Adern: in Ruhe ~0 V, beim Klingeln 6–24 V~.

#### D) Klingel mit Gleichspannung

Wie **B**, aber auf die Polung achten: Plus über den Vorwiderstand an `IN+`, Minus an
`IN−`. Die 1N4148 schützt bei vertauschter Polung (dann wird nur nichts erkannt).
Vorwiderstand nach der Gleichspannung (ohne Faktor 1,4): 12 V⎓ → 680 Ω – 1 kΩ,
24 V⎓ → 2,2 kΩ.

#### E) Bus-Sprechanlage

Die Adern führen Versorgung und Daten gemeinsam – ein Optokoppler erkennt dort das
Klingeln nicht zuverlässig und kann den Bus stören. Stattdessen den **Schaltaktor /
Relaisausgang** der Anlage (bei den meisten Herstellern als Zusatzmodul, Funktion
„Etagenruf/Türruf schaltet Kontakt“) als potentialfreien Kontakt nutzen → Variante **A**.

#### Ausgangsseite des Optokoppler-Moduls (B, C, D)

| Modul | ESP32 | ESP8266 (HW-364A) |
| --- | --- | --- |
| VCC | **3V3** (nicht 5 V!) | 3V3 |
| GND | GND | G |
| OUT | GPIO27 | D2 (GPIO4) |

**VCC immer an 3,3 V:** Der Ausgang wird auf dem Modul meist auf VCC hochgezogen – an
5 V lägen 5 V am ESP-Eingang (verträgt nur 3,3 V). Hat das Modul ausgangsseitig nur
zwei Anschlüsse (Kollektor/Emitter): Kollektor an GPIO27, Emitter an GND; den Pull-up
hat der ESP intern.

Logik: Klingeln → Optokoppler leitet → `OUT` = LOW → passt zu `SIGNAL_ACTIVE_LOW true`
in [src/config.h](src/config.h) (Voreinstellung, nichts ändern).

**Test:** In der Weboberfläche zeigt die Kachel *Signal* beim Klingeln „aktiv“, darüber
erscheint „Es klingelt!“ und im Verlauf „🔔 Klingeln“.

### Status-LED

| Muster                         | Bedeutung                                |
|--------------------------------|------------------------------------------|
| dauerhaft an                   | Summer schaltet                          |
| schnelles Blinken              | WLAN-Einrichtung (`Tueroeffner-Setup`) offen |
| langsames Blinken              | kein WLAN                                |
| kurzer Blitz alle 2 s          | SIP nicht angemeldet                     |
| aus                            | alles in Ordnung                         |

**Keine Onboard-LED?** Nicht jedes ESP32-DevKit hat eine schaltbare LED an GPIO2
(meist blau). Die **rote LED** ist bei vielen Boards nur die Betriebs-LED – sie hängt
fest an 3,3 V, leuchtet immer und lässt sich nicht ansteuern (flackert höchstens kurz,
wenn das Relais anzieht). Dann einfach eine **externe LED** anschließen:

```text
GPIO2 ──[ 330 Ω ]──►|── GND      (langes Bein = Anode zu GPIO2)
```

Die Firmware muss dafür nicht geändert werden. Soll die LED an einem anderen Pin
hängen, `PIN_STATUS_LED` (und ggf. `STATUS_LED_ACTIVE_LOW`) in
[src/config.h](src/config.h) anpassen. Ohne LED funktioniert alles genauso – den
Zustand zeigen dann Weboberfläche und Home Assistant.

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

**Weboberfläche:** Die Seite liegt in [src/web/index.html](src/web/index.html). Beim
Bauen bettet [tools/embed_html.py](tools/embed_html.py) sie gzip-komprimiert als
`include/index_html.h` ein (automatisch, nicht einchecken).

**ESP32-Plattform:** `esp32dev` ist fest auf die offizielle
[pioarduino](https://github.com/pioarduino/platform-espressif32)-Plattform
(Arduino-Core 3.1.3) eingestellt. Die gleichnamige Tasmota-Variante enthält kein TLS,
das für Push-Mitteilungen (HTTPS) nötig ist. Die Flash-Aufteilung `min_spiffs.csv`
schafft Platz für die größere Firmware (2× 1,9 MB für OTA) und enthält 128 KB für
das LittleFS. **Wer von Version 1.2 kommt, muss einmal per USB flashen** – eine neue
Flash-Aufteilung lässt sich nicht per OTA übertragen. Die Einstellungen bleiben dabei
erhalten.

### Update von Version 1.3

- Die Einstellungen wandern beim ersten Start automatisch aus dem EEPROM ins
  **LittleFS** – nichts weiter zu tun. Ein Zurück auf 1.3 nutzt wieder den alten
  (unveränderten) EEPROM-Stand.
- **OTA aus PlatformIO funktioniert nur noch mit Admin-Passwort.** In
  [platformio.ini](platformio.ini) bei `upload_flags = --auth=PASSWORT` das
  Admin-Passwort eintragen. Ohne Passwort: einmal per Browser-Upload aktualisieren.
- Das Einrichtungs-WLAN hat jetzt ein Passwort (Standard `tuer-einrichten`, im Web
  änderbar – bitte je Installation ändern).
- Die HTTP-API erwartet Parameter im **Body** (Formular), nicht mehr in der URL;
  Aufrufe von fremden Webseiten werden abgelehnt (siehe *Sicherheit*).

### Update über WLAN

Ab Version 1.2 ist kein USB-Kabel mehr nötig:

- **Browser:** Tab **System → Firmware-Update**, Datei
  `.pio/build/esp32dev/firmware.bin` (bzw. `nodemcuv2`) auswählen, hochladen.
- **PlatformIO:**

  ```powershell
  pio run -e esp32dev_ota  -t upload   # ESP32
  pio run -e nodemcuv2_ota -t upload   # ESP8266
  ```

  OTA ist nur aktiv, wenn ein Admin-Passwort gesetzt ist (danach einmal neu
  starten). In [platformio.ini](platformio.ini) bei `upload_flags = --auth=PASSWORT`
  das Passwort eintragen.

## Inbetriebnahme

1. Nach dem ersten Start mit dem WLAN `Tueroeffner-Setup` verbinden (Passwort
   `tuer-einrichten`, steht auch auf dem Display) und das Heim-WLAN eintragen.
2. Die IP-Adresse erscheint auf dem Display bzw. in der seriellen Ausgabe.
3. Im Browser die IP öffnen und unter **Einstellungen** eintragen:
   - **SIP-Zugang**: Server, Port, Benutzer, Passwort (eine eigene Nebenstelle für
     den Türöffner in der Telefonanlage anlegen),
   - **Türsummer & Anruf**: Dauer, Rufkette (z. B. `100, 101`), optional ein
     Öffnen-Code, Schalter „Beim Klingeln anrufen“.
4. Mit **Test-Anruf** prüfen: Telefon klingelt → abheben → Piepton → `*` (bzw. Code)
   drücken → Summer schaltet, Anruf endet.
5. Unter **System** ein **Admin-Passwort** setzen (die Oberfläche warnt, solange keins
   gesetzt ist), das Passwort der **WLAN-Einrichtung** ändern, bei Bedarf einen
   Tür-Passwort für den Kunden anlegen und eine **Sicherung** herunterladen.

## Weboberfläche

| Tab                | Inhalt                                                          |
|--------------------|-----------------------------------------------------------------|
| **Tür**            | Öffnen-Knopf, Status (SIP, Tür, Modus, letzter Anruf), Verlauf  |
| **Einstellungen**  | Summer, Rufkette, Code, Anrufe/Gästecodes, Nachtruhe, Praxis-Modus, Türkontakt, SIP |
| **Dienste**        | Push-Mitteilungen (ntfy/Telegram), Home Assistant (MQTT), Syslog |
| **System**         | Geräte-Info, Netzwerk, WLAN-Einrichtung, Zugänge, Sicherung, Firmware-Update |

**Anmeldung:** Die Oberfläche hat eine eigene Anmeldeseite (kein Browser-Fenster).
„Angemeldet bleiben“ hält die Anmeldung 30 Tage, auch über Neustarts des Geräts;
sonst bis zum Schließen des Browsers. Oben rechts stehen *Anmelden* / *Abmelden* und
mit wem man angemeldet ist.

| Admin-Passwort | Tür-Passwort | Tab „Tür“ (öffnen, Verlauf) | Einstellungen |
| --- | --- | --- | --- |
| – | – | offen | offen |
| gesetzt | – | **ohne Anmeldung** | Admin |
| gesetzt | gesetzt | Tür- oder Admin-Anmeldung | Admin |

Wechselt ein Passwort, werden die Anmeldungen der betroffenen Rolle beendet.

**Bedienung:**

- **Öffnen**: den Knopf **0,8 s gedrückt halten** (der Ring füllt sich) – kurzes
  Antippen öffnet nicht (Schutz vor versehentlichem Öffnen, z. B. in der Tasche).
  Mit der Tastatur (Enter/Leertaste) öffnet er sofort. Öffnen beendet auch einen
  gerade laufenden Anruf.
- **Verlauf**: zeigt die neuesten 5 Einträge, aufklappbar; Filter *Klingeln*,
  *Öffnungen*, *Anrufe*.
- Beim Klingeln: Hinweis auf der Seite, Titel des Browser-Tabs „🔔 Es klingelt“,
  Vibration (Handy) und – nach einem Klick auf die Seite – ein Ton.
- Passwortfelder lassen sich per Auge-Symbol im Klartext anzeigen.
- Der geöffnete Tab steht in der Adresse (`#einstellungen`, `#system`, …) und
  bleibt beim Neuladen erhalten.
- Antwortet das Gerät nicht mehr, erscheint ein deutlicher Hinweis.
- Auf dem Handy über „Zum Startbildschirm hinzufügen“ wie eine App nutzbar.

### HTTP-API

Änderungen per `POST`, Parameter als Formular im Body (`application/x-www-form-urlencoded`).

| Methode | Pfad           | Parameter                          | Funktion                  |
|---------|----------------|------------------------------------|---------------------------|
| GET     | `/status`      | –                                  | Live-Zustand als JSON     |
| GET     | `/config`      | –                                  | Einstellungen als JSON (Admin) |
| GET     | `/log`         | –                                  | Verlauf als JSON          |
| POST    | `/api/login`   | `user`, `pw`, `keep` (1 = 30 Tage) | Anmelden (setzt Sitzungs-Cookie) |
| POST    | `/logout`      | –                                  | Abmelden                  |
| POST    | `/open`        | –                                  | Tür öffnen                |
| POST    | `/call`        | –                                  | Test-Anruf                |
| POST    | `/setduration` | `s`                                | Summer-Dauer (Sekunden)   |
| POST    | `/setdial`     | `nr`, `pin`, `auto`, `s`           | Rufkette, Code, Dauer     |
| POST    | `/setincoming` | `on`, `guests`                     | Anrufe annehmen, Gästecodes (`Code:bisUnix:einmalig:Name;…`) |
| POST    | `/setsip`      | `server`, `port`, `user`, `pw`     | SIP-Zugang                |
| POST    | `/setmqtt`     | `server`, `port`, `user`, `pw`     | MQTT-Broker               |
| POST    | `/setsyslog`   | `server`                           | Syslog-Server             |
| POST    | `/setquiet`    | `on`, `from`, `to` (Minuten)       | Nachtruhe                 |
| POST    | `/setpraxis`   | `on`, `days` (Bits Mo–So), `from`, `to`, `from2`, `to2`, `free` | Praxis-Modus |
| POST    | `/setdoor`     | `on`, `inv`, `alert` (Minuten)     | Türkontakt                |
| POST    | `/setpush`     | `type`, `server`, `topic`, `token`, `ev` | Push-Mitteilungen   |
| POST    | `/testpush`    | –                                  | Test-Mitteilung           |
| POST    | `/setweb`      | `user`, `pw` oder `off=1`          | Admin-Zugang              |
| POST    | `/setop`       | `user`, `pw` oder `off=1`          | Tür-Zugang                |
| POST    | `/setap`       | `pw` (leer = offen)                | Passwort WLAN-Einrichtung |
| POST    | `/setnet`      | `static`, `ip`, `mask`, `gw`, `dns` | Netzwerk (Neustart)      |
| POST    | `/resetcounters` | –                                | Zähler auf 0              |
| GET     | `/backup`      | –                                  | Sicherung herunterladen   |
| POST    | `/restore`     | Felder der Sicherung (Formular)    | Sicherung einspielen      |
| POST    | `/restart`     | –                                  | Neustart                  |
| POST    | `/wifireset`   | –                                  | WLAN-Daten löschen        |
| POST    | `/update`      | Datei (multipart)                  | Firmware-Update           |

Der Tür-Zugang darf nur `/status`, `/log`, `/open` und `/call`. Ohne Anmeldung
antwortet das Gerät mit `401` (ohne Browser-Anmeldefenster).

Werkzeuge können statt der Anmeldeseite **Basic-Auth** verwenden, z. B.:
`curl -X POST -u admin:PASSWORT http://tueroeffner.local/open`

## Sicherheit

- **Passwort setzen!** Ohne Admin-Passwort kann jeder im WLAN die Tür öffnen; die
  Oberfläche zeigt dann eine Warnung. Anmeldung über die eigene Anmeldeseite
  (Sitzungs-Cookie `HttpOnly`, `SameSite=Strict`) oder Basic-Auth für Werkzeuge. Die
  Übertragung ist unverschlüsselt – den ESP nur im eigenen Netz betreiben (kein
  Port-Forwarding).
- **Sperre nach falschen Anmeldungen**: nach 5 Fehlversuchen wird die Adresse 60 s
  gesperrt, bei weiteren Fehlern länger (bis 15 min).
- **Schutz vor fremden Webseiten (CSRF)**: Änderungen werden nur von der eigenen Seite
  angenommen (`Origin`/`Sec-Fetch-Site`). `curl` & Co. ohne `Origin` funktionieren
  weiter. Ohne Passwort ist die Oberfläche zudem nur über IP-Adresse oder
  `tueroeffner(.local)` erreichbar (Schutz vor DNS-Rebinding).
- **SIP**: Anfragen (Anrufe, DTMF per INFO) werden nur von der Telefonanlage
  angenommen, DTMF-Töne per RTP nur von der Gegenstelle des Gesprächs.
- **Home Assistant**: „Tür öffnen“ reagiert nur auf `PRESS`/`OPEN`/`UNLOCK`; Befehle
  direkt nach dem Verbinden und gespeicherte (*retained*) Befehle werden ignoriert
  bzw. gelöscht – eine versehentlich gespeicherte Nachricht öffnet also nicht bei
  jedem Neustart die Tür.
- **OTA** aus PlatformIO nur mit Admin-Passwort; das WLAN-Einrichtungsportal bietet
  keinen Firmware-Upload an.
- **Notfall-Reset:** Klingel-Taster beim Einschalten 5 s gedrückt halten – dann sind
  beide Passwörter gelöscht und das Gerät nutzt wieder DHCP statt einer festen IP.
  Den Klingel-Taster deshalb **nicht von außen zugänglich** montieren.

## Anrufe an den Türöffner und Gästecodes

Unter **Einstellungen → Anrufe an den Türöffner** einschalten. Ruft jemand die
Nebenstelle des Türöffners (SIP-Benutzer) an, nimmt der ESP an, spielt einen kurzen
Ton und wartet 30 s auf einen Code:

- den **Öffnen-Code** (wie beim Rückruf) oder
- einen **Gästecode**: bis zu 5 Stück mit Name, optionalem Ablaufdatum und
  „einmalig“ (wird nach dem Öffnen gelöscht) – z. B. für Paketdienst oder Handwerker.

`*` oder `#` löscht die Eingabe. Nach 3 falschen Codes legt der ESP auf; nach 6 falschen
Codes innerhalb von 15 min werden Anrufe 15 min lang abgelehnt (mit Push-Meldung).
Ohne Öffnen-Code und ohne gültigen Gästecode werden keine Anrufe angenommen.

## Push-Mitteilungen

Unter **Dienste → Push-Mitteilungen**:

- **ntfy**: App „ntfy“ installieren, ein schwer erratbares Topic abonnieren (z. B.
  `tuer-k7x2p9`) und dasselbe Topic eintragen. Eigener ntfy-Server und Zugangs-Token
  sind möglich.
- **Telegram**: Bot über `@BotFather` anlegen, Bot-Token und Chat-ID eintragen.

Wählbar sind Meldungen bei Klingeln, Öffnen und „Tür zu lange offen“; die Sperre nach
falschen Codes wird immer gemeldet. Auf dem ESP32 laufen sie in einem eigenen Task und
prüfen das Server-Zertifikat. Auf dem ESP8266 werden sie vor dem Anruf gesendet
(verzögert den Anruf um 1–2 s), nicht während der Summer läuft, und ohne
Zertifikatsprüfung.

## Syslog

Unter **Dienste → Syslog** einen Server eintragen (UDP 514, z. B. rsyslog, Synology
Log Center oder das Home-Assistant-Add-on). Der ESP schickt dann alle Meldungen
(Start, WLAN, SIP-Anmeldung, MQTT, Ereignisse) dorthin – ideal zur Fehlersuche bei
einer Kundeninstallation. Codes und Passwörter werden nicht übertragen.

## Home Assistant

Voraussetzung: das Add-on **Mosquitto broker** und die **MQTT-Integration**.

1. In der Weboberfläche im Tab **Dienste** Broker (IP von Home Assistant),
   Port `1883` sowie Benutzer und Passwort eintragen → „Speichern & verbinden“.
2. Das Gerät **Türöffner** erscheint automatisch unter
   *Einstellungen → Geräte & Dienste → MQTT*.

| Entität                       | Typ         | Funktion                                         |
|-------------------------------|-------------|--------------------------------------------------|
| Tür öffnen                    | Button      | Summer auslösen (beendet laufenden Anruf)        |
| Türsummer                     | Schloss     | „Öffnen“/„Aufschließen“ löst den Summer aus      |
| Letztes Ereignis              | Sensor      | z. B. „Geöffnet (Gästecode) – Paketdienst“       |
| SIP-Status                    | Sensor      | „angemeldet“, „Zugangsdaten falsch“, … (Diagnose)|
| Anrufe annehmen               | Schalter    | Anrufe an den Türöffner ein/aus (Konfiguration)  |
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

Jedes Ereignis wird zusätzlich auf `tueroeffner/<id>/event` veröffentlicht (JSON mit
`type`, `event`, `detail`) – praktisch für eigene Automationen.

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
| `LOOP_WATCHDOG_SEC`      | 60              | Watchdog, falls das Programm hängt                 |
| `HOSTNAME`               | `tueroeffner`   | Name im Netz (`http://tueroeffner.local`)          |
| `TIME_ZONE`              | MEZ/MESZ        | Zeitzone für Verlauf und Nachtruhe                 |
| `SCREEN_TIMEOUT_SECONDS` | 60              | OLED-Bildschirmschoner (0 = aus)                   |
| `MQTT_DISCOVERY_PREFIX`  | `homeassistant` | Discovery-Prefix von Home Assistant                |
| `WIFI_AP_PASSWORD`       | `tuer-einrichten` | Standard-Passwort der WLAN-Einrichtung           |
| `WIFI_PORTAL_SECONDS`    | 300             | so lange bleibt die WLAN-Einrichtung offen         |
| `INCOMING_CALL_SECONDS`  | 30              | Dauer eines Anrufs an den Türöffner                |
| `INCOMING_LOCK_FAILS`    | 6               | falsche Codes bis zur Sperre (`INCOMING_LOCK_MIN`) |
| `AUTH_MAX_FAILS`         | 5               | falsche Anmeldungen bis zur Sperre                 |
| `LOG_SIZE`               | 30              | Einträge im Verlauf                                |

**Fehlersuche SIP:** In [platformio.ini](platformio.ini) `build_flags = -DDEBUGLOG`
einkommentieren – dann werden alle SIP-Pakete seriell ausgegeben.

## Projektstruktur

| Datei | Inhalt |
|-------|--------|
| [src/main.cpp](src/main.cpp) | Start, Loop, WLAN/Einrichtung, Netzdienste, Watchdog, Selbstheilung |
| [src/settings.cpp](src/settings.cpp) | Einstellungen (eine Tabelle für Datei, Sicherung, Wiederherstellung), Zähler/Verlauf, Übernahme aus dem EEPROM |
| [src/events.cpp](src/events.cpp) | Ereignisprotokoll, serielle Ausgabe, Syslog |
| [src/door.cpp](src/door.cpp) | Relais/Summer, Eingänge, Zeitfenster, Display, Status-LED |
| [src/phone.cpp](src/phone.cpp) | SIP-Anmeldung, Rufkette, Codes, Anrufe an den Türöffner |
| [src/push.cpp](src/push.cpp) | ntfy / Telegram |
| [src/mqtt.cpp](src/mqtt.cpp) | Home Assistant |
| [src/web.cpp](src/web.cpp) | HTTP-API, Anmeldung, Schutzmechanismen |
| [src/web/index.html](src/web/index.html) | Weboberfläche |
| [src/config.h](src/config.h) | Pins und Grundeinstellungen |
| [lib/ArduinoSIP](lib/ArduinoSIP) | SIP-Client (REGISTER, INVITE, Digest, RTP, eingehende Anrufe) |

Neue Einstellung: Variable in `settings.cpp` anlegen, eine Zeile in der Tabelle
`SETTINGS` ergänzen – Speichern, Sicherung und Wiederherstellung funktionieren damit
automatisch.

## Lizenz

Dieses Projekt steht unter der [Apache License 2.0](LICENSE).
Die mitgelieferte Bibliothek [lib/ArduinoSIP](lib/ArduinoSIP) stammt von
Juergen Liegner und Thorsten Godau, wurde für dieses Projekt erweitert und steht
weiterhin unter der BSD-3-Clause-Lizenz (siehe [NOTICE](NOTICE) und den Kopf von
`ArduinoSIP.h`).

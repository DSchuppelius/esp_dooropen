# esp_dooropen

SIP-Türöffner für **ESP32 DevKit (WROOM-32)**, **WT32-ETH01** (ESP32 mit Ethernet) oder
**ESP8266 HW-364A** (NodeMCU mit fest verbautem OLED). Klingelt es, ruft der ESP per SIP ein Telefon an; mit **`*`** (oder einem Code) am
Telefon, über die **Weboberfläche**, per **Taster** oder aus **Home Assistant** wird die
Tür geöffnet.

Die Zielplattform wird über die PlatformIO-Umgebung gewählt (siehe
[Bauen & Flashen](#bauen--flashen-platformio)); der Code passt sich per
Präprozessor-Weiche automatisch an.

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
  - **SIP-Status im Klartext** und Ergebnis des letzten Anrufs,
  - **robust im WLAN**: `INVITE` (auch mit Digest), `CANCEL`, `BYE` und `REGISTER`
    werden bei Paketverlust wiederholt (RFC 3261); eine gescheiterte Erneuerung
    meldet erst „abgemeldet“, wenn die Anmeldung wirklich abläuft. Kopfzeilen in
    jeder Schreibweise und Kurzform (`i:`, `f:` …) werden verstanden; ein Server als
    Hostname wird einmal je Anmeldung aufgelöst (folgt auch einem DHCP-Wechsel).
- **Klingeln per Anruf**: Eine TK-Anlage (z. B. OpenCom) ruft beim Klingeln den
  Türöffner an – er nimmt nicht ab und wertet das als Klingeln. Ganz ohne Verdrahtung.
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
- **Zweites Relais** (Tor, Garage, zweite Tür) mit eigenem Knopf, eigener Dauer und
  wahlweise auch für den Tür-Zugang.
- **Tastenfeld / RFID-Leser** (Wiegand, ESP32): Öffnen-Code oder Gästecode am
  Tastenfeld, Karten/Chips mit Namen; Sperre nach Fehlversuchen.
- **Telegram-Bot** (ESP32): Klingel-Mitteilung mit Knopf **„Öffnen“**, Befehle
  `/status` und `/oeffnen` – auch unterwegs, ohne Portfreigabe.
- **Apple Home** (HomeKit, eigene ESP32-Variante): Türschloss mit Türklingel direkt in
  der Home-App, ohne Home Assistant.
- **Zugänge** mit eigener Anmeldeseite: Admin (alles) und Tür-Zugang (nur öffnen und
  Verlauf sehen, z. B. für den Kunden; ohne Tür-Passwort ist die Tür offen), dazu bis
  zu 8 **weitere Benutzer** mit Rolle und Zeitfenster – der Verlauf zeigt, wer
  geöffnet hat. Ohne Admin-Passwort warnt die Oberfläche deutlich.
- **Firmware-Update über WLAN**: per Browser-Upload oder aus PlatformIO (OTA, nur
  mit Admin-Passwort).
- **Erreichbar unter `http://tueroeffner.local`** (mDNS).
- **Verlauf** der letzten 30 Ereignisse mit Uhrzeit (NTP) und Detail (z. B. Name des
  Gästecodes, Nummer des Anrufers, Benutzer); **übersteht Neustarts**. Dazu ein langer
  Verlauf (mehrere Tausend Einträge) im Flash, als **CSV** herunterladbar.
- **Nachtruhe**: in einem Zeitfenster beim Klingeln nicht anrufen.
- **Praxis-Modus**: an gewählten Wochentagen in **zwei Zeitfenstern** öffnet Klingeln
  die Tür automatisch (ohne Anruf); **gesetzliche Feiertage je Bundesland** werden
  automatisch berücksichtigt, weitere **Ausnahmetage** (Urlaub) einstellbar. Bei
  Fenstern über Mitternacht (z. B. 22–2 Uhr) gehört der Teil nach Mitternacht zum
  Vortag (Wochentag, Feier- und Ausnahmetag des Vortags).
- **Uhrzeit** per NTP: `pool.ntp.org`, ersatzweise das **Gateway** (Router oder
  TK-Anlage, z. B. FRITZ!Box „Zeitserver im Heimnetz bereitstellen“) und
  `de.pool.ntp.org`. So laufen Praxis-Modus, Nachtruhe und Zeitfenster auch ohne
  Internet, wenn der Router NTP anbietet. Ohne gültige Uhrzeit sind Praxis-Modus und
  Nachtruhe aus.
- **Türkontakt** (Reed): Tür offen/zu im Web und in Home Assistant, Meldung, wenn die
  Tür zu lange offen steht, und Prüfung, ob nach dem Summer wirklich jemand
  hereinkam („Tür nach Summer geöffnet“ / „Summer, Tür blieb zu“).
- **Push-Mitteilungen ohne Home Assistant** über **ntfy** oder **Telegram**
  (Klingeln, Öffnen, Tür zu lange offen, Sperre nach falschen Codes).
- **Syslog**: Meldungen und Ereignisse an einen Syslog-Server (Fehlersuche aus der Ferne).
- **Ethernet** statt WLAN mit dem WT32-ETH01 (z. B. per PoE versorgt).
- **Selbstheilung**: Ist das WLAN weg, verbindet der ESP jede Minute neu; als letzte
  Stufe Neustart nach 30 min. Ethernet: Neustart nur, wenn ein Link da ist, aber
  10 min keine IP kommt (bei gezogenem Kabel nicht). Außerdem Neustart, wenn der
  Speicher knapp oder zerstückelt ist oder das Programm hängt (Watchdog auf beiden
  Boards); der Grund des letzten Starts wird angezeigt.
- **Neue IP per DHCP** wird erkannt, SIP meldet sich dann sofort neu an.
- **Sicherung**: alle Einstellungen als Datei herunterladen und wieder einspielen.
- **Feste IP-Adresse** wahlweise statt DHCP (vor dem Speichern geprüft: Maske,
  Gateway im selben Netz, keine Netz-/Broadcast-Adresse – gegen Aussperren).
- **Sperre gegen Sturmklingeln**: Klingeln innerhalb von 5 s löst keinen neuen
  Anruf aus.
- **WiFiManager**: Ohne gespeicherte Zugangsdaten öffnet der ESP das
  passwortgeschützte WLAN `Tueroeffner-Setup` zum Eintragen – **ohne zu blockieren**:
  Klingel, Taster und Summer funktionieren währenddessen weiter. Ist das bekannte WLAN
  nur weg (z. B. startet der Router nach einem Stromausfall langsamer als der ESP),
  verbindet er weiter und öffnet die Einrichtung erst nach 15 min ohne Verbindung –
  dann für 5 min, während die Verbindung zum WLAN weiter versucht wird.
- Einstellungen, Zähler und Verlauf liegen im **LittleFS** (Zähler/Verlauf werden
  höchstens alle 5 min geschrieben, der lange Verlauf gebündelt höchstens einmal pro
  Minute, um den Flash zu schonen; vor Neustart und Update wird alles gesichert).
  Gespeichert wird geprüft über eine Hilfsdatei – bei vollem oder defektem Speicher
  bleibt die alte Datei, die Oberfläche meldet den Fehler. Für die Einstellungen bleibt
  immer Platz frei (notfalls wird der ältere Verlauf gelöscht). Wurde der Speicher beim
  Start formatiert oder ist er nicht verfügbar, zeigt die Oberfläche eine Warnung.
  Ereignisse ohne Uhrzeit (kein NTP) bleiben erhalten und erscheinen als „ohne Uhrzeit“.

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
| Relais 2 (optional)  | GPIO13   | zweites Relais (Tor, Garage …)           |
| Wiegand D0 (optional)| GPIO18   | Tastenfeld/RFID-Leser, siehe unten       |
| Wiegand D1 (optional)| GPIO19   | Tastenfeld/RFID-Leser, siehe unten       |

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
| Relais 2 (opt.)     | D0 (GPIO16) | zweites Relais (Tor, Garage …)           |

Tastenfeld/RFID (Wiegand) gibt es auf dem ESP8266 nicht – dafür sind keine Pins frei.

**Summer-Taster an D3 (GPIO0):** GPIO0 entscheidet beim Start, ob der ESP8266 normal
startet oder auf eine neue Firmware wartet. Ist der Taster beim Einschalten **oder bei
einem Neustart** (Update, Selbstheilung, Watchdog) gedrückt – oder klemmt er bzw. ist
die Leitung feucht –, bleibt der ESP im Flash-Modus hängen, bis er stromlos war. Die
Firmware kann das nicht abfangen. Für Dauerbetrieb daher ESP32 bzw. die Steuerplatine
nutzen oder den Taster so montieren, dass er nicht klemmen kann.

### WT32-ETH01 (Ethernet)

Umgebung `wt32-eth01`. Netzwerk über das eingebaute LAN8720 (kein WLAN, keine
WLAN-Einrichtung; feste IP wie gewohnt in der Weboberfläche). IO35/IO36/IO39 sind
reine Eingänge **ohne** internen Pull-up.

| Funktion             | Pin   | Hinweis                                       |
|----------------------|-------|-----------------------------------------------|
| Relais (Summer)      | IO4   | active-high                                   |
| Klingel-Kontakt      | IO32  | gegen GND                                     |
| Klingel-Taster       | IO33  | gegen GND                                     |
| Summer-Taster        | IO12  | gegen GND (Strapping-Pin: nie auf 3,3 V ziehen) |
| Türkontakt           | IO2   | gegen GND                                     |
| Status-LED           | IO17  | LED mit 330 Ω gegen GND                        |
| Relais 2             | IO5   | Strapping-Pin, siehe Relais beim Start        |
| Wiegand D0 / D1      | IO35 / IO36 | Pull-up bzw. Pegelwandler nötig         |
| OLED SDA / SCL       | IO15 / IO14 | optional                                |

### Relais beim Start

Die Firmware setzt die Relais als allererstes in Ruhelage. Davor – während Reset,
Bootloader und Programmstart (einige 100 ms) – bestimmt die Hardware den Pegel:

| Board | Relais 2 | Pegel beim Start | Risiko |
| --- | --- | --- | --- |
| ESP32 DevKit / [Steuerplatine](hardware/README.md) | GPIO13 | kein Pull-up | keins (Platine: 100 kΩ hält das Gate auf GND) |
| WT32-ETH01 | IO5 | interner **Pull-up** (Strapping-Pin) | active-high-Modul kann kurz anziehen |
| ESP8266 | D0 (GPIO16) | beim Start **HIGH** | active-high-Modul kann kurz anziehen |

Relais 1 (Summer: GPIO26, IO4, D1/GPIO5) ist davon nicht betroffen. Hängt an Relais 2
etwas, das schon ein kurzer Impuls auslöst (Tor, Garage):

- **WT32-ETH01:** einen **Pull-down von 4,7–10 kΩ** vom Relais-Eingang (IN/S) nach GND
  einbauen – stärker als der interne Pull-up; IO5 darf beim Start LOW sein (betrifft
  nur das SDIO-Timing).
- **WT32-ETH01 und ESP8266:** ein Relais-Modul nehmen, das bei **LOW** schaltet, und in
  [src/config.h](src/config.h) `RELAY2_ACTIVE_LOW` auf `true` setzen – der HIGH-Pegel
  beim Start bedeutet dann „aus“.

### Tastenfeld / RFID-Leser (Wiegand)

Viele Zutritts-Tastenfelder und Kartenleser haben eine **Wiegand-Schnittstelle**
(Leitungen D0, D1, GND). Erkannt werden Tastendrücke (4 bzw. 8 Bit) und Karten
(26 Bit mit Parität, 34 Bit).

```text
Leser D0 (grün) ──[Pegelwandler]── GPIO18     Leser GND ── GND
Leser D1 (weiß) ──[Pegelwandler]── GPIO19     Leser +12 V ── eigenes Netzteil
```

Die meisten Leser ziehen D0/D1 auf **5 V** hoch – das verträgt der ESP32 nicht. Dann
einen **Pegelwandler** (z. B. BSS138-Modul) oder je Leitung einen Spannungsteiler
(z. B. 10 kΩ / 20 kΩ) dazwischen. Leser mit Open-Collector-Ausgang können direkt an
die Pins (der ESP schaltet den internen Pull-up ein).

Bedienung am Tastenfeld: Code eintippen, **#** bestätigt, ***** löscht. Gültig sind
Öffnen-Code und Gästecodes. Karten werden unter **Einstellungen → Tastenfeld / RFID**
angelernt: Karte an den Leser halten, die Nummer erscheint dort zum Übernehmen.

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

Gegen Störungen: Das Signal allein braucht mindestens 3 aktive Abtastungen
(`SIGNAL_MIN_SAMPLES`), und solange der Summer an ist sowie 300 ms danach zählt
Klingeln nicht (`RING_BUZZER_GUARD_MS`, Einkopplung vom Türöffner). Ein Eingang, der
schon beim Start aktiv ist (klemmender Taster, Feuchte), zählt erst, nachdem er einmal
frei war – das gilt auch für den Summer-Taster. Hängt das Programm kurz (Netzwerk,
Flash), erkennt ein Interrupt Tastendrücke ab 50 ms trotzdem und holt sie nach.

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

**Alternative ganz ohne Verdrahtung:** Die Anlage ruft beim Klingeln den Türöffner an –
siehe [Klingeln per Anruf](#klingeln-per-anruf-tk-anlage).

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
pio run -e esp32dev         -t upload   # ESP32 DevKit
pio run -e esp32dev_homekit -t upload   # ESP32 DevKit mit Apple Home
pio run -e wt32-eth01       -t upload   # WT32-ETH01 (Ethernet)
pio run -e nodemcuv2        -t upload   # ESP8266 HW-364A

pio device monitor -b 115200            # serielle Ausgabe
```

| Umgebung           | Board          | Besonderheit                                       |
|--------------------|----------------|----------------------------------------------------|
| `esp32dev`         | ESP32 DevKit   | alle Funktionen außer Apple Home                   |
| `esp32dev_homekit` | ESP32 DevKit   | zusätzlich Apple Home (Programmspeicher ~97 % voll) |
| `wt32-eth01`       | WT32-ETH01     | Ethernet statt WLAN                                |
| `nodemcuv2`        | ESP8266        | ohne Telegram-Bot, Tastenfeld/RFID und Apple Home  |

Apple Home ist eine eigene Variante, weil die Bibliothek HomeSpan rund 400 KB belegt
und schon beim Start in die WLAN-Einstellungen eingreift.

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

### Update auf Version 1.5

- Neue Einstellungen erscheinen mit ausgeschalteten Voreinstellungen – das Verhalten
  bleibt, bis man etwas aktiviert.
- Der lange Verlauf (CSV) beginnt mit dem Update; der bisherige kurze Verlauf bleibt.

### Update von Version 1.3

- Die Einstellungen wandern beim ersten Start automatisch aus dem EEPROM ins
  **LittleFS** – nichts weiter zu tun. Danach wird der alte EEPROM-Stand stillgelegt
  (er wird nie wieder geladen, auch nicht nach einem Formatieren des LittleFS); ein
  Zurück auf 1.3 beginnt deshalb mit Standardwerten.
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
  gerade laufenden Anruf. Der Countdown startet erst, wenn das Gerät das Öffnen
  bestätigt hat; ein Fehler wird angezeigt.
- **Verlauf**: zeigt die neuesten 5 Einträge, aufklappbar; Filter *Klingeln*,
  *Öffnungen*, *Anrufe*.
- Beim Klingeln: Hinweis auf der Seite, Titel des Browser-Tabs „🔔 Es klingelt“,
  Vibration (Handy) und – nach einem Klick auf die Seite – ein Ton.
- Passwortfelder lassen sich per Auge-Symbol im Klartext anzeigen.
- Der geöffnete Tab steht in der Adresse (`#einstellungen`, `#system`, …) und
  bleibt beim Neuladen erhalten.
- Antwortet das Gerät nicht mehr, erscheint ein deutlicher Hinweis (nach 3 s ohne
  Antwort gilt die Anzeige als veraltet; Anfragen brechen nach 4 s ab).
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
| POST    | `/open2`       | –                                  | zweites Relais schalten   |
| GET     | `/log.csv`     | –                                  | langer Verlauf als CSV    |
| POST    | `/call`        | –                                  | Test-Anruf                |
| POST    | `/setduration` | `s`                                | Summer-Dauer (Sekunden)   |
| POST    | `/setdial`     | `nr`, `pin`, `auto`, `s`           | Rufkette, Code, Dauer     |
| POST    | `/setincoming` | `on`, `guests`                     | Anrufe annehmen, Gästecodes (`Code:bisUnix:einmalig:Name;…`) |
| POST    | `/setsip`      | `server`, `port`, `user`, `pw`     | SIP-Zugang                |
| POST    | `/setmqtt`     | `server`, `port`, `user`, `pw`     | MQTT-Broker               |
| POST    | `/setsyslog`   | `server`                           | Syslog-Server             |
| POST    | `/setring`     | `callers` (Liste, `*` = jeder)     | Klingeln per Anruf        |
| POST    | `/setrelay2`   | `on`, `name`, `dur`, `user`        | zweites Relais            |
| POST    | `/setusers`    | `users` (`Name:Passwort:Rolle:Tage:Von:Bis;…`, Felder %-kodiert, leeres Passwort = unverändert) | weitere Benutzer |
| POST    | `/settelegram` | `open`, `chats`                    | Telegram-Bot              |
| POST    | `/setkeypad`   | `on`, `cards` (`Nummer:Name;…`)    | Tastenfeld / RFID         |
| POST    | `/sethomekit`  | `on`, `reset` (neuer Code)         | Apple Home (nach Neustart)|
| POST    | `/setquiet`    | `on`, `from`, `to` (Minuten)       | Nachtruhe                 |
| POST    | `/setpraxis`   | `on`, `days` (Bits Mo–So), `from`, `to`, `from2`, `to2`, `free`, `holiday` (z. B. `NW`) | Praxis-Modus |
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

Der Tür-Zugang darf nur `/status`, `/log`, `/log.csv`, `/open`, `/call` und – falls
freigegeben – `/open2`. Ohne Anmeldung
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
  angenommen, DTMF-Töne per RTP nur von der Gegenstelle des Gesprächs oder der Anlage.
- **Home Assistant**: „Tür öffnen“ reagiert nur auf `PRESS`/`OPEN`/`UNLOCK`; Befehle
  direkt nach dem Verbinden und gespeicherte (*retained*) Befehle werden ignoriert
  bzw. gelöscht (noch vor dem Abonnieren) – eine versehentlich gespeicherte Nachricht
  öffnet also nicht bei jedem Neustart die Tür.
- **Telegram**: Ein „Öffnen“-Knopf gilt nur kurz und nur bis zum nächsten Neustart;
  Nachrichten, die vor dem Start eingingen, werden verworfen.
- **OTA** aus PlatformIO nur mit Admin-Passwort; das WLAN-Einrichtungsportal bietet
  keinen Firmware-Upload an.
- **Notfall-Reset:** Klingel-Taster beim Einschalten (oder nach der Reset-Taste)
  5 s gedrückt halten, bis „Jetzt loslassen“ erscheint, und **loslassen** – dann sind
  beide Passwörter gelöscht und das Gerät nutzt wieder DHCP statt einer festen IP.
  Nach einem Neustart per Software oder Watchdog wirkt der Taster nicht, ein
  dauerhaft gedrückter (klemmender) Taster löst nie aus. Der Reset steht im Verlauf
  („Gerät gestartet – Notfall-Reset“) und wird per Push gemeldet. Den Klingel-Taster
  trotzdem **nicht von außen zugänglich** montieren.

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
Klingelt es während eines solchen Anrufs, hat die Klingel Vorrang: der Anruf wird
beendet und die Rufkette startet, sobald die Leitung frei ist.

## Klingeln per Anruf (TK-Anlage)

Statt das Klingelsignal zu verdrahten, kann die Telefonanlage den Türöffner beim
Klingeln **anrufen** – genau wie sie die Telefone anruft. Unter **Einstellungen →
Klingeln per Anruf** die Nummer(n) eintragen, von denen solche Anrufe kommen (`*` =
jeder Anrufer). Der Türöffner nimmt diese Anrufe nicht an (Antwort „486 Besetzt“, damit
die Anlage die übrigen Telefone weiter klingeln lässt) und wertet sie als Klingeln:
Verlauf, Push, Home Assistant, Praxis-Modus – alles wie beim Klingelkontakt.

Einrichten, z. B. OpenCom: in der Anlage beim Türklingel-Sensor die Nebenstelle des
Türöffners als (zusätzliches) Ziel eintragen, einmal klingeln, dann in der
Weboberfläche bei „Letzter Anrufer“ auf **Übernehmen** klicken. Damit der Türöffner
nicht zusätzlich selbst anruft, „Beim Klingeln anrufen“ ausschalten, wenn die Anlage
die Telefone schon klingeln lässt.

## Weitere Benutzer

Unter **System → Weitere Benutzer** bis zu 8 Personen mit eigenem Namen und Passwort
anlegen – Rolle **Tür** (öffnen, Verlauf) oder **Admin**. Optional mit Zeitfenster
(Wochentage, Uhrzeit), z. B. „Reinigung: Mo–Fr 6–9 Uhr“. Außerhalb des Fensters ist
keine Anmeldung möglich und bestehende Anmeldungen ruhen. Im Verlauf steht, wer
geöffnet hat (z. B. „Geöffnet (Web) – Anna“).

## Push-Mitteilungen

Unter **Dienste → Push-Mitteilungen**:

- **ntfy**: App „ntfy“ installieren, ein schwer erratbares Topic abonnieren (z. B.
  `tuer-k7x2p9`) und dasselbe Topic eintragen. Eigener ntfy-Server und Zugangs-Token
  sind möglich.
- **Telegram**: Bot über `@BotFather` anlegen, Bot-Token und Chat-ID eintragen.

Auf dem ESP8266 hat jede Mitteilung ein festes Zeitbudget von 5 s (Namensauflösung
höchstens 2 s). Ist der Server nicht auflösbar oder für HTTPS zu wenig Speicher frei
(BearSSL braucht 16 KB am Stück), wird sie übersprungen und das im Log vermerkt. Bis zu
3 Mitteilungen warten; ist die Warteschlange voll, fällt die älteste weg (ebenfalls im
Log, auf dem ESP32 bei mehr als 4 wartenden).

Zusätzlich wählbar: **„Melden, wenn nach dem Summer niemand hereinkam“** (braucht den
Türkontakt).

### Telegram-Bot (ESP32)

Ist Telegram eingestellt, beantwortet der Türöffner auch Nachrichten:

| Befehl       | Funktion                                              |
|--------------|-------------------------------------------------------|
| `/status`    | Zustand (SIP, Zähler, Tür, Modus, letztes Ereignis)   |
| `/oeffnen`   | Rückfrage mit Knopf **„Jetzt öffnen“**                |

Mit **„Öffnen per Telegram erlauben“** bekommen Klingel-Mitteilungen einen Knopf
**„🔓 Öffnen“**. Ein Knopf gilt 3 Minuten und nur bis zum nächsten Neustart – alte
Mitteilungen öffnen später nicht mehr. Nachrichten, die vor dem Start eingingen,
verwirft der Bot.
Nur die eingetragene Chat-ID und die **weiteren erlaubten Chat-IDs** dürfen den Bot
benutzen; ein fremder Chat bekommt seine Chat-ID angezeigt (zum Freischalten). Der Bot
fragt per Long-Polling bei Telegram nach – keine Portfreigabe nötig. Auf dem ESP8266
nicht verfügbar (zu wenig Speicher für die dauernde TLS-Verbindung).

Wählbar sind Meldungen bei Klingeln, Öffnen und „Tür zu lange offen“; die Sperre nach
falschen Codes wird immer gemeldet. Auf dem ESP32 laufen sie in einem eigenen Task und
prüfen das Server-Zertifikat. Auf dem ESP8266 werden sie erst nach dem Anruf bzw. der
Rufkette gesendet (der Anruf hat Vorrang), nicht während der Summer läuft, und ohne
Zertifikatsprüfung.

## Syslog

Unter **Dienste → Syslog** einen Server eintragen (UDP 514, z. B. rsyslog, Synology
Log Center oder das Home-Assistant-Add-on). Der ESP schickt dann alle Meldungen
(Start, WLAN, SIP-Anmeldung, Gespräche, MQTT, Ereignisse) dorthin – ideal zur Fehlersuche bei
einer Kundeninstallation. Codes und Passwörter werden nicht übertragen.

## Apple Home (HomeKit)

Variante `esp32dev_homekit` flashen, dann unter **Dienste → Apple Home** einschalten
und neu starten. In der Home-App: **Gerät hinzufügen → Weitere Optionen →
Türöffner** und den angezeigten Kopplungscode eingeben.

- **Türschloss**: „Aufschließen“ löst den Summer aus; das Schloss zeigt „offen“,
  solange der Summer läuft.
- **Türklingel**: Klingeln erzeugt eine Mitteilung auf iPhone, Watch und HomePod.

HomeSpan nutzt das WLAN des Türöffners und den Port 1201; `tueroeffner.local` und die
Weboberfläche bleiben unverändert. Ein- und Ausschalten wirkt nach einem Neustart.
HomeSpan 2.0 bekommt dazu die WLAN-Zugangsdaten des Türöffners (ohne sie startet es
den HomeKit-Dienst nicht), baut die Verbindung aber nicht selbst auf – das WLAN
verwaltet weiter der Türöffner. Serielle HomeSpan-Befehle sind abgeschaltet. Der Status in der
Weboberfläche zeigt, ob der Dienst wirklich läuft („wartet auf WLAN“, „bereit zum
Koppeln“, „gekoppelt“). Beim Start wartet HomeSpan einmalig 2 s (fest in der Bibliothek).

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
| *Name* öffnen                 | Button      | zweites Relais (nur wenn aktiviert)              |

Jedes Ereignis wird zusätzlich auf `tueroeffner/<id>/event` veröffentlicht (JSON mit
`type`, `event`, `detail`) – praktisch für eigene Automationen.

Fällt der ESP aus, werden alle Entitäten als *nicht verfügbar* angezeigt.
Nach einem Neustart von Home Assistant meldet sich der ESP automatisch neu an. Ist der
Broker nicht erreichbar, versucht er es in wachsenden Abständen erneut (bis 5 min);
ein Broker-Name (auch `.local`) wird aufgelöst, ohne die Klingel zu blockieren.

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
| `WIFI_LOST_RESTART_MIN`  | 30              | Neustart, wenn das WLAN so lange weg ist           |
| `WIFI_RETRY_SEC`         | 60              | WLAN weg: so oft neu verbinden (s)                 |
| `ETH_NO_IP_RESTART_MIN`  | 10              | Ethernet: Neustart, wenn Link ohne IP so lange     |
| `LOOP_WATCHDOG_SEC`      | 60              | Watchdog, falls das Programm hängt                 |
| `HOSTNAME`               | `tueroeffner`   | Name im Netz (`http://tueroeffner.local`)          |
| `TIME_ZONE`              | MEZ/MESZ        | Zeitzone für Verlauf und Nachtruhe                 |
| `SCREEN_TIMEOUT_SECONDS` | 60              | OLED-Bildschirmschoner (0 = aus)                   |
| `MQTT_DISCOVERY_PREFIX`  | `homeassistant` | Discovery-Prefix von Home Assistant                |
| `WIFI_AP_PASSWORD`       | `tuer-einrichten` | Standard-Passwort der WLAN-Einrichtung           |
| `WIFI_PORTAL_SECONDS`    | 300             | so lange bleibt die WLAN-Einrichtung offen         |
| `WIFI_PORTAL_AFTER_MIN`  | 15              | WLAN-Einrichtung erst nach so langer WLAN-Störung  |
| `INCOMING_CALL_SECONDS`  | 30              | Dauer eines Anrufs an den Türöffner                |
| `INCOMING_LOCK_FAILS`    | 6               | falsche Codes bis zur Sperre (`INCOMING_LOCK_MIN`) |
| `AUTH_MAX_FAILS`         | 5               | falsche Anmeldungen bis zur Sperre                 |
| `LOG_SIZE`               | 30              | Einträge im (kurzen) Verlauf                       |
| `EVENT_FILE_MAX`         | 48000 / 24000   | Größe einer Datei des langen Verlaufs (Bytes, 2 Dateien; ESP32 24000) |
| `FS_RESERVE_BYTES`       | 24576           | im LittleFS immer frei für Einstellungen (Bytes)   |
| `EVENT_FLUSH_MS`         | 60000           | langen Verlauf höchstens so oft schreiben (ms)     |
| `WEB_SEND_MAX_MS`        | 5000            | Startseite/CSV höchstens so lange senden (ms)      |
| `DOOR_PASS_WINDOW_SEC`   | 10              | so lange nach dem Summer muss die Tür aufgehen     |
| `USERS_MAX`              | 8               | weitere Benutzer                                   |
| `KEYPAD_LOCK_FAILS`      | 5               | Fehlversuche am Tastenfeld bis zur Sperre (`KEYPAD_LOCK_MIN`) |
| `TELEGRAM_BUTTON_SEC`    | 180             | Gültigkeit des Telegram-Knopfs „Öffnen“            |

**Fehlersuche SIP:** Nach jedem Gespräch steht eine Zeile im Log (seriell/Syslog), z. B.
`SIP: Gespraech beendet (PCMA, DTMF-PT 101, Gegenstelle 192.168.20.247, RTP 312, Tasten 1)`.
`RTP 0` heißt: kein Audio vom Telefon angekommen; `Tasten 0` trotz Tastendruck: die Anlage
schickt DTMF nicht per RFC 4733/SIP INFO (z. B. nur als Ton im Audio – in der Anlage
umstellen); `verworfen … von <IP>`: RTP kam von einer unerwarteten Adresse.
Für alle SIP-Pakete in [platformio.ini](platformio.ini) `build_flags = -DDEBUGLOG`
einkommentieren – dann werden sie seriell ausgegeben.

## Projektstruktur

| Datei | Inhalt |
|-------|--------|
| [src/main.cpp](src/main.cpp) | Start, Loop, WLAN/Einrichtung, Netzdienste, Watchdog, Selbstheilung |
| [src/settings.cpp](src/settings.cpp) | Einstellungen (eine Tabelle für Datei, Sicherung, Wiederherstellung), Zähler/Verlauf, Übernahme aus dem EEPROM |
| [src/events.cpp](src/events.cpp) | Ereignisprotokoll, serielle Ausgabe, Syslog |
| [src/door.cpp](src/door.cpp) | Relais/Summer, Eingänge, Zeitfenster, Display, Status-LED |
| [src/phone.cpp](src/phone.cpp) | SIP-Anmeldung, Rufkette, Codes, Anrufe an den Türöffner |
| [src/push.cpp](src/push.cpp) | ntfy / Telegram, Telegram-Bot |
| [src/keypad.cpp](src/keypad.cpp) | Tastenfeld / RFID (Wiegand) |
| [src/homekit.cpp](src/homekit.cpp) | Apple Home (HomeSpan) |
| [src/net.cpp](src/net.cpp) | Netzwerk-Abstraktion WLAN / Ethernet |
| [src/holidays.h](src/holidays.h) | gesetzliche Feiertage je Bundesland |
| [src/timewin.h](src/timewin.h) | Zeitfenster (auch über Mitternacht) |
| [src/presswatch.h](src/presswatch.h) | verpasste Tastendrücke per Interrupt nachholen |
| [src/mqtt.cpp](src/mqtt.cpp) | Home Assistant |
| [src/web.cpp](src/web.cpp) | HTTP-API, Anmeldung, Schutzmechanismen |
| [src/web/index.html](src/web/index.html) | Weboberfläche |
| [src/config.h](src/config.h) | Pins und Grundeinstellungen |
| [lib/ArduinoSIP](lib/ArduinoSIP) | SIP-Client (REGISTER, INVITE, Digest, RTP, eingehende Anrufe) |

Neue Einstellung: Variable in `settings.cpp` anlegen, eine Zeile in der Tabelle
`SETTINGS` ergänzen – Speichern, Sicherung und Wiederherstellung funktionieren damit
automatisch.

## Tests und CI

- **Host-Tests** ([tests/host](tests/host)): SIP-Bibliothek (Digest nach RFC 2617,
  REGISTER, ausgehende/eingehende Anrufe, Klingel-Anrufe, DTMF, Wiederholungen bei
  Paketverlust, Kurzformen/Schreibweisen, Re-INVITE, 20 000 Zufallspakete)
  sowie Feiertagsberechnung, Zeitfenster über Mitternacht und das Nachholen verpasster
  Tastendrücke – übersetzt mit AddressSanitizer/UBSan:

  ```bash
  bash tests/host/run.sh      # Linux oder WSL, braucht g++
  ```

- Beim Bauen prüft [tools/embed_html.py](tools/embed_html.py) das JavaScript der
  Weboberfläche mit Node (falls installiert) und bricht bei Syntaxfehlern ab.
- **GitHub Actions** ([.github/workflows/ci.yml](.github/workflows/ci.yml)) baut bei
  jedem Push alle Varianten, führt die Host-Tests aus und stellt die `firmware.bin`
  als Artefakt bereit.

## Lizenz

Dieses Projekt steht unter der [Apache License 2.0](LICENSE).
Die mitgelieferte Bibliothek [lib/ArduinoSIP](lib/ArduinoSIP) stammt von
Juergen Liegner und Thorsten Godau, wurde für dieses Projekt erweitert und steht
weiterhin unter der BSD-3-Clause-Lizenz (siehe [NOTICE](NOTICE) und den Kopf von
`ArduinoSIP.h`).

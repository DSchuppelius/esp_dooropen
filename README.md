# esp_dooropen

Türöffner mit ESP8266 **HW-364A** (NodeMCU mit fest verbautem OLED) **oder ESP32
DevKit (WROOM-32)** und Weboberfläche. Die Zielplattform wird über die
PlatformIO-Umgebung gewählt (`nodemcuv2` bzw. `esp32dev`); der Code passt sich per
Präprozessor-Weiche automatisch an.

- **Signalisierung** (2 Adern, potentialfreier Kontakt) wird eingelesen und angezeigt.
- **Summer** (2 Adern) wird über ein Relais für X Sekunden geschaltet, wenn auf
  der Weboberfläche „Öffnen“ gedrückt wird. Dauer ist im Web einstellbar (im EEPROM gespeichert).
- **Onboard-OLED SSD1306 0,96" (I²C)** zeigt IP, Signal- und Summer-Status.
  (Beim ESP32 optional; läuft auch ohne Display.)
- **WiFiManager**: Beim ersten Start öffnet der ESP ein WLAN `Tueroeffner-Setup`
  zum Eintragen der Zugangsdaten.

## Verdrahtung (ESP8266 HW-364A)

Das OLED ist auf dem HW-364A **fest verdrahtet** an D5/D6 und muss nicht angeschlossen werden.

| Funktion            | Pin        | Hinweis                                   |
|---------------------|------------|-------------------------------------------|
| OLED SDA (onboard)  | D5 (GPIO14)| fest verbaut, nicht belegen               |
| OLED SCL (onboard)  | D6 (GPIO12)| fest verbaut, nicht belegen               |
| KY-019 S (Signal)   | D1 (GPIO5) | schaltet das Relais (active-high)         |
| KY-019 + (VCC)      | 5V / VU    | 5V vom USB-Pin (nicht 3V3!)               |
| KY-019 - (GND)      | G          | gemeinsame Masse                          |
| Signal-Kontakt      | D2 (GPIO4) | eine Ader hier, andere Ader an GND        |

Der Türöffner haengt am **Relaiskontakt** (COM + NO) zusammen mit einem
**Klingeltrafo** (8-12 V AC): Trafo -> COM, NO -> Tueroeffner -> zurueck zum Trafo.

**Wichtig:**
- Der **Signal-Eingang** ist für einen *potentialfreien Kontakt* ausgelegt
  (`INPUT_PULLUP`, aktiv = gegen GND). Kommt dort **Fremdspannung** an, muss ein
  **Optokoppler (z. B. PC817)** dazwischen — sonst wird der ESP beschädigt.
- Der **Summer** wird **nicht** direkt vom ESP versorgt. Der ESP schaltet nur das
  Relais, der Summer hat seinen eigenen Stromkreis / sein eigenes Netzteil.
- Ist dein Relais-Modul „active low“ (schaltet bei LOW), passt die Voreinstellung.
  Sonst in `src/config.h` `RELAY_ACTIVE_LOW` auf `false` setzen.

## Verdrahtung (ESP32 DevKit / WROOM-32)

Die ESP8266-Pin-Labels (D1/D2/…) gibt es auf dem ESP32 nicht — hier gelten echte
GPIO-Nummern. Ein OLED ist optional; das Programm läuft auch ohne.

| Funktion            | Pin        | Hinweis                                   |
|---------------------|------------|-------------------------------------------|
| Relais S (Signal)   | GPIO26     | schaltet das Relais (active-high)         |
| Signal-Kontakt      | GPIO27     | eine Ader hier, andere Ader an GND        |
| Status-LED (onboard)| GPIO2      | active-high, spiegelt den Summer-Zustand  |
| OLED SDA (optional) | GPIO21     | nur falls ein Display genutzt wird        |
| OLED SCL (optional) | GPIO22     | nur falls ein Display genutzt wird        |

Alle ESP32-Pins stehen im `#if defined(ESP32)`-Block in [src/config.h](src/config.h)
und lassen sich dort anpassen.

## Bauen & Flashen (PlatformIO)

```powershell
# ESP8266 HW-364A (Standard-Umgebung)
pio run -e nodemcuv2 -t upload

# ESP32 DevKit
pio run -e esp32dev -t upload

pio device monitor -b 115200     # serielle Ausgabe
```

**Hinweis ESP32-Flashen:** Manche ESP32-Boards wechseln nicht automatisch in den
Download-Modus. Falls beim Upload `Wrong boot mode detected (0x13)` erscheint,
den Chip manuell latchen: **BOOT** gedrückt halten → **EN/RST** kurz drücken →
**BOOT** loslassen, dann den Upload starten. Die Umgebung `esp32dev` ist bereits
mit `board_upload.before_reset = no_reset` dafür vorbereitet.

## Bedienung

1. Nach dem ersten Start mit dem WLAN `Tueroeffner-Setup` verbinden und dein
   Heim-WLAN eintragen.
2. Die IP-Adresse erscheint auf dem Display (oder in der seriellen Ausgabe, falls
   kein OLED angeschlossen ist).
3. Im Browser die IP öffnen → „Öffnen“ löst den Summer aus, Dauer einstellbar.

## Anpassen

Alle Pins und Grundeinstellungen stehen in [src/config.h](src/config.h).
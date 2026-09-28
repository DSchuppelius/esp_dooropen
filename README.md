# esp_dooropen

Türöffner mit ESP8266 **HW-364A** (NodeMCU mit fest verbautem OLED) und Weboberfläche.

- **Signalisierung** (2 Adern, potentialfreier Kontakt) wird eingelesen und angezeigt.
- **Summer** (2 Adern) wird über ein Relais für X Sekunden geschaltet, wenn auf
  der Weboberfläche „Öffnen“ gedrückt wird. Dauer ist im Web einstellbar (im EEPROM gespeichert).
- **Onboard-OLED SSD1306 0,96" (I²C)** zeigt IP, Signal- und Summer-Status.
- **WiFiManager**: Beim ersten Start öffnet der ESP ein WLAN `Tueroeffner-Setup`
  zum Eintragen der Zugangsdaten.

## Verdrahtung (HW-364A)

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

## Bauen & Flashen (PlatformIO)

```powershell
pio run                 # kompilieren
pio run -t upload       # per USB flashen
pio device monitor      # serielle Ausgabe (115200 Baud)
```

## Bedienung

1. Nach dem ersten Start mit dem WLAN `Tueroeffner-Setup` verbinden und dein
   Heim-WLAN eintragen.
2. Die IP-Adresse erscheint auf dem Display.
3. Im Browser die IP öffnen → „Öffnen“ löst den Summer aus, Dauer einstellbar.

## Anpassen

Alle Pins und Grundeinstellungen stehen in [src/config.h](src/config.h).
# Steuerplatine (KiCad)

KiCad-10-Projekt für eine eigene Platine mit ESP32-WROOM-32E. Die Pinbelegung entspricht dem ESP32-Zweig in [src/config.h](../src/config.h), es ist also keine Firmware-Änderung nötig.

| Datei | Inhalt |
| --- | --- |
| `esp_dooropen.kicad_pro` / `.kicad_sch` | KiCad-Projekt und Schaltplan |
| `schaltplan.pdf` | Schaltplan als PDF |
| `stueckliste.csv` | Stückliste, gruppiert nach Wert und Footprint |

Stand: nur der Schaltplan. Ein PCB-Layout gibt es noch nicht.

## Funktionsblöcke

- **Versorgung:** 12 V DC über Hohlstecker 5,5/2,1 oder Schraubklemme. Danach PTC 1,1 A, Verpolschutz SS34 und TVS-Diode SMBJ15CA. Ein RECOM R-78E5.0-1.0 erzeugt 5 V, ein AMS1117 daraus 3,3 V. USB-5V ist über eine Schottky-Diode entkoppelt, sodass USB und 12 V gleichzeitig angeschlossen sein dürfen.
- **USB-C:** CH340C mit Auto-Reset über DTR/RTS. Geflasht wird direkt per PlatformIO, ohne Tasten zu drücken. RESET und BOOT gibt es trotzdem als Taster.
- **Relais 1 und 2:** SRD-12VDC-SL-C mit 12-V-Spule, angesteuert über AO3400A mit Freilaufdiode und LED. Die Kontakte NO/COM/NC sind potentialfrei auf Schraubklemmen geführt.
- **Klingelsignal:** Zwei Varianten, beide verdrahtet. Sie wirken als ODER auf GPIO27 und brauchen keine Jumper.
  - Potentialfreier Kontakt an J4, Klemme 1 gegen GND.
  - 8–24 V AC oder DC an J5 über den Optokoppler LTV-814 (PC814). Der Optokoppler hat antiparallele LEDs, die Polarität ist also egal, und er trennt galvanisch. C11 überbrückt die Nulldurchgänge bei Wechselspannung.
- **Taster und Türkontakt:** Je 1 kΩ in Serie, 10 kΩ Pull-up und 100 nF gegen Störungen auf langen Leitungen.
- **Wiegand:** 12-V-Versorgung über PTC 200 mA. D0/D1 laufen über Schottky-Dioden BAT54W, daher sind 5-V-Leser ohne Pegelwandler erlaubt.
- **Erweiterungen:** OLED-Steckplatz (I2C, 3,3 V) und eine Erweiterungsleiste mit IO4, IO14, IO16 und IO17.

## Klemmenbelegung

| Klemme | Pins |
| --- | --- |
| J2 12 V | 1 +12 V, 2 GND |
| J4 Eingänge | 1 Klingel potentialfrei, 2 GND, 3 Klingeltaster, 4 Summertaster, 5 Türkontakt, 6 GND |
| J5 Klingel AC/DC | 1, 2 (8–24 V, beliebige Polarität) |
| J6 Wiegand | 1 +12 V, 2 GND, 3 D0, 4 D1 |
| J9 / J10 Relais | 1 NO, 2 COM, 3 NC |

## Zuordnung der GPIOs

| GPIO | Funktion |
| --- | --- |
| 26 | Relais 1 (Türsummer) |
| 13 | Relais 2 |
| 27 | Klingelsignal |
| 32 | Klingeltaster |
| 33 | Summertaster |
| 25 | Türkontakt |
| 18 / 19 | Wiegand D0 / D1 |
| 21 / 22 | I2C SDA / SCL |
| 2 | Status-LED |

## Hinweise

- **ERC:** Die einzige Meldung lautet „Power output an Power output“ zwischen CH340C-V3 und AMS1117. Das ist gewollt: Laut Datenblatt wird V3 bei 3,3-V-Betrieb mit VCC verbunden.
- **Nur Kleinspannung:** Die Platine ist für Kleinspannung ausgelegt (Klingeltrafo, Türöffner 8–24 V). Für 230 V an den Relaiskontakten fehlen die nötigen Luft- und Kriechstrecken.
- **Antenne:** Beim Layout muss die Antenne des WROOM-Moduls über den Platinenrand ragen. Darunter dürfen weder Kupfer noch Bauteile liegen.
- **Strapping-Pins:** GPIO12, GPIO15 und GPIO5 sind bewusst frei. GPIO0 und EN werden nur vom Auto-Reset und den Tastern benutzt.

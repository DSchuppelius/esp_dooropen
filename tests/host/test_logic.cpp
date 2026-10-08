// Host-Test reiner Logik: Feiertage, Zeitfenster, verpasste Druecke
#include <cstdio>
#include <cstring>
#include "holidays.h"
#include "presswatch.h"
#include "timewin.h"

static int fails = 0, checks = 0;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("FEHLER: %s (Zeile %d)\n", msg, __LINE__); } } while (0)

static bool is(int y, int m, int d, const char *st, const char *expect) {
  const char *n = holidays::name(y, m, d, st);
  if (!expect) return n == nullptr;
  return n && strcmp(n, expect) == 0;
}

// ------------------------------------------------------------
//  Zeitfenster (Praxis-Modus wie in door.cpp: Mo-Fr, Feiertage NW, Ausnahmetag 31.12.)
// ------------------------------------------------------------
static timewin::Day day(int y, int m, int d) {
  return { y, m, d, holidays::weekday(y, m, d) };
}

static bool praxisDayOk(const timewin::Day &d) {
  if (!(0x1F & timewin::dayBit(d.wday))) return false;    // Mo-Fr
  if (d.d == 31 && d.m == 12) return false;                // Ausnahmetag
  return !holidays::name(d.y, d.m, d.d, "NW");
}

static bool praxis(int y, int m, int d, int hh, int mm, int from, int to) {
  return timewin::inWindow(day(y, m, d), hh * 60 + mm, from, to, praxisDayOk);
}

static bool sameDay(const timewin::Day &a, int y, int m, int d, int wday) {
  return a.y == y && a.m == m && a.d == d && a.wday == wday;
}

static void testTimeWindows() {
  // Vortag: Monats-/Jahreswechsel, Schaltjahre, Wochentag
  CHECK(sameDay(timewin::prevDay(day(2026, 1, 1)), 2025, 12, 31, 3), "Vortag 1.1.2026 = Mi 31.12.2025");
  CHECK(sameDay(timewin::prevDay(day(2024, 3, 1)), 2024, 2, 29, 4), "Vortag 1.3.2024 = Do 29.2.");
  CHECK(sameDay(timewin::prevDay(day(2023, 3, 1)), 2023, 2, 28, 2), "Vortag 1.3.2023 = Di 28.2.");
  CHECK(sameDay(timewin::prevDay(day(2100, 3, 1)), 2100, 2, 28, 0), "2100 kein Schaltjahr");
  CHECK(sameDay(timewin::prevDay(day(2000, 3, 1)), 2000, 2, 29, 2), "2000 Schaltjahr");
  CHECK(sameDay(timewin::prevDay(day(2026, 5, 1)), 2026, 4, 30, 4), "Vortag 1.5.2026 = Do 30.4.");
  CHECK(sameDay(timewin::prevDay(day(2026, 10, 12)), 2026, 10, 11, 0), "Vortag Mo = So");
  CHECK(timewin::dayBit(1) == 1 && timewin::dayBit(0) == 64, "Bit 0 = Montag, Bit 6 = Sonntag");

  // inRange (Nachtruhe): Ende exklusiv, Fenster aus bei from == to
  CHECK(timewin::inRange(23 * 60, 22 * 60, 7 * 60) && timewin::inRange(6 * 60 + 59, 22 * 60, 7 * 60), "22-07 nachts");
  CHECK(!timewin::inRange(7 * 60, 22 * 60, 7 * 60) && !timewin::inRange(12 * 60, 22 * 60, 7 * 60), "22-07 tagsueber nicht");
  CHECK(!timewin::inRange(600, 600, 600), "from == to = aus");

  const int N22 = 22 * 60, N02 = 2 * 60, H08 = 8 * 60, H12 = 12 * 60;
  // Fenster am Tag: wie bisher
  CHECK(praxis(2026, 10, 12, 10, 0, H08, H12), "Mo 10:00 in 08-12");
  CHECK(!praxis(2026, 10, 12, 12, 0, H08, H12) && !praxis(2026, 10, 12, 7, 59, H08, H12), "Mo 08-12 Raender");
  CHECK(!praxis(2026, 10, 10, 10, 0, H08, H12), "Sa nicht (Mo-Fr)");
  CHECK(!praxis(2026, 10, 12, 10, 0, H08, H08), "Fenster aus");
  // Ueber Mitternacht: der Teil danach gehoert zum Vortag
  CHECK(praxis(2026, 10, 9, 23, 0, N22, N02), "Fr 23:00 offen");
  CHECK(praxis(2026, 10, 10, 1, 0, N22, N02), "Sa 01:00 = Freitagabend -> offen");
  CHECK(!praxis(2026, 10, 10, 23, 0, N22, N02), "Sa 23:00 zu");
  CHECK(!praxis(2026, 10, 12, 1, 0, N22, N02), "Mo 01:00 = Sonntagabend -> zu");
  CHECK(praxis(2026, 10, 13, 1, 59, N22, N02), "Di 01:59 = Montagabend -> offen");
  CHECK(!praxis(2026, 10, 13, 2, 0, N22, N02), "Di 02:00 Ende");
  CHECK(!praxis(2026, 10, 10, 3, 0, N22, N02), "Sa 03:00 ausserhalb");
  // Feiertage und Ausnahmetage des Vortags
  CHECK(!praxis(2026, 5, 2, 1, 0, N22, N02), "Sa 2.5. 01:00: Vortag 1.5. Feiertag");
  CHECK(!praxis(2026, 4, 7, 1, 0, N22, N02), "Di 7.4. 01:00: Vortag Ostermontag");
  CHECK(praxis(2026, 4, 8, 1, 0, N22, N02), "Mi 8.4. 01:00: Vortag normaler Dienstag");
  CHECK(!praxis(2026, 4, 6, 23, 0, N22, N02), "Ostermontag 23:00 zu");
  CHECK(praxis(2026, 4, 7, 23, 0, N22, N02), "Di 7.4. 23:00 offen (Feiertag war gestern)");
  CHECK(!praxis(2026, 1, 1, 1, 0, N22, N02), "1.1. 01:00: Vortag 31.12. ist Ausnahmetag");
  CHECK(!praxis(2026, 1, 2, 1, 0, N22, N02), "Fr 2.1. 01:00: Vortag Neujahr");
  auto newYearsEve = [](const timewin::Day &d) { return d.y == 2024 && d.m == 12 && d.d == 31 && d.wday == 2; };
  CHECK(timewin::inWindow(day(2025, 1, 1), 60, N22, N02, newYearsEve), "1.1.2025 01:00 gehoert zu Di 31.12.2024");

  // Benutzer-Zeitfenster: nur freitags 22-02 -> Samstag frueh erlaubt, Freitag frueh nicht
  auto friOnly = [](const timewin::Day &d) { return (timewin::dayBit(d.wday) & (1 << 4)) != 0; };
  CHECK(timewin::inWindow(day(2026, 10, 10), 60, N22, N02, friOnly), "Benutzer Fr 22-02: Sa 01:00");
  CHECK(!timewin::inWindow(day(2026, 10, 9), 60, N22, N02, friOnly), "Benutzer Fr 22-02: Fr 01:00 nicht");
}

// ------------------------------------------------------------
//  Verpasste Druecke (presswatch.h), Entprellzeit 50 ms
// ------------------------------------------------------------
static const uint32_t DB = 50;

// Phase aktiv von..bis (Flanken), dazwischen keine Abfrage (Loop blockiert)
static void press(presswatch::Watch &w, uint32_t from, uint32_t to) {
  presswatch::edge(w, true, from, DB);
  presswatch::edge(w, false, to, DB);
}

static void testPressWatch() {
  {  // Druck komplett waehrend blockierter Loop -> einmal nachgemeldet
    presswatch::Watch w{};
    press(w, 1000, 1200);
    CHECK(!presswatch::poll(w, false, 1220, DB), "erst nach der Entprellzeit melden");
    CHECK(presswatch::poll(w, false, 1260, DB), "verpasster Druck");
    CHECK(!presswatch::poll(w, false, 1300, DB) && !presswatch::poll(w, false, 5000, DB), "nur einmal");
  }
  {  // Polling hat den Druck erkannt -> nicht doppelt
    presswatch::Watch w{};
    presswatch::edge(w, true, 1000, DB);
    CHECK(!presswatch::poll(w, true, 1060, DB), "Polling erkennt den Druck");
    presswatch::edge(w, false, 1200, DB);
    CHECK(!presswatch::poll(w, false, 1300, DB), "keine Doppelzaehlung");
  }
  {  // Prellen beim Druecken und Loslassen: eine Phase
    presswatch::Watch w{};
    press(w, 1000, 1001); press(w, 1003, 1004); press(w, 1006, 1300); press(w, 1302, 1303);
    CHECK(presswatch::poll(w, false, 1400, DB), "prellender Druck: gemeldet");
    CHECK(!presswatch::poll(w, false, 1500, DB), "prellender Druck: nur einmal");
  }
  {  // Polling erkennt den Druck, Prell-Luecke >= Entprellzeit sieht nur der Interrupt:
     // solange das Polling aktiv sieht, gehoert auch die zweite Phase dazu
    presswatch::Watch w{};
    presswatch::edge(w, true, 1000, DB);
    CHECK(!presswatch::poll(w, true, 1060, DB), "erkannt");
    presswatch::edge(w, false, 1100, DB);
    presswatch::edge(w, true, 1155, DB);   // Luecke 55 ms, Polling hat sie nicht gesehen
    CHECK(!presswatch::poll(w, true, 1200, DB), "zweite Phase vom Polling gesehen");
    presswatch::edge(w, false, 1400, DB);
    CHECK(!presswatch::poll(w, false, 1500, DB), "keine Doppelzaehlung nach Luecke");
  }
  {  // Wechselspannung (10 ms an, 10 ms aus) und Stoerspitzen: nie gemeldet
    presswatch::Watch w{};
    for (uint32_t t = 1000; t < 2000; t += 20) press(w, t, t + 10);
    CHECK(!presswatch::poll(w, false, 2100, DB), "Wechselspannung nur ueber das Polling");
    press(w, 3000, 3002); press(w, 3030, 3031); press(w, 3060, 3062);
    CHECK(!presswatch::poll(w, false, 3200, DB), "Stoerspitzen");
  }
  {  // Zwei Druecke waehrend einer Blockade: mindestens einer wird gemeldet
    presswatch::Watch w{};
    press(w, 1000, 1200); press(w, 1500, 1700);
    CHECK(presswatch::poll(w, false, 2000, DB), "zwei Druecke: gemeldet");
    CHECK(!presswatch::poll(w, false, 2100, DB), "zwei Druecke: eine Meldung");
  }
  {  // Eingang beim Start schon aktiv: keine Flanke -> Loslassen meldet nichts
    presswatch::Watch w{};
    presswatch::edge(w, false, 5000, DB);
    CHECK(!presswatch::poll(w, false, 6000, DB), "aktiv beim Start");
  }
  {  // Verwerfen (Notfall-Reset): langer Druck wird nicht nachgemeldet
    presswatch::Watch w{};
    press(w, 1000, 7000);
    presswatch::reset(w);
    CHECK(!presswatch::poll(w, false, 8000, DB), "verworfen");
  }
  {  // Polling sah den alten Druck noch aktiv, war aber blockiert: neuer Druck zaehlt
    presswatch::Watch w{};
    presswatch::edge(w, true, 1000, DB);
    CHECK(!presswatch::poll(w, true, 1060, DB), "erster Druck vom Polling");
    presswatch::edge(w, false, 1200, DB);
    press(w, 2000, 2300);                  // Loop blockiert, Eingang beim naechsten Poll frei
    CHECK(presswatch::poll(w, false, 3000, DB), "zweiter Druck nachgemeldet");
  }
  {  // millis-Ueberlauf waehrend eines Drucks
    presswatch::Watch w{};
    press(w, 0xFFFFFF00u, 0x00000100u);
    CHECK(presswatch::poll(w, false, 0x00000200u, DB), "Ueberlauf");
  }
}

int main() {
  testTimeWindows();
  testPressWatch();

  // Ostersonntag bekannter Jahre (Tag im Jahr -> Datum ueber Karfreitag/Ostermontag geprueft)
  CHECK(is(2024, 3, 29, "NW", "Karfreitag"), "Karfreitag 2024");
  CHECK(is(2024, 4, 1, "NW", "Ostermontag"), "Ostermontag 2024");
  CHECK(is(2025, 4, 18, "BY", "Karfreitag"), "Karfreitag 2025");
  CHECK(is(2025, 4, 21, "BY", "Ostermontag"), "Ostermontag 2025");
  CHECK(is(2026, 4, 3, "HE", "Karfreitag"), "Karfreitag 2026");
  CHECK(is(2026, 4, 6, "HE", "Ostermontag"), "Ostermontag 2026");
  CHECK(is(2027, 3, 26, "SH", "Karfreitag"), "Karfreitag 2027");
  CHECK(is(2038, 4, 26, "NI", "Ostermontag"), "Ostermontag 2038 (spaetestes Ostern 25.4.)");
  CHECK(is(2285, 3, 23, "NI", "Ostermontag") && is(2285, 3, 20, "NI", "Karfreitag"), "Ostern 22.3.2285 (fruehestes)");
  // bewegliche Feiertage 2026
  CHECK(is(2026, 5, 14, "BE", "Christi Himmelfahrt"), "Himmelfahrt 2026");
  CHECK(is(2026, 5, 25, "BE", "Pfingstmontag"), "Pfingstmontag 2026");
  CHECK(is(2026, 6, 4, "NW", "Fronleichnam"), "Fronleichnam 2026 NW");
  CHECK(is(2026, 6, 4, "BE", nullptr), "kein Fronleichnam in Berlin");
  CHECK(is(2026, 5, 24, "BB", "Pfingstsonntag"), "Pfingstsonntag BB");
  CHECK(is(2026, 4, 5, "BB", "Ostersonntag"), "Ostersonntag BB");
  CHECK(is(2026, 4, 5, "BY", nullptr), "Ostersonntag nur BB");
  // feste Feiertage
  CHECK(is(2026, 1, 1, "HH", "Neujahr"), "Neujahr");
  CHECK(is(2026, 10, 3, "SN", "Tag der Deutschen Einheit"), "3. Oktober");
  CHECK(is(2026, 12, 26, "SL", "2. Weihnachtstag"), "26.12.");
  CHECK(is(2026, 1, 6, "BW", "Heilige Drei Könige"), "6.1. BW");
  CHECK(is(2026, 1, 6, "NW", nullptr), "6.1. nicht NW");
  CHECK(is(2026, 3, 8, "BE", "Internationaler Frauentag"), "Frauentag BE");
  CHECK(is(2022, 3, 8, "MV", nullptr) && is(2023, 3, 8, "MV", "Internationaler Frauentag"), "Frauentag MV ab 2023");
  CHECK(is(2026, 8, 15, "SL", "Mariä Himmelfahrt") && is(2026, 8, 15, "BY", nullptr), "15.8. nur SL");
  CHECK(is(2026, 9, 20, "TH", "Weltkindertag"), "Weltkindertag TH");
  CHECK(is(2026, 10, 31, "NI", "Reformationstag") && is(2017, 10, 31, "NI", nullptr), "Reformationstag NI ab 2018");
  CHECK(is(2026, 10, 31, "SN", "Reformationstag") && is(2026, 10, 31, "BW", nullptr), "Reformationstag SN, nicht BW");
  CHECK(is(2026, 11, 1, "RP", "Allerheiligen") && is(2026, 11, 1, "HE", nullptr), "Allerheiligen");
  // Buss- und Bettag: 18.11.2026, 22.11.2023, 16.11.2022
  CHECK(is(2026, 11, 18, "SN", "Buß- und Bettag"), "Bettag 2026");
  CHECK(is(2023, 11, 22, "SN", "Buß- und Bettag"), "Bettag 2023");
  CHECK(is(2022, 11, 16, "SN", "Buß- und Bettag"), "Bettag 2022");
  CHECK(is(2026, 11, 18, "BY", nullptr), "Bettag nur SN");
  // normale Tage, kein Land
  CHECK(is(2026, 7, 14, "BY", nullptr), "normaler Tag");
  CHECK(is(2026, 1, 1, "", nullptr), "ohne Land kein Feiertag");
  CHECK(is(2026, 1, 1, "XX", "Neujahr"), "unbekanntes Land: nur bundesweit");

  printf("Logik: %d Pruefungen, %d Fehler\n", checks, fails);
  return fails ? 1 : 0;
}

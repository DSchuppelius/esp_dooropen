// Host-Test reiner Logik: Feiertage
#include <cstdio>
#include <cstring>
#include "holidays.h"

static int fails = 0, checks = 0;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("FEHLER: %s (Zeile %d)\n", msg, __LINE__); } } while (0)

static bool is(int y, int m, int d, const char *st, const char *expect) {
  const char *n = holidays::name(y, m, d, st);
  if (!expect) return n == nullptr;
  return n && strcmp(n, expect) == 0;
}

int main() {
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

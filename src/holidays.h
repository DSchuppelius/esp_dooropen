#pragma once
// Gesetzliche Feiertage in Deutschland je Bundesland (ohne Abhaengigkeiten,
// auf dem PC testbar: tests/host/test_logic.cpp).
//
// Laender-Kuerzel: BW BY BE BB HB HH HE MV NI NW RP SL SN ST SH TH
// Nicht beruecksichtigt: nur oertlich geltende Feiertage (z.B. Augsburger
// Friedensfest, Fronleichnam in Teilen von Sachsen/Thueringen, Mariae
// Himmelfahrt in Teilen Bayerns).
#include <string.h>

namespace holidays {

// Tag im Jahr (1..366)
inline int dayOfYear(int y, int m, int d) {
  static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  return cum[m - 1] + d + (leap && m > 2 ? 1 : 0);
}

// Ostersonntag (Gausssche Osterformel, Ergaenzung nach Lichtenberg)
inline int easterDayOfYear(int y) {
  int k = y / 100;
  int m = 15 + (3 * k + 3) / 4 - (8 * k + 13) / 25;
  int s = 2 - (3 * k + 3) / 4;
  int a = y % 19;
  int d = (19 * a + m) % 30;
  int r = (d + a / 11) / 29;
  int og = 21 + d - r;
  int sz = 7 - (y + y / 4 + s) % 7;
  int oe = 7 - (og - sz) % 7;
  int os = og + oe;                     // Tag im Maerz (kann > 31 sein = April)
  return dayOfYear(y, 3, 1) - 1 + os;
}

// Wochentag 0 = Sonntag (Sakamoto)
inline int weekday(int y, int m, int d) {
  static const int t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

inline bool in(const char *state, const char *list) {
  return state && state[0] && strstr(list, state) != nullptr;
}

// Name des Feiertags am Datum fuer das Bundesland, sonst nullptr
inline const char *name(int y, int m, int d, const char *state) {
  if (!state || strlen(state) != 2) return nullptr;
  int doy = dayOfYear(y, m, d);
  int e   = easterDayOfYear(y);

  // bundesweit
  if (m == 1 && d == 1)   return "Neujahr";
  if (doy == e - 2)       return "Karfreitag";
  if (doy == e + 1)       return "Ostermontag";
  if (m == 5 && d == 1)   return "Tag der Arbeit";
  if (doy == e + 39)      return "Christi Himmelfahrt";
  if (doy == e + 50)      return "Pfingstmontag";
  if (m == 10 && d == 3)  return "Tag der Deutschen Einheit";
  if (m == 12 && d == 25) return "1. Weihnachtstag";
  if (m == 12 && d == 26) return "2. Weihnachtstag";

  // je Land
  if (m == 1 && d == 6 && in(state, "BW BY ST"))                      return "Heilige Drei Könige";
  if (m == 3 && d == 8 && (in(state, "BE") || (in(state, "MV") && y >= 2023)))
                                                                      return "Internationaler Frauentag";
  if (doy == e && in(state, "BB"))                                    return "Ostersonntag";
  if (doy == e + 49 && in(state, "BB"))                               return "Pfingstsonntag";
  if (doy == e + 60 && in(state, "BW BY HE NW RP SL"))                return "Fronleichnam";
  if (m == 8 && d == 15 && in(state, "SL"))                           return "Mariä Himmelfahrt";
  if (m == 9 && d == 20 && in(state, "TH") && y >= 2019)              return "Weltkindertag";
  if (m == 10 && d == 31 && (in(state, "BB MV SN ST TH") ||
                             (in(state, "HB HH NI SH") && y >= 2018))) return "Reformationstag";
  if (m == 11 && d == 1 && in(state, "BW BY NW RP SL"))               return "Allerheiligen";
  // Buss- und Bettag: Mittwoch vor dem 23. November
  if (m == 11 && in(state, "SN") && d >= 16 && d <= 22 && weekday(y, m, d) == 3)
                                                                      return "Buß- und Bettag";
  return nullptr;
}

}  // namespace holidays

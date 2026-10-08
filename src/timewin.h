#pragma once
// Zeitfenster (Nachtruhe, Praxis-Modus, Benutzer) ohne Abhaengigkeiten, auf dem
// PC testbar: tests/host/test_logic.cpp
#include <stdint.h>

namespace timewin {

struct Day { int y, m, d, wday; };   // wday: 0 = Sonntag (wie struct tm)

// Bit des Wochentags in den Tage-Masken (Bit 0 = Montag)
inline uint8_t dayBit(int wday) { return 1 << ((wday + 6) % 7); }

inline bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

// Vortag (Monats- und Jahreswechsel, Schaltjahr)
inline Day prevDay(Day t) {
  static const uint8_t len[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  t.wday = (t.wday + 6) % 7;
  if (--t.d >= 1) return t;
  if (--t.m < 1) { t.m = 12; t.y--; }
  t.d = len[t.m - 1] + (t.m == 2 && leap(t.y) ? 1 : 0);
  return t;
}

// m im Fenster from..to (Minuten)? from == to = Fenster aus. Ueber Mitternacht moeglich.
inline bool inRange(uint16_t m, uint16_t from, uint16_t to) {
  if (from == to) return false;
  if (from < to) return m >= from && m < to;
  return m >= from || m < to;   // z.B. 22:00-07:00
}

// Wie inRange, zusaetzlich muss der Tag passen (dayOk: Wochentag, Feier- und
// Ausnahmetage). Der Teil nach Mitternacht eines Fensters wie 22:00-02:00 gehoert
// zum Vortag: Samstag 01:00 ist dann noch der Freitagabend.
template <typename F> bool inWindow(const Day &t, uint16_t m, uint16_t from, uint16_t to, F dayOk) {
  if (!inRange(m, from, to)) return false;
  return from > to && m < to ? dayOk(prevDay(t)) : dayOk(t);
}

}  // namespace timewin

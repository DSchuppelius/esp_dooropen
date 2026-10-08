#pragma once
// Verpasste Druecke (Klingel, Summer-Taster) ohne Abhaengigkeiten, auf dem PC
// testbar: tests/host/test_logic.cpp
//
// Die Eingaenge werden gepollt. Haengt die Loop (Netzwerk, Flash), gingen kurze
// Druecke verloren. Darum misst ein Interrupt (edge) die aktiven Strecken mit; eine
// durchgehend aktive Strecke >= Entprellzeit, die das Polling nicht erkannt hat,
// meldet poll() einmal nach. Luecken < Entprellzeit (Prellen) gehoeren zur selben
// Phase. Klingeln an Wechselspannung (Halbwellen ~10 ms) und Stoerspitzen erreichen
// die Entprellzeit nie am Stueck - sie laufen weiter nur ueber das Polling. Ist ein
// Eingang beim Start schon aktiv, gibt es keine Flanke und damit keine Phase.
#include <stdint.h>
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

namespace presswatch {

struct Watch {
  volatile uint32_t since;     // Beginn der laufenden aktiven Strecke (0 = inaktiv)
  volatile uint32_t lastOff;   // Ende der letzten aktiven Strecke
  volatile bool     longOn;    // Phase hatte eine Strecke >= Entprellzeit
  volatile bool     seen;      // ... und das Polling hat sie erkannt
  volatile bool     missed;    // abgeschlossene Phase, die das Polling verpasst hat
};

// Interrupt (CHANGE): active = Eingang jetzt aktiv
static inline void IRAM_ATTR edge(Watch &w, bool active, uint32_t now, uint32_t debounce) {
  if (active) {
    if (w.since) return;                    // keine neue Flanke
    if (now - w.lastOff >= debounce) {      // vorige Phase ist abgeschlossen
      if (w.longOn && !w.seen) w.missed = true;
      w.longOn = false;
      w.seen   = false;
    }
    w.since = now | 1;
  } else if (w.since) {
    if (now - w.since >= debounce) w.longOn = true;
    w.since   = 0;
    w.lastOff = now;
  }
}

// Aus der Loop, mit gesperrten Interrupts. seenNow: das Polling hat den Druck
// erkannt und der Eingang ist gerade aktiv. true = verpasster Druck (einmal).
static inline bool poll(Watch &w, bool seenNow, uint32_t now, uint32_t debounce) {
  if (seenNow) w.seen = true;
  // Phase abgeschlossen (seit der Entprellzeit inaktiv)?
  if (!w.since && w.longOn && now - w.lastOff >= debounce) {
    if (!w.seen) w.missed = true;
    w.longOn = false;
  }
  bool hit = w.missed;
  w.missed = false;
  return hit;
}

// Bisherige Phasen verwerfen (mit gesperrten Interrupts)
static inline void reset(Watch &w) {
  w.longOn = false;
  w.seen   = false;
  w.missed = false;
}

}  // namespace presswatch

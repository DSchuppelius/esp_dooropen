#pragma once
// Minimale Arduino-Stubs fuer Host-Tests (g++ auf dem PC)
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cmath>
#include <strings.h>
#include <string>
#include <deque>
#include <vector>
#include <utility>

#ifndef PI
#define PI 3.14159265358979323846
#endif

extern uint32_t g_millis;
inline uint32_t millis() { return g_millis; }
inline void delay(uint32_t ms) { g_millis += ms; }

extern uint32_t g_random;
inline uint32_t secureRandom(uint32_t) { return g_random++; }

class IPAddress {
 public:
  uint32_t v = 0;
  IPAddress(uint32_t x = 0) : v(x) {}
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : v(a | (b << 8) | (c << 16) | ((uint32_t)d << 24)) {}
  bool fromString(const char *s) {
    unsigned a, b, c, d; char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    v = a | (b << 8) | (c << 16) | (d << 24);
    return true;
  }
  operator uint32_t() const { return v; }
  bool operator==(const IPAddress &o) const { return v == o.v; }
  bool operator!=(const IPAddress &o) const { return v != o.v; }
};

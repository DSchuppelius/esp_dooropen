#pragma once
#include "Arduino.h"
// DNS-Ersatz: liefert addr (bzw. scheitert mit fail); lookups zaehlt die Anfragen
struct WiFiStub {
  IPAddress addr = IPAddress(10, 0, 0, 1);
  bool fail = false;
  int lookups = 0;
  int hostByName(const char *, IPAddress &a) { lookups++; if (fail) return 0; a = addr; return 1; }
};
extern WiFiStub WiFi;

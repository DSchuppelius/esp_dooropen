#pragma once
#include "Arduino.h"
struct WiFiStub {
  int hostByName(const char *, IPAddress &a) { a = IPAddress(10, 0, 0, 1); return 1; }
};
extern WiFiStub WiFi;

#pragma once
#include "Arduino.h"

struct Packet { IPAddress from; std::string data; };

// UDP-Ersatz: eingehende Pakete aus rx, gesendete landen in sent
class WiFiUDP {
 public:
  std::deque<Packet> rx;
  std::vector<std::string> sent;
  std::string cur, out;
  IPAddress curFrom;
  int port = 0;
  uint8_t begin(uint16_t p) { port = p; return 1; }
  // Wie der ESP32: ein neues Paket gibt es erst, wenn das vorige ganz gelesen
  // ist; laengere Pakete schneidet recvfrom() bei 1460 Bytes ab.
  int parsePacket() {
    if (!cur.empty() || rx.empty()) return 0;
    cur = rx.front().data.substr(0, 1460); curFrom = rx.front().from; rx.pop_front();
    return (int)cur.size();
  }
  int available() { return (int)cur.size(); }
  int read(char *buf, size_t len) {
    size_t n = cur.size() < len ? cur.size() : len;
    memcpy(buf, cur.data(), n);
    cur.erase(0, n);
    return (int)n;
  }
  int read(uint8_t *buf, size_t len) { return read((char *)buf, len); }
  IPAddress remoteIP() { return curFrom; }
  uint16_t remotePort() { return 5060; }
  int beginPacket(const char *, uint16_t) { out.clear(); return 1; }
  int beginPacket(IPAddress, uint16_t) { out.clear(); return 1; }
  size_t write(const uint8_t *b, size_t n) { out.append((const char *)b, n); return n; }
  int endPacket() { sent.push_back(out); return 1; }
};

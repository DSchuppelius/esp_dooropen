// Netzwerk-Abstraktion: WLAN (Standard) oder Ethernet (WT32-ETH01, USE_ETHERNET).
// Alle Module fragen hierueber Verbindung und Adressen ab.
#include "app.h"
#if defined(USE_ETHERNET)
  #include <ETH.h>
#endif

#if defined(USE_ETHERNET)
bool      netUp()         { return ETH.linkUp() && (uint32_t)ETH.localIP() != 0; }
IPAddress netIP()         { return ETH.localIP(); }
IPAddress netMask()       { return ETH.subnetMask(); }
IPAddress netGateway()    { return ETH.gatewayIP(); }
IPAddress netDns()        { return ETH.dnsIP(); }
String    netMac()        { return ETH.macAddress(); }
int       netRssi()       { return 0; }
bool      netIsEthernet() { return true; }
#else
bool      netUp()         { return WiFi.status() == WL_CONNECTED; }
IPAddress netIP()         { return WiFi.localIP(); }
IPAddress netMask()       { return WiFi.subnetMask(); }
IPAddress netGateway()    { return WiFi.gatewayIP(); }
IPAddress netDns()        { return WiFi.dnsIP(); }
String    netMac()        { return WiFi.macAddress(); }
int       netRssi()       { return WiFi.RSSI(); }
bool      netIsEthernet() { return false; }
#endif

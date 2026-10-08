/* ====================================================================

   Copyright (c) 2018 Juergen Liegner  All rights reserved.
   (https://www.mikrocontroller.net/topic/444994)

   Copyright (c) 2019 Thorsten Godau (dl9sec)
   (Created an Arduino library from the original code and did some beautification)

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   1. Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

   2. Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the
      distribution.

   3. Neither the name of the author(s) nor the names of any contributors
      may be used to endorse or promote products derived from this software
      without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) AND CONTRIBUTORS "AS IS" AND
   ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
   IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
   ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR(S) OR CONTRIBUTORS BE LIABLE
   FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
   DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
   OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
   HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
   LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
   OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
   SUCH DAMAGE.

   ====================================================================*/
#include <MD5Builder.h>
#include <math.h>
#if defined(ESP32)
  #include <WiFi.h>
#else
  #include <ESP8266WiFi.h>
#endif

#include "ArduinoSIP.h"

#ifndef PROGMEM
  #define PROGMEM
#endif
#ifndef pgm_read_byte
  #define pgm_read_byte(a) (*(const uint8_t *)(a))
#endif

// Groesstes SIP-Paket: ein UDP-Datagramm, das der ESP32 noch am Stueck sendet
// und empfaengt (write() teilt groessere, recvfrom() schneidet ab)
static const size_t SIP_MAX_UDP = 1460;

/////////////////////////////////////////////////////////////////////////////////////////////////////
//
// Hardware and API independent Sip class
//
/////////////////////////////////////////////////////////////////////////////////////////////////////

Sip::Sip(char *pBuf, size_t lBuf) {

  pbuf = pBuf;
  lbuf = lBuf;
  pDialNr = "";
  pDialDesc = "";
  pSipIp = "";
  pSipUser = "";
  pSipPassWd = "";
  pMyIp = "";
  caRead[0] = 0;
  caInCaller[0] = 0;
  caRingCaller[0] = 0;
  caLastCaller[0] = 0;
}


Sip::~Sip() {

}


void Sip::Init(const char *SipIp, int SipPort, const char *MyIp, int MyPort, const char *SipUser, const char *SipPassWd, int MaxDialSec) {

  Udp.begin(MyPort);

  caRead[0] = 0;
  pbuf[0] = 0;
  pSipIp = SipIp;
  iSipPort = SipPort;
  pSipUser = SipUser;
  pSipPassWd = SipPassWd;
  pMyIp = MyIp;
  iMyPort = MyPort;
  iAuthCnt = 0;
  StopCall();
  iRingTime = 0;
  bIncoming = false;
  bInAckPending = false;
  reqType = REQ_NONE;    // nichts mit alten Daten wiederholen
  iMaxTime = MaxDialSec * 1000;
  bRegistered = false;   // neue Zugangsdaten -> neu registrieren
  bRegPending = false;
  iRegStatus = -1;
  iRegGranted = 0;
  regCallId = 0;         // neue Registrierung (neue Call-ID)

  // Server-Adresse (Ziel aller Pakete, Absenderpruefung). Ein Hostname wird erst
  // in StartRegister() aufgeloest (DNS blockiert die Loop, ESP8266 bis 10 s).
  IPAddress a;
  sipAddr  = IPAddress((uint32_t)0);
  bSipHost = false;
  if ( SipIp[0] && a.fromString(SipIp) )
    sipAddr = a;
  else if ( SipIp[0] )
    bSipHost = true;
}


// Hostname der Anlage aufloesen: einmal je REGISTER-Zyklus statt bei jedem Paket,
// so folgt der Client auch einem DHCP-Wechsel der Anlage. Waehrend eines Anrufs
// nicht (DNS blockiert, Ton/DTMF stocken), dann gilt die letzte Adresse. Scheitert
// die Aufloesung, bleibt die letzte bekannte Adresse; ohne eine wird nichts gesendet
// und keine Anfrage angenommen - der naechste REGISTER-Versuch probiert es erneut.
void Sip::ResolveServer() {

  if ( !bSipHost || (iRingTime && (uint32_t)sipAddr != 0) )
    return;
  IPAddress a;
#if defined(ESP8266)
  int ok = WiFi.hostByName(pSipIp, a, 2000);   // statt 10 s Standard-Wartezeit
#else
  int ok = WiFi.hostByName(pSipIp, a);
#endif
  if ( ok == 1 && (uint32_t)a != 0 )
    sipAddr = a;
}


// Nicht-blockierendes REGISTER mit Digest-Auth (Erweiterung ggue. Original).
// Sendet REGISTER; die Antworten (401/407 -> erneut mit Digest, 200 OK) werden
// in Processing() ausgewertet, sodass die Loop dabei weiterlaeuft.
// Erneuerungen behalten Call-ID und Tag (eine Bindung beim Registrar).
void Sip::StartRegister(int Expires) {

  if ( pSipIp[0] == 0 )
  {
    iRegStatus = -1;
    return;
  }
  ResolveServer();
  if ( (uint32_t)sipAddr == 0 )
  {
    iRegStatus = -2;   // Adresse unbekannt, spaeter erneut versuchen
    return;
  }
  iRegExpires  = Expires;
  if ( regCallId == 0 )
  {
    regCallId  = Random();
    regTag     = Random();
  }
  iRegCSeq++;
  regBranch     = Random();
  regSends      = 0;
  bRegAuthTried = false;
  bRegPending   = true;
  SendRegister(false);
}


// REGISTER senden, auf Wunsch mit dem Header aus caRegAuth. Wiederholungen
// (regSends > 0) sind byte-gleich: gleicher Branch, gleiche CSeq.
void Sip::SendRegister(bool withAuth) {

  pbuf[0] = 0;
  AddSipLine("REGISTER sip:%s SIP/2.0", pSipIp);
  AddSipLine("Via: SIP/2.0/UDP %s:%i;branch=z9hG4bK%010u;rport", pMyIp, iMyPort, regBranch);
  AddSipLine("Max-Forwards: 70");
  AddSipLine("From: <sip:%s@%s>;tag=%010u", pSipUser, pSipIp, regTag);
  AddSipLine("To: <sip:%s@%s>", pSipUser, pSipIp);
  AddSipLine("Call-ID: %010u@%s", regCallId, pMyIp);
  AddSipLine("CSeq: %i REGISTER", iRegCSeq);
  AddSipLine("Contact: <sip:%s@%s:%i;transport=udp>", pSipUser, pMyIp, iMyPort);
  if ( withAuth )
    AddSipLine("%s", caRegAuth);
  AddSipLine("Allow: INVITE, ACK, CANCEL, BYE, OPTIONS, INFO");
  AddSipLine("Expires: %i", iRegExpires);
  AddSipLine("Content-Length: 0");
  AddSipLine("");
  SendUdp();
  regSentAt = millis();
  if ( regSends == 0 )
  {
    regTxAt     = regSentAt;
    regInterval = 500;                    // T1, dann 1 s, 2 s
  }
  else if ( regInterval < 2000 )
    regInterval *= 2;
  regSends++;
}


void Sip::HandleRegisterResponse(const char *p) {

  // verspaetete Antwort einer frueheren Anfrage -> ignorieren
  if ( !bRegPending || !IsCallId(p, regCallId) || GrepInteger(p, "\nCSeq: ") != iRegCSeq )
    return;

  int code = atoi(p + 8);

  if ( code < 200 )   // 1xx: weiter warten
    return;

  if ( code == 200 )
  {
    bRegistered = true;
    bRegPending = false;
    iRegStatus  = 200;
    // Vom Registrar gewaehrte Dauer (kann kuerzer sein als angefragt)
    int exp = GrepInteger(p, ";expires=");
    if ( exp <= 0 )
      exp = GrepInteger(p, "\nExpires: ");
    iRegGranted = exp > 0 ? exp : 0;
    // Gueltig ab der ersten Sendung dieser Anfrage (eher zu kurz als zu lang)
    regOkAt    = regTxAt;
    regValidMs = (uint32_t)(iRegGranted > 0 ? iRegGranted : iRegExpires) * 1000UL;
    return;
  }

  // Nur einmal mit Digest antworten; erneute Aufforderung = Zugangsdaten falsch
  if ( (code == 401 || code == 407) && !bRegAuthTried )
  {
    char uri[64];
    snprintf(uri, sizeof(uri), "sip:%s", pSipIp);
    if ( BuildAuth(p, "REGISTER", uri, caRegAuth, sizeof(caRegAuth)) )
    {
      bRegAuthTried = true;
      iRegCSeq++;
      regBranch = Random();   // neue Transaktion
      regSends  = 0;
      SendRegister(true);
      return;
    }
  }

  // Jede andere Endantwort = fehlgeschlagen; naechster Versuch mit neuer Call-ID.
  // Eine bestehende Anmeldung gilt beim Registrar bis zu ihrem Ablauf weiter.
  bRegPending = false;
  iRegStatus  = code;
  regCallId   = 0;
}


bool Sip::Dial(const char *DialNr, const char *DialDesc) {

  if ( iRingTime )
    return false;

  bAnswered = false;
  bIncoming = false;
  eCallResult = CALL_NONE;
  iCallCode = 0;
  cLastDtmf = 0;
  pDialNr = DialNr;
  pDialDesc = DialDesc;
  iRingTime = Millis();
  // Ohne Server-Adresse oder zu gross: gar nicht erst warten, sondern gleich als
  // gescheitert melden (Code wie eine SIP-Antwort, z.B. 503 / 513)
  int err = Invite();
  if ( err != 0 && err != 500 )
  {
    StopCall();
    iRingTime = 0;
    eCallResult = CALL_FAILED;
    iCallCode = err;
  }

  return true;
}


void Sip::Processing(char *pBuf, size_t lBuf) {

  int packetSize = Udp.parsePacket();
  bool fromServer = false;
  bool truncated = false;
  int bodyAt = -1;

  if ( packetSize > 0 )
  {
    // Anfragen (INVITE, BYE, INFO, ...) nur von der Anlage annehmen. Ist ihre
    // Adresse (noch) unbekannt, keine - nicht still jeden Absender zulassen.
    fromServer = (uint32_t)sipAddr != 0 && Udp.remoteIP() == sipAddr;
    pBuf[0] = 0;
    packetSize = Udp.read(pBuf, lBuf - 1);   // Platz fuer die Null am Ende
    // Rest eines zu grossen Pakets verwerfen (ESP32: sonst kommt kein neues mehr)
    char junk[64];
    while ( Udp.available() > 0 && Udp.read(junk, sizeof(junk)) > 0 )
      truncated = true;
    if ( packetSize > 0 )
    {
      pBuf[packetSize] = 0;

#ifdef DEBUGLOG
      IPAddress remoteIp = Udp.remoteIP();
      Serial.printf("\r\n----- read %i bytes from: %s:%i ----\r\n", (int)packetSize, remoteIp.toString().c_str(), Udp.remotePort());
      Serial.print(pBuf);
      Serial.printf("----------------------------------------------------\r\n");
#endif

      // Kopfzeilen vereinheitlichen; passt das Ergebnis nicht -> verwerfen
      packetSize = Normalize(pBuf, packetSize, lBuf, &bodyAt);
      // Content-Length groesser als der empfangene Body = abgeschnitten (der ESP32
      // kuerzt Datagramme ueber 1460 Bytes ohne Meldung)
      int cl = packetSize > 0 ? GrepInteger(pBuf, "\nContent-Length: ") : -1;
      if ( cl > 0 && cl > packetSize - (bodyAt >= 0 ? bodyAt : packetSize) )
        truncated = true;
      // ohne vollstaendige Kopfzeilen nicht auswerten
      if ( truncated && bodyAt < 0 )
        packetSize = 0;
    }
  }

  // Erst das Paket auswerten, dann die Zeitgeber: eine Antwort, die im selben
  // Durchlauf wie der Zeitablauf kommt, zaehlt noch
  if ( packetSize > 0 )
    HandleUdpPacket(pBuf, fromServer, truncated);
  HandleTimers();
  RtpProcessing();
}


// Zeitgeber: Anrufdauer, Wiederholungen (UDP verliert Pakete), REGISTER-Ablauf
void Sip::HandleTimers() {

  uint32_t iWorkTime = iRingTime ? (Millis() - iRingTime) : 0;

  // Klingelt zu lange -> CANCEL. Nach dem Abheben gilt die Gespraechsdauer;
  // die beendet RtpProcessing(), hier nur die Absicherung ohne RTP-Strom.
  if ( iRingTime && !bAnswered && iWorkTime > iMaxTime )
    Hangup();
  else if ( iRingTime && bAnswered && !bInCall
            && (millis() - callAnsweredAt) > (uint32_t)TalkSeconds() * 1000UL )
    Hangup();

  // Eingehender Anruf: 200 OK wiederholen, bis das ACK kommt (UDP)
  if ( iRingTime && bIncoming && bInAckPending && (millis() - inOkSentAt) > inOkInterval )
  {
    if ( millis() - inOkFirstAt > 32000 )
      Hangup();                     // kein ACK -> Dialog gescheitert
    else
    {
      SendIncomingOk();
      if ( inOkInterval < 4000 ) inOkInterval *= 2;
    }
  }

  // Ausstehende Anfrage wiederholen (RFC 3261 Timer A/E): INVITE bis zur ersten
  // Antwort (0,5 / 1 / 2 / 4 s ...), laengstens bis der Anruf endet; CANCEL und
  // BYE bis zur Endantwort, hoechstens 4x in 3,5 s - auch nach dem Anruf
  if ( reqType != REQ_NONE && (millis() - reqSentAt) > reqInterval )
  {
    if ( reqType == REQ_INVITE ? (iRingTime != 0 && !bIncoming) : reqSends < 4 )
    {
      SendReq();
      reqSends++;
      reqSentAt = millis();
      if ( reqType == REQ_INVITE || reqInterval < 2000 )
        reqInterval *= 2;
    }
    else
      reqType = REQ_NONE;   // keine Antwort - aufgeben
  }

  // REGISTER ohne Antwort wiederholen (gleiche CSeq; 0,5 / 1 / 2 s), erst danach
  // "keine Antwort". Eine bestehende Anmeldung gilt bis zu ihrem Ablauf weiter.
  if ( bRegPending && (millis() - regSentAt) > regInterval )
  {
    if ( regSends < 4 )
      SendRegister(bRegAuthTried);
    else
    {
      bRegPending = false;
      iRegStatus  = 0;   // keine Antwort; Call-ID bleibt (Bindung evtl. noch gueltig)
    }
  }
  if ( bRegistered && (millis() - regOkAt) >= regValidMs )
    bRegistered = false;   // abgelaufen (auch gegen Ueberlauf von millis())
}


void Sip::HandleUdpPacket(const char *p, bool fromServer, bool truncated) {

  bool isResponse = strncmp(p, "SIP/2.0 ", 8) == 0;

  // ---------------- Anfragen ----------------
  if ( !isResponse )
  {
    if ( !fromServer )
      return;   // nicht von der Anlage -> ignorieren (Schutz vor gefaelschten Paketen)

    if ( IsRequest(p, "INVITE") )
    {
      HandleIncomingInvite(p, truncated);
      return;
    }
    if ( IsRequest(p, "OPTIONS") || IsRequest(p, "NOTIFY") )
    {
      Respond(p, 200, "OK");   // z.B. Erreichbarkeitspruefung (qualify) der Anlage
      return;
    }

    bool ourIncoming = bIncoming && iRingTime && IsCallIdStr(p, caInCallId);
    bool ourOutgoing = !bIncoming && iRingTime && IsCallId(p, callid);

    if ( IsRequest(p, "ACK") )
    {
      if ( ourIncoming ) bInAckPending = false;
      return;
    }

    if ( !ourIncoming && !ourOutgoing )
    {
      // Requests eines alten Dialogs (z.B. verspaetetes BYE) noch hoeflich
      // beantworten, aber den laufenden Anruf nicht anfassen.
      if ( IsRequest(p, "BYE") || IsRequest(p, "INFO") || IsRequest(p, "CANCEL") )
        Respond(p, 200, "OK");
      return;
    }

    if ( IsRequest(p, "BYE") )
    {
      Respond(p, 200, "OK");
      StopCall();
      iRingTime = 0;
      bIncoming = false;
      bInAckPending = false;
    }
    else if ( IsRequest(p, "CANCEL") )
    {
      Respond(p, 200, "OK");   // eingehend schon angenommen -> Anrufer legt mit BYE auf
    }
    else if ( IsRequest(p, "INFO") )
    {
      // DTMF per SIP INFO (application/dtmf-relay): "Signal=*". Herkunft merken.
      const char *sig = strstr(p, "Signal=");
      if ( sig ) { char c = sig[7]; if ( c == ' ' ) c = sig[8]; if ( c > ' ' ) { cLastDtmf = c; bDtmfIncoming = ourIncoming; iDtmfRx++; } }
      Respond(p, 200, "OK");
    }
    return;
  }

  HandleResponse(p, fromServer);
}


// Liegt die Antwort (CSeq-Nummer cseq) zur ausstehenden CANCEL/BYE-Anfrage?
bool Sip::IsReqResponse(const char *p, int cseq) {

  if ( cseq != reqCSeq )
    return false;
  if ( reqType == REQ_CANCEL )
    return IsResponseTo(p, "CANCEL") && IsCallId(p, callid);
  if ( reqType == REQ_BYE )
    return IsResponseTo(p, "BYE") && IsCallId(p, callid);
  if ( reqType == REQ_BYE_IN )
    return IsResponseTo(p, "BYE") && IsCallIdStr(p, caInCallId);
  return false;
}


void Sip::HandleResponse(const char *p, bool fromServer) {

  if ( IsResponseTo(p, "REGISTER") )
  {
    HandleRegisterResponse(p);
    return;
  }

  int code = atoi(p + 8);
  int cseq = GrepInteger(p, "\nCSeq: ");

  // Endantwort auf unser CANCEL/BYE -> nicht mehr wiederholen
  if ( IsResponseTo(p, "CANCEL") || IsResponseTo(p, "BYE") )
  {
    if ( code >= 200 && IsReqResponse(p, cseq) )
      reqType = REQ_NONE;
    return;
  }

  // ---------------- Antworten auf unseren ausgehenden Anruf ----------------
  if ( !IsResponseTo(p, "INVITE") )
    return;

  if ( !IsCallId(p, callid) )
  {
    // Endantwort eines frueheren Anrufs (z.B. 487 nach CANCEL, waehrend schon die
    // naechste Nummer gewaehlt wird): nur quittieren, sonst wiederholt die Anlage
    if ( code >= 300 && fromServer )
      Ack(p);
    return;
  }

  // Antwort auf eine aeltere Transaktion (z.B. wiederholtes 401, weil unser ACK
  // verloren ging): Endantwort nur quittieren, den laufenden Anruf nicht anfassen
  if ( cseq != iInviteCSeq )
  {
    if ( code >= 300 )
      Ack(p);
    return;
  }

  // Irgendeine Antwort auf das aktuelle INVITE -> nicht mehr wiederholen; eine
  // Endantwort erledigt auch ein ausstehendes CANCEL (487 = abgebrochen)
  if ( reqType == REQ_INVITE || (reqType == REQ_CANCEL && code >= 200) )
    reqType = REQ_NONE;

  if ( code == 401 || code == 407 )
  {
    Ack(p);
    if ( !iRingTime || bIncoming )
      return;
    // Nur einmal mit Digest antworten: eine erneute Aufforderung (auf das INVITE mit
    // Zugangsdaten) heisst Zugangsdaten falsch -> Anruf beenden statt Schleife
    int err = iAuthCnt > 0 ? -1 : Invite(p);
    if ( err != 0 && err != 500 )
    {
      StopCall();
      iRingTime = 0;
      eCallResult = CALL_FAILED;
      iCallCode = err > 0 ? err : code;
    }
  }
  else if ( code >= 200 && code < 300 )		// OK
  {
    ParseReturnParams(p);
    Ack(p);
    if ( !iRingTime || bIncoming )
    {
      // Angenommen, waehrend unser CANCEL unterwegs war -> sauber beenden
      StartReq(REQ_BYE, iInviteCSeq + 1);
      return;
    }
    if ( bAnswered )
      return;   // Wiederholung des 200 OK: ACK genuegt
    bAnswered = true;
    eCallResult = CALL_ANSWERED;
    callAnsweredAt = millis();
    // Anruf angenommen, Beep-Audio starten
    if ( iBeepSeconds > 0 && !bInCall )
      StartRtp(p);
  }
  else if ( code < 200 )   // 100 Trying, 180 Ringing, 183 Session Progress
  {
    ParseReturnParams(p);
  }
  else
  {
    Ack(p);
    if ( !iRingTime || bIncoming )
      return;
    StopCall();
    iRingTime = 0;
    if ( code == 603 )                                     eCallResult = CALL_DECLINED;
    else if ( code == 486 || code == 600 )                 eCallResult = CALL_BUSY;
    else if ( code != 487 ) { eCallResult = CALL_FAILED; iCallCode = code; }
    // 487 = Antwort auf unser CANCEL: Ergebnis hat Hangup() schon gesetzt
  }
}


void Sip::AddSipLine(const char* constFormat , ... ) {

  va_list arglist;
  va_start(arglist, constFormat);
  size_t l = strlen(pbuf);
  if ( l + 3 >= lbuf )
  {
    va_end(arglist);
    return;
  }
  char *p = pbuf + l;
  vsnprintf(p, lbuf - l, constFormat, arglist );
  va_end(arglist);
  l = strlen(pbuf);
  if ( l < (lbuf - 2) )
  {
    pbuf[l] = '\r';
    pbuf[l + 1] = '\n';
    pbuf[l + 2] = 0;
  }
}


// Erste Zeile ab psearch (z.B. "\nFrom: ") aus p an pbuf anhaengen
bool Sip::AddCopySipLine(const char *p, const char *psearch) {

  const char *pa = strstr(p, psearch);

  if ( !pa )
    return false;
  if ( psearch[0] == '\n' )
    pa++;

  const char *pe = strpbrk(pa, "\r\n");
  if ( !pe )
    pe = pa + strlen(pa);
  if ( pe <= pa )
    return false;

  AddSipLine("%.*s", (int)(pe - pa), pa);
  return true;
}


// Alle Zeilen ab psearch (z.B. alle Via-Zeilen) in Reihenfolge anhaengen
void Sip::AddCopyAllLines(const char *p, const char *psearch) {

  const char *pa = p;
  while ( (pa = strstr(pa, psearch)) != NULL )
  {
    AddCopySipLine(pa, psearch);
    pa += strlen(psearch);
  }
}


// Header-Namen fuer Normalize(): Kurzform (bzw. '-') und Langform, wie sie die
// Suchen im Code erwarten ("\nCall-ID: " ...). Im Flash (ESP8266: spart RAM).
static const char kHeaderNames[] PROGMEM =
  "iCall-ID\0" "fFrom\0" "tTo\0" "vVia\0" "mContact\0" "lContent-Length\0"
  "cContent-Type\0" "kSupported\0" "sSubject\0" "eContent-Encoding\0"
  "-CSeq\0" "-Expires\0" "-WWW-Authenticate\0" "-Proxy-Authenticate\0";

static char Lower(char c) { return ( c >= 'A' && c <= 'Z' ) ? (char)(c + 32) : c; }

// Langform (im Flash) zum Header-Namen [n, n + nl), sonst nullptr
static const char *CanonicalHeader(const char *n, size_t nl) {

  const char *t = kHeaderNames;
  while ( pgm_read_byte(t) )
  {
    char k = (char)pgm_read_byte(t);
    const char *name = t + 1;
    if ( nl == 1 && k != '-' && Lower(n[0]) == k )
      return name;
    size_t i = 0;
    while ( i < nl && pgm_read_byte(name + i) && Lower((char)pgm_read_byte(name + i)) == Lower(n[i]) )
      i++;
    if ( i == nl && pgm_read_byte(name + i) == 0 )
      return name;
    while ( pgm_read_byte(name + i) )
      i++;
    t = name + i + 1;
  }
  return nullptr;
}

// n Bytes an dst[o] anhaengen (pgm: Quelle im Flash, bis zur Null)
static bool PutBytes(char *dst, size_t cap, size_t &o, const char *s, size_t n, bool pgm = false) {

  if ( pgm )
    for ( n = 0; pgm_read_byte(s + n); n++ ) {}
  if ( o + n > cap )
    return false;
  for ( size_t i = 0; i < n; i++ )
    dst[o + i] = pgm ? (char)pgm_read_byte(s + i) : s[i];
  o += n;
  return true;
}

static bool IsWsp(char c) { return c == ' ' || c == '\t'; }


// Empfangene Nachricht vereinheitlichen (RFC 3261 7.3): Header-Namen in beliebiger
// Schreibweise oder Kurzform ("i:", "f:", "t:", ...) und mit Leerraum um den
// Doppelpunkt werden zu "Name: Wert", Folgezeilen werden angehaengt. So finden die
// Suchen im Code ("\nCall-ID: " ...) jede Schreibweise. Startzeile und Body bleiben
// unveraendert; pbuf dient als Zwischenspeicher. Liefert die neue Laenge (bodyAt =
// Beginn des Body, -1 ohne Leerzeile) oder -1, wenn das Ergebnis nicht passt.
int Sip::Normalize(char *buf, int len, size_t cap, int *bodyAt) {

  // Ende der Kopfzeilen: Leerzeile (CRLF CRLF bzw. LF LF)
  int he = len;
  *bodyAt = -1;
  for ( int i = 0; i < len; i++ )
  {
    if ( buf[i] != '\n' )
      continue;
    if ( i + 1 < len && buf[i + 1] == '\n' )
    {
      he = i + 1; *bodyAt = i + 2; break;
    }
    if ( i + 2 < len && buf[i + 1] == '\r' && buf[i + 2] == '\n' )
    {
      he = i + 1; *bodyAt = i + 3; break;
    }
  }

  size_t o = 0;
  bool ok = true;
  for ( int i = 0; ok && i < he; )
  {
    int e = i;
    while ( e < he && buf[e] != '\n' )
      e++;
    int nx = e < he ? e + 1 : e;                         // naechste Zeile
    int ce = ( e > i && buf[e - 1] == '\r' ) ? e - 1 : e; // Ende des Inhalts
    const char *c = (const char *)memchr(buf + i, ':', ce - i);

    if ( i == 0 )
      ok = PutBytes(pbuf, lbuf, o, buf + i, nx - i);     // Startzeile
    else if ( IsWsp(buf[i]) )
    {
      // Folgezeile: Zeilenumbruch + Leerraum = ein Leerzeichen
      while ( o > 0 && ( pbuf[o - 1] == '\n' || pbuf[o - 1] == '\r' ) )
        o--;
      int s = i;
      while ( s < ce && IsWsp(buf[s]) ) s++;
      ok = PutBytes(pbuf, lbuf, o, " ", 1) && PutBytes(pbuf, lbuf, o, buf + s, ce - s)
           && PutBytes(pbuf, lbuf, o, buf + ce, nx - ce);
    }
    else if ( !c )
      ok = PutBytes(pbuf, lbuf, o, buf + i, nx - i);     // keine Kopfzeile: unveraendert
    else
    {
      int ne = c - buf;
      while ( ne > i && IsWsp(buf[ne - 1]) ) ne--;
      int vs = c - buf + 1;
      while ( vs < ce && IsWsp(buf[vs]) ) vs++;
      int ve = ce;
      while ( ve > vs && IsWsp(buf[ve - 1]) ) ve--;
      const char *cn = CanonicalHeader(buf + i, ne - i);
      ok = ( cn ? PutBytes(pbuf, lbuf, o, cn, 0, true) : PutBytes(pbuf, lbuf, o, buf + i, ne - i) )
           && PutBytes(pbuf, lbuf, o, ": ", 2) && PutBytes(pbuf, lbuf, o, buf + vs, ve - vs)
           && PutBytes(pbuf, lbuf, o, buf + ce, nx - ce);
    }
    i = nx;
  }

  size_t tail = len - he;   // Leerzeile und Body
  if ( !ok || o + tail + 1 > cap )
  {
    pbuf[0] = 0;
    return -1;
  }
  memmove(buf + o, buf + he, tail);
  memcpy(buf, pbuf, o);
  buf[o + tail] = 0;
  pbuf[0] = 0;
  if ( *bodyAt >= 0 )
    *bodyAt = (int)o + (*bodyAt - he);
  return (int)(o + tail);
}


// Wert eines Headers (name z.B. "\nFrom: "): Anfang und Laenge bis zum Zeilenende
static const char *FindHeader(const char *p, const char *name, size_t *len) {

  const char *pa = strstr(p, name);

  if ( !pa )
    return nullptr;
  pa += strlen(name);
  const char *pe = strpbrk(pa, "\r\n");
  *len = pe ? (size_t)(pe - pa) : strlen(pa);
  return pa;
}


// Enthaelt der Header-Wert [v, v + l) einen Tag (";tag=")?
static bool HasTag(const char *v, size_t l) {

  for ( size_t i = 0; v && i + 5 <= l; i++ )
    if ( strncasecmp(v + i, ";tag=", 5) == 0 )
      return true;
  return false;
}


// Copy Call-ID, From, Via and To from response to caRead using later for BYE or CANCEL the call
bool Sip::ParseReturnParams(const char *p) {

  pbuf[0] = 0;

  AddCopySipLine(p, "\nCall-ID: ");
  AddCopySipLine(p, "\nFrom: ");
  AddCopySipLine(p, "\nVia: ");
  AddCopySipLine(p, "\nTo: ");

  size_t l = strlen(pbuf);
  if ( l < 2 || l - 2 >= sizeof(caRead) )
    return false;   // zu lang: lieber die alten Werte behalten als abschneiden

  memcpy(caRead, pbuf, l - 2);   // letztes CRLF weglassen
  caRead[l - 2] = 0;
  return true;
}


// Prueft, ob das Paket (p) die Call-ID "<id>@<eigene IP>" traegt
bool Sip::IsCallId(const char *p, uint32_t id32) {

  const char *pc = strstr(p, "\nCall-ID: ");

  if ( !pc )
    return false;

  char id[40];
  snprintf(id, sizeof(id), "%010u@%s", id32, pMyIp);
  size_t l = strlen(id);
  return strncmp(pc + 10, id, l) == 0 && ( pc[10 + l] == '\r' || pc[10 + l] == '\n' || pc[10 + l] == 0 );
}


// Prueft, ob das Paket (p) genau die Call-ID id traegt
bool Sip::IsCallIdStr(const char *p, const char *id) {

  const char *pc = strstr(p, "\nCall-ID: ");

  if ( !pc || !id[0] )
    return false;

  size_t l = strlen(id);
  return strncmp(pc + 10, id, l) == 0 && ( pc[10 + l] == '\r' || pc[10 + l] == '\n' || pc[10 + l] == 0 );
}


// Ist p eine Anfrage der Methode method ("BYE sip:...")?
bool Sip::IsRequest(const char *p, const char *method) {

  size_t l = strlen(method);
  return strncmp(p, method, l) == 0 && p[l] == ' ';
}


// Prueft, ob die Antwort (p) zur angegebenen Methode gehoert (CSeq-Zeile)
bool Sip::IsResponseTo(const char *p, const char *method) {

  const char *pc = strstr(p, "\nCSeq: ");

  if ( !pc )
    return false;

  pc += 7;
  while ( *pc == ' ' || ( *pc >= '0' && *pc <= '9' ) )
    pc++;

  size_t l = strlen(method);
  return strncmp(pc, method, l) == 0 && ( pc[l] == '\r' || pc[l] == '\n' || pc[l] == ' ' || pc[l] == 0 );
}


// Wert eines Headers (name z.B. "\nFrom: ") bis zum Zeilenende kopieren
bool Sip::HeaderValue(const char *p, const char *name, char *dest, size_t destlen) {

  size_t l;
  const char *pa = FindHeader(p, name, &l);

  if ( !pa || l >= destlen )
    return false;

  memcpy(dest, pa, l);
  dest[l] = 0;
  return true;
}


// URI aus To/Contact kopieren: "<sip:...>" (auch mit Anzeigename davor) oder
// ohne spitze Klammern bis zum ersten Parameter (';')
bool Sip::HeaderUri(const char *p, const char *name, char *dest, size_t destlen) {

  size_t l;
  const char *v = FindHeader(p, name, &l);

  if ( !v )
    return false;
  const char *s = (const char *)memchr(v, '<', l);
  const char *e;
  if ( s )
  {
    s++;
    e = (const char *)memchr(s, '>', v + l - s);
    if ( !e )
      return false;   // kein Endzeichen -> kaputtes Paket
  }
  else
  {
    s = v;
    e = s;
    while ( e < v + l && *e != ';' && !IsWsp(*e) ) e++;
  }
  if ( e <= s || (size_t)(e - s) >= destlen )
    return false;
  memcpy(dest, s, e - s);
  dest[e - s] = 0;
  return true;
}


int Sip::GrepInteger(const char *p, const char *psearch) {

  int param = -1;
  const char *pc = strstr(p, psearch);

  if ( pc )
  {
    param = atoi(pc + strlen(psearch));
  }

  return param;
}


// Parameter (realm, nonce, ...) aus einer Authenticate-Zeile [start, end) lesen
static bool AuthParam(const char *start, const char *end, const char *name, char *out, size_t len) {

  size_t nl = strlen(name);
  for ( const char *s = start; s + nl < end; s++ )
  {
    if ( strncasecmp(s, name, nl) != 0 )
      continue;
    char before = s == start ? ' ' : s[-1];
    if ( before != ' ' && before != ',' && before != '\t' )
      continue;   // z.B. "cnonce" statt "nonce"
    const char *v = s + nl;
    while ( v < end && *v == ' ' ) v++;
    if ( v >= end || *v != '=' )
      continue;
    v++;
    while ( v < end && *v == ' ' ) v++;
    const char *ve;
    if ( v < end && *v == '"' )
    {
      v++;
      ve = (const char *)memchr(v, '"', end - v);
      if ( !ve )
        return false;
    }
    else
    {
      ve = v;
      while ( ve < end && *ve != ',' && *ve != ' ' ) ve++;
    }
    size_t l = ve - v;
    if ( l >= len )
      return false;
    memcpy(out, v, l);
    out[l] = 0;
    return true;
  }
  return false;
}


// Digest-Antwort (RFC 2617, auch mit qop=auth und opaque) nach out (Standard:
// caAuth) schreiben
bool Sip::BuildAuth(const char *p, const char *method, const char *uri, char *out, size_t outLen) {

  if ( !out )
  {
    out    = caAuth;
    outLen = sizeof(caAuth);
  }
  bool proxy = false;
  const char *h = strstr(p, "\nWWW-Authenticate:");
  if ( !h )
  {
    h = strstr(p, "\nProxy-Authenticate:");
    proxy = true;
  }
  if ( !h )
    return false;
  h++;
  const char *eol = strpbrk(h, "\r\n");
  if ( !eol )
    eol = h + strlen(h);

  char realm[128], nonce[160], opaque[128], qop[48], algo[16];
  if ( !AuthParam(h, eol, "realm", realm, sizeof(realm))
       || !AuthParam(h, eol, "nonce", nonce, sizeof(nonce)) )
    return false;
  if ( !AuthParam(h, eol, "opaque", opaque, sizeof(opaque)) )
    opaque[0] = 0;
  if ( AuthParam(h, eol, "algorithm", algo, sizeof(algo)) && strcasecmp(algo, "MD5") != 0 )
    return false;   // nur MD5 wird unterstuetzt

  // qop="auth,auth-int" -> "auth" verwenden, falls angeboten
  bool useQop = false;
  if ( AuthParam(h, eol, "qop", qop, sizeof(qop)) )
  {
    for ( char *t = strtok(qop, ", "); t; t = strtok(NULL, ", ") )
      if ( strcmp(t, "auth") == 0 ) useQop = true;
    if ( !useQop )
      return false;   // nur auth-int angeboten
  }

  char ha1[33], ha2[33], resp[33], cnonce[12], temp[340];
  snprintf(temp, sizeof(temp), "%s:%s:%s", pSipUser, realm, pSipPassWd);
  MakeMd5Digest(ha1, temp);
  snprintf(temp, sizeof(temp), "%s:%s", method, uri);
  MakeMd5Digest(ha2, temp);
  snprintf(cnonce, sizeof(cnonce), "%08x", (unsigned)Random());
  if ( useQop )
    snprintf(temp, sizeof(temp), "%s:%s:00000001:%s:auth:%s", ha1, nonce, cnonce, ha2);
  else
    snprintf(temp, sizeof(temp), "%s:%s:%s", ha1, nonce, ha2);
  MakeMd5Digest(resp, temp);

  int n = snprintf(out, outLen,
           "%s: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\", algorithm=MD5",
           proxy ? "Proxy-Authorization" : "Authorization", pSipUser, realm, nonce, uri, resp);
  if ( n < 0 || n >= (int)outLen )
    return false;
  if ( useQop )
    n += snprintf(out + n, outLen - n, ", qop=auth, nc=00000001, cnonce=\"%s\"", cnonce);
  if ( opaque[0] && n < (int)outLen )
    n += snprintf(out + n, outLen - n, ", opaque=\"%s\"", opaque);
  return n < (int)outLen;
}


void Sip::Ack(const char *p) {

  char ca[128];

  // Ziel = URI aus To, mit oder ohne <> (RFC 3261 erlaubt beides)
  if ( !HeaderUri(p, "\nTo: ", ca, sizeof(ca)) )
    return;

  pbuf[0] = 0;
  AddSipLine("ACK %s SIP/2.0", ca);
  AddCopySipLine(p, "\nCall-ID: ");
  int cseq = GrepInteger(p, "\nCSeq: ");
  AddSipLine("CSeq: %i ACK",  cseq);
  AddCopySipLine(p, "\nFrom: ");
  AddCopySipLine(p, "\nVia: ");
  AddCopySipLine(p, "\nTo: ");
  AddSipLine("Content-Length: 0");
  AddSipLine("");
  SendUdp();
}


// CANCEL fuer das noch nicht angenommene INVITE. Muss Request-URI, Call-ID,
// From, To (ohne Tag), Via und CSeq-Nummer des INVITE exakt uebernehmen.
int Sip::Cancel() {

  pbuf[0] = 0;
  AddSipLine("CANCEL sip:%s@%s SIP/2.0", pDialNr, pSipIp);
  AddSipLine("Call-ID: %010u@%s", callid, pMyIp);
  AddSipLine("CSeq: %i CANCEL", iInviteCSeq);
  AddSipLine("Max-Forwards: 70");
  AddSipLine("From: \"%s\"  <sip:%s@%s>;tag=%010u", pDialDesc, pSipUser, pSipIp, tagid);
  AddSipLine("Via: SIP/2.0/UDP %s:%i;branch=z9hG4bK%010u;rport=%i", pMyIp, iMyPort, branchid, iMyPort);
  AddSipLine("To: <sip:%s@%s>", pDialNr, pSipIp);
  AddSipLine("Content-Length: 0");
  AddSipLine("");
  return SendUdp();
}


int Sip::Bye(int cseq) {

  if ( caRead[0] == 0 )
    return -1;

  pbuf[0] = 0;
  AddSipLine("%s sip:%s@%s SIP/2.0",  "BYE", pDialNr, pSipIp);
  AddSipLine("%s",  caRead);
  AddSipLine("CSeq: %i %s", cseq, "BYE");
  AddSipLine("Max-Forwards: 70");
  AddSipLine("User-Agent: sip-client/0.0.1");
  AddSipLine("Content-Length: 0");
  AddSipLine("");
  return SendUdp();
}


// Anfrage senden und fuer Wiederholungen merken (siehe HandleTimers()). Eine neue
// Anfrage ersetzt die vorige. Liefert das Ergebnis von SendUdp(); laesst sie sich
// gar nicht senden (keine Adresse, zu gross, keine Daten), wird nicht wiederholt.
int Sip::StartReq(uint8_t type, int cseq) {

  reqType     = type;
  reqCSeq     = cseq;
  reqSends    = 1;
  reqInterval = 500;   // T1
  int err = SendReq();
  reqSentAt   = millis();
  if ( err != 0 && err != 500 )
    reqType = REQ_NONE;
  return err;
}


// Ausstehende Anfrage (erneut) senden - aus dem Zustand gebaut, also byte-gleich
int Sip::SendReq() {

  switch ( reqType )
  {
    case REQ_INVITE: return SendInvite();
    case REQ_CANCEL: return Cancel();
    case REQ_BYE:    return Bye(reqCSeq);
    case REQ_BYE_IN: return ByeIncoming(reqCSeq);
  }
  return -1;
}


// Antwort ohne Inhalt auf eine Anfrage (alle Via-Zeilen, To ggf. mit Tag);
// mit sdp als 200 OK auf ein (Re-)INVITE mit unserem Contact und SDP
void Sip::Respond(const char *p, int code, const char *reason, const char *sdp) {

  pbuf[0] = 0;
  AddSipLine("SIP/2.0 %i %s", code, reason);
  AddCopyAllLines(p, "\nVia: ");
  AddCopySipLine(p, "\nFrom: ");
  size_t l;
  const char *to = FindHeader(p, "\nTo: ", &l);
  if ( to )
  {
    if ( HasTag(to, l) || code < 200 )
      AddSipLine("To: %.*s", (int)l, to);
    else
      AddSipLine("To: %.*s;tag=%010u", (int)l, to, Random());
  }
  AddCopySipLine(p, "\nCall-ID: ");
  AddCopySipLine(p, "\nCSeq: ");
  if ( sdp )
  {
    AddSipLine("Contact: <sip:%s@%s:%i;transport=udp>", pSipUser, pMyIp, iMyPort);
    AddSipLine("Content-Type: application/sdp");
    AddSipLine("Content-Length: %i", (int)strlen(sdp));
    AddSipLine("");
    strncat(pbuf, sdp, lbuf - strlen(pbuf) - 1);
  }
  else
  {
    AddSipLine("Content-Length: 0");
    AddSipLine("");
  }
  SendUdp();
}


// SDP: Angebot (G.711 A-law, u-law, DTMF) oder Antwort mit dem gewaehlten Codec
int Sip::BuildSdp(char *out, size_t len, bool offer) {

  if ( offer )
    return snprintf(out, len,
      "v=0\r\n"
      "o=- %010u %010u IN IP4 %s\r\n"
      "s=doorbell\r\n"
      "c=IN IP4 %s\r\n"
      "t=0 0\r\n"
      "m=audio %i RTP/AVP 8 0 101\r\n"
      "a=rtpmap:8 PCMA/8000\r\n"
      "a=rtpmap:0 PCMU/8000\r\n"
      "a=rtpmap:101 telephone-event/8000\r\n"
      "a=fmtp:101 0-15\r\n"
      "a=ptime:20\r\n"
      "a=sendrecv\r\n",
      callid, callid, pMyIp, pMyIp, iRtpPort);

  char dtmf[96] = "";
  if ( dtmfPt < 128 )
    snprintf(dtmf, sizeof(dtmf), "a=rtpmap:%u telephone-event/8000\r\na=fmtp:%u 0-15\r\n", dtmfPt, dtmfPt);
  char pts[12] = "";
  if ( dtmfPt < 128 )
    snprintf(pts, sizeof(pts), " %u", dtmfPt);
  // Sitzung: eingehend unser Tag; ausgehend (Antwort auf ein Re-INVITE) die des
  // Angebots im INVITE, Version + 1
  uint32_t sid = bIncoming ? inTag : callid;
  uint32_t ver = bIncoming ? inTag : callid + 1;
  return snprintf(out, len,
    "v=0\r\n"
    "o=- %010u %010u IN IP4 %s\r\n"
    "s=doorbell\r\n"
    "c=IN IP4 %s\r\n"
    "t=0 0\r\n"
    "m=audio %i RTP/AVP %u%s\r\n"
    "a=rtpmap:%u %s/8000\r\n"
    "%s"
    "a=ptime:20\r\n"
    "a=sendrecv\r\n",
    sid, ver, pMyIp, pMyIp, iRtpPort, rtpPt, pts,
    rtpPt, rtpPt == 8 ? "PCMA" : "PCMU", dtmf);
}


// Call invite without or with the response from peer: ohne p neuer Anruf (neue
// Call-ID, Tag, Branch), mit p (401/407) mit Digest als neue Transaktion (CSeq + 1,
// neuer Branch). Liefert das Ergebnis von SendUdp(), -1 = Digest nicht moeglich.
int Sip::Invite(const char *p) {

  if ( !p )
  {
    iAuthCnt = 0;
    callid = Random();
    tagid = Random();
    branchid = Random();
    iInviteCSeq = 1;
  }
  else
  {
    char uri[96];
    snprintf(uri, sizeof(uri), "sip:%s@%s", pDialNr, pSipIp);
    if ( !BuildAuth(p, "INVITE", uri) )
    {
      caRead[0] = 0;
      return -1;
    }
    iInviteCSeq++;         // fuer CANCEL (gleiche Nummer) und BYE (hoeher)
    branchid = Random();   // neue Transaktion -> neuer Branch
    iAuthCnt++;
  }
  caRead[0] = 0;
  return StartReq(REQ_INVITE, iInviteCSeq);
}


// INVITE aus dem Zustand bauen und senden; Wiederholungen sind byte-gleich
// (gleicher Branch, gleiche CSeq, gleicher Authorization-Header)
int Sip::SendInvite() {

  pbuf[0] = 0;
  AddSipLine("INVITE sip:%s@%s SIP/2.0", pDialNr, pSipIp);
  AddSipLine("Call-ID: %010u@%s",  callid, pMyIp);
  AddSipLine("CSeq: %i INVITE",  iInviteCSeq);
  AddSipLine("Max-Forwards: 70");
  // not needed for fritzbox
  // AddSipLine("User-Agent: sipdial by jl");
  AddSipLine("From: \"%s\"  <sip:%s@%s>;tag=%010u", pDialDesc, pSipUser, pSipIp, tagid);
  AddSipLine("Via: SIP/2.0/UDP %s:%i;branch=z9hG4bK%010u;rport=%i", pMyIp, iMyPort, branchid, iMyPort);
  AddSipLine("To: <sip:%s@%s>", pDialNr, pSipIp);
  AddSipLine("Contact: \"%s\" <sip:%s@%s:%i;transport=udp>", pSipUser, pSipUser, pMyIp, iMyPort);

  if ( iAuthCnt > 0 )
    AddSipLine("%s", caAuth);

  if ( iBeepSeconds > 0 )
  {
    // SDP-Angebot (early offer): G.711 A-law und u-law auf unserem RTP-Port.
    // Viele europaeische Anlagen akzeptieren nur PCMA (sonst 488 Not Acceptable).
    char sdp[340];
    BuildSdp(sdp, sizeof(sdp), true);
    AddSipLine("Content-Type: application/sdp");
    AddSipLine("Content-Length: %i", (int)strlen(sdp));
    AddSipLine("");                                   // Leerzeile Header/Body
    strncat(pbuf, sdp, lbuf - strlen(pbuf) - 1);      // Body roh anhaengen
  }
  else
  {
    AddSipLine("Content-Type: application/sdp");
    AddSipLine("Content-Length: 0");
    AddSipLine("");
  }
  return SendUdp();
}


// ----- Eingehender Anruf (Erweiterung) ------------------------------------

// SDP-Angebot des Anrufers auswerten: Codec waehlen (PCMA vor PCMU), Ziel fuer RTP
bool Sip::ParseOffer(const char *p) {

  const char *m = strstr(p, "\nm=audio ");
  if ( !m )
    return false;   // ohne Angebot (late offer) wird nicht angenommen
  m++;
  uint16_t port = (uint16_t)atoi(m + 8);
  const char *eol = strpbrk(m, "\r\n");
  const char *avp = strstr(m, "RTP/AVP ");
  if ( !port || !avp || (eol && avp > eol) )
    return false;

  bool hasA = false, hasU = false;
  for ( const char *t = avp + 8; *t && (!eol || t < eol); )
  {
    int pt = atoi(t);
    if ( pt == 8 ) hasA = true;
    if ( pt == 0 && *t == '0' ) hasU = true;
    while ( *t && *t != ' ' && *t != '\r' && *t != '\n' ) t++;
    while ( *t == ' ' ) t++;
  }
  if ( !hasA && !hasU )
    return false;

  uint8_t dpt = 0xFF;   // ohne telephone-event nur DTMF per SIP INFO
  for ( const char *r = strstr(p, "a=rtpmap:"); r; r = strstr(r + 9, "a=rtpmap:") )
  {
    const char *sp = strchr(r + 9, ' ');
    if ( sp && strncmp(sp + 1, "telephone-event", 15) == 0 )
    {
      dpt = (uint8_t)atoi(r + 9);
      break;
    }
  }

  const char *c = strstr(p, "c=IN IP4 ");
  if ( !c )
    return false;
  c += 9;
  char ip[24];
  int i = 0;
  while ( i < (int)sizeof(ip) - 1 && *c && *c != '\r' && *c != '\n' && *c != ' ' )
    ip[i++] = *c++;
  ip[i] = 0;
  IPAddress a;   // fromString() schreibt beim ESP8266 auch bei Fehler teilweise
  if ( !a.fromString(ip) )
    return false;
  // Erst jetzt uebernehmen: ein unpassendes Angebot (Re-INVITE) aendert nichts
  rtpPt         = hasA ? 8 : 0;
  dtmfPt        = dpt;
  remoteRtpIp   = a;
  remoteRtpPort = port;
  return true;
}


// Rufnummer aus From ("Name" <sip:NUMMER@...>, auch sips:/tel:, ohne <>) nach out;
// leer, wenn nicht lesbar
static void CallerFrom(const char *v, size_t vl, char *out, size_t len) {
  out[0] = 0;
  const char *e = v + vl;
  const char *u = (const char *)memchr(v, '<', vl);
  u = u ? u + 1 : v;
  while ( u < e && IsWsp(*u) ) u++;
  if ( e - u > 4 && strncasecmp(u, "sip:", 4) == 0 )       u += 4;
  else if ( e - u > 5 && strncasecmp(u, "sips:", 5) == 0 ) u += 5;
  else if ( e - u > 4 && strncasecmp(u, "tel:", 4) == 0 )  u += 4;
  else return;
  const char *at = u;
  while ( at < e && *at != '@' && *at != ';' && *at != '>' && !IsWsp(*at) ) at++;
  size_t l = at - u;
  if ( l >= len ) l = len - 1;
  memcpy(out, u, l);
  out[l] = 0;
}


// FNV-1a ueber den Wert eines Headers (z.B. Call-ID beliebiger Laenge)
static uint32_t HeaderHash(const char *p, const char *name) {
  size_t l = 0;
  const char *v = FindHeader(p, name, &l);
  uint32_t h = 2166136261UL;
  for ( size_t i = 0; v && i < l; i++ )
  {
    h ^= (uint8_t)v[i];
    h *= 16777619UL;
  }
  return h;
}


// truncated: abgeschnitten (groesser als der Puffer) -> 513 statt mit halbem SDP
// weiterzuarbeiten; ein Klingel-Anruf braucht kein SDP und wird trotzdem erkannt
void Sip::HandleIncomingInvite(const char *p, bool truncated) {

  // Re-INVITE im laufenden eingehenden Gespraech (z.B. Media-Wechsel)
  if ( bIncoming && iRingTime && IsCallIdStr(p, caInCallId) )
  {
    if ( truncated )
    {
      Respond(p, 513, "Message Too Large");
      return;
    }
    pbuf[0] = 0;
    AddCopyAllLines(p, "\nVia: ");
    if ( strlen(pbuf) < sizeof(caInVia) ) strcpy(caInVia, pbuf);
    iInCSeq = GrepInteger(p, "\nCSeq: ");
    if ( strstr(p, "\nm=audio ") ) ParseOffer(p);
    SendIncomingOk();
    return;
  }

  // Re-INVITE im laufenden ausgehenden Gespraech (gleiche Call-ID): kein neuer
  // Anruf - also auch kein Klingeln ueber den Filter
  if ( !bIncoming && iRingTime && IsCallId(p, callid) )
  {
    if ( truncated )
      Respond(p, 513, "Message Too Large");
    else
      HandleOutgoingReinvite(p);
    return;
  }

  // Anfrage in einem Dialog, den es nicht (mehr) gibt (To mit Tag, z.B. Re-INVITE
  // nach dem Auflegen): weder Anruf noch Klingeln
  size_t tl = 0;
  const char *to = FindHeader(p, "\nTo: ", &tl);
  if ( to && HasTag(to, tl) )
  {
    Respond(p, 481, "Call/Transaction Does Not Exist");
    return;
  }

  // Nummer des Anrufers merken (zum Einrichten) und pruefen, ob es ein
  // Klingel-Anruf ist: dann abweisen (486 = nur hier besetzt, nicht 6xx -
  // sonst bricht die Anlage evtl. den ganzen Gruppenruf ab) und melden.
  // Vorher leeren: bei unlesbarem From nie die Nummer des vorigen Anrufers pruefen.
  caLastCaller[0] = 0;
  size_t fl = 0;
  const char *from = FindHeader(p, "\nFrom: ", &fl);
  if ( from )
    CallerFrom(from, fl, caLastCaller, sizeof(caLastCaller));
  if ( pRingFilter && caLastCaller[0] && pRingFilter(caLastCaller) )
  {
    Respond(p, 486, "Busy Here");
    // INVITE-Wiederholungen (gleiche Call-ID) nur einmal melden; die Call-ID nur
    // als Hash merken (auch ueberlange werden gemeldet, spart RAM)
    uint32_t h = HeaderHash(p, "\nCall-ID: ");
    if ( h != ringCallIdHash )
    {
      ringCallIdHash = h;
      strcpy(caRingCaller, caLastCaller);
      bRingCall = true;
    }
    return;
  }

  if ( iRingTime )
  {
    Respond(p, 486, "Busy Here");
    return;
  }
  if ( !bAcceptIncoming )
  {
    Respond(p, 603, "Decline");
    return;
  }
  if ( truncated )
  {
    Respond(p, 513, "Message Too Large");
    return;
  }

  // Ab hier werden die Daten des neuen Anrufs gemerkt: ein noch ausstehendes BYE
  // des vorigen eingehenden Anrufs nutzt sie und wird nicht mehr wiederholt
  if ( reqType == REQ_BYE_IN )
    reqType = REQ_NONE;

  pbuf[0] = 0;
  AddCopyAllLines(p, "\nVia: ");
  if ( strlen(pbuf) >= sizeof(caInVia)
       || !HeaderValue(p, "\nCall-ID: ", caInCallId, sizeof(caInCallId))
       || !HeaderValue(p, "\nFrom: ", caInFrom, sizeof(caInFrom))
       || !HeaderValue(p, "\nTo: ", caInTo, sizeof(caInTo))
       || strstr(caInTo, ";tag=") )
  {
    caInCallId[0] = 0;
    Respond(p, 400, "Bad Request");
    return;
  }
  strcpy(caInVia, pbuf);
  iInCSeq = GrepInteger(p, "\nCSeq: ");

  // Ziel fuer unser BYE: URI aus Contact (sonst die Anlage)
  if ( !HeaderUri(p, "\nContact: ", caInContact, sizeof(caInContact)) )
    snprintf(caInContact, sizeof(caInContact), "sip:%s", pSipIp);

  // Rufnummer des Anrufers fuer das Protokoll
  CallerFrom(caInFrom, strlen(caInFrom), caInCaller, sizeof(caInCaller));

  if ( !ParseOffer(p) )
  {
    Respond(p, 488, "Not Acceptable Here");
    return;
  }

  inTag        = Random();
  iInByeCSeq   = 1;
  bIncoming    = true;
  bAnswered    = true;
  iRingTime    = Millis();
  bNewIncoming = true;
  cLastDtmf    = 0;
  SendIncomingOk();
  bInAckPending = true;
  inOkFirstAt   = millis();
  inOkInterval  = 500;
  StartStream(1);   // kurzer Ton: "bitte Code eingeben"
}


// Re-INVITE im laufenden ausgehenden Gespraech (Anlage aendert z.B. das
// Medienziel): mit unserem SDP annehmen und das neue Ziel uebernehmen wie beim
// eingehenden Anruf; ohne passenden Codec (bzw. noch nicht angenommen) 488
void Sip::HandleOutgoingReinvite(const char *p) {

  if ( !bAnswered || ( strstr(p, "\nm=audio ") && !ParseOffer(p) ) )
  {
    Respond(p, 488, "Not Acceptable Here");
    return;
  }
  char sdp[400];
  BuildSdp(sdp, sizeof(sdp), false);
  Respond(p, 200, "OK", sdp);
}


void Sip::SendIncomingOk() {

  char sdp[400];
  BuildSdp(sdp, sizeof(sdp), false);
  pbuf[0] = 0;
  AddSipLine("SIP/2.0 200 OK");
  strncat(pbuf, caInVia, lbuf - strlen(pbuf) - 1);   // enthaelt schon CRLF
  AddSipLine("From: %s", caInFrom);
  AddSipLine("To: %s;tag=%010u", caInTo, inTag);
  AddSipLine("Call-ID: %s", caInCallId);
  AddSipLine("CSeq: %i INVITE", iInCSeq);
  AddSipLine("Contact: <sip:%s@%s:%i;transport=udp>", pSipUser, pMyIp, iMyPort);
  AddSipLine("Allow: INVITE, ACK, CANCEL, BYE, OPTIONS, INFO");
  AddSipLine("Content-Type: application/sdp");
  AddSipLine("Content-Length: %i", (int)strlen(sdp));
  AddSipLine("");
  strncat(pbuf, sdp, lbuf - strlen(pbuf) - 1);
  SendUdp();
  inOkSentAt = millis();
}


// BYE fuer den eingehenden Anruf; Wiederholungen mit gleichem Branch und CSeq
int Sip::ByeIncoming(int cseq) {

  pbuf[0] = 0;
  AddSipLine("BYE %s SIP/2.0", caInContact);
  AddSipLine("Via: SIP/2.0/UDP %s:%i;branch=z9hG4bK%010u;rport", pMyIp, iMyPort, inByeBranch);
  AddSipLine("Max-Forwards: 70");
  AddSipLine("From: %s;tag=%010u", caInTo, inTag);
  AddSipLine("To: %s", caInFrom);
  AddSipLine("Call-ID: %s", caInCallId);
  AddSipLine("CSeq: %i BYE", cseq);
  AddSipLine("Content-Length: 0");
  AddSipLine("");
  return SendUdp();
}


/////////////////////////////////////////////////////////////////////////////////////////////////////
//
// Hardware dependent interface functions
//
/////////////////////////////////////////////////////////////////////////////////////////////////////

uint32_t Sip::Millis() {

  return (uint32_t)millis() + 1;
}


// Generate a 30 bit random number
uint32_t Sip::Random() {

#if defined(ESP32)
  return esp_random() & 0x3fffffff;   // ESP8266 hat secureRandom(), ESP32 nicht
#else
  return secureRandom(0x3fffffff);
#endif
}


// Paket aus pbuf an die Anlage senden. Liefert 0 oder den Grund wie einen
// SIP-Code: 503 = Server-Adresse unbekannt, 513 = zu gross, 500 = UDP-Fehler.
int Sip::SendUdp() {

  size_t l = strlen(pbuf);

  // Groesser als ein Datagramm (der ESP32 teilt es sonst in zwei) oder schon beim
  // Bauen abgeschnitten -> lieber gar nicht senden
  if ( l > SIP_MAX_UDP || l + 3 >= lbuf )
    return 513;
  if ( (uint32_t)sipAddr == 0 )
    return 503;
  // Feste Adresse statt Hostname: kein DNS je Paket (blockiert). Rueckgabe pruefen -
  // beim ESP32 haengt nach gescheitertem beginPacket() write() an das vorige Paket an.
  if ( !Udp.beginPacket(sipAddr, iSipPort) )
    return 500;
  Udp.write((const uint8_t *)pbuf, l);
  int ok = Udp.endPacket();
#ifdef DEBUGLOG
  Serial.printf("\r\n----- send %i bytes -----------------------\r\n%s", (int)l, pbuf);
  Serial.printf("------------------------------------------------\r\n");
#endif

  return ok ? 0 : 500;
}


void Sip::MakeMd5Digest(char *pOutHex33, const char *pIn) {

  MD5Builder aMd5;

  aMd5.begin();
  aMd5.add(pIn);
  aMd5.calculate();
  aMd5.getChars(pOutHex33);
}


// ----- RTP / Beep-Audio (Erweiterung) -------------------------------------

// Lineares 16-bit PCM -> G.711 u-law
uint8_t Sip::Lin2Ulaw(int16_t sample) {
  const uint16_t BIAS = 0x84;
  const int16_t  CLIP = 32635;
  uint8_t sign = (sample >> 8) & 0x80;
  if (sign) sample = -sample;
  if (sample > CLIP) sample = CLIP;
  sample = (int16_t)(sample + BIAS);
  int exponent = 7;
  for (int mask = 0x4000; (sample & mask) == 0 && exponent > 0; exponent--, mask >>= 1)
    ;
  int mantissa = (sample >> (exponent + 3)) & 0x0F;
  return (uint8_t)(~(sign | (exponent << 4) | mantissa));
}

// Lineares 16-bit PCM -> G.711 A-law
uint8_t Sip::Lin2Alaw(int16_t sample) {
  int v = sample >> 3;                          // 13 Bit
  uint8_t mask;
  if (v >= 0) mask = 0xD5;
  else { mask = 0x55; v = -v - 1; }
  if (v > 0xFFF) v = 0xFFF;
  int seg = 0;
  while (seg < 7 && v > (0x20 << seg) - 1) seg++;   // Segmentgrenzen 0x1F, 0x3F ... 0xFFF
  uint8_t aval = (uint8_t)(seg << 4);
  aval |= (seg < 2 ? (v >> 1) : (v >> seg)) & 0x0F;
  return aval ^ mask;
}

// 200 OK ausgewertet: Gegenstellen-RTP-Ziel ermitteln und Streaming starten
void Sip::StartRtp(const char *p) {
  remoteRtpPort = 0;   // keine Werte vom vorherigen Anruf weiterverwenden
  rtpPt  = 0;
  dtmfPt = 101;
  const char *m = strstr(p, "m=audio ");
  if (m) {
    remoteRtpPort = (uint16_t)atoi(m + 8);
    // Erster Payload Type der Antwort ist der gewaehlte Codec
    const char *avp = strstr(m, "RTP/AVP ");
    const char *eol = strchr(m, '\r');
    if (avp && (!eol || avp < eol) && atoi(avp + 8) == 8) rtpPt = 8;
  }
  // DTMF-Payload-Type aus "a=rtpmap:NN telephone-event/8000" uebernehmen
  for (const char *r = strstr(p, "a=rtpmap:"); r; r = strstr(r + 9, "a=rtpmap:")) {
    const char *sp = strchr(r + 9, ' ');
    if (sp && strncmp(sp + 1, "telephone-event", 15) == 0) {
      dtmfPt = (uint8_t)atoi(r + 9);
      break;
    }
  }
  const char *c = strstr(p, "c=IN IP4 ");
  IPAddress a;   // ohne gueltiges Ziel kein Strom (nicht das des vorigen Anrufs)
  if (c) {
    c += 9;
    char ip[24];
    int i = 0;
    while (i < (int)sizeof(ip) - 1 && *c && *c != '\r' && *c != '\n' && *c != ' ')
      ip[i++] = *c++;
    ip[i] = 0;
    if (!a.fromString(ip)) a = IPAddress((uint32_t)0);
  }
  remoteRtpIp = a;
  if (remoteRtpPort == 0 || (uint32_t)remoteRtpIp == 0) return;
  StartStream(iBeepSeconds);
}

// RTP-Strom zum ermittelten Ziel starten; beepSec = Dauer des Pieptons
void Sip::StartStream(int beepSec) {
  if (!bToneReady) {                       // 1000 Hz Ton: 8 Samples/Periode
    for (int k = 0; k < 8; k++) {
      int16_t s = (int16_t)(8000.0f * sinf(2.0f * PI * k / 8.0f));
      ulawTone[k] = Lin2Ulaw(s);
      alawTone[k] = Lin2Alaw(s);
    }
    bToneReady = true;
  }
  if (!bRtpBound) { Rtp.begin(iRtpPort); bRtpBound = true; }
  // Liegengebliebene Pakete verwerfen - z.B. die wiederholten Ende-Pakete der
  // Taste, mit der das vorige Gespraech endete (sonst zaehlt ein alter '*')
  RtpSkipRest();
  while (Rtp.parsePacket() > 0) RtpSkipRest();

  rtpSeq  = (uint16_t)Random();
  rtpTs   = Random();
  rtpSsrc = Random();
  callAnsweredAt = millis();
  lastRtpAt = 0;
  rtpFrame = 0;
  iStreamBeepSec = beepSec;
  bDtmfTsValid = false;
  iRtpRx = 0;
  iRtpForeign = 0;
  foreignRtpIp = IPAddress((uint32_t)0);
  iDtmfRx = 0;
  bInCall = true;
#ifdef DEBUGLOG
  Serial.printf("\r\n----- RTP start -> %s:%i -----\r\n", remoteRtpIp.toString().c_str(), remoteRtpPort);
#endif
}

// Gespraech beenden; eine noch nicht gelesene Taste verfaellt (sonst wuerde sie
// nach dem Auflegen einem anderen Anruf zugerechnet)
void Sip::StopCall() {
  bInCall = false;
  cLastDtmf = 0;
}

// CANCEL/BYE werden wiederholt, bis die Anlage antwortet (siehe HandleTimers())
void Sip::Hangup() {
  if ( !iRingTime )
    return;

  if ( bIncoming )
  {
    inByeBranch = Random();
    StartReq(REQ_BYE_IN, iInByeCSeq++);
  }
  else if ( bAnswered )
    StartReq(REQ_BYE, iInviteCSeq + 1);   // CSeq muss ueber der des INVITE liegen
  else
  {
    StartReq(REQ_CANCEL, iInviteCSeq);    // klingelt noch -> Anruf zurueckziehen
    eCallResult = CALL_NOANSWER;
  }
  StopCall();
  iRingTime = 0;
  bIncoming = false;
  bInAckPending = false;
}

// Sendet alle 20 ms ein RTP-Frame (160 Samples) und legt nach der Gespraechsdauer auf
void Sip::RtpProcessing() {
  if (!bInCall) return;

  // Eingehende RTP-Pakete lesen: DTMF via telephone-event (RFC 4733).
  // Nur Pakete der ausgehandelten Gegenstelle oder der Anlage selbst zaehlen
  // (manche Anlagen senden von einer anderen Adresse als im SDP angegeben).
  int ps;
  while ( (ps = Rtp.parsePacket()) > 0 )
  {
    uint8_t rb[180];
    IPAddress from = Rtp.remoteIP();
    int n = Rtp.read(rb, sizeof(rb));
    RtpSkipRest();
    if ( !(from == remoteRtpIp) && !((uint32_t)sipAddr && from == sipAddr) )
    {
      iRtpForeign++;
      foreignRtpIp = from;
      continue;
    }
    iRtpRx++;
    if ( n < 16 || (rb[0] & 0xC0) != 0x80 )
      continue;   // zu kurz bzw. kein RTP Version 2
    // Ereignis hinter dem Kopf: 12 Bytes + 4 je CSRC, ggf. Erweiterung
    // (4 Bytes Kopf + Laenge in 32-Bit-Worten)
    int off = 12 + 4 * (rb[0] & 0x0F);
    if ( (rb[0] & 0x10) && n >= off + 4 )
      off += 4 + 4 * (((int)rb[off + 2] << 8) | rb[off + 3]);
    // Bei eigenen Anrufen sendet die Gegenstelle laut RFC 3264 mit dem PT
    // unseres Angebots (101), manche Anlagen aber mit dem ihrer Antwort.
    uint8_t pt = rb[1] & 0x7F;
    if ( n >= off + 4 && (pt == dtmfPt || (!bIncoming && pt == 101)) )
    {
      uint8_t  event  = rb[off];
      bool     endbit = rb[off + 1] & 0x80;
      uint32_t ts     = ((uint32_t)rb[4] << 24) | ((uint32_t)rb[5] << 16) | ((uint32_t)rb[6] << 8) | rb[7];
      // Ende-Paket wird 3x gesendet (gleicher Zeitstempel) -> nur einmal zaehlen
      if ( endbit && (!bDtmfTsValid || ts != lastDtmfTs) )
      {
        lastDtmfTs = ts;
        bDtmfTsValid = true;
        char c = 0;
        if ( event <= 9 )       c = '0' + event;
        else if ( event == 10 ) c = '*';
        else if ( event == 11 ) c = '#';
        if ( c ) { cLastDtmf = c; bDtmfIncoming = bIncoming; iDtmfRx++; }
      }
    }
  }

  uint32_t now = millis();
  if (now - callAnsweredAt > (uint32_t)TalkSeconds() * 1000UL) {
    Hangup();
    return;
  }
  // Fester 20-ms-Takt (lastRtpAt = Faelligkeit des naechsten Frames). Nicht auf
  // "now" setzen, sonst driftet der Takt und der Jitterbuffer der Gegenstelle
  // laeuft leer -> hackeliger Ton. Verpasste Frames (loop blockiert) nachholen.
  if (lastRtpAt == 0) lastRtpAt = now;
  int32_t late = (int32_t)(now - lastRtpAt);
  if (late < 0) return;
  if (late > 100) { lastRtpAt = now; late = 0; }   // zu weit hinten: neu aufsetzen

  for (int k = 0; k <= late / 20; k++)
  {
    SendRtpFrame();
    lastRtpAt += 20;
  }
}

// Rest des aktuellen RTP-Pakets verwerfen. Beim ESP32 noetig: solange ein Paket
// nicht ganz gelesen ist, liefert parsePacket() kein neues mehr - ein einziges
// grosses Paket (z.B. 30 ms Audio = 252 Bytes) wuerde den Empfang bis zum
// Neustart blockieren. flush() verwirft nur beim ESP32 (ESP8266: endPacket()).
void Sip::RtpSkipRest() {
  uint8_t junk[64];
  while ( Rtp.available() > 0 && Rtp.read(junk, sizeof(junk)) > 0 ) {}
}

// Laufendes bzw. letztes Gespraech fuer das Protokoll
void Sip::CallInfo(char *out, size_t len) {
  uint32_t a = (uint32_t)remoteRtpIp;
  int l = snprintf(out, len, "%s, DTMF-PT %u, Gegenstelle %u.%u.%u.%u, RTP %u, Tasten %u",
                   rtpPt == 8 ? "PCMA" : "PCMU", dtmfPt,
                   (unsigned)(a & 0xFF), (unsigned)((a >> 8) & 0xFF), (unsigned)((a >> 16) & 0xFF), (unsigned)(a >> 24),
                   iRtpRx, iDtmfRx);
  if ( iRtpForeign && l > 0 && (size_t)l < len )
  {
    uint32_t f = (uint32_t)foreignRtpIp;
    snprintf(out + l, len - l, ", verworfen %u von %u.%u.%u.%u", iRtpForeign,
             (unsigned)(f & 0xFF), (unsigned)((f >> 8) & 0xFF), (unsigned)((f >> 16) & 0xFF), (unsigned)(f >> 24));
  }
}

// Ein RTP-Frame (20 ms, 160 Samples) senden
void Sip::SendRtpFrame() {
  uint8_t pkt[12 + 160];
  pkt[0] = 0x80;
  pkt[1] = rtpPt;                             // Payload Type 0 = PCMU, 8 = PCMA
  if (rtpFrame == 0) pkt[1] |= 0x80;          // Marker beim ersten Frame
  pkt[2] = (rtpSeq >> 8) & 0xFF;
  pkt[3] = rtpSeq & 0xFF;
  pkt[4] = (rtpTs >> 24) & 0xFF;
  pkt[5] = (rtpTs >> 16) & 0xFF;
  pkt[6] = (rtpTs >> 8) & 0xFF;
  pkt[7] = rtpTs & 0xFF;
  pkt[8]  = (rtpSsrc >> 24) & 0xFF;
  pkt[9]  = (rtpSsrc >> 16) & 0xFF;
  pkt[10] = (rtpSsrc >> 8) & 0xFF;
  pkt[11] = rtpSsrc & 0xFF;

  // Muster: 200 ms Ton, 200 ms Pause (10 Frames an, 10 aus) - am Stream-Takt
  // ausgerichtet statt an millis(), damit die Toene sauber geschnitten sind.
  // Nach der Beep-Dauer nur noch Stille (Gespraech bleibt fuer Code-Eingabe offen).
  bool on = (rtpFrame % 20) < 10 && rtpFrame < (uint32_t)iStreamBeepSec * 50;
  const uint8_t *tone   = rtpPt == 8 ? alawTone : ulawTone;
  const uint8_t silence = rtpPt == 8 ? 0xD5 : 0xFF;   // Stille in A-law / u-law
  for (int i = 0; i < 160; i++)
    pkt[12 + i] = on ? tone[i & 7] : silence;

  // Ziel 0.0.0.0 (z.B. Halten per Re-INVITE): nur den Takt weiterzaehlen
  if ((uint32_t)remoteRtpIp != 0 && remoteRtpPort != 0 && Rtp.beginPacket(remoteRtpIp, remoteRtpPort)) {
    Rtp.write(pkt, sizeof(pkt));
    Rtp.endPacket();
  }

  rtpSeq++;
  rtpTs += 160;
  rtpFrame++;
}

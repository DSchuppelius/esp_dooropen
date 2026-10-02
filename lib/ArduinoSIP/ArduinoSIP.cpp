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
  caRingCallId[0] = 0;
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
  iRingTime = 0;
  bIncoming = false;
  bInAckPending = false;
  iMaxTime = MaxDialSec * 1000;
  bRegistered = false;   // neue Zugangsdaten -> neu registrieren
  bRegPending = false;
  iRegStatus = -1;
  iRegGranted = 0;
  regCallId = 0;         // neue Registrierung (neue Call-ID)

  // Server-Adresse fuer die Absenderpruefung eingehender Anfragen
  sipAddr = IPAddress((uint32_t)0);
  if ( SipIp[0] && !sipAddr.fromString(SipIp) )
  {
    IPAddress a;
    if ( WiFi.hostByName(SipIp, a) == 1 )
      sipAddr = a;
  }
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
  iRegExpires  = Expires;
  if ( regCallId == 0 )
  {
    regCallId  = Random();
    regTag     = Random();
  }
  iRegCSeq++;
  bRegAuthTried = false;
  bRegPending   = true;
  SendRegister(false);
}


// REGISTER senden, auf Wunsch mit dem Header aus caAuth
void Sip::SendRegister(bool withAuth) {

  pbuf[0] = 0;
  AddSipLine("REGISTER sip:%s SIP/2.0", pSipIp);
  AddSipLine("Via: SIP/2.0/UDP %s:%i;branch=z9hG4bK%010u;rport", pMyIp, iMyPort, Random());
  AddSipLine("Max-Forwards: 70");
  AddSipLine("From: <sip:%s@%s>;tag=%010u", pSipUser, pSipIp, regTag);
  AddSipLine("To: <sip:%s@%s>", pSipUser, pSipIp);
  AddSipLine("Call-ID: %010u@%s", regCallId, pMyIp);
  AddSipLine("CSeq: %i REGISTER", iRegCSeq);
  AddSipLine("Contact: <sip:%s@%s:%i;transport=udp>", pSipUser, pMyIp, iMyPort);
  if ( withAuth )
    AddSipLine("%s", caAuth);
  AddSipLine("Allow: INVITE, ACK, CANCEL, BYE, OPTIONS, INFO");
  AddSipLine("Expires: %i", iRegExpires);
  AddSipLine("Content-Length: 0");
  AddSipLine("");
  SendUdp();
  regSentAt = millis();
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
    return;
  }

  // Nur einmal mit Digest antworten; erneute Aufforderung = Zugangsdaten falsch
  if ( (code == 401 || code == 407) && !bRegAuthTried )
  {
    char uri[64];
    snprintf(uri, sizeof(uri), "sip:%s", pSipIp);
    if ( BuildAuth(p, "REGISTER", uri) )
    {
      bRegAuthTried = true;
      iRegCSeq++;
      SendRegister(true);
      return;
    }
  }

  // Jede andere Endantwort = fehlgeschlagen; naechster Versuch mit neuer Call-ID
  bRegistered = false;
  bRegPending = false;
  iRegStatus  = code;
  regCallId   = 0;
}


bool Sip::Dial(const char *DialNr, const char *DialDesc) {

  if ( iRingTime )
    return false;

  iDialRetries = 0;
  bAnswered = false;
  bGotResponse = false;
  bIncoming = false;
  eCallResult = CALL_NONE;
  iCallCode = 0;
  pDialNr = DialNr;
  pDialDesc = DialDesc;
  Invite();
  iDialRetries++;
  iRingTime = Millis();

  return true;
}


void Sip::Processing(char *pBuf, size_t lBuf) {

  int packetSize = Udp.parsePacket();
  bool fromServer = false;

  if ( packetSize > 0 )
  {
    // Anfragen (INVITE, BYE, INFO, ...) nur von der Anlage annehmen
    fromServer = (uint32_t)sipAddr == 0 || Udp.remoteIP() == sipAddr;
    pBuf[0] = 0;
    packetSize = Udp.read(pBuf, lBuf - 1);   // Platz fuer die Null am Ende
    if ( packetSize > 0 )
    {
      pBuf[packetSize] = 0;

#ifdef DEBUGLOG
      IPAddress remoteIp = Udp.remoteIP();
      Serial.printf("\r\n----- read %i bytes from: %s:%i ----\r\n", (int)packetSize, remoteIp.toString().c_str(), Udp.remotePort());
      Serial.print(pBuf);
      Serial.printf("----------------------------------------------------\r\n");
#endif

    }
  }

  // REGISTER ohne Antwort -> abbrechen, der Aufrufer versucht es spaeter erneut
  if ( bRegPending && (millis() - regSentAt) > 2000 )
  {
    bRegPending = false;
    bRegistered = false;
    iRegStatus  = 0;   // keine Antwort
    regCallId   = 0;
  }

  HandleUdpPacket((packetSize > 0) ? pBuf : 0, fromServer);
  RtpProcessing();
}


void Sip::HandleUdpPacket(const char *p, bool fromServer) {

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

  if ( !p )
  {
    // INVITE wiederholen, solange noch gar keine Antwort kam (max. 5x)
    if ( iRingTime && !bIncoming && !bGotResponse && iAuthCnt == 0
         && iDialRetries < 5 && iWorkTime > (uint32_t)(iDialRetries * 200) )
    {
      iDialRetries++;
      Invite();
    }

    return;
  }

  bool isResponse = strncmp(p, "SIP/2.0 ", 8) == 0;

  if ( isResponse && IsResponseTo(p, "REGISTER") )
  {
    HandleRegisterResponse(p);
    return;
  }

  // ---------------- Anfragen ----------------
  if ( !isResponse )
  {
    if ( !fromServer )
      return;   // nicht von der Anlage -> ignorieren (Schutz vor gefaelschten Paketen)

    if ( IsRequest(p, "INVITE") )
    {
      HandleIncomingInvite(p);
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
      // DTMF per SIP INFO (application/dtmf-relay): "Signal=*"
      const char *sig = strstr(p, "Signal=");
      if ( sig ) { char c = sig[7]; if ( c == ' ' ) c = sig[8]; if ( c > ' ' ) cLastDtmf = c; }
      Respond(p, 200, "OK");
    }
    return;
  }

  // ---------------- Antworten auf unseren ausgehenden Anruf ----------------
  if ( !IsCallId(p, callid) || !IsResponseTo(p, "INVITE") )
    return;

  int code = atoi(p + 8);
  bGotResponse = true;

  if ( code == 401 || code == 407 )
  {
    Ack(p);
    // call Invite with response data (p) to build auth md5 hashes
    if ( iRingTime )
      Invite(p);
  }
  else if ( code >= 200 && code < 300 )		// OK
  {
    ParseReturnParams(p);
    Ack(p);
    if ( !iRingTime || bIncoming )
    {
      // Angenommen, waehrend unser CANCEL unterwegs war -> sauber beenden
      Bye(iInviteCSeq + 1);
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
    if ( !iRingTime )
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


// Parse parameter value from http formated string
bool Sip::ParseParameter(char *dest, int destlen, const char *name, const char *line, char cq) {

  const char *r = strstr(line, name);

  if ( r == NULL )
    return false;

  r += strlen(name);
  const char *qp = strchr(r, cq);
  if ( qp == NULL )
    return false;   // kein Endzeichen -> kaputtes Paket

  int l = qp - r;
  if ( l < 0 || l >= destlen )
    return false;

  memcpy(dest, r, l);
  dest[l] = 0;
  return true;
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
  return strncmp(pc + 10, id, l) == 0 && ( pc[10 + l] == '\r' || pc[10 + l] == '\n' );
}


// Prueft, ob das Paket (p) genau die Call-ID id traegt
bool Sip::IsCallIdStr(const char *p, const char *id) {

  const char *pc = strstr(p, "\nCall-ID: ");

  if ( !pc || !id[0] )
    return false;

  size_t l = strlen(id);
  return strncmp(pc + 10, id, l) == 0 && ( pc[10 + l] == '\r' || pc[10 + l] == '\n' );
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

  const char *pa = strstr(p, name);

  if ( !pa )
    return false;

  pa += strlen(name);
  const char *pe = strpbrk(pa, "\r\n");
  if ( !pe )
    pe = pa + strlen(pa);

  size_t l = pe - pa;
  if ( l >= destlen )
    return false;

  memcpy(dest, pa, l);
  dest[l] = 0;
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


// Digest-Antwort (RFC 2617, auch mit qop=auth und opaque) nach caAuth schreiben
bool Sip::BuildAuth(const char *p, const char *method, const char *uri) {

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

  int n = snprintf(caAuth, sizeof(caAuth),
           "%s: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\", algorithm=MD5",
           proxy ? "Proxy-Authorization" : "Authorization", pSipUser, realm, nonce, uri, resp);
  if ( n < 0 || n >= (int)sizeof(caAuth) )
    return false;
  if ( useQop )
    n += snprintf(caAuth + n, sizeof(caAuth) - n, ", qop=auth, nc=00000001, cnonce=\"%s\"", cnonce);
  if ( opaque[0] && n < (int)sizeof(caAuth) )
    n += snprintf(caAuth + n, sizeof(caAuth) - n, ", opaque=\"%s\"", opaque);
  return n < (int)sizeof(caAuth);
}


void Sip::Ack(const char *p) {

  char ca[128];

  if ( !ParseParameter(ca, (int)sizeof(ca), "\nTo: <", p, '>') )
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
void Sip::Cancel() {

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
  SendUdp();
}


void Sip::Bye(int cseq) {

  if ( caRead[0] == 0 )
    return;

  pbuf[0] = 0;
  AddSipLine("%s sip:%s@%s SIP/2.0",  "BYE", pDialNr, pSipIp);
  AddSipLine("%s",  caRead);
  AddSipLine("CSeq: %i %s", cseq, "BYE");
  AddSipLine("Max-Forwards: 70");
  AddSipLine("User-Agent: sip-client/0.0.1");
  AddSipLine("Content-Length: 0");
  AddSipLine("");
  SendUdp();
}


// Antwort ohne Inhalt auf eine Anfrage (alle Via-Zeilen, To ggf. mit Tag)
void Sip::Respond(const char *p, int code, const char *reason) {

  pbuf[0] = 0;
  AddSipLine("SIP/2.0 %i %s", code, reason);
  AddCopyAllLines(p, "\nVia: ");
  AddCopySipLine(p, "\nFrom: ");
  char to[160];
  if ( HeaderValue(p, "\nTo: ", to, sizeof(to)) )
  {
    if ( strstr(to, ";tag=") || code < 200 )
      AddSipLine("To: %s", to);
    else
      AddSipLine("To: %s;tag=%010u", to, Random());
  }
  AddCopySipLine(p, "\nCall-ID: ");
  AddCopySipLine(p, "\nCSeq: ");
  AddSipLine("Content-Length: 0");
  AddSipLine("");
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
    inTag, inTag, pMyIp, pMyIp, iRtpPort, rtpPt, pts,
    rtpPt, rtpPt == 8 ? "PCMA" : "PCMU", dtmf);
}


// Call invite without or with the response from peer
void Sip::Invite(const char *p) {

  // prevent loops
  if ( p && iAuthCnt > 3 )
    return;

  int cseq = 1;

  if ( !p )
  {
    iAuthCnt = 0;

    if ( iDialRetries == 0 )
    {
      callid = Random();
      tagid = Random();
      branchid = Random();
    }
  }
  else
  {
    char uri[96];
    snprintf(uri, sizeof(uri), "sip:%s@%s", pDialNr, pSipIp);
    if ( !BuildAuth(p, "INVITE", uri) )
    {
      caRead[0] = 0;
      return;
    }
    cseq = iInviteCSeq + 1;
    branchid = Random();   // neue Transaktion -> neuer Branch
  }

  pbuf[0] = 0;
  AddSipLine("INVITE sip:%s@%s SIP/2.0", pDialNr, pSipIp);
  AddSipLine("Call-ID: %010u@%s",  callid, pMyIp);
  AddSipLine("CSeq: %i INVITE",  cseq);
  AddSipLine("Max-Forwards: 70");
  // not needed for fritzbox
  // AddSipLine("User-Agent: sipdial by jl");
  AddSipLine("From: \"%s\"  <sip:%s@%s>;tag=%010u", pDialDesc, pSipUser, pSipIp, tagid);
  AddSipLine("Via: SIP/2.0/UDP %s:%i;branch=z9hG4bK%010u;rport=%i", pMyIp, iMyPort, branchid, iMyPort);
  AddSipLine("To: <sip:%s@%s>", pDialNr, pSipIp);
  AddSipLine("Contact: \"%s\" <sip:%s@%s:%i;transport=udp>", pSipUser, pSipUser, pMyIp, iMyPort);

  if ( p )
  {
    AddSipLine("%s", caAuth);
    iAuthCnt++;
  }

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
  caRead[0] = 0;
  iInviteCSeq = cseq;   // fuer CANCEL (gleiche Nummer) und BYE (hoeher)
  SendUdp();
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

  rtpPt  = hasA ? 8 : 0;
  dtmfPt = 0xFF;   // ohne telephone-event nur DTMF per SIP INFO
  for ( const char *r = strstr(p, "a=rtpmap:"); r; r = strstr(r + 9, "a=rtpmap:") )
  {
    const char *sp = strchr(r + 9, ' ');
    if ( sp && strncmp(sp + 1, "telephone-event", 15) == 0 )
    {
      dtmfPt = (uint8_t)atoi(r + 9);
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
  if ( !remoteRtpIp.fromString(ip) )
    return false;
  remoteRtpPort = port;
  return true;
}


// Rufnummer aus der From-Zeile ("sip:NUMMER@...") nach out
static void CallerFrom(const char *from, char *out, size_t len) {
  out[0] = 0;
  const char *u = strstr(from, "sip:");
  if ( !u )
    return;
  u += 4;
  const char *at = strpbrk(u, "@;>");
  size_t l = at ? (size_t)(at - u) : strlen(u);
  if ( l >= len ) l = len - 1;
  memcpy(out, u, l);
  out[l] = 0;
}


void Sip::HandleIncomingInvite(const char *p) {

  // Re-INVITE im laufenden eingehenden Gespraech (z.B. Media-Wechsel)
  if ( bIncoming && iRingTime && IsCallIdStr(p, caInCallId) )
  {
    pbuf[0] = 0;
    AddCopyAllLines(p, "\nVia: ");
    if ( strlen(pbuf) < sizeof(caInVia) ) strcpy(caInVia, pbuf);
    iInCSeq = GrepInteger(p, "\nCSeq: ");
    if ( strstr(p, "\nm=audio ") ) ParseOffer(p);
    SendIncomingOk();
    return;
  }

  // Nummer des Anrufers merken (zum Einrichten) und pruefen, ob es ein
  // Klingel-Anruf ist: dann abweisen (486 = nur hier besetzt, nicht 6xx -
  // sonst bricht die Anlage evtl. den ganzen Gruppenruf ab) und melden
  char from[160], callId[100];
  if ( HeaderValue(p, "\nFrom: ", from, sizeof(from)) )
    CallerFrom(from, caLastCaller, sizeof(caLastCaller));
  if ( pRingFilter && pRingFilter(caLastCaller) )
  {
    Respond(p, 486, "Busy Here");
    // INVITE-Wiederholungen (gleiche Call-ID) nur einmal melden
    if ( HeaderValue(p, "\nCall-ID: ", callId, sizeof(callId)) && strcmp(callId, caRingCallId) != 0 )
    {
      strcpy(caRingCallId, callId);
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

  char contact[160];
  pbuf[0] = 0;
  AddCopyAllLines(p, "\nVia: ");
  if ( strlen(pbuf) >= sizeof(caInVia)
       || !HeaderValue(p, "\nCall-ID: ", caInCallId, sizeof(caInCallId))
       || !HeaderValue(p, "\nFrom: ", caInFrom, sizeof(caInFrom))
       || !HeaderValue(p, "\nTo: ", caInTo, sizeof(caInTo))
       || strstr(caInTo, ";tag=") )
  {
    Respond(p, 400, "Bad Request");
    return;
  }
  strcpy(caInVia, pbuf);
  iInCSeq = GrepInteger(p, "\nCSeq: ");

  // Ziel fuer unser BYE: URI aus Contact (sonst die Anlage)
  caInContact[0] = 0;
  if ( HeaderValue(p, "\nContact: ", contact, sizeof(contact)) )
  {
    const char *s = strchr(contact, '<');
    s = s ? s + 1 : contact;
    const char *e = strpbrk(s, s == contact ? ";> " : ">");
    size_t l = e ? (size_t)(e - s) : strlen(s);
    if ( l < sizeof(caInContact) ) { memcpy(caInContact, s, l); caInContact[l] = 0; }
  }
  if ( !caInContact[0] )
    snprintf(caInContact, sizeof(caInContact), "sip:%s", pSipIp);

  // Rufnummer des Anrufers fuer das Protokoll
  CallerFrom(caInFrom, caInCaller, sizeof(caInCaller));

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
  SendIncomingOk();
  bInAckPending = true;
  inOkFirstAt   = millis();
  inOkInterval  = 500;
  StartStream(1);   // kurzer Ton: "bitte Code eingeben"
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


void Sip::ByeIncoming() {

  pbuf[0] = 0;
  AddSipLine("BYE %s SIP/2.0", caInContact);
  AddSipLine("Via: SIP/2.0/UDP %s:%i;branch=z9hG4bK%010u;rport", pMyIp, iMyPort, Random());
  AddSipLine("Max-Forwards: 70");
  AddSipLine("From: %s;tag=%010u", caInTo, inTag);
  AddSipLine("To: %s", caInFrom);
  AddSipLine("Call-ID: %s", caInCallId);
  AddSipLine("CSeq: %i BYE", iInByeCSeq++);
  AddSipLine("Content-Length: 0");
  AddSipLine("");
  SendUdp();
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


int Sip::SendUdp() {

  Udp.beginPacket(pSipIp, iSipPort);
  Udp.write((const uint8_t *)pbuf, strlen(pbuf));
  Udp.endPacket();
#ifdef DEBUGLOG
  Serial.printf("\r\n----- send %i bytes -----------------------\r\n%s", strlen(pbuf), pbuf);
  Serial.printf("------------------------------------------------\r\n");
#endif

  return 0;
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
  if (c) {
    c += 9;
    char ip[24];
    int i = 0;
    while (i < (int)sizeof(ip) - 1 && *c && *c != '\r' && *c != '\n' && *c != ' ')
      ip[i++] = *c++;
    ip[i] = 0;
    remoteRtpIp.fromString(ip);
  }
  if (remoteRtpPort == 0) return;
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

  rtpSeq  = (uint16_t)Random();
  rtpTs   = Random();
  rtpSsrc = Random();
  callAnsweredAt = millis();
  lastRtpAt = 0;
  rtpFrame = 0;
  iStreamBeepSec = beepSec;
  bDtmfTsValid = false;
  bInCall = true;
#ifdef DEBUGLOG
  Serial.printf("\r\n----- RTP start -> %s:%i -----\r\n", remoteRtpIp.toString().c_str(), remoteRtpPort);
#endif
}

void Sip::StopCall() {
  bInCall = false;
}

void Sip::Hangup() {
  if ( !iRingTime )
    return;

  if ( bIncoming )
    ByeIncoming();
  else if ( bAnswered )
    Bye(iInviteCSeq + 1);   // CSeq muss ueber der des INVITE liegen
  else
  {
    Cancel();               // klingelt noch -> Anruf zurueckziehen
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
  // Nur Pakete von der ausgehandelten Gegenstelle zaehlen.
  int ps;
  while ( (ps = Rtp.parsePacket()) > 0 )
  {
    uint8_t rb[180];
    bool fromPeer = Rtp.remoteIP() == remoteRtpIp;
    int n = Rtp.read(rb, sizeof(rb));
    if ( fromPeer && n >= 16 && (rb[1] & 0x7F) == dtmfPt )
    {
      uint8_t  event  = rb[12];
      bool     endbit = rb[13] & 0x80;
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
        if ( c ) cLastDtmf = c;
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

  Rtp.beginPacket(remoteRtpIp, remoteRtpPort);
  Rtp.write(pkt, sizeof(pkt));
  Rtp.endPacket();

  rtpSeq++;
  rtpTs += 160;
  rtpFrame++;
}

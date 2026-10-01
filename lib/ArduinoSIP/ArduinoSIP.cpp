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

}


Sip::~Sip() {
  
}


void Sip::Init(const char *SipIp, int SipPort, const char *MyIp, int MyPort, const char *SipUser, const char *SipPassWd, int MaxDialSec) {
  
  Udp.begin(SipPort);
  
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
  iMaxTime = MaxDialSec * 1000;
  bRegistered = false;   // neue Zugangsdaten -> neu registrieren
  bRegPending = false;
  iRegStatus = -1;
}


// Nicht-blockierendes REGISTER mit Digest-Auth (Erweiterung ggue. Original).
// Sendet REGISTER; die Antworten (401/407 -> erneut mit Digest, 200 OK) werden
// in Processing() ausgewertet, sodass die Loop dabei weiterlaeuft.
void Sip::StartRegister(int Expires) {

  iRegExpires  = Expires;
  regCallId    = Random();
  regTag       = Random();
  iRegCSeq     = 1;
  bRegPending  = true;
  SendRegister(0);
}


// REGISTER senden; pAuth = fertiger Authorization-Header oder 0
void Sip::SendRegister(const char *pAuth) {

  pbuf[0] = 0;
  AddSipLine("REGISTER sip:%s SIP/2.0", pSipIp);
  AddSipLine("Via: SIP/2.0/UDP %s:%i;branch=%010u;rport", pMyIp, iMyPort, Random());
  AddSipLine("Max-Forwards: 70");
  AddSipLine("From: <sip:%s@%s>;tag=%010u", pSipUser, pSipIp, regTag);
  AddSipLine("To: <sip:%s@%s>", pSipUser, pSipIp);
  AddSipLine("Call-ID: %010u@%s", regCallId, pMyIp);
  AddSipLine("CSeq: %i REGISTER", iRegCSeq);
  AddSipLine("Contact: <sip:%s@%s:%i;transport=udp>", pSipUser, pMyIp, iMyPort);
  if ( pAuth )
    AddSipLine("%s", pAuth);
  AddSipLine("Expires: %i", iRegExpires);
  AddSipLine("Content-Length: 0");
  AddSipLine("");
  SendUdp();
  regSentAt = millis();
}


void Sip::HandleRegisterResponse(const char *p) {

  if ( !bRegPending || !IsCallId(p, regCallId) )
    return;   // verspaetete Antwort einer frueheren Registrierung

  if ( strstr(p, "SIP/2.0 1") == p )   // 1xx: weiter warten
    return;

  if ( strstr(p, "SIP/2.0 200") == p )
  {
    bRegistered = true;
    bRegPending = false;
    iRegStatus  = 200;
    return;
  }

  bool challenge = strstr(p, "SIP/2.0 401") == p || strstr(p, "SIP/2.0 407") == p;
  char caRealm[128];
  char caNonce[160];

  // Nur einmal mit Digest antworten; kein fuehrendes Leerzeichen bei realm/nonce:
  // manche Server (z.B. LANCOM) trennen ohne Space
  if ( challenge && iRegCSeq == 1
       && ParseParameter(caRealm, (int)sizeof(caRealm), "realm=\"", p)
       && ParseParameter(caNonce, (int)sizeof(caNonce), "nonce=\"", p) )
  {
    char ha1Hex[33], ha2Hex[33], haResp[33], temp[256];
    snprintf(temp, sizeof(temp), "%s:%s:%s", pSipUser, caRealm, pSipPassWd);
    MakeMd5Digest(ha1Hex, temp);
    snprintf(temp, sizeof(temp), "REGISTER:sip:%s", pSipIp);
    MakeMd5Digest(ha2Hex, temp);
    snprintf(temp, sizeof(temp), "%s:%s:%s", ha1Hex, caNonce, ha2Hex);
    MakeMd5Digest(haResp, temp);

    char auth[400];
    snprintf(auth, sizeof(auth),
             "%s: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"sip:%s\", response=\"%s\"",
             strstr(p, "Proxy-Authenticate") ? "Proxy-Authorization" : "Authorization",
             pSipUser, caRealm, caNonce, pSipIp, haResp);
    iRegCSeq++;
    SendRegister(auth);
    return;
  }

  // Jede andere Endantwort (oder erneute Auth-Aufforderung) = fehlgeschlagen
  bRegistered = false;
  bRegPending = false;
  iRegStatus  = atoi(p + 8);   // "SIP/2.0 403 ..." -> 403
}


bool Sip::Dial(const char *DialNr, const char *DialDesc) {
  
  if ( iRingTime )
    return false;

  iDialRetries = 0;
  bAnswered = false;
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
  
  if ( packetSize > 0 )
  {
    pBuf[0] = 0;
    packetSize = Udp.read(pBuf, lBuf);
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
  }

  HandleUdpPacket((packetSize > 0) ? pBuf : 0 );
  RtpProcessing();
}


void Sip::HandleUdpPacket(const char *p) {
  
  uint32_t iWorkTime = iRingTime ? (Millis() - iRingTime) : 0;

  // Klingelt zu lange -> CANCEL. Nach dem Abheben gilt die Gespraechsdauer;
  // die beendet RtpProcessing(), hier nur die Absicherung ohne RTP-Strom.
  if ( iRingTime && !bAnswered && iWorkTime > iMaxTime )
    Hangup();
  else if ( iRingTime && bAnswered && !bInCall
            && (millis() - callAnsweredAt) > (uint32_t)TalkSeconds() * 1000UL )
    Hangup();

  if ( !p )
  {
    // max 5 dial retry when loos first invite packet
    if ( iAuthCnt == 0 && iDialRetries < 5 && iWorkTime > (iDialRetries * 200) )
    {
      iDialRetries++;
      delay(30);
      Invite();
    }
	
    return;
  }

  if ( IsResponseTo(p, "REGISTER") )
  {
    HandleRegisterResponse(p);
    return;
  }

  // Requests eines alten Dialogs (z.B. verspaetetes BYE) noch hoeflich mit
  // 200 OK beantworten, aber den laufenden Anruf nicht anfassen.
  if ( !IsCallId(p, callid) )
  {
    if ( strstr(p, "BYE") == p || strstr(p, "INFO") == p )
      Ok(p);
    return;
  }

  if ( strstr(p, "SIP/2.0 401 Unauthorized") == p
    || strstr(p, "SIP/2.0 407 ") == p )   // Proxy Authentication Required
  {
    Ack(p);
    // call Invite with response data (p) to build auth md5 hashes
    Invite(p);
  }
  else if ( strstr(p, "BYE") == p )
  {
    Ok(p);
    StopCall();
    iRingTime = 0;
  }
  else if ( strstr(p, "SIP/2.0 200") == p )		// OK
  {
    // Nur das 200 OK auf unser INVITE auswerten. Ein 200 OK auf BYE/INFO
    // darf weder ACKt werden noch (z.B. wegen "Allow: INVITE, ...") das
    // Beep-Audio erneut starten - sonst legt der Geister-Timer spaeter
    // den naechsten Anruf per BYE auf.
    if ( IsResponseTo(p, "INVITE") )
    {
      ParseReturnParams(p);
      Ack(p);
      if ( !iRingTime )
      {
        // Angenommen, waehrend unser CANCEL unterwegs war -> sauber beenden
        Bye(iInviteCSeq + 1);
        return;
      }
      bAnswered = true;
      eCallResult = CALL_ANSWERED;
      callAnsweredAt = millis();
      // Anruf angenommen, Beep-Audio starten
      if ( iBeepSeconds > 0 && !bInCall )
        StartRtp(p);
    }
  }
  else if (    strstr(p, "SIP/2.0 183 ") == p 	// Session Progress
            || strstr(p, "SIP/2.0 180 ") == p 	// Ringing
            || strstr(p, "SIP/2.0 100 ") == p )	// Trying (vorlaeufig: kein ACK)
  {
    ParseReturnParams(p);
  }
  else if (    strstr(p, "SIP/2.0 486 ") == p 	// Busy Here
            || strstr(p, "SIP/2.0 600 ") == p 	// Busy Everywhere
            || strstr(p, "SIP/2.0 603 ") == p 	// Decline
            || strstr(p, "SIP/2.0 487 ") == p) 	// Request Terminatet
  {
    Ack(p);
    StopCall();
    iRingTime = 0;
    if ( strstr(p, "SIP/2.0 603 ") == p )      eCallResult = CALL_DECLINED;
    else if ( strstr(p, "SIP/2.0 487 ") != p ) eCallResult = CALL_BUSY;
    // 487 = Antwort auf unser CANCEL: Ergebnis hat Hangup() schon gesetzt
  }
  else if ( strstr(p, "SIP/2.0 ") == p && IsResponseTo(p, "INVITE") && atoi(p + 8) >= 300 )
  {
    // Sonstige Ablehnung (404 unbekannt, 480 nicht erreichbar, 503, ...):
    // sofort beenden statt bis zum Timeout zu warten
    Ack(p);
    StopCall();
    iRingTime = 0;
    eCallResult = CALL_FAILED;
    iCallCode = atoi(p + 8);
  }
  else if (strstr(p, "INFO") == p)
  {
    iLastCSeq = GrepInteger(p, "\nCSeq: ");
    // DTMF per SIP INFO (application/dtmf-relay): "Signal=*"
    const char *sig = strstr(p, "Signal=");
    if ( sig ) { char c = sig[7]; if ( c == ' ' ) c = sig[8]; if ( c > ' ' ) cLastDtmf = c; }
    Ok(p);
  }

}


void Sip::AddSipLine(const char* constFormat , ... ) {
  
  va_list arglist;
  va_start(arglist, constFormat);
  uint16_t l = (uint16_t)strlen(pbuf);
  char *p = pbuf + l;
  vsnprintf(p, lbuf - l, constFormat, arglist );
  va_end(arglist);
  l = (uint16_t)strlen(pbuf);
  if ( l < (lbuf - 2) )
  {
    pbuf[l] = '\r';
    pbuf[l + 1] = '\n';
    pbuf[l + 2] = 0;
  }
}


// Search a line in response date (p) and append on pbuf
bool Sip::AddCopySipLine(const char *p, const char *psearch) {

  char *pa = strstr((char*)p, psearch);

  if ( pa )
  {
    char *pe = strstr(pa, "\r");

    if ( pe == 0 )
      pe = strstr(pa, "\n");

    if ( pe > pa )
    {
      char c = *pe;
      *pe = 0;
      AddSipLine("%s", pa);
      *pe = c;
	  
      return true;
    }
  }
  
  return false;
}


// Parse parameter value from http formated string
bool Sip::ParseParameter(char *dest, int destlen, const char *name, const char *line, char cq) {

  const char *qp;
  const char *r;

  if ( ( r = strstr(line, name) ) != NULL )
  {
    r = r + strlen(name);
    qp = strchr(r, cq);
    int l = qp - r;
    if ( l < destlen )
    {
      strncpy(dest, r, l);
      dest[l] = 0;
	  
      return true;
    }
  }
  
  return false;
}


// Copy Call-ID, From, Via and To from response to caRead using later for BYE or CANCEL the call
bool Sip::ParseReturnParams(const char *p) {
  
  pbuf[0] = 0;
  
  AddCopySipLine(p, "Call-ID: ");
  AddCopySipLine(p, "From: ");
  AddCopySipLine(p, "Via: ");
  AddCopySipLine(p, "To: ");
  
  if ( strlen(pbuf) >= 2 )
  {
    strcpy(caRead, pbuf);
    caRead[strlen(caRead) - 2] = 0;
  }
  
  return true;
}


// Prueft, ob das Paket (p) die Call-ID "<id>@<eigene IP>" traegt
bool Sip::IsCallId(const char *p, uint32_t id32) {

  const char *pc = strstr(p, "\nCall-ID: ");

  if ( !pc )
    return false;

  char id[40];
  snprintf(id, sizeof(id), "%010u@%s", id32, pMyIp);
  return strncmp(pc + 10, id, strlen(id)) == 0;
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


int Sip::GrepInteger(const char *p, const char *psearch) {

  int param = -1;
  const char *pc = strstr(p, psearch);

  if ( pc )
  {
    param = atoi(pc + strlen(psearch));
  }
  
  return param;
}


void Sip::Ack(const char *p) {
  
  char ca[32];
 
  bool b = ParseParameter(ca, (int)sizeof(ca), "To: <", p, '>');
 
  if ( !b )
    return;

  pbuf[0] = 0;
  AddSipLine("ACK %s SIP/2.0", ca);
  AddCopySipLine(p, "Call-ID: ");
  int cseq = GrepInteger(p, "\nCSeq: ");
  AddSipLine("CSeq: %i ACK",  cseq);
  AddCopySipLine(p, "From: ");
  AddCopySipLine(p, "Via: ");
  AddCopySipLine(p, "To: ");
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
  AddSipLine("Via: SIP/2.0/UDP %s:%i;branch=%010u;rport=%i", pMyIp, iMyPort, branchid, iMyPort);
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


void Sip::Ok(const char *p) {
  
  pbuf[0] = 0;
  AddSipLine("SIP/2.0 200 OK");
  AddCopySipLine(p, "Call-ID: ");
  AddCopySipLine(p, "CSeq: ");
  AddCopySipLine(p, "From: ");
  AddCopySipLine(p, "Via: ");
  AddCopySipLine(p, "To: ");
  AddSipLine("Content-Length: 0");
  AddSipLine("");
  SendUdp();
}


// Call invite without or with the response from peer
void Sip::Invite(const char *p) {

  // prevent loops
  if ( p && iAuthCnt > 3 )
    return;

  // using caRead for temp. store realm and nonce
  char *caRealm = caRead;
  char *caNonce = caRead + 128;

  char *haResp = 0;
  int   cseq = 1;
  
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
    cseq = 2;
    
	if (    ParseParameter(caRealm, 128, "realm=\"", p)
         && ParseParameter(caNonce, 128, "nonce=\"", p) )
    {
      // using output buffer to build the md5 hashes
      // store the md5 haResp to end of buffer
      char *ha1Hex = pbuf;
      char *ha2Hex = pbuf + 33;
      haResp = pbuf + lbuf - 34;
      char *pTemp = pbuf + 66;

      snprintf(pTemp, lbuf - 100, "%s:%s:%s", pSipUser, caRealm, pSipPassWd);
      MakeMd5Digest(ha1Hex, pTemp);

      snprintf(pTemp, lbuf - 100, "INVITE:sip:%s@%s", pDialNr, pSipIp);
      MakeMd5Digest(ha2Hex, pTemp);

      snprintf(pTemp, lbuf - 100, "%s:%s:%s", ha1Hex, caNonce, ha2Hex);
      MakeMd5Digest(haResp, pTemp);
    }
    else
    {
      caRead[0] = 0;
	  
      return;
    }
  }
  
  pbuf[0] = 0;
  AddSipLine("INVITE sip:%s@%s SIP/2.0", pDialNr, pSipIp);
  AddSipLine("Call-ID: %010u@%s",  callid, pMyIp);
  AddSipLine("CSeq: %i INVITE",  cseq);
  AddSipLine("Max-Forwards: 70");
  // not needed for fritzbox
  // AddSipLine("User-Agent: sipdial by jl");
  AddSipLine("From: \"%s\"  <sip:%s@%s>;tag=%010u", pDialDesc, pSipUser, pSipIp, tagid);
  AddSipLine("Via: SIP/2.0/UDP %s:%i;branch=%010u;rport=%i", pMyIp, iMyPort, branchid, iMyPort);
  AddSipLine("To: <sip:%s@%s>", pDialNr, pSipIp);
  AddSipLine("Contact: \"%s\" <sip:%s@%s:%i;transport=udp>", pSipUser, pSipUser, pMyIp, iMyPort);
  
  if ( p )
  {
    // authentication: bei 407 ist es Proxy-Authorization, sonst Authorization
    const char *authHdr = strstr(p, "Proxy-Authenticate") ? "Proxy-Authorization" : "Authorization";
    AddSipLine("%s: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"sip:%s@%s\", response=\"%s\"", authHdr, pSipUser, caRealm, caNonce, pDialNr, pSipIp, haResp);
    iAuthCnt++;
  }
  
  if ( iBeepSeconds > 0 )
  {
    // SDP-Angebot (early offer): G.711 A-law und u-law auf unserem RTP-Port.
    // Viele europaeische Anlagen akzeptieren nur PCMA (sonst 488 Not Acceptable).
    char sdp[340];
    snprintf(sdp, sizeof(sdp),
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
	
  // return ((((uint32_t)rand())&0x7fff)<<15) + ((((uint32_t)rand())&0x7fff));
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


void Sip::MakeMd5Digest(char *pOutHex33, char *pIn) {
  
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

  if ( bAnswered )
    Bye(iInviteCSeq + 1);   // CSeq muss ueber der des INVITE liegen
  else
  {
    Cancel();               // klingelt noch -> Anruf zurueckziehen
    eCallResult = CALL_NOANSWER;
  }
  StopCall();
  iRingTime = 0;
}

// Sendet alle 20 ms ein RTP-Frame (160 u-law Samples) und legt nach der Beep-Dauer auf
void Sip::RtpProcessing() {
  if (!bInCall) return;

  // Eingehende RTP-Pakete lesen: DTMF via telephone-event (PT 101, RFC 4733)
  int ps;
  while ( (ps = Rtp.parsePacket()) > 0 )
  {
    uint8_t rb[180];
    int n = Rtp.read(rb, sizeof(rb));
    if ( n >= 16 && (rb[1] & 0x7F) == dtmfPt )
    {
      uint8_t event  = rb[12];
      bool    endbit = rb[13] & 0x80;
      if ( endbit && (millis() - lastDtmfAt) > 400 )
      {
        lastDtmfAt = millis();
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

// Ein RTP-Frame (20 ms, 160 u-law Samples) senden
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
  bool on = (rtpFrame % 20) < 10 && rtpFrame < (uint32_t)iBeepSeconds * 50;
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


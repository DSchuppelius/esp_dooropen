/* ====================================================================

   Copyright (c) 2018 Juergen Liegner  All rights reserved.
   (https://www.mikrocontroller.net/topic/444994)

   Copyright (c) 2019 Thorsten Godau (dl9sec)
   (Created an Arduino library encapsulation from the original code and did
   some beautification)

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
#ifndef ARDUINO_SIP_H
#define ARDUINO_SIP_H

#if defined(ARDUINO) && ARDUINO >= 100
  #include "Arduino.h"
#else
  #include "WProgram.h"
#endif

#include <WiFiUdp.h>

class Sip
{
  public:
    Sip(char *pBuf, size_t lBuf);
	~Sip();


	void        Init(const char *SipIp, int SipPort, const char *MyIp, int MyPort, const char *SipUser, const char *SipPassWd, int MaxDialSec = 10);
    bool        Dial(const char *DialNr, const char *DialDesc = "");
	void		Processing(char *pBuf, size_t lBuf);
    bool        IsBusy() { return iRingTime != 0; }
    // Nicht-blockierendes REGISTER mit Digest-Auth; Ergebnis per IsRegistered(),
    // sobald IsRegistering() false ist (Antworten laufen ueber Processing()). (Erweiterung)
    void        StartRegister(int Expires = 3600);
    bool        IsRegistering() { return bRegPending; }
    // Angemeldet, solange die zuletzt gewaehrte Gueltigkeit laeuft - auch wenn eine
    // Erneuerung gerade scheiterte (Paketverlust, Anlage kurz weg): die Bindung beim
    // Registrar gilt bis dahin weiter.
    bool        IsRegistered() { return bRegistered && (millis() - regOkAt) < regValidMs; }
    // Beep-Dauer (Sekunden) fuer den RTP-Ton nach dem Abheben; 0 = kein Audio.
    void        SetBeepSeconds(int s) { iBeepSeconds = s; }
    // Liefert die zuletzt empfangene DTMF-Taste (0 wenn keine) und loescht sie.
    // incoming: stammt sie aus einem eingehenden Anruf? Beim Empfang gemerkt -
    // IsIncoming() kann im selben Durchlauf schon false sein (aufgelegt).
    char        ReadDtmf(bool *incoming = nullptr) {
      char c = cLastDtmf; cLastDtmf = 0;
      if ( incoming ) *incoming = bDtmfIncoming;
      return c;
    }
    // Anruf sofort beenden: BYE wenn angenommen, sonst CANCEL. (Erweiterung)
    void        Hangup();
    // CANCEL/BYE eines beendeten Anrufs wird noch wiederholt (Antwort steht aus)
    bool        IsClosing() { return reqType >= REQ_CANCEL; }
    // Gespraechsdauer nach dem Abheben (Sekunden), mindestens die Beep-Dauer;
    // laenger z.B. fuer die Eingabe eines Oeffnungs-Codes. (Erweiterung)
    void        SetCallSeconds(int s) { iCallSeconds = s; }
    // Ergebnis der letzten Registrierung: -1 = noch keine, 0 = keine Antwort,
    // -2 = Server-Adresse nicht aufloesbar (DNS), sonst SIP-Statuscode (200 = ok,
    // 401 nach Digest = Zugangsdaten falsch, ...). Zeigt auch den Fehler einer
    // gescheiterten Erneuerung, waehrend IsRegistered() noch true ist.
    int         RegisterStatus() { return iRegStatus; }
    // Vom Registrar gewaehrte Gueltigkeit (s) der letzten Registrierung, 0 = unbekannt
    int         RegisterExpires() { return iRegGranted; }
    // Ergebnis des letzten Anrufs (gueltig, sobald IsBusy() false ist)
    enum CallResult { CALL_NONE, CALL_ANSWERED, CALL_NOANSWER, CALL_BUSY, CALL_DECLINED, CALL_FAILED };
    CallResult  LastCallResult() { return eCallResult; }
    int         LastCallCode() { return iCallCode; }   // SIP-Code bei CALL_FAILED

    // Eingehende Anrufe (Erweiterung): angenommen wird nur, wenn erlaubt und
    // kein anderer Anruf laeuft. Nach dem Annehmen kurzer Ton, dann Code-Eingabe.
    void        SetAcceptIncoming(bool on) { bAcceptIncoming = on; }
    void        SetIncomingSeconds(int s) { iIncomingSeconds = s; }
    bool        IsIncoming() { return bIncoming && iRingTime != 0; }
    // true genau einmal nach dem Annehmen eines Anrufs; Nummer des Anrufers in IncomingFrom()
    bool        NewIncoming() { bool b = bNewIncoming; bNewIncoming = false; return b; }
    const char *IncomingFrom() { return caInCaller; }

    // Klingeln per Anruf (Erweiterung): liefert der Filter fuer die Nummer des
    // Anrufers true, wird der Anruf mit "486 Busy Here" abgewiesen und als Klingeln
    // gemeldet (RingCall() einmal true). So kann z.B. eine TK-Anlage beim Klingeln
    // den Tueroeffner wie ein Telefon anrufen.
    typedef bool (*RingFilter)(const char *caller);
    void        SetRingFilter(RingFilter f) { pRingFilter = f; }
    bool        RingCall() { bool b = bRingCall; bRingCall = false; return b; }
    const char *RingCaller() { return caRingCaller; }
    // Nummer des letzten Anrufers (egal ob angenommen) - zum Einrichten
    const char *LastCaller() { return caLastCaller; }

    // Diagnose (Erweiterung): laeuft gerade RTP? CallInfo() beschreibt das
    // laufende bzw. letzte Gespraech (Codec, DTMF, empfangene Pakete).
    bool        InCall() { return bInCall; }
    void        CallInfo(char *out, size_t len);

  private:
    char       *pbuf;
    size_t      lbuf;
    char        caRead[512];      // Call-ID/From/Via/To fuer BYE eines ausgehenden Anrufs
    char        caAuth[512];      // fertiger (Proxy-)Authorization-Header des INVITE
    char        caRegAuth[512];   // ... des REGISTER (beide werden unveraendert wiederholt)
    bool        bRegistered = false;

    // REGISTER-Zustand (Erweiterung)
    bool        bRegPending = false;
    bool        bRegAuthTried = false;
    uint32_t    regSentAt = 0;
    uint32_t    regTxAt = 0;      // erste Sendung der laufenden Transaktion
    uint32_t    regOkAt = 0;      // Beginn der zuletzt bestaetigten Registrierung
    uint32_t    regValidMs = 0;   // deren Gueltigkeit
    uint32_t    regBranch = 0;    // Wiederholungen mit gleichem Branch
    uint32_t    regInterval = 500;
    uint8_t     regSends = 0;
    uint32_t    regCallId = 0;
    uint32_t    regTag = 0;
    int         iRegCSeq = 0;
    int         iRegExpires = 3600;
    int         iRegStatus = -1;
    int         iRegGranted = 0;

    // Anruf-Ergebnis / Gespraechsdauer (Erweiterung)
    CallResult  eCallResult = CALL_NONE;
    int         iCallCode = 0;
    int         iCallSeconds = 0;

    const char *pSipIp;
    int         iSipPort;
    IPAddress   sipAddr;          // aufgeloeste Server-Adresse (Ziel, Absenderpruefung)
    bool        bSipHost = false; // Server als Hostname: je REGISTER neu aufloesen
    const char *pSipUser;
    const char *pSipPassWd;
    const char *pMyIp;
    int         iMyPort;
    const char *pDialNr;
    const char *pDialDesc;

    uint32_t    callid;
    uint32_t    tagid;
    uint32_t    branchid;

    int         iAuthCnt;
    uint32_t    iRingTime;
    uint32_t    iMaxTime;
    int         iInviteCSeq = 1;
    bool        bAnswered = false;

	WiFiUDP 	Udp;

    // Ausstehende Anfrage (Erweiterung): Wiederholung ueber UDP nach RFC 3261. Das
    // Paket wird aus dem unveraenderten Zustand neu gebaut (gleicher Branch, gleiche
    // CSeq, gleicher Auth-Header) - spart die Kopie des Pakets.
    enum { REQ_NONE, REQ_INVITE, REQ_CANCEL, REQ_BYE, REQ_BYE_IN };
    uint8_t     reqType = REQ_NONE;
    uint8_t     reqSends = 0;
    int         reqCSeq = 0;
    uint32_t    reqSentAt = 0;
    uint32_t    reqInterval = 500;
    uint32_t    inByeBranch = 0;  // Branch unseres BYE bei eingehendem Anruf

    // Eingehender Anruf (Erweiterung)
    bool        bAcceptIncoming = false;
    bool        bIncoming = false;
    bool        bNewIncoming = false;
    int         iIncomingSeconds = 30;
    char        caInCallId[100];
    char        caInFrom[160];      // From des Anrufers (mit dessen Tag)
    char        caInTo[160];        // To ohne unseren Tag
    char        caInVia[400];       // alle Via-Zeilen der letzten Anfrage (mit CRLF)
    char        caInContact[120];   // Ziel fuer unser BYE
    char        caInCaller[32];     // Rufnummer fuer das Protokoll
    RingFilter  pRingFilter = nullptr;
    bool        bRingCall = false;
    char        caRingCaller[32];
    char        caLastCaller[32];
    uint32_t    ringCallIdHash = 0; // Call-ID (Hash, beliebig lang) gegen doppelte Meldung
    int         iInCSeq = 0;
    int         iInByeCSeq = 1;
    uint32_t    inTag = 0;
    bool        bInAckPending = false;
    uint32_t    inOkSentAt = 0;
    uint32_t    inOkInterval = 500;
    uint32_t    inOkFirstAt = 0;

    // RTP / Beep-Audio (Erweiterung)
    WiFiUDP     Rtp;
    int         iRtpPort = 16384;
    IPAddress   remoteRtpIp;
    uint16_t    remoteRtpPort = 0;
    bool        bInCall = false;
    bool        bRtpBound = false;
    uint32_t    callAnsweredAt = 0;
    uint32_t    lastRtpAt = 0;
    uint16_t    rtpSeq = 0;
    uint32_t    rtpTs = 0;
    uint32_t    rtpSsrc = 0;
    uint32_t    rtpFrame = 0;
    int         iBeepSeconds = 0;
    int         iStreamBeepSec = 0;   // Beep-Dauer des laufenden Gespraechs
    uint8_t     ulawTone[8];
    uint8_t     alawTone[8];
    bool        bToneReady = false;
    uint8_t     rtpPt = 0;        // ausgehandelter Codec: 0 = PCMU, 8 = PCMA
    uint8_t     dtmfPt = 101;     // telephone-event Payload Type laut Antwort
    char        cLastDtmf = 0;
    bool        bDtmfIncoming = false;   // Taste kam aus einem eingehenden Anruf
    uint32_t    lastDtmfTs = 0;   // RTP-Zeitstempel des letzten DTMF-Ereignisses
    bool        bDtmfTsValid = false;
    uint16_t    iRtpRx = 0;       // Diagnose: angenommene RTP-Pakete
    uint16_t    iRtpForeign = 0;  //           verworfene (fremder Absender)
    IPAddress   foreignRtpIp;     //           letzter fremder Absender
    uint8_t     iDtmfRx = 0;      //           erkannte Tasten (RTP und INFO)

	void        HandleUdpPacket(const char *p, bool fromServer, bool truncated);
    void        HandleResponse(const char *p, bool fromServer);
    void        HandleTimers();
    int         Normalize(char *buf, int len, size_t cap, int *bodyAt);
	void        AddSipLine(const char* constFormat , ... );
    bool        AddCopySipLine(const char *p, const char *psearch);
    void        AddCopyAllLines(const char *p, const char *psearch);
    bool        ParseReturnParams(const char *p);
    int         GrepInteger(const char *p, const char *psearch);
    bool        IsResponseTo(const char *p, const char *method);
    bool        IsCallId(const char *p, uint32_t id32);
    bool        IsCallIdStr(const char *p, const char *id);
    bool        IsRequest(const char *p, const char *method);
    bool        HeaderValue(const char *p, const char *name, char *dest, size_t destlen);
    bool        HeaderUri(const char *p, const char *name, char *dest, size_t destlen);
    bool        BuildAuth(const char *p, const char *method, const char *uri, char *out = nullptr, size_t outLen = 0);
    void        ResolveServer();
    void        SendRegister(bool withAuth);
    void        HandleRegisterResponse(const char *p);
    void        Ack(const char *pIn);
    int         Cancel();
    int         Bye(int cseq);
    void        Respond(const char *pIn, int code, const char *reason, const char *sdp = nullptr);
    int         Invite(const char *pIn = 0);
    int         SendInvite();

    // Ausstehende Anfrage (Erweiterung)
    int         StartReq(uint8_t type, int cseq);
    int         SendReq();
    bool        IsReqResponse(const char *p, int cseq);

    // Eingehender Anruf (Erweiterung)
    void        HandleIncomingInvite(const char *p, bool truncated);
    void        HandleOutgoingReinvite(const char *p);
    bool        ParseOffer(const char *p);
    void        SendIncomingOk();
    int         ByeIncoming(int cseq);
    int         BuildSdp(char *out, size_t len, bool offer);

    uint32_t    Millis();
    uint32_t    Random();
    int         SendUdp();
    void        MakeMd5Digest(char *pOutHex33, const char *pIn);

    // RTP / Beep-Helfer (Erweiterung)
    int         TalkSeconds() {
      if ( bIncoming ) return iIncomingSeconds;
      return iCallSeconds > iBeepSeconds ? iCallSeconds : iBeepSeconds;
    }
    void        StartRtp(const char *pIn);
    void        StartStream(int beepSec);
    void        StopCall();
    void        RtpProcessing();
    void        RtpSkipRest();
    void        SendRtpFrame();
    uint8_t     Lin2Ulaw(int16_t sample);
    uint8_t     Lin2Alaw(int16_t sample);

};

#endif	// ARDUINO_SIP_H

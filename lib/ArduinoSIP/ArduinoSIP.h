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
    bool        IsRegistered() { return bRegistered; }
    // Beep-Dauer (Sekunden) fuer den RTP-Ton nach dem Abheben; 0 = kein Audio.
    void        SetBeepSeconds(int s) { iBeepSeconds = s; }
    // Liefert die zuletzt empfangene DTMF-Taste (0 wenn keine) und loescht sie.
    char        ReadDtmf() { char c = cLastDtmf; cLastDtmf = 0; return c; }
    // Anruf sofort beenden: BYE wenn angenommen, sonst CANCEL. (Erweiterung)
    void        Hangup();
    // Gespraechsdauer nach dem Abheben (Sekunden), mindestens die Beep-Dauer;
    // laenger z.B. fuer die Eingabe eines Oeffnungs-Codes. (Erweiterung)
    void        SetCallSeconds(int s) { iCallSeconds = s; }
    // Ergebnis der letzten Registrierung: -1 = noch keine, 0 = keine Antwort,
    // sonst SIP-Statuscode (200 = ok, 401 nach Digest = Zugangsdaten falsch, ...)
    int         RegisterStatus() { return iRegStatus; }
    // Ergebnis des letzten Anrufs (gueltig, sobald IsBusy() false ist)
    enum CallResult { CALL_NONE, CALL_ANSWERED, CALL_NOANSWER, CALL_BUSY, CALL_DECLINED, CALL_FAILED };
    CallResult  LastCallResult() { return eCallResult; }
    int         LastCallCode() { return iCallCode; }   // SIP-Code bei CALL_FAILED

  private:
    char       *pbuf;
    size_t      lbuf;
    char        caRead[256];
    bool        bRegistered = false;

    // REGISTER-Zustand (Erweiterung)
    bool        bRegPending = false;
    uint32_t    regSentAt = 0;
    uint32_t    regCallId = 0;
    uint32_t    regTag = 0;
    int         iRegCSeq = 1;
    int         iRegExpires = 3600;
    int         iRegStatus = -1;

    // Anruf-Ergebnis / Gespraechsdauer (Erweiterung)
    CallResult  eCallResult = CALL_NONE;
    int         iCallCode = 0;
    int         iCallSeconds = 0;

    const char *pSipIp;
    int         iSipPort;
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
    int         iDialRetries;
    int         iLastCSeq;
    int         iInviteCSeq = 1;
    bool        bAnswered = false;

	WiFiUDP 	Udp;

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
    uint8_t     ulawTone[8];
    uint8_t     alawTone[8];
    bool        bToneReady = false;
    uint8_t     rtpPt = 0;        // ausgehandelter Codec: 0 = PCMU, 8 = PCMA
    uint8_t     dtmfPt = 101;     // telephone-event Payload Type laut Antwort
    char        cLastDtmf = 0;
    uint32_t    lastDtmfAt = 0;
	
	void        HandleUdpPacket(const char *p);
	void        AddSipLine(const char* constFormat , ... );
    bool        AddCopySipLine(const char *p, const char *psearch);
    bool        ParseParameter(char *dest, int destlen, const char *name, const char *line, char cq = '\"');
    bool        ParseReturnParams(const char *p);
    int         GrepInteger(const char *p, const char *psearch);
    bool        IsResponseTo(const char *p, const char *method);
    bool        IsCallId(const char *p, uint32_t id32);
    void        SendRegister(const char *pAuth);
    void        HandleRegisterResponse(const char *p);
    void        Ack(const char *pIn);
    void        Cancel();
    void        Bye(int cseq);
    void        Ok(const char *pIn);
    void        Invite(const char *pIn = 0);

    uint32_t    Millis();
    uint32_t    Random();
    int         SendUdp();
    void        MakeMd5Digest(char *pOutHex33, char *pIn);

    // RTP / Beep-Helfer (Erweiterung)
    int         TalkSeconds() { return iCallSeconds > iBeepSeconds ? iCallSeconds : iBeepSeconds; }
    void        StartRtp(const char *pIn);
    void        StopCall();
    void        RtpProcessing();
    void        SendRtpFrame();
    uint8_t     Lin2Ulaw(int16_t sample);
    uint8_t     Lin2Alaw(int16_t sample);

};

#endif	// ARDUINO_SIP_H
// Host-Test der ArduinoSIP-Bibliothek (mit AddressSanitizer/UBSan uebersetzen,
// siehe tests/host/run.sh). Prueft Digest (RFC 2617), REGISTER, ausgehende und
// eingehende Anrufe, DTMF sowie kaputte/zu grosse Pakete, Wiederholungen (UDP),
// Zuordnung nach CSeq, Kurzformen/Schreibweisen, Re-INVITE und Hostnamen.
#define ARDUINO 100
#include "Arduino.h"
#include <MD5Builder.h>
#include <WiFiUdp.h>
#include <ESP8266WiFi.h>
#include <string>
#define private public
#include "ArduinoSIP.cpp"
#undef private

uint32_t g_millis = 1000;
uint32_t g_random = 0x0a4f113b;
WiFiStub WiFi;

static int fails = 0, checks = 0;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("FEHLER: %s (Zeile %d)\n", msg, __LINE__); } } while (0)

static char inBuf[1600], outBuf[1500];
static const IPAddress SERVER(192, 168, 1, 10);

static std::string lastSent(Sip &s) { return s.Udp.sent.empty() ? "" : s.Udp.sent.back(); }
static void feed(Sip &s, const std::string &p, IPAddress from = SERVER) {
  s.Udp.rx.push_back({from, p});
  s.Processing(inBuf, sizeof(inBuf));
}
static std::string header(const std::string &msg, const std::string &name) {
  size_t a = msg.find("\r\n" + name);
  if (a == std::string::npos) return "";
  a += 2 + name.size();
  return msg.substr(a, msg.find("\r\n", a) - a);
}
static std::string md5(const std::string &s) {
  MD5Builder m; m.begin(); m.add(s.c_str()); m.calculate(); char o[33]; m.getChars(o); return o;
}

static const std::string INVITE_IN =
  "INVITE sip:200@192.168.1.50:5060 SIP/2.0\r\n"
  "Via: SIP/2.0/UDP 192.168.1.10:5060;branch=z9hG4bK-a;rport\r\n"
  "Via: SIP/2.0/UDP 192.168.1.20:5062;branch=z9hG4bK-b\r\n"
  "From: \"Handwerker\" <sip:0171555@192.168.1.10>;tag=abc\r\n"
  "To: <sip:200@192.168.1.50>\r\n"
  "Contact: <sip:0171555@192.168.1.10:5060>\r\n"
  "Call-ID: in-123@192.168.1.10\r\n"
  "CSeq: 102 INVITE\r\n"
  "Content-Type: application/sdp\r\n\r\n"
  "v=0\r\nc=IN IP4 192.168.1.10\r\nm=audio 12000 RTP/AVP 0 8 101\r\n"
  "a=rtpmap:0 PCMU/8000\r\na=rtpmap:8 PCMA/8000\r\na=rtpmap:101 telephone-event/8000\r\n";

static std::string withCallId(const std::string &id) {
  std::string s = INVITE_IN;
  s.replace(s.find("in-123"), 6, id);
  return s;
}

static void testDigest() {
  Sip s(outBuf, sizeof(outBuf));
  s.Init("192.168.1.10", 5060, "192.168.1.50", 5060, "Mufasa", "Circle Of Life", 15);
  g_random = 0x0a4f113b;   // ergibt cnonce "0a4f113b" wie im RFC-Beispiel
  const char *p = "SIP/2.0 401 Unauthorized\r\n"
    "WWW-Authenticate: Digest realm=\"testrealm@host.com\", qop=\"auth,auth-int\", "
    "nonce=\"dcd98b7102dd2f0e8b11d0f600bfb0c093\", opaque=\"5ccc069c403ebaf9f0171e9517f40e41\"\r\n\r\n";
  CHECK(s.BuildAuth(p, "GET", "/dir/index.html"), "BuildAuth RFC-Beispiel");
  std::string a = s.caAuth;
  CHECK(a.find("response=\"6629fae49393a05397450978507c4ef1\"") != std::string::npos, "RFC-2617-Antwort");
  CHECK(a.find("qop=auth, nc=00000001, cnonce=\"0a4f113b\"") != std::string::npos, "qop/nc/cnonce");
  CHECK(a.find("opaque=\"5ccc069c403ebaf9f0171e9517f40e41\"") != std::string::npos, "opaque");
  CHECK(a.rfind("Authorization: Digest", 0) == 0, "Header-Name");

  const char *p2 = "SIP/2.0 407 Proxy Authentication Required\r\n"
    "Proxy-Authenticate: Digest realm=\"asterisk\",nonce=\"abc123\"\r\n\r\n";
  CHECK(s.BuildAuth(p2, "INVITE", "sip:100@192.168.1.10"), "BuildAuth ohne qop");
  std::string exp = md5(md5("Mufasa:asterisk:Circle Of Life") + ":abc123:" + md5("INVITE:sip:100@192.168.1.10"));
  a = s.caAuth;
  CHECK(a.find("response=\"" + exp + "\"") != std::string::npos, "Antwort ohne qop");
  CHECK(a.find("qop=") == std::string::npos, "kein qop ohne Angebot");
  CHECK(a.rfind("Proxy-Authorization:", 0) == 0, "Proxy-Authorization bei 407");
  CHECK(!s.BuildAuth("x\r\nWWW-Authenticate: Digest realm=\"r\", nonce=\"n\", qop=\"auth-int\"\r\n", "REGISTER", "sip:x"), "auth-int abgelehnt");
  CHECK(!s.BuildAuth("x\r\nWWW-Authenticate: Digest realm=\"r\", nonce=\"n\", algorithm=SHA-256\r\n", "REGISTER", "sip:x"), "SHA-256 abgelehnt");
  CHECK(s.BuildAuth("x\r\nWWW-Authenticate: Digest cnonce=\"falsch\", realm=\"r\", nonce=\"richtig\"\r\n", "REGISTER", "sip:x"), "cnonce/nonce");
  CHECK(std::string(s.caAuth).find("nonce=\"richtig\"") != std::string::npos, "richtige nonce");
  CHECK(!s.BuildAuth("x\r\nWWW-Authenticate: Digest realm=\"kaputt, nonce=\"n\r\n", "REGISTER", "sip:x"), "kaputte Challenge");
}

static void testRegister() {
  Sip s(outBuf, sizeof(outBuf));
  s.Init("192.168.1.10", 5060, "192.168.1.50", 5060, "200", "geheim", 15);
  g_random = 12345;
  s.StartRegister(120);
  std::string r1 = lastSent(s);
  CHECK(r1.rfind("REGISTER sip:192.168.1.10 SIP/2.0", 0) == 0, "REGISTER gesendet");
  std::string cid = header(r1, "Call-ID: ");
  CHECK(header(r1, "CSeq: ") == "1 REGISTER", "CSeq 1");
  feed(s, "SIP/2.0 401 Unauthorized\r\nVia: x\r\nCall-ID: " + cid + "\r\nCSeq: 1 REGISTER\r\n"
          "WWW-Authenticate: Digest algorithm=MD5, realm=\"asterisk\", nonce=\"5f3a\", qop=\"auth\"\r\n\r\n");
  std::string r2 = lastSent(s);
  CHECK(header(r2, "CSeq: ") == "2 REGISTER", "CSeq 2 nach 401");
  CHECK(r2.find("Authorization: Digest username=\"200\"") != std::string::npos, "Authorization gesendet");
  CHECK(r2.find("qop=auth") != std::string::npos, "qop im REGISTER");
  feed(s, "SIP/2.0 200 OK\r\nCall-ID: " + cid + "\r\nCSeq: 1 REGISTER\r\n\r\n");
  CHECK(!s.IsRegistered(), "alte CSeq ignoriert");
  feed(s, "SIP/2.0 200 OK\r\nCall-ID: " + cid + "\r\nCSeq: 2 REGISTER\r\n"
          "Contact: <sip:200@192.168.1.50:5060>;expires=60\r\n\r\n");
  CHECK(s.IsRegistered() && s.RegisterStatus() == 200, "registriert");
  CHECK(s.RegisterExpires() == 60, "gewaehrte Dauer 60 s");
  s.StartRegister(120);
  std::string r3 = lastSent(s);
  CHECK(header(r3, "Call-ID: ") == cid, "Erneuerung mit gleicher Call-ID");
  CHECK(header(r3, "CSeq: ") == "3 REGISTER", "Erneuerung CSeq 3");
  feed(s, "SIP/2.0 401 Unauthorized\r\nCall-ID: " + cid + "\r\nCSeq: 3 REGISTER\r\n"
          "WWW-Authenticate: Digest realm=\"asterisk\", nonce=\"6a\"\r\n\r\n");
  feed(s, "SIP/2.0 401 Unauthorized\r\nCall-ID: " + cid + "\r\nCSeq: 4 REGISTER\r\n"
          "WWW-Authenticate: Digest realm=\"asterisk\", nonce=\"6b\"\r\n\r\n");
  CHECK(s.RegisterStatus() == 401, "falsches Passwort erkannt");
  CHECK(s.IsRegistered(), "Anmeldung gilt bis zum Ablauf weiter");
  s.StartRegister(120);
  for (int i = 0; i < 60; i++) { g_millis += 100; s.Processing(inBuf, sizeof(inBuf)); }
  CHECK(!s.IsRegistering() && s.RegisterStatus() == 0, "Zeitueberschreitung");
  g_millis += 60000;
  s.Processing(inBuf, sizeof(inBuf));
  CHECK(!s.IsRegistered(), "nach Ablauf abgemeldet");
}

static int countSent(Sip &s, const std::string &prefix) {
  int n = 0;
  for (auto &p : s.Udp.sent) if (p.rfind(prefix, 0) == 0) n++;
  return n;
}
static void tick(Sip &s, uint32_t ms) { g_millis += ms; s.Processing(inBuf, sizeof(inBuf)); }

// S4: REGISTER wird wiederholt; Anmeldung gilt bis zum Ablauf; Paket vor Zeitablauf
static void testRegisterRetry() {
  Sip s(outBuf, sizeof(outBuf));
  s.Init("192.168.1.10", 5060, "192.168.1.50", 5060, "200", "geheim", 15);
  s.StartRegister(120);
  std::string r1 = lastSent(s);
  std::string cid = header(r1, "Call-ID: ");
  tick(s, 400);
  CHECK(s.Udp.sent.size() == 1, "vor 0,5 s keine Wiederholung");
  tick(s, 200);
  CHECK(s.Udp.sent.size() == 2 && lastSent(s) == r1, "REGISTER nach 0,5 s byte-gleich wiederholt");
  tick(s, 1100);
  tick(s, 2100);
  CHECK(s.Udp.sent.size() == 4 && lastSent(s) == r1, "nach 1 s und 2 s erneut (gleiche CSeq)");
  CHECK(s.IsRegistering() && s.RegisterStatus() == -1, "noch keine Meldung 'keine Antwort'");
  tick(s, 1500);
  CHECK(s.IsRegistering(), "nach 3,5 s noch wartend");
  tick(s, 600);
  CHECK(!s.IsRegistering() && s.RegisterStatus() == 0 && s.Udp.sent.size() == 4, "erst nach 4 Versuchen keine Antwort");

  // Mit Digest: auch das REGISTER mit Authorization wird wiederholt
  s.StartRegister(120);
  r1 = lastSent(s);
  cid = header(r1, "Call-ID: ");
  CHECK(header(r1, "CSeq: ") == "2 REGISTER", "naechster Versuch mit hoeherer CSeq");
  feed(s, "SIP/2.0 401 Unauthorized\r\nCall-ID: " + cid + "\r\nCSeq: 2 REGISTER\r\n"
          "WWW-Authenticate: Digest realm=\"asterisk\", nonce=\"ab\"\r\n\r\n");
  std::string r2 = lastSent(s);
  CHECK(header(r2, "CSeq: ") == "3 REGISTER" && r2.find("Authorization: Digest") != std::string::npos, "REGISTER mit Digest");
  CHECK(header(r2, "Via: ") != header(r1, "Via: "), "neue Transaktion, neuer Branch");
  tick(s, 600);
  CHECK(lastSent(s) == r2, "REGISTER mit Digest byte-gleich wiederholt");
  // Antwort kommt im selben Durchlauf, in dem die Wartezeit ablaeuft -> zaehlt
  g_millis += 6000;
  feed(s, "SIP/2.0 200 OK\r\nCall-ID: " + cid + "\r\nCSeq: 3 REGISTER\r\nContact: <sip:200@192.168.1.50:5060>;expires=60\r\n\r\n");
  CHECK(s.IsRegistered() && s.RegisterStatus() == 200 && !s.IsRegistering(), "Paket vor dem Zeitablauf ausgewertet");

  // Erneuerung geht verloren: Status zeigt den Fehler, Anmeldung gilt noch
  g_millis += 20000;
  s.StartRegister(120);
  for (int i = 0; i < 60; i++) tick(s, 100);
  CHECK(s.RegisterStatus() == 0 && s.IsRegistered(), "gescheiterte Erneuerung: noch angemeldet");
  s.StartRegister(120);
  CHECK(header(lastSent(s), "Call-ID: ") == cid, "Call-ID bleibt bei Zeitueberschreitung");
  g_millis += 35000;
  s.Processing(inBuf, sizeof(inBuf));
  CHECK(!s.IsRegistered(), "60 s nach dem letzten 200 OK abgelaufen");
}

static std::string answer200(const std::string &cid, int cseq, const std::string &to, const std::string &sdp = "") {
  return "SIP/2.0 200 OK\r\nVia: SIP/2.0/UDP 192.168.1.50:5060\r\nFrom: <sip:200@192.168.1.10>;tag=1\r\nTo: " + to + "\r\n"
         "Call-ID: " + cid + "\r\nCSeq: " + std::to_string(cseq) + " INVITE\r\n" +
         (sdp.empty() ? "\r\n" : "Content-Type: application/sdp\r\n\r\n" + sdp);
}
static const std::string SDP_ANSWER = "v=0\r\nc=IN IP4 192.168.1.10\r\nm=audio 10000 RTP/AVP 8 101\r\na=rtpmap:101 telephone-event/8000\r\n";

// S2/S3: Wiederholung von INVITE (auch mit Digest), CANCEL, BYE; Zuordnung nach CSeq
static void testRetransmit() {
  Sip s(outBuf, sizeof(outBuf));
  s.Init("192.168.1.10", 5060, "192.168.1.50", 5060, "200", "geheim", 15);
  s.SetBeepSeconds(6);
  CHECK(s.Dial("100", "Tuer"), "Dial");
  std::string inv1 = lastSent(s), cid = header(inv1, "Call-ID: ");
  tick(s, 400);
  CHECK(s.Udp.sent.size() == 1, "INVITE vor 0,5 s nicht wiederholt");
  tick(s, 200);
  CHECK(s.Udp.sent.size() == 2 && lastSent(s) == inv1, "INVITE nach 0,5 s byte-gleich wiederholt");
  tick(s, 1100);
  CHECK(s.Udp.sent.size() == 3 && lastSent(s) == inv1, "INVITE nach weiteren 1 s wiederholt");

  std::string r401 = "SIP/2.0 401 Unauthorized\r\nVia: " + header(inv1, "Via: ") + "\r\nFrom: " + header(inv1, "From: ") +
    "\r\nTo: <sip:100@192.168.1.10>;tag=as1\r\nCall-ID: " + cid + "\r\nCSeq: 1 INVITE\r\n"
    "WWW-Authenticate: Digest realm=\"asterisk\", nonce=\"77\"\r\n\r\n";
  feed(s, r401);
  std::string inv2 = lastSent(s);
  CHECK(header(inv2, "CSeq: ") == "2 INVITE" && inv2.find("Authorization: Digest") != std::string::npos, "INVITE mit Digest");
  size_t n = s.Udp.sent.size();
  tick(s, 600);
  CHECK(s.Udp.sent.size() == n + 1 && lastSent(s) == inv2, "INVITE mit Digest byte-gleich wiederholt");
  tick(s, 1100);
  CHECK(s.Udp.sent.size() == n + 2 && lastSent(s) == inv2, "INVITE mit Digest erneut wiederholt");

  // Wiederholtes 401 (unser ACK ging verloren): nur ACK, kein drittes INVITE
  n = s.Udp.sent.size();
  feed(s, r401);
  CHECK(s.Udp.sent.size() == n + 1 && lastSent(s).rfind("ACK ", 0) == 0 && header(lastSent(s), "CSeq: ") == "1 ACK", "altes 401 nur quittiert");
  CHECK(countSent(s, "INVITE ") == 6, "kein INVITE mit CSeq 3");
  // Fehlerantwort auf die alte Transaktion beendet den Anruf nicht
  std::string r500 = r401;
  r500.replace(8, 16, "500 Server Error");
  feed(s, r500);
  CHECK(s.IsBusy() && lastSent(s).rfind("ACK ", 0) == 0, "500 auf alte CSeq: nur ACK, Anruf laeuft");

  // Erste Antwort auf das aktuelle INVITE -> keine Wiederholung mehr
  feed(s, "SIP/2.0 100 Trying\r\nVia: v\r\nFrom: f\r\nTo: <sip:100@192.168.1.10>\r\nCall-ID: " + cid + "\r\nCSeq: 2 INVITE\r\n\r\n");
  n = s.Udp.sent.size();
  tick(s, 5000);
  CHECK(s.Udp.sent.size() == n, "nach 100 Trying keine Wiederholung");

  // CANCEL bis 200 wiederholen
  s.Hangup();
  std::string can = lastSent(s);
  CHECK(can.rfind("CANCEL ", 0) == 0 && header(can, "CSeq: ") == "2 CANCEL" && header(can, "Via: ") == header(inv2, "Via: "), "CANCEL zum INVITE mit Digest");
  CHECK(s.IsClosing() && !s.IsBusy(), "CANCEL ausstehend");
  tick(s, 600);
  CHECK(lastSent(s) == can, "CANCEL wiederholt");
  feed(s, "SIP/2.0 200 OK\r\nVia: v\r\nFrom: f\r\nTo: t\r\nCall-ID: " + cid + "\r\nCSeq: 2 CANCEL\r\n\r\n");
  CHECK(!s.IsClosing(), "200 auf CANCEL beendet die Wiederholung");
  n = s.Udp.sent.size();
  tick(s, 3000);
  CHECK(s.Udp.sent.size() == n, "kein weiteres CANCEL");
  feed(s, "SIP/2.0 487 Request Terminated\r\nVia: v\r\nFrom: f\r\nTo: <sip:100@192.168.1.10>;tag=as1\r\nCall-ID: " + cid + "\r\nCSeq: 2 INVITE\r\n\r\n");
  CHECK(lastSent(s).rfind("ACK ", 0) == 0 && s.LastCallResult() == Sip::CALL_NOANSWER, "487 quittiert");

  // CANCEL ohne Antwort: hoechstens 4x, auch nach dem Anruf; 487 beendet es ebenfalls
  CHECK(s.Dial("101", "Tuer"), "zweiter Anruf");
  std::string cid2 = header(lastSent(s), "Call-ID: ");
  feed(s, "SIP/2.0 180 Ringing\r\nVia: v\r\nFrom: f\r\nTo: <sip:101@192.168.1.10>;tag=r\r\nCall-ID: " + cid2 + "\r\nCSeq: 1 INVITE\r\n\r\n");
  s.Hangup();
  int c0 = countSent(s, "CANCEL ");
  for (int i = 0; i < 100; i++) tick(s, 100);
  CHECK(countSent(s, "CANCEL ") == c0 + 3 && !s.IsClosing(), "CANCEL 4x gesendet, dann aufgegeben");

  // Zweites 401 auf das INVITE mit Digest = Zugangsdaten falsch
  CHECK(s.Dial("102", "Tuer"), "dritter Anruf");
  std::string inv3 = lastSent(s), cid3 = header(inv3, "Call-ID: ");
  std::string c401 = "SIP/2.0 401 Unauthorized\r\nVia: v\r\nFrom: f\r\nTo: <sip:102@192.168.1.10>;tag=a\r\nCall-ID: " + cid3 +
                     "\r\nCSeq: 1 INVITE\r\nWWW-Authenticate: Digest realm=\"asterisk\", nonce=\"1\"\r\n\r\n";
  feed(s, c401);
  CHECK(header(lastSent(s), "CSeq: ") == "2 INVITE", "INVITE mit Digest");
  std::string c401b = c401;
  c401b.replace(c401b.find("CSeq: 1"), 7, "CSeq: 2");
  c401b.replace(c401b.find("nonce=\"1\""), 9, "nonce=\"2\"");
  int inv0 = countSent(s, "INVITE ");
  feed(s, c401b);
  CHECK(!s.IsBusy() && s.LastCallResult() == Sip::CALL_FAILED && s.LastCallCode() == 401, "zweites 401: Zugangsdaten falsch");
  CHECK(countSent(s, "INVITE ") == inv0 && lastSent(s).rfind("ACK ", 0) == 0, "kein weiteres INVITE, nur ACK");
  tick(s, 3000);
  CHECK(countSent(s, "INVITE ") == inv0, "auch keine Wiederholung");

  // Ohne jede Antwort: INVITE nach 0 / 0,5 / 1,5 / 3,5 / 7,5 s, nach 15 s CANCEL
  CHECK(s.Dial("103", "Tuer"), "vierter Anruf");
  inv0 = countSent(s, "INVITE ");
  c0 = countSent(s, "CANCEL ");
  for (int i = 0; i < 160; i++) tick(s, 100);
  CHECK(countSent(s, "INVITE ") == inv0 + 4 && countSent(s, "CANCEL ") >= c0 + 1, "INVITE bis iMaxTime wiederholt, dann CANCEL");
  CHECK(!s.IsBusy() && s.LastCallResult() == Sip::CALL_NOANSWER, "nicht angenommen");
  for (int i = 0; i < 100; i++) tick(s, 100);

  // BYE bis 200 OK wiederholen
  CHECK(s.Dial("104", "Tuer"), "fuenfter Anruf");
  std::string cid5 = header(lastSent(s), "Call-ID: ");
  feed(s, answer200(cid5, 1, "<sip:104@192.168.1.10>;tag=b"));
  CHECK(s.LastCallResult() == Sip::CALL_ANSWERED, "angenommen");
  s.Hangup();
  std::string bye = lastSent(s);
  CHECK(bye.rfind("BYE ", 0) == 0 && header(bye, "CSeq: ") == "2 BYE" && s.IsClosing(), "BYE ausstehend");
  tick(s, 600);
  CHECK(lastSent(s) == bye, "BYE byte-gleich wiederholt");
  feed(s, "SIP/2.0 200 OK\r\nVia: v\r\nFrom: f\r\nTo: t\r\nCall-ID: " + cid5 + "\r\nCSeq: 2 BYE\r\n\r\n");
  CHECK(!s.IsClosing(), "200 auf BYE beendet die Wiederholung");
  n = s.Udp.sent.size();
  tick(s, 4000);
  CHECK(s.Udp.sent.size() == n, "kein weiteres BYE");
  // ... ohne Antwort hoechstens 4x
  CHECK(s.Dial("105", "Tuer"), "sechster Anruf");
  std::string cid6 = header(lastSent(s), "Call-ID: ");
  feed(s, answer200(cid6, 1, "<sip:105@192.168.1.10>;tag=c"));
  s.Hangup();
  int b0 = countSent(s, "BYE ");
  for (int i = 0; i < 100; i++) tick(s, 100);
  CHECK(countSent(s, "BYE ") == b0 + 3 && !s.IsClosing(), "BYE 4x gesendet, dann aufgegeben");
}

// S2: BYE eines eingehenden Anrufs wird wiederholt
static void testIncomingBye() {
  Sip s(outBuf, sizeof(outBuf));
  s.Init("192.168.1.10", 5060, "192.168.1.50", 5060, "200", "geheim", 15);
  s.SetAcceptIncoming(true);
  feed(s, INVITE_IN);
  feed(s, "ACK sip:200@192.168.1.50:5060 SIP/2.0\r\nVia: x\r\nCall-ID: in-123@192.168.1.10\r\nCSeq: 102 ACK\r\n\r\n");
  s.Hangup();
  std::string bye = lastSent(s);
  CHECK(bye.rfind("BYE ", 0) == 0 && header(bye, "CSeq: ") == "1 BYE" && s.IsClosing(), "BYE gesendet");
  tick(s, 600);
  CHECK(lastSent(s) == bye, "BYE mit gleichem Branch und CSeq wiederholt");
  feed(s, "SIP/2.0 200 OK\r\n" + std::string("Via: ") + header(bye, "Via: ") + "\r\nFrom: f\r\nTo: t\r\n"
          "Call-ID: in-123@192.168.1.10\r\nCSeq: 1 BYE\r\n\r\n");
  CHECK(!s.IsClosing(), "200 auf BYE beendet die Wiederholung");
  size_t n = s.Udp.sent.size();
  tick(s, 4000);
  CHECK(s.Udp.sent.size() == n, "kein weiteres BYE");
  // Neuer Anruf, waehrend das BYE des vorigen noch aussteht: altes BYE nicht mehr
  // mit den Daten des neuen Anrufs wiederholen
  feed(s, withCallId("in-124"));
  s.Hangup();
  feed(s, withCallId("in-125"));
  CHECK(s.IsIncoming() && !s.IsClosing(), "neuer Anruf ersetzt das ausstehende BYE");
  tick(s, 600);
  CHECK(countSent(s, "BYE ") == 3, "kein BYE in den neuen Anruf");
  s.Hangup();
}

static std::string dtmfPacket(uint8_t event, uint8_t ts) {
  std::string ev(16, '\0');
  ev[0] = (char)0x80; ev[1] = 101; ev[7] = (char)ts; ev[12] = (char)event; ev[13] = (char)0x80;
  return ev;
}

// S1: Herkunft der Taste; Taste verfaellt, wenn der Anruf im selben Durchlauf endet
static void testDtmfOrigin() {
  Sip s(outBuf, sizeof(outBuf));
  s.Init("192.168.1.10", 5060, "192.168.1.50", 5060, "200", "geheim", 15);
  s.SetAcceptIncoming(true);
  s.SetIncomingSeconds(30);
  feed(s, INVITE_IN);
  feed(s, "ACK sip:200@192.168.1.50:5060 SIP/2.0\r\nVia: x\r\nCall-ID: in-123@192.168.1.10\r\nCSeq: 102 ACK\r\n\r\n");
  bool inc = false;
  g_millis += 29000;
  s.Rtp.rx.push_back({SERVER, dtmfPacket(10, 1)});
  s.Processing(inBuf, sizeof(inBuf));
  CHECK(s.ReadDtmf(&inc) == '*' && inc, "Taste aus eingehendem Anruf");
  // Zeitablauf (Hangup in RtpProcessing) im selben Durchlauf wie die Taste
  g_millis += 1500;
  s.Rtp.rx.push_back({SERVER, dtmfPacket(10, 2)});
  s.Processing(inBuf, sizeof(inBuf));
  inc = true;
  char d = s.ReadDtmf(&inc);
  CHECK(!s.IsBusy() && (d == 0 || inc), "Taste beim Auflegen nie als ausgehend gewertet");
  CHECK(d == 0, "Taste beim Auflegen verworfen");

  // INFO, dann BYE vor dem Lesen: Taste verfaellt
  feed(s, withCallId("in-301"));
  std::string info = "INFO sip:200@192.168.1.50 SIP/2.0\r\nVia: SIP/2.0/UDP 192.168.1.10:5060;branch=z9hG4bK-i\r\n"
    "From: <sip:0171555@192.168.1.10>;tag=abc\r\nTo: <sip:200@192.168.1.50>;tag=x\r\n"
    "Call-ID: in-301@192.168.1.10\r\nCSeq: 103 INFO\r\n\r\nSignal=*\r\nDuration=160\r\n";
  s.Udp.rx.push_back({SERVER, info});
  s.Udp.rx.push_back({SERVER, "BYE sip:200@192.168.1.50 SIP/2.0\r\nVia: v\r\nFrom: f\r\nTo: t;tag=1\r\nCall-ID: in-301@192.168.1.10\r\nCSeq: 104 BYE\r\n\r\n"});
  s.Processing(inBuf, sizeof(inBuf));
  s.Processing(inBuf, sizeof(inBuf));
  CHECK(!s.IsBusy() && s.ReadDtmf() == 0, "Taste per INFO verfaellt mit BYE");
  feed(s, withCallId("in-302"));
  info.replace(info.find("in-301"), 6, "in-302");
  feed(s, info);
  CHECK(s.ReadDtmf(&inc) == '*' && inc, "INFO im eingehenden Anruf");
  s.Hangup();

  // Ausgehender Anruf: Taste als ausgehend
  s.SetBeepSeconds(6);
  CHECK(s.Dial("100", "Tuer"), "Dial");
  feed(s, answer200(header(lastSent(s), "Call-ID: "), 1, "<sip:100@192.168.1.10>;tag=o", SDP_ANSWER));
  s.Rtp.rx.push_back({SERVER, dtmfPacket(10, 9)});
  s.Processing(inBuf, sizeof(inBuf));
  inc = true;
  CHECK(s.ReadDtmf(&inc) == '*' && !inc, "Taste aus ausgehendem Anruf");
  s.Hangup();
}

// S10: RTP mit CSRC-Liste und Erweiterung
static void testRtpHeader() {
  Sip s(outBuf, sizeof(outBuf));
  s.Init("192.168.1.10", 5060, "192.168.1.50", 5060, "200", "geheim", 15);
  s.SetBeepSeconds(6);
  CHECK(s.Dial("100", "Tuer"), "Dial");
  feed(s, answer200(header(lastSent(s), "Call-ID: "), 1, "<sip:100@192.168.1.10>;tag=o", SDP_ANSWER));
  CHECK(s.bInCall, "Gespraech laeuft");
  std::string ev(32, '\0');                 // 2 CSRC + Erweiterung (1 Wort)
  ev[0] = (char)(0x80 | 0x10 | 2); ev[1] = 101; ev[7] = 1;
  ev[23] = 1;                               // Laenge der Erweiterung (Byte 20..23)
  ev[28] = 11; ev[29] = (char)0x80;         // '#' mit Ende-Bit
  s.Rtp.rx.push_back({SERVER, ev});
  s.Processing(inBuf, sizeof(inBuf));
  CHECK(s.ReadDtmf() == '#', "Ereignis hinter CSRC und Erweiterung");
  std::string shortCc(20, '\0');            // 15 CSRC angekuendigt, Paket zu kurz
  shortCc[0] = (char)0x8F; shortCc[1] = 101; shortCc[7] = 2; shortCc[13] = (char)0x80;
  s.Rtp.rx.push_back({SERVER, shortCc});
  std::string bigExt(24, '\0');             // Erweiterung laenger als das Paket
  bigExt[0] = (char)0x90; bigExt[1] = 101; bigExt[7] = 3; bigExt[14] = (char)0xFF; bigExt[15] = (char)0xFF;
  s.Rtp.rx.push_back({SERVER, bigExt});
  std::string v1 = dtmfPacket(10, 4);       // keine RTP-Version 2
  v1[0] = 0x40;
  s.Rtp.rx.push_back({SERVER, v1});
  s.Processing(inBuf, sizeof(inBuf));
  CHECK(s.ReadDtmf() == 0, "zu kurze / fremde Pakete ignoriert");
  s.Hangup();
}

static bool everyone(const char *) { return true; }

// S11: Re-INVITE im ausgehenden Gespraech
static void testReinvite() {
  Sip s(outBuf, sizeof(outBuf));
  s.Init("192.168.1.10", 5060, "192.168.1.50", 5060, "200", "geheim", 15);
  s.SetBeepSeconds(6);
  s.SetRingFilter(everyone);   // "*" = jeder Anrufer klingelt
  CHECK(s.Dial("100", "Tuer"), "Dial");
  std::string inv = lastSent(s), cid = header(inv, "Call-ID: ");
  std::string ourTag = header(inv, "From: ").substr(header(inv, "From: ").find(";tag=") + 5);
  feed(s, answer200(cid, 1, "<sip:100@192.168.1.10>;tag=o", SDP_ANSWER));
  std::string re = "INVITE sip:200@192.168.1.50:5060 SIP/2.0\r\nVia: SIP/2.0/UDP 192.168.1.10:5060;branch=z9hG4bK-re1\r\n"
    "From: <sip:100@192.168.1.10>;tag=o\r\nTo: \"Tuer\" <sip:200@192.168.1.10>;tag=" + ourTag + "\r\n"
    "Call-ID: " + cid + "\r\nCSeq: 7 INVITE\r\nContact: <sip:100@192.168.1.10>\r\nContent-Type: application/sdp\r\n\r\n"
    "v=0\r\nc=IN IP4 192.168.1.30\r\nm=audio 20000 RTP/AVP 0 101\r\na=rtpmap:101 telephone-event/8000\r\n";
  feed(s, re);
  std::string ok = lastSent(s);
  CHECK(ok.rfind("SIP/2.0 200 OK", 0) == 0 && header(ok, "CSeq: ") == "7 INVITE", "Re-INVITE mit 200 OK beantwortet");
  CHECK(ok.find("m=audio 16384 RTP/AVP 0 101") != std::string::npos && !header(ok, "Contact: ").empty(), "200 OK mit SDP und Contact");
  CHECK(header(ok, "To: ") == "\"Tuer\" <sip:200@192.168.1.10>;tag=" + ourTag, "To mit unserem Tag");
  CHECK(!s.RingCall(), "Re-INVITE ist kein Klingeln");
  CHECK(s.IsBusy() && s.remoteRtpIp == IPAddress(192, 168, 1, 30) && s.remoteRtpPort == 20000 && s.rtpPt == 0, "neues Medienziel uebernommen");
  tick(s, 40);
  CHECK(!s.Rtp.sentTo.empty() && s.Rtp.sentTo.back() == IPAddress(192, 168, 1, 30), "RTP an neues Ziel");
  std::string bad = re;
  bad.replace(bad.find("CSeq: 7"), 7, "CSeq: 8");
  bad.replace(bad.find("RTP/AVP 0 101"), 13, "RTP/AVP 9 101");
  bad.replace(bad.find("192.168.1.30"), 12, "192.168.1.40");
  feed(s, bad);
  CHECK(lastSent(s).rfind("SIP/2.0 488", 0) == 0 && s.remoteRtpIp == IPAddress(192, 168, 1, 30) && s.rtpPt == 0, "ohne passenden Codec 488, Ziel bleibt");
  s.Hangup();
  feed(s, re);
  CHECK(lastSent(s).rfind("SIP/2.0 481", 0) == 0 && !s.RingCall() && !s.IsBusy(), "Re-INVITE nach dem Auflegen: 481, kein Klingeln");
}

static std::string filterArg;
static int filterCalls = 0;
static bool recordFilter(const char *c) { filterCalls++; filterArg = c; return strcmp(c, "31") == 0; }

// S6: Klingel-Filter mit unlesbarem From und langer Call-ID
static void testRingFilter2() {
  Sip s(outBuf, sizeof(outBuf));
  s.Init("192.168.1.10", 5060, "192.168.1.50", 5060, "200", "geheim", 15);
  s.SetRingFilter(recordFilter);
  std::string ring = INVITE_IN;
  ring.replace(ring.find("sip:0171555@"), 12, "sip:31@");
  feed(s, ring);
  CHECK(s.RingCall(), "Klingeln von 31");
  std::string garbage = withCallId("in-900");
  garbage.replace(garbage.find("\"Handwerker\" <sip:0171555@192.168.1.10>"), 39, "kaputt");
  filterCalls = 0;
  feed(s, garbage);
  CHECK(!s.RingCall() && filterCalls == 0 && s.LastCaller()[0] == 0, "unlesbares From: nicht die vorige Nummer gefiltert");
  std::string longId = std::string(150, 'x') + "@pbx";
  std::string ringLong = ring;
  ringLong.replace(ringLong.find("in-123@192.168.1.10"), 19, longId);
  feed(s, ringLong);
  CHECK(lastSent(s).rfind("SIP/2.0 486", 0) == 0 && s.RingCall(), "lange Call-ID wird gemeldet");
  feed(s, ringLong);
  CHECK(!s.RingCall(), "Wiederholung mit langer Call-ID nicht erneut gemeldet");
  std::string ringLong2 = ringLong;
  ringLong2[ringLong2.find(longId) + 140] = 'y';
  feed(s, ringLong2);
  CHECK(s.RingCall(), "andere lange Call-ID wird gemeldet");
  std::string tel = ring;
  tel.replace(tel.find("<sip:31@192.168.1.10>"), 21, "<tel:31;phone-context=x>");
  tel.replace(tel.find("in-123"), 6, "in-777");
  feed(s, tel);
  CHECK(s.RingCall() && filterArg == "31", "tel:-URI erkannt");
}

// S7: Schreibweise, Kurzformen, Leerraum, Folgezeilen
static void testNormalize() {
  Sip s(outBuf, sizeof(outBuf));
  s.Init("192.168.1.10", 5060, "192.168.1.50", 5060, "200", "geheim", 15);
  char b[256];
  strcpy(b, "INVITE sip:x SIP/2.0\r\nf :  <sip:1@x>  \r\nCALL-id:abc\r\nX-Foo:bar\r\nSubject: a\r\n  b\r\nCSEQ:\t3 INVITE\r\n\r\nm=audio 1\r\nc: body\r\n");
  int bodyAt = 0;
  int n = s.Normalize(b, (int)strlen(b), sizeof(b), &bodyAt);
  std::string exp = "INVITE sip:x SIP/2.0\r\nFrom: <sip:1@x>\r\nCall-ID: abc\r\nX-Foo: bar\r\nSubject: a b\r\nCSeq: 3 INVITE\r\n\r\nm=audio 1\r\nc: body\r\n";
  CHECK(n == (int)exp.size() && std::string(b) == exp, "Kopfzeilen vereinheitlicht, Body unveraendert");
  CHECK(bodyAt == (int)exp.find("m=audio"), "Beginn des Body");
  strcpy(b, "SIP/2.0 200 OK\ni:1\nl:0\n\nx");
  n = s.Normalize(b, (int)strlen(b), sizeof(b), &bodyAt);
  CHECK(std::string(b) == "SIP/2.0 200 OK\nCall-ID: 1\nContent-Length: 0\n\nx" && bodyAt == n - 1, "nur LF");
  strcpy(b, "SIP/2.0 200 OK\r\ni:1\r\nl:0\r\nt:x\r\nf:y\r\nv:z\r\n\r\n");
  CHECK(s.Normalize(b, (int)strlen(b), 64, &bodyAt) == -1, "passt nicht -> verworfen");

  // REGISTER: 200 OK mit "i:" und "cseq:"
  s.StartRegister(120);
  std::string cid = header(lastSent(s), "Call-ID: ");
  feed(s, "SIP/2.0 200 OK\r\nv: SIP/2.0/UDP 192.168.1.50:5060;branch=x\r\ni: " + cid + "\r\ncseq:   1 REGISTER\r\n"
          "m: <sip:200@192.168.1.50:5060>;expires=90\r\nl: 0\r\n\r\n");
  CHECK(s.IsRegistered() && s.RegisterExpires() == 90, "REGISTER-200 in Kurzform/Kleinschreibung");

  // 200 OK auf INVITE: To ohne <>, Header in anderer Schreibweise
  CHECK(s.Dial("100", "Tuer"), "Dial");
  cid = header(lastSent(s), "Call-ID: ");
  feed(s, "SIP/2.0 200 OK\r\nvia: SIP/2.0/UDP 192.168.1.50:5060\r\nFROM: <sip:200@192.168.1.10>;tag=1\r\n"
          "t: sip:100@192.168.1.10;tag=x\r\ncall-id : " + cid + "\r\nCSeq: 1 INVITE\r\n\r\n");
  std::string ack = lastSent(s);
  CHECK(s.LastCallResult() == Sip::CALL_ANSWERED, "angenommen");
  CHECK(ack.rfind("ACK sip:100@192.168.1.10 SIP/2.0", 0) == 0, "ACK an To ohne <>");
  CHECK(header(ack, "To: ") == "sip:100@192.168.1.10;tag=x" && header(ack, "Call-ID: ") == cid, "ACK mit To und Call-ID");
  s.Hangup();
  CHECK(header(lastSent(s), "To: ") == "sip:100@192.168.1.10;tag=x", "BYE mit To ohne <>");

  // Eingehender Anruf in Kurzform
  s.SetAcceptIncoming(true);
  feed(s, "INVITE sip:200@192.168.1.50:5060 SIP/2.0\r\nv: SIP/2.0/UDP 192.168.1.10:5060;branch=z9hG4bK-k\r\n"
          "f: \"Bote\" <sip:0170@192.168.1.10>;tag=k1\r\nt: <sip:200@192.168.1.50>\r\nm: <sip:0170@192.168.1.10:5060>\r\n"
          "i: k-1@192.168.1.10\r\nCSEQ: 5 INVITE\r\nc: application/sdp\r\n\r\n"
          "v=0\r\nc=IN IP4 192.168.1.10\r\nm=audio 12000 RTP/AVP 8 101\r\na=rtpmap:101 telephone-event/8000\r\n");
  std::string ok = lastSent(s);
  CHECK(s.IsIncoming() && std::string(s.IncomingFrom()) == "0170", "eingehender Anruf in Kurzform");
  CHECK(header(ok, "Via: ") == "SIP/2.0/UDP 192.168.1.10:5060;branch=z9hG4bK-k" && header(ok, "CSeq: ") == "5 INVITE"
        && header(ok, "Call-ID: ") == "k-1@192.168.1.10", "200 OK mit uebernommenen Kopfzeilen");
  feed(s, "ACK sip:200@192.168.1.50 SIP/2.0\r\nv: x\r\ni: k-1@192.168.1.10\r\ncseq: 5 ACK\r\n\r\n");
  CHECK(!s.bInAckPending, "ACK in Kurzform erkannt");
  s.Hangup();
  CHECK(lastSent(s).rfind("BYE sip:0170@192.168.1.10:5060 SIP/2.0", 0) == 0, "BYE an Contact aus Kurzform");
}

// S9: zu grosse Pakete
static void testSizes() {
  Sip s(outBuf, sizeof(outBuf));
  s.Init("192.168.1.10", 5060, "192.168.1.50", 5060, "200", "geheim", 15);
  s.SetAcceptIncoming(true);
  std::string cut = INVITE_IN, ct = "Content-Type: application/sdp\r\n";
  cut.replace(cut.find(ct), ct.size(), ct + "Content-Length: 900\r\n");
  feed(s, cut);
  CHECK(lastSent(s).rfind("SIP/2.0 513", 0) == 0 && !s.IsBusy(), "Content-Length groesser als Body: 513");
  // Datagramm ueber 1460 Bytes (der ESP32 schneidet ab)
  std::string big = withCallId("in-big");
  std::string pad;
  while (big.size() + pad.size() < 1700) pad += "a=x-pad:" + std::string(60, 'p') + "\r\n";
  std::string body = big.substr(big.find("\r\n\r\n") + 4) + pad;
  big = big.substr(0, big.find("\r\n\r\n")) + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
  feed(s, big);
  CHECK(lastSent(s).rfind("SIP/2.0 513", 0) == 0 && !s.IsBusy(), "abgeschnittenes INVITE: 513");
  // Klingel-Anruf braucht kein SDP: trotzdem gemeldet
  s.SetRingFilter(everyone);
  feed(s, big);
  CHECK(lastSent(s).rfind("SIP/2.0 486", 0) == 0 && s.RingCall(), "abgeschnittener Klingel-Anruf gemeldet");
  s.SetRingFilter(nullptr);
  // Rest eines Pakets, das nicht in den Puffer passt, wird verworfen
  s.Udp.rx.push_back({SERVER, INVITE_IN});
  s.Processing(inBuf, 300);
  size_t n = s.Udp.sent.size();
  CHECK(!s.IsBusy(), "unvollstaendige Kopfzeilen verworfen");
  feed(s, "OPTIONS sip:200@192.168.1.50 SIP/2.0\r\nVia: v\r\nFrom: <sip:a@b>;tag=1\r\nTo: <sip:200@c>\r\nCall-ID: q1\r\nCSeq: 1 OPTIONS\r\n\r\n");
  CHECK(s.Udp.sent.size() == n + 1 && lastSent(s).rfind("SIP/2.0 200 OK", 0) == 0, "naechstes Paket wird gelesen");
  // Senden: mehr als ein Datagramm -> gar nicht
  n = s.Udp.sent.size();
  memset(outBuf, 'A', 1461);
  outBuf[1461] = 0;
  CHECK(s.SendUdp() == 513 && s.Udp.sent.size() == n, "zu grosses Paket nicht gesendet");
  std::string vias;
  for (int i = 0; i < 12; i++) vias += "Via: SIP/2.0/UDP 10.0.0." + std::to_string(i) + ":5060;branch=z9hG4bK" + std::string(68 - (i > 9), 'v') + "\r\n";
  std::string opt = "OPTIONS sip:200@x SIP/2.0\r\n" + vias + "From: a\r\nTo: b\r\nCall-ID: q\r\nCSeq: 1 OPTIONS\r\n\r\n";
  CHECK(opt.size() <= 1460 && opt.size() > 1440, "Anfrage passt knapp in ein Datagramm");
  feed(s, opt);
  CHECK(s.Udp.sent.size() == n, "zu grosse Antwort nicht gesendet");
}

// S8: Server als Hostname
static void testHostname() {
  WiFi = WiFiStub();
  Sip s(outBuf, sizeof(outBuf));
  s.Init("pbx.example", 5060, "192.168.1.50", 5060, "200", "geheim", 15);
  CHECK(WiFi.lookups == 0, "Init loest nicht auf");
  s.SetAcceptIncoming(true);
  feed(s, INVITE_IN, IPAddress(10, 0, 0, 1));
  CHECK(s.Udp.sent.empty() && !s.IsBusy(), "ohne bekannte Adresse keine Anfrage angenommen");
  CHECK(s.Dial("100", "Tuer") && !s.IsBusy() && s.LastCallResult() == Sip::CALL_FAILED && s.LastCallCode() == 503, "Anruf ohne Adresse gleich gescheitert");
  WiFi.fail = true;
  s.StartRegister(120);
  CHECK(s.RegisterStatus() == -2 && !s.IsRegistering() && s.Udp.sent.empty(), "DNS-Fehler gemeldet, nichts gesendet");
  WiFi.fail = false;
  s.StartRegister(120);
  CHECK(s.Udp.sent.size() == 1 && s.Udp.sentTo.back() == IPAddress(10, 0, 0, 1) && s.Udp.hostSends == 0, "REGISTER an aufgeloeste Adresse");
  int l = WiFi.lookups;
  tick(s, 600);
  CHECK(s.Udp.sent.size() == 2 && WiFi.lookups == l, "Wiederholung ohne DNS");
  WiFi.addr = IPAddress(10, 0, 0, 2);   // Anlage hat per DHCP eine neue Adresse
  s.StartRegister(120);
  CHECK(s.Udp.sentTo.back() == IPAddress(10, 0, 0, 2), "neue Adresse beim naechsten REGISTER");
  size_t n = s.Udp.sent.size();
  feed(s, INVITE_IN, IPAddress(10, 0, 0, 1));
  CHECK(s.Udp.sent.size() == n, "alte Adresse nicht mehr angenommen");
  feed(s, INVITE_IN, IPAddress(10, 0, 0, 2));
  CHECK(s.IsIncoming(), "Anfrage der neuen Adresse angenommen");
  l = WiFi.lookups;
  WiFi.fail = true;
  s.StartRegister(120);
  CHECK(WiFi.lookups == l, "waehrend eines Anrufs keine Aufloesung");
  s.Hangup();
  s.StartRegister(120);
  CHECK(s.Udp.sentTo.back() == IPAddress(10, 0, 0, 2) && s.IsRegistering(), "DNS-Fehler: letzte Adresse bleibt");
  WiFi = WiFiStub();
}

static void testOutgoing() {
  Sip s(outBuf, sizeof(outBuf));
  s.Init("192.168.1.10", 5060, "192.168.1.50", 5060, "200", "geheim", 15);
  s.SetBeepSeconds(6);
  g_random = 777;
  CHECK(s.Dial("0171234567890123", "Tueroeffner"), "Dial");
  std::string inv = lastSent(s);
  std::string cid = header(inv, "Call-ID: ");
  CHECK(inv.find("m=audio 16384 RTP/AVP 8 0 101") != std::string::npos, "SDP-Angebot");
  std::string to = "<sip:0171234567890123@sip.sehr-langer-anbieter.example.de>";
  feed(s, "SIP/2.0 401 Unauthorized\r\nVia: SIP/2.0/UDP 192.168.1.50:5060\r\nFrom: <sip:200@192.168.1.10>;tag=1\r\n"
          "To: " + to + ";tag=as1\r\nCall-ID: " + cid + "\r\nCSeq: 1 INVITE\r\n"
          "WWW-Authenticate: Digest realm=\"asterisk\", nonce=\"77\"\r\n\r\n");
  CHECK(s.Udp.sent.size() >= 3, "ACK und neues INVITE");
  std::string ack = s.Udp.sent[s.Udp.sent.size() - 2];
  CHECK(ack.rfind("ACK sip:0171234567890123@sip.sehr-langer-anbieter.example.de SIP/2.0", 0) == 0, "ACK mit langer Adresse");
  std::string inv2 = lastSent(s);
  CHECK(header(inv2, "CSeq: ") == "2 INVITE", "INVITE CSeq 2");
  CHECK(inv2.find("Authorization: Digest") != std::string::npos, "INVITE mit Auth");
  CHECK(header(inv2, "Via: ") != header(inv, "Via: "), "neuer Branch fuer neues INVITE");
  feed(s, "SIP/2.0 180 Ringing\r\nVia: v\r\nFrom: f\r\nTo: " + to + ";tag=as2\r\nCall-ID: " + cid + "\r\nCSeq: 2 INVITE\r\n\r\n");
  feed(s, "SIP/2.0 200 OK\r\nVia: SIP/2.0/UDP 192.168.1.50:5060\r\nFrom: <sip:200@192.168.1.10>;tag=1\r\nTo: " + to + ";tag=as2\r\n"
          "Call-ID: " + cid + "\r\nCSeq: 2 INVITE\r\nContent-Type: application/sdp\r\n\r\n"
          "v=0\r\nc=IN IP4 192.168.1.10\r\nm=audio 10000 RTP/AVP 8 101\r\na=rtpmap:101 telephone-event/8000\r\n");
  CHECK(s.LastCallResult() == Sip::CALL_ANSWERED && s.bInCall && s.rtpPt == 8, "angenommen, RTP PCMA");
  g_millis += 100;
  s.Processing(inBuf, sizeof(inBuf));
  CHECK(!s.Rtp.sent.empty() && s.Rtp.sent.back().size() == 172, "RTP-Frames");
  std::string ev(16, '\0');
  ev[0] = (char)0x80; ev[1] = 101; ev[6] = 1; ev[7] = 2; ev[12] = 10; ev[13] = (char)0x80;
  s.Rtp.rx.push_back({IPAddress(6, 6, 6, 6), ev});
  s.Processing(inBuf, sizeof(inBuf));
  CHECK(s.ReadDtmf() == 0, "DTMF von fremder Adresse ignoriert");
  for (int i = 0; i < 3; i++) s.Rtp.rx.push_back({SERVER, ev});
  s.Processing(inBuf, sizeof(inBuf));
  CHECK(s.ReadDtmf() == '*' && s.ReadDtmf() == 0, "DTMF einmal erkannt");
  ev[7] = 3;
  s.Rtp.rx.push_back({SERVER, ev});
  s.Processing(inBuf, sizeof(inBuf));
  CHECK(s.ReadDtmf() == '*', "schnelle Wiederholung erkannt");
  // 30-ms-Audio (252 Bytes) darf den Empfang nicht blockieren (ESP32)
  std::string audio(12 + 240, (char)0xD5);
  audio[0] = (char)0x80; audio[1] = 8;
  s.Rtp.rx.push_back({SERVER, audio});
  ev[7] = 4;
  s.Rtp.rx.push_back({SERVER, ev});
  s.Processing(inBuf, sizeof(inBuf));
  CHECK(s.ReadDtmf() == '*', "DTMF nach langem Audio-Paket");
  s.Hangup();
  CHECK(lastSent(s).rfind("BYE sip:0171234567890123@192.168.1.10 SIP/2.0", 0) == 0, "BYE");
  CHECK(header(lastSent(s), "CSeq: ") == "3 BYE", "BYE CSeq hoeher als INVITE");
  CHECK(!s.IsBusy(), "Anruf beendet");

  // Zweiter Anruf: Medien von anderer Adresse als die Anlage, telephone-event
  // in der Antwort auf PT 96. Ein spaetes Ende-Paket des vorigen Gespraechs
  // liegt noch im Empfangspuffer und darf nicht als neue Taste zaehlen.
  ev[7] = 4;
  s.Rtp.rx.push_back({SERVER, ev});
  CHECK(s.Dial("100", "Tuer"), "zweiter Anruf");
  std::string cid2 = header(lastSent(s), "Call-ID: ");
  feed(s, "SIP/2.0 200 OK\r\nVia: v\r\nFrom: <sip:200@192.168.1.10>;tag=1\r\nTo: <sip:100@192.168.1.10>;tag=x\r\n"
          "Call-ID: " + cid2 + "\r\nCSeq: 1 INVITE\r\nContent-Type: application/sdp\r\n\r\n"
          "v=0\r\nc=IN IP4 192.168.1.20\r\nm=audio 10000 RTP/AVP 8 96\r\na=rtpmap:96 telephone-event/8000\r\n");
  CHECK(s.bInCall && s.dtmfPt == 96, "zweiter Anruf angenommen, DTMF-PT 96");
  CHECK(s.ReadDtmf() == 0, "altes Paket des vorigen Gespraechs verworfen");
  ev[7] = 5;
  s.Rtp.rx.push_back({SERVER, ev});
  s.Processing(inBuf, sizeof(inBuf));
  CHECK(s.ReadDtmf() == '*', "DTMF mit angebotenem PT 101 von der Anlage");
  ev[1] = 96; ev[7] = 6;
  s.Rtp.rx.push_back({IPAddress(192, 168, 1, 20), ev});
  s.Rtp.rx.push_back({IPAddress(6, 6, 6, 6), ev});
  s.Processing(inBuf, sizeof(inBuf));
  CHECK(s.ReadDtmf() == '*', "DTMF mit PT der Antwort");
  char info[128];
  s.CallInfo(info, sizeof(info));
  CHECK(std::string(info) == "PCMA, DTMF-PT 96, Gegenstelle 192.168.1.20, RTP 2, Tasten 2, verworfen 1 von 6.6.6.6", "Diagnose");
  s.Hangup();
}

static void testIncoming() {
  Sip s(outBuf, sizeof(outBuf));
  s.Init("192.168.1.10", 5060, "192.168.1.50", 5060, "200", "geheim", 15);
  feed(s, INVITE_IN);
  CHECK(lastSent(s).rfind("SIP/2.0 603 Decline", 0) == 0, "ohne Freigabe abgelehnt");
  CHECK(!s.IsBusy(), "nicht angenommen");
  s.SetAcceptIncoming(true);
  size_t n = s.Udp.sent.size();
  feed(s, INVITE_IN, IPAddress(6, 6, 6, 6));
  CHECK(s.Udp.sent.size() == n && !s.IsBusy(), "INVITE von fremder Adresse ignoriert");
  feed(s, INVITE_IN);
  std::string ok = lastSent(s);
  CHECK(ok.rfind("SIP/2.0 200 OK", 0) == 0, "200 OK");
  CHECK(ok.find("branch=z9hG4bK-a") != std::string::npos && ok.find("branch=z9hG4bK-b") != std::string::npos, "alle Via-Zeilen");
  CHECK(header(ok, "To: ").find("<sip:200@192.168.1.50>;tag=") == 0, "To mit eigenem Tag");
  CHECK(header(ok, "CSeq: ") == "102 INVITE", "CSeq uebernommen");
  CHECK(ok.find("m=audio 16384 RTP/AVP 8 101") != std::string::npos, "Antwort PCMA + DTMF");
  CHECK(s.IsIncoming() && s.NewIncoming() && !s.NewIncoming(), "NewIncoming einmal");
  CHECK(std::string(s.IncomingFrom()) == "0171555", "Rufnummer des Anrufers");
  n = s.Udp.sent.size();
  g_millis += 600;
  s.Processing(inBuf, sizeof(inBuf));
  CHECK(s.Udp.sent.size() == n + 1 && lastSent(s).rfind("SIP/2.0 200 OK", 0) == 0, "200 OK wiederholt");
  feed(s, "ACK sip:200@192.168.1.50:5060 SIP/2.0\r\nVia: x\r\nCall-ID: in-123@192.168.1.10\r\nCSeq: 102 ACK\r\n\r\n");
  n = s.Udp.sent.size();
  g_millis += 5000;
  s.Processing(inBuf, sizeof(inBuf));
  CHECK(s.Udp.sent.size() == n, "nach ACK keine Wiederholung");
  feed(s, withCallId("in-999"));
  CHECK(lastSent(s).rfind("SIP/2.0 486 Busy Here", 0) == 0, "zweiter Anruf besetzt");
  feed(s, "INFO sip:200@192.168.1.50 SIP/2.0\r\nVia: SIP/2.0/UDP 192.168.1.10:5060;branch=z9hG4bK-i\r\n"
          "From: <sip:0171555@192.168.1.10>;tag=abc\r\nTo: <sip:200@192.168.1.50>;tag=x\r\n"
          "Call-ID: in-123@192.168.1.10\r\nCSeq: 103 INFO\r\n\r\nSignal=7\r\nDuration=160\r\n");
  CHECK(s.ReadDtmf() == '7', "DTMF per INFO");
  CHECK(lastSent(s).rfind("SIP/2.0 200 OK", 0) == 0, "INFO beantwortet");
  feed(s, "OPTIONS sip:200@192.168.1.50 SIP/2.0\r\nVia: v\r\nFrom: <sip:a@b>;tag=1\r\nTo: <sip:200@c>\r\nCall-ID: q1\r\nCSeq: 1 OPTIONS\r\n\r\n");
  CHECK(lastSent(s).rfind("SIP/2.0 200 OK", 0) == 0 && header(lastSent(s), "To: ").find(";tag=") != std::string::npos, "OPTIONS 200 mit Tag");
  s.Hangup();
  std::string bye = lastSent(s);
  CHECK(bye.rfind("BYE sip:0171555@192.168.1.10:5060 SIP/2.0", 0) == 0, "BYE an Contact");
  CHECK(header(bye, "From: ").find("<sip:200@192.168.1.50>;tag=") == 0, "BYE From = unser To mit Tag");
  CHECK(header(bye, "To: ") == "\"Handwerker\" <sip:0171555@192.168.1.10>;tag=abc", "BYE To = Anrufer");
  CHECK(!s.IsBusy() && !s.IsIncoming(), "beendet");
  feed(s, withCallId("in-555"));
  CHECK(s.IsIncoming(), "neuer eingehender Anruf");
  feed(s, "BYE sip:200@192.168.1.50 SIP/2.0\r\nVia: v\r\nFrom: f\r\nTo: t;tag=1\r\nCall-ID: in-555@192.168.1.10\r\nCSeq: 104 BYE\r\n\r\n");
  CHECK(!s.IsBusy() && lastSent(s).rfind("SIP/2.0 200 OK", 0) == 0, "BYE vom Anrufer");
  s.SetIncomingSeconds(30);
  feed(s, withCallId("in-777"));
  g_millis += 31000;
  s.Processing(inBuf, sizeof(inBuf));
  CHECK(!s.IsBusy() && lastSent(s).rfind("BYE ", 0) == 0, "nach 30 s aufgelegt");
  std::string noSdp = withCallId("in-888");
  noSdp = noSdp.substr(0, noSdp.find("\r\n\r\n") + 4);
  feed(s, noSdp);
  CHECK(lastSent(s).rfind("SIP/2.0 488", 0) == 0, "ohne SDP 488");
}

static bool ringFilter(const char *caller) { return strcmp(caller, "31") == 0; }

static void testRingCall() {
  Sip s(outBuf, sizeof(outBuf));
  s.Init("192.168.1.10", 5060, "192.168.1.50", 5060, "200", "geheim", 15);
  s.SetRingFilter(ringFilter);
  std::string ring = INVITE_IN;
  ring.replace(ring.find("sip:0171555@"), 12, "sip:31@");
  feed(s, ring);
  CHECK(lastSent(s).rfind("SIP/2.0 486 Busy Here", 0) == 0, "Klingel-Anruf mit 486 abgewiesen");
  CHECK(s.RingCall() && !s.RingCall(), "Klingeln einmal gemeldet");
  CHECK(std::string(s.RingCaller()) == "31", "Nummer der Klingel");
  CHECK(!s.IsBusy(), "Klingel-Anruf nicht angenommen");
  feed(s, ring);   // Wiederholung desselben INVITE
  CHECK(!s.RingCall(), "Wiederholung nicht erneut gemeldet");
  std::string ring2 = ring;
  ring2.replace(ring2.find("in-123"), 6, "in-124");
  feed(s, ring2);
  CHECK(s.RingCall(), "neuer Klingel-Anruf gemeldet");
  feed(s, withCallId("in-200"));   // anderer Anrufer, Annahme aus
  CHECK(lastSent(s).rfind("SIP/2.0 603", 0) == 0 && !s.RingCall(), "anderer Anrufer kein Klingeln");
  CHECK(std::string(s.LastCaller()) == "0171555", "letzter Anrufer gemerkt");
  // Klingeln auch waehrend eines laufenden Anrufs
  s.SetAcceptIncoming(true);
  feed(s, withCallId("in-201"));
  CHECK(s.IsIncoming(), "eingehender Anruf laeuft");
  std::string ring3 = ring;
  ring3.replace(ring3.find("in-123"), 6, "in-125");
  feed(s, ring3);
  CHECK(s.RingCall() && s.IsIncoming(), "Klingeln trotz laufendem Anruf, Anruf bleibt");
}

static void testMalformed() {
  Sip s(outBuf, sizeof(outBuf));
  s.Init("192.168.1.10", 5060, "192.168.1.50", 5060, "200", "geheim", 15);
  s.SetAcceptIncoming(true);
  s.SetBeepSeconds(6);
  feed(s, std::string(5000, 'A'));
  feed(s, "SIP/2.0 401 Unauthorized\r\nTo: <sip:ohne-ende\r\nCSeq: 1 INVITE\r\nCall-ID: x\r\n");
  feed(s, "INVITE sip:x SIP/2.0\r\nCall-ID: " + std::string(300, 'c') + "\r\nFrom: a\r\nTo: b\r\n\r\n");
  feed(s, "INVITE sip:x SIP/2.0\r\n" + std::string(2000, 'V') + "\r\n");
  std::string longVia;
  for (int i = 0; i < 20; i++) longVia += "Via: SIP/2.0/UDP 10.0.0." + std::to_string(i) + ":5060;branch=z9hG4bK" + std::string(20, 'x') + "\r\n";
  feed(s, "INVITE sip:x SIP/2.0\r\n" + longVia + "From: a\r\nTo: b\r\nCall-ID: v\r\nCSeq: 1 INVITE\r\n\r\nm=audio 1 RTP/AVP 8\r\nc=IN IP4 1.2.3.4\r\n");
  s.Dial("100", "Tuer");
  std::string cid = header(lastSent(s), "Call-ID: ");
  std::string big = "SIP/2.0 200 OK\r\nVia: SIP/2.0/UDP 192.168.1.50:5060;branch=" + std::string(200, 'b') + ";received=192.168.1.50;rport=5060\r\n"
    "From: \"" + std::string(100, 'f') + "\" <sip:200@192.168.1.10>;tag=" + std::string(60, 't') + "\r\n"
    "To: <sip:100@192.168.1.10>;tag=" + std::string(120, 'T') + "\r\nCall-ID: " + cid + "\r\nCSeq: 1 INVITE\r\n\r\n";
  feed(s, big);
  CHECK(s.LastCallResult() == Sip::CALL_ANSWERED, "200 OK mit langen Kopfzeilen angenommen");
  s.Hangup();
  srand(1);
  const char *prefixes[] = { "SIP/2.0 200 OK\r\n", "SIP/2.0 401 x\r\n", "INVITE s SIP/2.0\r\n", "BYE s SIP/2.0\r\n", "INFO s SIP/2.0\r\n", "" };
  const char *tok[] = { "\r\n", "Call-ID: ", "CSeq: 1 INVITE", "To: <", "From: ", "Via: ", "realm=\"", "nonce=\"", "WWW-Authenticate: Digest ",
                        "m=audio ", "RTP/AVP ", "c=IN IP4 ", "a=rtpmap:", " telephone-event", "Signal=", "\"", ">", ";tag=", "Contact: <sip:",
                        "\r\ni:", "\r\ncseq : 2 INVITE", "\r\nt: sip:", "\r\nl: 4000", "\r\n\t", "\n", "\r\n\r\n", ":", "Content-Length: 900\r\n" };
  const int ntok = (int)(sizeof(tok) / sizeof(tok[0]));
  for (int i = 0; i < 20000; i++) {
    std::string p = prefixes[rand() % 6];
    int len = rand() % 1800;
    while ((int)p.size() < len) {
      if (rand() % 3) p += tok[rand() % ntok];
      else p += (char)(rand() % 256);
    }
    feed(s, p);
    if (s.IsBusy() && rand() % 4 == 0) s.Hangup();
  }
  CHECK(true, "Zufallsdaten ohne Absturz");
}

int main() {
  testDigest();
  testRegister();
  testRegisterRetry();
  testOutgoing();
  testRetransmit();
  testIncoming();
  testIncomingBye();
  testDtmfOrigin();
  testRtpHeader();
  testReinvite();
  testRingCall();
  testRingFilter2();
  testNormalize();
  testSizes();
  testHostname();
  testMalformed();
  printf("SIP: %d Pruefungen, %d Fehler\n", checks, fails);
  return fails ? 1 : 0;
}

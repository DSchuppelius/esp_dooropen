// Host-Test der ArduinoSIP-Bibliothek (mit AddressSanitizer/UBSan uebersetzen,
// siehe tests/host/run.sh). Prueft Digest (RFC 2617), REGISTER, ausgehende und
// eingehende Anrufe, DTMF sowie kaputte/zu grosse Pakete.
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
  CHECK(!s.IsRegistered() && s.RegisterStatus() == 401, "falsches Passwort erkannt");
  s.StartRegister(120);
  g_millis += 2500;
  s.Processing(inBuf, sizeof(inBuf));
  CHECK(!s.IsRegistering() && s.RegisterStatus() == 0, "Zeitueberschreitung");
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
  s.Hangup();
  CHECK(lastSent(s).rfind("BYE sip:0171234567890123@192.168.1.10 SIP/2.0", 0) == 0, "BYE");
  CHECK(header(lastSent(s), "CSeq: ") == "3 BYE", "BYE CSeq hoeher als INVITE");
  CHECK(!s.IsBusy(), "Anruf beendet");
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
                        "m=audio ", "RTP/AVP ", "c=IN IP4 ", "a=rtpmap:", " telephone-event", "Signal=", "\"", ">", ";tag=", "Contact: <sip:" };
  for (int i = 0; i < 20000; i++) {
    std::string p = prefixes[rand() % 6];
    int len = rand() % 1800;
    while ((int)p.size() < len) {
      if (rand() % 3) p += tok[rand() % 19];
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
  testOutgoing();
  testIncoming();
  testRingCall();
  testMalformed();
  printf("SIP: %d Pruefungen, %d Fehler\n", checks, fails);
  return fails ? 1 : 0;
}

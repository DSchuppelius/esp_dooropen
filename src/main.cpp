#include <Arduino.h>
#if defined(ESP32)
  #include <WiFi.h>
  #include <WebServer.h>
#else
  #include <ESP8266WiFi.h>
  #include <ESP8266WebServer.h>
#endif
#include <WiFiManager.h>
#include <EEPROM.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoSIP.h>

#include "config.h"

// ------------------------------------------------------------
//  Globale Objekte
// ------------------------------------------------------------
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
#if defined(ESP32)
WebServer server(80);
#else
ESP8266WebServer server(80);
#endif

// SIP: Ein- und Ausgabepuffer + Client
char acSipIn[2048];
char acSipOut[2048];
Sip  aSip(acSipOut, sizeof(acSipOut));
String myIpStr;                      // eigene IP (muss fuer Sip.Init leben)

// Stabile Puffer fuer die SIP-Lib: sie merkt sich nur ZEIGER, keine Kopien.
// Deshalb duerfen diese Adressen sich nie aendern (kein String::c_str()!).
char sipServerBuf[41];
char sipUserBuf[25];
char sipPwBuf[33];
char myIpBuf[16];
char dialBuf[MAX_DIAL_LEN + 1];

// Laufzeit-Einstellungen (aus EEPROM; Defaults aus config.h)
uint8_t  buzzerSeconds     = DEFAULT_BUZZER_SECONDS;
bool     buzzerActive      = false;
uint32_t buzzerOffAt       = 0;      // millis-Zeitpunkt zum Abschalten
uint32_t buzzerTriggers    = 0;      // Zaehler

String   dialNr            = DEFAULT_DIAL_NR;  // Zielrufnummer
bool     callOnRing        = true;            // beim Klingeln automatisch anrufen
uint32_t callCount         = 0;
String   sipServer         = SIP_SERVER_IP;
uint16_t sipPort           = SIP_PORT;
String   sipUser           = SIP_USER;
String   sipPw             = SIP_PW;
uint32_t lastRegisterAt    = 0;

bool     signalActive      = false;  // aktueller (entprellter) Zustand
bool     lastSignalRaw     = false;
uint32_t lastSignalChange  = 0;
uint32_t signalCount       = 0;
uint32_t lastRingAt        = 0;      // millis des letzten Klingelns
bool     displayDirty      = true;
uint32_t lastActivityAt    = 0;      // fuer Bildschirmschoner
bool     displayOn         = true;

// ------------------------------------------------------------
//  EEPROM Persistenz (als Struct)
// ------------------------------------------------------------
static const uint8_t SETTINGS_MAGIC = 0x53;   // aendern, wenn sich Layout aendert
struct Settings {
  uint8_t  magic;
  uint8_t  buzzerSeconds;
  uint8_t  callOnRing;
  uint16_t sipPort;
  char     dialNr[MAX_DIAL_LEN + 1];
  char     sipServer[41];
  char     sipUser[25];
  char     sipPw[33];
};
static const uint16_t EEPROM_SIZE = sizeof(Settings) + 8;

static void copyToField(char *dst, size_t dstSize, const String &src) {
  size_t n = src.length();
  if (n > dstSize - 1) n = dstSize - 1;
  memcpy(dst, src.c_str(), n);
  dst[n] = '\0';
}

void loadSettings() {
  EEPROM.begin(EEPROM_SIZE);
  Settings s;
  EEPROM.get(0, s);
  if (s.magic != SETTINGS_MAGIC) return;   // nichts gespeichert -> Defaults behalten
  if (s.buzzerSeconds >= MIN_BUZZER_SECONDS && s.buzzerSeconds <= MAX_BUZZER_SECONDS)
    buzzerSeconds = s.buzzerSeconds;
  callOnRing = s.callOnRing != 0;
  if (s.sipPort > 0) sipPort = s.sipPort;
  s.dialNr[sizeof(s.dialNr) - 1]       = '\0';
  s.sipServer[sizeof(s.sipServer) - 1] = '\0';
  s.sipUser[sizeof(s.sipUser) - 1]     = '\0';
  s.sipPw[sizeof(s.sipPw) - 1]         = '\0';
  dialNr    = String(s.dialNr);
  sipServer = String(s.sipServer);
  sipUser   = String(s.sipUser);
  sipPw     = String(s.sipPw);
}

void saveSettings() {
  Settings s;
  memset(&s, 0, sizeof(s));
  s.magic         = SETTINGS_MAGIC;
  s.buzzerSeconds = buzzerSeconds;
  s.callOnRing    = callOnRing ? 1 : 0;
  s.sipPort       = sipPort;
  copyToField(s.dialNr,    sizeof(s.dialNr),    dialNr);
  copyToField(s.sipServer, sizeof(s.sipServer), sipServer);
  copyToField(s.sipUser,   sizeof(s.sipUser),   sipUser);
  copyToField(s.sipPw,     sizeof(s.sipPw),     sipPw);
  EEPROM.put(0, s);
  EEPROM.commit();
}

// SIP-Client mit den aktuellen Einstellungen (neu) initialisieren
void initSip() {
  if (myIpStr.length() == 0) myIpStr = WiFi.localIP().toString();
  copyToField(sipServerBuf, sizeof(sipServerBuf), sipServer);
  copyToField(sipUserBuf,   sizeof(sipUserBuf),   sipUser);
  copyToField(sipPwBuf,     sizeof(sipPwBuf),     sipPw);
  copyToField(myIpBuf,      sizeof(myIpBuf),      myIpStr);
  aSip.Init(sipServerBuf, sipPort, myIpBuf, sipPort,
            sipUserBuf, sipPwBuf, SIP_MAX_DIAL_SEC);
  aSip.SetBeepSeconds(SIP_BEEP_SECONDS);
  bool reg = false;
  for (int i = 0; i < 3 && !reg; i++) {   // erster Versuch nach Boot scheitert oft am Timing
    reg = aSip.Register(SIP_REG_EXPIRES);
    if (!reg) delay(500);
  }
  lastRegisterAt = millis();
  Serial.printf("SIP-REGISTER: %s\n", reg ? "OK" : "fehlgeschlagen");
}

// ------------------------------------------------------------
//  Relais / Summer
// ------------------------------------------------------------
void setRelay(bool on) {
  bool level = RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(PIN_RELAY, level ? HIGH : LOW);
  bool led = STATUS_LED_ACTIVE_LOW ? !on : on;
  digitalWrite(PIN_STATUS_LED, led ? HIGH : LOW);
}

// Aktivitaet melden -> Bildschirmschoner-Timer zuruecksetzen und Display wecken
void markActivity() {
  lastActivityAt = millis();
  if (!displayOn) {
    display.ssd1306_command(SSD1306_DISPLAYON);
    displayOn = true;
    displayDirty = true;
  }
}

void startBuzzer() {
  buzzerActive = true;
  buzzerOffAt  = millis() + (uint32_t)buzzerSeconds * 1000UL;
  buzzerTriggers++;
  setRelay(true);
  markActivity();
  displayDirty = true;
}

void stopBuzzer() {
  buzzerActive = false;
  setRelay(false);
  displayDirty = true;
}

// SIP-Anruf an die Zielrufnummer ausloesen (nur Signalisierung, kein Audio)
bool placeCall() {
  if (dialNr.length() == 0) return false;
  copyToField(dialBuf, sizeof(dialBuf), dialNr);
  aSip.Dial(dialBuf, SIP_CALLER_NAME);
  callCount++;
  markActivity();
  return true;
}

// ------------------------------------------------------------
//  Display
// ------------------------------------------------------------
void drawDisplay() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  bool ringing = (millis() - lastRingAt) < RING_NOTIFY_MS && lastRingAt != 0;

  // Auffaelliger Klingel-Screen
  if (ringing) {
    display.setTextSize(2);
    display.setCursor(8, 8);
    display.println(F("ES"));
    display.setCursor(8, 28);
    display.println(F("KLINGELT!"));
    display.setTextSize(1);
    display.setCursor(0, 52);
    display.print(F("Anzahl: "));
    display.println(signalCount);
    display.display();
    return;
  }

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(F("Tueroeffner"));
  display.drawFastHLine(0, 10, OLED_WIDTH, SSD1306_WHITE);

  display.setCursor(0, 16);
  if (WiFi.status() == WL_CONNECTED) {
    display.print(F("IP "));
    display.println(WiFi.localIP().toString());
  } else {
    display.println(F("WLAN: offline"));
  }

  display.setCursor(0, 28);
  display.print(F("Signal: "));
  display.println(signalActive ? F("AKTIV") : F("ruhig"));

  display.setCursor(0, 40);
  display.print(F("Summer: "));
  display.println(buzzerActive ? F("AN") : F("aus"));

  display.setCursor(0, 52);
  display.print(F("Dauer: "));
  display.print(buzzerSeconds);
  display.println(F("s"));

  display.display();
}

// ------------------------------------------------------------
//  Signal-Eingang (entprellt)
// ------------------------------------------------------------
void handleSignalInput() {
  int raw = digitalRead(PIN_SIGNAL);
  bool pressed = SIGNAL_ACTIVE_LOW ? (raw == LOW) : (raw == HIGH);

  if (pressed != lastSignalRaw) {
    lastSignalRaw    = pressed;
    lastSignalChange = millis();
  }

  if ((millis() - lastSignalChange) > SIGNAL_DEBOUNCE_MS) {
    if (pressed != signalActive) {
      signalActive = pressed;
      if (signalActive) {
        signalCount++;
        lastRingAt = millis();
        markActivity();
        if (callOnRing) placeCall();
      }
      displayDirty = true;
    }
  }
}

// ------------------------------------------------------------
//  Webserver
// ------------------------------------------------------------
const char PAGE_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="de"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Tueroeffner</title>
<style>
 :root{color-scheme:dark}
 body{font-family:system-ui,sans-serif;margin:0;background:#111;color:#eee;
      display:flex;min-height:100vh;align-items:center;justify-content:center}
 .card{background:#1c1c1e;padding:24px;border-radius:16px;width:320px;
       box-shadow:0 8px 30px rgba(0,0,0,.5)}
 h1{font-size:20px;margin:0 0 16px}
 .row{display:flex;justify-content:space-between;padding:6px 0;font-size:14px;color:#aaa}
 .row b{color:#eee}
 button{width:100%;padding:16px;font-size:18px;border:0;border-radius:12px;
        background:#0a84ff;color:#fff;cursor:pointer;margin-top:8px}
 button:active{background:#0060df}
 .open{background:#30d158}
 input{width:70px;padding:6px;border-radius:8px;border:1px solid #444;
       background:#000;color:#eee;text-align:center}
 .status{margin:16px 0;padding:12px;border-radius:12px;background:#000}
 .on{color:#30d158}.off{color:#888}
 .bell{display:none;margin:0 0 16px;padding:16px;border-radius:12px;
       background:#ff9f0a;color:#000;font-weight:700;text-align:center;font-size:18px}
 .bell.show{display:block;animation:blink 1s infinite}
 @keyframes blink{50%{opacity:.4}}
 .tabs{display:flex;gap:6px;margin-bottom:16px}
 .tabs button{margin:0;padding:10px;font-size:15px;background:#2c2c2e}
 .tabs button.active{background:#0a84ff}
 .tab{display:none}
 .tab.show{display:block}
 hr{border:0;border-top:1px solid #333;margin:18px 0}
</style></head><body>
<div class="card">
 <h1>Tueroeffner</h1>
 <div class="tabs">
   <button id="tb0" class="active" onclick="tab(0)">Steuerung</button>
   <button id="tb1" onclick="tab(1)">Konfiguration</button>
 </div>

 <div id="t0" class="tab show">
   <div class="bell" id="bell">&#128276; Es klingelt!</div>
   <div class="status">
     <div class="row">Signal <b id="sig">-</b></div>
     <div class="row">Summer <b id="buz">-</b></div>
     <div class="row">Klingel-Anzahl <b id="rings">-</b></div>
     <div class="row">Ausloesungen <b id="cnt">-</b></div>
     <div class="row">Anrufe <b id="calls">-</b></div>
     <div class="row">SIP registriert <b id="reg">-</b></div>
   </div>
   <button class="open" onclick="openDoor()">Oeffnen</button>
   <button id="snd" onclick="initAudio();beep()">&#128263; Ton aktivieren / testen</button>
   <button onclick="testCall()">&#128222; Test-Anruf</button>
 </div>

 <div id="t1" class="tab">
   <div class="row"><span>Summer-Dauer</span>
     <span><input id="dur" type="number" min="1" max="30"> s</span></div>
   <button onclick="saveDur()">Dauer speichern</button>
   <hr>
   <div class="row"><span>Anruf-Ziel (Nebenstelle)</span>
     <span><input id="dial" type="text" style="width:110px"></span></div>
   <div class="row"><span>Bei Klingeln anrufen</span>
     <span><input id="auto" type="checkbox"></span></div>
   <button onclick="saveDial()">Anruf-Ziel speichern</button>
   <hr>
   <div class="row"><span>SIP-Server</span>
     <span><input id="sipserver" type="text" style="width:130px"></span></div>
   <div class="row"><span>SIP-Port</span>
     <span><input id="sipport" type="number" min="1" max="65535"></span></div>
   <div class="row"><span>SIP-Benutzer</span>
     <span><input id="sipuser" type="text" style="width:130px"></span></div>
   <div class="row"><span>SIP-Passwort</span>
     <span><input id="sippw" type="password" style="width:130px" placeholder="unveraendert"></span></div>
   <button onclick="saveSip()">SIP-Daten speichern</button>
 </div>
</div>
<script>
let audioCtx=null, soundOn=false, wasRinging=false;
function tab(n){
  for(let i=0;i<2;i++){
    document.getElementById('t'+i).className=i==n?'tab show':'tab';
    document.getElementById('tb'+i).className=i==n?'active':'';
  }
}
function initAudio(){
  if(!audioCtx){audioCtx=new (window.AudioContext||window.webkitAudioContext)();}
  if(audioCtx.state==='suspended')audioCtx.resume();
  soundOn=true;
  document.getElementById('snd').textContent='\uD83D\uDD0A Ton an';
}
function beep(){
  if(!audioCtx)return;
  const t=audioCtx.currentTime;
  for(let i=0;i<3;i++){
    const o=audioCtx.createOscillator(),g=audioCtx.createGain();
    o.type='square';o.frequency.value=880;
    o.connect(g);g.connect(audioCtx.destination);
    const s=t+i*0.35;
    g.gain.setValueAtTime(0.001,s);
    g.gain.exponentialRampToValueAtTime(0.3,s+0.02);
    g.gain.exponentialRampToValueAtTime(0.001,s+0.25);
    o.start(s);o.stop(s+0.26);
  }
}
document.addEventListener('click',initAudio,{once:true});
async function refresh(){
  try{
    const r=await fetch('/status');const s=await r.json();
    document.getElementById('sig').textContent=s.signal?'AKTIV':'ruhig';
    document.getElementById('sig').className=s.signal?'on':'off';
    document.getElementById('buz').textContent=s.buzzer?'AN':'aus';
    document.getElementById('buz').className=s.buzzer?'on':'off';
    document.getElementById('rings').textContent=s.signals;
    document.getElementById('cnt').textContent=s.triggers;
    document.getElementById('calls').textContent=s.calls;
    document.getElementById('reg').textContent=s.registered?'JA':'nein';
    document.getElementById('reg').className=s.registered?'on':'off';
    document.getElementById('bell').className=s.ringing?'bell show':'bell';
    if(s.ringing && !wasRinging && soundOn)beep();
    wasRinging=s.ringing;
    const d=document.getElementById('dur');
    if(document.activeElement!==d)d.value=s.seconds;
    const dl=document.getElementById('dial');
    if(document.activeElement!==dl)dl.value=s.dial;
    const au=document.getElementById('auto');
    if(document.activeElement!==au)au.checked=s.callonring;
    const ss=document.getElementById('sipserver');
    if(document.activeElement!==ss)ss.value=s.sipserver;
    const sp=document.getElementById('sipport');
    if(document.activeElement!==sp)sp.value=s.sipport;
    const su=document.getElementById('sipuser');
    if(document.activeElement!==su)su.value=s.sipuser;
    const pw=document.getElementById('sippw');
    pw.placeholder=s.haspw?'unveraendert':'nicht gesetzt';
  }catch(e){}
}
async function openDoor(){await fetch('/open',{method:'POST'});refresh();}
async function saveDur(){
  const v=document.getElementById('dur').value;
  await fetch('/setduration?s='+v,{method:'POST'});refresh();
}
async function saveDial(){
  const nr=encodeURIComponent(document.getElementById('dial').value);
  const au=document.getElementById('auto').checked?'1':'0';
  await fetch('/setdial?nr='+nr+'&auto='+au,{method:'POST'});refresh();
}
async function testCall(){await fetch('/call',{method:'POST'});refresh();}
async function saveSip(){
  const q=new URLSearchParams();
  q.set('server',document.getElementById('sipserver').value);
  q.set('port',document.getElementById('sipport').value);
  q.set('user',document.getElementById('sipuser').value);
  q.set('pw',document.getElementById('sippw').value);
  await fetch('/setsip?'+q.toString(),{method:'POST'});
  document.getElementById('sippw').value='';refresh();
}
setInterval(refresh,1000);refresh();
</script>
</body></html>
)HTML";

void handleRoot() {
  server.send_P(200, "text/html", PAGE_HTML);
}

void handleStatus() {
  bool ringing = (millis() - lastRingAt) < RING_NOTIFY_MS && lastRingAt != 0;
  String json = "{";
  json += "\"signal\":"   + String(signalActive ? "true" : "false");
  json += ",\"ringing\":" + String(ringing ? "true" : "false");
  json += ",\"buzzer\":"  + String(buzzerActive ? "true" : "false");
  json += ",\"seconds\":" + String(buzzerSeconds);
  json += ",\"triggers\":" + String(buzzerTriggers);
  json += ",\"signals\":"  + String(signalCount);
  json += ",\"dial\":\""   + dialNr + "\"";
  json += ",\"callonring\":" + String(callOnRing ? "true" : "false");
  json += ",\"calls\":"    + String(callCount);
  json += ",\"sipserver\":\"" + sipServer + "\"";
  json += ",\"sipport\":"  + String(sipPort);
  json += ",\"sipuser\":\"" + sipUser + "\"";
  json += ",\"haspw\":"    + String(sipPw.length() > 0 ? "true" : "false");
  json += ",\"registered\":" + String(aSip.IsRegistered() ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleOpen() {
  startBuzzer();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleSetDuration() {
  if (server.hasArg("s")) {
    int v = server.arg("s").toInt();
    if (v >= MIN_BUZZER_SECONDS && v <= MAX_BUZZER_SECONDS) {
      buzzerSeconds = (uint8_t)v;
      saveSettings();
      displayDirty = true;
      server.send(200, "application/json", "{\"ok\":true}");
      return;
    }
  }
  server.send(400, "application/json", "{\"ok\":false}");
}

void handleCall() {
  bool ok = placeCall();
  server.send(ok ? 200 : 400, "application/json",
              ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"keine Zielnummer\"}");
}

void handleSetDial() {
  if (server.hasArg("nr")) {
    String nr = server.arg("nr");
    if (nr.length() <= MAX_DIAL_LEN) {
      dialNr = nr;
      if (server.hasArg("auto")) callOnRing = server.arg("auto") == "1";
      saveSettings();
      server.send(200, "application/json", "{\"ok\":true}");
      return;
    }
  }
  server.send(400, "application/json", "{\"ok\":false}");
}

void handleSetSip() {
  if (server.hasArg("server")) sipServer = server.arg("server");
  if (server.hasArg("port")) {
    int p = server.arg("port").toInt();
    if (p > 0 && p <= 65535) sipPort = (uint16_t)p;
  }
  if (server.hasArg("user")) sipUser = server.arg("user");
  // Passwort nur uebernehmen, wenn ein neues angegeben wurde
  if (server.hasArg("pw") && server.arg("pw").length() > 0) sipPw = server.arg("pw");
  saveSettings();
  initSip();   // mit neuen Daten neu registrieren
  server.send(200, "application/json", "{\"ok\":true}");
}

// ------------------------------------------------------------
//  Setup / Loop
// ------------------------------------------------------------
// Wird aufgerufen, sobald das WLAN-Konfig-Portal (AP) startet.
void configModeCallback(WiFiManager *wm) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(F("WLAN einrichten:"));
  display.drawFastHLine(0, 10, OLED_WIDTH, SSD1306_WHITE);
  display.setCursor(0, 14);
  display.println(F("1) WLAN waehlen:"));
  display.print(F("   "));
  display.println(F(WIFI_AP_NAME));
  display.setCursor(0, 38);
  display.println(F("2) Browser:"));
  display.print(F("   http://"));
  display.println(WiFi.softAPIP().toString());
  display.display();
}

void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_STATUS_LED, OUTPUT);
  setRelay(false);
  pinMode(PIN_SIGNAL, INPUT_PULLUP);

  loadSettings();

  Wire.begin(PIN_SDA, PIN_SCL);

  // I2C-Bus scannen und gefundene Adressen ausgeben (Diagnose)
  Serial.println(F("I2C-Scan:"));
  uint8_t foundAddr = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  gefunden: 0x%02X\n", addr);
      if (addr == 0x3C || addr == 0x3D) foundAddr = addr;
    }
  }
  if (foundAddr == 0) {
    Serial.println(F("  Kein OLED gefunden! (HW-364A: OLED an D5/D6)"));
    foundAddr = OLED_ADDR;
  }

  if (!display.begin(SSD1306_SWITCHCAPVCC, foundAddr)) {
    Serial.printf("SSD1306 Init fehlgeschlagen (Adresse 0x%02X)!\n", foundAddr);
  } else {
    Serial.printf("Display OK auf 0x%02X\n", foundAddr);
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(F("Starte..."));
    display.println(F("WLAN verbinden"));
    display.display();
  }

  WiFiManager wm;
  wm.setAPCallback(configModeCallback);
  wm.setConfigPortalTimeout(300);
  if (!wm.autoConnect(WIFI_AP_NAME)) {
    Serial.println(F("WLAN fehlgeschlagen, Neustart"));
    ESP.restart();
  }
  // Modem-Sleep aus: sonst verschluckt der ESP Unicast-Pakete (Ping/HTTP)
#if defined(ESP32)
  WiFi.setSleep(false);
#else
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
#endif
  Serial.print(F("IP: "));
  Serial.println(WiFi.localIP());

  // SIP-Client initialisieren (eigene IP muss als String erhalten bleiben)
  myIpStr = WiFi.localIP().toString();
  initSip();

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/open", HTTP_POST, handleOpen);
  server.on("/setduration", HTTP_POST, handleSetDuration);
  server.on("/call", HTTP_POST, handleCall);
  server.on("/setdial", HTTP_POST, handleSetDial);
  server.on("/setsip", HTTP_POST, handleSetSip);
  server.begin();

  lastActivityAt = millis();
  displayDirty = true;
}

void loop() {
  server.handleClient();
  aSip.Processing(acSipIn, sizeof(acSipIn));
  handleSignalInput();

  // DTMF vom Telefon: '*' oeffnet die Tuer (loest den Summer aus)
  char dtmf = aSip.ReadDtmf();
  if (dtmf) Serial.printf("DTMF empfangen: %c\n", dtmf);
  if (dtmf == '*') startBuzzer();

  // SIP-Registrierung rechtzeitig erneuern
  if (millis() - lastRegisterAt > (uint32_t)(SIP_REG_EXPIRES - 30) * 1000UL) {
    lastRegisterAt = millis();
    aSip.Register(SIP_REG_EXPIRES);
  }

  if (buzzerActive && (int32_t)(millis() - buzzerOffAt) >= 0) {
    stopBuzzer();
  }

  // Bildschirmschoner: OLED nach Inaktivitaet ausschalten
  if (SCREEN_TIMEOUT_SECONDS > 0 && displayOn &&
      (millis() - lastActivityAt) > (uint32_t)SCREEN_TIMEOUT_SECONDS * 1000UL) {
    display.clearDisplay();
    display.display();
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    displayOn = false;
  }

  // Display regelmaessig auffrischen, damit der Klingel-Hinweis von selbst verschwindet
  static uint32_t lastDraw = 0;
  if (displayOn && millis() - lastDraw > 500) {
    lastDraw = millis();
    displayDirty = true;
  }

  if (displayOn && displayDirty) {
    displayDirty = false;
    drawDisplay();
  }
}

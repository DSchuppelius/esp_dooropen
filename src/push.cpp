// Push-Mitteilungen (ntfy / Telegram) und Telegram-Bot.
//  ESP32: Mitteilungen in eigenem Task (blockiert die Loop nicht); der Telegram-Bot
//         fragt in einem zweiten Task per Long-Polling nach Befehlen und Knoepfen.
//  ESP8266: Mitteilungen werden in der Loop gesendet, wenn weder Anruf noch Summer
//         laeuft - mit festem Zeitbudget (PUSH_TIMEOUT_MS); kein Bot (zu wenig
//         Speicher fuer dauernde TLS-Verbindungen).
#include "app.h"
#if defined(ESP32)
  #include <esp_tls.h>          // HTTPS ueber ESP-IDF (mit Zertifikatsbuendel)
  #include <esp_crt_bundle.h>
  #include <esp_random.h>
  #include <esp_timer.h>
  #include <ArduinoJson.h>
#else
  #include <WiFiClientSecure.h> // BearSSL
  #include <Schedule.h>
  #include <lwip/tcp.h>
  #include <include/ClientContext.h>   // TCP-Verbindung hart trennen (Frist fuer TLS)
#endif

volatile int lastPushCode = 0;   // HTTP-Status der letzten Mitteilung (-1 = Fehler)

#if defined(ESP32)
struct PushMsg {
  uint8_t type;
  bool    openButton;            // Telegram: Knopf "Oeffnen" anhaengen
  char    server[65];
  char    topic[65];             // ntfy-Topic bzw. Telegram-Chat-ID
  char    token[65];
  char    text[160];
};
#endif

static void copyToField(char *dst, size_t dstSize, const String &src) {
  size_t n = src.length();
  if (n > dstSize - 1) n = dstSize - 1;
  memcpy(dst, src.c_str(), n);
  dst[n] = '\0';
}

static String urlEncode(const char *s) {
  static const char hex[] = "0123456789ABCDEF";
  String r;
  for (; *s; s++) {
    uint8_t c = (uint8_t)*s;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') r += (char)c;
    else { r += '%'; r += hex[c >> 4]; r += hex[c & 15]; }
  }
  return r;
}

// "https://host:port/pfad" zerlegen
static bool splitUrl(const String &url, bool &https, String &host, uint16_t &port, String &path) {
  int p;
  if (url.startsWith("https://"))     { https = true;  p = 8; port = 443; }
  else if (url.startsWith("http://")) { https = false; p = 7; port = 80; }
  else return false;
  int slash = url.indexOf('/', p);
  if (slash < 0) slash = url.length();
  host = url.substring(p, slash);
  path = slash < (int)url.length() ? url.substring(slash) : String("/");
  int colon = host.indexOf(':');
  if (colon >= 0) {
    port = (uint16_t)host.substring(colon + 1).toInt();
    host = host.substring(0, colon);
  }
  return host.length() > 0 && port > 0;
}

// HTTP-Statuscode aus "HTTP/1.1 200 OK" lesen
static int parseStatus(const char *line) {
  return strncmp(line, "HTTP/", 5) == 0 && strchr(line, ' ') ? atoi(strchr(line, ' ') + 1) : -1;
}

// "Transfer-Encoding: chunked" aufloesen
static String unchunk(const String &b) {
  String out;
  int i = 0;
  while (i < (int)b.length()) {
    int eol = b.indexOf("\r\n", i);
    if (eol < 0) break;
    long n = strtol(b.substring(i, eol).c_str(), nullptr, 16);
    if (n <= 0) break;
    out += b.substring(eol + 2, eol + 2 + n);
    i = eol + 2 + n + 2;
  }
  return out;
}

#if !defined(ESP32)
// Verbleibende Zeit bis end (mindestens 1 ms)
static uint32_t msLeft(uint32_t end) {
  int32_t d = (int32_t)(end - millis());
  return d > 0 ? (uint32_t)d : 1;
}

// TLS-Client, der mit der vorab aufgeloesten IP verbindet und trotzdem den Namen
// fuer SNI mitschickt (connect(IPAddress) liesse SNI weg, connect(Name) loest erneut
// auf). Direkt der BearSSL-Kontext: nur bei ihm wirkt setTimeout().
class PushTls : public BearSSL::WiFiClientSecureCtx {
 public:
  int connectTo(const IPAddress &ip, uint16_t port, const char *host) {
    if (!WiFiClient::connect(ip, port)) return 0;
    return _connectSSL(host);
  }
  // TCP sofort trennen (WiFiClient::abort() wartet vorher bis 300 ms mit yield())
  void cut() { if (_client) _client->abort(); }
};

// BearSSL setzt die Frist fuer den TLS-Handshake intern fest auf 15 s (setTimeout()
// wirkt nur auf Verbindungsaufbau und Daten). Diese Funktion laeuft bei jedem
// yield()/delay() mit und trennt zum Fristende die TCP-Verbindung - der Handshake
// bricht dann sofort ab, den Rest raeumt stop() auf.
static PushTls *guardTls = nullptr;
static uint32_t guardEnd = 0;

static bool guardCheck() {
  if (!guardTls) return false;                         // fertig -> austragen
  if ((int32_t)(millis() - guardEnd) < 0) return true;
  guardTls->cut();
  guardTls = nullptr;
  return false;
}

// Anfrage senden und Statuszeile lesen (ok = Verbindung steht), dann trennen
static int exchange(WiFiClient &c, bool ok, const String &req, uint32_t end) {
  int code = -1;
  if (ok) {
    c.setTimeout(msLeft(end));
    c.print(req);
    while (!c.available() && c.connected() && (int32_t)(millis() - end) < 0) delay(10);
    char line[64] = {0};
    c.setTimeout(msLeft(end));      // Lesen nur bis zum Ende des Budgets
    size_t n = c.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = 0;
    code = parseStatus(line);
  }
  guardTls = nullptr;
  c.stop();
  return code;
}
#endif

// Kleiner HTTP(S)-Aufruf; liefert den Statuscode oder -1. resp (optional) erhaelt
// den Body (hoechstens maxResp Bytes). Blockiert bis timeoutMs (ESP8266: insgesamt).
static int httpRequest(const char *method, const String &url, const String &headers, const String &body,
                       String *resp = nullptr, uint32_t timeoutMs = PUSH_TIMEOUT_MS, size_t maxResp = 6000) {
  bool https;
  String host, path;
  uint16_t port;
  if (!splitUrl(url, https, host, port, path)) return -1;
  String req = String(method) + " " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n" + headers;
  if (body.length() || strcmp(method, "POST") == 0) req += "Content-Length: " + String(body.length()) + "\r\n";
  req += "\r\n" + body;
  String raw;
#if defined(ESP32)
  if (https) {
    esp_tls_cfg_t cfg = {};
    cfg.crt_bundle_attach = esp_crt_bundle_attach;   // Server-Zertifikat pruefen
    cfg.timeout_ms        = timeoutMs;
    esp_tls_t *tls = esp_tls_init();
    if (!tls) return -1;
    bool ok = false;
    if (esp_tls_conn_new_sync(host.c_str(), host.length(), port, &cfg, tls) == 1) {
      const char *p = req.c_str();
      size_t left = req.length();
      while (left > 0) {
        int n = esp_tls_conn_write(tls, p, left);
        if (n <= 0) break;
        p += n; left -= n;
      }
      if (left == 0) {
        ok = true;
        char buf[512];
        size_t limit = resp ? maxResp + 1024 : 200;   // ohne Body nur die Statuszeile
        while (raw.length() < limit) {
          int n = esp_tls_conn_read(tls, buf, sizeof(buf));
          if (n <= 0) break;   // 0 = Verbindung zu, <0 = Fehler/Zeitueberschreitung
          raw.concat(buf, n);
        }
      }
    }
    esp_tls_conn_destroy(tls);
    if (!ok) return -1;
  } else {
    WiFiClient c;
    if (!c.connect(host.c_str(), port, timeoutMs)) return -1;
    c.print(req);
    uint32_t t0 = millis();
    while (c.connected() && millis() - t0 < timeoutMs && raw.length() < (resp ? maxResp + 1024 : 200)) {
      while (c.available()) raw += (char)c.read();
      delay(5);
    }
    c.stop();
  }
#else
  // Zeitbudget fuer den ganzen Aufruf - die Loop steht so lange
  uint32_t end = millis() + timeoutMs;
  // Name vorab mit kurzer Frist aufloesen (connect(Name) wartet bis zu 10 s)
  IPAddress ip;
  if (!WiFi.hostByName(host.c_str(), ip, PUSH_DNS_TIMEOUT_MS)) {
    logMsg("Push: %s nicht aufloesbar - uebersprungen", host.c_str());
    return -1;
  }
  if (!https) {
    WiFiClient c;
    c.setTimeout(msLeft(end));
    return exchange(c, c.connect(ip, port), req, end);
  }
  // Speicher vorab pruefen: scheitert eine BearSSL-Reservierung, folgt abort()
  uint32_t heap = ESP.getFreeHeap(), block = ESP.getMaxFreeBlockSize();
  if (block < PUSH_TLS_MIN_BLOCK || heap < PUSH_TLS_MIN_HEAP) {
    logMsg("Push: zu wenig Speicher fuer HTTPS (frei %u, Block %u) - uebersprungen",
           (unsigned)heap, (unsigned)block);
    return -1;
  }
  PushTls tls;                    // erst hier anlegen: reserviert den BearSSL-Stack (6 KB)
  tls.setInsecure();              // BearSSL: kein Zertifikatsspeicher
  tls.setTimeout(msLeft(end));    // TCP-Verbindungsaufbau
  guardTls = &tls;                // TLS-Handshake: Frist ueber guardCheck()
  guardEnd = end;
  schedule_recurrent_function_us(guardCheck, 50000);
  return exchange(tls, tls.connectTo(ip, port, host.c_str()), req, end);
#endif
  int code = parseStatus(raw.c_str());
  if (resp) {
    int hdrEnd = raw.indexOf("\r\n\r\n");
    *resp = hdrEnd < 0 ? String() : raw.substring(hdrEnd + 4);
    String hdr = hdrEnd < 0 ? raw : raw.substring(0, hdrEnd);
    hdr.toLowerCase();
    if (hdr.indexOf("transfer-encoding: chunked") >= 0) *resp = unchunk(*resp);
  }
  return code;
}

// ------------------------------------------------------------
//  Telegram
// ------------------------------------------------------------
static String tgUrl(const char *token, const char *method) {
  return String("https://api.telegram.org/bot") + token + "/" + method;
}

#if defined(ESP32)
// Zufaellige Kennung dieses Starts (gesetzt, sobald das Netz steht - dann liefert
// esp_random() echte Zufallszahlen). Steckt in jedem Knopf: nach einem Neustart
// beginnt die Laufzeit wieder bei 0, alte Knoepfe passen dann aber nicht mehr.
static volatile uint32_t bootId = 0;

// Inline-Knopf "Oeffnen" (gueltig TELEGRAM_BUTTON_SEC ab jetzt, nur in diesem Start)
static String openKeyboard(const char *label) {
  char data[32];
  snprintf(data, sizeof(data), "open:%08lx:%lu", (unsigned long)bootId, (unsigned long)uptimeSeconds());
  return String("{\"inline_keyboard\":[[{\"text\":\"") + label + "\",\"callback_data\":\"" + data + "\"}]]}";
}
#endif

static int tgSend(const char *token, const String &chat, const String &text, const String &keyboard = String()) {
  String body = "chat_id=" + urlEncode(chat.c_str()) + "&text=" + urlEncode(text.c_str());
  if (keyboard.length()) body += "&reply_markup=" + urlEncode(keyboard.c_str());
  return httpRequest("POST", tgUrl(token, "sendMessage"), "Content-Type: application/x-www-form-urlencoded\r\n", body);
}

// Mitteilung senden (ESP32: im Push-Task, ESP8266: in der Loop)
static void sendPushNow(uint8_t type, const char *server, const char *topic, const char *token,
                        const char *text, bool openButton) {
  if (!netUp()) { lastPushCode = -1; return; }
  if (type == PUSH_NTFY) {
    String url = server;
    if (!url.endsWith("/")) url += "/";
    url += topic;
    String headers = "Content-Type: text/plain; charset=utf-8\r\nTitle: Tueroeffner\r\nTags: bell\r\n";
    if (token[0]) headers += String("Authorization: Bearer ") + token + "\r\n";
    lastPushCode = httpRequest("POST", url, headers, text);
  } else {
    String kb;
#if defined(ESP32)
    if (openButton) kb = openKeyboard("🔓 Öffnen");
#else
    (void)openButton;
#endif
    lastPushCode = tgSend(token, topic, text, kb);
  }
  Serial.printf("Push: HTTP %d\n", lastPushCode);
}

#if defined(ESP32)
static QueueHandle_t pushQueue = nullptr;

static void pushTask(void *) {
  PushMsg m;
  for (;;) {
    if (xQueueReceive(pushQueue, &m, portMAX_DELAY) == pdTRUE)
      sendPushNow(m.type, m.server, m.topic, m.token, m.text, m.openButton);
  }
}

// Einstellungen fuer den Bot-Task. Die Strings der Einstellungen weist die
// Weboberflaeche beim Speichern neu zu - ein anderer Task darf sie nicht lesen.
// Die Loop kopiert sie jede Sekunde (pushLoop), der Task holt die Kopie unter tgMutex.
struct TgConfig {
  bool active;                                 // Push = Telegram mit Token und Chat
  bool open;                                   // Oeffnen per Knopf erlaubt
  char token[65];
  char chat[65];                               // Haupt-Chat (Push)
  char chats[101];                             // weitere erlaubte Chat-IDs
};
static SemaphoreHandle_t tgMutex = nullptr;    // schuetzt tgCfg, tgStatus und tgOpenWho
static TgConfig          tgCfg   = {};

// Oeffnen-Anforderung vom Bot an die Loop (Tuer nur aus der Loop schalten)
static volatile bool tgOpenReq = false;
static char          tgOpenWho[40];

// Statustext baut die Loop (liest viele Einstellungen); der Bot fragt per tgStatusReq an
static volatile bool tgStatusReq = false;
static char          tgStatus[320];

bool telegramActive() {
  return pushType == PUSH_TELEGRAM && pushToken.length() && pushTopic.length();
}

// Darf dieser Chat den Bot benutzen? Haupt-Chat (Push) oder weitere Chats
static bool chatAllowed(const TgConfig &cfg, const String &chat) {
  if (chat == cfg.chat) return true;
  String list = cfg.chats, nr;
  for (uint8_t i = 0; dialTarget(list, i, nr); i++)
    if (nr == chat) return true;
  return false;
}

// Nur in der Loop aufrufen
static String statusText() {
  String t = "🚪 Türöffner\n";
  t += "SIP: "; t += sipStateText();
  t += "\nKlingeln: " + String(signalCount) + ", Öffnungen: " + String(buzzerTriggers);
  if (doorOn) t += String("\nTür: ") + (doorOpen ? "offen" : "zu");
  if (isPraxis()) t += "\nPraxis-Modus aktiv";
  else if (isQuiet()) t += "\nNachtruhe aktiv";
  t += "\nZuletzt: " + lastEventText();
  return t;
}

// Im Bot-Task: Statustext bei der Loop anfordern und abholen
static String loopStatusText() {
  tgStatusReq = true;
  for (int i = 0; i < 100 && tgStatusReq; i++) vTaskDelay(pdMS_TO_TICKS(50));
  if (tgStatusReq) return "Status gerade nicht verfügbar – bitte später erneut";
  xSemaphoreTake(tgMutex, portMAX_DELAY);
  String t = tgStatus;
  xSemaphoreGive(tgMutex);
  return t;
}

// Knopf "open:<Start-Kennung>:<Laufzeit>": aus diesem Start und juenger als
// TELEGRAM_BUTTON_SEC. Mit gestellter Uhr zusaetzlich das Alter der Mitteilung
// pruefen (date = Sendezeit laut Telegram, 0 = Mitteilung nicht mehr zugaenglich).
static bool buttonValid(const String &data, uint32_t date) {
  char *p;
  uint32_t id = strtoul(data.c_str() + 5, &p, 16);
  if (!bootId || id != bootId || *p != ':') return false;
  uint32_t ts = strtoul(p + 1, nullptr, 10), now = uptimeSeconds();
  if (ts > now || now - ts > TELEGRAM_BUTTON_SEC) return false;
  if (timeValid() && (date == 0 || (int32_t)((uint32_t)time(nullptr) - date) > TELEGRAM_BUTTON_SEC)) return false;
  return true;
}

static void tgHandle(const TgConfig &cfg, JsonObject upd) {
  const char *token = cfg.token;
  if (upd["callback_query"].is<JsonObject>()) {
    JsonObject cq   = upd["callback_query"];
    String     chat = cq["message"]["chat"]["id"].as<String>();
    String     data = cq["data"] | "";
    String     who  = cq["from"]["first_name"] | "Telegram";
    String     answer;
    if (!chatAllowed(cfg, chat)) {
      answer = "Nicht berechtigt";
    } else if (!data.startsWith("open:")) {
      answer = "?";
    } else if (!cfg.open) {
      answer = "Öffnen per Telegram ist ausgeschaltet";
    } else if (!buttonValid(data, cq["message"]["date"].as<uint32_t>())) {
      answer = "Knopf abgelaufen – bitte /oeffnen senden";
    } else {
      xSemaphoreTake(tgMutex, portMAX_DELAY);
      copyToField(tgOpenWho, sizeof(tgOpenWho), who);
      xSemaphoreGive(tgMutex);
      tgOpenReq = true;
      answer = "Tür wird geöffnet";
    }
    String body = "callback_query_id=" + urlEncode(cq["id"].as<String>().c_str()) +
                  "&text=" + urlEncode(answer.c_str());
    httpRequest("POST", tgUrl(token, "answerCallbackQuery"), "Content-Type: application/x-www-form-urlencoded\r\n", body);
    return;
  }
  if (!upd["message"].is<JsonObject>()) return;
  JsonObject msg  = upd["message"];
  String     chat = msg["chat"]["id"].as<String>();
  String     text = msg["text"] | "";
  text.trim();
  text.toLowerCase();
  if (!chatAllowed(cfg, chat)) {
    tgSend(token, chat, "Dieser Chat ist nicht freigegeben. Chat-ID: " + chat);
    logMsg("Telegram: unbekannter Chat %s", chat.c_str());
    return;
  }
  int at = text.indexOf('@');   // "/status@MeinBot"
  if (at > 0) text = text.substring(0, at);
  if (text == "/status") {
    tgSend(token, chat, loopStatusText());
  } else if (text == "/oeffnen" || text == "/öffnen" || text == "/open" || text == "öffnen" || text == "oeffnen") {
    if (!cfg.open) tgSend(token, chat, "Öffnen per Telegram ist in der Weboberfläche ausgeschaltet.");
    else           tgSend(token, chat, "Tür öffnen?", openKeyboard("🔓 Jetzt öffnen"));
  } else {
    tgSend(token, chat, String("Befehle:\n/status – Zustand\n/oeffnen – Tür öffnen") +
                        (cfg.open ? "" : " (ausgeschaltet)") + "\nChat-ID: " + chat);
  }
}

// Hoechste update_id in einer (evtl. abgeschnittenen) Antwort, -1 = keine
static long maxUpdateId(const String &s) {
  long best = -1;
  for (int i = s.indexOf("\"update_id\":"); i >= 0; i = s.indexOf("\"update_id\":", i + 12)) {
    long id = strtol(s.c_str() + i + 12, nullptr, 10);
    if (id > best) best = id;
  }
  return best;
}

// Long-Polling: getUpdates wartet bis zu 25 s auf neue Nachrichten
static void telegramTask(void *) {
  TgConfig cfg;
  char     botToken[65] = "";   // Bot, zu dem offset gehoert
  long     offset = 0;
  bool     first  = true;
  for (;;) {
    xSemaphoreTake(tgMutex, portMAX_DELAY);
    cfg = tgCfg;
    xSemaphoreGive(tgMutex);
    if (!cfg.active || !netUp()) { vTaskDelay(pdMS_TO_TICKS(5000)); continue; }
    // Anderer Bot: dessen Nummern passen nicht zu offset -> von vorn
    if (strcmp(cfg.token, botToken) != 0) {
      memcpy(botToken, cfg.token, sizeof(botToken));
      offset = 0;
      first  = true;
    }
    // Erster Abruf (Start, neuer Bot): offset=-1 liefert nur die juengste Nachricht und
    // verwirft alle aelteren; auch sie wird nicht ausgefuehrt (sonst wirkten alte Befehle)
    String url = tgUrl(cfg.token, "getUpdates") + "?timeout=" + String(first ? 0 : 25) +
                 "&limit=5&offset=" + String(first ? -1L : offset) +
                 "&allowed_updates=%5B%22message%22%2C%22callback_query%22%5D";
    String resp;
    int code = httpRequest("GET", url, "", "", &resp, 30000, 8000);
    if (code != 200) { vTaskDelay(pdMS_TO_TICKS(code == 401 || code == 404 ? 60000 : 5000)); continue; }
    JsonDocument doc;
    if (deserializeJson(doc, resp)) {
      // Abgeschnitten (sehr lange Nachricht) oder kaputt: trotzdem weiterschalten,
      // sonst kaeme dieselbe Antwort immer wieder und der Bot waere taub
      long last = maxUpdateId(resp);
      if (last >= offset) {
        logMsg("Telegram: Antwort nicht lesbar - Nachrichten bis %ld uebersprungen", last);
        offset = last + 1;
        first  = false;
      }
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }
    for (JsonObject upd : doc["result"].as<JsonArray>()) {
      offset = upd["update_id"].as<long>() + 1;
      if (!first) tgHandle(cfg, upd);
    }
    first = false;
  }
}
#else
// Wartende Mitteilungen (nur der Text; das Ziel wird beim Senden gelesen)
static char    pushTexts[PUSH_QUEUE_LEN][160];
static uint8_t pushHead  = 0;
static uint8_t pushCount = 0;

bool telegramActive() { return false; }
#endif

void pushBegin() {
#if defined(ESP32)
  // Push-Mitteilungen in eigenem Task (TLS blockiert sonst die Loop)
  tgMutex   = xSemaphoreCreateMutex();
  pushQueue = xQueueCreate(4, sizeof(PushMsg));
  xTaskCreatePinnedToCore(pushTask, "push", 12288, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(telegramTask, "telegram", 12288, nullptr, 1, nullptr, 0);
#endif
}

// Mitteilung einreihen; ev = PUSH_EV_* (0 = immer, z.B. Test oder Sicherheitshinweis)
void queuePush(uint8_t ev, const String &text, bool openButton) {
  if (pushType == PUSH_OFF || pushTopic.length() == 0) return;
  if (ev && !(pushEvents & ev)) return;
#if defined(ESP32)
  PushMsg m;
  m.type       = pushType;
  m.openButton = openButton && tgOpen;   // hier lesen: der Push-Task liest keine Einstellungen
  copyToField(m.server, sizeof(m.server), pushServer);
  copyToField(m.topic,  sizeof(m.topic),  pushTopic);
  copyToField(m.token,  sizeof(m.token),  pushToken);
  copyToField(m.text,   sizeof(m.text),   text);
  if (pushQueue && xQueueSend(pushQueue, &m, 0) != pdTRUE)
    logMsg("Push: Warteschlange voll - Mitteilung verworfen");
#else
  (void)openButton;
  if (pushCount == PUSH_QUEUE_LEN) {   // voll -> aelteste verwerfen
    logMsg("Push: Warteschlange voll - aelteste Mitteilung verworfen");
    pushHead = (pushHead + 1) % PUSH_QUEUE_LEN;
    pushCount--;
  }
  copyToField(pushTexts[(pushHead + pushCount) % PUSH_QUEUE_LEN], sizeof(pushTexts[0]), text);
  pushCount++;
#endif
}

// ESP8266: wartende Mitteilung senden - nicht waehrend eines Anrufs und nicht,
// solange der Summer laeuft (das Senden blockiert bis PUSH_TIMEOUT_MS).
// ESP32: Anfragen des Telegram-Bots ausfuehren (Oeffnen, Statustext) und ihm die
// Einstellungen kopieren.
void pushLoop() {
#if defined(ESP32)
  if (!bootId && netUp()) bootId = (esp_random() ^ (uint32_t)esp_timer_get_time()) | 1;
  if (tgOpenReq) {
    xSemaphoreTake(tgMutex, portMAX_DELAY);
    String who = tgOpenWho;
    xSemaphoreGive(tgMutex);
    tgOpenReq = false;
    startBuzzer(EV_OPEN_TELEGRAM, who);
    aSip.Hangup();
  }
  if (tgStatusReq) {
    String t = statusText();
    xSemaphoreTake(tgMutex, portMAX_DELAY);
    copyToField(tgStatus, sizeof(tgStatus), t);
    xSemaphoreGive(tgMutex);
    tgStatusReq = false;
  }
  static uint32_t lastCfg = 0;
  if (millis() - lastCfg >= 1000) {
    lastCfg = millis();
    TgConfig c;
    c.active = telegramActive();
    c.open   = tgOpen;
    copyToField(c.token, sizeof(c.token), pushToken);
    copyToField(c.chat,  sizeof(c.chat),  pushTopic);
    copyToField(c.chats, sizeof(c.chats), tgChats);
    xSemaphoreTake(tgMutex, portMAX_DELAY);
    tgCfg = c;
    xSemaphoreGive(tgMutex);
  }
#else
  // Auch nicht in der Pause zwischen zwei Nummern der Rufkette (verzoegerte den naechsten Anruf)
  if (pushCount && !aSip.IsBusy() && !buzzerActive && chainIdx < 0) {
    // Ziel erst jetzt lesen (gleicher Ablauf wie die Weboberflaeche - kein Datenrennen)
    if (pushType != PUSH_OFF && pushTopic.length())
      sendPushNow(pushType, pushServer.c_str(), pushTopic.c_str(), pushToken.c_str(), pushTexts[pushHead], false);
    pushHead = (pushHead + 1) % PUSH_QUEUE_LEN;
    pushCount--;
  }
#endif
}

// Push-Mitteilungen (ntfy / Telegram) und Telegram-Bot.
//  ESP32: Mitteilungen in eigenem Task (blockiert die Loop nicht); der Telegram-Bot
//         fragt in einem zweiten Task per Long-Polling nach Befehlen und Knoepfen.
//  ESP8266: Mitteilungen werden in der Loop gesendet, wenn weder Anruf noch Summer
//         laeuft; kein Bot (zu wenig Speicher fuer dauernde TLS-Verbindungen).
#include "app.h"
#if defined(ESP32)
  #include <esp_tls.h>          // HTTPS ueber ESP-IDF (mit Zertifikatsbuendel)
  #include <esp_crt_bundle.h>
  #include <ArduinoJson.h>
#else
  #include <WiFiClientSecure.h> // BearSSL
#endif

volatile int lastPushCode = 0;   // HTTP-Status der letzten Mitteilung (-1 = Fehler)

struct PushMsg {
  uint8_t type;
  bool    openButton;            // Telegram: Knopf "Oeffnen" anhaengen
  char    server[65];
  char    topic[65];             // ntfy-Topic bzw. Telegram-Chat-ID
  char    token[65];
  char    text[160];
};

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

// Kleiner HTTP(S)-Aufruf; liefert den Statuscode oder -1. resp (optional) erhaelt
// den Body (hoechstens maxResp Bytes). Blockiert bis timeoutMs.
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
  BearSSL::WiFiClientSecure tls;
  WiFiClient plain;
  WiFiClient *cp = &plain;
  if (https) { tls.setInsecure(); cp = &tls; }   // BearSSL: kein Zertifikatsspeicher
  WiFiClient &c = *cp;
  c.setTimeout(timeoutMs);
  if (!c.connect(host.c_str(), port)) return -1;
  c.print(req);
  uint32_t t0 = millis();
  while (!c.available() && c.connected() && millis() - t0 < timeoutMs) delay(10);
  char line[64] = {0};
  size_t n = c.readBytesUntil('\n', line, sizeof(line) - 1);
  line[n] = 0;
  c.stop();
  return parseStatus(line);
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
// Inline-Knopf "Oeffnen" (gueltig TELEGRAM_BUTTON_SEC ab jetzt)
static String openKeyboard(const char *label) {
  return String("{\"inline_keyboard\":[[{\"text\":\"") + label +
         "\",\"callback_data\":\"open:" + String(uptimeSeconds()) + "\"}]]}";
}
#endif

static int tgSend(const char *token, const String &chat, const String &text, const String &keyboard = String()) {
  String body = "chat_id=" + urlEncode(chat.c_str()) + "&text=" + urlEncode(text.c_str());
  if (keyboard.length()) body += "&reply_markup=" + urlEncode(keyboard.c_str());
  return httpRequest("POST", tgUrl(token, "sendMessage"), "Content-Type: application/x-www-form-urlencoded\r\n", body);
}

static void sendPushNow(const PushMsg &m) {
  if (!netUp()) { lastPushCode = -1; return; }
  if (m.type == PUSH_NTFY) {
    String url = m.server;
    if (!url.endsWith("/")) url += "/";
    url += m.topic;
    String headers = "Content-Type: text/plain; charset=utf-8\r\nTitle: Tueroeffner\r\nTags: bell\r\n";
    if (m.token[0]) headers += String("Authorization: Bearer ") + m.token + "\r\n";
    lastPushCode = httpRequest("POST", url, headers, m.text);
  } else {
    String kb;
#if defined(ESP32)
    if (m.openButton && tgOpen) kb = openKeyboard("🔓 Öffnen");
#endif
    lastPushCode = tgSend(m.token, m.topic, m.text, kb);
  }
  Serial.printf("Push: HTTP %d\n", lastPushCode);
}

#if defined(ESP32)
static QueueHandle_t pushQueue = nullptr;

static void pushTask(void *) {
  PushMsg m;
  for (;;) {
    if (xQueueReceive(pushQueue, &m, portMAX_DELAY) == pdTRUE) sendPushNow(m);
  }
}

// Oeffnen-Anforderung vom Bot an die Loop (Tuer nur aus der Loop schalten)
static volatile bool tgOpenReq = false;
static char          tgOpenWho[40];

bool telegramActive() {
  return pushType == PUSH_TELEGRAM && pushToken.length() && pushTopic.length();
}

// Darf dieser Chat den Bot benutzen? Haupt-Chat (Push) oder in tgChats
static bool chatAllowed(const String &chat) {
  if (chat == pushTopic) return true;
  String nr;
  for (uint8_t i = 0; dialTarget(tgChats, i, nr); i++)
    if (nr == chat) return true;
  return false;
}

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

static void tgHandle(const char *token, JsonObject upd) {
  if (upd["callback_query"].is<JsonObject>()) {
    JsonObject cq   = upd["callback_query"];
    String     chat = cq["message"]["chat"]["id"].as<String>();
    String     data = cq["data"] | "";
    String     who  = cq["from"]["first_name"] | "Telegram";
    String     answer;
    if (!chatAllowed(chat)) {
      answer = "Nicht berechtigt";
    } else if (!data.startsWith("open:")) {
      answer = "?";
    } else if (!tgOpen) {
      answer = "Öffnen per Telegram ist ausgeschaltet";
    } else {
      uint32_t ts = strtoul(data.c_str() + 5, nullptr, 10), now = uptimeSeconds();
      if (ts > now || now - ts > TELEGRAM_BUTTON_SEC) {
        answer = "Knopf abgelaufen – bitte /oeffnen senden";
      } else {
        copyToField(tgOpenWho, sizeof(tgOpenWho), who);
        tgOpenReq = true;
        answer = "Tür wird geöffnet";
      }
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
  if (!chatAllowed(chat)) {
    tgSend(token, chat, "Dieser Chat ist nicht freigegeben. Chat-ID: " + chat);
    logMsg("Telegram: unbekannter Chat %s", chat.c_str());
    return;
  }
  int at = text.indexOf('@');   // "/status@MeinBot"
  if (at > 0) text = text.substring(0, at);
  if (text == "/status") {
    tgSend(token, chat, statusText());
  } else if (text == "/oeffnen" || text == "/öffnen" || text == "/open" || text == "öffnen" || text == "oeffnen") {
    if (!tgOpen) tgSend(token, chat, "Öffnen per Telegram ist in der Weboberfläche ausgeschaltet.");
    else         tgSend(token, chat, "Tür öffnen?", openKeyboard("🔓 Jetzt öffnen"));
  } else {
    tgSend(token, chat, String("Befehle:\n/status – Zustand\n/oeffnen – Tür öffnen") +
                        (tgOpen ? "" : " (ausgeschaltet)") + "\nChat-ID: " + chat);
  }
}

// Long-Polling: getUpdates wartet bis zu 25 s auf neue Nachrichten
static void telegramTask(void *) {
  long offset = 0;
  bool first  = true;
  for (;;) {
    if (!telegramActive() || !netUp()) { vTaskDelay(pdMS_TO_TICKS(5000)); continue; }
    char token[65];
    copyToField(token, sizeof(token), pushToken);
    // Beim Start alte Nachrichten verwerfen (sonst wuerden z.B. alte Knoepfe wirken)
    String url = tgUrl(token, "getUpdates") + "?timeout=" + String(first ? 0 : 25) +
                 "&offset=" + String(offset) + "&allowed_updates=%5B%22message%22%2C%22callback_query%22%5D";
    String resp;
    int code = httpRequest("GET", url, "", "", &resp, 30000, 8000);
    if (code != 200) { vTaskDelay(pdMS_TO_TICKS(code == 401 || code == 404 ? 60000 : 5000)); continue; }
    JsonDocument doc;
    if (deserializeJson(doc, resp)) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
    for (JsonObject upd : doc["result"].as<JsonArray>()) {
      offset = upd["update_id"].as<long>() + 1;
      if (!first) tgHandle(token, upd);
    }
    first = false;
  }
}
#else
static PushMsg pendingPush;
static bool    pendingPushDue = false;

bool telegramActive() { return false; }
#endif

void pushBegin() {
#if defined(ESP32)
  // Push-Mitteilungen in eigenem Task (TLS blockiert sonst die Loop)
  pushQueue = xQueueCreate(4, sizeof(PushMsg));
  xTaskCreatePinnedToCore(pushTask, "push", 12288, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(telegramTask, "telegram", 12288, nullptr, 1, nullptr, 0);
#endif
}

// Mitteilung einreihen; ev = PUSH_EV_* (0 = immer, z.B. Test oder Sicherheitshinweis)
void queuePush(uint8_t ev, const String &text, bool openButton) {
  if (pushType == PUSH_OFF || pushTopic.length() == 0) return;
  if (ev && !(pushEvents & ev)) return;
  PushMsg m;
  m.type       = pushType;
  m.openButton = openButton;
  copyToField(m.server, sizeof(m.server), pushServer);
  copyToField(m.topic,  sizeof(m.topic),  pushTopic);
  copyToField(m.token,  sizeof(m.token),  pushToken);
  copyToField(m.text,   sizeof(m.text),   text);
#if defined(ESP32)
  if (pushQueue) xQueueSend(pushQueue, &m, 0);
#else
  pendingPush    = m;
  pendingPushDue = true;
#endif
}

// ESP8266: wartende Mitteilung senden - nicht waehrend eines Anrufs und nicht,
// solange der Summer laeuft (das Senden blockiert einige Sekunden).
// ESP32: Oeffnen-Anforderung des Telegram-Bots ausfuehren.
void pushLoop() {
#if defined(ESP32)
  if (tgOpenReq) {
    tgOpenReq = false;
    startBuzzer(EV_OPEN_TELEGRAM, tgOpenWho);
    aSip.Hangup();
  }
#else
  if (pendingPushDue && !aSip.IsBusy() && !buzzerActive) {
    pendingPushDue = false;
    sendPushNow(pendingPush);
  }
#endif
}

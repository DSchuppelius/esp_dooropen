// Push-Mitteilungen (ntfy / Telegram).
//  ESP32: eigener Task, blockiert die Loop nicht.
//  ESP8266: wird in der Loop gesendet, wenn weder Anruf noch Summer laeuft.
#include "app.h"
#if defined(ESP32)
  #include <esp_tls.h>          // HTTPS ueber ESP-IDF (mit Zertifikatsbuendel)
  #include <esp_crt_bundle.h>
#else
  #include <WiFiClientSecure.h> // BearSSL
#endif

volatile int lastPushCode = 0;   // HTTP-Status der letzten Mitteilung (-1 = Fehler)

struct PushMsg {
  uint8_t type;
  char    server[65];
  char    topic[65];
  char    token[65];
  char    text[120];
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

// Kleiner HTTP(S)-POST; liefert den Statuscode oder -1. Blockiert bis PUSH_TIMEOUT_MS.
static int httpPost(const String &url, const String &headers, const String &body) {
  bool https;
  String host, path;
  uint16_t port;
  if (!splitUrl(url, https, host, port, path)) return -1;
  String req = "POST " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n" + headers +
               "Content-Length: " + String(body.length()) + "\r\n\r\n" + body;
  char line[64] = {0};
#if defined(ESP32)
  if (https) {
    esp_tls_cfg_t cfg = {};
    cfg.crt_bundle_attach = esp_crt_bundle_attach;   // Server-Zertifikat pruefen
    cfg.timeout_ms        = PUSH_TIMEOUT_MS;
    esp_tls_t *tls = esp_tls_init();
    if (!tls) return -1;
    int code = -1;
    if (esp_tls_conn_new_sync(host.c_str(), host.length(), port, &cfg, tls) == 1) {
      const char *p = req.c_str();
      size_t left = req.length();
      while (left > 0) {
        int n = esp_tls_conn_write(tls, p, left);
        if (n <= 0) break;
        p += n; left -= n;
      }
      if (left == 0) {
        int n = esp_tls_conn_read(tls, line, sizeof(line) - 1);
        if (n > 0) { line[n] = 0; code = parseStatus(line); }
      }
    }
    esp_tls_conn_destroy(tls);
    return code;
  }
  WiFiClient c;
  if (!c.connect(host.c_str(), port, PUSH_TIMEOUT_MS)) return -1;
#else
  BearSSL::WiFiClientSecure tls;
  WiFiClient plain;
  WiFiClient *cp = &plain;
  if (https) { tls.setInsecure(); cp = &tls; }   // BearSSL: kein Zertifikatsspeicher
  WiFiClient &c = *cp;
  c.setTimeout(PUSH_TIMEOUT_MS);
  if (!c.connect(host.c_str(), port)) return -1;
#endif
  c.print(req);
  uint32_t t0 = millis();
  while (!c.available() && c.connected() && millis() - t0 < PUSH_TIMEOUT_MS) delay(10);
  size_t n = c.readBytesUntil('\n', line, sizeof(line) - 1);
  line[n] = 0;
  c.stop();
  return parseStatus(line);
}

static void sendPushNow(const PushMsg &m) {
  if (WiFi.status() != WL_CONNECTED) { lastPushCode = -1; return; }
  String url, headers, body;
  if (m.type == PUSH_NTFY) {
    url = m.server;
    if (!url.endsWith("/")) url += "/";
    url += m.topic;
    headers = "Content-Type: text/plain; charset=utf-8\r\nTitle: Tueroeffner\r\nTags: bell\r\n";
    if (m.token[0]) headers += String("Authorization: Bearer ") + m.token + "\r\n";
    body = m.text;
  } else {
    url     = String("https://api.telegram.org/bot") + m.token + "/sendMessage";
    headers = "Content-Type: application/x-www-form-urlencoded\r\n";
    body    = "chat_id=" + urlEncode(m.topic) + "&text=" + urlEncode(m.text);
  }
  lastPushCode = httpPost(url, headers, body);
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
#else
static PushMsg pendingPush;
static bool    pendingPushDue = false;
#endif

void pushBegin() {
#if defined(ESP32)
  // Push-Mitteilungen in eigenem Task (TLS blockiert sonst die Loop)
  pushQueue = xQueueCreate(4, sizeof(PushMsg));
  xTaskCreatePinnedToCore(pushTask, "push", 12288, nullptr, 1, nullptr, 0);
#endif
}

// Mitteilung einreihen; ev = PUSH_EV_* (0 = immer, z.B. Test oder Sicherheitshinweis)
void queuePush(uint8_t ev, const String &text) {
  if (pushType == PUSH_OFF || pushTopic.length() == 0) return;
  if (ev && !(pushEvents & ev)) return;
  PushMsg m;
  m.type = pushType;
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
// solange der Summer laeuft (das Senden blockiert einige Sekunden)
void pushLoop() {
#if !defined(ESP32)
  if (pendingPushDue && !aSip.IsBusy() && !buzzerActive) {
    pendingPushDue = false;
    sendPushNow(pendingPush);
  }
#endif
}

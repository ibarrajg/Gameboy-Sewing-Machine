#include "link_radio.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <string.h>

static const uint8_t LINK_PKT_PING = 1;
static const uint8_t LINK_PKT_PONG = 2;
static const uint32_t PING_PERIOD_US = 1000; // ~1 kHz offered load
static const size_t RTT_SLOTS = 64;

struct __attribute__((packed)) LinkProofPacket {
  uint8_t type;
  uint32_t seq;
};

struct RttSlot {
  uint32_t seq;
  int64_t send_us;
  bool used;
};

static bool s_ready = false;
static bool s_running = false;
static LinkRole s_role = LinkRole::Idle;
static uint8_t s_peer[6] = {};
static bool s_peer_set = false;
static uint32_t s_seq = 0;
static int64_t s_last_ping_us = 0;
static bool s_send_gpio = false;
static bool s_recv_gpio = false;
static RttSlot s_rtt[RTT_SLOTS] = {};

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_sent = 0;
static volatile uint32_t s_pong = 0;
static volatile uint32_t s_send_fail = 0;
static volatile uint32_t s_echo_recv = 0;
static volatile uint32_t s_echo_sent = 0;
static volatile uint32_t s_ignored = 0;
static volatile int64_t s_rtt_last = 0;
static volatile int64_t s_rtt_min = 0;
static volatile int64_t s_rtt_max = 0;
static volatile uint64_t s_rtt_sum = 0;

static void formatMac(const uint8_t mac[6], char out[18]) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
}

static bool parseMac(const String &in, uint8_t out[6]) {
  uint8_t bytes[6] = {};
  int n = 0;
  int acc = -1;
  for (size_t i = 0; i < in.length() && n < 6; i++) {
    char c = in[i];
    int v = -1;
    if (c >= '0' && c <= '9')
      v = c - '0';
    else if (c >= 'a' && c <= 'f')
      v = 10 + (c - 'a');
    else if (c >= 'A' && c <= 'F')
      v = 10 + (c - 'A');
    else
      continue;
    if (acc < 0) {
      acc = v;
    } else {
      bytes[n++] = static_cast<uint8_t>((acc << 4) | v);
      acc = -1;
    }
  }
  if (n != 6) {
    return false;
  }
  memcpy(out, bytes, 6);
  return true;
}

static void gpioToggleSend() {
  s_send_gpio = !s_send_gpio;
  digitalWrite(LINK_GPIO_SEND, s_send_gpio ? HIGH : LOW);
}

static void gpioToggleRecv() {
  s_recv_gpio = !s_recv_gpio;
  digitalWrite(LINK_GPIO_RECV, s_recv_gpio ? HIGH : LOW);
}

static bool macEqual(const uint8_t *a, const uint8_t *b) {
  return memcmp(a, b, 6) == 0;
}

static bool applyPeer(const uint8_t mac[6]) {
  if (!s_ready) {
    return false;
  }
  if (s_peer_set) {
    esp_now_del_peer(s_peer);
    s_peer_set = false;
  }
  if (macEqual(mac, (const uint8_t *)"\x00\x00\x00\x00\x00\x00") ||
      macEqual(mac, (const uint8_t *)"\xFF\xFF\xFF\xFF\xFF\xFF")) {
    return false;
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = LINK_WIFI_CHANNEL;
  peer.encrypt = false;
  peer.ifidx = WIFI_IF_AP;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    return false;
  }
  memcpy(s_peer, mac, 6);
  s_peer_set = true;
  return true;
}

static void resetStats() {
  portENTER_CRITICAL(&s_mux);
  s_sent = 0;
  s_pong = 0;
  s_send_fail = 0;
  s_echo_recv = 0;
  s_echo_sent = 0;
  s_ignored = 0;
  s_rtt_last = 0;
  s_rtt_min = 0;
  s_rtt_max = 0;
  s_rtt_sum = 0;
  s_seq = 0;
  s_last_ping_us = 0;
  for (size_t i = 0; i < RTT_SLOTS; i++) {
    s_rtt[i].used = false;
  }
  portEXIT_CRITICAL(&s_mux);
}

static void noteRtt(int64_t rtt_us) {
  if (rtt_us < 0) {
    return;
  }
  s_rtt_last = rtt_us;
  if (s_rtt_min == 0 || rtt_us < s_rtt_min) {
    s_rtt_min = rtt_us;
  }
  if (rtt_us > s_rtt_max) {
    s_rtt_max = rtt_us;
  }
  s_rtt_sum += static_cast<uint64_t>(rtt_us);
}

static void handleRecv(const uint8_t *mac, const uint8_t *data, int len) {
  gpioToggleRecv();

  if (!s_ready || !s_peer_set || !mac || len != static_cast<int>(sizeof(LinkProofPacket))) {
    portENTER_CRITICAL(&s_mux);
    s_ignored++;
    portEXIT_CRITICAL(&s_mux);
    return;
  }
  if (!macEqual(mac, s_peer)) {
    portENTER_CRITICAL(&s_mux);
    s_ignored++;
    portEXIT_CRITICAL(&s_mux);
    return;
  }

  LinkProofPacket pkt;
  memcpy(&pkt, data, sizeof(pkt));

  if (pkt.type == LINK_PKT_PING && s_role == LinkRole::Echo && s_running) {
    portENTER_CRITICAL(&s_mux);
    s_echo_recv++;
    portEXIT_CRITICAL(&s_mux);
    pkt.type = LINK_PKT_PONG;
    gpioToggleSend();
    if (esp_now_send(s_peer, reinterpret_cast<uint8_t *>(&pkt), sizeof(pkt)) ==
        ESP_OK) {
      portENTER_CRITICAL(&s_mux);
      s_echo_sent++;
      portEXIT_CRITICAL(&s_mux);
    } else {
      portENTER_CRITICAL(&s_mux);
      s_send_fail++;
      portEXIT_CRITICAL(&s_mux);
    }
    return;
  }

  if (pkt.type == LINK_PKT_PONG && s_role == LinkRole::Ping) {
    const int64_t now = esp_timer_get_time();
    RttSlot &slot = s_rtt[pkt.seq % RTT_SLOTS];
    portENTER_CRITICAL(&s_mux);
    if (slot.used && slot.seq == pkt.seq) {
      noteRtt(now - slot.send_us);
      slot.used = false;
      s_pong++;
    }
    portEXIT_CRITICAL(&s_mux);
  }
}

#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 5
static void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  handleRecv(info ? info->src_addr : nullptr, data, len);
}
#else
static void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
  handleRecv(mac, data, len);
}
#endif

static void onSend(const uint8_t *mac, esp_now_send_status_t status) {
  (void)mac;
  if (status != ESP_NOW_SEND_SUCCESS) {
    portENTER_CRITICAL(&s_mux);
    s_send_fail++;
    portEXIT_CRITICAL(&s_mux);
  }
}

static void sendPing() {
  if (!s_ready || !s_running || !s_peer_set || s_role != LinkRole::Ping) {
    return;
  }
  LinkProofPacket pkt;
  pkt.type = LINK_PKT_PING;
  pkt.seq = ++s_seq;
  const int64_t now = esp_timer_get_time();
  RttSlot &slot = s_rtt[pkt.seq % RTT_SLOTS];
  portENTER_CRITICAL(&s_mux);
  slot.seq = pkt.seq;
  slot.send_us = now;
  slot.used = true;
  portEXIT_CRITICAL(&s_mux);

  gpioToggleSend();
  if (esp_now_send(s_peer, reinterpret_cast<uint8_t *>(&pkt), sizeof(pkt)) !=
      ESP_OK) {
    portENTER_CRITICAL(&s_mux);
    slot.used = false;
    s_send_fail++;
    portEXIT_CRITICAL(&s_mux);
    return;
  }
  portENTER_CRITICAL(&s_mux);
  s_sent++;
  portEXIT_CRITICAL(&s_mux);
}

static void appendStatusJson(String &json) {
  uint8_t own[6] = {};
  WiFi.softAPmacAddress(own);
  char ownStr[18];
  char peerStr[18];
  formatMac(own, ownStr);
  if (s_peer_set) {
    formatMac(s_peer, peerStr);
  } else {
    peerStr[0] = '\0';
  }

  uint32_t sent, pong, fail, echoRecv, echoSent, ignored;
  int64_t rttLast, rttMin, rttMax;
  uint64_t rttSum;
  portENTER_CRITICAL(&s_mux);
  sent = s_sent;
  pong = s_pong;
  fail = s_send_fail;
  echoRecv = s_echo_recv;
  echoSent = s_echo_sent;
  ignored = s_ignored;
  rttLast = s_rtt_last;
  rttMin = s_rtt_min;
  rttMax = s_rtt_max;
  rttSum = s_rtt_sum;
  portEXIT_CRITICAL(&s_mux);

  const char *role = "idle";
  if (s_role == LinkRole::Ping)
    role = "ping";
  else if (s_role == LinkRole::Echo)
    role = "echo";

  float success = 0;
  if (sent > 0) {
    success = (100.0f * static_cast<float>(pong)) / static_cast<float>(sent);
  }
  uint32_t avg = 0;
  if (pong > 0) {
    avg = static_cast<uint32_t>(rttSum / pong);
  }

  json += "{\"ok\":true,\"ready\":";
  json += s_ready ? "true" : "false";
  json += ",\"mac\":\"";
  json += ownStr;
  json += "\",\"peer\":\"";
  json += peerStr;
  json += "\",\"peerSet\":";
  json += s_peer_set ? "true" : "false";
  json += ",\"role\":\"";
  json += role;
  json += "\",\"running\":";
  json += s_running ? "true" : "false";
  json += ",\"channel\":";
  json += String(LINK_WIFI_CHANNEL);
  json += ",\"gpioSend\":";
  json += String(LINK_GPIO_SEND);
  json += ",\"gpioRecv\":";
  json += String(LINK_GPIO_RECV);
  json += ",\"sent\":";
  json += String(sent);
  json += ",\"pong\":";
  json += String(pong);
  json += ",\"lost\":";
  json += String(sent > pong ? sent - pong : 0);
  json += ",\"sendFail\":";
  json += String(fail);
  json += ",\"echoRecv\":";
  json += String(echoRecv);
  json += ",\"echoSent\":";
  json += String(echoSent);
  json += ",\"ignored\":";
  json += String(ignored);
  json += ",\"rttUsLast\":";
  json += String(static_cast<long>(rttLast));
  json += ",\"rttUsMin\":";
  json += String(static_cast<long>(rttMin));
  json += ",\"rttUsMax\":";
  json += String(static_cast<long>(rttMax));
  json += ",\"rttUsAvg\":";
  json += String(avg);
  json += ",\"successPct\":";
  json += String(success, 2);
  json += "}";
}

static void linkTask(void *arg) {
  (void)arg;
  for (;;) {
    if (s_running && s_role == LinkRole::Ping) {
      linkRadioPoll();
      vTaskDelay(1); // ~1 ms → ~1 kHz offered load
    } else {
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }
}

bool linkRadioBegin() {
  pinMode(LINK_GPIO_SEND, OUTPUT);
  pinMode(LINK_GPIO_RECV, OUTPUT);
  digitalWrite(LINK_GPIO_SEND, LOW);
  digitalWrite(LINK_GPIO_RECV, LOW);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[LINK] esp_now_init failed");
    s_ready = false;
    return false;
  }
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSend);
  s_ready = true;
  xTaskCreatePinnedToCore(linkTask, "link", 4096, nullptr, 1, nullptr, 0);

  uint8_t own[6] = {};
  WiFi.softAPmacAddress(own);
  char ownStr[18];
  formatMac(own, ownStr);
  Serial.printf("[LINK] ESP-NOW ready  AP MAC=%s  ch=%u  GPIO send=%d recv=%d\n",
                ownStr, LINK_WIFI_CHANNEL, LINK_GPIO_SEND, LINK_GPIO_RECV);
  return true;
}

void linkRadioPoll() {
  if (!s_running || s_role != LinkRole::Ping || !s_peer_set) {
    return;
  }
  const int64_t now = esp_timer_get_time();
  if (s_last_ping_us != 0 && (now - s_last_ping_us) < static_cast<int64_t>(PING_PERIOD_US)) {
    return;
  }
  s_last_ping_us = now;
  sendPing();
}

void linkRadioRegisterRoutes(AsyncWebServer &server) {
  server.on("/api/link", HTTP_GET, [](AsyncWebServerRequest *req) {
    String json;
    json.reserve(512);
    appendStatusJson(json);
    req->send(200, "application/json", json);
  });

  server.on("/api/link", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (req->hasParam("peer", true)) {
      uint8_t mac[6];
      if (!parseMac(req->getParam("peer", true)->value(), mac) ||
          !applyPeer(mac)) {
        req->send(400, "application/json",
                  "{\"ok\":false,\"error\":\"Bad peer MAC (need unicast)\"}");
        return;
      }
    }
    if (req->hasParam("role", true)) {
      const String role = req->getParam("role", true)->value();
      if (role == "ping")
        s_role = LinkRole::Ping;
      else if (role == "echo")
        s_role = LinkRole::Echo;
      else
        s_role = LinkRole::Idle;
    }
    if (req->hasParam("run", true)) {
      const String run = req->getParam("run", true)->value();
      const bool on = (run == "1" || run == "true" || run == "on");
      if (on && !s_peer_set) {
        req->send(400, "application/json",
                  "{\"ok\":false,\"error\":\"Set peer MAC first\"}");
        return;
      }
      if (on && s_role == LinkRole::Idle) {
        req->send(400, "application/json",
                  "{\"ok\":false,\"error\":\"Pick ping or echo\"}");
        return;
      }
      if (on) {
        resetStats();
      }
      s_running = on;
    }

    String json;
    json.reserve(512);
    appendStatusJson(json);
    req->send(200, "application/json", json);
  });
}

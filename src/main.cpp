/**
 * ESP32 Wireless Web-Based Game Boy
 * Phase 1: Peanut-GB emulation + WebSocket A/V stream + web controls
 *
 * Packet (ESP32 → browser), type 0x01:
 *   [0]      type
 *   [1..2]   audio length N (uint16 LE)
 *   [3..3+N) 8-bit mono PCM
 *   rest     5760 bytes packed 2-bit video (160x144)
 *
 * Input (browser → ESP32): 1-byte bitmask
 *   bit0 Right, bit1 Left, bit2 Up, bit3 Down,
 *   bit4 A, bit5 B, bit6 Select, bit7 Start
 */

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include "link_radio.h"
#include <WiFi.h>
#include "wifi_secrets.h"

#define ENABLE_SOUND 1
#define ENABLE_LCD 1

extern "C" {
#include "minigb_apu.h"
}

static struct minigb_apu_ctx apu;

extern "C" uint8_t audio_read(uint16_t addr) {
  return minigb_apu_audio_read(&apu, addr);
}

extern "C" void audio_write(uint16_t addr, uint8_t val) {
  minigb_apu_audio_write(&apu, addr, val);
}

extern "C" {
#include "peanut_gb.h"

}

static uint8_t jn100_pending_tx = 0;
static bool jn100_pending_internal_clock = false;
static bool jn100_transfer_pending = false;
static volatile uint32_t jn100_tx_count = 0;
static volatile uint32_t jn100_rx_count = 0;
static volatile uint8_t jn100_last_rx = 0;
static constexpr uint32_t JN100_EVENT_LOG_SIZE = 1024;
static uint8_t jn100_event_bytes[JN100_EVENT_LOG_SIZE] = {};
static bool jn100_event_internal_clock[JN100_EVENT_LOG_SIZE] = {};
static volatile uint32_t jn100_event_total = 0;
static constexpr uint16_t JN100_PACKET_SIZE = 128;
static uint8_t jn100_packet[JN100_PACKET_SIZE] = {};
static volatile uint16_t jn100_packet_length = 0;
static volatile bool jn100_packet_complete = false;
static volatile bool jn100_capture_armed = true;
static uint8_t jn100_sync_80_count = 0;
static bool jn100_sync_saw_86 = false;
static bool jn100_waiting_for_external_payload = false;
static constexpr uint16_t JN100_TRANSPORT_QUEUE_SIZE = 256;
static uint8_t jn100_transport_queue[JN100_TRANSPORT_QUEUE_SIZE] = {};
static volatile uint16_t jn100_transport_head = 0;
static volatile uint16_t jn100_transport_tail = 0;
static volatile uint16_t jn100_transport_depth = 0;
static volatile uint32_t jn100_transport_enqueued = 0;
static volatile uint32_t jn100_transport_dequeued = 0;
static volatile uint32_t jn100_transport_overflows = 0;
static volatile uint8_t jn100_transport_last_byte = 0;

static bool jn100_transport_enqueue(const uint8_t value)
{
    if (jn100_transport_depth >= JN100_TRANSPORT_QUEUE_SIZE) {
        ++jn100_transport_overflows;
        return false;
    }

    jn100_transport_queue[jn100_transport_tail] = value;
    jn100_transport_tail =
        (jn100_transport_tail + 1) % JN100_TRANSPORT_QUEUE_SIZE;
    ++jn100_transport_depth;
    ++jn100_transport_enqueued;
    return true;
}

static bool jn100_transport_dequeue(uint8_t *value)
{
    if (value == nullptr || jn100_transport_depth == 0) {
        return false;
    }

    *value = jn100_transport_queue[jn100_transport_head];
    jn100_transport_head =
        (jn100_transport_head + 1) % JN100_TRANSPORT_QUEUE_SIZE;
    --jn100_transport_depth;
    ++jn100_transport_dequeued;
    jn100_transport_last_byte = *value;
    return true;
}

static void jn100_transport_reset()
{
    jn100_transport_head = 0;
    jn100_transport_tail = 0;
    jn100_transport_depth = 0;
    jn100_transport_enqueued = 0;
    jn100_transport_dequeued = 0;
    jn100_transport_overflows = 0;
    jn100_transport_last_byte = 0;
}

static void jn100_capture_payload(const uint8_t payload)
{
    if (!jn100_capture_armed || jn100_packet_complete) {
        return;
    }

    if (jn100_packet_length != 0) {
        const uint16_t index = jn100_packet_length;
        if (index < JN100_PACKET_SIZE) {
            jn100_packet[index] = payload;
            jn100_packet_length = index + 1;
        }
        if (jn100_packet_length == JN100_PACKET_SIZE) {
            jn100_packet_complete = true;
            jn100_capture_armed = false;
        }
        return;
    }

    /*
     * B9 is the first byte of a 128-byte JN-100 pattern packet. Start
     * directly from the header because a browser-triggered clear can occur
     * partway through the preceding 80 80 80 86 synchronization sequence.
     */
    if (payload == 0xB9) {
        jn100_packet[0] = payload;
        jn100_packet_length = 1;
        return;
    }

    if (jn100_sync_saw_86) {
        if (payload == 0xB9) {
            jn100_packet[0] = payload;
            jn100_packet_length = 1;
            return;
        }
        jn100_sync_saw_86 = false;
        jn100_sync_80_count = (payload == 0x80) ? 1 : 0;
        return;
    }

    if (payload == 0x80) {
        if (jn100_sync_80_count < 3) {
            ++jn100_sync_80_count;
        }
    } else if (payload == 0x86 && jn100_sync_80_count >= 3) {
        jn100_sync_saw_86 = true;
    } else {
        jn100_sync_80_count = 0;
    }
}

static void jn100_serial_tx(struct gb_s *gb_ctx, const uint8_t tx)
{
    const bool internal_clock =
        (gb_ctx->hram_io[IO_SC] & 0x01) != 0;
    bool replacement_payload = false;

    if (internal_clock) {
        jn100_waiting_for_external_payload = false;
    } else if (jn100_waiting_for_external_payload) {
        replacement_payload = true;
        jn100_waiting_for_external_payload = false;
    } else {
        jn100_waiting_for_external_payload = true;
    }

    jn100_pending_tx = tx;
    jn100_pending_internal_clock = internal_clock;

    jn100_transfer_pending = true;
    ++jn100_tx_count;

    const uint32_t event_number = jn100_event_total;
    const uint32_t event_index = event_number % JN100_EVENT_LOG_SIZE;
    jn100_event_bytes[event_index] = tx;
    jn100_event_internal_clock[event_index] =
        jn100_pending_internal_clock;
    jn100_event_total = event_number + 1;

    if (replacement_payload) {
        /*
         * Queue every meaningful ROM-generated byte. Until the physical
         * clock backend exists, immediately consume whole bytes through a
         * diagnostic sink so continuous 0x80 polling cannot fill the queue.
         */
        if (jn100_transport_enqueue(tx)) {
            uint8_t diagnostic_byte = 0;
            jn100_transport_dequeue(&diagnostic_byte);
        }
        jn100_capture_payload(tx);
    }

    Serial.printf(
        "[JN100] clock=%s tx=%02X\n",
        jn100_pending_internal_clock ? "internal" : "external",
        static_cast<unsigned int>(tx)
    );
}

static enum gb_serial_rx_ret_e jn100_serial_rx(
    struct gb_s *gb_ctx,
    uint8_t *rx
) {
    (void)gb_ctx;

    if (!jn100_transfer_pending) {
        return GB_SERIAL_RX_NO_CONNECTION;
    }

    // Virtual JN-100 response for software testing.
    *rx = jn100_pending_internal_clock ? 0xFF : 0x00;
    jn100_last_rx = *rx;
    ++jn100_rx_count;

    Serial.printf(
        "[JN100] rx=%02X\n",
        static_cast<unsigned int>(*rx)
    );

    jn100_transfer_pending = false;
    return GB_SERIAL_RX_SUCCESS;
}
// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const char *AP_SSID = "ESP-GameBoy";
static volatile uint8_t wifi_last_disconnect_reason = 0;
static const char *ROMS_DIR = "/roms";
static const char *SAVES_DIR = "/saves";
static const char *CURRENT_PATH = "/current.txt";
static const char *SAVE_TMP_PATH = "/saves/upload.part";
// Fixed short temp name — long Pokémon titles + ".gb.part" can exceed LittleFS's 64-char limit
static const char *ROM_TMP_PATH = "/roms/_up.part";
static const char *ROM_TMP_FALLBACK = "/_up.part";
static const char *LEGACY_ROM_PATH = "/rom.gb";
static const char *LEGACY_SAVE_PATH = "/rom.sav";
static const char *LEGACY_NAME_PATH = "/rom.name";
static const size_t MAX_ROM_BYTES = 2 * 1024 * 1024; // 2 MiB per ROM
static const size_t MAX_SAVE_BYTES = 128 * 1024;      // GB cart RAM upper bound
static const uint32_t SAVE_AUTOSAVE_MS = 15000;

// Active cartridge identity (basename inside /roms, e.g. "Tetris.gb")
static String current_rom_id;
static char current_rom_path[96];
static char current_save_path[96];

static const uint16_t LCD_W = 160;
static const uint16_t LCD_H = 144;
static const size_t VIDEO_BYTES = (LCD_W * LCD_H) / 4; // 5760
static const uint8_t PKT_FRAME = 0x01;

// ---------------------------------------------------------------------------
// Emulator private state
// ---------------------------------------------------------------------------
struct priv_t {
  uint8_t *rom;
  size_t rom_size;
  uint8_t *cart_ram;
  size_t cart_ram_size;
};

static struct gb_s gb;
static struct priv_t priv = {};

// Packed video double-buffer + staging packet
static uint8_t *video_draw = nullptr;  // written by lcd_draw_line
static uint8_t *video_send = nullptr;  // snapshot for WS
static uint8_t *frame_packet = nullptr;
static size_t frame_packet_cap = 0;

// PCM generated every emu frame so APU stays in sync even when WS drops.
// Length is 548 or 549 (Bresenham) so average matches exact 548.625 samples/frame.
static uint8_t audio_pcm[AUDIO_SAMPLES_MAX];
static uint16_t audio_pcm_len = 0;
static uint64_t audio_sample_accum = 0;

static volatile uint8_t joypad_mask = 0; // Spec bitmask, 1 = pressed
static volatile bool frame_ready = false;
static volatile bool emu_running = false;
static portMUX_TYPE joy_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t frame_mutex = nullptr;

// ROM upload state
static File uploadFile;
static bool uploadOk = false;
static bool uploadActive = false;
static size_t uploadBytes = 0;
static String uploadOrigName;
static String uploadPartPath; // ROM_TMP_PATH or fallback
static String uploadDestPath; // /roms/<name>.gb
static String uploadError;
static volatile bool reboot_pending = false;

// Battery save (cart RAM) persistence
static volatile bool save_dirty = false;
static uint32_t last_autosave_ms = 0;
static bool save_upload_ok = false;
static size_t save_upload_bytes = 0;
static File saveUploadFile;

// ---------------------------------------------------------------------------
// Peanut-GB callbacks
// ---------------------------------------------------------------------------
static uint8_t gb_rom_read(struct gb_s *gb_ptr, const uint_fast32_t addr) {
  auto *p = static_cast<priv_t *>(gb_ptr->direct.priv);
  if (addr >= p->rom_size) return 0xFF;
  return p->rom[addr];
}

static uint8_t gb_cart_ram_read(struct gb_s *gb_ptr, const uint_fast32_t addr) {
  auto *p = static_cast<priv_t *>(gb_ptr->direct.priv);
  if (!p->cart_ram || addr >= p->cart_ram_size) return 0xFF;
  return p->cart_ram[addr];
}

static void gb_cart_ram_write(struct gb_s *gb_ptr, const uint_fast32_t addr,
                              const uint8_t val) {
  auto *p = static_cast<priv_t *>(gb_ptr->direct.priv);
  if (!p->cart_ram || addr >= p->cart_ram_size) return;
  if (p->cart_ram[addr] != val) {
    p->cart_ram[addr] = val;
    save_dirty = true;
  }
}

// ---------------------------------------------------------------------------
// ROM library helpers
// ---------------------------------------------------------------------------
static bool fsRenameOrCopy(const char *from, const char *to) {
  LittleFS.remove(to);
  if (LittleFS.rename(from, to)) return true;
  File src = LittleFS.open(from, "r");
  File dst = LittleFS.open(to, "w");
  if (!src || !dst) {
    if (src) src.close();
    if (dst) dst.close();
    return false;
  }
  uint8_t buf[1024];
  while (src.available()) {
    size_t n = src.read(buf, sizeof(buf));
    if (dst.write(buf, n) != n) {
      src.close();
      dst.close();
      LittleFS.remove(to);
      return false;
    }
  }
  src.close();
  dst.close();
  LittleFS.remove(from);
  return true;
}

static String sanitizeRomFilename(const String &in) {
  int slash = in.lastIndexOf('/');
  int bslash = in.lastIndexOf('\\');
  int cut = slash > bslash ? slash : bslash;
  String base = in.substring(cut + 1);
  String out;
  out.reserve(40);
  for (size_t i = 0; i < base.length() && out.length() < 36; i++) {
    char c = base[i];
    if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ||
        c == '.') {
      out += c;
    } else {
      out += '_';
    }
  }
  out.toLowerCase();
  // Strip extension, then force .gb (DMG core; file bytes unchanged)
  int dot = out.lastIndexOf('.');
  if (dot >= 0) out = out.substring(0, dot);
  // Keep final name well under LittleFS CONFIG_LITTLEFS_OBJ_NAME_LEN (64)
  if (out.length() > 28) out = out.substring(0, 28);
  while (out.length() && (out.endsWith("_") || out.endsWith("-") || out.endsWith("."))) {
    out.remove(out.length() - 1);
  }
  if (out.length() < 1) out = "game";
  out += ".gb";
  return out;
}

static String saveIdFromRomId(const String &rom_id) {
  String base = rom_id;
  if (base.endsWith(".gb")) base = base.substring(0, base.length() - 3);
  return base + ".sav";
}

static void setActiveRomId(const String &rom_id) {
  current_rom_id = rom_id;
  snprintf(current_rom_path, sizeof(current_rom_path), "%s/%s", ROMS_DIR,
           rom_id.c_str());
  String sav = saveIdFromRomId(rom_id);
  snprintf(current_save_path, sizeof(current_save_path), "%s/%s", SAVES_DIR,
           sav.c_str());
}

static bool writeCurrentRomId(const String &rom_id) {
  File f = LittleFS.open(CURRENT_PATH, "w");
  if (!f) return false;
  f.print(rom_id);
  f.close();
  setActiveRomId(rom_id);
  return true;
}

static String readCurrentRomId() {
  if (!LittleFS.exists(CURRENT_PATH)) return String();
  File f = LittleFS.open(CURRENT_PATH, "r");
  if (!f) return String();
  String id = f.readString();
  f.close();
  id.trim();
  return sanitizeRomFilename(id);
}

static size_t fsFreeBytes() {
  size_t total = LittleFS.totalBytes();
  size_t used = LittleFS.usedBytes();
  return total > used ? total - used : 0;
}

static void appendStorageJson(String &json) {
  size_t total = LittleFS.totalBytes();
  size_t used = LittleFS.usedBytes();
  size_t free_b = total > used ? total - used : 0;
  json += "\"storage\":{\"total\":";
  json += String(static_cast<unsigned>(total));
  json += ",\"used\":";
  json += String(static_cast<unsigned>(used));
  json += ",\"free\":";
  json += String(static_cast<unsigned>(free_b));
  json += "}";
}

static bool flushSaveToFs() {
  if (!priv.cart_ram || priv.cart_ram_size == 0) return true;
  if (!save_dirty) return true;
  if (current_save_path[0] == '\0') return false;

  File f = LittleFS.open(current_save_path, "w");
  if (!f) {
    Serial.println("[SAVE] write open failed");
    return false;
  }
  size_t written = f.write(priv.cart_ram, priv.cart_ram_size);
  f.close();
  if (written != priv.cart_ram_size) {
    Serial.println("[SAVE] short write");
    return false;
  }
  save_dirty = false;
  Serial.printf("[SAVE] flushed %u bytes → %s\n",
                static_cast<unsigned>(priv.cart_ram_size), current_save_path);
  return true;
}

static bool loadSaveFromFs() {
  if (!priv.cart_ram || priv.cart_ram_size == 0) return true;
  if (current_save_path[0] == '\0' || !LittleFS.exists(current_save_path)) {
    Serial.println("[SAVE] no save file yet");
    return true;
  }
  File f = LittleFS.open(current_save_path, "r");
  if (!f) return false;
  size_t sz = f.size();
  size_t to_read = sz < priv.cart_ram_size ? sz : priv.cart_ram_size;
  size_t got = f.read(priv.cart_ram, to_read);
  f.close();
  save_dirty = false;
  Serial.printf("[SAVE] loaded %u / %u bytes from %s\n",
                static_cast<unsigned>(got),
                static_cast<unsigned>(priv.cart_ram_size), current_save_path);
  return got > 0;
}

static void migrateLegacyLibrary() {
  LittleFS.mkdir(ROMS_DIR);
  LittleFS.mkdir(SAVES_DIR);

  if (LittleFS.exists(LEGACY_ROM_PATH)) {
    String name = "legacy.gb";
    if (LittleFS.exists(LEGACY_NAME_PATH)) {
      File nf = LittleFS.open(LEGACY_NAME_PATH, "r");
      if (nf) {
        name = sanitizeRomFilename(nf.readString());
        nf.close();
      }
    }
    String dest = String(ROMS_DIR) + "/" + name;
    if (!LittleFS.exists(dest.c_str())) {
      fsRenameOrCopy(LEGACY_ROM_PATH, dest.c_str());
    } else {
      LittleFS.remove(LEGACY_ROM_PATH);
    }
    if (!LittleFS.exists(CURRENT_PATH)) {
      writeCurrentRomId(name);
    }
    LittleFS.remove(LEGACY_NAME_PATH);

    if (LittleFS.exists(LEGACY_SAVE_PATH)) {
      String sav = String(SAVES_DIR) + "/" + saveIdFromRomId(name);
      if (!LittleFS.exists(sav.c_str())) {
        fsRenameOrCopy(LEGACY_SAVE_PATH, sav.c_str());
      } else {
        LittleFS.remove(LEGACY_SAVE_PATH);
      }
    }
    Serial.printf("[LIB] migrated legacy ROM → %s\n", dest.c_str());
  }

  String cur = readCurrentRomId();
  if (cur.length()) {
    String path = String(ROMS_DIR) + "/" + cur;
    if (LittleFS.exists(path.c_str())) {
      setActiveRomId(cur);
    } else {
      LittleFS.remove(CURRENT_PATH);
      current_rom_id = "";
      current_rom_path[0] = '\0';
      current_save_path[0] = '\0';
    }
  }
}

static void gb_error(struct gb_s *gb_ptr, const enum gb_error_e err,
                     const uint16_t addr) {
  (void)gb_ptr;
  Serial.printf("[GB] error %d @ 0x%04X\n", static_cast<int>(err), addr);
}

static void lcd_draw_line(struct gb_s *gb_ptr, const uint8_t *pixels,
                          const uint_fast8_t line) {
  (void)gb_ptr;
  if (!video_draw || line >= LCD_H) return;

  uint8_t *dst = video_draw + (static_cast<size_t>(line) * (LCD_W / 4));
  for (uint16_t x = 0; x < LCD_W; x += 4) {
    const uint8_t b =
        ((pixels[x] & 0x03) << 6) |
        ((pixels[x + 1] & 0x03) << 4) |
        ((pixels[x + 2] & 0x03) << 2) |
        (pixels[x + 3] & 0x03);
    *dst++ = b;
  }
}

/** Map Spec bitmask (1=pressed) → Peanut-GB joypad (0=pressed). */
static uint8_t mask_to_peanut(uint8_t mask) {
  uint8_t joy = 0xFF;
  if (mask & (1 << 0)) joy &= ~JOYPAD_RIGHT;
  if (mask & (1 << 1)) joy &= ~JOYPAD_LEFT;
  if (mask & (1 << 2)) joy &= ~JOYPAD_UP;
  if (mask & (1 << 3)) joy &= ~JOYPAD_DOWN;
  if (mask & (1 << 4)) joy &= ~JOYPAD_A;
  if (mask & (1 << 5)) joy &= ~JOYPAD_B;
  if (mask & (1 << 6)) joy &= ~JOYPAD_SELECT;
  if (mask & (1 << 7)) joy &= ~JOYPAD_START;
  return joy;
}

// ---------------------------------------------------------------------------
// Networking
// ---------------------------------------------------------------------------
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

static void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                      AwsEventType type, void *arg, uint8_t *data, size_t len) {
  (void)server;
  (void)arg;
  if (type == WS_EVT_CONNECT) {
    Serial.printf("[WS] client #%u connected\n", client->id());
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("[WS] client #%u disconnected\n", client->id());
  } else if (type == WS_EVT_DATA) {
    if (len >= 1) {
      portENTER_CRITICAL(&joy_mux);
      joypad_mask = data[0];
      portEXIT_CRITICAL(&joy_mux);
    }
  }
}

static DNSServer dnsServer;

// IP the phone must use for the real UI on *this* TCP connection (whatever
// SoftAP/STA address the ESP is answering on right now — never hardcode).
static String webUiIpForRequest(AsyncWebServerRequest *request) {
  if (request && request->client()) {
    const IPAddress lip = request->client()->localIP();
    if (lip && lip != IPAddress(0, 0, 0, 0)) {
      return lip.toString();
    }
  }
  if (WiFi.getMode() & WIFI_AP) {
    const IPAddress ap = WiFi.softAPIP();
    if (ap && ap != IPAddress(0, 0, 0, 0)) {
      return ap.toString();
    }
  }
  if (WiFi.status() == WL_CONNECTED) {
    return WiFi.localIP().toString();
  }
  return WiFi.softAPIP().toString();
}

static bool hostIsOurWebUi(const String &host, const String &uiIp) {
  if (!host.length()) {
    return false;
  }
  if (host == uiIp || host == uiIp + ":80") {
    return true;
  }
  const String ap = WiFi.softAPIP().toString();
  if (ap.length() && (host == ap || host == ap + ":80")) {
    return true;
  }
  if (WiFi.status() == WL_CONNECTED) {
    const String sta = WiFi.localIP().toString();
    if (sta.length() && (host == sta || host == sta + ":80")) {
      return true;
    }
  }
  return false;
}

// Captive probes land here. Only a link into the system browser — the full UI
// inside the captive sheet breaks controls / WebSocket / audio.
static void sendCaptivePortalPage(AsyncWebServerRequest *request) {
  const String ip = webUiIpForRequest(request);
  const String url = String("http://") + ip + "/";
  // Android: push Chrome out of the captive sheet when a plain <a> stays sandboxed.
  String intent = String("intent://") + ip +
                  "/#Intent;scheme=http;package=com.android.chrome;end";

  String html;
  html.reserve(1400);
  html += F(
      "<!DOCTYPE html><html lang=\"en\"><head>"
      "<meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<title>ESP-GameBoy</title>"
      "<style>"
      "html,body{margin:0;min-height:100%;background:#c4c4b0;color:#2a2a22;"
      "font-family:Trebuchet MS,Segoe UI,sans-serif;}"
      "main{min-height:100dvh;display:flex;flex-direction:column;"
      "align-items:center;justify-content:center;padding:24px;text-align:center;}"
      ".mark{font-size:.85rem;font-weight:700;letter-spacing:.12em;color:#8b1e5a;}"
      "h1{margin:.35rem 0 1rem;font-size:1.6rem;letter-spacing:.08em;}"
      "p{margin:0 0 1.25rem;max-width:22rem;line-height:1.4;color:#4a4a3a;}"
      "a.primary{display:inline-block;padding:14px 22px;border-radius:10px;"
      "background:#8b1e5a;color:#fff;text-decoration:none;font-weight:700;"
      "font-size:1.05rem;word-break:break-all;}"
      "a.primary:active{background:#5c123c;}"
      "a.secondary{display:inline-block;margin-top:12px;color:#8b1e5a;"
      "font-weight:700;font-size:.95rem;}"
      ".hint{margin-top:1.25rem;font-size:.85rem;}"
      "</style></head><body><main>"
      "<div class=\"mark\">ESP</div><h1>GAME BOY</h1>"
      "<p>Open this address in Safari or Chrome — the sign-in window cannot run "
      "the controller.</p>"
      "<a class=\"primary\" target=\"_blank\" rel=\"noopener\" href=\"");
  html += url;
  html += F("\">");
  html += url;
  html += F("</a><a class=\"secondary\" href=\"");
  html += intent;
  html += F("\">Open in Chrome (Android)</a>"
            "<p class=\"hint\">Stay on this Wi‑Fi (no internet is normal). "
            "If the link stays in the sign-in window, paste the address into "
            "your browser.</p>"
            "</main></body></html>");

  AsyncWebServerResponse *res =
      request->beginResponse(200, "text/html", html);
  res->addHeader("Cache-Control", "no-store");
  request->send(res);
}

// Any HTTP GET whose Host is not our live UI IP → captive landing page.
// Registered first so it wins over serveStatic for "/" under a fake host.
class CaptivePortalHandler : public AsyncWebHandler {
public:
  bool canHandle(AsyncWebServerRequest *request) const override {
    if (request->method() != HTTP_GET) {
      return false;
    }
    const String host = request->host();
    return host.length() && !hostIsOurWebUi(host, webUiIpForRequest(request));
  }

  void handleRequest(AsyncWebServerRequest *request) override {
    sendCaptivePortalPage(request);
  }
};

static void setupWifiAp() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  WiFi.setHostname("esp-gameboy");

  WiFi.onEvent(
      [](WiFiEvent_t event, WiFiEventInfo_t info) {
        (void)event;
        wifi_last_disconnect_reason =
            info.wifi_sta_disconnected.reason;
      },
      ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  // Try connecting to the home 2.4 GHz network.
  WiFi.begin(HOME_WIFI_SSID, HOME_WIFI_PASSWORD);

  Serial.print("[WiFi] Connecting to ");
  Serial.println(HOME_WIFI_SSID);

  const uint32_t connect_start = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - connect_start < 15000) {
    delay(250);
  }

  // When connected, the AP must share the router's Wi-Fi channel.
  const uint8_t ap_channel =
      (WiFi.status() == WL_CONNECTED)
          ? WiFi.channel()
          : LINK_WIFI_CHANNEL;

  // Keep the original ESP-GameBoy network as a fallback.
  WiFi.softAP(AP_SSID, nullptr, ap_channel, 0, 1);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] Home network IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("[WiFi] Home connection failed; using fallback AP");
  }

  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", WiFi.softAPIP());

  Serial.print("[WiFi] Fallback AP: ");
  Serial.print(AP_SSID);
  Serial.print(" IP=");
  Serial.println(WiFi.softAPIP());
}

static void jsonEscape(const String &in, String &out) {
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<uint8_t>(c) < 0x20) {
      // skip control chars
    } else {
      out += c;
    }
  }
}

static const char *wifiStatusName(const wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS: return "IDLE";
    case WL_NO_SSID_AVAIL: return "NO_SSID_AVAILABLE";
    case WL_SCAN_COMPLETED: return "SCAN_COMPLETED";
    case WL_CONNECTED: return "CONNECTED";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED: return "DISCONNECTED";
    default: return "UNKNOWN";
  }
}

static bool isPlausibleRomSize(size_t n) {
  // Common GB/GBC cart sizes (bytes), including padded dumps
  static const size_t kSizes[] = {
      32768u,    65536u,    131072u,   262144u,  524288u,
      1048576u,  2097152u,  4194304u,  8388608u};
  for (size_t s : kSizes) {
    if (n == s) return true;
    // Allow small overdump padding (up to +1 KiB) seen on some dumps
    if (n > s && n <= s + 1024) return true;
  }
  return n >= 0x8000 && n <= MAX_ROM_BYTES;
}

static bool romHasNintendoLogo(File &f) {
  if (!f) return false;
  if (!f.seek(0x104)) return false;
  uint8_t logo[4] = {0};
  if (f.read(logo, 4) != 4) return false;
  // Official boot logo begins CE ED 66 66
  return logo[0] == 0xCE && logo[1] == 0xED && logo[2] == 0x66 &&
         logo[3] == 0x66;
}

static void handleRomUpload(AsyncWebServerRequest *request, String filename,
                            size_t index, uint8_t *data, size_t len,
                            bool final) {
  if (index == 0) {
    uploadActive = true;
    uploadOk = true;
    uploadBytes = 0;
    uploadError = "";
    uploadOrigName = sanitizeRomFilename(filename);
    uploadDestPath = String(ROMS_DIR) + "/" + uploadOrigName;
    uploadPartPath = ROM_TMP_PATH;
    emu_running = false;
    flushSaveToFs();

    // Close any leftover handle from a previous attempt
    if (uploadFile) uploadFile.close();

    size_t free_b = fsFreeBytes();
    size_t content_len = request ? request->contentLength() : 0;
    Serial.printf("[ROM] upload begin name=%s dest=%s free=%u contentLen=%u\n",
                  uploadOrigName.c_str(), uploadDestPath.c_str(),
                  static_cast<unsigned>(free_b),
                  static_cast<unsigned>(content_len));

    // Multipart body is a bit larger than the raw ROM. Big carts (~1MB) need room.
    size_t min_free = 128 * 1024;
    if (content_len > 700000) min_free = 1200 * 1024;
    if (free_b < min_free) {
      uploadError = "Not enough free flash for this ROM. Delete a game first.";
      Serial.printf("[ROM] abort: free=%u need>=%u\n",
                    static_cast<unsigned>(free_b),
                    static_cast<unsigned>(min_free));
      uploadOk = false;
      return;
    }

    LittleFS.mkdir(ROMS_DIR);
    if (LittleFS.exists(ROM_TMP_PATH)) LittleFS.remove(ROM_TMP_PATH);
    if (LittleFS.exists(ROM_TMP_FALLBACK)) LittleFS.remove(ROM_TMP_FALLBACK);

    uploadFile = LittleFS.open(ROM_TMP_PATH, "w");
    if (!uploadFile) {
      // Fallback to root (same style as the original /rom.gb.tmp path)
      uploadPartPath = ROM_TMP_FALLBACK;
      uploadFile = LittleFS.open(ROM_TMP_FALLBACK, "w");
    }
    if (!uploadFile) {
      uploadError = "Could not create temp ROM file on flash";
      Serial.printf("[ROM] open failed tmp=%s fallback=%s free=%u total=%u\n",
                    ROM_TMP_PATH, ROM_TMP_FALLBACK,
                    static_cast<unsigned>(free_b),
                    static_cast<unsigned>(LittleFS.totalBytes()));
      uploadOk = false;
    } else {
      Serial.printf("[ROM] writing temp %s\n", uploadPartPath.c_str());
    }
  }

  if (!uploadOk) return;

  if (uploadBytes + len > MAX_ROM_BYTES) {
    uploadError = "ROM exceeds 2MB limit";
    uploadOk = false;
    uploadFile.close();
    LittleFS.remove(uploadPartPath.c_str());
    return;
  }

  if (len > 0) {
    size_t written = uploadFile.write(data, len);
    if (written != len) {
      uploadError = "Flash write failed mid-upload (space or FS error)";
      Serial.printf("[ROM] write fail at %u want %u got %u free=%u\n",
                    static_cast<unsigned>(uploadBytes),
                    static_cast<unsigned>(len),
                    static_cast<unsigned>(written),
                    static_cast<unsigned>(fsFreeBytes()));
      uploadOk = false;
      uploadFile.close();
      LittleFS.remove(uploadPartPath.c_str());
      return;
    }
    uploadBytes += written;
    if ((uploadBytes & 0x1FFFF) == 0) { // every 128 KiB
      Serial.printf("[ROM] … %u bytes\n", static_cast<unsigned>(uploadBytes));
    }
    // Keep Wi-Fi/async happy on long transfers
    delay(0);
  }

  if (final) {
    uploadFile.close();
    if (!uploadOk) {
      LittleFS.remove(uploadPartPath.c_str());
      return;
    }
    if (uploadBytes < 0x150) {
      uploadError = "File too small to be a Game Boy ROM";
      uploadOk = false;
      LittleFS.remove(uploadPartPath.c_str());
      return;
    }

    File check = LittleFS.open(uploadPartPath.c_str(), "r");
    size_t stored = check ? check.size() : 0;
    bool logo_ok = romHasNintendoLogo(check);
    if (check) check.close();

    Serial.printf("[ROM] finalize stored=%u recv=%u logo=%d\n",
                  static_cast<unsigned>(stored),
                  static_cast<unsigned>(uploadBytes), logo_ok ? 1 : 0);

    // Size mismatch usually means the SoftAP dropped chunks — ask for retry
    if (stored != uploadBytes) {
      uploadError = "Upload incomplete (Wi-Fi dropped data). Try again closer.";
      uploadOk = false;
      LittleFS.remove(uploadPartPath.c_str());
      return;
    }

    if (!logo_ok && !isPlausibleRomSize(uploadBytes)) {
      uploadError = "Not a valid Game Boy ROM (bad header/size)";
      uploadOk = false;
      LittleFS.remove(uploadPartPath.c_str());
      return;
    }

    // Same-directory rename .part → .gb (this is what used to work for /rom.gb.tmp)
    if (LittleFS.exists(uploadDestPath.c_str())) {
      LittleFS.remove(uploadDestPath.c_str());
    }
    if (!LittleFS.rename(uploadPartPath.c_str(), uploadDestPath.c_str())) {
      // Last resort copy (still same volume)
      if (!fsRenameOrCopy(uploadPartPath.c_str(), uploadDestPath.c_str())) {
        uploadError = "Failed to finalize ROM into library";
        uploadOk = false;
        LittleFS.remove(uploadPartPath.c_str());
        Serial.println("[ROM] rename/copy to destination failed");
        return;
      }
    }

    writeCurrentRomId(uploadOrigName);
    Serial.printf("[ROM] OK saved %u bytes → %s\n",
                  static_cast<unsigned>(uploadBytes), uploadDestPath.c_str());
  }
}

static void handleSaveUpload(AsyncWebServerRequest *request, String filename,
                             size_t index, uint8_t *data, size_t len,
                             bool final) {
  (void)request;
  (void)filename;

  if (index == 0) {
    save_upload_ok = true;
    save_upload_bytes = 0;
    emu_running = false;
    flushSaveToFs();
    if (current_save_path[0] == '\0') {
      save_upload_ok = false;
      Serial.println("[SAVE] no active ROM for save upload");
      return;
    }
    LittleFS.mkdir(SAVES_DIR);
    if (LittleFS.exists(SAVE_TMP_PATH)) LittleFS.remove(SAVE_TMP_PATH);
    saveUploadFile = LittleFS.open(SAVE_TMP_PATH, "w");
    if (!saveUploadFile) {
      save_upload_ok = false;
      Serial.println("[SAVE] upload open failed");
    }
  }

  if (!save_upload_ok) return;

  size_t limit = MAX_SAVE_BYTES;
  if (priv.cart_ram_size > 0) limit = priv.cart_ram_size;
  if (save_upload_bytes + len > limit) {
    save_upload_ok = false;
    saveUploadFile.close();
    LittleFS.remove(SAVE_TMP_PATH);
    Serial.println("[SAVE] upload too large");
    return;
  }

  if (len > 0) {
    if (saveUploadFile.write(data, len) != len) {
      save_upload_ok = false;
      saveUploadFile.close();
      LittleFS.remove(SAVE_TMP_PATH);
      return;
    }
    save_upload_bytes += len;
  }

  if (final) {
    saveUploadFile.close();
    if (!save_upload_ok || save_upload_bytes == 0) {
      LittleFS.remove(SAVE_TMP_PATH);
      save_upload_ok = false;
      return;
    }
    if (!fsRenameOrCopy(SAVE_TMP_PATH, current_save_path)) {
      LittleFS.remove(SAVE_TMP_PATH);
      save_upload_ok = false;
      return;
    }
    Serial.printf("[SAVE] uploaded %u bytes → %s\n",
                  static_cast<unsigned>(save_upload_bytes), current_save_path);
  }
}

static void setupWebServer() {
  server.addHandler(new CaptivePortalHandler());

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  linkRadioRegisterRoutes(server);

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    String esc;
    jsonEscape(current_rom_id, esc);

    size_t rom_size = 0;
    if (current_rom_path[0] && LittleFS.exists(current_rom_path)) {
      File f = LittleFS.open(current_rom_path, "r");
      if (f) {
        rom_size = f.size();
        f.close();
      }
    }

    String json = "{";
    json += "\"ssid\":\"";
    json += AP_SSID;
    json += "\",\"ip\":\"";
    json += WiFi.softAPIP().toString();
    json += "\",\"staIp\":\"";
    json += (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString()
                                            : String("");
    json += "\",\"rom\":";
    json += (rom_size > 0) ? "true" : "false";
    json += ",\"romSize\":";
    json += String(static_cast<unsigned>(rom_size));
    json += ",\"romName\":\"";
    json += esc;
    json += "\",\"clients\":";
    json += String(ws.count());
    json += ",\"maxRomBytes\":";
    json += String(static_cast<unsigned>(MAX_ROM_BYTES));
    json += ",\"running\":";
    json += emu_running ? "true" : "false";
    json += ",\"battery\":";
    json += (priv.cart_ram_size > 0) ? "true" : "false";
    json += ",\"batterySize\":";
    json += String(static_cast<unsigned>(priv.cart_ram_size));
    size_t save_size = 0;
    if (current_save_path[0] && LittleFS.exists(current_save_path)) {
      File sf = LittleFS.open(current_save_path, "r");
      if (sf) {
        save_size = sf.size();
        sf.close();
      }
    }
    json += ",\"save\":";
    json += (save_size > 0) ? "true" : "false";
    json += ",\"saveSize\":";
    json += String(static_cast<unsigned>(save_size));
    json += ",";
    appendStorageJson(json);
    json += "}";
    req->send(200, "application/json", json);
  });

  server.on("/api/wifi-debug", HTTP_GET,
            [](AsyncWebServerRequest *req) {
    const wl_status_t status = WiFi.status();
    String escaped_ssid;
    jsonEscape(String(HOME_WIFI_SSID), escaped_ssid);

    String json = "{\n";
    json += "  \"configuredSsid\": \"";
    json += escaped_ssid;
    json += "\",\n  \"status\": ";
    json += String(static_cast<int>(status));
    json += ",\n  \"statusName\": \"";
    json += wifiStatusName(status);
    json += "\",\n  \"lastDisconnectReason\": ";
    json += String(static_cast<unsigned>(wifi_last_disconnect_reason));
    json += ",\n  \"staIp\": \"";
    json += (status == WL_CONNECTED) ? WiFi.localIP().toString()
                                     : String("");
    json += "\",\n  \"staMac\": \"";
    json += WiFi.macAddress();
    json += "\",\n  \"apIp\": \"";
    json += WiFi.softAPIP().toString();
    json += "\",\n  \"apMac\": \"";
    json += WiFi.softAPmacAddress();
    json += "\",\n  \"channel\": ";
    json += String(static_cast<unsigned>(WiFi.channel()));
    json += ",\n  \"rssi\": ";
    json += String((status == WL_CONNECTED) ? WiFi.RSSI() : 0);
    json += "\n}";

    AsyncWebServerResponse *response =
        req->beginResponse(200, "application/json", json);
    response->addHeader("Cache-Control", "no-store");
    req->send(response);
  });

  server.on("/api/jn100", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (req->hasParam("clear") &&
        req->getParam("clear")->value() == "1") {
      jn100_event_total = 0;
      jn100_packet_length = 0;
      jn100_packet_complete = false;
      jn100_capture_armed = true;
      jn100_sync_80_count = 0;
      jn100_sync_saw_86 = false;
      jn100_waiting_for_external_payload = false;
      jn100_transport_reset();
      req->send(200, "application/json", "{\"cleared\":true}");
      return;
    }

    const uint32_t event_total = jn100_event_total;
    const uint32_t event_count =
        (event_total < JN100_EVENT_LOG_SIZE) ? event_total
                                             : JN100_EVENT_LOG_SIZE;
    const uint32_t first_event = event_total - event_count;

    String json = "{\n";
    json += "  \"txCount\": ";
    json += String(static_cast<unsigned long>(jn100_tx_count));
    json += ",\n  \"rxCount\": ";
    json += String(static_cast<unsigned long>(jn100_rx_count));
    json += ",\n  \"lastTx\": ";
    json += String(static_cast<unsigned>(jn100_pending_tx));
    json += ",\n  \"lastRx\": ";
    json += String(static_cast<unsigned>(jn100_last_rx));
    json += ",\n  \"clock\": \"";
    json += jn100_pending_internal_clock ? "internal" : "external";
    json += "\",\n  \"eventCount\": ";
    json += String(static_cast<unsigned long>(event_count));
    json += ",\n  \"packetState\": \"";
    if (jn100_packet_complete) {
      json += "complete";
    } else if (jn100_packet_length != 0) {
      json += "capturing";
    } else {
      json += "waiting";
    }
    json += "\",\n  \"packetLength\": ";
    const uint16_t packet_length = jn100_packet_length;
    json += String(static_cast<unsigned>(packet_length));
    const bool packet_start_valid =
        packet_length == JN100_PACKET_SIZE && jn100_packet[0] == 0xB9;
    const bool packet_footer_valid =
        packet_length == JN100_PACKET_SIZE && jn100_packet[125] == 0xBA;
    uint16_t packet_checksum_calculated = 0;
    if (packet_length == JN100_PACKET_SIZE) {
      for (uint16_t i = 0; i <= 125; ++i) {
        packet_checksum_calculated =
            static_cast<uint16_t>(packet_checksum_calculated +
                                  jn100_packet[i]);
      }
    }
    const uint16_t packet_checksum_stored =
        (packet_length == JN100_PACKET_SIZE)
            ? static_cast<uint16_t>(jn100_packet[126] |
                                    (jn100_packet[127] << 8))
            : 0;
    const bool packet_checksum_valid =
        packet_length == JN100_PACKET_SIZE &&
        packet_checksum_calculated == packet_checksum_stored;
    const bool packet_valid = packet_start_valid && packet_footer_valid &&
                              packet_checksum_valid;
    char checksum_calculated_text[5];
    char checksum_stored_text[5];
    snprintf(checksum_calculated_text, sizeof(checksum_calculated_text),
             "%04X", static_cast<unsigned>(packet_checksum_calculated));
    snprintf(checksum_stored_text, sizeof(checksum_stored_text), "%04X",
             static_cast<unsigned>(packet_checksum_stored));
    json += ",\n  \"packetValid\": ";
    json += packet_valid ? "true" : "false";
    json += ",\n  \"startMarkerValid\": ";
    json += packet_start_valid ? "true" : "false";
    json += ",\n  \"footerMarkerValid\": ";
    json += packet_footer_valid ? "true" : "false";
    json += ",\n  \"checksumValid\": ";
    json += packet_checksum_valid ? "true" : "false";
    json += ",\n  \"checksumCalculated\": \"";
    json += checksum_calculated_text;
    json += "\",\n  \"checksumStored\": \"";
    json += checksum_stored_text;
    json += "\",\n  \"transport\": {\n";
    json += "    \"queueDepth\": ";
    json += String(static_cast<unsigned>(jn100_transport_depth));
    json += ",\n    \"enqueued\": ";
    json += String(static_cast<unsigned long>(jn100_transport_enqueued));
    json += ",\n    \"dequeued\": ";
    json += String(static_cast<unsigned long>(jn100_transport_dequeued));
    json += ",\n    \"overflows\": ";
    json += String(static_cast<unsigned long>(jn100_transport_overflows));
    json += ",\n    \"lastByte\": ";
    json += String(static_cast<unsigned>(jn100_transport_last_byte));
    json += "\n  }";
    json += ",\n  \"packet\": [";
    for (uint16_t i = 0; i < packet_length; ++i) {
      if (i != 0) json += ',';
      if ((i % 16) == 0) json += "\n    ";
      char byte_text[5];
      snprintf(byte_text, sizeof(byte_text), "\"%02X\"",
               static_cast<unsigned>(jn100_packet[i]));
      json += byte_text;
    }
    if (packet_length != 0) json += '\n';
    json += "  ],\n  \"events\": [";
    for (uint32_t i = 0; i < event_count; ++i) {
      if (i != 0) json += ',';
      if ((i % 12) == 0) json += "\n    ";
      const uint32_t index = (first_event + i) % JN100_EVENT_LOG_SIZE;
      char event_text[6];
      snprintf(event_text, sizeof(event_text), "\"%c%02X\"",
               jn100_event_internal_clock[index] ? 'I' : 'E',
               static_cast<unsigned>(jn100_event_bytes[index]));
      json += event_text;
    }
    if (event_count != 0) json += '\n';
    json += "  ]\n}";
    req->send(200, "application/json", json);
  });

  server.on("/api/roms", HTTP_GET, [](AsyncWebServerRequest *req) {
    String json = "{\"roms\":[";
    bool first = true;
    File dir = LittleFS.open(ROMS_DIR);
    if (dir && dir.isDirectory()) {
      File f = dir.openNextFile();
      while (f) {
        String name = f.name();
        // Some FS stacks return full path
        int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        if (!name.startsWith(".") && !name.startsWith("_") &&
            name.endsWith(".gb")) {
          if (!first) json += ",";
          first = false;
          String esc;
          jsonEscape(name, esc);
          String sav = String(SAVES_DIR) + "/" + saveIdFromRomId(name);
          json += "{\"name\":\"";
          json += esc;
          json += "\",\"size\":";
          json += String(static_cast<unsigned>(f.size()));
          json += ",\"active\":";
          json += (name == current_rom_id) ? "true" : "false";
          json += ",\"hasSave\":";
          json += LittleFS.exists(sav.c_str()) ? "true" : "false";
          json += "}";
        }
        f.close();
        f = dir.openNextFile();
      }
      dir.close();
    }
    json += "],";
    appendStorageJson(json);
    json += ",\"active\":\"";
    String escActive;
    jsonEscape(current_rom_id, escActive);
    json += escActive;
    json += "\"}";
    req->send(200, "application/json", json);
  });

  server.on("/api/roms/select", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!req->hasParam("name", true)) {
      req->send(400, "application/json",
                "{\"ok\":false,\"error\":\"Missing name\"}");
      return;
    }
    String name = sanitizeRomFilename(req->getParam("name", true)->value());
    String path = String(ROMS_DIR) + "/" + name;
    if (!LittleFS.exists(path.c_str())) {
      req->send(404, "application/json",
                "{\"ok\":false,\"error\":\"ROM not in library\"}");
      return;
    }
    flushSaveToFs();
    writeCurrentRomId(name);
    req->send(200, "application/json",
              "{\"ok\":true,\"active\":\"" + name + "\",\"rebooting\":true}");
    reboot_pending = true;
  });

  server.on("/api/roms", HTTP_DELETE, [](AsyncWebServerRequest *req) {
    if (!req->hasParam("name")) {
      req->send(400, "application/json",
                "{\"ok\":false,\"error\":\"Missing name\"}");
      return;
    }
    String name = sanitizeRomFilename(req->getParam("name")->value());
    String path = String(ROMS_DIR) + "/" + name;
    if (!LittleFS.exists(path.c_str())) {
      req->send(404, "application/json",
                "{\"ok\":false,\"error\":\"ROM not found\"}");
      return;
    }

    bool was_active = (name == current_rom_id);
    if (was_active) flushSaveToFs();

    LittleFS.remove(path.c_str());
    String sav = String(SAVES_DIR) + "/" + saveIdFromRomId(name);
    LittleFS.remove(sav.c_str());

    if (was_active) {
      LittleFS.remove(CURRENT_PATH);
      current_rom_id = "";
      current_rom_path[0] = '\0';
      current_save_path[0] = '\0';
      req->send(200, "application/json",
                "{\"ok\":true,\"deleted\":true,\"rebooting\":true}");
      reboot_pending = true;
    } else {
      req->send(200, "application/json", "{\"ok\":true,\"deleted\":true}");
    }
  });

  server.on("/api/save", HTTP_GET, [](AsyncWebServerRequest *req) {
    flushSaveToFs();
    if (!current_save_path[0] || !LittleFS.exists(current_save_path)) {
      req->send(404, "application/json",
                "{\"ok\":false,\"error\":\"No save file\"}");
      return;
    }
    req->send(LittleFS, current_save_path, "application/octet-stream", true);
  });

  server.on(
      "/api/save", HTTP_POST,
      [](AsyncWebServerRequest *req) {
        if (!save_upload_ok || save_upload_bytes == 0) {
          req->send(400, "application/json",
                    "{\"ok\":false,\"error\":\"Save upload failed\"}");
          return;
        }
        req->send(200, "application/json",
                  "{\"ok\":true,\"bytes\":" +
                      String(static_cast<unsigned>(save_upload_bytes)) +
                      ",\"rebooting\":true}");
        reboot_pending = true;
      },
      handleSaveUpload);

  server.on("/api/save", HTTP_DELETE, [](AsyncWebServerRequest *req) {
    if (current_save_path[0]) LittleFS.remove(current_save_path);
    if (priv.cart_ram && priv.cart_ram_size > 0) {
      memset(priv.cart_ram, 0, priv.cart_ram_size);
    }
    save_dirty = false;
    req->send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
    reboot_pending = true;
  });

  server.on(
      "/api/rom", HTTP_POST,
      [](AsyncWebServerRequest *req) {
        uploadActive = false;
        if (!uploadOk || uploadBytes == 0) {
          String err = uploadError.length() ? uploadError
                                            : "Upload failed or invalid ROM";
          String esc;
          jsonEscape(err, esc);
          req->send(400, "application/json",
                    "{\"ok\":false,\"error\":\"" + esc + "\",\"bytes\":" +
                        String(static_cast<unsigned>(uploadBytes)) + "}");
          return;
        }
        flushSaveToFs();
        String escName;
        jsonEscape(uploadOrigName, escName);
        req->send(200, "application/json",
                  "{\"ok\":true,\"bytes\":" +
                      String(static_cast<unsigned>(uploadBytes)) +
                      ",\"name\":\"" + escName + "\",\"rebooting\":true}");
        Serial.println("[ROM] reboot to load library cartridge");
        reboot_pending = true;
      },
      handleRomUpload);

  server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (uploadActive) {
      req->send(409, "application/json",
                "{\"ok\":false,\"error\":\"Upload in progress\"}");
      return;
    }
    flushSaveToFs();
    Serial.println("[SYS] reboot requested from UI");
    req->send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
    reboot_pending = true;
  });

  server.serveStatic("/", LittleFS, "/")
      .setDefaultFile("index.html")
      .setCacheControl("no-cache");

  // Captive probes get a link to the live UI IP; unknown paths on that IP
  // still fall back to the real UI.
  server.onNotFound([](AsyncWebServerRequest *req) {
    const String host = req->host();
    if (host.length() && !hostIsOurWebUi(host, webUiIpForRequest(req))) {
      sendCaptivePortalPage(req);
      return;
    }
    req->send(LittleFS, "/index.html", "text/html");
  });

  server.begin();
  Serial.println("[HTTP] server started");
}

// ---------------------------------------------------------------------------
// ROM / emulator init
// ---------------------------------------------------------------------------
static void *ps_alloc(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = malloc(n);
  return p;
}

static bool loadRomFromFs(const char *path) {
  File f = LittleFS.open(path, "r");
  if (!f) {
    Serial.printf("[ROM] missing %s — place a .gb ROM in the LittleFS data folder\n",
                  path);
    return false;
  }

  size_t sz = f.size();
  uint8_t *buf = static_cast<uint8_t *>(ps_alloc(sz));
  if (!buf) {
    Serial.println("[ROM] alloc failed");
    f.close();
    return false;
  }

  size_t got = f.read(buf, sz);
  f.close();
  if (got != sz) {
    Serial.println("[ROM] short read");
    free(buf);
    return false;
  }

  priv.rom = buf;
  priv.rom_size = sz;
  Serial.printf("[ROM] loaded %u bytes from %s\n", static_cast<unsigned>(sz),
                path);
  return true;
}

static bool initEmulator() {
  if (!current_rom_path[0] || !LittleFS.exists(current_rom_path)) {
    Serial.println("[ROM] no active cartridge in library");
    return false;
  }
  if (!loadRomFromFs(current_rom_path)) return false;

  enum gb_init_error_e err =
      gb_init(&gb, gb_rom_read, gb_cart_ram_read, gb_cart_ram_write, gb_error,
              &priv);
  if (err != GB_INIT_NO_ERROR) {
    Serial.printf("[GB] gb_init failed: %d\n", static_cast<int>(err));
    return false;
  }

  gb_init_serial(&gb, jn100_serial_tx, jn100_serial_rx);

  size_t save_size = 0;
  if (gb_get_save_size_s(&gb, &save_size) != 0) {
    save_size = 0;
  }
  if (save_size > 0) {
    priv.cart_ram = static_cast<uint8_t *>(ps_alloc(save_size));
    if (!priv.cart_ram) {
      Serial.println("[GB] cart RAM alloc failed");
      return false;
    }
    memset(priv.cart_ram, 0, save_size);
    priv.cart_ram_size = save_size;
    Serial.printf("[GB] cart RAM %u bytes\n", static_cast<unsigned>(save_size));
    loadSaveFromFs();
  }

  gb_init_lcd(&gb, lcd_draw_line);
  minigb_apu_audio_init(&apu);
  audio_sample_accum = 0;
  audio_pcm_len = 0;
  gb.direct.joypad = 0xFF;
  return true;
}

static bool allocFrameBuffers() {
  video_draw = static_cast<uint8_t *>(ps_alloc(VIDEO_BYTES));
  video_send = static_cast<uint8_t *>(ps_alloc(VIDEO_BYTES));
  // Header(1) + audioLen(2) + max audio (~550) + video
  frame_packet_cap = 3 + 1024 + VIDEO_BYTES;
  frame_packet = static_cast<uint8_t *>(ps_alloc(frame_packet_cap));
  if (!video_draw || !video_send || !frame_packet) {
    Serial.println("[MEM] frame buffer alloc failed");
    return false;
  }
  memset(video_draw, 0, VIDEO_BYTES);
  memset(video_send, 0, VIDEO_BYTES);
  return true;
}

/** Samples for this VBlank: exact long-term rate is rate*70224/4194304. */
static uint16_t audioSamplesForFrame() {
  audio_sample_accum += static_cast<uint64_t>(AUDIO_SAMPLE_RATE) * 70224u;
  const uint16_t n =
      static_cast<uint16_t>(audio_sample_accum / 4194304u);
  audio_sample_accum %= 4194304u;
  if (n < 1) return 1;
  if (n > AUDIO_SAMPLES_MAX) return AUDIO_SAMPLES_MAX;
  return n;
}

/** Convert one VBlank of stereo S16 APU output into mono U8 PCM. */
static void captureAudioFrame() {
  const uint16_t n = audioSamplesForFrame();
  audio_sample_t stereo[AUDIO_SAMPLES_TOTAL];
  minigb_apu_audio_callback(&apu, stereo, n);
  for (uint16_t i = 0; i < n; i++) {
    int32_t mixed = (static_cast<int32_t>(stereo[i * 2]) +
                     static_cast<int32_t>(stereo[i * 2 + 1])) /
                    2;
    int32_t u8 = (mixed / 256) + 128;
    if (u8 < 0) u8 = 0;
    if (u8 > 255) u8 = 255;
    audio_pcm[i] = static_cast<uint8_t>(u8);
  }
  audio_pcm_len = n;
}

/** Build frame packet from latest PCM + packed video. */
static size_t buildFramePacket() {
  const uint16_t audio_len = audio_pcm_len;
  size_t need = 3 + audio_len + VIDEO_BYTES;
  if (need > frame_packet_cap) return 0;

  frame_packet[0] = PKT_FRAME;
  frame_packet[1] = static_cast<uint8_t>(audio_len & 0xFF);
  frame_packet[2] = static_cast<uint8_t>((audio_len >> 8) & 0xFF);
  if (audio_len > 0) {
    memcpy(frame_packet + 3, audio_pcm, audio_len);
  }

  if (xSemaphoreTake(frame_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    memcpy(frame_packet + 3 + audio_len, video_send, VIDEO_BYTES);
    xSemaphoreGive(frame_mutex);
  } else {
    memset(frame_packet + 3 + audio_len, 0, VIDEO_BYTES);
  }
  return need;
}

// ---------------------------------------------------------------------------
// Tasks
// ---------------------------------------------------------------------------
static void emulationTask(void *arg) {
  (void)arg;
  // True DMG refresh: 4194304 Hz / 70224 ~= 59.7275 Hz = 16743 us
  // (pdMS_TO_TICKS(16) was ~62.5 Hz and flooded the client jitter buffer)
  static const int64_t FRAME_PERIOD_US = 16743;
  int64_t next_frame_us = esp_timer_get_time();

  Serial.println("[EMU] task on Core 1");
  while (true) {
    if (!emu_running || uploadActive) {
      vTaskDelay(pdMS_TO_TICKS(20));
      next_frame_us = esp_timer_get_time();
      gb.direct.frame_skip = false;
      continue;
    }

    uint8_t mask;
    portENTER_CRITICAL(&joy_mux);
    mask = joypad_mask;
    portEXIT_CRITICAL(&joy_mux);
    gb.direct.joypad = mask_to_peanut(mask);

    // If already late entering this frame, skip LCD draw (CPU/APU still run).
    // Keeps Pokémon dialogue / logic at full speed instead of slow-mo.
    gb.direct.frame_skip = (esp_timer_get_time() > next_frame_us);

    const int64_t frame_start_us = esp_timer_get_time();
    gb_run_frame(&gb);
    // Always tick APU once per frame (even if WS is congested / drops video)
    captureAudioFrame();
    const int64_t frame_cost_us = esp_timer_get_time() - frame_start_us;

    // Publish draw buffer for the stream task
    if (xSemaphoreTake(frame_mutex, portMAX_DELAY) == pdTRUE) {
      memcpy(video_send, video_draw, VIDEO_BYTES);
      frame_ready = true;
      xSemaphoreGive(frame_mutex);
    }

    next_frame_us += FRAME_PERIOD_US;
    for (;;) {
      const int64_t now = esp_timer_get_time();
      const int64_t delay_us = next_frame_us - now;
      if (delay_us <= 0) {
        // Still behind after a heavy frame — keep skipping draws next loop
        gb.direct.frame_skip = true;
        // Fell more than ~3 frames behind — resync instead of spiral
        if (delay_us < -(FRAME_PERIOD_US * 3)) {
          next_frame_us = now;
        }
        break;
      }
      // Comfortable budget again → full LCD next frame
      // (also disable skip if this frame was cheap even if we entered late)
      if (frame_cost_us < 14000) {
        gb.direct.frame_skip = false;
      }
      if (delay_us >= 2000) {
        vTaskDelay(1); // ~1 ms tick; loop for the remainder
      } else {
        taskYIELD();
      }
    }
  }
}

static void streamTask(void *arg) {
  (void)arg;
  Serial.println("[STREAM] task on Core 0");
  while (true) {
    bool ready = false;
    if (emu_running && ws.count() > 0 &&
        xSemaphoreTake(frame_mutex, pdMS_TO_TICKS(2)) == pdTRUE) {
      ready = frame_ready;
      xSemaphoreGive(frame_mutex);
    }

    // Only consume the frame once the socket can take it — avoids dropping
    // APU ticks / frames when TCP is briefly congested.
    if (ready && ws.availableForWriteAll()) {
      if (xSemaphoreTake(frame_mutex, pdMS_TO_TICKS(2)) == pdTRUE) {
        if (frame_ready) {
          frame_ready = false;
          xSemaphoreGive(frame_mutex);
          size_t len = buildFramePacket();
          if (len > 0) {
            ws.binaryAll(frame_packet, len);
          }
        } else {
          xSemaphoreGive(frame_mutex);
        }
      }
    }
    ws.cleanupClients();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

// ---------------------------------------------------------------------------
// Arduino entry
// ---------------------------------------------------------------------------
void setup() {
  // Dedicate clocks to the app: 240 MHz CPU (S3 default, force in case of brownout scaling)
  setCpuFrequencyMhz(240);

  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== ESP-GameBoy ===");
  Serial.printf("[SYS] CPU %u MHz · free heap %u · free PSRAM %u\n",
                static_cast<unsigned>(getCpuFrequencyMhz()),
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getFreePsram()));

  if (!LittleFS.begin(true)) {
    Serial.println("[FS] LittleFS mount failed");
  } else {
    Serial.println("[FS] LittleFS mounted");
    migrateLegacyLibrary();
    Serial.printf("[FS] %u used / %u total (%u free)\n",
                  static_cast<unsigned>(LittleFS.usedBytes()),
                  static_cast<unsigned>(LittleFS.totalBytes()),
                  static_cast<unsigned>(fsFreeBytes()));
  }

  if (!allocFrameBuffers()) {
    Serial.println("Fatal: no frame buffers");
    return;
  }

  frame_mutex = xSemaphoreCreateMutex();
  if (!frame_mutex) {
    Serial.println("Fatal: frame mutex");
    return;
  }

  setupWifiAp();
  linkRadioBegin();
  setupWebServer();

  // Always create emu task; it idles until a ROM is present / upload finishes
  xTaskCreatePinnedToCore(emulationTask, "emu", 16384, nullptr, 2, nullptr, 1);

  if (initEmulator()) {
    emu_running = true;
  } else {
    Serial.println("[EMU] idle — upload a ROM via http://192.168.4.1/");
  }

  // Network / stream pump on Core 0 (Wi-Fi stack affinity)
  xTaskCreatePinnedToCore(streamTask, "stream", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
  dnsServer.processNextRequest();

  if (reboot_pending) {
    flushSaveToFs();
    delay(400); // let HTTP response flush
    ESP.restart();
  }

  uint32_t now = millis();
  if (save_dirty && (now - last_autosave_ms) >= SAVE_AUTOSAVE_MS) {
    last_autosave_ms = now;
    flushSaveToFs();
  }

  // Keep DNS snappy for captive-portal probes; autosave still throttled above.
  vTaskDelay(pdMS_TO_TICKS(20));
}

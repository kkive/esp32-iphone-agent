// HID bridge: an ESP32-S3 (XIAO, 8 MB PSRAM) plugged into the iPhone. It is the phone's
// USB mouse + keyboard, an AirPlay screen-mirroring receiver, and a BLE peripheral for the
// iOS app on the same phone. It holds no logic; the app decides. Protocol: docs/ble-protocol.md.
//
// HID commands (unchanged from the old TCP protocol, the board answers "ok" or "err <why>"):
//   ping | home | move <dx> <dy> | press | release | click
//   scroll <v> <h> | type <ascii text> | key <combo, e.g. cmd+h, enter>
//   homescreen (the consumer-control Home key, "AC Home": iOS goes to the Home Screen, like a keyboard's home key)

#include <esp_task_wdt.h>
#include <esp_mac.h>
#include "USB.h"
#include "USBHIDMouse.h"
#include "USBHIDKeyboard.h"
#include "USBHIDConsumerControl.h"
#include "tusb.h"
#include "ble_link.h"
#include "ble_hid.h"
#include "netcfg.h"
#include "video_link.h"
#include "wifi_link.h"
#include "src/airplay/airplay.h"

const int STEP = 10;      // max pointer delta per report while moving
const int STEP_MS = 8;    // report interval; constant pace keeps iOS acceleration predictable
const int HOME_REPORTS = 50;
#ifndef DEBUG_SERIAL
#define DEBUG_SERIAL 0  // 1: echo commands and the AirPlay handshake over USB serial
#endif

const int WATCHDOG_MS = 8000;  // a hung loop (e.g. blocked USB) reboots the board instead of staying offline

USBHIDMouse mouse;
USBHIDKeyboard keyboard;
USBHIDConsumerControl consumer;
const uint16_t AC_HOME = 0x0223;  // HID consumer page: AC Home
static String receiverName = "ESP32 Agent";

// Where HID reports go: "usb" (the phone's port), "ble" (Bluetooth, no cable) or "auto" (default):
// auto prefers USB when a USB host is present and falls back to Bluetooth when the board runs on
// battery power. Not persisted: every boot starts on auto, like `coex`.
static String hidMode = "auto";
static uint8_t bleButtons = 0;  // held mouse buttons while in ble mode

static bool bleHid() {
  if (hidMode == "ble") return true;
  if (hidMode == "usb") return false;
  return !tud_mounted();  // auto: no USB host (battery power) -> Bluetooth
}

// --- HID output, USB or BLE. The USB objects stay alive in ble mode; the phone just sees no input.
static void hidMove(int dx, int dy) {
  if (bleHid()) ble_hid_mouse(bleButtons, constrain(dx, -127, 127), constrain(dy, -127, 127), 0, 0);
  else mouse.move(dx, dy);
}
static void hidScroll(int v, int h) {
  if (bleHid()) ble_hid_mouse(bleButtons, 0, 0, constrain(v, -127, 127), constrain(h, -127, 127));
  else mouse.move(0, 0, v, h);
}
static void hidPress() {
  if (bleHid()) { bleButtons |= 1; ble_hid_mouse(bleButtons, 0, 0, 0, 0); }
  else mouse.press(MOUSE_LEFT);
}
static void hidRelease() {
  if (bleHid()) { bleButtons &= ~1; ble_hid_mouse(bleButtons, 0, 0, 0, 0); }
  else mouse.release(MOUSE_LEFT);
}
static void hidReleaseAll() {
  if (bleHid()) { bleButtons = 0; const uint8_t none[6] = {0}; ble_hid_keys(0, none); }
  else keyboard.releaseAll();
}
static void bleKeys(uint8_t mods, uint8_t a, uint8_t b = 0) {
  const uint8_t k[6] = {a, b, 0, 0, 0, 0};
  ble_hid_keys(mods, k);
}

// US keyboard mapping. Low byte is the HID keyboard usage; bit 8 means Left Shift.
// The Arduino USB keyboard class translates ASCII internally, while BLE reports require
// raw HID usages, so the BLE backend must do this conversion explicitly.
static uint16_t asciiHid(uint8_t c) {
  if (c >= 'a' && c <= 'z') return 0x04 + c - 'a';
  if (c >= 'A' && c <= 'Z') return 0x100 | (0x04 + c - 'A');
  if (c >= '1' && c <= '9') return 0x1E + c - '1';
  if (c == '0') return 0x27;
  if (c == '\n' || c == '\r') return 0x28;
  if (c == '\b') return 0x2A;
  if (c == '\t') return 0x2B;
  if (c == ' ') return 0x2C;
  if (c == 0x1B) return 0x29;
  if (c == 0x7F) return 0x4C;
  const char *plain = "-=[]\\;',./`";
  const char *shift = "_+{}|:\"<>?~";
  const uint8_t usages[] = {0x2D,0x2E,0x2F,0x30,0x31,0x33,0x34,0x36,0x37,0x38,0x35};
  for (size_t i = 0; i < sizeof usages; i++) {
    if (c == (uint8_t)plain[i]) return usages[i];
    if (c == (uint8_t)shift[i]) return 0x100 | usages[i];
  }
  const char *digits = "!@#$%^&*()";
  for (int i = 0; i < 10; i++) if (c == (uint8_t)digits[i]) return 0x100 | (0x1E + i);
  return 0;
}

static bool hidTypeChar(uint8_t c) {
  if (bleHid()) {
    uint16_t key = asciiHid(c);
    if (!key) return false;
    bleKeys(key & 0x100 ? 2 : 0, (uint8_t)key);
    delay(8);
    bleKeys(0, 0);
    delay(8);
  } else {
    keyboard.press(c);
    delay(5);
    keyboard.release(c);
  }
  return true;
}
static void hidConsumer(uint16_t usage) {
  if (bleHid()) { ble_hid_consumer(usage); delay(40); ble_hid_consumer(0); }
  else { consumer.press(usage); delay(40); consumer.release(); }
}

void movePaced(long dx, long dy) {
  while (dx || dy) {
    int sx = constrain(dx, -STEP, STEP), sy = constrain(dy, -STEP, STEP);
    hidMove(sx, sy);
    dx -= sx;
    dy -= sy;
    delay(STEP_MS);
  }
}

uint8_t keyCode(const String &k) {
  if (k == "enter") return KEY_RETURN;
  if (k == "esc") return KEY_ESC;
  if (k == "tab") return KEY_TAB;
  if (k == "backspace") return KEY_BACKSPACE;
  if (k == "space") return ' ';
  if (k == "up") return KEY_UP_ARROW;
  if (k == "down") return KEY_DOWN_ARROW;
  if (k == "left") return KEY_LEFT_ARROW;
  if (k == "right") return KEY_RIGHT_ARROW;
  if (k == "cmd") return KEY_LEFT_GUI;
  if (k == "ctrl") return KEY_LEFT_CTRL;
  if (k == "alt") return KEY_LEFT_ALT;
  if (k == "shift") return KEY_LEFT_SHIFT;
  return k.length() == 1 ? k[0] : 0;
}

bool pressCombo(String combo) {
  combo += '+';
  if (bleHid()) {  // assemble modifiers + slots, send once, release
    uint8_t mods = 0, slots[6] = {0};
    int n = 0;
    for (int i; (i = combo.indexOf('+')) >= 0; combo = combo.substring(i + 1)) {
      String token = combo.substring(0, i);
      uint16_t key = 0;
      if (token == "cmd") mods |= 8;
      else if (token == "ctrl") mods |= 1;
      else if (token == "alt") mods |= 4;
      else if (token == "shift") mods |= 2;
      else if (token == "enter") key = 0x28;
      else if (token == "esc") key = 0x29;
      else if (token == "tab") key = 0x2B;
      else if (token == "backspace") key = 0x2A;
      else if (token == "space") key = 0x2C;
      else if (token == "up") key = 0x52;
      else if (token == "down") key = 0x51;
      else if (token == "left") key = 0x50;
      else if (token == "right") key = 0x4F;
      else if (token.length() == 1) {
        key = asciiHid((uint8_t)token[0]);
        if (!key) { ble_hid_keys(0, slots); return false; }
      }
      else { ble_hid_keys(0, slots); return false; }
      if (key) {
        if (key & 0x100) mods |= 2;
        uint8_t usage = (uint8_t)key;
        bool duplicate = false;
        for (int j = 0; j < n; j++) if (slots[j] == usage) duplicate = true;
        if (!duplicate) {
          if (n >= 6) { ble_hid_keys(0, slots); return false; }
          slots[n++] = usage;
        }
      }
    }
    ble_hid_keys(mods, slots);
    delay(30);
    const uint8_t none[6] = {0};
    ble_hid_keys(0, none);
    return true;
  }
  for (int i; (i = combo.indexOf('+')) >= 0; combo = combo.substring(i + 1)) {
    uint8_t c = keyCode(combo.substring(0, i));
    if (!c) { keyboard.releaseAll(); return false; }
    keyboard.press(c);
  }
  delay(30);
  keyboard.releaseAll();
  return true;
}

// A locked or sleeping phone suspends USB; sending HID reports then can stall. Ask it to wake up instead,
// and report "asleep" right away if it does not, so the board never hangs. In ble mode the check is
// whether a host's HID stack has subscribed to the input reports (nothing to wake: no USB).
bool usbReady() {
  if (bleHid()) return ble_hid_connected();
  if (tud_suspended()) {
    tud_remote_wakeup();
    for (int i = 0; i < 50 && tud_suspended(); i++) delay(10);
  }
  return tud_mounted() && !tud_suspended();
}

String run(const String &line) {
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String arg = sp < 0 ? "" : line.substring(sp + 1);
  long a = 0, b = 0;
  sscanf(arg.c_str(), "%ld %ld", &a, &b);

  if (cmd == "ping") return "ok";
  if (cmd == "hid") {  // auto | usb | ble: where HID reports go (not persisted, like coex)
    if (arg != "usb" && arg != "ble" && arg != "auto") return "err usage: hid auto|usb|ble";
    hidReleaseAll();
    hidMode = arg;
    return String("ok ") + hidMode + (bleHid() && !ble_hid_connected() ? " (no bluetooth hid host yet)" : "");
  }
  if (!usbReady()) return bleHid() ? "err no bluetooth hid host (pair the board)" : "err phone usb asleep (unlock the phone)";
  if (cmd == "home") {  // slam the pointer into the top-left corner
    for (int i = 0; i < HOME_REPORTS; i++) { hidMove(-127, -127); delay(2); }
    return "ok";
  }
  if (cmd == "move") { movePaced(a, b); return "ok"; }
  if (cmd == "press") { hidPress(); return "ok"; }
  if (cmd == "release") { hidRelease(); return "ok"; }
  if (cmd == "click") { hidPress(); delay(60); hidRelease(); return "ok"; }
  if (cmd == "scroll") { hidScroll(a, b); return "ok"; }
  if (cmd == "type") {
    if (bleHid()) for (unsigned i = 0; i < arg.length(); i++) if (!asciiHid((uint8_t)arg[i])) return "err type supports ASCII only";
    for (unsigned i = 0; i < arg.length(); i++) if (!hidTypeChar((uint8_t)arg[i])) return "err type supports ASCII only";
    return "ok";
  }
  if (cmd == "key") return pressCombo(arg) ? "ok" : "err unknown key";
  if (cmd == "homescreen") { hidConsumer(AC_HOME); return "ok"; }
  return "err unknown command";
}

const char *resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_TASK_WDT: case ESP_RST_INT_WDT: case ESP_RST_WDT: return "watchdog";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_SW: return "software";
    case ESP_RST_USB: return "usb";
    default: return "other";
  }
}

const char *usbState() {
  if (!tud_mounted()) return "off";
  return tud_suspended() ? "asleep" : "ok";
}

// Compatibility for the existing iOS app: its readiness checks still read the `usb` field.
// In BLE mode report the effective input channel there, while `hid` exposes the selected mode.
const char *inputState() {
  if (bleHid()) return ble_hid_connected() ? "ok" : "off";
  return usbState();
}

// Board-level commands from the app; everything else is an HID command for run().
String board(const String &line, const String &id) {
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String arg = sp < 0 ? "" : line.substring(sp + 1);
  long a = 0, b = 0;
  int n = sscanf(arg.c_str(), "%ld %ld", &a, &b);
  if (cmd == "stream") {
    if (arg != "on" && arg != "off") return "err usage: stream on|off";
    video_stream(arg == "on");
    return "ok";
  }
  if (cmd == "app" || cmd == "log") {  // debug relay: Mac -> app ("app run ...") and app -> Mac ("log ...")
    ble_reply(cmd + " " + arg);         // every peer sees it; each side ignores lines it doesn't handle
    return "ok";
  }
  if (cmd == "snap") { video_snap(id); return ""; }  // replies when the GOP is sent
  if (cmd == "bench") {
    if (n < 1 || a <= 0 || a > 4096) return "err usage: bench KB (1-4096)";
    video_bench(id, a);
    return "";
  }
  if (cmd == "res") {
    if (n < 2 || a < 160 || b < 160 || a > 1920 || b > 2560) return "err usage: res W H";
    cfg_set_res(a, b);
    airplay_set_display(a, b, cfg_fps());
    return "ok";
  }
  if (cmd == "fps") {
    if (n < 1 || a < 1 || a > 60) return "err usage: fps N (1-60)";
    cfg_set_fps(a);
    airplay_set_display(cfg_width(), cfg_height(), a);
    return "ok";
  }
  if (cmd == "coex") {  // A/B knob, not stored: every boot starts at balance
    if (arg.length() && !net_coex(arg)) return "err usage: coex bt|balance|wifi";
    return String("ok ") + net_coex_mode();
  }
  if (cmd == "unpair") {
    ble_reply(id + " ok");  // answer before the link goes down
    ble_unpair();
    return "";
  }
  return run(line);
}

String statusJson(bool full) {
  static const char *apStates[] = {"idle", "connected", "streaming"};
  VideoStats v = video_stats();
  WiFiLinkStats wl = wifi_link_stats();
  char s[512];
  snprintf(s, sizeof s,
           "{\"up\":%lu,\"rst\":\"%s\",\"heap\":%u,\"psram\":%u,\"usb\":\"%s\",\"hid\":\"%s\",\"net\":\"%s\",\"ip\":\"%s\",\"sta\":%d,"
           "\"ap\":\"%s\",\"w\":%u,\"h\":%u,\"kbps\":%lu,\"fps\":%.1f,\"idr\":%lu,\"gop\":%lu,"
           "\"ble\":{\"mtu\":%u,\"phy\":%u,\"kbps\":%lu,\"drop\":%lu,\"rdrop\":%lu,\"n\":%d},"
           "\"wifi\":{\"up\":%d,\"auth\":%d,\"kbps\":%lu,\"err\":%lu,\"autherr\":%lu}}",
           millis(), resetReason(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), inputState(), hidMode.c_str(), net_mode(), net_ip().c_str(), net_peers(),
           apStates[airplay_state()], cfg_width(), cfg_height(), (unsigned long)v.kbps, v.fps, (unsigned long)v.idr_ms,
           (unsigned long)v.gop_bytes, ble_mtu(), ble_phy(), (unsigned long)ble_kbps(), (unsigned long)ble_drops(),
           (unsigned long)ble_reply_drops(), ble_peers(), wl.connected, wl.authenticated, (unsigned long)wl.kbps,
           (unsigned long)wl.errors, (unsigned long)wl.auth_failures);
  if (full) return s;
  // MTU below ~250 (before the MTU exchange): keep only the fields the app needs to stay alive
  snprintf(s, sizeof s, "{\"up\":%lu,\"rst\":\"%s\",\"usb\":\"%s\",\"net\":\"%s\",\"ap\":\"%s\",\"ble\":{\"mtu\":%u}}",
           millis(), resetReason(), inputState(), net_mode(), apStates[airplay_state()], ble_mtu());
  return s;
}

void statusTask(void *) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    String s = statusJson(true);
    wifi_link_status(s);
    if (ble_connected()) {
      if (s.length() > ble_mtu() - 3u) s = statusJson(false);
      ble_status(s);
    }
  }
}

void logLine(const char *msg) {
#if DEBUG_SERIAL
  Serial.printf("[%lu] %s\n", millis(), msg);
#endif
}

void startAirplay() {
  static airplay_cfg cfg;
  cfg.name = receiverName.c_str();
  esp_read_mac(cfg.hwaddr, ESP_MAC_WIFI_STA);
  cfg_ed25519_seed(cfg.ed25519_seed);
  cfg.width = cfg_width();
  cfg.height = cfg_height();
  cfg.max_fps = cfg_fps();
  cfg.on_config = video_on_config;
  cfg.on_au = video_on_au;
  cfg.log = logLine;
  if (!airplay_start(&cfg)) Serial.println("airplay: start failed");
}

void setup() {
  Serial.begin(115200);
#if DEBUG_SERIAL
  for (int i = 0; i < 30 && !Serial; i++) delay(100);  // give a serial monitor 3 s to catch the boot lines
#endif
  mouse.begin();
  keyboard.begin();
  consumer.begin();
  USB.productName("HID Bridge");
  USB.begin();
  esp_task_wdt_config_t wdt = {.timeout_ms = WATCHDOG_MS, .idle_core_mask = 0, .trigger_panic = true};
  esp_task_wdt_reconfigure(&wdt);
  esp_task_wdt_add(NULL);

  net_begin();
  if (!video_begin()) Serial.println("video: no PSRAM for the GOP buffer");
  if (!wifi_link_begin()) Serial.println("wifi-link: start failed");
  ble_begin("ESP32 Agent " + board_suffix());
  startAirplay();
  xTaskCreatePinnedToCore(statusTask, "status", 4096, NULL, 1, NULL, 1);
  Serial.printf("boot %s: free internal heap %u (largest %u), PSRAM %u, reset %s\n", receiverName.c_str(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), resetReason());
}

void loop() {
  static bool linked = false;
  esp_task_wdt_reset();
  net_poll();
  if (linked && !ble_connected()) {
    hidRelease();  // a gesture cut off mid-way must not leave the button held (long press)
    hidReleaseAll();
    video_stream(false);
  }
  linked = ble_connected();

  String line;
  if (Serial.available()) {  // USB serial takes the same `<id> <command>` lines, for debugging from a Mac
    line = Serial.readStringUntil('\n');
    line.trim();
    if (line.startsWith("0 net ")) { net_request(line.substring(6)); Serial.println("0 ok"); return; }
    int sp = line.indexOf(' ');
    String id = sp < 0 ? line : line.substring(0, sp);
    Serial.println(id + " " + board(sp < 0 ? "" : line.substring(sp + 1), id));
    return;
  }
  uint32_t wifiId = 0;
  if (wifi_link_pop_cmd(line, wifiId)) {
    String cmd = line;
    cmd.trim();
    int sp = cmd.indexOf(' ');
    String verb = sp < 0 ? cmd : cmd.substring(0, sp);
    String reply;
    // These commands complete later on the BLE video/debug path. Wi-Fi variants arrive in alpha.2.
    if (verb == "snap" || verb == "bench" || verb == "app" || verb == "log" || verb == "unpair")
      reply = "err command pending Wi-Fi implementation";
    else
      reply = board(cmd, String(wifiId));
    if (!reply.length()) reply = "err no reply";
    wifi_link_reply(wifiId, reply);
    return;
  }
  if (!ble_pop_cmd(line, 5)) return;
  int sp = line.indexOf(' ');
  String id = sp < 0 ? line : line.substring(0, sp);
  String rest = sp < 0 ? "" : line.substring(sp + 1);
  rest.trim();
#if DEBUG_SERIAL
  unsigned long started = millis();
#endif
  String reply = id.toInt() || id == "0" ? board(rest, id) : "err missing request id";
  if (reply.length()) ble_reply(id + " " + reply);
#if DEBUG_SERIAL
  Serial.printf("%s -> %s (%lu ms)\n", line.c_str(), reply.c_str(), millis() - started);
#endif
}

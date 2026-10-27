// Authenticated TCP control/status/video transport for the iPhone app.
// Copyright (c) 2026 Henry Yuan. MIT License; see the repository LICENSE.
#include "wifi_link.h"
#include <Network.h>
#include <NetworkClient.h>
#include <NetworkServer.h>
#include "netcfg.h"
#include "src/wifi/wire.h"

static const uint16_t PORT = 8765;
static const size_t CONTROL_PAYLOAD_MAX = 1024;
static const uint8_t VIDEO_FLAG_IDR = 1;

struct CommandItem { uint32_t id; char text[513]; };
struct ReplyItem { uint32_t id; char text[769]; };
struct StatusItem { char text[513]; };

static QueueHandle_t commands, replies, statuses;
static SemaphoreHandle_t txLock;
static NetworkClient client;
static portMUX_TYPE statsMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool statConnected, statAuthed;
static volatile uint32_t statKbps, statErrors, statAuthFailures, statRtt, statWindowBytes;
static uint32_t videoSeq;

static bool tokenEqual(const uint8_t *p, size_t n, const String &expected) {
  size_t diff = n ^ expected.length();
  size_t m = n < expected.length() ? n : expected.length();
  for (size_t i = 0; i < m; i++) diff |= p[i] ^ (uint8_t)expected[i];
  return diff == 0;
}

static void setLink(bool connected, bool authed) {
  portENTER_CRITICAL(&statsMux);
  statConnected = connected;
  statAuthed = authed;
  portEXIT_CRITICAL(&statsMux);
}

static void addBytes(size_t n) {
  portENTER_CRITICAL(&statsMux);
  statWindowBytes += n;
  portEXIT_CRITICAL(&statsMux);
}

static void addError(bool auth = false) {
  portENTER_CRITICAL(&statsMux);
  statErrors++;
  if (auth) statAuthFailures++;
  portEXIT_CRITICAL(&statsMux);
}

// The link task is the only reader. It and the video task serialize all writes and socket replacement here.
static bool writeAllUnlocked(const uint8_t *p, size_t n) {
  while (n && client.connected()) {
    size_t sent = client.write(p, n);
    if (!sent) return false;
    p += sent;
    n -= sent;
    addBytes(sent);
  }
  return n == 0;
}

static bool sendFrameUnlocked(uint8_t type, uint8_t flags, uint32_t seq, uint32_t boardMs,
                              const void *payload, size_t len) {
  iaw_header h = {type, flags, (uint32_t)len, seq, boardMs};
  uint8_t raw[IAW_HEADER_LEN];
  iaw_encode_header(raw, &h);
  return writeAllUnlocked(raw, sizeof raw) && (!len || writeAllUnlocked((const uint8_t *)payload, len));
}

static bool sendFrame(uint8_t type, uint8_t flags, uint32_t seq, uint32_t boardMs,
                      const void *payload, size_t len) {
  xSemaphoreTake(txLock, portMAX_DELAY);
  bool ok = client.connected() && sendFrameUnlocked(type, flags, seq, boardMs, payload, len);
  xSemaphoreGive(txLock);
  if (!ok) addError();
  return ok;
}

static bool sendText(uint8_t type, uint32_t seq, const char *text) {
  return sendFrame(type, 0, seq, millis(), text, strlen(text));
}

static void stopClient() {
  xSemaphoreTake(txLock, portMAX_DELAY);
  if (client) client.stop();
  xSemaphoreGive(txLock);
  setLink(false, false);
}

static void queueCommand(const iaw_header &h, uint8_t *payload) {
  if (!statAuthed) {
    sendText(IAW_ERROR, h.seq, "auth required");
    addError();
    return;
  }
  if (!h.payload_len || h.payload_len > 512) {
    sendText(IAW_REPLY, h.seq, "err command too long");
    return;
  }
  CommandItem item = {h.seq, {0}};
  memcpy(item.text, payload, h.payload_len);
  if (xQueueSend(commands, &item, 0) != pdTRUE) sendText(IAW_REPLY, h.seq, "err busy");
}

static bool dispatch(const iaw_header &h, uint8_t *payload, bool &authed) {
  if (!authed && h.type != IAW_HELLO) {
    sendText(IAW_ERROR, h.seq, "hello required");
    addError();
    return false;
  }
  switch (h.type) {
    case IAW_HELLO:
      if (authed || !tokenEqual(payload, h.payload_len, net_api_token())) {
        sendText(IAW_HELLO_REPLY, h.seq, "err auth");
        addError(true);
        return false;
      }
      authed = true;
      setLink(true, true);
      sendText(IAW_HELLO_REPLY, h.seq, "ok");
      return true;
    case IAW_COMMAND:
      queueCommand(h, payload);
      return true;
    case IAW_PING:
      sendFrame(IAW_PONG, 0, h.seq, millis(), payload, h.payload_len);
      portENTER_CRITICAL(&statsMux);
      statRtt = 0;
      portEXIT_CRITICAL(&statsMux);
      return true;
    default:
      sendText(IAW_ERROR, h.seq, "unsupported message");
      addError();
      return true;
  }
}

static void linkTask(void *) {
  NetworkServer server(PORT, 1);
  server.setNoDelay(true);
  server.begin();
  uint8_t headerRaw[IAW_HEADER_LEN], payload[CONTROL_PAYLOAD_MAX + 1];
  size_t headerHave = 0, payloadHave = 0;
  iaw_header header = {0};
  bool haveHeader = false, authed = false;
  uint32_t acceptedAt = 0, winAt = millis();

  for (;;) {
    if (!client.connected()) {
      stopClient();
      authed = false;
      headerHave = payloadHave = 0;
      haveHeader = false;
      if (net_up() && server.hasClient()) {
        NetworkClient incoming = server.accept();
        if (incoming) {
          incoming.setNoDelay(true);
          incoming.setTimeout(1);
          xSemaphoreTake(txLock, portMAX_DELAY);
          client = incoming;
          xSemaphoreGive(txLock);
          acceptedAt = millis();
          setLink(true, false);
        }
      }
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    if (!net_up() || (!authed && millis() - acceptedAt > 5000)) {
      if (!authed) addError(true);
      stopClient();
      continue;
    }

    while (client.available()) {
      if (!haveHeader) {
        int n = client.read(headerRaw + headerHave, IAW_HEADER_LEN - headerHave);
        if (n <= 0) break;
        headerHave += n;
        addBytes(n);
        if (headerHave < IAW_HEADER_LEN) continue;
        if (!iaw_decode_header(headerRaw, &header) || header.payload_len > CONTROL_PAYLOAD_MAX) {
          addError();
          stopClient();
          break;
        }
        haveHeader = true;
        payloadHave = 0;
      }
      if (header.payload_len) {
        int n = client.read(payload + payloadHave, header.payload_len - payloadHave);
        if (n <= 0) break;
        payloadHave += n;
        addBytes(n);
        if (payloadHave < header.payload_len) continue;
      }
      payload[header.payload_len] = 0;
      bool keep = dispatch(header, payload, authed);
      headerHave = payloadHave = 0;
      haveHeader = false;
      if (!keep) stopClient();
    }

    if (authed && client.connected()) {
      ReplyItem reply;
      while (xQueueReceive(replies, &reply, 0) == pdTRUE) sendText(IAW_REPLY, reply.id, reply.text);
      StatusItem status;
      if (xQueueReceive(statuses, &status, 0) == pdTRUE) sendText(IAW_STATUS, 0, status.text);
    }

    uint32_t now = millis();
    if (now - winAt >= 1000) {
      portENTER_CRITICAL(&statsMux);
      statKbps = statWindowBytes * 8 / (now - winAt);
      statWindowBytes = 0;
      portEXIT_CRITICAL(&statsMux);
      winAt = now;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

bool wifi_link_begin() {
  commands = xQueueCreate(8, sizeof(CommandItem));
  replies = xQueueCreate(8, sizeof(ReplyItem));
  statuses = xQueueCreate(1, sizeof(StatusItem));
  txLock = xSemaphoreCreateMutex();
  if (!commands || !replies || !statuses || !txLock) return false;
  return xTaskCreatePinnedToCore(linkTask, "wifi-link", 6144, NULL, 2, NULL, 1) == pdPASS;
}

bool wifi_link_pop_cmd(String &command, uint32_t &request_id) {
  CommandItem item;
  if (!commands || xQueueReceive(commands, &item, 0) != pdTRUE) return false;
  request_id = item.id;
  command = item.text;
  return true;
}

void wifi_link_reply(uint32_t request_id, const String &reply) {
  if (!replies) return;
  ReplyItem item = {request_id, {0}};
  strncpy(item.text, reply.c_str(), sizeof item.text - 1);
  xQueueSend(replies, &item, 0);
}

void wifi_link_status(const String &json) {
  if (!statuses) return;
  StatusItem item = {{0}};
  strncpy(item.text, json.c_str(), sizeof item.text - 1);
  xQueueOverwrite(statuses, &item);
}

WiFiLinkStats wifi_link_stats() {
  WiFiLinkStats s;
  portENTER_CRITICAL(&statsMux);
  s = {statConnected, statAuthed, statKbps, statErrors, statAuthFailures, statRtt};
  portEXIT_CRITICAL(&statsMux);
  return s;
}

bool wifi_link_video_ready() {
  portENTER_CRITICAL(&statsMux);
  bool ready = statAuthed;
  portEXIT_CRITICAL(&statsMux);
  return ready;
}

bool wifi_link_send_video(bool config, bool idr, uint32_t boardMs, const uint8_t *data, size_t len) {
  if (!data || !len || len > IAW_MAX_PAYLOAD || !wifi_link_video_ready()) return false;
  xSemaphoreTake(txLock, portMAX_DELAY);
  bool ok = statAuthed && client.connected() &&
            sendFrameUnlocked(config ? IAW_VIDEO_CONFIG : IAW_VIDEO_AU, idr ? VIDEO_FLAG_IDR : 0,
                              videoSeq++, boardMs, data, len);
  if (!ok && client) client.stop();
  xSemaphoreGive(txLock);
  if (!ok) {
    setLink(false, false);
    addError();
  }
  return ok;
}

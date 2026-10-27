// Authenticated TCP control/status transport for the iPhone app.
// Copyright (c) 2026 Henry Yuan. MIT License; see the repository LICENSE.
#include "wifi_link.h"
#include <Network.h>
#include <NetworkClient.h>
#include <NetworkServer.h>
#include "netcfg.h"
#include "src/wifi/wire.h"

static const uint16_t PORT = 8765;
static const size_t CONTROL_PAYLOAD_MAX = 1024;

struct CommandItem {
  uint32_t id;
  char text[513];
};

struct ReplyItem {
  uint32_t id;
  char text[769];
};

struct StatusItem {
  char text[513];
};

static QueueHandle_t commands;
static QueueHandle_t replies;
static QueueHandle_t statuses;
static portMUX_TYPE statsMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool statConnected, statAuthed;
static volatile uint32_t statKbps, statErrors, statAuthFailures, statRtt;

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

static void addError(bool auth = false) {
  portENTER_CRITICAL(&statsMux);
  statErrors++;
  if (auth) statAuthFailures++;
  portEXIT_CRITICAL(&statsMux);
}

static bool writeAll(NetworkClient &client, const uint8_t *p, size_t n, uint32_t *bytes) {
  while (n && client.connected()) {
    size_t sent = client.write(p, n);
    if (!sent) return false;
    p += sent;
    n -= sent;
    *bytes += sent;
  }
  return n == 0;
}

static bool sendFrame(NetworkClient &client, uint8_t type, uint32_t seq, const void *payload, size_t len, uint32_t *bytes) {
  iaw_header h = {type, 0, (uint32_t)len, seq, millis()};
  uint8_t raw[IAW_HEADER_LEN];
  iaw_encode_header(raw, &h);
  return writeAll(client, raw, sizeof raw, bytes) && (!len || writeAll(client, (const uint8_t *)payload, len, bytes));
}

static void sendText(NetworkClient &client, uint8_t type, uint32_t seq, const char *text, uint32_t *bytes) {
  if (!sendFrame(client, type, seq, text, strlen(text), bytes)) addError();
}

static void queueCommand(NetworkClient &client, const iaw_header &h, uint8_t *payload, uint32_t *bytes) {
  if (!statAuthed) {
    sendText(client, IAW_ERROR, h.seq, "auth required", bytes);
    addError();
    return;
  }
  if (!h.payload_len || h.payload_len > 512) {
    sendText(client, IAW_REPLY, h.seq, "err command too long", bytes);
    return;
  }
  CommandItem item = {h.seq, {0}};
  memcpy(item.text, payload, h.payload_len);
  if (xQueueSend(commands, &item, 0) != pdTRUE) sendText(client, IAW_REPLY, h.seq, "err busy", bytes);
}

static bool dispatch(NetworkClient &client, const iaw_header &h, uint8_t *payload, bool &authed, uint32_t &lastPing,
                     uint32_t *bytes) {
  if (!authed && h.type != IAW_HELLO) {
    sendText(client, IAW_ERROR, h.seq, "hello required", bytes);
    addError();
    return false;
  }
  switch (h.type) {
    case IAW_HELLO:
      if (authed || !tokenEqual(payload, h.payload_len, net_api_token())) {
        sendText(client, IAW_HELLO_REPLY, h.seq, "err auth", bytes);
        addError(true);
        return false;
      }
      authed = true;
      setLink(true, true);
      sendText(client, IAW_HELLO_REPLY, h.seq, "ok", bytes);
      return true;
    case IAW_COMMAND:
      queueCommand(client, h, payload, bytes);
      return true;
    case IAW_PING:
      lastPing = millis();
      sendFrame(client, IAW_PONG, h.seq, payload, h.payload_len, bytes);
      portENTER_CRITICAL(&statsMux);
      statRtt = 0;  // board-side turnaround; the app reports the actual round trip
      portEXIT_CRITICAL(&statsMux);
      return true;
    default:
      sendText(client, IAW_ERROR, h.seq, "unsupported message", bytes);
      addError();
      return true;
  }
}

static void linkTask(void *) {
  NetworkServer server(PORT, 1);
  server.setNoDelay(true);
  server.begin();
  NetworkClient client;
  uint8_t headerRaw[IAW_HEADER_LEN], payload[CONTROL_PAYLOAD_MAX + 1];
  size_t headerHave = 0, payloadHave = 0;
  iaw_header header = {0};
  bool haveHeader = false, authed = false;
  uint32_t acceptedAt = 0, lastPing = 0, winAt = millis(), winBytes = 0;

  for (;;) {
    if (!client.connected()) {
      if (client) client.stop();
      setLink(false, false);
      authed = false;
      headerHave = payloadHave = 0;
      haveHeader = false;
      if (net_up() && server.hasClient()) {
        client = server.accept();
        if (client) {
          client.setNoDelay(true);
          client.setTimeout(1);
          acceptedAt = millis();
          setLink(true, false);
        }
      }
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    if (!net_up() || (!authed && millis() - acceptedAt > 5000)) {
      if (!authed) addError(true);
      client.stop();
      continue;
    }

    while (client.available()) {
      if (!haveHeader) {
        int n = client.read(headerRaw + headerHave, IAW_HEADER_LEN - headerHave);
        if (n <= 0) break;
        headerHave += n;
        winBytes += n;
        if (headerHave < IAW_HEADER_LEN) continue;
        if (!iaw_decode_header(headerRaw, &header) || header.payload_len > CONTROL_PAYLOAD_MAX) {
          addError();
          client.stop();
          break;
        }
        haveHeader = true;
        payloadHave = 0;
      }
      if (header.payload_len) {
        int n = client.read(payload + payloadHave, header.payload_len - payloadHave);
        if (n <= 0) break;
        payloadHave += n;
        winBytes += n;
        if (payloadHave < header.payload_len) continue;
      }
      payload[header.payload_len] = 0;
      bool keep = dispatch(client, header, payload, authed, lastPing, &winBytes);
      headerHave = payloadHave = 0;
      haveHeader = false;
      if (!keep) client.stop();
    }

    if (authed) {
      ReplyItem reply;
      while (xQueueReceive(replies, &reply, 0) == pdTRUE)
        sendText(client, IAW_REPLY, reply.id, reply.text, &winBytes);
      StatusItem status;
      if (xQueueReceive(statuses, &status, 0) == pdTRUE)
        sendText(client, IAW_STATUS, 0, status.text, &winBytes);
    }

    uint32_t now = millis();
    if (now - winAt >= 1000) {
      portENTER_CRITICAL(&statsMux);
      statKbps = winBytes * 8 / (now - winAt);
      portEXIT_CRITICAL(&statsMux);
      winBytes = 0;
      winAt = now;
    }
    (void)lastPing;
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

bool wifi_link_begin() {
  commands = xQueueCreate(8, sizeof(CommandItem));
  replies = xQueueCreate(8, sizeof(ReplyItem));
  statuses = xQueueCreate(1, sizeof(StatusItem));
  if (!commands || !replies || !statuses) return false;
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


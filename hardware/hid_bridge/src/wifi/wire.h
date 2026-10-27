// ESP32 iPhone Agent Wi-Fi framing protocol. Pure C so the codec can be host-tested.
// Copyright (c) 2026 Henry Yuan. MIT License; see the repository LICENSE.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IAW_MAGIC "IAW1"
#define IAW_HEADER_LEN 20u
#define IAW_MAX_PAYLOAD (64u * 1024u)

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  IAW_HELLO = 1,
  IAW_HELLO_REPLY = 2,
  IAW_VIDEO_CONFIG = 3,
  IAW_VIDEO_AU = 4,
  IAW_STATUS = 5,
  IAW_COMMAND = 6,
  IAW_REPLY = 7,
  IAW_PING = 8,
  IAW_PONG = 9,
  IAW_ERROR = 10,
} iaw_type;

typedef struct {
  uint8_t type;
  uint8_t flags;
  uint32_t payload_len;
  uint32_t seq;
  uint32_t board_ms;
} iaw_header;

void iaw_encode_header(uint8_t out[IAW_HEADER_LEN], const iaw_header *h);
bool iaw_decode_header(const uint8_t in[IAW_HEADER_LEN], iaw_header *h);

#ifdef __cplusplus
}
#endif


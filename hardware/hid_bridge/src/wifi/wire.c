// Copyright (c) 2026 Henry Yuan. MIT License; see the repository LICENSE.
#include "wire.h"
#include <string.h>

static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }

static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void iaw_encode_header(uint8_t out[IAW_HEADER_LEN], const iaw_header *h) {
  memcpy(out, IAW_MAGIC, 4);
  out[4] = h->type;
  out[5] = h->flags;
  put16(out + 6, IAW_HEADER_LEN);
  put32(out + 8, h->payload_len);
  put32(out + 12, h->seq);
  put32(out + 16, h->board_ms);
}

bool iaw_decode_header(const uint8_t in[IAW_HEADER_LEN], iaw_header *h) {
  if (memcmp(in, IAW_MAGIC, 4) || get16(in + 6) != IAW_HEADER_LEN) return false;
  uint32_t len = get32(in + 8);
  if (len > IAW_MAX_PAYLOAD) return false;
  h->type = in[4];
  h->flags = in[5];
  h->payload_len = len;
  h->seq = get32(in + 12);
  h->board_ms = get32(in + 16);
  return true;
}


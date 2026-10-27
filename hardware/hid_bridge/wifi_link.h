// Authenticated TCP control/status transport for the iPhone app.
// Copyright (c) 2026 Henry Yuan. MIT License; see the repository LICENSE.
#pragma once
#include <Arduino.h>

struct WiFiLinkStats {
  bool connected;
  bool authenticated;
  uint32_t kbps;
  uint32_t errors;
  uint32_t auth_failures;
  uint32_t rtt_ms;
};

bool wifi_link_begin();
bool wifi_link_pop_cmd(String &command, uint32_t &request_id);
void wifi_link_reply(uint32_t request_id, const String &reply);
void wifi_link_status(const String &json);
WiFiLinkStats wifi_link_stats();


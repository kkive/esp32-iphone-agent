# Wi-Fi transport protocol (IAW1)

This is the authenticated low-latency transport between the ESP32 and the iPhone app. The board listens on TCP port **8765**; its default SoftAP address is **192.168.4.1**. BLE remains available for encrypted provisioning, status/wake events, and video fallback.

## Authentication

At first boot the board creates a random 128-bit API token and stores it in NVS. The token is returned only in the encrypted/bonded BLE `net` characteristic, together with `ip` and `port`. A TCP client must send a `HELLO` frame containing the 32-character token within five seconds. The board answers `HELLO_REPLY` with `ok`; a failed authentication closes the socket.

## Frame

All integers are little-endian. A frame is a fixed 20-byte header followed by `payload_len` bytes.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | ASCII magic `IAW1` |
| 4 | 1 | message type |
| 5 | 1 | flags |
| 6 | 2 | header length, always 20 |
| 8 | 4 | payload length, maximum 524,288 |
| 12 | 4 | sequence/request ID |
| 16 | 4 | board uptime in milliseconds |

Message types: `1 HELLO`, `2 HELLO_REPLY`, `3 VIDEO_CONFIG`, `4 VIDEO_AU`, `5 STATUS`, `6 COMMAND`, `7 REPLY`, `8 PING`, `9 PONG`, `10 ERROR`.

- `COMMAND` is UTF-8 text using the same command vocabulary as BLE. Its `seq` is echoed by `REPLY`.
- `STATUS` is UTF-8 JSON and uses sequence zero.
- `PING` payload and sequence are echoed by `PONG`.
- `VIDEO_CONFIG` carries Annex-B SPS/PPS. `VIDEO_AU` carries one complete Annex-B access unit; flag bit 0 marks an IDR.
  When the authenticated socket is ready, continuous H.264 uses Wi-Fi. A socket failure makes the firmware restart
  the current GOP over BLE, so the app changes transport without losing decoder state.

## Alpha.1 smoke test

1. Pair the app over BLE and read the board's network configuration/token.
2. Join the board hotspot.
3. Run:

```powershell
python hardware/tools/wifi_smoke.py --token TOKEN --command ping
```

The expected output includes `authenticated`, a ping RTT, and `reply ok`.

## Attribution

Based on [ESP32 iPhone Agent](https://github.com/onehumanbeing/esp32-iphone-agent) by Henry Yuan. See `LICENSE` and `NOTICE`.


# v0.2 Wi-Fi transport field test

This checklist validates the complete foreground loop on a XIAO ESP32-S3: HID command → iPhone UI change → AirPlay H.264 → decoded frame in the app.

## Install matching artifacts

1. Download the firmware ZIP and unsigned IPA from the same GitHub release.
2. Keep the previous firmware ZIP as the rollback copy.
3. Flash `hid_bridge.ino.merged.bin` at offset `0x0` using the command in `FLASH.txt`.
4. Sign/install the matching unsigned IPA using the same Apple ID method used for earlier builds.
5. Pair the board in the app, join `ESP32Agent-XXXX`, and start Screen Mirroring to **ESP32 Agent**.

## Acceptance check

Open **Settings → Diagnostics** and confirm:

- Wi-Fi link state is `ready`.
- App-side Wi-Fi video throughput rises above zero while the screen changes.
- Command path changes to `Wi-Fi` after a normal command.
- Tap **Wi-Fi HID → frame**. It moves the pointer out and back and reports the time from the first HID command to a newly decoded frame.
- Target: **≤ 1000 ms**, excluding model inference time.

The BLE connection stays subscribed throughout the test. It provisions the token, carries background wake/status events, and becomes the immediate command/video fallback when the TCP link is not ready.

## Useful trace

After a failure, use **Diagnostics → Export traces**. The metrics include `wifi_kbps_app`, `wifi_loop_ms`, board Wi-Fi errors/authentication failures, BLE throughput, decode time and command timing.

Based on [ESP32 iPhone Agent](https://github.com/onehumanbeing/esp32-iphone-agent) by Henry Yuan. Public demos and posts should keep that credit.


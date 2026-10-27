# Changelog

## Unreleased (main)

### v0.2.0-alpha.1 Wi-Fi transport foundation

- Added the authenticated `IAW1` TCP service on port 8765, using a per-board 128-bit token provisioned through encrypted BLE.
- Added Wi-Fi status, ping/pong RTT diagnostics, and command/reply framing while retaining BLE as the active video path and fallback.
- Added an iOS Network.framework client, diagnostics panel, protocol tests, a desktop smoke-test tool, and tagged-release automation.
- Planned next: H.264 over Wi-Fi with automatic BLE fallback (`alpha.2`), then Wi-Fi commands/background wake (`alpha.3`).

- No computer: the ESP32 is the AirPlay receiver (ported from UxPlay) with its own hotspot, and talks to a new
  iPhone app over BLE. The app is the agent: planner/actor loop, pointer calibration, run replay, Live Activity,
  widgets, Siri and Shortcuts, self-healing when the board, hotspot or mirroring drops.
- Prompts moved to `prompts/`.
- Product name: ESP32 Agent.
- `server/` is legacy and no longer needed.

## v0.1.0

- Computer-side version: UxPlay + Python server + web UI; the ESP32 is a USB HID bridge over Wi-Fi.

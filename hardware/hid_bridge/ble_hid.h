// BLE HID (HID over GATT) backend: a standard Bluetooth mouse/keyboard/remote next to the
// custom app service, so the phone can be driven with no USB cable (board on any power source).
// Service layout per HID over GATT (HOGP): 0x1812 with Report Map + per-report characteristics
// (Report Reference descriptors), Protocol Mode, HID Information and Control Point.
// Only report mode; boot mode is not implemented (iOS and macOS both use report mode).
#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>

// Call before server->start(): creates the HID service on the given server.
void ble_hid_begin(NimBLEServer *server);
// Extra advertisement fields: mouse appearance + the HID service UUID (fits the 31-byte budget).
void ble_hid_advertise(NimBLEAdvertisementData &data);
// True once a central has enabled notifications on the HID input reports (the host's HID stack).
bool ble_hid_connected();

// Input reports. Deltas are int8 like the USB reports; buttons is bit 0 = left.
void ble_hid_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel, int8_t pan);
void ble_hid_keys(uint8_t modifiers, const uint8_t keys[6]);  // HID usage IDs, 0 = empty slot
void ble_hid_consumer(uint16_t usage);                        // e.g. AC Home 0x0223, 0 releases

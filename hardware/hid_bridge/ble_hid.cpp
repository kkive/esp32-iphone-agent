// BLE HID backend, see ble_hid.h. Report IDs and the descriptor below must stay in sync:
//   1 = mouse    buttons + X/Y/wheel/AC-pan (int8 each)
//   2 = keyboard modifiers + reserved + 6 usage slots (input), LED byte (output)
//   3 = consumer one 16-bit usage
#include "ble_hid.h"
#include <atomic>

namespace {

// Combined report descriptor: mouse + keyboard + consumer control, with report IDs.
const uint8_t REPORT_MAP[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x02,       // Usage (Mouse)
    0xA1, 0x01,       // Collection (Application)
    0x85, 0x01,       //   Report ID 1
    0x09, 0x01,       //   Usage (Pointer)
    0xA1, 0x00,       //   Collection (Physical)
    0x05, 0x09,       //     Usage Page (Buttons)
    0x19, 0x01,       //     Usage Min 1
    0x29, 0x03,       //     Usage Max 3
    0x15, 0x00,       //     Logical Min 0
    0x25, 0x01,       //     Logical Max 1
    0x95, 0x03,       //     Count 3
    0x75, 0x01,       //     Size 1
    0x81, 0x02,       //     Input (Data, Var, Abs)
    0x95, 0x01,       //     Count 1
    0x75, 0x05,       //     Size 5
    0x81, 0x03,       //     Input (Const) padding
    0x05, 0x01,       //     Usage Page (Generic Desktop)
    0x09, 0x30,       //     Usage X
    0x09, 0x31,       //     Usage Y
    0x15, 0x81,       //     Logical Min -127
    0x25, 0x7F,       //     Logical Max 127
    0x75, 0x08,       //     Size 8
    0x95, 0x02,       //     Count 2
    0x81, 0x06,       //     Input (Data, Var, Rel)
    0x09, 0x38,       //     Usage Wheel
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
    0x05, 0x0C,       //     Usage Page (Consumer)
    0x0A, 0x38, 0x02, //     Usage AC Pan
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
    0xC0,             //   End Collection (Physical)
    0xC0,             // End Collection (Application)

    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x06,       // Usage (Keyboard)
    0xA1, 0x01,       // Collection (Application)
    0x85, 0x02,       //   Report ID 2
    0x05, 0x07,       //   Usage Page (Keyboard)
    0x19, 0xE0,       //   Usage Min 0xE0 (Left Ctrl)
    0x29, 0xE7,       //   Usage Max 0xE7 (Right GUI)
    0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,  // modifiers
    0x95, 0x01, 0x75, 0x08, 0x81, 0x03,  // reserved
    0x95, 0x06, 0x75, 0x08,              // 6 usage slots
    0x15, 0x00, 0x25, 0xFF,
    0x05, 0x07, 0x19, 0x00, 0x29, 0xFF, 0x81, 0x00,
    0x05, 0x08,       //   Usage Page (LEDs)
    0x19, 0x01, 0x29, 0x03, 0x95, 0x03, 0x75, 0x01, 0x91, 0x02,  // LED output
    0x95, 0x01, 0x75, 0x05, 0x91, 0x03,                          // LED padding
    0xC0,             // End Collection

    0x05, 0x0C,       // Usage Page (Consumer)
    0x09, 0x01,       // Usage (Consumer Control)
    0xA1, 0x01,       // Collection (Application)
    0x85, 0x03,       //   Report ID 3
    0x15, 0x00, 0x26, 0xFF, 0x03,      //   Logical Max 0x3FF
    0x19, 0x00, 0x2A, 0xFF, 0x03,      //   Usage 0..0x3FF
    0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xC0,
};

// Input reports are 1 byte report ID + payload (sent with the ID prepended).
const uint8_t HID_INFO[] = {0x11, 0x00, 0x02};  // v1.1, country 0, normally connectable
std::atomic<int> hidHost{-1};                   // the central whose system HID stack subscribed
std::atomic<uint8_t> hidReports{0};             // mouse=1, keyboard=2, consumer=4

NimBLECharacteristic *mouseChr, *keysChr, *consChr;

uint8_t reportBit(NimBLECharacteristic *c) {
  return c == mouseChr ? 1 : c == keysChr ? 2 : c == consChr ? 4 : 0;
}

class HidCb : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic *c, NimBLEConnInfo &ci, uint16_t sub) override {
    bool on = sub & 1;
    uint8_t bit = reportBit(c);
    int handle = ci.getConnHandle();
    if (on) {
      int none = -1;
      hidHost.compare_exchange_strong(none, handle);
      if (hidHost == handle) hidReports |= bit;
    } else if (hidHost == handle) {
      hidReports &= ~bit;
      if (!hidReports) hidHost = -1;
    }
#if DEBUG_SERIAL
    Serial.printf("ble-hid: conn %u %s %s (reports 0x%x)\n", ci.getConnHandle(), on ? "sub" : "unsub",
                  c == mouseChr ? "mouse" : c == keysChr ? "keys" : "consumer", (unsigned)hidReports.load());
#endif
    // HID over GATT: the device asks for security; the host then runs its pairing prompt.
    // Without this the host's HID stack subscribes over a plain link and never pairs (spinner).
    if (on && !ci.isEncrypted()) NimBLEDevice::startSecurity(ci.getConnHandle());
  }
};

HidCb hidCb;

// Report Reference descriptor 0x2908: {report id, input(1) | output(2)}.
void reportRef(NimBLECharacteristic *c, uint8_t id, uint8_t type) {
  NimBLEDescriptor *d = c->createDescriptor(NimBLEUUID((uint16_t)0x2908), NIMBLE_PROPERTY::READ, 2);
  const uint8_t ref[2] = {id, type};
  d->setValue(ref, 2);
}

}  // namespace

void ble_hid_begin(NimBLEServer *server) {
  NimBLEService *svc = server->createService(NimBLEUUID((uint16_t)0x1812));

  NimBLECharacteristic *info = svc->createCharacteristic(NimBLEUUID((uint16_t)0x2A4A), NIMBLE_PROPERTY::READ);
  info->setValue(HID_INFO, sizeof HID_INFO);
  NimBLECharacteristic *map = svc->createCharacteristic(NimBLEUUID((uint16_t)0x2A4B), NIMBLE_PROPERTY::READ);
  map->setValue(REPORT_MAP, sizeof REPORT_MAP);
  svc->createCharacteristic(NimBLEUUID((uint16_t)0x2A4C), NIMBLE_PROPERTY::WRITE_NR);   // Control Point
  NimBLECharacteristic *mode = svc->createCharacteristic(NimBLEUUID((uint16_t)0x2A4E),  // Protocol Mode
                                                         NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE_NR);
  const uint8_t reportMode = 1;
  mode->setValue(&reportMode, 1);

  // Input reports: read (for hosts that poll) + notify (the normal path).
  mouseChr = svc->createCharacteristic(NimBLEUUID((uint16_t)0x2A4D),
                                       NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  reportRef(mouseChr, 1, 1);
  keysChr = svc->createCharacteristic(NimBLEUUID((uint16_t)0x2A4D),
                                      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  reportRef(keysChr, 2, 1);
  consChr = svc->createCharacteristic(NimBLEUUID((uint16_t)0x2A4D),
                                      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  reportRef(consChr, 3, 1);
  // Keyboard LED output report: the host writes it; no action needed.
  NimBLECharacteristic *leds = svc->createCharacteristic(NimBLEUUID((uint16_t)0x2A4D), NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  reportRef(leds, 2, 2);
  const uint8_t noLeds = 0;
  leds->setValue(&noLeds, 1);

  mouseChr->setCallbacks(&hidCb);
  keysChr->setCallbacks(&hidCb);
  consChr->setCallbacks(&hidCb);
  svc->start();
}

void ble_hid_advertise(NimBLEAdvertisementData &data) {
  data.setAppearance(0x03C2);  // HID mouse
  data.setPartialServices16({NimBLEUUID((uint16_t)0x1812)});
}

bool ble_hid_connected() { return hidHost >= 0 && hidReports != 0; }

void ble_hid_disconnected(uint16_t connHandle) {
  if (hidHost == (int)connHandle) {
    hidReports = 0;
    hidHost = -1;
  }
}

static bool notifyRetry(NimBLECharacteristic *c, const uint8_t *report, size_t len) {
  if (!ble_hid_connected()) return false;
  for (int tries = 0; tries < 10; tries++) {
    if (c->notify(report, len, (uint16_t)hidHost.load())) return true;
    delay(2);  // BLE/video coexistence can temporarily fill the controller queue.
  }
  return false;
}

void ble_hid_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel, int8_t pan) {
  const uint8_t r[6] = {1, buttons, (uint8_t)dx, (uint8_t)dy, (uint8_t)wheel, (uint8_t)pan};
  notifyRetry(mouseChr, r, sizeof r);
}

void ble_hid_keys(uint8_t modifiers, const uint8_t keys[6]) {
  const uint8_t r[9] = {2, modifiers, 0, keys[0], keys[1], keys[2], keys[3], keys[4], keys[5]};
  notifyRetry(keysChr, r, sizeof r);
}

void ble_hid_consumer(uint16_t usage) {
  const uint8_t r[3] = {3, (uint8_t)(usage & 0xFF), (uint8_t)(usage >> 8)};
  notifyRetry(consChr, r, sizeof r);
}

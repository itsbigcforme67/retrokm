// RetroKM Xbox pad: an M5Stack Atom S3R that plays an original Xbox
// controller (XID, "Controller S") on its USB-C port, with the game pad the
// RetroKM hub passes to it.
#pragma once
#include <Arduino.h>
#include "xidpad.h"

#define PIN_BUTTON 41  // the button under the screen

// xbox: the USB port is the controller (TinyUSB, XID).
// pc:   the USB port stays the ESP32's own serial/JTAG port, for flashing.
//       Chosen with "mode pc", or by holding the button while plugging in.
enum UsbMode { MODE_XBOX, MODE_PC };
extern UsbMode usb_mode;

// ---- xid_usb.cpp ----
struct XidStats {
  bool mounted;          // the host configured us
  uint32_t reports;      // input reports delivered
  uint32_t get_reports;  // GET_REPORT requests
  uint32_t xid_reqs;     // XID vendor requests (descriptor, capabilities)
  uint32_t rumbles;      // rumble reports received
  uint32_t resets;       // bus resets
};
void xid_start();                         // installs TinyUSB with the XID device
void xid_set_input(const xid_input_t &in);
bool xid_get_rumble(xid_rumble_t *out, uint32_t *seq);
XidStats xid_stats();
void xid_switch_to_pc_and_restart();      // hand the port back to serial/JTAG

// ---- screen.cpp ----
struct Status {
  const char *label, *screen;  // this board, and its hub screen name
  String ip;
  bool hub_up;                 // the hub welcomed us
  bool has_pad;                // between ENTER and LEAVE
};
void screen_start();
void screen_loop(const Status &st);
bool button_down();

// ---- main.cpp ----
const xid_input_t &current_input();

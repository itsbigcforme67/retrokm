// The keyboard side of PS/2: what a keyboard says back to the computer's
// commands, and the scan codes for each key.  No hardware here, so it also
// builds on a PC for testing (test/ps2kbd_test.cpp).
//
// PCs use scan code set 2.  SGI workstations (Indy, Indigo2, O2, Octane)
// switch the keyboard to set 3 and ask for make/break on every key, so both
// sets are here and the computer chooses.
#pragma once
#include <stdint.h>
#include <string.h>

struct ByteQueue {
  uint8_t b[64];
  int n = 0;
  void push(uint8_t v) { if (n < (int)sizeof b) b[n++] = v; }
  void clear() { n = 0; }
};

// HID usage -> set 2 make code.  0x100 bit: preceded by E0.  0 = no key.
static const uint16_t SET2[256] = {
  /* 00 */ 0, 0, 0, 0, 0x1C, 0x32, 0x21, 0x23, 0x24, 0x2B, 0x34, 0x33, 0x43, 0x3B, 0x42, 0x4B,
  /* 10 */ 0x3A, 0x31, 0x44, 0x4D, 0x15, 0x2D, 0x1B, 0x2C, 0x3C, 0x2A, 0x1D, 0x22, 0x35, 0x1A, 0x16, 0x1E,
  /* 20 */ 0x26, 0x25, 0x2E, 0x36, 0x3D, 0x3E, 0x46, 0x45, 0x5A, 0x76, 0x66, 0x0D, 0x29, 0x4E, 0x55, 0x54,
  /* 30 */ 0x5B, 0x5D, 0x5D, 0x4C, 0x52, 0x0E, 0x41, 0x49, 0x4A, 0x58, 0x05, 0x06, 0x04, 0x0C, 0x03, 0x0B,
  /* 40 */ 0x83, 0x0A, 0x01, 0x09, 0x78, 0x07, 0x17C, 0x7E, 0, 0x170, 0x16C, 0x17D, 0x171, 0x169, 0x17A, 0x174,
  /* 50 */ 0x16B, 0x172, 0x175, 0x77, 0x14A, 0x7C, 0x7B, 0x79, 0x15A, 0x69, 0x72, 0x7A, 0x6B, 0x73, 0x74, 0x6C,
  /* 60 */ 0x75, 0x7D, 0x70, 0x71, 0x61, 0x12F, 0, 0x0F,
  /* 68: F13.. */ 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x57, 0x5F,
  /* 74.. */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  /* 80.. */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  /* 90.. */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  /* A0.. */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  /* B0.. */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  /* C0.. */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  /* D0.. */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  /* E0 */ 0x14, 0x12, 0x11, 0x11F, 0x114, 0x59, 0x111, 0x127,
};

// HID usage -> set 3 make code (one byte, break = F0 code).  0 = no key.
static const uint8_t SET3[256] = {
  /* 00 */ 0, 0, 0, 0, 0x1C, 0x32, 0x21, 0x23, 0x24, 0x2B, 0x34, 0x33, 0x43, 0x3B, 0x42, 0x4B,
  /* 10 */ 0x3A, 0x31, 0x44, 0x4D, 0x15, 0x2D, 0x1B, 0x2C, 0x3C, 0x2A, 0x1D, 0x22, 0x35, 0x1A, 0x16, 0x1E,
  /* 20 */ 0x26, 0x25, 0x2E, 0x36, 0x3D, 0x3E, 0x46, 0x45, 0x5A, 0x08, 0x66, 0x0D, 0x29, 0x4E, 0x55, 0x54,
  /* 30 */ 0x5B, 0x5C, 0x53, 0x4C, 0x52, 0x0E, 0x41, 0x49, 0x4A, 0x14, 0x07, 0x0F, 0x17, 0x1F, 0x27, 0x2F,
  /* 40 */ 0x37, 0x3F, 0x47, 0x4F, 0x56, 0x5E, 0x57, 0x5F, 0x62, 0x67, 0x6E, 0x6F, 0x64, 0x65, 0x6D, 0x6A,
  /* 50 */ 0x61, 0x60, 0x63, 0x76, 0x77, 0x7E, 0x84, 0x7C, 0x79, 0x69, 0x72, 0x7A, 0x6B, 0x73, 0x74, 0x6C,
  /* 60 */ 0x75, 0x7D, 0x70, 0x71, 0x13, 0x8D, 0, 0,
  /* 68.. */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  /* 74.. */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  /* 80.. */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  /* 90.. */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  /* A0.. */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  /* B0.. */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  /* C0.. */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  /* D0.. */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  /* E0 */ 0x11, 0x12, 0x19, 0x8B, 0x58, 0x59, 0x39, 0x8C,
};

enum { HID_PRINTSCREEN_ = 0x46, HID_PAUSE_ = 0x48, HID_LCTRL_ = 0xE0 };

class Ps2Keyboard {
 public:
  ByteQueue out;      // bytes for the computer, in order
  int set = 2;        // scan code set in use
  bool enabled = true;
  bool held[256] = {};
  uint8_t leds = 0;   // bit 0 scroll, 1 num, 2 caps

  // Set 3 key types, as asked for by the computer (F7..FA): all keys alike
  enum { T3_DEFAULT, T3_TYPEMATIC, T3_MAKE_BREAK, T3_MAKE, T3_ALL } mode3 = T3_DEFAULT;

  // Power on: the self-test passed
  void powerOn() { reset(); out.push(0xAA); }

  // One byte from the computer.  Every command is acknowledged with FA;
  // some take an argument byte, some answer more.
  void hostByte(uint8_t b) {
    if (arg_) {                       // the argument of the previous command
      uint8_t cmd = arg_;
      arg_ = 0;
      if (cmd == 0xED) { leds = b & 7; out.push(0xFA); return; }
      if (cmd == 0xF3) { out.push(0xFA); return; }          // typematic rate: noted, unused
      if (cmd == 0xF0) {
        out.push(0xFA);
        if (b == 0) out.push((uint8_t)set);                  // "which set?"
        else if (b == 1 || b == 2 || b == 3) set = b == 1 ? 2 : b;   // set 1: we answer in 2
        return;
      }
      if (cmd >= 0xFB && cmd <= 0xFD) {                      // per-key set 3 types: a list
        if (b < 0xED) { arg_ = cmd; out.push(0xFA); return; }
        // a command ends the list: fall through and handle it
      }
    }
    switch (b) {
    case 0xFF:                                               // reset
      reset();
      out.push(0xFA);
      out.push(0xAA);
      break;
    case 0xFE:                                               // resend
      out.push(last_);
      break;
    case 0xF2:                                               // identify
      out.push(0xFA);
      out.push(0xAB);
      out.push(0x83);
      break;
    case 0xEE:                                               // echo
      out.push(0xEE);
      break;
    case 0xED: case 0xF3: case 0xF0:                         // take an argument
    case 0xFB: case 0xFC: case 0xFD:
      arg_ = b;
      out.push(0xFA);
      break;
    case 0xF4: enabled = true; out.push(0xFA); break;
    case 0xF5: enabled = false; defaults(); out.push(0xFA); break;
    case 0xF6: defaults(); out.push(0xFA); break;
    case 0xF7: mode3 = T3_TYPEMATIC; out.push(0xFA); break;
    case 0xF8: mode3 = T3_MAKE_BREAK; out.push(0xFA); break;
    case 0xF9: mode3 = T3_MAKE; out.push(0xFA); break;
    case 0xFA: mode3 = T3_ALL; out.push(0xFA); break;
    default: out.push(0xFE); break;                          // unknown: "resend"
    }
  }

  // A key from the hub: state 0 up, 1 down, 2 repeat (auto-repeat is the
  // keyboard's job in PS/2, so the hub's repeats are passed on as makes).
  void key(uint8_t usage, int state) {
    if (!enabled) return;
    if (state == 0 && !held[usage]) return;
    if (state == 2 && !held[usage]) state = 1;
    if (state == 2 && set == 3 && (mode3 == T3_MAKE_BREAK || mode3 == T3_MAKE)) return;
    held[usage] = state != 0;
    if (set == 3) {
      uint8_t c = SET3[usage];
      if (!c) return;
      if (state == 0) {
        if (mode3 == T3_MAKE || mode3 == T3_TYPEMATIC) return;   // these never send a break
        out.push(0xF0);
      }
      out.push(c);
      return;
    }
    if (usage == HID_PAUSE_) {                               // make only, a fixed string
      if (state == 1) for (uint8_t v : {0xE1, 0x14, 0x77, 0xE1, 0xF0, 0x14, 0xF0, 0x77}) out.push(v);
      return;
    }
    if (usage == HID_PRINTSCREEN_) {
      if (state) { for (uint8_t v : {0xE0, 0x12, 0xE0, 0x7C}) out.push(v); }
      else { for (uint8_t v : {0xE0, 0xF0, 0x7C, 0xE0, 0xF0, 0x12}) out.push(v); }
      return;
    }
    uint16_t c = SET2[usage];
    if (!c) return;
    if (c & 0x100) out.push(0xE0);
    if (state == 0) out.push(0xF0);
    out.push((uint8_t)c);
  }

  // Let go of everything (the pointer left this machine)
  void releaseAll() {
    for (int u = 0; u < 256; u++)
      if (held[u]) key((uint8_t)u, 0);
  }

  // The last byte actually sent, for "resend"
  void sent(uint8_t b) { if (b != 0xFE) last_ = b; }

 private:
  uint8_t arg_ = 0, last_ = 0xAA;
  void defaults() { mode3 = T3_DEFAULT; memset(held, 0, sizeof held); }
  void reset() { set = 2; enabled = true; leds = 0; arg_ = 0; out.clear(); defaults(); }
};

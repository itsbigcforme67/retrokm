#include "xidpad.h"
#include <string.h>
#include "rkm_proto.h"

void xid_from_rkm(unsigned b, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly,
                  int16_t rx, int16_t ry, xid_input_t *out) {
  static const struct { unsigned rkm; uint8_t xb; } dig[] = {
      {RKM_PAD_UP, XB_UP}, {RKM_PAD_DOWN, XB_DOWN}, {RKM_PAD_LEFT, XB_LEFT},
      {RKM_PAD_RIGHT, XB_RIGHT}, {RKM_PAD_START, XB_START}, {RKM_PAD_BACK, XB_BACK},
      {RKM_PAD_LS, XB_LS}, {RKM_PAD_RS, XB_RS},
  };
  static const struct { unsigned rkm; int xa; } ana[] = {
      {RKM_PAD_A, XA_A}, {RKM_PAD_B, XA_B}, {RKM_PAD_X, XA_X}, {RKM_PAD_Y, XA_Y},
      {RKM_PAD_RB, XA_BLACK}, {RKM_PAD_LB, XA_WHITE},
  };
  unsigned i;
  memset(out, 0, sizeof *out);
  for (i = 0; i < sizeof dig / sizeof dig[0]; i++)
    if (b & dig[i].rkm) out->digital |= dig[i].xb;
  for (i = 0; i < sizeof ana / sizeof ana[0]; i++)
    if (b & ana[i].rkm) out->analog[ana[i].xa] = 255;
  out->analog[XA_LT] = lt;
  out->analog[XA_RT] = rt;
  out->lx = lx;
  out->ly = ly;
  out->rx = rx;
  out->ry = ry;
}

static void put16(uint8_t *p, int16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((uint16_t)v >> 8);
}

static int16_t get16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }

void xid_pack(const xid_input_t *in, uint8_t out[XID_IN_LEN]) {
  out[0] = 0x00;
  out[1] = XID_IN_LEN;
  out[2] = in->digital;
  out[3] = 0;
  memcpy(out + 4, in->analog, XA_COUNT);
  put16(out + 12, in->lx);
  put16(out + 14, in->ly);
  put16(out + 16, in->rx);
  put16(out + 18, in->ry);
}

void xid_unpack(const uint8_t in[XID_IN_LEN], xid_input_t *out) {
  out->digital = in[2];
  memcpy(out->analog, in + 4, XA_COUNT);
  out->lx = get16(in + 12);
  out->ly = get16(in + 14);
  out->rx = get16(in + 16);
  out->ry = get16(in + 18);
}

bool xid_parse_rumble(const uint8_t *buf, int len, xid_rumble_t *out) {
  if (len < XID_OUT_LEN || buf[0] != 0x00 || buf[1] < XID_OUT_LEN) return false;
  out->left = (uint16_t)(buf[2] | buf[3] << 8);
  out->right = (uint16_t)(buf[4] | buf[5] << 8);
  return true;
}

// A Microsoft "Controller S" (045E:0289). No strings, like the real one.
const uint8_t xid_device_desc[18] = {
    18, 0x01,    // device descriptor
    0x10, 0x01,  // USB 1.1
    0x00, 0x00, 0x00,  // class per interface
    64,          // EP0 size (TinyUSB's on the ESP32-S3)
    0x5E, 0x04,  // Microsoft
    0x89, 0x02,  // Controller S
    0x21, 0x01,  // bcdDevice 1.21
    0, 0, 0,     // no strings
    1,           // one configuration
};

const uint8_t xid_config_desc[32] = {
    9, 0x02, 32, 0, 1, 1, 0, 0x80, 50,  // 1 interface, bus powered, 100 mA
    9, 0x04, 0, 0, 2, 0x58, 0x42, 0x00, 0,  // interface 0: XID class 'X' 'B'
    7, 0x05, 0x81, 0x03, 32, 0, 4,  // EP1 IN interrupt, 32 bytes, 4 ms
    7, 0x05, 0x02, 0x03, 32, 0, 4,  // EP2 OUT interrupt, 32 bytes, 4 ms
};

const uint8_t xid_xid_desc[16] = {
    16, 0x42,    // XID descriptor
    0x00, 0x01,  // bcdXid 1.00
    0x01,        // type: gamepad
    0x02,        // subtype: Controller S (0x01 would be the Duke)
    XID_IN_LEN, XID_OUT_LEN,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // no alternate product ids
};

// Which bits of each report the device really drives (all of them).
const uint8_t xid_caps_in[20] = {
    0x00, XID_IN_LEN, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};
const uint8_t xid_caps_out[6] = {0x00, XID_OUT_LEN, 0xFF, 0xFF, 0xFF, 0xFF};

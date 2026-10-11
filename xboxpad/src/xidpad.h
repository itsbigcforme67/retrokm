// Original Xbox controller (XID) report formats. Plain C, no hardware, so
// the PC test in test_host/ can check it.
//
// Input report, 20 bytes, sent on the interrupt IN endpoint and in reply to
// GET_REPORT:
//   0     report id 0x00
//   1     length 0x14
//   2     digital buttons: up 01 down 02 left 04 right 08 start 10 back 20
//         left-stick click 40, right-stick click 80
//   3     0
//   4-11  analog buttons 0..255: A B X Y black white left-trigger right-trigger
//   12-19 sticks LX LY RX RY, int16 little endian, up and right positive
//
// Output (rumble) report, 6 bytes, from the interrupt OUT endpoint or SET_REPORT:
//   0 0x00, 1 0x06, 2-3 left (big) motor u16 LE, 4-5 right (small) motor u16 LE
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  XB_UP = 0x01, XB_DOWN = 0x02, XB_LEFT = 0x04, XB_RIGHT = 0x08,
  XB_START = 0x10, XB_BACK = 0x20, XB_LS = 0x40, XB_RS = 0x80,
};
enum { XA_A, XA_B, XA_X, XA_Y, XA_BLACK, XA_WHITE, XA_LT, XA_RT, XA_COUNT };

#define XID_IN_LEN 20
#define XID_OUT_LEN 6

typedef struct {
  uint8_t digital;           // XB_* bits
  uint8_t analog[XA_COUNT];  // 0..255
  int16_t lx, ly, rx, ry;    // up / right positive
} xid_input_t;

typedef struct {
  uint16_t left, right;  // big and small motor
} xid_rumble_t;

// The hub's pad state (RKM_PAD) as an original Xbox pad: A B X Y analog,
// LB = white, RB = black, View = back, Menu = start. Guide and Share have no
// place on the Xbox pad.
void xid_from_rkm(unsigned buttons, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly,
                  int16_t rx, int16_t ry, xid_input_t *out);

void xid_pack(const xid_input_t *in, uint8_t out[XID_IN_LEN]);
void xid_unpack(const uint8_t in[XID_IN_LEN], xid_input_t *out);
// False when the bytes are not a rumble report.
bool xid_parse_rumble(const uint8_t *buf, int len, xid_rumble_t *out);

// Descriptors the console asks for. The device descriptor and configuration
// are standard USB; the other three answer XID vendor requests.
extern const uint8_t xid_device_desc[18];
extern const uint8_t xid_config_desc[32];
extern const uint8_t xid_xid_desc[16];      // GET_DESCRIPTOR 0x42
extern const uint8_t xid_caps_in[20];       // GET_CAPABILITIES input
extern const uint8_t xid_caps_out[6];       // GET_CAPABILITIES output

#ifdef __cplusplus
}
#endif

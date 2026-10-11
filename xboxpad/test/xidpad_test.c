// Checks the XID report layout against how Linux's xpad driver reads an
// original Xbox pad (drivers/input/joystick/xpad.c, XTYPE_XBOX), and the
// descriptors against the published Controller S values.
#include <stdio.h>
#include <string.h>
#include "xidpad.h"
#include "rkm_proto.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static int le16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }

int main(void) {
  xid_input_t in = {0};
  in.digital = XB_UP | XB_RIGHT | XB_START | XB_RS;
  for (int i = 0; i < XA_COUNT; i++) in.analog[i] = (uint8_t)(10 + i);
  in.lx = -32768; in.ly = 32767; in.rx = 1234; in.ry = -1;
  uint8_t r[XID_IN_LEN];
  memset(r, 0xEE, sizeof r);
  xid_pack(&in, r);

  CHECK(r[0] == 0x00 && r[1] == 20);
  CHECK(r[2] == 0x01 + 0x08 + 0x10 + 0x80);  // xpad: up 01 down 02 left 04 right 08 start 10 back 20 thumbs 40/80
  CHECK(r[3] == 0);
  CHECK(r[4] == 10 && r[5] == 11 && r[6] == 12 && r[7] == 13);  // xpad BTN_A..BTN_Y = data[4..7]
  CHECK(r[8] == 14 && r[9] == 15);    // xpad BTN_C (black) data[8], BTN_Z (white) data[9]
  CHECK(r[10] == 16 && r[11] == 17);  // xpad ABS_Z / ABS_RZ = data[10] / data[11]
  CHECK(le16(r + 12) == -32768 && le16(r + 14) == 32767);
  CHECK(le16(r + 16) == 1234 && le16(r + 18) == -1);

  xid_input_t back;
  xid_unpack(r, &back);
  CHECK(!memcmp(&back, &in, sizeof in));

  // xpad's rumble: 00 06 00 <strong> 00 <weak>
  xid_rumble_t rm;
  const uint8_t xpad_rumble[6] = {0x00, 0x06, 0x00, 0xC0, 0x00, 0x40};
  CHECK(xid_parse_rumble(xpad_rumble, 6, &rm) && rm.left == 0xC000 && rm.right == 0x4000);
  const uint8_t junk[6] = {0x01, 0x06, 0, 0, 0, 0};
  CHECK(!xid_parse_rumble(junk, 6, &rm));
  CHECK(!xid_parse_rumble(xpad_rumble, 5, &rm));

  // descriptors
  CHECK(xid_device_desc[0] == 18 && xid_device_desc[1] == 1);
  CHECK(xid_device_desc[8] == 0x5E && xid_device_desc[9] == 0x04);    // 045E
  CHECK(xid_device_desc[10] == 0x89 && xid_device_desc[11] == 0x02);  // 0289
  CHECK(xid_config_desc[2] == sizeof xid_config_desc && xid_config_desc[3] == 0);
  int total = 0;
  for (int i = 0; i < (int)sizeof xid_config_desc; i += xid_config_desc[i]) total += xid_config_desc[i];
  CHECK(total == (int)sizeof xid_config_desc);
  CHECK(xid_config_desc[9 + 5] == 0x58 && xid_config_desc[9 + 6] == 0x42);  // 'X' 'B'
  CHECK(xid_xid_desc[0] == sizeof xid_xid_desc && xid_xid_desc[1] == 0x42);
  CHECK(xid_xid_desc[6] == 20 && xid_xid_desc[7] == 6);
  CHECK(xid_caps_in[1] == 20 && xid_caps_out[1] == 6);

  // the hub's pad state
  xid_input_t m;
  xid_from_rkm(RKM_PAD_A | RKM_PAD_LB | RKM_PAD_RB | RKM_PAD_UP | RKM_PAD_START | RKM_PAD_GUIDE,
               7, 200, 100, -100, 32767, -32768, &m);
  CHECK(m.digital == (XB_UP | XB_START));
  CHECK(m.analog[XA_A] == 255 && m.analog[XA_B] == 0 && m.analog[XA_WHITE] == 255 && m.analog[XA_BLACK] == 255);
  CHECK(m.analog[XA_LT] == 7 && m.analog[XA_RT] == 200);
  CHECK(m.lx == 100 && m.ly == -100 && m.rx == 32767 && m.ry == -32768);
  xid_from_rkm(0xFFFF, 0, 0, 0, 0, 0, 0, &m);
  CHECK(m.digital == 0xFF);
  for (int i = 0; i < 6; i++) CHECK(m.analog[i] == 255);

  printf("%d/%d checks passed\n", checks - fails, checks);
  return fails != 0;
}

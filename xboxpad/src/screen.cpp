// The Atom S3R's 128x128 screen: mode, hub link, whether this screen has
// the pad, the buttons being sent and the game's rumble.
#include "dongle.h"
#include <M5Unified.h>

static M5Canvas canvas(&M5.Display);
static bool ok;

void screen_start() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 0;  // the USB port is not ours to print on
  cfg.internal_imu = false;
  cfg.internal_rtc = false;
  cfg.internal_spk = false;
  cfg.internal_mic = false;
  M5.begin(cfg);
  M5.Display.setBrightness(60);
  ok = canvas.createSprite(M5.Display.width(), M5.Display.height()) != nullptr;
}

bool button_down() { return M5.BtnA.isPressed(); }

static void bar(int x, int y, int w, int h, uint32_t v, uint32_t max, uint16_t col) {
  canvas.drawRect(x, y, w, h, TFT_DARKGREY);
  int f = (int)((uint64_t)v * (w - 2) / max);
  if (f > 0) canvas.fillRect(x + 1, y + 1, f, h - 2, col);
}

static void dot(int x, int y, bool on, const char *label) {
  if (on) canvas.fillCircle(x, y, 5, TFT_GREEN);
  else canvas.drawCircle(x, y, 5, TFT_DARKGREY);
  canvas.setTextColor(on ? TFT_BLACK : TFT_DARKGREY);
  canvas.drawCenterString(label, x, y - 3);
}

void screen_loop(const Status &st) {
  M5.update();
  static uint32_t last;
  if (!ok || millis() - last < 200) return;
  last = millis();

  XidStats s = xid_stats();
  const xid_input_t &in = current_input();
  canvas.fillScreen(TFT_BLACK);
  canvas.setFont(&fonts::Font0);
  canvas.setTextSize(1);

  canvas.setTextColor(TFT_GREEN);
  canvas.drawString(st.label, 2, 2);
  if (usb_mode == MODE_PC) {
    canvas.setTextColor(TFT_YELLOW);
    canvas.drawString("PC MODE (USB serial)", 2, 14);
  } else {
    canvas.setTextColor(s.mounted ? TFT_WHITE : TFT_RED);
    canvas.drawString(s.mounted ? "Xbox: pad plugged in" : "Xbox: not seen yet", 2, 14);
  }
  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.drawString(st.ip.c_str(), 2, 26);
  canvas.setTextColor(!st.hub_up ? TFT_RED : st.has_pad ? TFT_GREEN : TFT_LIGHTGREY);
  char line[40];
  snprintf(line, sizeof line, "%s: %s", st.screen,
           !st.hub_up ? "no hub" : st.has_pad ? "HAS THE PAD" : "pad elsewhere");
  canvas.drawString(line, 2, 38);

  for (int i = 0; i < 2; i++) {  // sticks
    int cx = 22 + i * 44, cy = 70;
    int16_t x = i ? in.rx : in.lx, y = i ? in.ry : in.ly;
    canvas.drawCircle(cx, cy, 16, TFT_DARKGREY);
    canvas.fillCircle(cx + x * 14 / 32768, cy - y * 14 / 32768, 3,
                      (in.digital & (i ? XB_RS : XB_LS)) ? TFT_YELLOW : TFT_WHITE);
  }
  static const char *face[4] = {"A", "B", "X", "Y"};
  static const int fx[4] = {110, 120, 100, 110}, fy[4] = {78, 68, 68, 58};
  for (int i = 0; i < 4; i++) dot(fx[i], fy[i], in.analog[i] > 30, face[i]);
  dot(10, 98, in.digital & XB_UP, "");
  dot(10, 112, in.digital & XB_DOWN, "");
  dot(3, 105, in.digital & XB_LEFT, "");
  dot(17, 105, in.digital & XB_RIGHT, "");
  dot(40, 105, in.digital & XB_BACK, "");
  dot(54, 105, in.digital & XB_START, "");
  dot(100, 92, in.analog[XA_WHITE] > 30, "");
  dot(116, 92, in.analog[XA_BLACK] > 30, "");
  bar(70, 100, 26, 6, in.analog[XA_LT], 255, TFT_CYAN);
  bar(100, 100, 26, 6, in.analog[XA_RT], 255, TFT_CYAN);
  xid_rumble_t r;
  uint32_t seq = 0;
  xid_get_rumble(&r, &seq);
  bar(70, 110, 26, 6, r.left, 65535, TFT_ORANGE);
  bar(100, 110, 26, 6, r.right, 65535, TFT_ORANGE);
  canvas.setTextColor(TFT_DARKGREY);
  canvas.drawString("rumble", 70, 119);

  canvas.pushSprite(0, 0);
}

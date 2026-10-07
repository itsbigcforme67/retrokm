// Little vector pictures of the machines on the desk.  Each is drawn in a
// 160x120 box centred on (0,0) and scaled; swap in better art any time.
#pragma once
#include <M5Unified.h>
#include <string>

using Gfx = LGFX_Sprite;

// Mix two rgb888 colours; t = 0 gives a, 1 gives b
static inline uint32_t mix(uint32_t a, uint32_t b, float t) {
  auto ch = [&](int s) {
    float x = ((a >> s) & 255) * (1 - t) + ((b >> s) & 255) * t;
    return (uint32_t)(x + 0.5f) << s;
  };
  return ch(16) | ch(8) | ch(0);
}

struct Pen {
  Gfx& g;
  float cx, cy, s;
  float dim;  // 0 = normal, up to 1 = faded into the background
  uint32_t bg;
  int X(float x) const { return (int)(cx + x * s + 0.5f); }
  int Y(float y) const { return (int)(cy + y * s + 0.5f); }
  int L(float v) const { int r = (int)(v * s + 0.5f); return r < 1 ? 1 : r; }
  lgfx::rgb888_t c(uint32_t col) const {  // 0xRRGGBB, faded by dim
    if (dim > 0) col = mix(col, bg, dim);
    return lgfx::rgb888_t(col >> 16, (col >> 8) & 255, col & 255);
  }

  void rect(float x, float y, float w, float h, uint32_t col) { g.fillRect(X(x), Y(y), L(w), L(h), c(col)); }
  void rrect(float x, float y, float w, float h, float r, uint32_t col) {
    g.fillSmoothRoundRect(X(x), Y(y), L(w), L(h), L(r), c(col));
  }
  void frame(float x, float y, float w, float h, float r, uint32_t col) {
    g.drawRoundRect(X(x), Y(y), L(w), L(h), L(r), c(col));
  }
  void quad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t col) {
    g.fillTriangle(X(x0), Y(y0), X(x1), Y(y1), X(x2), Y(y2), c(col));
    g.fillTriangle(X(x0), Y(y0), X(x2), Y(y2), X(x3), Y(y3), c(col));
  }
  void line(float x0, float y0, float x1, float y1, uint32_t col, float w = 0) {
    if (w <= 0) g.drawLine(X(x0), Y(y0), X(x1), Y(y1), c(col));
    else g.drawWideLine(X(x0), Y(y0), X(x1), Y(y1), w * s, c(col));
  }
  void dot(float x, float y, float r, uint32_t col) { g.fillSmoothCircle(X(x), Y(y), L(r), c(col)); }
  void ellipse(float x, float y, float rx, float ry, uint32_t col) { g.fillEllipse(X(x), Y(y), L(rx), L(ry), c(col)); }
};

// Lenovo IdeaPad Gaming 3: black laptop, blue-lit keyboard
static void artIdeapad(Pen& p) {
  p.ellipse(0, 56, 82, 5, 0x0B0C0E);                             // shadow
  p.rrect(-58, -58, 116, 78, 5, 0x24272D);                       // lid
  p.rrect(-53, -53, 106, 66, 2, 0x16181C);                       // bezel
  p.rect(-51, -51, 102, 62, 0x1E4FD0);                           // screen
  p.rect(-51, -51, 102, 20, 0x2C63E6);
  p.rect(-44, -43, 40, 26, 0x7FA9F5);                            // a window
  p.rect(-44, -43, 40, 5, 0xDCE7FB);
  p.rect(6, -30, 34, 30, 0x4C83EE);
  p.rect(-51, 3, 102, 7, 0x10131A);                              // taskbar
  p.rect(-40, 20, 80, 3, 0x15171B);                              // hinge
  p.quad(-62, 23, 62, 23, 78, 50, -78, 50, 0x2C3036);            // deck
  for (int r = 0; r < 4; r++) {                                  // keys
    float t0 = r / 4.0f, t1 = (r + 0.7f) / 4.0f;
    float y0 = 26 + t0 * 15, y1 = 26 + t1 * 15;
    float half0 = 54 + t0 * 11, half1 = 54 + t1 * 11;
    for (int k = 0; k < 12; k++) {
      float a = k / 12.0f, b = (k + 0.78f) / 12.0f;
      p.quad(-half0 + a * 2 * half0, y0, -half0 + b * 2 * half0, y0,
             -half1 + b * 2 * half1, y1, -half1 + a * 2 * half1, y1, 0x111317);
    }
  }
  p.line(-56, 42, 56, 42, 0x3E7BFF);                             // blue glow
  p.rrect(-13, 43.5f, 26, 5, 1, 0x3A3E45);                       // touchpad
  p.quad(-78, 50, 78, 50, 76, 55, -76, 55, 0x1A1C20);            // front edge
}

// Dell Precision 410: putty-grey mid tower
static void artPrecision(Pen& p) {
  p.ellipse(4, 58, 50, 5, 0x0B0C0E);
  p.quad(-34, -52, 22, -52, 36, -62, -20, -62, 0xEAE5D8);        // top
  p.quad(22, -52, 36, -62, 36, 48, 22, 58, 0xAFA999);            // side
  p.rect(-34, -52, 56, 110, 0xD8D2C2);                           // front
  for (int i = 0; i < 2; i++) {                                  // 5.25" bays
    float y = -46 + i * 13;
    p.rect(-29, y, 46, 11, 0xCBC4B2);
    p.rect(-29, y + 10, 46, 1, 0xA39D8D);
    p.rect(-24, y + 5, 30, 1.5f, 0x8C8676);
  }
  p.rect(-29, -18, 46, 9, 0xCBC4B2);                             // floppy
  p.rect(-20, -14, 26, 2, 0x55514A);
  p.rect(10, -15, 4, 3, 0xA39D8D);
  p.dot(-6, 8, 6, 0xB9B3A3);                                     // badge
  p.dot(-6, 8, 4, 0x8E8878);
  p.dot(-14, 28, 3.5f, 0x8E8878);                                // power
  p.dot(-2, 28, 1.8f, 0x30D060);                                 // LED
  for (int i = 0; i < 4; i++) p.rect(-28, 38 + i * 4, 44, 1.5f, 0xBAB3A2);  // vents
  p.rect(-32, 58, 8, 2, 0x55514A);                               // feet
  p.rect(12, 58, 8, 2, 0x55514A);
}

// Small white ARM media box
static void artArmbox(Pen& p) {
  p.ellipse(4, 22, 66, 6, 0x0B0C0E);
  p.quad(-56, -4, 36, -4, 58, -22, -34, -22, 0xF3F4F6);          // top
  p.quad(36, -4, 58, -22, 58, -6, 36, 12, 0xBFC3C9);             // side
  p.rect(-56, -4, 92, 16, 0xDCDFE4);                             // front
  p.rect(-56, -4, 92, 1, 0xFFFFFF);
  p.dot(-46, 4, 2, 0x3B82F6);                                    // LED
  p.rect(18, 0, 12, 7, 0x2A2D33);                                // IR window
  p.quad(-14, -12, 14, -12, 22, -17, -6, -17, 0xE1E3E8);         // logo
  // a little remote, so it reads as a media box
  p.quad(-40, 36, -10, 30, -6, 36, -36, 42, 0x2B2E34);
  p.dot(-30, 37, 1.6f, 0xD94848);
  p.dot(-22, 35.5f, 1.2f, 0x8A8F98);
  p.dot(-16, 34.5f, 1.2f, 0x8A8F98);
}

// SGI Octane: chunky blue-green box with the cube logo
static void artOctane(Pen& p) {
  p.ellipse(6, 58, 52, 5, 0x0B0C0E);
  p.quad(-38, -50, 26, -50, 44, -60, -20, -60, 0x4A93B8);        // top
  p.quad(26, -50, 44, -60, 44, 44, 26, 56, 0x1F5574);            // side
  p.rrect(-40, -52, 68, 110, 10, 0x2E7298);                      // front
  p.rrect(-34, -44, 56, 92, 8, 0x3883AC);                        // front skin
  p.line(-30, -38, -30, 40, 0x5BA4CB);                           // highlight
  // cube logo, as three rhombi
  float cx = -6, cy = -16, r = 9;
  p.quad(cx, cy - r, cx + r * 0.87f, cy - r / 2, cx, cy, cx - r * 0.87f, cy - r / 2, 0xE4F3FA);
  p.quad(cx - r * 0.87f, cy - r / 2, cx, cy, cx, cy + r, cx - r * 0.87f, cy + r / 2, 0xA9D3E7);
  p.quad(cx, cy, cx + r * 0.87f, cy - r / 2, cx + r * 0.87f, cy + r / 2, cx, cy + r, 0x6FB3D3);
  p.rect(-22, 8, 32, 3, 0x9CCBE2);                               // name badge
  p.dot(-20, 36, 3, 0x1B4D6A);                                   // buttons
  p.dot(-10, 36, 3, 0x1B4D6A);
  p.dot(4, 36, 1.6f, 0x30D060);
  p.rect(-36, 56, 60, 3, 0x173F57);                              // plinth
}

static void artGeneric(Pen& p) {
  p.ellipse(0, 52, 60, 5, 0x0B0C0E);
  p.rrect(-50, -40, 100, 90, 6, 0x6B7280);
  p.rrect(-42, -32, 84, 60, 3, 0x1F2937);
  p.rect(-20, 36, 40, 4, 0x9CA3AF);
}

static void drawArt(Gfx& g, const std::string& art, int cx, int cy, float scale,
                    float dim = 0, uint32_t bg = 0x15181D) {
  Pen p{g, (float)cx, (float)cy, scale, dim, bg};
  if (art == "ideapad") artIdeapad(p);
  else if (art == "precision") artPrecision(p);
  else if (art == "armbox") artArmbox(p);
  else if (art == "octane") artOctane(p);
  else artGeneric(p);
}

// Cable colour for each machine
static uint32_t machineColor(const std::string& art, int index) {
  if (art == "ideapad") return 0x4F8DF7;
  if (art == "precision") return 0xF2A33A;
  if (art == "armbox") return 0xE5E7EB;
  if (art == "octane") return 0x2EC4B6;
  static const uint32_t pal[] = {0xF472B6, 0xA78BFA, 0xFB7185, 0x84CC16, 0xFACC15};
  return pal[index % 5];
}

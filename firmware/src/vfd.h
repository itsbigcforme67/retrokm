// Speak & Spell style VFD: slanted 14-segment cyan glyphs on smoked glass in
// a red plastic bezel, scrolling right to left. Included from main.cpp.
#pragma once

// Segments: A top, B upper right, C lower right, D bottom, E lower left,
// F upper left, G mid left, P mid right (G2), H diag up-left, I upper centre,
// J diag up-right, K diag down-left, L lower centre, M diag down-right
enum : uint16_t {
  SA = 1 << 0, SB = 1 << 1, SC = 1 << 2, SD = 1 << 3, SE = 1 << 4, SF = 1 << 5,
  SG = 1 << 6, SP = 1 << 7, SH = 1 << 8, SI = 1 << 9, SJ = 1 << 10, SK = 1 << 11,
  SL = 1 << 12, SM = 1 << 13, SDOT = 1 << 14
};

static uint16_t vfdGlyph(char c) {
  if (c >= 'a' && c <= 'z') c -= 32;
  switch (c) {
    case 'A': return SA | SB | SC | SE | SF | SG | SP;
    case 'B': return SA | SB | SC | SD | SI | SL | SP;
    case 'C': return SA | SD | SE | SF;
    case 'D': return SA | SB | SC | SD | SI | SL;
    case 'E': return SA | SD | SE | SF | SG;
    case 'F': return SA | SE | SF | SG;
    case 'G': return SA | SC | SD | SE | SF | SP;
    case 'H': return SB | SC | SE | SF | SG | SP;
    case 'I': return SA | SD | SI | SL;
    case 'J': return SB | SC | SD | SE;
    case 'K': return SE | SF | SG | SJ | SM;
    case 'L': return SD | SE | SF;
    case 'M': return SB | SC | SE | SF | SH | SJ;
    case 'N': return SB | SC | SE | SF | SH | SM;
    case 'O': case '0': return SA | SB | SC | SD | SE | SF;
    case 'P': return SA | SB | SE | SF | SG | SP;
    case 'Q': return SA | SB | SC | SD | SE | SF | SM;
    case 'R': return SA | SB | SE | SF | SG | SP | SM;
    case 'S': case '5': return SA | SC | SD | SF | SG | SP;
    case 'T': return SA | SI | SL;
    case 'U': return SB | SC | SD | SE | SF;
    case 'V': return SE | SF | SK | SJ;
    case 'W': return SB | SC | SE | SF | SK | SM;
    case 'X': return SH | SJ | SK | SM;
    case 'Y': return SH | SJ | SL;
    case 'Z': return SA | SD | SJ | SK;
    case '1': return SB | SC | SJ;
    case '2': return SA | SB | SD | SE | SG | SP;
    case '3': return SA | SB | SC | SD | SP;
    case '4': return SB | SC | SF | SG | SP;
    case '6': return SA | SC | SD | SE | SF | SG | SP;
    case '7': return SA | SB | SC;
    case '8': return SA | SB | SC | SD | SE | SF | SG | SP;
    case '9': return SA | SB | SC | SD | SF | SG | SP;
    case '-': return SG | SP;
    case '+': return SG | SP | SI | SL;
    case '*': return SG | SP | SH | SI | SJ | SK | SL | SM;
    case '/': return SJ | SK;
    case '=': return SG | SP | SD;
    case '_': return SD;
    case '(': case '<': return SJ | SM;
    case ')': case '>': return SH | SK;
    case '\'': return SJ;
    case '"': return SF | SI;
    case '$': return SA | SC | SD | SF | SG | SP | SI | SL;
    case '%': return SC | SF | SJ | SK;
    case '&': return SA | SD | SE | SH | SG | SM;  // close enough
    case '?': return SA | SB | SP | SL | SDOT;
    case '!': return SI | SDOT;
    case '.': return SDOT;
    case ',': return SK;
    case ':': case ';': return SI | SL;
    default: return 0;  // space and anything else
  }
}

static constexpr uint16_t rgb565(int r, int g, int b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static const uint16_t VFD_ON = rgb565(110, 245, 235);    // blue-green phosphor
static const uint16_t VFD_GLOW = rgb565(0, 85, 100);
static const uint16_t VFD_GHOST = rgb565(8, 18, 26);    // unlit segments
static const uint16_t VFD_GLASS = rgb565(6, 10, 18);
static const uint16_t VFD_RED = rgb565(205, 32, 28);
static const uint16_t VFD_RED_DARK = rgb565(120, 14, 12);

static const int VFD_X = 4, VFD_Y = 152, VFD_W = W - 8, VFD_H = 84;  // bezel
static const int VFD_CELLS = 10, VFD_CW = 28;  // character cells, pitch
static const int VFD_GW = 18, VFD_GH = 40;     // glyph size
static const float VFD_SLANT = 0.16f;
static const float VFD_CPS = 7.0f;             // marquee speed, chars/second

static void vfdSegments(int x0, int y0, uint16_t segs, int width, uint16_t color) {
  const int gw = VFD_GW, gh = VFD_GH;
  // Each segment as two points in an unslanted (u, v) box, v down
  static const int8_t seg[15][4] = {
      {2, 0, 16, 0},     {18, 2, 18, 18},  {18, 22, 18, 38}, {2, 40, 16, 40},
      {0, 22, 0, 38},    {0, 2, 0, 18},    {2, 20, 7, 20},   {11, 20, 16, 20},
      {3, 3, 7, 17},     {9, 2, 9, 18},    {15, 3, 11, 17},  {7, 23, 3, 37},
      {9, 22, 9, 38},    {11, 23, 15, 37}, {0, 0, 0, 0}};
  for (int s = 0; s < 14; s++) {
    if (!(segs & (1 << s))) continue;
    const int8_t* p = seg[s];
    float x1 = x0 + p[0] + (gh - p[1]) * VFD_SLANT, y1 = y0 + p[1];
    float x2 = x0 + p[2] + (gh - p[3]) * VFD_SLANT, y2 = y0 + p[3];
    canvas.drawWideLine(x1, y1, x2, y2, width / 2.0f, color);
  }
  if (segs & SDOT) canvas.fillCircle(x0 + gw + 5, y0 + gh, width / 2 + 1, color);
}

// Shows text scrolled so that `charPos` characters have entered from the right
static void drawVFD(const String& text, float charPos) {
  canvas.fillRoundRect(VFD_X, VFD_Y, VFD_W, VFD_H, 12, VFD_RED);
  canvas.drawRoundRect(VFD_X, VFD_Y, VFD_W, VFD_H, 12, VFD_RED_DARK);
  int gx = VFD_X + 10, gy = VFD_Y + 10, gwid = VFD_W - 20, ght = VFD_H - 20;
  canvas.fillRoundRect(gx, gy, gwid, ght, 6, VFD_GLASS);
  canvas.drawFastHLine(gx + 8, gy + 3, gwid - 16, rgb565(30, 36, 48));  // glass glint

  int left = gx + (gwid - VFD_CELLS * VFD_CW) / 2 + 2;
  int top = gy + (ght - VFD_GH) / 2;
  int shift = (int)floorf(charPos);  // whole characters, like the real thing
  for (int cell = 0; cell < VFD_CELLS; cell++) {
    int x = left + cell * VFD_CW;
    vfdSegments(x, top, 0x7FFF & ~SDOT, 3, VFD_GHOST);
    int idx = shift - VFD_CELLS + cell;
    if (idx < 0 || idx >= (int)text.length()) continue;
    uint16_t g = vfdGlyph(text[idx]);
    if (!g) continue;
    vfdSegments(x, top, g, 7, VFD_GLOW);
    vfdSegments(x, top, g, 3, VFD_ON);
  }
}

// Characters that must enter for the whole text to pass through and leave
static float vfdPassLength(const String& text) { return text.length() + VFD_CELLS; }

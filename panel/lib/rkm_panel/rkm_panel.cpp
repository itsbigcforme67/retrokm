// RetroKM touch panel for the M5Stack Tab5 (1280x720): the desk map.
//
// Top: the monitors as they stand on the desk.  Drag one to move it; the
// hub's screen edges follow.  Bottom: the machines.  Drag from a machine to
// a monitor to switch the Extron so that monitor shows it; drag a cable's
// plug off a monitor to unplug it.  Tap a machine to give it the keyboard.
//
// A component (rkm_panel.h): the panel's own firmware (src/main.cpp) runs it
// full time, and lil' C's Tab5 firmware opens it from his desktop.  The same
// code runs in a window on Linux (env:native, SDL2) for testing.
#include <M5Unified.h>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>
#include "rkm_panel.h"
#include "model.h"
#include "net.h"
#include "art.h"
#if !defined(PANEL_NATIVE)
#include <WiFi.h>
#endif

uint32_t nowMs() {
#if defined(PANEL_NATIVE)
  using namespace std::chrono;
  static auto t0 = steady_clock::now();
  return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - t0).count();
#else
  return millis();
#endif
}

// ------------------------------------------------------------ look

static const int SW = 1280, SH = 720;

// Colours are written as 0xRRGGBB; hand them to the library as rgb888 (a
// bare integer would be taken as a raw 565 value)
static inline lgfx::rgb888_t C(uint32_t v) { return lgfx::rgb888_t(v >> 16, (v >> 8) & 255, v & 255); }
static const uint32_t BG = 0x15181D, PANEL_BG = 0x1E232B, LINE = 0x2E3540;
static const uint32_t TEXT = 0xE8ECF1, MUTED = 0x8A93A0, OK_C = 0x34D399, BAD_C = 0xF87171;
static const uint32_t ACCENT = 0x34D399;

static const int HEAD_H = 56;
static const int AX = 20, AY = 66, AW = 1240, AH = 380;  // desk map
static const int CY = 482, CH = 204;                      // machine cards

// The frame lives in the panel's own layout (720x1280 portrait, 565 not
// byte-swapped), drawn on through setRotation(1), so it reaches the screen
// as straight row copies (the trick from lil' C).
static M5Canvas frame(&M5.Display);  // what goes to the screen
static M5Canvas scene(&M5.Display);  // the desk without whatever is under the finger
static M5Canvas* G = &frame;         // the one being drawn on

// Lands on the screen in two layers, as in lil' C: the scene is redrawn only
// when something changes (the layout, a drag starting or ending, a toast);
// while a finger moves, only the patch under the dragged thing is restored
// from the scene, redrawn, and sent to the panel.
struct Box {
  int x0 = 1 << 20, y0 = 1 << 20, x1 = -(1 << 20), y1 = -(1 << 20);
  bool empty() const { return x0 > x1; }
  void add(int ax, int ay, int bx, int by) {
    x0 = std::min(x0, std::min(ax, bx)); y0 = std::min(y0, std::min(ay, by));
    x1 = std::max(x1, std::max(ax, bx)); y1 = std::max(y1, std::max(ay, by));
  }
  void add(const Box& b) { if (!b.empty()) add(b.x0, b.y0, b.x1, b.y1); }
};
static Box ink;                      // what the finger layer covers this frame
static bool sceneDirty = true, fingerDirty = false;

static Layout lay;
static HubLink hub;
static bool embedded = false, wantBack = false;  // inside lil' C: a back button
static std::string toast;
static uint32_t toastUntil = 0;

static void say(const std::string& s) {
  toast = s;
  toastUntil = nowMs() + 3500;
  sceneDirty = true;
}

// ------------------------------------------------------------ geometry

struct Rect {
  int x, y, w, h;
  bool has(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

struct Grid {
  int cmin = 0, rmin = 0, ncol = 1, nrow = 1;
  float cw = 200, ch = 150, ox = 0, oy = 0;
  Rect cell(int c, int r) const { return {(int)(ox + c * cw), (int)(oy + r * ch), (int)cw, (int)ch}; }
  void at(int x, int y, int& c, int& r) const {
    c = (int)floorf((x - ox) / cw);
    r = (int)floorf((y - oy) / ch);
  }
};
static Grid grid;
static bool gridFrozen = false;  // while a monitor is being dragged

static void computeGrid() {
  if (gridFrozen || lay.monitors.empty()) return;
  int c0 = 1 << 20, c1 = -(1 << 20), r0 = c0, r1 = c1;
  for (auto& m : lay.monitors) {
    c0 = std::min(c0, m.col); c1 = std::max(c1, m.col);
    r0 = std::min(r0, m.row); r1 = std::max(r1, m.row);
  }
  grid.cmin = c0; grid.rmin = r0;
  grid.ncol = c1 - c0 + 1; grid.nrow = r1 - r0 + 1;
  float ch = std::min(230.0f, (float)AH / grid.nrow);
  float cw = std::min({270.0f, (float)AW / grid.ncol, ch * 1.45f});
  ch = std::min(ch, cw / 1.2f);
  grid.cw = cw; grid.ch = ch;
  grid.ox = AX + (AW - grid.ncol * cw) / 2 - c0 * cw;
  grid.oy = AY + (AH - grid.nrow * ch) / 2 - r0 * ch;
}

// Parts of a monitor drawn in a cell
struct MonGeo {
  Rect screen, bezel, all;
  int neckX, neckY, baseY, plugX, plugY, labelY;
};
static MonGeo monGeo(const Rect& c, bool portrait = false, bool capture = false) {
  MonGeo m;
  int mx = (int)(c.w * 0.07f);
  int sw = c.w - 2 * mx, sh = (int)(c.h * 0.56f);
  if (portrait) {  // a monitor turned on its side
    sh = (int)(c.h * 0.66f);
    sw = (int)(sh * 0.6f);
    mx = (c.w - sw) / 2;
  }
  if (capture) {  // a small box with a preview window
    sw = (int)(c.w * 0.62f);
    sh = (int)(sw * 0.56f);
    mx = (c.w - sw) / 2;
  }
  m.screen = {c.x + mx, c.y + (int)(c.h * 0.05f) + 5, sw, sh};
  m.bezel = {m.screen.x - 6, m.screen.y - 6, sw + 12, sh + 12};
  m.neckX = c.x + c.w / 2;
  m.neckY = m.bezel.y + m.bezel.h;
  m.baseY = m.neckY + (int)(c.h * 0.07f);
  if (capture) {  // no stand; the box is the bezel plus a strip below the preview
    m.bezel = {m.screen.x - 10, m.screen.y - 10, sw + 20, sh + 44};
    m.neckY = m.baseY = m.bezel.y + m.bezel.h;
  }
  m.labelY = m.baseY + 18;
  m.plugX = m.neckX;
  m.plugY = m.labelY + 34;  // below the name (and the "tap" hint), so cables miss the text
  m.all = {m.bezel.x, m.bezel.y, m.bezel.w, m.plugY + 8 - m.bezel.y};
  return m;
}

static MonGeo geoOf(const Monitor& m, const Rect& cell) { return monGeo(cell, m.portrait, m.capture); }
static MonGeo geoOf(const Monitor& m) { return geoOf(m, grid.cell(m.col, m.row)); }
// Its cable is not a switcher cable: wired straight in, or the shared input
static bool directLink(const Monitor& m) { return !m.fixed.empty() || m.sharedOn; }

// A machine's colour: what the hub says (picked by hand, or its default)
static uint32_t colorOf(int i) {
  if (i < 0 || i >= (int)lay.machines.size()) return 0x3A414C;
  const Machine& m = lay.machines[i];
  return m.color ? m.color : machineColor(m.art, i);
}

static Rect cardRect(int i) {
  int n = std::max<int>(1, lay.machines.size());
  int gap = 16;
  int w = std::min(290, (AW - (n - 1) * gap) / n);
  int x0 = AX + (AW - (n * w + (n - 1) * gap)) / 2;
  return {x0 + i * (w + gap), CY, w, CH};
}
static void plugPos(int i, int& x, int& y) {
  Rect r = cardRect(i);
  x = r.x + r.w / 2;
  y = r.y;
}

// ------------------------------------------------------------ touch

enum DragKind { D_NONE, D_PRESS_CARD, D_CABLE, D_PRESS_MON, D_MON, D_PLUG, D_PRESS_LOCK, D_PRESS_BACK,
                D_LONG,        // the long press that opened the options: ignore until let go
                D_MENU_TAP,    // a press on an options button (dragIdx = which)
                D_PICK_SV, D_PICK_HUE };
static uint32_t pressAt;  // when the finger went down
static DragKind drag = D_NONE;
static int dragIdx = -1;          // machine (cable) or monitor (mon, plug)
static int pressX, pressY, curX, curY;
static Rect lockBtn{0, 0, 0, 0};
static Rect backBtn{0, 0, 0, 0};

static int monitorAt(int x, int y) {
  for (size_t i = 0; i < lay.monitors.size(); i++) {
    MonGeo geo = geoOf(lay.monitors[i]);
    if (geo.all.has(x, y)) return (int)i;
  }
  return -1;
}
static int plugAt(int x, int y) {  // a monitor's cable plug
  for (size_t i = 0; i < lay.monitors.size(); i++) {
    auto& m = lay.monitors[i];
    if (directLink(m) || m.shows.empty()) continue;
    MonGeo geo = geoOf(m);
    int dx = x - geo.plugX, dy = y - geo.plugY;
    if (dx * dx + dy * dy < 30 * 30) return (int)i;
  }
  return -1;
}
static int cardAt(int x, int y) {
  for (size_t i = 0; i < lay.machines.size(); i++)
    if (cardRect(i).has(x, y)) return (int)i;
  int px, py;
  for (size_t i = 0; i < lay.machines.size(); i++) {  // the knob sticks out
    plugPos(i, px, py);
    if ((x - px) * (x - px) + (y - py) * (y - py) < 26 * 26) return (int)i;
  }
  return -1;
}

static const char* labelOf(const std::string& name) {
  Machine* m = lay.machine(name);
  return m ? m->label.c_str() : name.c_str();
}

// Plugging machine mi into monitor ni (from a dragged cable)
static void plugInto(int mi, int ni, int fromMon = -1) {
  Machine& mc = lay.machines[mi];
  Monitor& mon = lay.monitors[ni];
  if (mon.shared == mc.name) {  // it is on the monitor's other input: switch the monitor over
    if (mon.sharedOn) { say(mc.label + " is already on the " + mon.name + " monitor"); return; }
    hub.send("share " + mon.name + " on");
    mon.sharedOn = true;
    mon.shows = mc.name;
    say((mon.ddc == 1 ? "Switching the " + mon.name + " monitor to " : "The " + mon.name + " monitor shows ") +
        mc.label);
    return;
  }
  if (!mon.fixed.empty()) {
    say(mon.fixed == mc.name ? std::string("That monitor is already ") + mc.label + "'s own screen"
                             : std::string("That monitor is wired straight to ") + labelOf(mon.fixed));
    return;
  }
  if (!mc.input) { say(mc.label + " is not wired to the switcher"); return; }
  if (!lay.switcherOnline) { say("The switcher is not connected to the hub"); return; }
  if (fromMon >= 0 && fromMon != ni) {
    hub.send("untie " + lay.monitors[fromMon].name);
    lay.monitors[fromMon].shows.clear();
  }
  hub.send("tie " + mc.name + " " + mon.name);
  if (mon.sharedOn) {
    mon.sharedOn = false;
    if (mon.ddc == 1) say("Switching the " + mon.name + " monitor to VGA");
    else say("Now set the " + mon.name + " monitor to its VGA input");
  }
  mon.shows = mc.name;  // the hub confirms in a moment
}

// ------------------------------------------------------------ machine options

// Long-press a machine: a sheet with its colour (a hue bar and a
// saturation/brightness square) and a button to give it the keyboard.
static int menuIdx = -1;
static float pickH, pickS, pickV;
static uint32_t lastColorSend;
static const Rect MENU{250, 100, 780, 520};
static const Rect SV{290, 250, 300, 210};
static const Rect HUE{290, 486, 300, 30};
static const Rect PREVIEW{640, 250, 350, 96};
static const Rect BTN_DEFAULT{640, 372, 166, 54};
static const Rect BTN_USE{824, 372, 166, 54};
static const Rect BTN_DONE{640, 462, 350, 54};
enum { B_DEFAULT = 1, B_USE, B_DONE };

static uint32_t hsv(float h, float s, float v) {
  float c = v * s, x = c * (1 - fabsf(fmodf(h / 60.0f, 2) - 1)), m = v - c, r, g, b;
  if (h < 60) { r = c; g = x; b = 0; } else if (h < 120) { r = x; g = c; b = 0; }
  else if (h < 180) { r = 0; g = c; b = x; } else if (h < 240) { r = 0; g = x; b = c; }
  else if (h < 300) { r = x; g = 0; b = c; } else { r = c; g = 0; b = x; }
  auto q = [&](float f) { return (uint32_t)((f + m) * 255 + 0.5f); };
  return q(r) << 16 | q(g) << 8 | q(b);
}

static void toHsv(uint32_t rgb, float& h, float& s, float& v) {
  float r = ((rgb >> 16) & 255) / 255.0f, g = ((rgb >> 8) & 255) / 255.0f, b = (rgb & 255) / 255.0f;
  float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
  v = mx;
  s = mx > 0 ? d / mx : 0;
  if (d == 0) h = 0;
  else if (mx == r) h = 60 * fmodf((g - b) / d, 6);
  else if (mx == g) h = 60 * ((b - r) / d + 2);
  else h = 60 * ((r - g) / d + 4);
  if (h < 0) h += 360;
}

static void openMenu(int i) {
  menuIdx = i;
  toHsv(colorOf(i), pickH, pickS, pickV);
  sceneDirty = true;
}

static std::string hex6(uint32_t rgb) {
  char b[8];
  snprintf(b, sizeof b, "%06X", (unsigned)(rgb & 0xFFFFFF));
  return b;
}

// Apply the picked colour: locally at once, to the hub at most ~8 times a second
static void pickTo(int x, int y, bool final) {
  if (drag == D_PICK_SV) {
    pickS = std::min(1.0f, std::max(0.0f, (x - SV.x) / (float)(SV.w - 1)));
    pickV = std::min(1.0f, std::max(0.0f, 1 - (y - SV.y) / (float)(SV.h - 1)));
  } else if (drag == D_PICK_HUE) {
    pickH = std::min(359.9f, std::max(0.0f, (x - HUE.x) * 360.0f / HUE.w));
  }
  Machine& m = lay.machines[menuIdx];
  m.color = hsv(pickH, pickS, pickV);
  m.colorSet = true;
  if (final || nowMs() - lastColorSend > 120) {
    lastColorSend = nowMs();
    hub.send("color " + m.name + " " + hex6(m.color));
  }
  sceneDirty = true;
}

static void menuButton(int b) {
  Machine& m = lay.machines[menuIdx];
  if (b == B_DEFAULT) {
    hub.send("color " + m.name + " default");
    m.color = 0;  // the hub sends the real one back
    m.colorSet = false;
    toHsv(colorOf(menuIdx), pickH, pickS, pickV);
  } else if (b == B_USE) {
    if (!m.agent) say(m.label + " is video only: no keyboard or mouse");
    else if (!m.ready) say(m.label + " is offline");
    else { hub.send("goto " + m.name); lay.active = m.name; menuIdx = -1; }
  } else if (b == B_DONE) {
    menuIdx = -1;
  }
  sceneDirty = true;
}

static void release() {
  switch (drag) {
  case D_PRESS_CARD: {
    Machine& m = lay.machines[dragIdx];
    if (!m.agent) say(m.label + " is video only: no keyboard or mouse");
    else if (!m.ready) say(m.label + " is offline (its agent is not running)");
    else { hub.send("goto " + m.name); lay.active = m.name; }
    break;
  }
  case D_CABLE: {
    int ni = monitorAt(curX, curY);
    if (ni >= 0) plugInto(dragIdx, ni);
    break;
  }
  case D_PLUG: {
    int ni = monitorAt(curX, curY);
    Monitor& from = lay.monitors[dragIdx];
    int mi = lay.machineIndex(from.shows);
    if (ni == dragIdx) break;
    if (ni >= 0 && mi >= 0) plugInto(mi, ni, dragIdx);
    else if (lay.switcherOnline) { hub.send("untie " + from.name); from.shows.clear(); }
    else say("The switcher is not connected to the hub");
    break;
  }
  case D_MON: {
    int c, r;
    grid.at(curX, curY, c, r);
    Monitor& m = lay.monitors[dragIdx];
    if (c != m.col || r != m.row) {
      for (auto& o : lay.monitors)
        if (&o != &m && o.col == c && o.row == r) { o.col = m.col; o.row = m.row; }
      m.col = c; m.row = r;
      hub.send("move " + m.name + " " + std::to_string(c) + " " + std::to_string(r));
    }
    gridFrozen = false;
    computeGrid();
    break;
  }
  case D_PRESS_MON: {  // a tap: say which input a two-input monitor is on
    Monitor& m = lay.monitors[dragIdx];
    if (m.shared.empty()) break;
    m.sharedOn = !m.sharedOn;
    hub.send("share " + m.name + (m.sharedOn ? " on" : " off"));
    std::string verb = m.ddc == 1 ? "Switching the " + m.name + " monitor to " : "The " + m.name + " monitor shows ";
    if (m.sharedOn) { m.shows = m.shared; say(verb + labelOf(m.shared) + " (HDMI)"); }
    else say(verb + "the switcher, output " + std::to_string(m.output) + " (VGA)");
    break;
  }
  case D_PRESS_LOCK:
    hub.send("lock");
    lay.locked = !lay.locked;
    break;
  case D_PRESS_BACK:
    if (backBtn.has(curX, curY)) wantBack = true;
    break;
  case D_MENU_TAP: {  // still on the button it went down on?
    const Rect& r = dragIdx == B_DEFAULT ? BTN_DEFAULT : dragIdx == B_USE ? BTN_USE : BTN_DONE;
    if (r.has(curX, curY)) menuButton(dragIdx);
    break;
  }
  case D_PICK_SV:
  case D_PICK_HUE:
    pickTo(curX, curY, true);
    break;
  default:
    break;
  }
  drag = D_NONE;
  dragIdx = -1;
  sceneDirty = true;
}

static void touchInput(bool down, int x, int y) {
  static bool was = false;
  if (down && !was) {  // press
    pressX = curX = x; pressY = curY = y;
    int i;
    pressAt = nowMs();
    if (embedded && backBtn.has(x, y)) drag = D_PRESS_BACK;
    else if (!hub.connected() || !lay.valid) drag = D_NONE;
    else if (menuIdx >= 0) {  // the options sheet takes every touch while it is open
      if (SV.has(x, y)) { drag = D_PICK_SV; pickTo(x, y, false); }
      else if (Rect{HUE.x, HUE.y - 10, HUE.w, HUE.h + 20}.has(x, y)) { drag = D_PICK_HUE; pickTo(x, y, false); }
      else if (BTN_DEFAULT.has(x, y)) { drag = D_MENU_TAP; dragIdx = B_DEFAULT; }
      else if (BTN_USE.has(x, y)) { drag = D_MENU_TAP; dragIdx = B_USE; }
      else if (BTN_DONE.has(x, y)) { drag = D_MENU_TAP; dragIdx = B_DONE; }
      else if (!MENU.has(x, y)) { menuIdx = -1; drag = D_NONE; }  // tap outside: close
      else drag = D_NONE;
    }
    else if (lockBtn.has(x, y)) drag = D_PRESS_LOCK;
    else if ((i = plugAt(x, y)) >= 0) { drag = D_PLUG; dragIdx = i; }
    else if ((i = monitorAt(x, y)) >= 0) { drag = D_PRESS_MON; dragIdx = i; }
    else if ((i = cardAt(x, y)) >= 0) { drag = D_PRESS_CARD; dragIdx = i; }
    sceneDirty = true;
  } else if (down && was) {  // move
    if ((x != curX || y != curY) && drag != D_NONE && drag != D_LONG && drag != D_MENU_TAP) fingerDirty = true;
    curX = x; curY = y;
    if (drag == D_PICK_SV || drag == D_PICK_HUE) { pickTo(x, y, false); fingerDirty = false; }
    bool far = abs(x - pressX) + abs(y - pressY) > 14;
    if (drag == D_PRESS_CARD && far) { drag = D_CABLE; sceneDirty = true; }
    if (drag == D_PRESS_MON && far) { drag = D_MON; gridFrozen = true; sceneDirty = true; }
  } else if (!down && was) {
    release();
  }
  was = down;
}

// ------------------------------------------------------------ drawing

static void text(const char* s, int x, int y, uint32_t col, const lgfx::IFont* f,
                 textdatum_t d = middle_center) {
  G->setFont(f);
  G->setTextDatum(d);
  G->setTextColor(lgfx::color565(col >> 16, (col >> 8) & 255, col & 255));  // uint16_t: rgb565
  G->drawString(s, x, y);
}

enum CableStyle { SOLID, FAINT, DOTTED };

// A cable from a machine's knob (x0,y0) up to a monitor's plug (x1,y1).
// DOTTED: a link that exists but is not the one selected (a shared
// monitor's other input).  Adds what it covers to ink.
static void cable(int x0, int y0, int x1, int y1, uint32_t col, CableStyle style = SOLID) {
  float k = std::max(70.0f, fabsf((float)(y0 - y1)) * 0.55f);
  const int N = style == DOTTED ? 36 : 20;
  float xs[40], ys[40];
  for (int i = 0; i <= N; i++) {
    float t = (float)i / N, u = 1 - t;
    // control points: straight up out of the machine, straight down into the monitor
    xs[i] = u * u * u * x0 + 3 * u * u * t * x0 + 3 * u * t * t * x1 + t * t * t * x1;
    ys[i] = u * u * u * y0 + 3 * u * u * t * (y0 - k) + 3 * u * t * t * (y1 + k) + t * t * t * y1;
    ink.add((int)xs[i] - 7, (int)ys[i] - 7, (int)xs[i] + 7, (int)ys[i] + 7);
  }
  if (style == DOTTED) {
    uint32_t c = mix(col, BG, 0.25f);
    for (int i = 0; i < N; i += 2) G->drawWideLine(xs[i], ys[i], xs[i + 1], ys[i + 1], 1.6f, C(c));
    return;
  }
  bool faint = style == FAINT;
  for (int i = 0; i < N; i++) G->drawWideLine(xs[i], ys[i], xs[i + 1], ys[i + 1], faint ? 3.5f : 5.0f, C(0x0A0C0F));
  for (int i = 0; i < N; i++)
    G->drawWideLine(xs[i], ys[i], xs[i + 1], ys[i + 1], faint ? 1.5f : 3.0f, C(faint ? mix(col, BG, 0.55f) : col));
}

// Monitors are numbered for talking about them ("put the Octane on monitor
// 2"): screens left to right, then the capture cards.  lil' C's bridge counts
// the same way.
static int monitorNumber(const Monitor& m) {
  int n = 1;
  for (auto& o : lay.monitors) {
    if (&o == &m || o.name == m.name) continue;
    bool before = o.capture != m.capture ? !o.capture
                : o.row != m.row ? o.row < m.row : o.col < m.col;
    if (before) n++;
  }
  return n;
}

static void drawMonitor(const Monitor& m, const Rect& cell, bool lifted, uint32_t hl) {
  MonGeo geo = geoOf(m, cell);
  bool fixed = !m.fixed.empty();
  Machine* shown = lay.machine(m.shows);
  bool kbd = shown && shown->name == lay.active;
  if (lifted) G->fillRoundRect(geo.bezel.x + 8, geo.bezel.y + 10, geo.bezel.w, geo.bezel.h, 10, C(0x08090B));
  if (m.capture) {  // capture box: dark body, preview window, record light
    G->fillSmoothRoundRect(geo.bezel.x, geo.bezel.y, geo.bezel.w, geo.bezel.h, 12, C(0x202429));
    G->fillSmoothRoundRect(geo.bezel.x + 3, geo.bezel.y + 3, geo.bezel.w - 6, 4, 2, C(0x2E333A));
    int sy = geo.screen.y + geo.screen.h + 17;
    G->fillSmoothCircle(geo.screen.x + 10, sy, 5, C(shown ? 0xEF4444 : 0x4B1D1D));
    text(shown ? "REC" : "idle", geo.screen.x + 22, sy, shown ? 0xF3B4B4 : 0x6B7482, &fonts::Font2,
         middle_left);
    for (int i = 0; i < 3; i++)  // vents
      G->fillRect(geo.screen.x + geo.screen.w - 30 + i * 10, sy - 5, 4, 10, C(0x15181C));
  } else {
    // stand
    int nw = std::max(10, geo.screen.w / 12);
    G->fillRect(geo.neckX - nw / 2, geo.neckY, nw, geo.baseY - geo.neckY, C(0x2A2F37));
    G->fillSmoothRoundRect(geo.neckX - geo.screen.w / 5, geo.baseY, geo.screen.w * 2 / 5, 7, 3, C(0x353B45));
    // bezel
    G->fillSmoothRoundRect(geo.bezel.x, geo.bezel.y, geo.bezel.w, geo.bezel.h, 8, C(fixed ? 0x2B2A2E : 0x262B33));
  }
  const Rect& s = geo.screen;
  if (shown) {
    int mi = lay.machineIndex(shown->name);
    uint32_t col = colorOf(mi);
    bool lit = shown->ready || !shown->agent;
    uint32_t tint = mix(col, 0x0B0D11, lit ? 0.78f : 0.9f);
    G->fillRect(s.x, s.y, s.w, s.h, C(tint));
    float sc = std::min(s.w / 160.0f, s.h / 120.0f) * 0.62f;
    drawArt(*G, shown->art, s.x + s.w / 2, s.y + s.h / 2 - s.h * 0.08f, sc, lit ? 0 : 0.5f, tint);
    const lgfx::IFont* f = s.w < 150 ? (const lgfx::IFont*)&fonts::Font2 : &fonts::FreeSans9pt7b;
    G->setFont(f);
    if (G->textWidth(shown->label.c_str()) <= s.w - 6)  // a tall, narrow screen may not fit it
      text(shown->label.c_str(), s.x + s.w / 2, s.y + s.h - 12, lit ? TEXT : MUTED, f);
  } else {
    G->fillRect(s.x, s.y, s.w, s.h, C(0x0B0D10));
    text(m.output ? "no signal" : "-", s.x + s.w / 2, s.y + s.h / 2, 0x4B5563, &fonts::FreeSans9pt7b);
  }
  if (kbd) {  // this is where the keyboard is
    for (int i = 0; i < 3; i++)
      G->drawRoundRect(geo.bezel.x - 2 - i, geo.bezel.y - 2 - i, geo.bezel.w + 4 + 2 * i,
                      geo.bezel.h + 4 + 2 * i, 10, C(ACCENT));
  }
  if (hl) {
    for (int i = 0; i < 4; i++)
      G->drawRoundRect(geo.bezel.x - 6 - i, geo.bezel.y - 6 - i, geo.bezel.w + 12 + 2 * i,
                      geo.bezel.h + 12 + 2 * i, 12, C(hl));
  }
  {  // its number, on the bezel's corner
    char nb[4];
    snprintf(nb, sizeof nb, "%d", monitorNumber(m));
    int bx = geo.bezel.x + 2, by = geo.bezel.y + 2;
    G->fillSmoothCircle(bx, by, 13, C(0x0A0C0F));
    G->fillSmoothCircle(bx, by, 11, C(0x39414D));
    text(nb, bx, by + 1, TEXT, &fonts::FreeSansBold9pt7b);
  }
  // name and how it is wired
  char buf[64];
  const char* nm = m.label.empty() ? m.name.c_str() : m.label.c_str();
  if (fixed) snprintf(buf, sizeof buf, "%s  -  built in", nm);
  else if (m.sharedOn) snprintf(buf, sizeof buf, "%s  -  HDMI", nm);
  else if (!m.shared.empty()) snprintf(buf, sizeof buf, "%s  -  VGA, out %d", nm, m.output);
  else snprintf(buf, sizeof buf, "%s  -  out %d", nm, m.output);
  text(buf, geo.neckX, geo.labelY, fixed ? MUTED : 0xB8C0CC, &fonts::FreeSans9pt7b);
  if (!m.shared.empty()) {  // tap to say which input it is on
    G->setFont(&fonts::Font2);
    const char* hint = m.ddc == 1 ? "tap to switch input" : "tap to say which input";
    text(hint, geo.neckX, geo.labelY + 17, 0x6B7482, &fonts::Font2);
  }
}

static void drawPlugSocket(const Monitor& m, const Rect& cell) {
  MonGeo geo = geoOf(m, cell);
  Machine* shown = m.sharedOn ? nullptr : lay.machine(m.shows);
  uint32_t col = shown ? colorOf(lay.machineIndex(shown->name)) : 0x3A414C;
  if (!m.fixed.empty()) return;
  G->fillSmoothRoundRect(geo.plugX - 9, geo.plugY - 5, 18, 10, 3, C(shown ? col : 0x3A414C));
  G->drawRoundRect(geo.plugX - 9, geo.plugY - 5, 18, 10, 3, C(0x0A0C0F));
}

static void drawCard(int i, bool pressed) {
  Machine& m = lay.machines[i];
  Rect r = cardRect(i);
  uint32_t col = colorOf(i);
  bool kbd = m.name == lay.active;
  G->fillSmoothRoundRect(r.x, r.y, r.w, r.h, 14, C(pressed ? 0x262D37 : PANEL_BG));
  if (kbd)
    for (int k = 0; k < 3; k++) G->drawRoundRect(r.x + k, r.y + k, r.w - 2 * k, r.h - 2 * k, 14 - k, C(ACCENT));
  else
    G->drawRoundRect(r.x, r.y, r.w, r.h, 14, C(LINE));
  bool lit = m.ready || !m.agent;  // consoles have no agent to wait for
  drawArt(*G, m.art, r.x + r.w / 2, r.y + 84, std::min(1.0f, (r.w - 40) / 160.0f), lit ? 0 : 0.45f,
          pressed ? 0x262D37 : PANEL_BG);
  text(m.label.c_str(), r.x + r.w / 2, r.y + 158, lit ? TEXT : MUTED, &fonts::FreeSansBold12pt7b);
  // status line
  int y = r.y + 188;
  const char* st = !m.agent ? "video only"
                 : !m.ready ? "offline"
                 : kbd ? "keyboard"
                 : m.kbd && !m.soft ? "PS/2 only"   // booting, or no agent: keys still work
                 : m.kbd ? "online + PS/2" : "online";
  char wire[24];
  bool own = false;
  for (auto& mon : lay.monitors) if (mon.fixed == m.name) own = true;
  if (m.input) snprintf(wire, sizeof wire, "in %d", m.input);
  else snprintf(wire, sizeof wire, own ? "own screen" : "not wired");
  G->setFont(&fonts::FreeSans9pt7b);
  int w1 = G->textWidth(st), w2 = G->textWidth(wire);
  int x = r.x + r.w / 2 - (14 + w1 + 22 + w2 + 16) / 2;
  G->fillSmoothCircle(x + 5, y, 5, C(m.ready ? OK_C : 0x59606B));
  text(st, x + 14, y, m.ready ? 0xC9D1DB : MUTED, &fonts::FreeSans9pt7b, middle_left);
  int cx = x + 14 + w1 + 22;
  G->fillSmoothRoundRect(cx - 8, y - 11, w2 + 16, 22, 11, C(0x2B323C));
  text(wire, cx, y, 0xC9D1DB, &fonts::FreeSans9pt7b, middle_left);
  // the knob the cables come out of
  int px, py;
  plugPos(i, px, py);
  G->fillSmoothCircle(px, py, 12, C(0x0A0C0F));
  G->fillSmoothCircle(px, py, 9, C(m.input ? col : 0x4B5563));
}

static int chip(int right, int y, const char* s, uint32_t dot, uint32_t fg = 0xC9D1DB, Rect* out = nullptr) {
  G->setFont(&fonts::FreeSans9pt7b);
  int w = G->textWidth(s) + (dot ? 34 : 24);
  int x = right - w;
  G->fillSmoothRoundRect(x, y - 15, w, 30, 15, C(0x232932));
  if (dot) G->fillSmoothCircle(x + 15, y, 5, C(dot));
  text(s, x + (dot ? 26 : 12), y, fg, &fonts::FreeSans9pt7b, middle_left);
  if (out) *out = {x, y - 15, w, 30};
  return x - 10;
}

static void drawHeader() {
  G->fillRect(0, 0, SW, HEAD_H, C(0x101317));
  G->drawFastHLine(0, HEAD_H, SW, C(LINE));
  int tx0 = 24;
  if (embedded) {  // back to lil' C's desktop
    backBtn = {10, 8, 136, HEAD_H - 16};
    G->fillSmoothRoundRect(backBtn.x, backBtn.y, backBtn.w, backBtn.h, 20,
                           C(drag == D_PRESS_BACK ? 0x3A4250 : 0x262C35));
    // a tiny pair of xeyes
    for (int e = 0; e < 2; e++) {
      int ex = backBtn.x + 26 + e * 17, ey = backBtn.y + backBtn.h / 2;
      G->fillEllipse(ex, ey, 8, 11, C(0x000000));
      G->fillEllipse(ex, ey, 6, 9, C(0xFFFFFF));
      G->fillEllipse(ex - 2, ey + 1, 2, 3, C(0x000000));
    }
    text("lil' C", backBtn.x + 62, HEAD_H / 2, TEXT, &fonts::FreeSansBold12pt7b, middle_left);
    tx0 = backBtn.x + backBtn.w + 18;
  }
  text("RetroKM", tx0, HEAD_H / 2, TEXT, &fonts::FreeSansBold12pt7b, middle_left);
  text("desk", tx0 + 116, HEAD_H / 2 + 1, MUTED, &fonts::FreeSans12pt7b, middle_left);
  int x = SW - 20, y = HEAD_H / 2;
  if (hub.connected() && lay.valid) {
    x = chip(x, y, lay.locked ? "edges locked" : "edges free", 0, lay.locked ? 0xFBBF24 : 0xC9D1DB, &lockBtn);
    std::string k = lay.active.empty() ? std::string("keyboard: nobody") : std::string("keyboard: ") + labelOf(lay.active);
    x = chip(x, y, k.c_str(), ACCENT);
    if (!lay.switcherPresent) x = chip(x, y, "no switcher", 0x59606B);
    else x = chip(x, y, lay.switcherOnline ? "switcher" : "switcher offline", lay.switcherOnline ? OK_C : BAD_C);
    x = chip(x, y, "hub", OK_C);
  } else {
    lockBtn = {0, 0, 0, 0};
    x = chip(x, y, "no hub", BAD_C);
  }
  if (!toast.empty() && nowMs() < toastUntil) {
    G->setFont(&fonts::FreeSans12pt7b);
    int w = G->textWidth(toast.c_str()) + 40;
    int tx = std::max(embedded ? 420 : 250, std::min(SW / 2 - w / 2, x - w));
    G->fillSmoothRoundRect(tx, 9, w, HEAD_H - 18, 19, C(0x3B2F14));
    text(toast.c_str(), tx + w / 2, HEAD_H / 2, 0xFDE68A, &fonts::FreeSans12pt7b);
  }
}

// Machine on a shared monitor's other input (the one not selected)
static int otherInputMachine(const Monitor& m) {
  if (m.shared.empty()) return -1;
  if (!m.sharedOn) return lay.machineIndex(m.shared);
  for (size_t i = 0; i < lay.machines.size(); i++)
    if (m.input && lay.machines[i].input == m.input) return (int)i;
  return -1;
}

static void drawMenu();

static void drawScene() {
  G = &scene;
  computeGrid();
  G->fillScreen( C(BG));
  drawHeader();

  if (!lay.valid) {
    char buf[96];
#if defined(PANEL_NATIVE)
    bool wifi = true;
#else
    bool wifi = WiFi.status() == WL_CONNECTED;
#endif
    if (!wifi) snprintf(buf, sizeof buf, "Joining WiFi...");
    else snprintf(buf, sizeof buf, "Looking for the hub at %s:%d...", hub.host().c_str(), hub.port());
    text(buf, SW / 2, SH / 2, MUTED, &fonts::FreeSans12pt7b);
    return;
  }

  // desk surface
  G->fillSmoothRoundRect(AX - 8, AY - 4, AW + 16, AH + 8, 16, C(0x181C22));
  G->fillSmoothRoundRect(AX - 8, CY - 22, AW + 16, CH + 30, 16, C(0x12151A));
  text("drag a machine onto a monitor   |   pull a plug off to unplug   |   "
       "drag monitors to rearrange   |   tap a machine for the keyboard",
       SW / 2, SH - 9, 0x5B6573, &fonts::FreeSans9pt7b);

  // drop slots next to the monitors while one is being moved
  if (drag == D_MON) {
    for (int r = grid.rmin - 1; r <= grid.rmin + grid.nrow; r++)
      for (int c = grid.cmin - 1; c <= grid.cmin + grid.ncol; c++) {
        Rect cell = grid.cell(c, r);
        if (cell.x < AX - 4 || cell.y < AY - 4 || cell.x + cell.w > AX + AW + 4 || cell.y + cell.h > AY + AH + 4)
          continue;
        bool near = false, taken = false;
        for (auto& m : lay.monitors) {
          if (m.col == c && m.row == r) taken = true;
          if (abs(m.col - c) + abs(m.row - r) == 1) near = true;
        }
        if (!near && !taken) continue;
        MonGeo geo = geoOf(lay.monitors[dragIdx], cell);
        G->drawRoundRect(geo.bezel.x, geo.bezel.y, geo.bezel.w, geo.bezel.h, 8, C(0x39414D));
      }
  }

  // monitors
  for (size_t ni = 0; ni < lay.monitors.size(); ni++) {
    auto& m = lay.monitors[ni];
    if (drag == D_MON && (int)ni == dragIdx) continue;
    uint32_t hl = 0;
    Monitor shown = m;
    if (drag == D_PLUG && (int)ni == dragIdx) shown.shows.clear();  // being unplugged
    Rect cell = grid.cell(m.col, m.row);
    drawMonitor(shown, cell, false, hl);
  }

  // cables, over the monitors so each one can be followed
  for (size_t ni = 0; ni < lay.monitors.size(); ni++) {
    auto& m = lay.monitors[ni];
    int mi = lay.machineIndex(m.shows);
    if (mi < 0) continue;
    if ((drag == D_PLUG || drag == D_MON) && (int)ni == dragIdx) continue;
    MonGeo geo = geoOf(m);
    int px, py;
    plugPos(mi, px, py);
    cable(px, py, geo.plugX, geo.plugY, colorOf(mi), directLink(m) ? FAINT : SOLID);
  }
  // a shared monitor's other input, dotted, into the side of its plug
  for (size_t ni = 0; ni < lay.monitors.size(); ni++) {
    auto& m = lay.monitors[ni];
    int oi = otherInputMachine(m);
    if (oi < 0 || (drag == D_MON && (int)ni == dragIdx)) continue;
    MonGeo geo = geoOf(m);
    int px, py;
    plugPos(oi, px, py);
    cable(px, py, geo.plugX + 16, geo.plugY, colorOf(oi), DOTTED);
  }
  for (size_t ni = 0; ni < lay.monitors.size(); ni++) {
    if (drag == D_MON && (int)ni == dragIdx) continue;
    Monitor shown = lay.monitors[ni];
    if (drag == D_PLUG && (int)ni == dragIdx) shown.shows.clear();
    drawPlugSocket(shown, grid.cell(shown.col, shown.row));
  }

  // machines
  for (size_t i = 0; i < lay.machines.size(); i++)
    drawCard(i, (drag == D_PRESS_CARD || drag == D_CABLE || drag == D_LONG) && (int)i == dragIdx);
  drawMenu();

}

static void button(const Rect& r, const char* label, uint32_t bg, uint32_t fg) {
  G->fillSmoothRoundRect(r.x, r.y, r.w, r.h, 12, C(bg));
  text(label, r.x + r.w / 2, r.y + r.h / 2, fg, &fonts::FreeSansBold12pt7b);
}

static void drawMenu() {
  if (menuIdx < 0 || menuIdx >= (int)lay.machines.size()) { menuIdx = -1; return; }
  Machine& m = lay.machines[menuIdx];
  uint32_t col = colorOf(menuIdx);
  // shade the desk behind it: every other row and column
  for (int y = HEAD_H + 1; y < SH; y += 2) G->drawFastHLine(0, y, SW, C(0x07080A));
  for (int x = 0; x < SW; x += 2) G->drawFastVLine(x, HEAD_H + 1, SH - HEAD_H - 1, C(0x07080A));
  G->fillSmoothRoundRect(MENU.x + 8, MENU.y + 10, MENU.w, MENU.h, 20, C(0x050608));
  G->fillSmoothRoundRect(MENU.x, MENU.y, MENU.w, MENU.h, 20, C(PANEL_BG));
  G->drawRoundRect(MENU.x, MENU.y, MENU.w, MENU.h, 20, C(col));
  drawArt(*G, m.art, MENU.x + 80, MENU.y + 70, 0.55f, 0, PANEL_BG);
  text(m.label.c_str(), MENU.x + 150, MENU.y + 52, TEXT, &fonts::FreeSansBold18pt7b, middle_left);
  text(m.name.c_str(), MENU.x + 152, MENU.y + 92, MUTED, &fonts::FreeSans12pt7b, middle_left);
  text("colour", SV.x, SV.y - 22, MUTED, &fonts::FreeSans12pt7b, middle_left);
  // saturation (across) by brightness (down) for the picked hue
  for (int y = 0; y < SV.h; y++) {
    float v = 1 - y / (float)(SV.h - 1);
    for (int x = 0; x < SV.w; x++) G->drawPixel(SV.x + x, SV.y + y, C(hsv(pickH, x / (float)(SV.w - 1), v)));
  }
  for (int x = 0; x < HUE.w; x++)
    G->drawFastVLine(HUE.x + x, HUE.y, HUE.h, C(hsv(x * 360.0f / HUE.w, 1, 1)));
  // where the pick is
  int cx = SV.x + (int)(pickS * (SV.w - 1)), cy = SV.y + (int)((1 - pickV) * (SV.h - 1));
  G->drawCircle(cx, cy, 9, C(0x000000));
  G->drawCircle(cx, cy, 10, C(0xFFFFFF));
  int hx = HUE.x + (int)(pickH / 360 * HUE.w);
  G->fillRect(hx - 3, HUE.y - 6, 6, HUE.h + 12, C(0xFFFFFF));
  G->drawRect(hx - 4, HUE.y - 7, 8, HUE.h + 14, C(0x000000));
  // preview: what the cables and the keyboard will look like
  G->fillSmoothRoundRect(PREVIEW.x, PREVIEW.y, PREVIEW.w, PREVIEW.h, 14, C(col));
  std::string hx6 = "#" + hex6(col);
  uint32_t ink = ((col >> 16 & 255) * 3 + (col >> 8 & 255) * 6 + (col & 255)) / 10 > 140 ? 0x111111 : 0xFFFFFF;
  text(hx6.c_str(), PREVIEW.x + PREVIEW.w / 2, PREVIEW.y + 34, ink, &fonts::FreeSansBold18pt7b);
  text(m.colorSet ? "picked" : "default", PREVIEW.x + PREVIEW.w / 2, PREVIEW.y + 70, ink, &fonts::FreeSans9pt7b);
  bool usable = m.agent && m.ready;
  button(BTN_DEFAULT, "default", 0x2B323C, m.colorSet ? TEXT : MUTED);
  button(BTN_USE, m.name == lay.active ? "in use" : "use it", usable ? 0x1F4D3A : 0x2B323C,
         usable ? TEXT : MUTED);
  button(BTN_DONE, "done", 0x2B323C, TEXT);
}

static void outline(const MonGeo& geo, uint32_t col) {  // around a monitor, on the finger layer
  for (int i = 0; i < 4; i++)
    G->drawRoundRect(geo.bezel.x - 6 - i, geo.bezel.y - 6 - i, geo.bezel.w + 12 + 2 * i,
                     geo.bezel.h + 12 + 2 * i, 12, C(col));
  ink.add(geo.bezel.x - 12, geo.bezel.y - 12, geo.bezel.x + geo.bezel.w + 12, geo.bezel.y + geo.bezel.h + 12);
}

// Whatever is under the finger, drawn over the scene; ink collects its extent
static void drawFinger() {
  G = &frame;
  if (!lay.valid) return;
  if (drag == D_CABLE || drag == D_PLUG) {  // the monitor it would land on
    int ni = monitorAt(curX, curY);
    if (ni >= 0) {
      auto& m = lay.monitors[ni];
      int mi = drag == D_CABLE ? dragIdx : lay.machineIndex(lay.monitors[dragIdx].shows);
      bool okDrop = m.fixed.empty() && mi >= 0 && lay.machines[mi].input && lay.switcherOnline;
      if (mi >= 0 && m.shared == lay.machines[mi].name) okDrop = true;  // its other input
      if (drag == D_PLUG && ni == dragIdx) okDrop = true;
      outline(geoOf(m), okDrop ? ACCENT : BAD_C);
    }
  }
  if (drag == D_MON) {  // the slot it would land in
    int tc, tr;
    grid.at(curX, curY, tc, tr);
    Rect cell = grid.cell(tc, tr);
    if (cell.x >= AX - 4 && cell.y >= AY - 4 && cell.x + cell.w <= AX + AW + 4 && cell.y + cell.h <= AY + AH + 4)
      outline(geoOf(lay.monitors[dragIdx], cell), ACCENT);
  }
  if (drag == D_CABLE) {
    int px, py;
    plugPos(dragIdx, px, py);
    auto& mc = lay.machines[dragIdx];
    cable(px, py, curX, curY, colorOf(dragIdx));
    G->fillSmoothRoundRect(curX - 10, curY - 6, 20, 12, 3, C(colorOf(dragIdx)));
    ink.add(curX - 12, curY - 8, curX + 12, curY + 8);
  } else if (drag == D_PLUG) {
    auto& from = lay.monitors[dragIdx];
    int mi = lay.machineIndex(from.shows);
    if (mi >= 0) {
      int px, py;
      plugPos(mi, px, py);
      uint32_t col = colorOf(mi);
      cable(px, py, curX, curY, col);
      G->fillSmoothRoundRect(curX - 10, curY - 6, 20, 12, 3, C(col));
      ink.add(curX - 12, curY - 8, curX + 12, curY + 8);
    }
  } else if (drag == D_MON) {
    auto& m = lay.monitors[dragIdx];
    Rect home = grid.cell(m.col, m.row);
    Rect cell = {home.x + curX - pressX, home.y + curY - pressY, home.w, home.h};
    int mi = lay.machineIndex(m.shows);
    if (mi >= 0) {
      MonGeo geo = geoOf(m, cell);
      int px, py;
      plugPos(mi, px, py);
      cable(px, py, geo.plugX, geo.plugY, colorOf(mi), directLink(m) ? FAINT : SOLID);
    }
    drawMonitor(m, cell, true, 0);
    drawPlugSocket(m, cell);
    ink.add(cell.x - 12, cell.y - 12, cell.x + cell.w + 20, cell.y + cell.h + 50);
  }
}

// The sprites are portrait (the panel's own layout) and drawn on through
// setRotation(1); learn where a landscape pixel lands in the buffer.
static int bIdx0, bStepX, bStepY;
static void learnLayout() {
  uint16_t* buf = (uint16_t*)frame.getBuffer();
  auto find = [&](int x, int y) {
    frame.fillScreen(TFT_BLACK);
    frame.drawPixel(x, y, TFT_WHITE);
    for (int i = 0; i < SW * SH; i++) if (buf[i]) return i;
    return 0;
  };
  bIdx0 = find(0, 0);
  bStepX = find(1, 0) - bIdx0;
  bStepY = find(0, 1) - bIdx0;
}

// A landscape box -> the matching rectangle in the portrait buffer
static bool bufRect(const Box& b, int& bx, int& by, int& bw, int& bh) {
  int x0 = std::max(0, b.x0), y0 = std::max(0, b.y0), x1 = std::min(SW - 1, b.x1), y1 = std::min(SH - 1, b.y1);
  if (x0 > x1 || y0 > y1) return false;
  int i1 = bIdx0 + x0 * bStepX + y0 * bStepY, i2 = bIdx0 + x1 * bStepX + y1 * bStepY;
  const int pw = SH;  // the buffer is portrait: SH pixels per row
  int ax = i1 % pw, ay = i1 / pw, cx = i2 % pw, cy = i2 / pw;
  bx = std::min(ax, cx); by = std::min(ay, cy);
  bw = abs(cx - ax) + 1; bh = abs(cy - ay) + 1;
  return true;
}

static void restore(const Box& b) {  // scene -> frame, inside b
  int bx, by, bw, bh;
  if (!bufRect(b, bx, by, bw, bh)) return;
  uint16_t* d = (uint16_t*)frame.getBuffer();
  const uint16_t* s = (const uint16_t*)scene.getBuffer();
  for (int y = by; y < by + bh; y++) memcpy(d + y * SH + bx, s + y * SH + bx, bw * 2);
}

static void push(const Box* b) {  // frame -> panel (null: all of it)
  int bx = 0, by = 0, bw = SH, bh = SW;
  if (b && !bufRect(*b, bx, by, bw, bh)) return;
  M5.Display.setRotation(0);  // unrotated, same pixel format: the library copies rows
  M5.Display.setClipRect(bx, by, bw, bh);
  frame.pushSprite(&M5.Display, 0, 0);
  M5.Display.clearClipRect();
  M5.Display.setRotation(1);
}

static Box lastInk;
static void render() {
  static bool frameStale = true;  // frame does not hold the scene yet
  bool under = drag == D_CABLE || drag == D_PLUG || drag == D_MON;
  if (sceneDirty) {  // the whole thing
    sceneDirty = fingerDirty = false;
    drawScene();
    ink = Box();
    if (!under) {  // nothing over it: show the scene as it is, copy it later if needed
      M5Canvas* f = &scene;
      M5.Display.setRotation(0);
      f->pushSprite(&M5.Display, 0, 0);
      M5.Display.setRotation(1);
      frameStale = true;
      G = &scene;  // (screenshots read G)
      lastInk = ink;
      return;
    }
    memcpy(frame.getBuffer(), scene.getBuffer(), SW * SH * 2);
    frameStale = false;
    drawFinger();
    push(nullptr);
  } else if (fingerDirty) {  // only around the finger
    fingerDirty = false;
    if (frameStale) {
      memcpy(frame.getBuffer(), scene.getBuffer(), SW * SH * 2);
      frameStale = false;
    }
    restore(lastInk);
    ink = Box();
    drawFinger();
    Box both = ink;
    both.add(lastInk);
    push(&both);
  } else {
    return;
  }
  lastInk = ink;
}

// ------------------------------------------------------------ test scripts

#if defined(PANEL_NATIVE)
// PANEL_SCRIPT="d 300 600; m 500 200; s name; u; w 500; q" drives the touch
// screen: d/m/u = down/move/up at x y, w = wait ms, s = save the next frame
// as PANEL_SHOT<name>.ppm, q = quit.  For testing without a finger.
static std::vector<std::string> script;
static size_t scriptPos = 0;
static uint32_t scriptWait = 0;
static bool sDown = false;
static int sX = 0, sY = 0;
static std::string shotName;

static void savePpm(const std::string& name) {
  const char* prefix = getenv("PANEL_SHOT");
  std::string path = std::string(prefix ? prefix : "/tmp/panel_") + name + ".ppm";
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", SW, SH);
  for (int y = 0; y < SH; y++)
    for (int x = 0; x < SW; x++) {
      auto c = G->readPixelRGB(x, y);
      unsigned char px[3] = {c.R8(), c.G8(), c.B8()};
      fwrite(px, 1, 3, f);
    }
  fclose(f);
  printf("saved %s\n", path.c_str());
}

static bool scriptStep() {  // false when there is no script
  if (script.empty()) return false;
  if (nowMs() < scriptWait) return true;
  if (scriptPos >= script.size()) return true;
  std::string st = script[scriptPos++];
  char op = 0;
  int a = 0, b = 0;
  char name[64] = "";
  sscanf(st.c_str(), " %c", &op);
  if (op == 'd' || op == 'm') { sscanf(st.c_str(), " %*c %d %d", &a, &b); sDown = true; sX = a; sY = b; }
  else if (op == 'u') sDown = false;
  else if (op == 'w') { sscanf(st.c_str(), " %*c %d", &a); scriptWait = nowMs() + a; }
  else if (op == 's') { sscanf(st.c_str(), " %*c %63s", name); shotName = name; sceneDirty = true; }
  else if (op == 'q') exit(0);
  return true;
}
#endif

// ------------------------------------------------------------ the component

void rkmPanelBegin(const char* host, int port, bool inside) {
  embedded = inside;
  for (M5Canvas* c : {&frame, &scene}) {
    c->setPsram(true);
    c->setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
    c->createSprite(SH, SW);  // portrait, like the panel
    c->setRotation(1);
  }
  learnLayout();
#if defined(PANEL_NATIVE)
  if (const char* s = getenv("PANEL_SCRIPT")) {
    std::string all = s;
    size_t p = 0;
    while (p <= all.size()) {
      size_t q = all.find(';', p);
      if (q == std::string::npos) q = all.size();
      std::string part = all.substr(p, q - p);
      if (part.find_first_not_of(" ") != std::string::npos) script.push_back(part);
      p = q + 1;
    }
  }
#endif
  hub.begin(host, port);
  hub.onLine = [](const std::string& line) {
    if (line.rfind("layout ", 0) == 0) {
      Layout l;
      if (l.parse(line.substr(7))) {
        // don't yank the ground from under a finger: keep local positions mid-drag
        if (drag == D_MON) {
          for (auto& m : l.monitors)
            if (auto* o = lay.monitor(m.name)) { m.col = o->col; m.row = o->row; }
        }
        bool sameMachines = l.machines.size() == lay.machines.size();
        lay = l;
        if (!sameMachines) drag = D_NONE;
        sceneDirty = true;
      }
    } else if (line.rfind("error ", 0) == 0) {
      say(line.substr(6));
    }
  };
}

void rkmPanelPoll(bool netReady) {
  static bool wasConnected = false;
  hub.poll(netReady);
  if (hub.connected() != wasConnected) {
    wasConnected = hub.connected();
    if (!wasConnected) { lay.valid = false; drag = D_NONE; gridFrozen = false; }
    sceneDirty = true;
  }
}

bool rkmPanelConnected() { return hub.connected() && lay.valid; }

void rkmPanelShow() {
  drag = D_NONE;
  dragIdx = -1;
  menuIdx = -1;
  gridFrozen = false;
  wantBack = false;
  sceneDirty = true;
}

bool rkmPanelLoop() {
  uint32_t now = nowMs();
  static uint32_t lastDraw = 0;

#if defined(PANEL_NATIVE)
  if (scriptStep()) touchInput(sDown, sX, sY);
  else
#endif
  {
    auto t = M5.Touch.getDetail();
    if (t.wasPressed() && !t.isPressed()) {  // a tap shorter than one pass of the loop
      touchInput(true, t.x, t.y);
      touchInput(false, t.x, t.y);
    } else {
      touchInput(t.isPressed(), t.x, t.y);
    }
  }
  if (wantBack) {
    wantBack = false;
    return false;
  }

  // a long press on a machine opens its options
  if (drag == D_PRESS_CARD && nowMs() - pressAt > 550) {
    openMenu(dragIdx);
    drag = D_LONG;
  }

  if (!toast.empty() && now > toastUntil) { toast.clear(); sceneDirty = true; }
  if (!lay.valid && now - lastDraw > 1000) sceneDirty = true;  // "joining WiFi..." etc.

  if (sceneDirty || fingerDirty) {
    bool full = sceneDirty;
    lastDraw = now;
    uint32_t t0 = nowMs();
    render();
    static uint32_t reported = 0;
    if (nowMs() - reported > 1000) {
      reported = nowMs();
      printf("[panel] %s frame %u ms\n", full ? "full" : "finger", (unsigned)(nowMs() - t0));
    }
#if defined(PANEL_NATIVE)
    if (!shotName.empty()) { savePpm(shotName); shotName.clear(); }
#endif
  }
  return true;
}

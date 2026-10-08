// RetroKM touch panel for the M5Stack Tab5 (1280x720).
//
// Top: the monitors as they stand on the desk.  Drag one to move it; the
// hub's screen edges follow.  Bottom: the machines.  Drag from a machine to
// a monitor to switch the Extron so that monitor shows it; drag a cable's
// plug off a monitor to unplug it.  Tap a machine to give it the keyboard.
//
// The same code runs in a window on Linux (env:native, SDL2) for testing.
#include <M5Unified.h>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>
#include "secrets.h"
#include "model.h"
#include "net.h"
#include "art.h"
#if !defined(PANEL_NATIVE)
#include <WiFi.h>
// ESP32-C6 WiFi co-processor on SDIO (M5Stack Tab5 docs)
#define WIFI_SDIO_PINS 12, 13, 11, 10, 9, 8, 15
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

enum DragKind { D_NONE, D_PRESS_CARD, D_CABLE, D_PRESS_MON, D_MON, D_PLUG, D_PRESS_LOCK };
static DragKind drag = D_NONE;
static int dragIdx = -1;          // machine (cable) or monitor (mon, plug)
static int pressX, pressY, curX, curY;
static Rect lockBtn{0, 0, 0, 0};

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
    if (!hub.connected() || !lay.valid) drag = D_NONE;
    else if (lockBtn.has(x, y)) drag = D_PRESS_LOCK;
    else if ((i = plugAt(x, y)) >= 0) { drag = D_PLUG; dragIdx = i; }
    else if ((i = monitorAt(x, y)) >= 0) { drag = D_PRESS_MON; dragIdx = i; }
    else if ((i = cardAt(x, y)) >= 0) { drag = D_PRESS_CARD; dragIdx = i; }
    sceneDirty = true;
  } else if (down && was) {  // move
    if ((x != curX || y != curY) && drag != D_NONE) fingerDirty = true;
    curX = x; curY = y;
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
    uint32_t col = machineColor(shown->art, mi);
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
  uint32_t col = shown ? machineColor(shown->art, lay.machineIndex(shown->name)) : 0x3A414C;
  if (!m.fixed.empty()) return;
  G->fillSmoothRoundRect(geo.plugX - 9, geo.plugY - 5, 18, 10, 3, C(shown ? col : 0x3A414C));
  G->drawRoundRect(geo.plugX - 9, geo.plugY - 5, 18, 10, 3, C(0x0A0C0F));
}

static void drawCard(int i, bool pressed) {
  Machine& m = lay.machines[i];
  Rect r = cardRect(i);
  uint32_t col = machineColor(m.art, i);
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
  const char* st = !m.agent ? "video only" : m.ready ? (kbd ? "keyboard" : "online") : "offline";
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
  text("RetroKM", 24, HEAD_H / 2, TEXT, &fonts::FreeSansBold12pt7b, middle_left);
  text("desk", 140, HEAD_H / 2 + 1, MUTED, &fonts::FreeSans12pt7b, middle_left);
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
    int tx = std::max(250, std::min(SW / 2 - w / 2, x - w));
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
    if (!wifi) snprintf(buf, sizeof buf, "Joining WiFi \"%s\"...", WIFI_SSID);
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
    cable(px, py, geo.plugX, geo.plugY, machineColor(lay.machines[mi].art, mi), directLink(m) ? FAINT : SOLID);
  }
  // a shared monitor's other input, dotted, into the side of its plug
  for (size_t ni = 0; ni < lay.monitors.size(); ni++) {
    auto& m = lay.monitors[ni];
    int oi = otherInputMachine(m);
    if (oi < 0 || (drag == D_MON && (int)ni == dragIdx)) continue;
    MonGeo geo = geoOf(m);
    int px, py;
    plugPos(oi, px, py);
    cable(px, py, geo.plugX + 16, geo.plugY, machineColor(lay.machines[oi].art, oi), DOTTED);
  }
  for (size_t ni = 0; ni < lay.monitors.size(); ni++) {
    if (drag == D_MON && (int)ni == dragIdx) continue;
    Monitor shown = lay.monitors[ni];
    if (drag == D_PLUG && (int)ni == dragIdx) shown.shows.clear();
    drawPlugSocket(shown, grid.cell(shown.col, shown.row));
  }

  // machines
  for (size_t i = 0; i < lay.machines.size(); i++)
    drawCard(i, (drag == D_PRESS_CARD || drag == D_CABLE) && (int)i == dragIdx);

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
    cable(px, py, curX, curY, machineColor(mc.art, dragIdx));
    G->fillSmoothRoundRect(curX - 10, curY - 6, 20, 12, 3, C(machineColor(mc.art, dragIdx)));
    ink.add(curX - 12, curY - 8, curX + 12, curY + 8);
  } else if (drag == D_PLUG) {
    auto& from = lay.monitors[dragIdx];
    int mi = lay.machineIndex(from.shows);
    if (mi >= 0) {
      int px, py;
      plugPos(mi, px, py);
      uint32_t col = machineColor(lay.machines[mi].art, mi);
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
      cable(px, py, geo.plugX, geo.plugY, machineColor(lay.machines[mi].art, mi), directLink(m) ? FAINT : SOLID);
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

// ------------------------------------------------------------ main

void setup() {
  M5.begin();
  M5.Display.setRotation(1);
  M5.Display.setBrightness(140);
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
  const char* host = getenv("PANEL_HUB") ? getenv("PANEL_HUB") : HUB_HOST;
  int port = getenv("PANEL_PORT") ? atoi(getenv("PANEL_PORT")) : HUB_PORT;
  hub.begin(host, port);
#else
  WiFi.setPins(WIFI_SDIO_PINS);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  hub.begin(HUB_HOST, HUB_PORT);
#endif
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

void loop() {
  M5.update();
  uint32_t now = nowMs();
  static bool wasConnected = false;
  static uint32_t lastDraw = 0;

#if defined(PANEL_NATIVE)
  hub.poll(true);
  if (scriptStep()) touchInput(sDown, sX, sY);
  else
#else
  hub.poll(WiFi.status() == WL_CONNECTED);
  static uint32_t lastJoin = 0;
  if (WiFi.status() != WL_CONNECTED && now - lastJoin > 10000) { lastJoin = now; WiFi.reconnect(); }
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

  if (hub.connected() != wasConnected) {
    wasConnected = hub.connected();
    if (!wasConnected) { lay.valid = false; drag = D_NONE; gridFrozen = false; }
    sceneDirty = true;
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
#if defined(PANEL_NATIVE)
  lgfx::delay(8);
#else
  delay(2);
#endif
}

#if defined(PANEL_NATIVE)
int user_func(bool* running) {
  setup();
  do { loop(); } while (*running);
  return 0;
}
int main(int, char**) { return lgfx::Panel_sdl::main(user_func, 128); }
#endif

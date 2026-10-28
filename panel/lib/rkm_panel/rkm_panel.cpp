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

static void computeScale();
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
  computeScale();
}

// A monitor's screen in inches, as it stands (turned or not)
static const float DEFAULT_INCHES = 22;
static void monInches(const Monitor& m, float& w, float& h) {
  float d = m.inches10 > 0 ? m.inches10 / 10.0f : DEFAULT_INCHES;
  float aw = std::max(1, m.aw), ah = std::max(1, m.ah), k = d / sqrtf(aw * aw + ah * ah);
  w = aw * k; h = ah * k;
  if (m.portrait) std::swap(w, h);
}

// The room a screen may take in its cell, and one pixels-per-inch for the
// whole desk, so monitors are drawn to scale against each other
static const float SCREEN_W = 0.86f, SCREEN_H = 0.66f;
static float inchPx = 8;
static void computeScale() {
  float k = 1e9f;
  for (auto& m : lay.monitors) {
    if (m.capture) continue;
    float w, h;
    monInches(m, w, h);
    k = std::min({k, grid.cw * SCREEN_W / w, grid.ch * SCREEN_H / h});
  }
  inchPx = k < 1e8f ? k : 8;
}

// Parts of a monitor drawn in a cell
struct MonGeo {
  Rect screen, bezel, all;
  int neckX, neckY, baseY, plugX, plugY, labelY;
};
static MonGeo monGeo(const Rect& c, const Monitor& mon) {
  MonGeo m;
  int top = c.y + (int)(c.h * 0.05f) + 5, bottom = top + (int)(c.h * SCREEN_H);
  int sw, sh;
  if (mon.capture) {  // a small box with a preview window, the shape of its picture
    float aw = std::max(1, mon.aw), ah = std::max(1, mon.ah);
    sw = (int)(c.w * 0.62f);
    sh = std::min((int)(sw * ah / aw), (int)(c.h * 0.42f));
    sw = std::min(sw, (int)(sh * aw / ah));
    top = c.y + (int)(c.h * 0.05f) + 5;
  } else {  // to scale, standing on the desk: every screen's bottom on one line
    float w, h;
    monInches(mon, w, h);
    sw = std::max(12, (int)(w * inchPx));
    sh = std::max(12, (int)(h * inchPx));
    top = bottom - sh;
  }
  m.screen = {c.x + (c.w - sw) / 2, top, sw, sh};
  m.bezel = {m.screen.x - 6, m.screen.y - 6, sw + 12, sh + 12};
  m.neckX = c.x + c.w / 2;
  m.neckY = m.bezel.y + m.bezel.h;
  m.baseY = bottom + 6 + (int)(c.h * 0.07f);
  if (mon.capture) {  // no stand; the box is the bezel plus a strip below the preview
    m.bezel = {m.screen.x - 10, m.screen.y - 10, sw + 20, sh + 44};
    m.neckY = m.baseY = m.bezel.y + m.bezel.h;
  }
  m.labelY = m.baseY + 18;
  m.plugX = m.neckX;
  m.plugY = m.labelY + 34;  // below the name (and the "tap" hint), so cables miss the text
  m.all = {m.bezel.x, m.bezel.y, m.bezel.w, m.plugY + 8 - m.bezel.y};
  return m;
}

static MonGeo geoOf(const Monitor& m, const Rect& cell) { return monGeo(cell, m); }
static MonGeo geoOf(const Monitor& m) { return geoOf(m, grid.cell(m.col, m.row)); }
// Its cable is not a switcher cable: wired straight in, or the shared input
static bool directLink(const Monitor& m) { return !m.fixed.empty() || m.sharedOn; }

// A machine's colour: what the hub says (picked by hand, or its default)
static uint32_t colorOf(int i) {
  if (i < 0 || i >= (int)lay.machines.size()) return 0x3A414C;
  const Machine& m = lay.machines[i];
  return m.color ? m.color : machineColor(m.art, i);
}

// Slot `i` of `n` cards in a row, centred, at height y
static Rect rowRect(int i, int n, int y, int h, int maxW = 290) {
  n = std::max(1, n);
  int gap = 16;
  int w = std::min(maxW, (AW - (n - 1) * gap) / n);
  int x0 = AX + (AW - (n * w + (n - 1) * gap)) / 2;
  return {x0 + i * (w + gap), y, w, h};
}

// The desk page's cards: machines not hidden behind a box (consoles in the
// RetroTINK live on its page; the RetroTINK's card stands in for them here)
static std::vector<int> desk;  // machine indices, in card order
static void computeDesk() {
  desk.clear();
  for (size_t i = 0; i < lay.machines.size(); i++)
    if (lay.onDesk(i)) desk.push_back((int)i);
}
// The machine whose desk card stands for machine i (itself, or its box)
static int deskOwner(int i) {
  for (int n = 0; i >= 0 && n < 32; n++) {
    if (lay.onDesk(i)) return i;
    i = lay.machineIndex(lay.machines[i].via);
  }
  return -1;
}
static int slotOf(int i) {
  i = deskOwner(i);
  for (size_t k = 0; k < desk.size(); k++) if (desk[k] == i) return (int)k;
  return -1;
}
static Rect cardRect(int slot) { return rowRect(slot, desk.size(), CY, CH); }
static void plugPos(int i, int& x, int& y) {  // the knob of machine i's desk card
  Rect r = cardRect(std::max(0, slotOf(i)));
  x = r.x + r.w / 2;
  y = r.y;
}

// ------------------------------------------------------------ touch

enum DragKind { D_NONE, D_PRESS_CARD, D_CABLE, D_PRESS_MON, D_MON, D_PLUG, D_PRESS_LOCK, D_PRESS_BACK,
                D_PRESS_RESET, // the "reset" chip: the hub starts over
                D_PRESS_TAB,   // a page tab in the header (dragIdx = which)
                D_PRESS_ACC,   // accessories page: a press on an accessory (dragIdx = which)
                D_ACCWIRE,     // ... its line being drawn to a machine
                D_LINEPLUG,    // ... a line's end pulled off a machine (dragIdx = accessory, dragIdx2 = machine)
                D_LONG,        // the long press that opened the options: ignore until let go
                D_MENU_TAP,    // a press on an options button (dragIdx = which)
                D_PICK_SV, D_PICK_HUE };
static uint32_t pressAt;  // when the finger went down
static DragKind drag = D_NONE;
static int dragIdx = -1;          // machine (cable) or monitor (mon, plug)
static int dragIdx2 = -1;         // a second index where a drag needs one
static int pressX, pressY, curX, curY;
static Rect lockBtn{0, 0, 0, 0};
static Rect resetBtn{0, 0, 0, 0};
static Rect backBtn{0, 0, 0, 0};

// ------------------------------------------------------------ pages
//
// The panel shows one page at a time, picked from tabs in the header: the
// desk (monitors and machines), and a page for each box machines go through
// that the hub cannot switch (the RetroTINK: which console is wired into it).
// A new kind of page is a new Page subclass; header, toasts, the options
// sheet and the two-layer drawing are shared.
struct Page {
  virtual ~Page() {}
  virtual std::string key() const = 0;    // stays the same across layout updates
  virtual std::string title() const = 0;  // on its tab
  virtual const char* hint() const = 0;   // the help line along the bottom
  virtual void draw() = 0;                // the page, on the scene layer
  virtual void finger() = 0;              // whatever follows the finger, over it
  virtual void press(int x, int y) = 0;   // sets drag / dragIdx
  virtual void moved(bool far) = 0;       // the finger moved; far: past a tap
  virtual void release() = 0;             // drag ended (page's own kinds)
  virtual bool carrying() const = 0;      // something follows the finger now
};
static Page* page = nullptr;
static std::vector<Rect> tabRects;
static void showPage(int tab);

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
static int cardAt(int x, int y) {  // a machine index
  for (size_t k = 0; k < desk.size(); k++)
    if (cardRect(k).has(x, y)) return desk[k];
  for (size_t k = 0; k < desk.size(); k++) {  // the knob sticks out
    Rect r = cardRect(k);
    int px = r.x + r.w / 2, py = r.y;
    if ((x - px) * (x - px) + (y - py) * (y - py) < 26 * 26) return desk[k];
  }
  return -1;
}

static const char* labelOf(const std::string& name) {
  Machine* m = lay.machine(name);
  return m ? m->label.c_str() : name.c_str();
}

// The machine whose cable reaches the switcher for mi: itself, or the box
// it is behind (the RetroTINK); null if neither is on the switcher
static const Machine* switchedVia(int mi) {
  if (mi < 0) return nullptr;
  const Machine* m = &lay.machines[mi];
  for (int n = 0; m && !m->input && !m->via.empty() && n < 8; n++) m = lay.machine(m->via);
  return m && m->input ? m : nullptr;
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
  // a console behind a box (the RetroTINK): plug it in there, and put the
  // box's way into the switcher on this monitor
  const Machine* road = switchedVia(mi);
  if (!road) { say(mc.label + " is not wired to the switcher"); return; }
  if (!lay.switcherOnline) { say("The switcher is not connected to the hub"); return; }
  if (fromMon >= 0 && fromMon != ni) {
    hub.send("untie " + lay.monitors[fromMon].name);
    lay.monitors[fromMon].shows.clear();
  }
  if (road != &mc) {  // every box on the way gets the one before it
    const Machine* c = &mc;
    for (int n = 0; c != road && n < 8; n++) {
      hub.send("plug " + c->name);
      if (Machine* b = lay.machine(c->via)) b->plugged = c->name;
      c = lay.machine(c->via);
    }
    say(mc.label + " goes through the " + labelOf(mc.via));
  }
  hub.send("tie " + road->name + " " + mon.name);
  if (mon.sharedOn) {
    mon.sharedOn = false;
    if (mon.ddc == 1) say("Switching the " + mon.name + " monitor to VGA");
    else say("Now set the " + mon.name + " monitor to its VGA input");
  }
  mon.shows = mc.name;  // the hub confirms in a moment
}

// ------------------------------------------------------------ options sheet

// Long-press a monitor: a sheet with its shape (16:9, 4:3 ...), whether it
// is turned on its side, and its size, so the desk page draws it to scale.
// Long-press a machine or an accessory: a sheet with its colour (a hue bar
// and a saturation/brightness square), for a machine which of the things
// linked to it it keeps while out of focus and the players of the pads
// linked to it, for an accessory a "works out of focus everywhere" switch,
// its player on each machine and where it is kept, and how it is attached.
static int menuIdx = -1;       // the machine (or accessory, menuAcc) the sheet is about
static bool menuAcc = false;
static bool menuMon = false;   // ... or the monitor menuIdx
static float pickH, pickS, pickV;
static uint32_t lastColorSend;
static const Rect MENU{70, 74, 1140, 630};
static const Rect SV{MENU.x + 30, MENU.y + 158, 300, 186};
static const Rect HUE{MENU.x + 30, MENU.y + 360, 300, 28};
static const Rect PREVIEW{MENU.x + 30, MENU.y + 408, 300, 56};
// Everything tappable on the sheet is listed as it is drawn
enum HitKind { H_DEFAULT, H_USE, H_DONE, H_KEEP, H_KEEPALL, H_PLAYER, H_ASPECT, H_TURN, H_SIZE };
struct Hit { Rect r; HitKind kind; int a, b, c; };
static std::vector<Hit> hits;

// An accessory's colour: picked, or one for its kind and place
static uint32_t accColor(int i) {
  if (i < 0 || i >= (int)lay.accessories.size()) return 0x6B7280;
  const Accessory& a = lay.accessories[i];
  if (a.color >= 0) return (uint32_t)a.color;
  if (a.kind == "kbm") return 0x60A5FA;
  if (a.kind == "keys") return 0xD97757;   // Claude's keyboard
  static const uint32_t pal[] = {0xF472B6, 0xFACC15, 0xA78BFA, 0x2DD4BF, 0xFB923C};
  return pal[i % 5];
}
static uint32_t sheetColor() { return menuAcc ? accColor(menuIdx) : colorOf(menuIdx); }
static const std::string& sheetName() {
  return menuAcc ? lay.accessories[menuIdx].name : lay.machines[menuIdx].name;
}

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

static void openMonMenu(int i) {
  menuIdx = i;
  menuAcc = false;
  menuMon = true;
  sceneDirty = true;
}

// Tell the hub a monitor's shape, and redraw the desk to the new scale
static void sendShape(const Monitor& m) {
  hub.send("shape " + m.name + " " + std::to_string(m.aw) + " " + std::to_string(m.ah) + " " +
           std::to_string(m.inches10) + (m.portrait ? " 1" : " 0"));
  computeGrid();
}

static void openMenu(int i, bool acc = false) {
  menuIdx = i;
  menuAcc = acc;
  menuMon = false;
  toHsv(sheetColor(), pickH, pickS, pickV);
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
  uint32_t c = hsv(pickH, pickS, pickV);
  if (menuAcc) lay.accessories[menuIdx].color = c;
  else { lay.machines[menuIdx].color = c; lay.machines[menuIdx].colorSet = true; }
  if (final || nowMs() - lastColorSend > 120) {
    lastColorSend = nowMs();
    hub.send("color " + sheetName() + " " + hex6(c));
  }
  sceneDirty = true;
}

static void tapCard(int i);

static void menuHit(const Hit& h) {
  switch (h.kind) {
  case H_DEFAULT:
    hub.send("color " + sheetName() + " default");
    if (menuAcc) lay.accessories[menuIdx].color = -1;
    else { lay.machines[menuIdx].color = 0; lay.machines[menuIdx].colorSet = false; }  // the hub sends the real one
    toHsv(sheetColor(), pickH, pickS, pickV);
    break;
  case H_USE:
    tapCard(menuIdx);
    if (lay.active == lay.machines[menuIdx].name) menuIdx = -1;
    break;
  case H_DONE:
    menuIdx = -1;
    break;
  case H_ASPECT: {  // a:b
    Monitor& m = lay.monitors[menuIdx];
    m.aw = h.a; m.ah = h.b;
    sendShape(m);
    break;
  }
  case H_TURN: {  // a = turned
    Monitor& m = lay.monitors[menuIdx];
    m.portrait = h.a != 0;
    sendShape(m);
    break;
  }
  case H_SIZE: {  // a = change, tenths of an inch
    Monitor& m = lay.monitors[menuIdx];
    int in = m.inches10 > 0 ? m.inches10 : (int)(DEFAULT_INCHES * 10);
    m.inches10 = std::min(1000, std::max(30, in + h.a));
    sendShape(m);
    break;
  }
  case H_KEEP: {  // a = accessory, b = machine: that machine gets it out of focus too
    Accessory& a = lay.accessories[h.a];
    const std::string& sc = lay.machines[h.b].name;
    for (auto& l : a.links)
      if (l.screen == sc) {
        l.keep = !l.keep;
        hub.send("keep " + a.name + " " + sc + (l.keep ? " on" : " off"));
      }
    break;
  }
  case H_KEEPALL: {  // a = accessory: it works out of focus everywhere it is linked
    Accessory& a = lay.accessories[h.a];
    a.keepAll = !a.keepAll;
    hub.send("keepall " + a.name + (a.keepAll ? " on" : " off"));
    break;
  }
  case H_PLAYER: {  // a = accessory, b = machine, c = player
    Accessory& a = lay.accessories[h.a];
    const std::string& sc = lay.machines[h.b].name;
    hub.send("link " + a.name + " " + sc + " " + std::to_string(h.c));
    for (auto& l : a.links) if (l.screen == sc) l.player = h.c;
    break;
  }
  }
  sceneDirty = true;
}

// A tap on a machine: give it the keyboard (and the pad)
static void tapCard(int i) {
  Machine& m = lay.machines[i];
  if (!m.via.empty() && !m.ready) say(m.label + " has no controller dongle yet");
  else if (!m.agent) say(m.label + " is video only: no keyboard or mouse");
  else if (!m.ready) say(m.label + " is offline (its agent is not running)");
  else { hub.send("goto " + m.name); lay.active = m.name; }
}

// The desk page's own touches
static void deskRelease() {
  switch (drag) {
  case D_PRESS_CARD:
    tapCard(dragIdx);
    break;
  case D_CABLE: {
    int ni = monitorAt(curX, curY);
    if (ni >= 0) plugInto(dragIdx, ni);
    break;
  }
  case D_PLUG: {
    int ni = monitorAt(curX, curY);
    Monitor& from = lay.monitors[dragIdx];
    int mi = deskOwner(lay.machineIndex(from.shows));  // a console: its box goes with the plug
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
  default:
    break;
  }
}

static void deskPress(int x, int y) {
  int i;
  if ((i = plugAt(x, y)) >= 0) { drag = D_PLUG; dragIdx = i; }
  else if ((i = monitorAt(x, y)) >= 0) { drag = D_PRESS_MON; dragIdx = i; }
  else if ((i = cardAt(x, y)) >= 0) { drag = D_PRESS_CARD; dragIdx = i; }
}

static void deskMoved(bool far) {
  if (drag == D_PRESS_CARD && far) { drag = D_CABLE; sceneDirty = true; }
  if (drag == D_PRESS_MON && far) { drag = D_MON; gridFrozen = true; sceneDirty = true; }
}

// Touches that work the same on every page, then the page's own
static void release() {
  switch (drag) {
  case D_PRESS_LOCK:
    hub.send("lock");
    lay.locked = !lay.locked;
    break;
  case D_PRESS_RESET:  // let go on it: the hub restarts and everything reconnects
    if (resetBtn.has(curX, curY)) { hub.send("reset"); say("resetting the hub"); }
    break;
  case D_PRESS_BACK:
    if (backBtn.has(curX, curY)) wantBack = true;
    break;
  case D_PRESS_TAB:
    if (dragIdx < (int)tabRects.size() && tabRects[dragIdx].has(curX, curY)) showPage(dragIdx);
    break;
  case D_MENU_TAP:  // still on what it went down on?
    if (dragIdx >= 0 && dragIdx < (int)hits.size() && hits[dragIdx].r.has(curX, curY)) menuHit(hits[dragIdx]);
    break;
  case D_PICK_SV:
  case D_PICK_HUE:
    pickTo(curX, curY, true);
    break;
  default:
    if (page) page->release();
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
    drag = D_NONE;
    pressAt = nowMs();
    if (embedded && backBtn.has(x, y)) drag = D_PRESS_BACK;
    else if (!hub.connected() || !lay.valid) drag = D_NONE;
    else if (menuIdx >= 0) {  // the options sheet takes every touch while it is open
      if (menuMon) {
        if (!MENU.has(x, y)) { menuIdx = -1; drag = D_NONE; }
        else for (i = 0; i < (int)hits.size(); i++)
          if (hits[i].r.has(x, y)) { drag = D_MENU_TAP; dragIdx = i; }
      }
      else if (SV.has(x, y)) { drag = D_PICK_SV; pickTo(x, y, false); }
      else if (Rect{HUE.x, HUE.y - 10, HUE.w, HUE.h + 20}.has(x, y)) { drag = D_PICK_HUE; pickTo(x, y, false); }
      else if (!MENU.has(x, y)) { menuIdx = -1; drag = D_NONE; }  // tap outside: close
      else {
        for (i = 0; i < (int)hits.size(); i++)
          if (hits[i].r.has(x, y)) { drag = D_MENU_TAP; dragIdx = i; }
      }
    }
    else if (lockBtn.has(x, y)) drag = D_PRESS_LOCK;
    else if (resetBtn.has(x, y)) drag = D_PRESS_RESET;
    else {
      for (i = 0; i < (int)tabRects.size(); i++)
        if (tabRects[i].has(x, y)) { drag = D_PRESS_TAB; dragIdx = i; }
      if (drag != D_PRESS_TAB && page) page->press(x, y);
    }
    sceneDirty = true;
  } else if (down && was) {  // move
    if ((x != curX || y != curY) && drag != D_NONE && drag != D_LONG && drag != D_MENU_TAP) fingerDirty = true;
    curX = x; curY = y;
    if (drag == D_PICK_SV || drag == D_PICK_HUE) { pickTo(x, y, false); fingerDirty = false; }
    bool far = abs(x - pressX) + abs(y - pressY) > 14;
    if (page) page->moved(far);
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

// The first font in the list that fits s into w (the last one otherwise)
static const lgfx::IFont* fit(const char* s, int w, std::initializer_list<const lgfx::IFont*> fonts) {
  const lgfx::IFont* f = nullptr;
  for (auto* c : fonts) {
    f = c;
    G->setFont(c);
    if (G->textWidth(s) <= w) break;
  }
  return f;
}

// s cut down with "..." until it fits w in font f
static std::string ellipsize(const std::string& s, int w, const lgfx::IFont* f) {
  G->setFont(f);
  if (G->textWidth(s.c_str()) <= w) return s;
  std::string t = s;
  while (!t.empty() && G->textWidth((t + "...").c_str()) > w) t.pop_back();
  return t + "...";
}

// A little game pad, centred on x,y: the badge for "a controller is linked"
static void padGlyph(int x, int y, float k, uint32_t col, uint32_t bg) {
  G->fillSmoothRoundRect(x - (int)(13 * k), y - (int)(6 * k), (int)(26 * k), (int)(12 * k), (int)(6 * k), C(col));
  G->fillSmoothCircle(x - (int)(10 * k), y + (int)(4 * k), (int)(5 * k), C(col));   // grips
  G->fillSmoothCircle(x + (int)(10 * k), y + (int)(4 * k), (int)(5 * k), C(col));
  G->fillRect(x - (int)(10 * k), y - (int)(1 * k), (int)(5 * k), (int)(2 * k), C(bg));  // d-pad
  G->fillRect(x - (int)(8.5f * k), y - (int)(3 * k), (int)(2 * k), (int)(6 * k), C(bg));
  G->fillSmoothCircle(x + (int)(7 * k), y - (int)(1 * k), (int)(1.6f * k), C(bg));   // buttons
  G->fillSmoothCircle(x + (int)(10 * k), y + (int)(1 * k), (int)(1.6f * k), C(bg));
}

// A machine's card: picture, name, a status line, the knob on top its
// cable comes out of, and a controller badge in the corner when a pad is
// linked to it.  The same card on both pages.
static void drawCard(int i, const Rect& r, bool pressed, bool knob = true) {
  Machine& m = lay.machines[i];
  uint32_t col = colorOf(i), bg = pressed ? 0x262D37 : PANEL_BG;
  bool kbd = m.name == lay.active;
  bool console = !m.via.empty();
  G->fillSmoothRoundRect(r.x, r.y, r.w, r.h, 14, C(bg));
  if (kbd)
    for (int k = 0; k < 3; k++) G->drawRoundRect(r.x + k, r.y + k, r.w - 2 * k, r.h - 2 * k, 14 - k, C(ACCENT));
  else
    G->drawRoundRect(r.x, r.y, r.w, r.h, 14, C(LINE));
  bool lit = m.ready || !m.agent || console;  // consoles have no agent to wait for
  float sc = std::min(1.0f, (r.w - 30) / 160.0f);
  drawArt(*G, m.art, r.x + r.w / 2, r.y + (int)(r.h * 0.41f), sc, lit ? 0 : 0.45f, bg);
  const lgfx::IFont* lf = fit(m.label.c_str(), r.w - 14,
                              {&fonts::FreeSansBold12pt7b, &fonts::FreeSansBold9pt7b, &fonts::FreeSans9pt7b, &fonts::Font2});
  text(m.label.c_str(), r.x + r.w / 2, r.y + r.h - 46, lit ? TEXT : MUTED, lf);
  // status line: a dot and what it is doing, and how it is wired if there is room
  int y = r.y + r.h - 16;
  std::string st;
  if (kbd) st = "in focus";
  else if (console) st = m.pads ? "pad dongle" : "no dongle";
  else if (!m.agent) st = "video only";
  else if (!m.ready) st = "offline";
  else if (m.kbd && !m.soft) st = "PS/2 only";   // booting, or no agent: keys still work
  else st = m.kbd ? "online + PS/2" : "online";
  char wire[32];
  bool own = false;
  for (auto& mon : lay.monitors) if (mon.fixed == m.name) own = true;
  Machine* box = console ? lay.machine(m.via) : nullptr;
  if (m.input) snprintf(wire, sizeof wire, "in %d", m.input);
  else if (box) snprintf(wire, sizeof wire, box->plugged == m.name ? "via %s" : "unplugged",
                         box->art == "retrotink" ? "tink" : box->name.c_str());
  else snprintf(wire, sizeof wire, own ? "own screen" : "not wired");
  const lgfx::IFont* sf = &fonts::FreeSans9pt7b;
  G->setFont(sf);
  int w1 = G->textWidth(st.c_str()), w2 = G->textWidth(wire);
  bool both = 14 + w1 + 22 + w2 + 16 <= r.w - 10;
  if (!both && 14 + w1 > r.w - 10) { sf = &fonts::Font2; G->setFont(sf); w1 = G->textWidth(st.c_str()); }
  int x = r.x + r.w / 2 - (both ? 14 + w1 + 22 + w2 + 16 : 14 + w1) / 2;
  bool good = m.ready || (!m.agent && !console);
  G->fillSmoothCircle(x + 5, y, 5, C(good ? OK_C : 0x59606B));
  text(st.c_str(), x + 14, y, good ? 0xC9D1DB : MUTED, sf, middle_left);
  if (both) {
    int cx = x + 14 + w1 + 22;
    G->fillSmoothRoundRect(cx - 8, y - 11, w2 + 16, 22, 11, C(0x2B323C));
    text(wire, cx, y, 0xC9D1DB, &fonts::FreeSans9pt7b, middle_left);
  }
  // a controller linked to it: the badge in the top right corner
  int here = lay.padsLinked(m.name, true), all = lay.padsLinked(m.name, false);
  if (all) {
    int bx = r.x + r.w - 26, by = r.y + 22;
    G->fillSmoothRoundRect(bx - 20, by - 13, 40, 26, 13, C(0x0F1216));
    padGlyph(bx, by, 0.95f, here ? 0xE8ECF1 : 0x59606B, 0x0F1216);
    if (all > 1) {
      char nb[4];
      snprintf(nb, sizeof nb, "%d", all);
      G->fillSmoothCircle(bx + 18, by - 11, 9, C(ACCENT));
      text(nb, bx + 18, by - 10, 0x0B0D10, &fonts::Font2);
    }
  }
  // the knob the cables come out of
  if (!knob) return;
  int px = r.x + r.w / 2, py = r.y;
  bool wired = switchedVia(i) != nullptr;
  G->fillSmoothCircle(px, py, 12, C(0x0A0C0F));
  G->fillSmoothCircle(px, py, 9, C(wired ? col : 0x4B5563));
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

static int pageCount();
static Page* pageAt(int i);

// The page tabs, from x; returns where they end
static int drawTabs(int x) {
  tabRects.clear();
  int n = lay.valid ? pageCount() : 0;
  if (n < 2) {  // just the desk: a plain word, as before there were pages
    text("desk", x + 4, HEAD_H / 2 + 1, MUTED, &fonts::FreeSans12pt7b, middle_left);
    return x + 60;
  }
  G->setFont(&fonts::FreeSans12pt7b);
  for (int i = 0; i < n; i++) {
    Page* p = pageAt(i);
    std::string t = p->title();
    int w = G->textWidth(t.c_str()) + 32;
    Rect r{x, 9, w, HEAD_H - 18};
    bool on = p == page, held = drag == D_PRESS_TAB && dragIdx == i;
    G->fillSmoothRoundRect(r.x, r.y, r.w, r.h, r.h / 2, C(on ? 0x2B3A33 : held ? 0x2A303A : 0x1A1F26));
    if (on) G->drawRoundRect(r.x, r.y, r.w, r.h, r.h / 2, C(ACCENT));
    text(t.c_str(), r.x + w / 2, HEAD_H / 2 + 1, on ? TEXT : MUTED, &fonts::FreeSans12pt7b);
    tabRects.push_back(r);
    x += w + 8;
  }
  return x;
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
  int tabsEnd = drawTabs(tx0 + 112);
  int x = SW - 20, y = HEAD_H / 2;
  if (hub.connected() && lay.valid) {
    x = chip(x, y, lay.locked ? "edges locked" : "edges free", 0, lay.locked ? 0xFBBF24 : 0xC9D1DB, &lockBtn);
    std::string k = lay.active.empty() ? std::string("focus: nobody") : std::string("focus: ") + labelOf(lay.active);
    x = chip(x, y, k.c_str(), ACCENT);
    if (!lay.switcherPresent) x = chip(x, y, "no switcher", 0x59606B);
    else x = chip(x, y, lay.switcherOnline ? "switcher" : "switcher offline", lay.switcherOnline ? OK_C : BAD_C);
    x = chip(x, y, "hub", OK_C);
    x = chip(x, y, "reset", 0, drag == D_PRESS_RESET ? TEXT : 0xC9D1DB, &resetBtn);
  } else {
    lockBtn = {0, 0, 0, 0};
    resetBtn = {0, 0, 0, 0};
    x = chip(x, y, "no hub", BAD_C);
  }
  if (!toast.empty() && nowMs() < toastUntil) {
    G->setFont(&fonts::FreeSans12pt7b);
    int w = G->textWidth(toast.c_str()) + 40;
    int tx = std::max(tabsEnd + 12, std::min(SW / 2 - w / 2, x - w));
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
static void deskDraw();

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
  // the surfaces: the page's top area and the card shelf
  G->fillSmoothRoundRect(AX - 8, AY - 4, AW + 16, AH + 8, 16, C(0x181C22));
  G->fillSmoothRoundRect(AX - 8, CY - 22, AW + 16, CH + 30, 16, C(0x12151A));
  if (page) {
    text(page->hint(), SW / 2, SH - 9, 0x5B6573, &fonts::FreeSans9pt7b);
    page->draw();
  }
  drawMenu();
}

static void deskDraw() {

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
    if (mi < 0 || slotOf(mi) < 0) continue;  // (an empty RetroTINK: no card to come from)
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
    if (oi < 0 || slotOf(oi) < 0 || (drag == D_MON && (int)ni == dragIdx)) continue;
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
  for (size_t k = 0; k < desk.size(); k++)
    drawCard(desk[k], cardRect(k), (drag == D_PRESS_CARD || drag == D_CABLE || drag == D_LONG) && desk[k] == dragIdx);
}

static void button(const Rect& r, const char* label, uint32_t bg, uint32_t fg) {
  G->fillSmoothRoundRect(r.x, r.y, r.w, r.h, 12, C(bg));
  text(label, r.x + r.w / 2, r.y + r.h / 2, fg, &fonts::FreeSansBold12pt7b);
}

// The options sheet's card, over the page shaded every other row and column
static void sheetBackdrop(uint32_t edge) {
  for (int y = HEAD_H + 1; y < SH; y += 2) G->drawFastHLine(0, y, SW, C(0x07080A));
  for (int x = 0; x < SW; x += 2) G->drawFastVLine(x, HEAD_H + 1, SH - HEAD_H - 1, C(0x07080A));
  G->fillSmoothRoundRect(MENU.x + 8, MENU.y + 10, MENU.w, MENU.h, 20, C(0x050608));
  G->fillSmoothRoundRect(MENU.x, MENU.y, MENU.w, MENU.h, 20, C(PANEL_BG));
  G->drawRoundRect(MENU.x, MENU.y, MENU.w, MENU.h, 20, C(edge));
}

static void drawMonMenu() {
  if (menuIdx < 0 || menuIdx >= (int)lay.monitors.size()) { menuIdx = -1; return; }
  const Monitor& m = lay.monitors[menuIdx];
  sheetBackdrop(ACCENT);
  const char* nm = m.label.empty() ? m.name.c_str() : m.label.c_str();
  text(nm, MENU.x + 40, MENU.y + 46, TEXT, fit(nm, MENU.w - 80, {&fonts::FreeSansBold18pt7b, &fonts::FreeSansBold12pt7b}),
       middle_left);
  text(m.capture ? "capture card: the shape of its picture" : "screen shape and size, so the desk is drawn to scale",
       MENU.x + 42, MENU.y + 86, MUTED, &fonts::FreeSans12pt7b, middle_left);

  // column 1: how it will look
  Rect box{MENU.x + 30, MENU.y + 130, 330, 330};
  G->fillSmoothRoundRect(box.x, box.y, box.w, box.h, 14, C(0x0B0D10));
  {
    float w = m.portrait ? m.ah : m.aw, h = m.portrait ? m.aw : m.ah;
    float k = std::min((box.w - 60) / w, (box.h - 60) / h);
    int sw = (int)(w * k), sh = (int)(h * k);
    int sx = box.x + (box.w - sw) / 2, sy = box.y + (box.h - sh) / 2;
    G->fillSmoothRoundRect(sx - 8, sy - 8, sw + 16, sh + 16, 8, C(0x262B33));
    G->fillRect(sx, sy, sw, sh, C(mix(ACCENT, 0x0B0D11, 0.8f)));
    char b[24];
    snprintf(b, sizeof b, "%d:%d", m.portrait ? m.ah : m.aw, m.portrait ? m.aw : m.ah);
    text(b, sx + sw / 2, sy + sh / 2, TEXT, &fonts::FreeSansBold12pt7b);
  }

  // column 2: shape, and which way up
  int x2 = MENU.x + 400, y = MENU.y + 136;
  text("shape", x2, y, MUTED, &fonts::FreeSans12pt7b, middle_left);
  y += 26;
  static const int shapes[][2] = {{4, 3}, {5, 4}, {3, 2}, {16, 10}, {16, 9}, {21, 9}};
  for (int k = 0; k < 6; k++) {
    Rect b{x2 + (k % 3) * 112, y + (k / 3) * 60, 102, 50};
    bool on = m.aw == shapes[k][0] && m.ah == shapes[k][1];
    char t[8];
    snprintf(t, sizeof t, "%d:%d", shapes[k][0], shapes[k][1]);
    button(b, t, on ? 0x1F4D3A : 0x2B323C, on ? TEXT : 0xC9D1DB);
    hits.push_back({b, H_ASPECT, shapes[k][0], shapes[k][1], 0});
  }
  y += 140;
  if (!m.capture) {
    text("stands", x2, y, MUTED, &fonts::FreeSans12pt7b, middle_left);
    y += 26;
    for (int k = 0; k < 2; k++) {
      Rect b{x2 + k * 168, y, 158, 50};
      bool on = m.portrait == (k == 1);
      button(b, k ? "on its side" : "wide", on ? 0x1F4D3A : 0x2B323C, on ? TEXT : 0xC9D1DB);
      hits.push_back({b, H_TURN, k, 0, 0});
    }
  }

  // column 3: size
  int x3 = MENU.x + 790, w3 = 320;
  y = MENU.y + 136;
  if (!m.capture) {
    text("size (diagonal)", x3, y, MUTED, &fonts::FreeSans12pt7b, middle_left);
    y += 26;
    char b[24];
    if (m.inches10 > 0) snprintf(b, sizeof b, "%d.%d\"", m.inches10 / 10, m.inches10 % 10);
    else snprintf(b, sizeof b, "%d\"?", (int)DEFAULT_INCHES);
    text(b, x3 + w3 / 2, y + 44, m.inches10 > 0 ? TEXT : MUTED, &fonts::FreeSansBold24pt7b);
    y += 100;
    static const int steps[] = {-10, -1, 1, 10};
    static const char* names[] = {"-1\"", "-.1", "+.1", "+1\""};
    for (int k = 0; k < 4; k++) {
      Rect r{x3 + k * 80, y, 72, 50};
      button(r, names[k], 0x2B323C, TEXT);
      hits.push_back({r, H_SIZE, steps[k], 0, 0});
    }
  }
  Rect bDone{x3, MENU.y + 548, w3, 54};
  button(bDone, "done", 0x2B323C, TEXT);
  hits.push_back({bDone, H_DONE, 0, 0, 0});
}

static void drawMonMenu();
static void drawMenu() {
  hits.clear();
  if (menuMon) { drawMonMenu(); return; }
  int nItems = menuAcc ? (int)lay.accessories.size() : (int)lay.machines.size();
  if (menuIdx < 0 || menuIdx >= nItems) { menuIdx = -1; return; }
  uint32_t col = sheetColor();
  const std::string& label = menuAcc ? lay.accessories[menuIdx].label : lay.machines[menuIdx].label;
  sheetBackdrop(col);
  // title
  std::string art = menuAcc ? (lay.accessories[menuIdx].kind != "pad" ? "keyboard" : "gamepad") : lay.machines[menuIdx].art;
  drawArt(*G, art, MENU.x + 76, MENU.y + 62, 0.55f, 0, PANEL_BG);
  text(label.c_str(), MENU.x + 150, MENU.y + 46, TEXT,
       fit(label.c_str(), MENU.w - 200, {&fonts::FreeSansBold18pt7b, &fonts::FreeSansBold12pt7b}), middle_left);
  std::string sub = menuAcc ? (lay.accessories[menuIdx].kind == "kbm" ? "keyboard + mouse"
                                       : lay.accessories[menuIdx].kind == "keys" ? "keyboard" : "game controller")
                            : lay.machines[menuIdx].name;
  text(sub.c_str(), MENU.x + 152, MENU.y + 86, MUTED, &fonts::FreeSans12pt7b, middle_left);

  // column 1: colour
  text(menuAcc ? "line colour" : "colour", SV.x, SV.y - 22, MUTED, &fonts::FreeSans12pt7b, middle_left);
  for (int y = 0; y < SV.h; y++) {
    float v = 1 - y / (float)(SV.h - 1);
    for (int x = 0; x < SV.w; x++) G->drawPixel(SV.x + x, SV.y + y, C(hsv(pickH, x / (float)(SV.w - 1), v)));
  }
  for (int x = 0; x < HUE.w; x++) G->drawFastVLine(HUE.x + x, HUE.y, HUE.h, C(hsv(x * 360.0f / HUE.w, 1, 1)));
  int cx = SV.x + (int)(pickS * (SV.w - 1)), cy = SV.y + (int)((1 - pickV) * (SV.h - 1));
  G->drawCircle(cx, cy, 9, C(0x000000));
  G->drawCircle(cx, cy, 10, C(0xFFFFFF));
  int hx = HUE.x + (int)(pickH / 360 * HUE.w);
  G->fillRect(hx - 3, HUE.y - 6, 6, HUE.h + 12, C(0xFFFFFF));
  G->drawRect(hx - 4, HUE.y - 7, 8, HUE.h + 14, C(0x000000));
  G->fillSmoothRoundRect(PREVIEW.x, PREVIEW.y, PREVIEW.w, PREVIEW.h, 12, C(col));
  uint32_t ink = ((col >> 16 & 255) * 3 + (col >> 8 & 255) * 6 + (col & 255)) / 10 > 140 ? 0x111111 : 0xFFFFFF;
  bool picked = menuAcc ? lay.accessories[menuIdx].color >= 0 : lay.machines[menuIdx].colorSet;
  std::string hx6 = "#" + hex6(col) + (picked ? "  picked" : "  default");
  text(hx6.c_str(), PREVIEW.x + PREVIEW.w / 2, PREVIEW.y + PREVIEW.h / 2, ink, &fonts::FreeSansBold12pt7b);
  Rect bDef{SV.x, MENU.y + 480, 300, 50};
  button(bDef, "default colour", 0x2B323C, picked ? TEXT : MUTED);
  hits.push_back({bDef, H_DEFAULT, 0, 0, 0});

  // column 2: what it takes, and who plays it
  int x2 = MENU.x + 380, w2 = 360, y = MENU.y + 136;
  auto playerRow = [&](const std::string& name, int ai, int mi, int cur) {
    const lgfx::IFont* f = fit(name.c_str(), w2 - 4 * 50 - 14, {&fonts::FreeSans12pt7b, &fonts::FreeSans9pt7b});
    text(ellipsize(name, w2 - 4 * 50 - 14, f).c_str(), x2, y + 18, TEXT, f, middle_left);
    for (int p = 1; p <= 4; p++) {
      Rect b{x2 + w2 - (5 - p) * 50, y, 44, 36};
      bool on = p == cur;
      G->fillSmoothRoundRect(b.x, b.y, b.w, b.h, 9, C(on ? accColor(ai) : 0x2B323C));
      text(("P" + std::to_string(p)).c_str(), b.x + b.w / 2, b.y + b.h / 2, on ? 0x0B0D10 : 0xC9D1DB,
           &fonts::FreeSansBold9pt7b);
      hits.push_back({b, H_PLAYER, ai, mi, p});
    }
    y += 46;
  };
  // A labelled switch; tappable unless it is set from elsewhere
  auto toggleRow = [&](const std::string& name, bool on, bool live, Hit hit) {
    Rect sw{x2 + w2 - 66, y, 62, 34};
    const lgfx::IFont* f = fit(name.c_str(), w2 - 80, {&fonts::FreeSans12pt7b, &fonts::FreeSans9pt7b});
    text(ellipsize(name, w2 - 80, f).c_str(), x2, y + 17, live ? TEXT : MUTED, f, middle_left);
    uint32_t bg = on ? (live ? 0x1F7A55 : 0x2F5546) : 0x2B323C;
    G->fillSmoothRoundRect(sw.x, sw.y, sw.w, sw.h, 17, C(bg));
    G->fillSmoothCircle(on ? sw.x + sw.w - 17 : sw.x + 17, sw.y + 17, 13, C(on ? (live ? 0xE8ECF1 : 0x9AA3AE) : 0x8A93A0));
    if (live) { hit.r = Rect{x2, y - 4, w2, 42}; hits.push_back(hit); }
    y += 48;
  };
  if (!menuAcc) {
    Machine& m = lay.machines[menuIdx];
    text("keep while out of focus", x2, y, MUTED, &fonts::FreeSans12pt7b, middle_left);
    y += 26;
    bool anyLinked = false;
    for (size_t ai = 0; ai < lay.accessories.size(); ai++) {
      const Accessory& a = lay.accessories[ai];
      const Link* l = a.link(m.name);
      if (!l) continue;
      anyLinked = true;
      if (a.keepAll) toggleRow(a.label + "  (everywhere)", true, false, {});
      else toggleRow(a.label, l->keep, true, {Rect{}, H_KEEP, (int)ai, menuIdx, 0});
      if (y > MENU.y + 340) break;
    }
    if (!anyLinked) { text("nothing linked to it (accessories page)", x2, y + 10, 0x6B7482, &fonts::FreeSans9pt7b, middle_left); y += 30; }
    y += 14;
    bool any = false;
    for (size_t ai = 0; ai < lay.accessories.size(); ai++) {
      const Accessory& a = lay.accessories[ai];
      const Link* l = a.link(m.name);
      if (a.kind != "pad" || !l) continue;
      if (!any) { text("controllers (player)", x2, y, MUTED, &fonts::FreeSans12pt7b, middle_left); y += 26; any = true; }
      playerRow(a.label + (a.present ? "" : " (away)"), (int)ai, menuIdx, l->player);
    }
    if (!any) text("no controllers linked (accessories page)", x2, y, 0x6B7482, &fonts::FreeSans9pt7b, middle_left);
  } else {
    const Accessory& a = lay.accessories[menuIdx];
    toggleRow("works out of focus everywhere", a.keepAll, true, {Rect{}, H_KEEPALL, menuIdx, 0, 0});
    y += 6;
    text(a.kind == "pad" ? "player on each machine" : "may reach", x2, y, MUTED, &fonts::FreeSans12pt7b, middle_left);
    y += 26;
    for (auto& l : a.links) {
      int mi = lay.machineIndex(l.screen);
      if (mi < 0) continue;
      if (a.kind == "pad") playerRow(lay.machines[mi].label, menuIdx, mi, l.player);
      else { text(lay.machines[mi].label.c_str(), x2, y + 12, TEXT, &fonts::FreeSans12pt7b, middle_left); y += 32; }
      if (y > MENU.y + MENU.h - 60) break;
    }
    if (a.links.empty()) text("not linked to anything yet", x2, y, 0x6B7482, &fonts::FreeSans9pt7b, middle_left);
    else if (!a.keepAll && y < MENU.y + MENU.h - 110) {  // or just on some of them
      y += 8;
      text("keep while out of focus on", x2, y, MUTED, &fonts::FreeSans12pt7b, middle_left);
      y += 26;
      for (auto& l : a.links) {
        int mi = lay.machineIndex(l.screen);
        if (mi < 0) continue;
        if (y > MENU.y + MENU.h - 60) break;
        toggleRow(lay.machines[mi].label, l.keep, true, {Rect{}, H_KEEP, menuIdx, mi, 0});
      }
    }
  }

  // column 3: how it is attached
  int x3 = MENU.x + 790, w3 = 320;
  y = MENU.y + 136;
  text("about", x3, y, MUTED, &fonts::FreeSans12pt7b, middle_left);
  y += 30;
  std::string about = menuAcc ? lay.accessories[menuIdx].about : lay.machines[menuIdx].about;
  if (menuAcc && !lay.accessories[menuIdx].present && lay.accessories[menuIdx].kind == "pad")
    about = "not connected now\n" + about;
  size_t p0 = 0;
  while (p0 <= about.size() && y < MENU.y + 470) {
    size_t p1 = about.find('\n', p0);
    if (p1 == std::string::npos) p1 = about.size();
    std::string ln = about.substr(p0, p1 - p0);
    if (!ln.empty())
      text(ln.c_str(), x3, y, 0xC9D1DB, fit(ln.c_str(), w3, {&fonts::FreeSans9pt7b, &fonts::Font2}), middle_left);
    y += 26;
    p0 = p1 + 1;
  }
  Rect bDone{x3, MENU.y + 548, w3, 54};
  if (!menuAcc) {
    Machine& m = lay.machines[menuIdx];
    bool usable = m.ready;
    Rect bUse{x3, MENU.y + 480, w3, 54};
    button(bUse, m.name == lay.active ? "in focus" : "focus it", usable ? 0x1F4D3A : 0x2B323C, usable ? TEXT : MUTED);
    hits.push_back({bUse, H_USE, 0, 0, 0});
  }
  button(bDone, "done", 0x2B323C, TEXT);
  hits.push_back({bDone, H_DONE, 0, 0, 0});
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
  if (lay.valid && page) page->finger();
}

static void deskFinger() {
  if (drag == D_CABLE || drag == D_PLUG) {  // the monitor it would land on
    int ni = monitorAt(curX, curY);
    if (ni >= 0) {
      auto& m = lay.monitors[ni];
      int mi = drag == D_CABLE ? dragIdx : deskOwner(lay.machineIndex(lay.monitors[dragIdx].shows));
      bool okDrop = m.fixed.empty() && switchedVia(mi) && lay.switcherOnline;
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
    if (mi >= 0 && slotOf(mi) >= 0) {
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

// ------------------------------------------------------------ the pages

struct DeskPage : Page {
  std::string key() const override { return "displays"; }
  std::string title() const override { return "displays"; }
  const char* hint() const override {
    return "drag a machine onto a monitor   |   pull a plug off to unplug   |   "
           "drag monitors to rearrange   |   tap a machine to focus it   |   hold for options";
  }
  void draw() override { deskDraw(); }
  void finger() override { deskFinger(); }
  void press(int x, int y) override { deskPress(x, y); }
  void moved(bool far) override { deskMoved(far); }
  void release() override { deskRelease(); }
  bool carrying() const override { return drag == D_CABLE || drag == D_PLUG || drag == D_MON; }
};

// The things you hold (keyboard + mouse, game pads) along the top, the
// machines along the bottom, and a line for each link: drag from an
// accessory to a machine to link it, pull a line's end off a machine to
// unlink it (or drop it on another machine).  Lines into a machine sit side
// by side on its top edge; pads show their player there.
struct AccessoriesPage : Page {
  std::string key() const override { return "accessories"; }
  std::string title() const override { return "accessories"; }
  const char* hint() const override {
    return "drag from an accessory to a machine to link them   |   pull a line off to unlink   |   "
           "tap a machine to focus it   |   hold for options";
  }

  static const int TOP_Y = AY + 30, TOP_H = 176;
  Rect accRect(int i) const { return rowRect(i, lay.accessories.size(), TOP_Y, TOP_H, 240); }
  static void accKnob(const Rect& r, int& x, int& y) { x = r.x + r.w / 2; y = r.y + r.h; }

  // Where accessory ai's line ends on machine mi's card (or false)
  bool lineEnd(int ai, int mi, int& x, int& y) const {
    int slot = -1;
    for (size_t k = 0; k < desk.size(); k++) if (desk[k] == mi) slot = (int)k;
    if (slot < 0) return false;
    const std::string& sc = lay.machines[mi].name;
    int n = 0, j = -1;
    for (size_t a = 0; a < lay.accessories.size(); a++)
      if (lay.accessories[a].link(sc)) { if ((int)a == ai) j = n; n++; }
    if (j < 0) return false;
    Rect r = cardRect(slot);
    int step = std::min(26, (r.w - 40) / std::max(1, n));
    x = r.x + r.w / 2 + (int)((j - (n - 1) / 2.0f) * step);
    y = r.y;
    return true;
  }

  int accAt(int x, int y) const {
    for (size_t i = 0; i < lay.accessories.size(); i++) {
      Rect r = accRect(i);
      int kx, ky;
      accKnob(r, kx, ky);
      if (r.has(x, y) || (x - kx) * (x - kx) + (y - ky) * (y - ky) < 26 * 26) return (int)i;
    }
    return -1;
  }

  void drawAcc(int i, bool pressed) {
    const Accessory& a = lay.accessories[i];
    Rect r = accRect(i);
    uint32_t col = accColor(i), bg = pressed ? 0x262D37 : PANEL_BG;
    G->fillSmoothRoundRect(r.x, r.y, r.w, r.h, 14, C(bg));
    G->drawRoundRect(r.x, r.y, r.w, r.h, 14, C(mix(col, bg, 0.5f)));
    drawArt(*G, a.kind != "pad" ? "keyboard" : "gamepad", r.x + r.w / 2, r.y + 62,
            std::min(0.85f, (r.w - 40) / 180.0f), a.present ? 0 : 0.5f, bg);
    text(a.label.c_str(), r.x + r.w / 2, r.y + r.h - 50, a.present ? TEXT : MUTED,
         fit(a.label.c_str(), r.w - 14, {&fonts::FreeSansBold12pt7b, &fonts::FreeSansBold9pt7b, &fonts::FreeSans9pt7b, &fonts::Font2}));
    int n = 0;
    for (auto& l : a.links) { int mi = lay.machineIndex(l.screen); if (mi >= 0 && lay.onDesk(mi)) n++; }
    std::string st = !a.present ? "not connected" : std::to_string(n) + (n == 1 ? " machine" : " machines");
    G->fillSmoothCircle(r.x + r.w / 2 - G->textWidth(st.c_str()) / 2 - 12, r.y + r.h - 22, 5,
                        C(a.present ? OK_C : 0x59606B));
    text(st.c_str(), r.x + r.w / 2 + 2, r.y + r.h - 22, a.present ? 0xC9D1DB : MUTED, &fonts::FreeSans9pt7b);
    int kx, ky;
    accKnob(r, kx, ky);
    G->fillSmoothCircle(kx, ky, 12, C(0x0A0C0F));
    G->fillSmoothCircle(kx, ky, 9, C(col));
  }

  void draw() override {
    for (size_t i = 0; i < lay.accessories.size(); i++) drawAcc(i, (drag == D_PRESS_ACC || drag == D_ACCWIRE || drag == D_LONG) && dragIdx == (int)i);
    for (size_t k = 0; k < desk.size(); k++)
      drawCard(desk[k], cardRect(k), (drag == D_PRESS_CARD || drag == D_LONG) && desk[k] == dragIdx, false);
    // the lines, with a plug (and the player) where each one ends
    for (size_t ai = 0; ai < lay.accessories.size(); ai++) {
      const Accessory& a = lay.accessories[ai];
      int kx, ky;
      accKnob(accRect(ai), kx, ky);
      uint32_t col = accColor(ai);
      for (auto& l : a.links) {
        int mi = lay.machineIndex(l.screen), ex, ey;
        if (drag == D_LINEPLUG && dragIdx == (int)ai && dragIdx2 == mi) continue;
        if (!lineEnd(ai, mi, ex, ey)) continue;
        cable(ex, ey, kx, ky, col, a.present ? SOLID : FAINT);
        G->fillSmoothRoundRect(ex - 6, ey - 9, 12, 14, 3, C(col));
        if (a.kind == "pad") {
          std::string pl = "P" + std::to_string(l.player);
          G->fillSmoothRoundRect(ex - 13, ey - 34, 26, 20, 6, C(0x0F1216));
          text(pl.c_str(), ex, ey - 24, col, &fonts::FreeSansBold9pt7b);
        }
      }
    }
  }

  void finger() override {
    int from = -1;
    if (drag == D_ACCWIRE) {
      from = dragIdx;
      int mi = cardAt(curX, curY);
      if (mi >= 0) {
        Rect r = cardRect(slotOf(mi));
        for (int i = 0; i < 4; i++)
          G->drawRoundRect(r.x - 6 - i, r.y - 6 - i, r.w + 12 + 2 * i, r.h + 12 + 2 * i, 18, C(ACCENT));
        ink.add(r.x - 12, r.y - 12, r.x + r.w + 12, r.y + r.h + 12);
      }
    } else if (drag == D_LINEPLUG) {
      from = dragIdx;
    }
    if (from < 0) return;
    int kx, ky;
    accKnob(accRect(from), kx, ky);
    uint32_t col = accColor(from);
    cable(curX, curY, kx, ky, col);
    G->fillSmoothRoundRect(curX - 7, curY - 9, 14, 16, 3, C(col));
    ink.add(curX - 10, curY - 12, curX + 10, curY + 10);
  }

  void press(int x, int y) override {
    for (size_t ai = 0; ai < lay.accessories.size(); ai++)  // a line's end first: it sits on a card's edge
      for (auto& l : lay.accessories[ai].links) {
        int mi = lay.machineIndex(l.screen), ex, ey;
        if (lineEnd(ai, mi, ex, ey) && abs(x - ex) < 12 && y > ey - 38 && y < ey + 12) {
          drag = D_LINEPLUG; dragIdx = (int)ai; dragIdx2 = mi;
          return;
        }
      }
    int i;
    if ((i = accAt(x, y)) >= 0) { drag = D_PRESS_ACC; dragIdx = i; }
    else if ((i = cardAt(x, y)) >= 0) { drag = D_PRESS_CARD; dragIdx = i; }
  }

  void moved(bool far) override {
    if (drag == D_PRESS_ACC && far) { drag = D_ACCWIRE; sceneDirty = true; }
  }

  void link(int ai, int mi, int player) {
    Accessory& a = lay.accessories[ai];
    const std::string& sc = lay.machines[mi].name;
    hub.send("link " + a.name + " " + sc + (player ? " " + std::to_string(player) : ""));
    if (!a.link(sc)) a.links.push_back({sc, player ? player : 1});  // the hub says which player
  }
  void unlink(int ai, int mi) {
    Accessory& a = lay.accessories[ai];
    const std::string& sc = lay.machines[mi].name;
    hub.send("unlink " + a.name + " " + sc);
    for (size_t k = 0; k < a.links.size(); k++)
      if (a.links[k].screen == sc) { a.links.erase(a.links.begin() + k); break; }
  }

  void release() override {
    if (drag == D_PRESS_CARD) {
      tapCard(dragIdx);
    } else if (drag == D_PRESS_ACC) {
      const Accessory& a = lay.accessories[dragIdx];
      say(a.label + (a.present ? "" : " is not connected") + "  -  hold it for options");
    } else if (drag == D_ACCWIRE) {
      int mi = cardAt(curX, curY);
      if (mi < 0) return;
      if (lay.accessories[dragIdx].link(lay.machines[mi].name)) { say("Already linked"); return; }
      link(dragIdx, mi, 0);
    } else if (drag == D_LINEPLUG) {
      int mi = cardAt(curX, curY);
      if (mi == dragIdx2) return;                 // put back
      const Link* l = lay.accessories[dragIdx].link(lay.machines[dragIdx2].name);
      int player = l ? l->player : 0;
      unlink(dragIdx, dragIdx2);
      if (mi >= 0 && !lay.accessories[dragIdx].link(lay.machines[mi].name)) link(dragIdx, mi, player);
    }
  }

  bool carrying() const override { return drag == D_ACCWIRE || drag == D_LINEPLUG; }
};

static DeskPage deskPage;
static AccessoriesPage accessoriesPage;
static Page* const pages[] = {&deskPage, &accessoriesPage};  // a new page: add it here

static int pageCount() { return (int)(sizeof pages / sizeof pages[0]); }
static Page* pageAt(int i) { return pages[std::max(0, std::min(i, pageCount() - 1))]; }

static void showPage(int tab) {
  Page* p = pageAt(tab);
  if (p == page) return;
  page = p;
  drag = D_NONE;
  menuIdx = -1;
  sceneDirty = true;
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
  bool under = page && page->carrying();
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

void rkmPanelSetHub(const char* host) {
  if (hub.host() == host) return;
  hub.begin(host, hub.port());
  hub.reconnect();
}

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
  page = &deskPage;
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
        computeDesk();
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
  if ((drag == D_PRESS_CARD || drag == D_PRESS_ACC) && nowMs() - pressAt > 550) {
    openMenu(dragIdx, drag == D_PRESS_ACC);
    drag = D_LONG;
  }
  // ... and on a monitor, its shape and size
  if (drag == D_PRESS_MON && nowMs() - pressAt > 550) {
    openMonMenu(dragIdx);
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

// The desk as the hub describes it ("layout {json}" lines).
#pragma once
#include <string>
#include <vector>
#include <ArduinoJson.h>

struct Machine {
  std::string name, label, art;
  int input = 0;          // switcher input, 0 = not on the switcher
  bool ready = false;     // its agent is connected
  bool agent = true;      // false: video only (a console)
  bool kbd = false;       // a hardware (PS/2) keyboard is plugged into it
  bool soft = false;      // its software agent is running
  uint32_t color = 0;     // its colour (cables, card, keyboard lights); 0 = not sent
  bool colorSet = false;  // picked by hand rather than the default
  int pads = 0;           // controller dongles (pad players) connected
  std::string via;        // a console: the box its video goes through (the RetroTINK)
  std::string plugged;    // a box: what is wired into it now
  std::vector<int> always;  // per kind (Layout::kinds): takes that input out of focus
  std::string about;      // how it is attached, lines separated by \n
};

// A kind of accessory ("kbm", "pad"; later mics...), as the hub lists them
struct Kind {
  std::string id, label;
};

struct Link {
  std::string screen;
  int player = 1;         // pads: P1..P4 on that machine
};

// Something you hold: the keyboard + mouse, a game pad
struct Accessory {
  std::string name, label, kind, about;
  bool present = false;
  long color = -1;        // its lines; -1 = default
  std::vector<Link> links;
  const Link* link(const std::string& screen) const {
    for (auto& l : links) if (l.screen == screen) return &l;
    return nullptr;
  }
};

struct Monitor {
  std::string name;
  int col = 0, row = 0;
  std::string fixed;      // wired straight to this machine, or empty
  int output = 0;         // switcher output
  std::string shows;      // machine on it now, or empty
  int input = 0;          // switcher input tied to its output (even when not shown)
  std::string shared;     // its other input shows this machine (or empty)
  bool sharedOn = false;  // ... and that input is selected
  bool portrait = false;
  bool capture = false;   // a capture card rather than a screen
  int ddc = -1;           // the hub can switch its input: 1 yes, 0 not answering, -1 no
  std::string label;
};

struct Layout {
  bool valid = false;
  std::string active;     // machine with the keyboard
  bool locked = false;
  bool switcherPresent = false, switcherOnline = false;
  std::vector<Machine> machines;
  std::vector<Monitor> monitors;
  std::vector<Kind> kinds;
  std::vector<Accessory> accessories;

  Accessory* accessory(const std::string& n) {
    for (auto& a : accessories) if (a.name == n) return &a;
    return nullptr;
  }
  // Pads linked to a machine (for the little controller badge)
  int padsLinked(const std::string& screen, bool presentOnly) const {
    int n = 0;
    for (auto& a : accessories)
      if (a.kind == "pad" && a.link(screen) && (a.present || !presentOnly)) n++;
    return n;
  }

  Machine* machine(const std::string& n) {
    for (auto& m : machines) if (m.name == n) return &m;
    return nullptr;
  }
  Monitor* monitor(const std::string& n) {
    for (auto& m : monitors) if (m.name == n) return &m;
    return nullptr;
  }
  int machineIndex(const std::string& n) const {
    for (size_t i = 0; i < machines.size(); i++) if (machines[i].name == n) return (int)i;
    return -1;
  }

  // Boxes are machines other machines go through (via).  A root box is not
  // itself behind another one; each root box gets a page of its own.
  bool isBox(const std::string& n) const {
    for (auto& m : machines) if (m.via == n) return true;
    return false;
  }
  std::vector<int> rootBoxes() const {
    std::vector<int> out;
    for (size_t i = 0; i < machines.size(); i++)
      if (machines[i].via.empty() && isBox(machines[i].name)) out.push_back((int)i);
    return out;
  }
  // The root box a machine ends up in (itself when it is one), or -1
  int rootOf(int i) const {
    for (int n = 0; i >= 0 && n < 32; n++) {
      if (machines[i].via.empty()) return isBox(machines[i].name) ? i : -1;
      i = machineIndex(machines[i].via);
    }
    return -1;
  }
  int depthOf(int i) const {  // boxes between it and its root
    int d = 0;
    for (; i >= 0 && !machines[i].via.empty() && d < 32; d++) i = machineIndex(machines[i].via);
    return d;
  }
  // A card on the displays page: every computer and console; boxes (the
  // RetroTINK) are plumbing, shown through what is plugged into them
  bool onDesk(int i) const { return !isBox(machines[i].name); }

  bool parse(const std::string& json) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;
    Layout l;
    l.valid = true;
    l.active = doc["active"] | "";
    l.locked = doc["locked"] | 0;
    l.switcherPresent = doc["switcher"]["present"] | 0;
    l.switcherOnline = doc["switcher"]["online"] | 0;
    for (JsonObject s : doc["screens"].as<JsonArray>()) {
      Machine m;
      m.name = s["name"] | "";
      m.label = s["label"] | "";
      m.art = s["art"] | "";
      m.input = s["input"] | 0;
      m.ready = s["ready"] | 0;
      m.agent = s["agent"] | 1;
      m.kbd = s["kbd"] | 0;
      m.soft = s["soft"] | 0;
      m.color = s["color"] | 0u;
      m.colorSet = s["colorSet"] | 0;
      m.pads = s["pads"] | 0;
      m.about = s["about"] | "";
      for (int v : s["always"].as<JsonArray>()) m.always.push_back(v);
      m.via = s["via"] | "";
      m.plugged = s["plugged"] | "";
      l.machines.push_back(m);
    }
    for (JsonObject o : doc["monitors"].as<JsonArray>()) {
      Monitor m;
      m.name = o["name"] | "";
      m.col = o["col"] | 0;
      m.row = o["row"] | 0;
      m.fixed = o["fixed"] | "";
      m.output = o["output"] | 0;
      m.shows = o["shows"] | "";
      m.input = o["input"] | 0;
      m.shared = o["shared"] | "";
      m.sharedOn = o["sharedOn"] | 0;
      m.portrait = o["portrait"] | 0;
      m.capture = o["capture"] | 0;
      m.ddc = o["ddc"] | -1;
      m.label = o["label"] | "";
      l.monitors.push_back(m);
    }
    for (JsonObject k : doc["kinds"].as<JsonArray>()) l.kinds.push_back({k["id"] | "", k["label"] | ""});
    for (JsonObject o : doc["accessories"].as<JsonArray>()) {
      Accessory a;
      a.name = o["name"] | "";
      a.label = o["label"] | "";
      a.kind = o["kind"] | "";
      a.about = o["about"] | "";
      a.present = o["present"] | 0;
      a.color = o["color"] | -1L;
      for (JsonObject k : o["links"].as<JsonArray>()) a.links.push_back({k["screen"] | "", k["player"] | 1});
      l.accessories.push_back(a);
    }
    *this = l;
    return true;
  }
};

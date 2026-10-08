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
};

struct Monitor {
  std::string name;
  int col = 0, row = 0;
  std::string fixed;      // wired straight to this machine, or empty
  int output = 0;         // switcher output
  std::string shows;      // machine on it now, or empty
  std::string shared;     // its other input shows this machine (or empty)
  bool sharedOn = false;  // ... and that input is selected
  bool portrait = false;
};

struct Layout {
  bool valid = false;
  std::string active;     // machine with the keyboard
  bool locked = false;
  bool switcherPresent = false, switcherOnline = false;
  std::vector<Machine> machines;
  std::vector<Monitor> monitors;

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
      m.shared = o["shared"] | "";
      m.sharedOn = o["sharedOn"] | 0;
      m.portrait = o["portrait"] | 0;
      l.monitors.push_back(m);
    }
    *this = l;
    return true;
  }
};

"""Where lil' C is.

There is only one lil' C. He lives on his two homes (the Stack-chan and the
Tab5) and wanders over to the desk's computers now and then, where he shows
up as a little xeyes window: on the laptop through lilc_popup.py, elsewhere
through that machine's RetroKM agent (the hub's "buddy" window).

- Summoned (a tap on a home's screen, a question, a click on his window or a
  machine's tray icon): he goes there at once and stays a while.
- Left alone he gets restless: he may hop to his other home or visit an idle
  computer. He prefers the home that was used last.
- If a computer he is visiting starts being used, he gets out of the way.

All the weights and times are in config.json under "presence".
"""
import os
import random
import subprocess
import sys
import threading
import time

from lilc_x11 import x11_env, monitor_geometry, all_monitors

HERE = os.path.dirname(os.path.abspath(__file__))

PRESENCE_DEFAULTS = {
    "wander": True,
    # his homes: the devices that poll the bridge. yaw/pitch: where the
    # Stack-chan's head turns to look at it (degrees, + yaw = his left)
    "homes": {
        "stackchan": {"label": "the Stack-chan", "yaw": 0, "pitch": 0},
        "tab5": {"label": "the Tab5", "yaw": -70, "pitch": 0},
    },
    "stay_home_s": [180, 600],      # at a home before he gets restless
    "stay_visit_s": [40, 150],      # visiting a computer
    "summon_pin_s": 300,            # stays put this long after a summon
    "home_last_used_weight": 3.0,   # going home: the last-used one vs the other
    "home_other_weight": 1.0,
    "restless_visit": 0.35,         # restless at home: visit a computer
    "restless_other_home": 0.25,    #   ... or hop to the other home (else stay)
    "visit_another": 0.15,          # done visiting: try another computer first
    "recent_use_s": 120,            # don't visit a computer used this recently
    "busy_after_s": 4.0,            # this much steady mouse use (or any typing) and he leaves
    "device_timeout_s": 12,         # a home that stops polling is offline
    # the trip: the place he leaves turns its head to look, then he goes; the
    # place he's going shows "COMING!" until he gets there
    "travel_s": 2.8,
}


class LocalPopup:
    """lil' C's window on the laptop the bridge runs on."""

    def __init__(self, on_click):
        self.proc = None
        self.on_click = on_click

    def show(self, geo, monitors, label, calm):
        self.hide()
        try:
            self.proc = subprocess.Popen(
                [sys.executable, os.path.join(HERE, "lilc_popup.py"), *map(str, geo),
                 label, monitors, "calm" if calm else "busy"],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=x11_env())
            threading.Thread(target=self._read, args=(self.proc,), daemon=True).start()
        except OSError as e:
            print("pop-up failed:", e)

    def _read(self, proc):
        for line in proc.stdout:
            if line.strip() == "click" and self.on_click:
                self.on_click()

    def hide(self, wait=False):
        p, self.proc = self.proc, None
        if not p:
            return
        try:
            p.stdin.write("bye\n")
            p.stdin.flush()
            if wait:
                p.wait(timeout=3)
        except (OSError, subprocess.TimeoutExpired):
            p.kill()


def laptop_idle():
    """Seconds since the laptop's own keyboard/mouse (or the hub) was used."""
    try:
        from Xlib import display
        from Xlib.ext import screensaver
        if not hasattr(laptop_idle, "d"):
            laptop_idle.d = display.Display(x11_env()["DISPLAY"])
        d = laptop_idle.d
        return screensaver.query_info(d.screen().root).idle / 1000.0
    except Exception:
        laptop_idle.__dict__.pop("d", None)
        return None


class Presence:
    def __init__(self, cfg, desk):
        self.cfg = cfg
        self.p = dict(PRESENCE_DEFAULTS)
        self.p.update(cfg.get("presence") or {})
        self.desk = desk
        desk.on_buddy = self._buddy_event
        self.lock = threading.RLock()
        self.seen = {}                 # home -> last poll time
        self.where = ""                # "" = nowhere yet (no home online)
        self.since = time.time()
        self.seq = 0
        self.arrive_at = 0.0           # when he gets to where he is going
        self.came_from = ""
        self.pinned_until = 0.0
        self.next_move = time.time() + 30
        self.last_home = "stackchan"
        self.held = 0                  # questions running: no wandering
        self.hop_label = ""            # set while checking on a machine for a question
        self.busy_since = None
        self.popup = LocalPopup(lambda: self.summon("laptop", "clicked"))
        self.shown_on = None           # machine whose window is up
        threading.Thread(target=self._loop, daemon=True).start()

    # ------------------------------------------------------------ places

    def homes(self):
        return self.p["homes"]

    def home_online(self, h):
        return time.time() - self.seen.get(h, 0) < self.p["device_timeout_s"]

    def machines(self):
        """Computers he can visit right now: name -> label."""
        out = {}
        for name, m in self.cfg["machines"].items():
            if name == "laptop":
                out[name] = m.get("label", name)
        for s in self.desk.screens():
            if self.desk.buddy_ok(s["name"]):
                out[s["name"]] = s.get("label") or s["name"]
        return out

    def label(self, place):
        if place in self.homes():
            return self.homes()[place].get("label", place)
        m = self.cfg["machines"].get(place)
        if m:
            return m.get("label", place)
        s = self.desk.screen(place)
        return ("the " + s["label"]) if s and s.get("label") else place

    def aim(self, place):
        """(yaw, pitch) for the Stack-chan's head to look at that place."""
        d = self.homes().get(place) or self.cfg["machines"].get(place) or {}
        return d.get("yaw", 0), d.get("pitch", 0)

    def machine_idle(self, name):
        if name == "laptop":
            return laptop_idle()
        return self.desk.idle(name)

    # ------------------------------------------------------------ moving

    def _go(self, place, why, calm=True):
        with self.lock:
            if place == self.where:
                return
            print("lil' C: %s -> %s (%s)" % (self.where or "nowhere", place or "nowhere", why))
            self._leave_machine()
            self.came_from = self.label(self.where) if self.where else ""
            # waking up somewhere is instant; a trip takes a moment
            self.arrive_at = time.time() + (self.p["travel_s"] if self.where else 0)
            self.where = place
            self.since = time.time()
            self.seq += 1
            self.busy_since = None
            if place in self.homes():
                self.last_home = place if why in ("summoned", "asked", "touched") else self.last_home
            elif place:
                # give the home he left a moment to show him going
                threading.Timer(self.p["travel_s"], self._arrive_machine,
                                args=(place, self.seq, calm)).start()
            lo, hi = self.p["stay_home_s"] if place in self.homes() else self.p["stay_visit_s"]
            self.next_move = time.time() + random.uniform(lo, hi)

    def _arrive_machine(self, place, seq, calm):
        with self.lock:
            if self.where != place or self.seq != seq:
                return
            label = self.hop_label or "lil' C"
            if place == "laptop":  # the bridge's own PC: the pop-up knows its monitors
                m = self.cfg["machines"].get("laptop", {})
                self.popup.show(monitor_geometry(m.get("monitor", "")), all_monitors(), label, calm)
            else:
                self.desk.buddy(place, True, label)
            self.shown_on = place

    def _leave_machine(self):
        if not self.shown_on:
            return
        if self.shown_on == "laptop" and self.popup.proc:
            self.popup.hide()
        else:
            self.desk.buddy(self.shown_on, False)
        self.shown_on = None

    def best_home(self, avoid=None):
        online = [h for h in self.homes() if self.home_online(h) and h != avoid]
        if not online:
            return ""
        if len(online) == 1:
            return online[0]
        w = [self.p["home_last_used_weight"] if h == self.last_home else self.p["home_other_weight"]
             for h in online]
        return random.choices(online, weights=w)[0]

    # ------------------------------------------------------------ outside events

    def heartbeat(self, dev):
        if dev in self.homes():
            first = not self.home_online(dev)
            self.seen[dev] = time.time()
            if first:
                print("lil' C: %s is online" % dev)
                if not self.where:
                    self._go(dev, "woke up")

    def touched(self, dev):
        """Someone used that home's screen (it doesn't summon him by itself)."""
        if dev in self.homes():
            self.last_home = dev

    def summon(self, place, why="summoned"):
        with self.lock:
            if place in self.homes():
                self.seen.setdefault(place, time.time())
                self.last_home = place
            self.pinned_until = time.time() + self.p["summon_pin_s"]
            self._go(place, why)
            self.pinned_until = time.time() + self.p["summon_pin_s"]

    def _buddy_event(self, screen, event):
        if event in ("click", "summon"):
            self.summon(screen, "clicked on " + screen)

    def hold(self, on):
        """A question is being answered: don't wander off mid-sentence."""
        with self.lock:
            self.held += 1 if on else -1
            self.held = max(0, self.held)

    def hop_to(self, place, label):
        """Checking on a computer for a question: go there for a moment."""
        with self.lock:
            self.hop_label = label
            self._go(place, "checking on it", calm=False)

    def hop_back(self, home):
        with self.lock:
            self.hop_label = ""
            if self.shown_on == "laptop" and self.popup.proc:
                self.popup.hide(wait=True)  # watch him dart back first
                self.shown_on = None
            self._go(home, "asked")

    def state_for(self, dev):
        with self.lock:
            yaw, pitch = self.aim(self.where)
            kind = "home" if self.where in self.homes() else "machine" if self.where else ""
            return {"where": self.where, "label": self.label(self.where) if self.where else "",
                    "kind": kind, "here": self.where == dev, "seq": self.seq,
                    "yaw": yaw, "pitch": pitch,
                    # still on his way: ms until he arrives, and where from
                    "eta_ms": int(max(0.0, self.arrive_at - time.time()) * 1000),
                    "from": self.came_from}

    # ------------------------------------------------------------ restlessness

    def _loop(self):
        while True:
            time.sleep(0.5)
            try:
                self._tick()
            except Exception as e:  # never let him freeze
                print("presence:", e)

    def _tick(self):
        now = time.time()
        with self.lock:
            w = self.where
            # his home went dark: go to the other one
            if w in self.homes() and not self.home_online(w):
                self._go(self.best_home(avoid=w), "%s went offline" % w)
                return
            if not w:
                h = self.best_home()
                if h:
                    self._go(h, "woke up")
                return
            if self.held:
                return
            pinned = now < self.pinned_until
            if w not in self.homes():
                # visiting: if the machine is being used, get out of the way
                if w != "laptop" and not self.desk.buddy_ok(w):
                    self._go(self.best_home(), "%s went away" % w)
                    return
                idle = self.machine_idle(w)
                in_use = idle is not None and idle < 1.0 and now - self.since > 3
                if in_use:
                    self.busy_since = self.busy_since or now
                elif idle is None or idle > 3:
                    self.busy_since = None
                key = self.desk.key_idle(w)
                typing = key is not None and key < 1.0 and now - self.since > 3
                if not pinned and (typing or (self.busy_since and now - self.busy_since > self.p["busy_after_s"])):
                    self._go(self.best_home() or w, "getting out of the way on %s" % w)
                    return
            if pinned or not self.p["wander"] or now < self.next_move:
                return
            self._restless(w)

    def _visitable(self, exclude=None):
        out = []
        for name in self.machines():
            if name == exclude:
                continue
            idle = self.machine_idle(name)
            if idle is not None and idle < self.p["recent_use_s"]:
                continue  # somebody's using it
            out.append(name)
        return out

    def _restless(self, w):
        r = random.random()
        lo, hi = self.p["stay_home_s"] if w in self.homes() else self.p["stay_visit_s"]
        self.next_move = time.time() + random.uniform(lo, hi)
        if w in self.homes():
            visits = self._visitable()
            other = self.best_home(avoid=w)
            if visits and r < self.p["restless_visit"]:
                self._go(random.choice(visits), "wandering")
            elif other and r < self.p["restless_visit"] + self.p["restless_other_home"]:
                self._go(other, "wandering")
        else:
            visits = self._visitable(exclude=w)
            if visits and r < self.p["visit_another"]:
                self._go(random.choice(visits), "wandering")
            else:
                home = self.best_home()
                if home:
                    self._go(home, "going home")

"""Where things are on the desk, and which way lil' C looks to see them.

A small model of the desk, in centimetres, seen from the user's chair:
x to the right, z away from the user (towards the monitors), y up from the
desk top.

- Monitors come from the RetroKM layout (the hub's desk map): monitor column
  c stands at x = (c - c0) * monitor_spacing_cm, at the back of the desk
  (z = monitor_z_cm), its middle monitor_y_cm up; each row up adds
  row_height_cm. c0 is the column of the first home's monitor, so x = 0 is
  in front of it. Capture cards are not on the desk and don't count.
- A computer is wherever a monitor shows it (the laptop: its own screen).
- His homes face the user and are placed by monitor: "below" one (just in
  front of its stand) or "between" two (at home_z_cm).
- "places" can pin anything else (or override any of the above):
  {"x": cm, "y": cm, "z": cm}, or {"near": monitor, "dx": cm, "dy": cm, "dz": cm}.

aim(from_home, to_place) gives:
- yaw/pitch: degrees for the Stack-chan's neck. 0 = facing the user;
  + yaw = towards the user's left (the convention the firmware's eyes and the
  Tab5 window use); +-180 = behind him. Flip yaw_sign if his neck turns the
  wrong way.
- dir_x/dir_y: -1..1 on a screen facing the user (x right, y up), for the
  Tab5, whose "neck" is the xeyes window gliding across its desktop.

All of it is in config.json under "desk"; the defaults below are the user's
desk on 2026-10-10.
"""
import math

DESK_DEFAULTS = {
    "monitor_spacing_cm": 60,
    "monitor_z_cm": 70,
    "monitor_y_cm": 35,
    "row_height_cm": 40,
    "home_z_cm": 45,
    "home_y_cm": 8,
    "homes": {
        "stackchan": {"below": "tall"},
        "tab5": {"between": ["tall", "crt"]},
    },
    "places": {},
    "yaw_sign": 1,
}


class DeskGeo:
    def __init__(self, cfg, desk):
        self.p = dict(DESK_DEFAULTS)
        self.p.update(cfg.get("desk") or {})
        self.desk = desk  # lilc_desk.DeskLink: the RetroKM layout

    # ------------------------------------------------------------ positions

    def _monitors(self):
        return {m["name"]: m for m in self.desk.monitors() if not m.get("capture")}

    def _origin_col(self, mons):
        for h in self.p["homes"].values():
            ref = h.get("below") or (h.get("between") or [None])[0]
            if ref in mons:
                return mons[ref].get("col", 0)
        return min((m.get("col", 0) for m in mons.values()), default=0)

    def monitor_pos(self, name, mons=None):
        mons = mons if mons is not None else self._monitors()
        m = mons.get(name)
        if not m:
            return None
        c0 = self._origin_col(mons)
        rows = [o.get("row", 0) for o in mons.values()]
        up = max(rows) - m.get("row", 0) if rows else 0  # a lower row number stands higher
        return (float((m.get("col", 0) - c0) * self.p["monitor_spacing_cm"]),
                float(self.p["monitor_y_cm"] + up * self.p["row_height_cm"]),
                float(self.p["monitor_z_cm"]))

    def position(self, place):
        """(x, y, z) in cm, or None when nobody knows where it is."""
        mons = self._monitors()
        pin = self.p["places"].get(place)
        if pin:
            if "near" in pin:
                base = self.monitor_pos(pin["near"], mons)
                if base:
                    return (base[0] + pin.get("dx", 0), base[1] + pin.get("dy", 0),
                            base[2] + pin.get("dz", 0))
            elif "x" in pin:
                return (float(pin["x"]), float(pin.get("y", 0)), float(pin.get("z", 0)))
        home = self.p["homes"].get(place)
        if home:
            if home.get("below") in mons:
                x, _, z = self.monitor_pos(home["below"], mons)
                return (x, float(self.p["home_y_cm"]), z - 15.0)  # in front of its stand
            ends = [self.monitor_pos(n, mons) for n in home.get("between", []) if n in mons]
            if ends:
                return (sum(e[0] for e in ends) / len(ends), float(self.p["home_y_cm"]),
                        float(self.p["home_z_cm"]))
            return None
        # a computer: the monitors showing it, its own screen first
        shown = [m for m in mons.values() if m.get("shows") == place]
        shown.sort(key=lambda m: (not m.get("fixed"), m.get("col", 0)))
        return self.monitor_pos(shown[0]["name"], mons) if shown else None

    # ------------------------------------------------------------ looking

    def aim(self, frm, to):
        a, b = self.position(frm), self.position(to)
        if not a or not b or frm == to:
            return {"yaw": 0, "pitch": 0, "dir_x": 0.0, "dir_y": 0.0, "known": False}
        vx, vy, vz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
        yaw = math.degrees(math.atan2(-vx, -vz)) * self.p["yaw_sign"]
        pitch = math.degrees(math.atan2(vy, math.hypot(vx, vz)))
        n = max(abs(vx), abs(vy), 30.0)  # anything within 30 cm points only partly
        return {"yaw": round(yaw, 1), "pitch": round(pitch, 1),
                "dir_x": round(max(-1.0, min(1.0, vx / n)), 2),
                "dir_y": round(max(-1.0, min(1.0, vy / n)), 2), "known": True}

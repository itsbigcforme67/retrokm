"""lil' C and the RetroKM desk.

DeskLink keeps a connection to the RetroKM hub's panel port (the same one the
Tab5 desk map uses) so the bridge knows the desk layout, can switch the Extron
and the keyboard when the user asks out loud, and can show lil' C on other
machines through their RetroKM agents (the "buddy" window).

KVM commands come from Claude as tags in the reply:
  [[kvm:show MACHINE MONITOR]]   put that machine on that monitor
  [[kvm:blank MONITOR]]          unplug the switcher from that monitor
  [[kvm:keyboard MACHINE]]       give that machine the keyboard and mouse
"""
import json
import socket
import threading
import time


class DeskLink:
    def __init__(self, host="127.0.0.1", port=24852):
        self.host, self.port = host, port
        self.layout = None          # the hub's last "layout" JSON
        self.activity = {}          # screen -> (seconds since input, since a key)
        self.sock = None
        self.lock = threading.Lock()
        self.errors = []            # "error ..." lines from the hub, newest last
        self.on_buddy = None        # callback(screen, event) for buddy clicks
        self.on_sound = None        # callback(screen, id, status, channels, rate)
        threading.Thread(target=self._run, daemon=True).start()

    # ------------------------------------------------------------ link

    def connected(self):
        return self.sock is not None and self.layout is not None

    def send(self, line):
        with self.lock:
            s = self.sock
        if not s:
            return False
        try:
            s.sendall((line + "\n").encode())
            return True
        except OSError:
            return False

    def _run(self):
        while True:
            try:
                s = socket.create_connection((self.host, self.port), timeout=5)
                s.settimeout(1.0)
                with self.lock:
                    self.sock = s
                s.sendall(b"layout\n")
                buf, last_ping = b"", time.time()
                while True:
                    if time.time() - last_ping > 1.0:  # also keeps the hub from dropping us
                        last_ping = time.time()
                        s.sendall(b"activity\nping\n")
                    try:
                        d = s.recv(65536)
                    except socket.timeout:
                        continue
                    if not d:
                        break
                    buf += d
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        self._line(line.decode(errors="replace"))
            except OSError:
                pass
            with self.lock:
                self.sock = None
            self.layout = None
            time.sleep(3)

    def _line(self, line):
        if line.startswith("layout "):
            try:
                self.layout = json.loads(line[7:])
            except ValueError:
                pass
        elif line.startswith("activity "):
            try:
                self.activity = {k: tuple(v) for k, v in json.loads(line[9:]).items()}
            except (ValueError, TypeError):
                pass
        elif line.startswith("sound "):  # sound <screen> <id> playing|done|failed <channels> <rate>
            parts = line.split()
            if len(parts) >= 6 and self.on_sound:
                self.on_sound(parts[1], int(parts[2]), parts[3], int(parts[4]), int(parts[5]))
        elif line.startswith("buddy "):
            parts = line.split()
            if len(parts) >= 3 and self.on_buddy:
                self.on_buddy(parts[1], parts[2])
        elif line.startswith("error "):
            self.errors.append((time.time(), line[6:]))
            del self.errors[:-8]

    # ------------------------------------------------------------ desk facts

    def screens(self):
        return (self.layout or {}).get("screens", [])

    def screen(self, name):
        for s in self.screens():
            if s["name"] == name:
                return s
        return None

    def monitors(self):
        """Monitors left to right, top to bottom, as they stand on the desk."""
        mons = (self.layout or {}).get("monitors", [])
        return sorted(mons, key=lambda m: (m.get("row", 0), m.get("col", 0)))

    def numbered_monitors(self):
        """(number, monitor): real screens 1, 2, 3... left to right; capture
        cards after them. The Tab5 desk map shows the same numbers."""
        mons = self.monitors()
        order = [m for m in mons if not m.get("capture")] + [m for m in mons if m.get("capture")]
        return list(enumerate(order, 1))

    def idle(self, name):
        """Seconds since the hub last sent input to that machine (None: never)."""
        a = self.activity.get(name)
        return None if not a or a[0] < 0 else a[0]

    def key_idle(self, name):
        a = self.activity.get(name)
        return None if not a or a[1] < 0 else a[1]

    def buddy_ok(self, name):
        s = self.screen(name)
        return bool(s and s.get("buddy") and s.get("ready"))

    def describe(self):
        """One compact paragraph about the desk for Claude."""
        lay = self.layout
        if not lay:
            return "The RetroKM hub is not answering, so the desk can't be switched right now."
        labels = {s["name"]: s.get("label") or s["name"] for s in self.screens()}
        parts = []
        for n, m in self.numbered_monitors():
            what = labels.get(m.get("shows"), m.get("shows")) or "nothing"
            kind = ("capture card" if m.get("capture") else
                    "the laptop's own screen" if m.get("fixed") else
                    "portrait" if m.get("portrait") else "monitor")
            extra = ""
            if m.get("shared"):
                extra = ", second input %s (%s now)" % (
                    labels.get(m["shared"], m["shared"]),
                    "on it" if m.get("sharedOn") else "switcher input")
            if not m.get("fixed"):
                extra += ", Extron output %d" % m.get("output", 0)
            name = m["name"] if (m.get("label") or m["name"]) == m["name"] else \
                "%s (%s)" % (m["name"], m["label"])
            parts.append("monitor %d = %s: %s%s, shows %s" % (n, name, kind, extra, what))
        machines = []
        for s in self.screens():
            bits = []
            if s.get("input"):
                bits.append("Extron input %d" % s["input"])
            bits.append("video only" if not s.get("agent", 1) else
                        "online" if s.get("ready") else "offline")
            machines.append("%s = %s (%s)" % (s["name"], s.get("label") or s["name"], ", ".join(bits)))
        sw = lay.get("switcher", {})
        return ("Desk now. Monitors: " + "; ".join(parts) + ". Machines: " + "; ".join(machines) +
                ". Keyboard and mouse are on: " + (labels.get(lay.get("active"), "nobody")) +
                ". Switcher " + ("connected" if sw.get("online") else "NOT connected") + ".")

    # ------------------------------------------------------------ commands

    def _find_screen(self, word):
        w = word.lower().strip()
        for s in self.screens():
            if w in (s["name"].lower(), (s.get("label") or "").lower()):
                return s
        for s in self.screens():  # "the dell", "sgi"...
            if w in (s.get("label") or "").lower() or w in s["name"].lower():
                return s
        return None

    def _find_monitor(self, word):
        w = word.lower().strip()
        nums = self.numbered_monitors()
        if w.isdigit():
            for n, m in nums:
                if n == int(w):
                    return m
        for _, m in nums:
            if w in (m["name"].lower(), (m.get("label") or "").lower()):
                return m
        return None

    def run(self, cmd):
        """Runs one [[kvm:...]] command. Returns None, or a short spoken
        sentence when it could not be done."""
        if not self.connected():
            return "The desk hub isn't answering, so I can't switch anything."
        words = cmd.split()
        if not words:
            return None
        verb = words[0].lower()
        if verb == "show" and len(words) >= 3:
            s, m = self._find_screen(words[1]), self._find_monitor(words[2])
            if not s:
                return "I don't know a machine called %s." % words[1]
            if not m:
                return "I don't know which monitor %s is." % words[2]
            return self._show(s, m)
        if verb == "blank" and len(words) >= 2:
            m = self._find_monitor(words[1])
            if not m:
                return "I don't know which monitor %s is." % words[1]
            if m.get("fixed"):
                return "That one is wired straight to its computer."
            self.send("untie " + m["name"])
            return None
        if verb == "keyboard" and len(words) >= 2:
            s = self._find_screen(words[1])
            if not s:
                return "I don't know a machine called %s." % words[1]
            if not s.get("agent", 1):
                return "%s has no keyboard link." % s.get("label", s["name"])
            if not s.get("ready"):
                return "%s is offline right now." % s.get("label", s["name"])
            self.send("goto " + s["name"])
            return None
        return None

    def _show(self, s, m):
        """The panel's "plug a machine into a monitor", said out loud."""
        label = s.get("label") or s["name"]
        if m.get("shared") == s["name"]:          # it is that monitor's other input
            if not m.get("sharedOn"):
                self.send("share %s on" % m["name"])
            return None
        if m.get("fixed"):
            if m["fixed"] == s["name"]:
                return None
            return "That monitor only shows %s." % m["fixed"]
        if not s.get("input"):
            return "%s isn't wired to the switcher." % label
        if not (self.layout.get("switcher") or {}).get("online"):
            return "The switcher isn't connected to the hub."
        self.send("tie %s %s" % (s["name"], m["name"]))  # the hub also moves a shared monitor back to it
        return None

    def buddy(self, screen, show, label=""):
        self.send(("buddy %s %s %s" % (screen, "show" if show else "hide", label)).rstrip())

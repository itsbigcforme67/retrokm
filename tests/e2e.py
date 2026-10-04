#!/usr/bin/env python3
"""End-to-end test: hub + two X11 agents on Xvfb + a simulated classic-Mac
agent + a simulated Extron switcher.  Run from the repository root after
building build/rkm-x11, build/xq and hub/retrokm-hub."""
import os, socket, struct, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = tempfile.mkdtemp(prefix="rkm-test-")
PORT, CTL = 24950, 24951
procs, failures = [], []

def spawn(cmd, env=None, log=None):
    e = dict(os.environ); e.update(env or {})
    f = open(os.path.join(TMP, log), "w") if log else subprocess.DEVNULL
    p = subprocess.Popen(cmd, env=e, stdout=f, stderr=subprocess.STDOUT)
    procs.append(p)
    return p

def check(name, got, want):
    ok = got == want
    print("%s %-58s %s" % ("PASS" if ok else "FAIL", name, "" if ok else "got %.80r want %.80r" % (got, want)))
    if not ok: failures.append(name)

def xq(display, *args):
    r = subprocess.run([ROOT + "/build/xq"] + list(args), env=dict(os.environ, DISPLAY=display),
                       capture_output=True, timeout=10)
    return r.stdout

def pointer(display):
    return tuple(int(v) for v in xq(display, "pointer").split())

class Ctl:
    def __init__(self):
        self.s = socket.create_connection(("127.0.0.1", CTL)); self.s.settimeout(2)
    def __call__(self, cmd, settle=0.15):
        self.s.sendall(cmd.encode() + b"\n")
        time.sleep(settle)
        try: return self.s.recv(65536).decode()
        except socket.timeout: return ""

class FakeAgent:
    """Speaks the wire protocol the way the classic Mac agent does."""
    def __init__(self, name, caps=3, charset=3, eol=2, w=640, h=480):
        self.s = socket.create_connection(("127.0.0.1", PORT)); self.s.settimeout(0.3)
        self.buf = b""; self.frames = []
        self.send(1, bytes([1, caps, charset, eol]) + struct.pack(">HHH", w, h, 32) + name.encode())
    def send(self, t, payload=b""):
        self.s.sendall(bytes([t]) + struct.pack(">H", len(payload)) + payload)
    def pump(self, wait=0.3):
        end = time.time() + wait
        while time.time() < end:
            try: d = self.s.recv(65536)
            except socket.timeout: continue
            if not d: break
            self.buf += d
            while len(self.buf) >= 3:
                n = struct.unpack(">H", self.buf[1:3])[0]
                if len(self.buf) < 3 + n: break
                t, p = self.buf[0], self.buf[3:3 + n]; self.buf = self.buf[3 + n:]
                if t == 3: self.send(4)
                else: self.frames.append((t, p))
        return self.frames
    def take(self):
        f, self.frames = self.frames, []
        return f
    def clip(self):
        return b"".join(p for t, p in self.frames if t == 0x21)
    def send_clip(self, data):
        self.send(0x20, b"\x01" + struct.pack(">I", len(data)))
        for i in range(0, len(data), 480): self.send(0x21, data[i:i + 480])
        self.send(0x22)

try:
    spawn(["Xvfb", ":91", "-screen", "0", "800x600x24", "-nolisten", "tcp"])
    spawn(["Xvfb", ":92", "-screen", "0", "1280x1024x24", "-nolisten", "tcp"])
    link, fifo = TMP + "/extron", TMP + "/panel"
    spawn([sys.executable, ROOT + "/tests/fake_extron.py", link, fifo], log="extron.log")
    time.sleep(1.0)
    conf = TMP + "/hub.conf"
    open(conf, "w").write("""
[hub]
port = %d
control_port = %d
device = none
speed = 1.0
accel = 0

[extron]
device = %s
poll = 1

[screen octane]
input = 3
[screen pc]
input = 4
[screen mac]
input = 5
remap = leftalt:leftmeta, leftmeta:leftalt

[screen laptop]

[monitor left]
pos = 0,0
output = 1
[monitor right]
pos = 1,0
output = 2

[hotkeys]
ctrl+alt+1 = goto octane
""" % (PORT, CTL, link))
    spawn([ROOT + "/hub/retrokm-hub", "-v", "-c", conf], log="hub.log")
    time.sleep(0.5)
    spawn([ROOT + "/build/rkm-x11", "-n", "octane", "-p", str(PORT), "-l", "127.0.0.1"], {"DISPLAY": ":91"}, "octane.log")
    spawn([ROOT + "/build/rkm-x11", "-n", "pc", "-p", str(PORT), "127.0.0.1"], {"DISPLAY": ":92"}, "pc.log")
    ctl = Ctl()
    for _ in range(100):                         # Xvfb can take a few seconds to accept clients
        if ctl("status").count("ready") == 2: break
        time.sleep(0.2)

    # --- layout from the switcher -------------------------------------------
    ctl("tie 3 1"); ctl("tie 4 2", 0.5)
    st = ctl("status")
    check("extron ties resolve monitors", ("shows=octane" in st, "shows=pc" in st), (True, True))
    ctl("goto octane")
    st = ctl("status")
    check("octane active on left monitor", "active: octane" in st and "on monitor left" in st, True)

    # --- pointer ---------------------------------------------------------------
    ctl("rel -5000 -5000"); ctl("rel 100 50")
    check("absolute pointer on octane", pointer(":91")[:2], (100, 50))
    ctl("rel -500 0")
    check("left edge with no neighbour clamps", pointer(":91")[:2], (0, 50))
    ctl("rel 300 250")                           # y = 300 of 600 -> halfway down
    ctl("rel 2000 0")
    st = ctl("status")
    check("right edge crosses to pc", "active: pc" in st, True)
    check("entry point scaled to 1280x1024", pointer(":92")[:2], (2, int(300 / 599 * 1023)))

    # --- keys and buttons ------------------------------------------------------
    ctl("key a 1")
    check("key down reaches pc", xq(":92", "keys").split(), [b"a"])
    ctl("key a 0")
    check("key up reaches pc", xq(":92", "keys").split(), [])
    ctl("btn 1 1")
    check("button down reaches pc", pointer(":92")[2] & 1, 1)
    ctl("rel -3000 0")
    check("no screen switch while dragging", "active: pc" in ctl("status"), True)
    ctl("btn 1 0")
    ctl("key leftshift 1"); ctl("rel -3000 0")
    check("crossed back to octane", "active: octane" in ctl("status"), True)
    check("held modifier released on the old screen", xq(":92", "keys").split(), [])
    check("held modifier carried to the new screen", xq(":91", "keys").split(), [b"Shift_L"])
    ctl("key leftshift 0")
    check("modifier release", xq(":91", "keys").split(), [])

    # --- hotkey ----------------------------------------------------------------
    ctl("goto pc")
    ctl("key leftctrl 1"); ctl("key leftalt 1"); ctl("key 1 1"); ctl("key 1 0")
    check("hotkey ctrl+alt+1 jumps to octane", "active: octane" in ctl("status"), True)
    check("hotkey key itself is swallowed", b"1" in xq(":91", "keys").split(), False)
    ctl("key leftctrl 0"); ctl("key leftalt 0")

    # --- clipboard: UTF-8 pc -> Latin-1 octane ---------------------------------
    ctl("goto pc")
    spawn([ROOT + "/build/xq", "setclip", "café “quoted”\nline two"], {"DISPLAY": ":92"})
    time.sleep(0.3)
    ctl("goto octane", 0.8)                      # leaving pc reports its clipboard
    check("pc clipboard arrives on octane as Latin-1",
          xq(":91", "getclip", "latin1"), b"caf\xe9 \"quoted\"\nline two")

    # --- clipboard: simulated Mac (MacRoman, CR line ends) ----------------------
    mac = FakeAgent("mac"); mac.pump()
    check("mac agent welcomed", mac.take()[0], (2, b"\x01\x00"))
    ctl("goto mac"); mac.pump()
    fr = mac.take()
    check("mac gets ENTER", fr[0][0], 0x10)
    check("mac gets clipboard as MacRoman with CR", mac.clip() or b"".join(p for t, p in fr if t == 0x21),
          b"caf\x8e \xd2quoted\xd3\rline two")
    ctl("key leftalt 1"); mac.pump(0.2)
    check("per-screen remap alt -> command", mac.take()[-1], (0x16, bytes([0xE3, 1, 0x08])))
    ctl("key leftalt 0"); mac.pump(0.2); mac.take()
    ctl("rel 10 0"); mac.pump(0.2)
    check("mac gets absolute MOVE", mac.take()[-1][0], 0x12)
    mac.send_clip("naïve\rMac text •".encode("mac_roman"))
    ctl("goto pc", 0.6); mac.pump(0.2)
    check("mac gets LEAVE", (0x11, b"") in mac.take(), True)
    check("mac clipboard arrives on pc as UTF-8", xq(":92", "getclip"), "naïve\nMac text •".encode())

    # --- large clipboards and per-agent size limits ------------------------------
    big = ("0123456789abcdef" * 1875).encode()   # 30000 bytes, many frames
    ctl("goto mac"); mac.pump(0.2); mac.take()
    mac.send_clip(big)
    ctl("goto pc", 0.8); mac.pump(0.2); mac.take()
    check("30 KB clipboard survives chunking", xq(":92", "getclip") == big, True)
    spawn([ROOT + "/build/xq", "setclip", "y" * 100000], {"DISPLAY": ":92"})
    time.sleep(0.3)
    ctl("goto mac", 1.0); mac.pump(0.3)
    check("100 KB clipboard is withheld from an agent that accepts 32 KB",
          [t for t, p in mac.take() if t in (0x20, 0x21, 0x22)], [])
    ctl("goto octane", 0.8)
    check("... but still reaches an agent that can take it", len(xq(":91", "getclip", "latin1")), 100000)

    # --- switcher front panel changes the layout -------------------------------
    ctl("goto octane")
    open(fifo, "w").write("5 1\n")              # someone presses buttons: Mac to the left monitor
    time.sleep(0.6); mac.pump(0.2)
    st = ctl("status")
    check("front-panel tie: left monitor now shows mac", "monitor left" in st and "(input 5) shows=mac" in st, True)
    check("pointer follows the monitor to mac", "active: mac" in st, True)
    ctl("rel 5000 0"); mac.pump(0.2)
    check("mac's right neighbour is pc", "active: pc" in ctl("status"), True)
    ctl("show octane left", 0.6)
    st = ctl("status")
    check("'show' command routes video through the switcher", "(input 3) shows=octane" in st, True)

    # --- agent loss ---------------------------------------------------------------
    ctl("goto octane")
    procs[4].terminate(); time.sleep(0.6)       # octane agent dies
    st = ctl("status")
    check("active falls back when a machine disappears", "active: octane" not in st and "active: (none)" not in st, True)

    # --- relative-only agent (hardware injector) --------------------------------
    inj = FakeAgent("octane", caps=1 | 4, charset=0, eol=0, w=1280, h=1024); inj.pump()
    inj.take()
    ctl("goto octane"); inj.pump(0.2)
    kinds = [t for t, p in inj.take()]
    check("relative agent gets ENTER then homing MOVERELs", kinds[:3], [0x10, 0x13, 0x13])
    ctl("rel 7 -3"); inj.pump(0.2)
    check("relative agent gets MOVEREL deltas", inj.take()[-1], (0x13, struct.pack(">hh", 7, -3)))

    # --- separate clipboard helper sharing a screen name with an input agent ----
    clipfile = TMP + "/desktop-clipboard"
    open(clipfile, "w").write("old")
    lap = FakeAgent("laptop", caps=1, charset=0, eol=0, w=1920, h=1080); lap.pump()
    spawn([sys.executable, ROOT + "/agents/linux/rkm_clip.py", "-n", "laptop", "-p", str(PORT),
           "--copy", "sh -c 'cat > %s'" % clipfile, "--paste", "cat " + clipfile, "127.0.0.1"])
    for _ in range(50):
        if "screen laptop" in ctl("status") and "clipboard=127" in ctl("status").split("screen laptop")[1].split("\n")[0]: break
        time.sleep(0.1)
    ctl("goto laptop", 0.5)
    check("helper receives the hub clipboard on enter", open(clipfile).read(), "y" * 100000)
    open(clipfile, "w").write("copied on the laptop \u2192 ok")
    ctl("goto pc", 0.8)
    check("helper reports a new clipboard on leave", xq(":92", "getclip"), "copied on the laptop \u2192 ok".encode())
finally:
    for p in procs:
        p.terminate()
    print("\nlogs in", TMP)
    print("%d failure(s)" % len(failures))
    sys.exit(1 if failures else 0)

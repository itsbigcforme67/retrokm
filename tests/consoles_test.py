#!/usr/bin/env python3
"""Consoles behind boxes (RetroTINK, switchers) and game pad routing, no X
needed: a simulated pad player
(the Xbox dongle) and a simulated PC agent; the pad is played through the
control socket's "pad" command.  Run after building hub/retrokm-hub."""
import os, socket, struct, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = tempfile.mkdtemp(prefix="rkm-consoles-")
PORT, CTL = 24960, 24961
failures = []

def check(name, got, want):
    ok = got == want
    print("%s %-58s %s" % ("PASS" if ok else "FAIL", name, "" if ok else "got %.90r want %.90r" % (got, want)))
    if not ok: failures.append(name)

class Ctl:
    def __init__(self):
        self.s = socket.create_connection(("127.0.0.1", CTL)); self.s.settimeout(2)
    def __call__(self, cmd, settle=0.15):
        self.s.sendall(cmd.encode() + b"\n")
        time.sleep(settle)
        try: return self.s.recv(65536).decode()
        except socket.timeout: return ""

class FakeAgent:
    def __init__(self, name, caps, w=640, h=480):
        self.s = socket.create_connection(("127.0.0.1", PORT)); self.s.settimeout(0.05)
        self.buf = b""; self.frames = []
        self.send(1, bytes([1, caps, 0, 0]) + struct.pack(">HHH", w, h, 0) + name.encode())
    def send(self, t, payload=b""):
        self.s.sendall(bytes([t]) + struct.pack(">H", len(payload)) + payload)
    def pump(self, wait=0.2):
        end = time.time() + wait
        while time.time() < end:
            try: d = self.s.recv(65536)
            except socket.timeout: continue
            if not d: self.frames.append(("closed", b"")); break
            self.buf += d
            while len(self.buf) >= 3:
                n = struct.unpack(">H", self.buf[1:3])[0]
                if len(self.buf) < 3 + n: break
                t, p = self.buf[0], self.buf[3:3 + n]; self.buf = self.buf[3 + n:]
                if t == 3: self.send(4)
                else: self.frames.append((t, p))
        return self.frames
    def take(self, wait=0.2):
        self.pump(wait)
        f, self.frames = self.frames, []
        return [x for x in f if x[0] != 2]   # WELCOME is not interesting here

def pad(buttons, lt=0, rt=0, lx=0, ly=0, rx=0, ry=0):
    return (0x18, struct.pack(">BHBBhhhh", 0, buttons, lt, rt, lx, ly, rx, ry))

conf = TMP + "/hub.conf"
open(conf, "w").write("""
[hub]
port = %d
control_port = %d
device = none
pad = none

[screen pc]
[screen xbox]
via = switcher
[screen ps2]
via = switcher
[screen switcher]
via = tink
[screen dreamcast]
via = tink
[screen tink]
agent = no
[screen dell]

[monitor left]
pos = 0,0
screen = pc
[monitor right]
pos = 1,0
screen = tink
""" % (PORT, CTL))
log = open(TMP + "/hub.log", "w")
hub = subprocess.Popen([ROOT + "/hub/retrokm-hub", "-v", "-c", conf], stdout=log, stderr=subprocess.STDOUT)
try:
    time.sleep(0.4)
    ctl = Ctl()
    # consoles behind boxes: nothing plugged, the monitor shows the tink itself
    st = ctl("status")
    check("box monitor shows the box with nothing plugged", "shows=tink" in st, True)
    ctl("plug switcher"); ctl("plug xbox")
    check("console through two boxes reaches the monitor", "shows=xbox" in ctl("status"), True)
    check("plug refuses the wrong box", "error" in ctl("plug pc"), True)
    check("plugs are saved", sorted(l for l in open(conf + ".state") if l.startswith("plug")),
          ["plug switcher xbox\n", "plug tink switcher\n"])
    ctl("plug dreamcast")
    check("re-plugging the tink changes the monitor", "shows=dreamcast" in ctl("status"), True)
    ctl("unplug tink")
    check("unplugged: the tink again", "shows=tink" in ctl("status"), True)
    ctl("plug switcher")
    pc = FakeAgent("pc", caps=1)
    xb = FakeAgent("xbox", caps=0x40)
    time.sleep(0.3)
    st = ctl("status")
    check("pad player makes its screen ready", "screen xbox" in st and "pad=127.0.0.1" in st and
          "ready" in st.split("screen xbox")[1].split("\n")[0], True)
    pc.take(); xb.take()

    ctl("goto xbox")
    check("pad player gets ENTER and the pad as it is", [t for t, _ in xb.take()], [0x10, 0x18])
    pc.take()
    ctl("pad 0809 10 255 1000 -2000 32767 -32768")
    check("pad state reaches the player", xb.take(), [pad(0x0809, 10, 255, 1000, -2000, 32767, -32768)])
    check("...and nothing else", pc.take(), [])

    ctl("goto pc")
    check("pad player gets LEAVE when the keyboard goes", [t for t, _ in xb.take()], [0x11])
    ctl("pad 0001")
    check("pad goes nowhere while a PC has the keyboard", (xb.take(), [t for t, _ in pc.take()]), ([], [0x10]))

    ctl("pad 0100"); ctl("pad 0120")          # Guide, then Guide + RB
    check("Guide+RB hands the keyboard and pad over", "active: xbox" in ctl("status"), True)
    got = xb.take()
    check("...the player gets ENTER, the combo is not passed on", got, [(0x10, b""), pad(0)])
    ctl("pad 0100")                           # RB up, Guide still held: held back
    ctl("pad 0000")
    ctl("pad 0001")
    check("combo buttons held back until Guide is let go", xb.take(), [pad(1)])

    xb.send(0x19, struct.pack(">BHH", 0, 0xC000, 0x4000))   # rumble, no real pad here
    time.sleep(0.1)
    check("rumble without a real pad is harmless", "active: xbox" in ctl("status"), True)

    ctl("goto pc"); xb.take()
    ctl("rel 5000 0")                         # the mouse walks onto the tink's monitor
    check("the mouse reaches the xbox through the tink", "active: xbox" in ctl("status"), True)

    xb2 = FakeAgent("xbox", caps=0x40)        # the dongle rebooted
    time.sleep(0.3)
    check("a new pad connection replaces the old one", ("closed", b"") in xb.pump(0.3), True)
    ctl("pad 0002")
    check("...and gets the pad (as it is, then the change)", [f for f in xb2.take() if f[0] == 0x18], [pad(1), pad(2)])

    # --- accessories: links, players, out of focus -----------------------------
    ctl("pad 0")
    st = ctl("status")
    check("the keyboard is an accessory linked everywhere",
          "accessory keyboard" in st and "pc xbox ps2" in st.split("accessory keyboard")[1].split("\n")[0], True)
    ctl("unlink keyboard xbox")
    ctl("goto pc"); xb2.take(); pc.take()
    ctl("rel 5000 0")
    check("the mouse cannot reach a machine the keyboard is not linked to", "active: pc" in ctl("status"), True)
    ctl("link keyboard xbox")
    pc2 = FakeAgent("dell", caps=1)
    time.sleep(0.2)
    ctl("keep keyboard dell on")
    pc.take(); pc2.take()
    ctl("key a 1"); ctl("key a 0")
    check("keys reach the machine in focus", [t for t, _ in pc.take()].count(0x16), 2)
    check("...and a linked machine that takes keys out of focus", [t for t, _ in pc2.take()].count(0x16), 2)
    ctl("keep keyboard dell off"); pc2.take()
    ctl("key b 1"); ctl("key b 0")
    check("...not once that is off", [t for t, _ in pc2.take()].count(0x16), 0)

    # the xbox takes pad input while out of focus; scripts are player 1 everywhere
    ctl("keep script xbox on")
    xb2.take()
    ctl("pad 0004")
    check("a pad machine out of focus still gets the pad when told to", xb2.take(), [pad(4)])
    check("keeps are saved", "keep script xbox" in open(conf + ".state").read(), True)
    ctl("keep script xbox off")
    check("...and lets go when that is turned off", [t for t, _ in xb2.take()], [0x11])
    ctl("pad 0")
    ctl("keepall script on")
    xb2.take()
    ctl("pad 0004")
    check("keep everywhere works too", xb2.take()[-1:], [pad(4)])
    ctl("keepall script off"); ctl("pad 0"); xb2.take()
    check("toggles are saved", sorted(l.strip() for l in open(conf + ".state") if l.startswith("keep")), [])
    check("links are saved", "link keyboard xbox 1" in open(conf + ".state").read(), True)
except Exception:  # a crash in the test itself is a failure, not a quiet stop
    import traceback
    traceback.print_exc()
    failures.append("the test crashed")
finally:
    hub.terminate()
    print("\nlogs in", TMP)
    print("%d failure(s)" % len(failures))
    sys.exit(1 if failures else 0)

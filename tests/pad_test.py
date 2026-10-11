#!/usr/bin/env python3
"""Game pad routing through the hub, no X needed: a simulated pad player
(the Xbox dongle) and a simulated PC agent; the pad is played through the
control socket's "pad" command.  Run after building hub/retrokm-hub."""
import os, socket, struct, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = tempfile.mkdtemp(prefix="rkm-pad-")
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
[screen dell]

[monitor left]
pos = 0,0
screen = pc
[monitor right]
pos = 1,0
screen = xbox
""" % (PORT, CTL))
log = open(TMP + "/hub.log", "w")
hub = subprocess.Popen([ROOT + "/hub/retrokm-hub", "-v", "-c", conf], stdout=log, stderr=subprocess.STDOUT)
try:
    time.sleep(0.4)
    ctl = Ctl()
    pc = FakeAgent("pc", caps=1)
    xb = FakeAgent("xbox", caps=0x40)
    time.sleep(0.3)
    st = ctl("status")
    check("pad player makes its screen ready", "screen xbox" in st and "pad=127.0.0.1" in st and
          "ready" in st.split("screen xbox")[1].split("\n")[0], True)
    pc.take(); xb.take()

    ctl("goto xbox")
    check("pad player gets ENTER", [t for t, _ in xb.take()], [0x10])
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
    check("...the player gets ENTER, the combo is not passed on", [t for t, _ in got], [0x10])
    ctl("pad 0100")                           # RB up, Guide still held: held back
    ctl("pad 0000")
    ctl("pad 0001")
    check("combo buttons held back until Guide is let go", xb.take(), [pad(0), pad(0), pad(1)])

    xb.send(0x19, struct.pack(">BHH", 0, 0xC000, 0x4000))   # rumble, no real pad here
    time.sleep(0.1)
    check("rumble without a real pad is harmless", "active: xbox" in ctl("status"), True)

    xb2 = FakeAgent("xbox", caps=0x40)        # the dongle rebooted
    time.sleep(0.3)
    check("a new pad connection replaces the old one", ("closed", b"") in xb.pump(0.3), True)
    ctl("pad 0002")
    check("...and gets the pad", [f for f in xb2.take() if f[0] == 0x18], [pad(2)])
finally:
    hub.terminate()
    print("\nlogs in", TMP)
    print("%d failure(s)" % len(failures))
    sys.exit(1 if failures else 0)

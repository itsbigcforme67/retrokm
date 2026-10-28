#!/usr/bin/env python3
"""Claude's keyboard ("key-to <screen> <key> <0|1>" on the control socket):
a script types on one named machine while the user's keyboard stays on
another, and the two never cross.  No X needed: a simulated PS/2 keyboard
(the NanoC6 on the Octane), two simulated PC agents and a pad player.
Run after building hub/retrokm-hub."""
import os, socket, struct, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = tempfile.mkdtemp(prefix="rkm-keyto-")
PORT, CTL = 24970, 24971
failures = []

def check(name, got, want):
    ok = got == want
    print("%s %-58s %s" % ("PASS" if ok else "FAIL", name, "" if ok else "got %.90r want %.90r" % (got, want)))
    if not ok: failures.append(name)

class Ctl:
    def __init__(self):
        self.s = socket.create_connection(("127.0.0.1", CTL)); self.s.settimeout(2)
    def __call__(self, cmd, settle=0.1):
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
    def take(self, wait=0.2):
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
                elif t != 2: self.frames.append((t, p))
        f, self.frames = self.frames, []
        return f

def keys(frames):
    return [(p[0], p[1]) for t, p in frames if t == 0x16]

conf = TMP + "/hub.conf"
open(conf, "w").write("""
[hub]
port = %d
control_port = %d
device = none
pad = none

[screen pc]
[screen octane]
[screen dell]
[screen xbox]

[monitor left]
pos = 0,0
screen = pc
""" % (PORT, CTL))
log = open(TMP + "/hub.log", "w")
hub = subprocess.Popen([ROOT + "/hub/retrokm-hub", "-v", "-c", conf], stdout=log, stderr=subprocess.STDOUT)
try:
    time.sleep(0.4)
    ctl = Ctl()
    pc = FakeAgent("pc", caps=1)
    octane = FakeAgent("octane", caps=0x08)      # a hardware keyboard, like the NanoC6
    dell = FakeAgent("dell", caps=1)
    xbox = FakeAgent("xbox", caps=0x40)          # pad only: no keyboard
    time.sleep(0.3)
    st = ctl("status")
    check("Claude's keyboard is an accessory", "accessory claude" in st and "keys here" in st, True)
    ctl("goto pc")
    for a in (pc, octane, dell, xbox): a.take()

    # the user types on the pc while Claude types "A" on the octane, interleaved
    ctl("key h 1")
    check("key-to answers ok", ctl("key-to octane leftshift 1").strip(), "ok")
    ctl("key h 0"); ctl("key-to octane a 1"); ctl("key i 1"); ctl("key-to octane a 0")
    ctl("key i 0"); ctl("key-to octane leftshift 0")
    check("the octane gets Claude's keys only", keys(octane.take()),
          [(0xE1, 1), (0x04, 1), (0x04, 0), (0xE1, 0)])
    check("the pc gets the user's keys only", keys(pc.take()), [(0x0B, 1), (0x0B, 0), (0x0C, 1), (0x0C, 0)])
    check("nothing on the dell", keys(dell.take()), [])
    st = ctl("status")
    check("the user's focus did not move", st.startswith("active: pc"), True)

    # Claude's shift is its own: it does not shift the user's keys, and back
    ctl("key-to octane leftshift 1"); ctl("key j 1")
    pc_fr = [p for t, p in pc.take() if t == 0x16]
    check("user's key carries the user's modifiers, not Claude's", pc_fr[-1][2] if pc_fr else None, 0)
    ctl("key j 0"); ctl("key-to octane leftshift 0"); octane.take(); pc.take()

    # where it may not go
    check("no such screen", ctl("key-to nowhere a 1").startswith("error"), True)
    check("a screen with no keyboard", ctl("key-to xbox a 1").startswith("error"), True)
    check("unknown key", ctl("key-to octane notakey 1").startswith("error"), True)
    ctl("keepall claude off")
    check("not kept on the octane: refused", ctl("key-to octane a 1").startswith("error"), True)
    check("...but the machine in focus still takes it", ctl("key-to pc a 1").strip(), "ok")
    ctl("key-to pc a 0")
    ctl("keep claude octane on")
    check("kept on the octane again", ctl("key-to octane a 1").strip(), "ok")
    ctl("key-to octane a 0")
    ctl("unlink claude octane")
    check("unlinked: refused", ctl("key-to octane a 1").startswith("error"), True)
    check("refusals send nothing anywhere", (keys(dell.take()), keys(xbox.take())), ([], []))
    octane.s.close(); time.sleep(0.3)
    ctl("link claude octane"); ctl("keep claude octane on")
    check("the octane's keyboard gone: refused", ctl("key-to octane a 1").startswith("error"), True)
    check("...and nothing fell back to the pc", keys(pc.take()), [(0x04, 1), (0x04, 0)])  # from key-to pc above
    state = open(conf + ".state").read()
    check("Claude's keyboard settings are saved", ("keep claude octane" in state, "keepall claude" in state), (True, False))
finally:
    hub.terminate(); hub.wait()
print("%d failure(s)" % len(failures) if failures else "all passed")
sys.exit(1 if failures else 0)

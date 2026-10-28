#!/usr/bin/env python3
"""Seats (docs/SEATS.md): the first seat (the user) and Claude's seat drive
different machines at once and never cross; seats move like a hand on a
mouse (edges), may only go where their wires go, share a machine when they
meet, keep a clipboard each, go offline.  No X needed: a simulated PS/2
keyboard (the NanoC6 on the Octane), simulated PC agents and a pad player.
Run after building hub/retrokm-hub."""
import os, socket, struct, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = tempfile.mkdtemp(prefix="rkm-seats-")
PORT, CTL = 24970, 24971
ENTER, LEAVE, MOVE, BUTTON, KEY, CLIP_BEGIN, CLIP_DATA, CLIP_END = 0x10, 0x11, 0x12, 0x14, 0x16, 0x20, 0x21, 0x22
failures = []

def check(name, got, want):
    ok = got == want
    print("%s %-62s %s" % ("PASS" if ok else "FAIL", name, "" if ok else "got %.100r want %.100r" % (got, want)))
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
        self.send(1, bytes([1, caps, 3, 0]) + struct.pack(">HHH", w, h, 64) + name.encode())
    def send(self, t, payload=b""):
        self.s.sendall(bytes([t]) + struct.pack(">H", len(payload)) + payload)
    def send_clip(self, data):
        self.send(CLIP_BEGIN, bytes([1]) + struct.pack(">I", len(data)))
        self.send(CLIP_DATA, data)
        self.send(CLIP_END)
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
    return [(p[0], p[1]) for t, p in frames if t == KEY]

def types(frames):
    return [t for t, p in frames]

def clip(frames):
    return b"".join(p for t, p in frames if t == CLIP_DATA)

def seat_line(st, name):
    """The words after "seat <name>" in status"""
    return next((l for l in st.splitlines() if l.startswith("seat %s " % name)), "").split()[2:]

conf = TMP + "/hub.conf"
open(conf, "w").write("""
[hub]
port = %d
control_port = %d
device = none
pad = none

[seat james]
color = 22c55e

[screen pc]
[screen dell]
[screen octane]
[screen xbox]

[monitor left]
pos = 0,0
screen = pc
[monitor right]
pos = 1,0
screen = dell
""" % (PORT, CTL))
# a state file from before seats: Claude's keyboard was a "keys" accessory kept everywhere
open(conf + ".state", "w").write("""accessory keyboard kbm Keyboard + mouse
link keyboard pc 1
link keyboard dell 1
link keyboard octane 1
link keyboard xbox 1
accessory claude keys Claude's keyboard
link claude pc 1
link claude dell 1
link claude octane 1
link claude xbox 1
keepall claude
""")
log = open(TMP + "/hub.log", "w")
hub = subprocess.Popen([ROOT + "/hub/retrokm-hub", "-v", "-c", conf], stdout=log, stderr=subprocess.STDOUT)
try:
    time.sleep(0.4)
    ctl = Ctl()
    pc = FakeAgent("pc", caps=3, w=1000, h=800)       # input + clipboard
    dell = FakeAgent("dell", caps=3, w=1000, h=800)
    octane = FakeAgent("octane", caps=0x08)            # a hardware keyboard, like the NanoC6
    xbox = FakeAgent("xbox", caps=0x40)                # pad only: no keyboard
    time.sleep(0.3)
    ctl("goto pc")
    st = ctl("status")
    check("the config's seat is the first, Claude's is made", (seat_line(st, "james")[:2], seat_line(st, "claude")[0]),
          (["on", "pc"], "nowhere"))
    check("Claude's keyboard is now Claude's seat's keyboard + mouse",
          "accessory claude" in st and "kbm here seat=claude" in st, True)
    for a in (pc, dell, octane, xbox): a.take()

    # --- the user types on the pc while Claude types "A" on the octane, interleaved -------------
    ctl("key h 1")
    check("key-to answers ok", ctl("key-to octane leftshift 1").strip(), "ok")
    ctl("key h 0"); ctl("key-to octane a 1"); ctl("key i 1"); ctl("key-to octane a 0")
    ctl("key i 0"); ctl("key-to octane leftshift 0")
    fr = octane.take()
    check("the octane: ENTER, then Claude's keys only", (types(fr)[:1], keys(fr)),
          ([ENTER], [(0xE1, 1), (0x04, 1), (0x04, 0), (0xE1, 0)]))
    check("the pc gets the user's keys only", keys(pc.take()), [(0x0B, 1), (0x0B, 0), (0x0C, 1), (0x0C, 0)])
    check("nothing on the dell", keys(dell.take()), [])
    st = ctl("status")
    check("the user stayed on the pc, Claude is on the octane",
          (st.startswith("active: pc"), seat_line(st, "claude")[:2]), (True, ["on", "octane"]))
    ctl("seat claude key leftshift 1"); ctl("key j 1")
    pc_fr = [p for t, p in pc.take() if t == KEY]
    check("the user's key carries the user's modifiers, not Claude's", pc_fr[-1][2] if pc_fr else None, 0)
    ctl("key j 0"); ctl("seat claude key leftshift 0"); octane.take(); pc.take()

    # --- refusals -------------------------------------------------------------------------------
    check("no such screen", ctl("key-to nowhere a 1").startswith("error"), True)
    check("a screen with no keyboard", ctl("key-to xbox a 1").startswith("error"), True)
    check("unknown key", ctl("key-to octane notakey 1").startswith("error"), True)
    check("no such seat", ctl("seat nobody key a 1").startswith("error"), True)

    # --- a seat's mouse: goto, warp, click, edges ------------------------------------------------
    check("seat claude goto dell", ctl("seat claude goto dell").strip(), "ok")
    fr = octane.take()
    check("the octane hears LEAVE when Claude goes", types(fr), [LEAVE])
    ctl("seat claude warp 100 200"); ctl("seat claude btn 1 1"); ctl("seat claude btn 1 0")
    fr = dell.take()
    check("the dell: ENTER, Claude's pointer, a click", [(t, p) for t, p in fr if t != CLIP_BEGIN and t != CLIP_DATA and t != CLIP_END][1:],
          [(MOVE, struct.pack(">HH", 100, 200)), (BUTTON, b"\x01\x01"), (BUTTON, b"\x01\x00")])
    check("...and nothing on the pc", pc.take(), [])
    ctl("rel 5 5")
    check("the user's pointer still moves the pc", types(pc.take()), [MOVE])
    check("...not the dell", dell.take(), [])
    ctl("seat claude rel -3000 0", 0.2)                # off the dell's left edge: onto the pc
    st = ctl("status")
    check("Claude crosses the edge onto the pc like a mouse", seat_line(st, "claude")[:2], ["on", "pc"])
    fr = pc.take()
    check("...no second ENTER on a machine the user is on", ENTER in types(fr), False)
    check("...the dell hears LEAVE", LEAVE in types(dell.take()), True)
    ctl("key x 1"); ctl("key x 0"); ctl("seat claude key y 1"); ctl("seat claude key y 0")
    check("two seats on one machine: their keys merge", keys(pc.take()), [(0x1B, 1), (0x1B, 0), (0x1C, 1), (0x1C, 0)])
    ctl("seat claude key z 1")
    ctl("goto dell")
    fr = pc.take()
    check("the user leaving a shared machine: no LEAVE there", LEAVE in types(fr), False)
    ctl("seat claude key z 0"); pc.take(); dell.take()
    ctl("seat claude key z 1"); ctl("seat claude goto octane")
    fr = pc.take()
    check("the last seat leaving: Claude's held key let go, LEAVE", (types(fr)[-1:], LEAVE in types(fr)), ([LEAVE], True))
    ctl("seat claude key z 0"); octane.take()
    ctl("goto pc"); dell.take(); pc.take()

    # --- wires are permissions ----------------------------------------------------------------
    ctl("unlink claude dell")
    check("not wired: goto refused", ctl("seat claude goto dell").startswith("error"), True)
    ctl("seat claude goto pc"); pc.take()
    ctl("seat claude rel 3000 0", 0.2)
    check("not wired: no edge crossing either", seat_line(ctl("status"), "claude")[:2], ["on", "pc"])
    check("...nothing reached the dell", [t for t in types(dell.take()) if t != MOVE], [])
    ctl("unlink claude octane")
    check("unlinked: key-to refused", ctl("key-to octane a 1").startswith("error"), True)
    check("refusals send nothing anywhere", (keys(octane.take()), keys(xbox.take())), ([], []))
    ctl("link claude octane"); ctl("link claude dell")

    # --- one clipboard per seat -----------------------------------------------------------------
    ctl("seat claude goto dell"); dell.take(); pc.take()
    dell.send_clip(b"claude copied this"); time.sleep(0.2)
    pc.send_clip(b"the user copied this"); time.sleep(0.2)
    st = ctl("status")
    check("each seat's copy is its own", ("clipboard claude: 18 bytes" in st, "clipboard james: 20 bytes" in st), (True, True))
    ctl("seat claude goto pc")
    check("Claude arriving brings Claude's clipboard", clip(pc.take()), b"claude copied this")
    ctl("seat claude goto octane"); ctl("goto dell")
    check("the user arriving brings the user's", clip(dell.take()), b"the user copied this")
    ctl("goto pc"); pc.take(); dell.take()

    # --- offline ----------------------------------------------------------------------------------
    ctl("seat claude offline")
    check("offline: no ring", seat_line(ctl("status"), "claude")[0], "offline")
    check("offline: the octane hears LEAVE", types(octane.take())[-1:], [LEAVE])
    check("key-to brings Claude back", ctl("key-to octane a 1").strip(), "ok")
    ctl("key-to octane a 0")
    check("...on the octane", seat_line(ctl("status"), "claude")[:2], ["on", "octane"])
    octane.take()

    # --- seats come and go; accessories change hands ---------------------------------------------
    check("seat add", ctl("seat add guest 3366ff").strip(), "ok")
    ctl("own keyboard guest")
    check("the keyboard is the guest's now", "kbm here seat=guest" in ctl("status"), True)
    ctl("seat guest goto dell"); dell.take()
    ctl("seat remove guest")
    st = ctl("status")
    check("seat removed: its keyboard back to the first seat", ("seat guest" in st, "accessory keyboard" in st and
          [l for l in st.splitlines() if l.startswith("accessory keyboard")][0].find("seat=james") > 0), (False, True))
    check("...and it let go of the dell", LEAVE in types(dell.take()), True)

    # --- the old Claude's keyboard came over and the seats are saved ----------------------------
    octane.s.close(); time.sleep(0.3)
    check("the octane's keyboard gone: refused", ctl("key-to octane a 1").startswith("error"), True)
    check("...and nothing fell back to the pc", keys(pc.take()), [])
    ctl("keep claude octane on")                       # anything that saves the state
    state = open(conf + ".state").read()
    check("seats saved, Claude's old keep-everywhere dropped",
          ("seat james 22c55e" in state, "aseat claude claude" in state, "keepall claude" in state, "keep claude octane" in state),
          (True, True, False, True))
finally:
    hub.terminate(); hub.wait()
print("logs in %s" % TMP)
print("%d failure(s)" % len(failures) if failures else "all passed")
sys.exit(1 if failures else 0)

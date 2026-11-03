#!/usr/bin/env python3
"""Desk pets (docs/PETS.md): the engine in the hub with one pet pack, on
simulated agents that can show the buddy window.  The pet gets its look,
wanders by weight (never where the weight is 0), travels (looks where it is
going, the destination says "on my way"), minds its manners (leaves a
machine someone types on, not where someone is typing, stays when called),
looks at a wiggled mouse, and a brain on the panel port can drive it.
No X needed.  Run after building hub/retrokm-hub."""
import os, socket, struct, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = tempfile.mkdtemp(prefix="rkm-pets-")
PORT, CTL, PANEL = 24980, 24981, 24982
BUDDY, BUDDY_EVENT, LOOK, GAZE, SAY = 0x30, 0x31, 0x34, 0x35, 0x36
INPUT_CLIP_BUDDY = 0x13
failures = []
AGENTS = []

def nap(t):
    """Wait, answering the hub's pings for every fake agent meanwhile"""
    end = time.time() + t
    while True:
        for a in AGENTS: a.pump()
        if time.time() >= end: break
        time.sleep(0.02)

def check(name, got, want):
    ok = got == want
    print("%s %-62s %s" % ("PASS" if ok else "FAIL", name, "" if ok else "got %.120r want %.120r" % (got, want)))
    if not ok: failures.append(name)

class Ctl:
    def __init__(self):
        self.s = socket.create_connection(("127.0.0.1", CTL)); self.s.settimeout(2)
    def __call__(self, cmd, settle=0.05):
        self.s.sendall(cmd.encode() + b"\n")
        nap(settle)
        try: return self.s.recv(65536).decode()
        except socket.timeout: return ""

class Panel:
    """A brain on the panel port: sends commands, collects pet / buddy lines"""
    def __init__(self):
        self.s = socket.create_connection(("127.0.0.1", PANEL)); self.s.settimeout(0.05)
        self.buf = b""; self.lines = []
    def send(self, cmd):
        self.s.sendall(cmd.encode() + b"\n")
    def take(self, wait=0.2):
        end = time.time() + wait
        while True:
            for a in AGENTS: a.pump()
            try: d = self.s.recv(65536)
            except socket.timeout: d = None
            if d == b"": break
            self.buf += d or b""
            while b"\n" in self.buf:
                l, self.buf = self.buf.split(b"\n", 1)
                l = l.decode(errors="replace")
                if l.startswith(("pet ", "buddy ", "error")): self.lines.append(l)
            if time.time() >= end: break
        f, self.lines = self.lines, []
        return f
    def ask(self, cmd, wait=0.2):
        self.take(0.05)
        self.send(cmd)
        return self.take(wait)
    def wait_for(self, prefix, timeout):
        """Lines until one starts with prefix (that one last), or all of them at the timeout"""
        got, end = [], time.time() + timeout
        while time.time() < end:
            got += self.take(0.1)
            if any(l.startswith(prefix) for l in got): break
            self.send("ping")
        return got

class FakeAgent:
    def __init__(self, name, caps=INPUT_CLIP_BUDDY, w=1000, h=800):
        self.name = name
        self.s = socket.create_connection(("127.0.0.1", PORT)); self.s.setblocking(False)
        self.buf = b""; self.frames = []; self.closed = False
        AGENTS.append(self)
        self.send(1, bytes([1, caps, 0, 0]) + struct.pack(">HHH", w, h, 64) + name.encode())
    def send(self, t, payload=b""):
        self.s.sendall(bytes([t]) + struct.pack(">H", len(payload)) + payload)
    def pump(self):
        """Read what has come, keep the buddy frames, answer pings"""
        while not self.closed:
            try: d = self.s.recv(65536)
            except (BlockingIOError, InterruptedError): return
            if not d: self.close(); return
            self.buf += d
            while len(self.buf) >= 3:
                n = struct.unpack(">H", self.buf[1:3])[0]
                if len(self.buf) < 3 + n: break
                t, p = self.buf[0], self.buf[3:3 + n]; self.buf = self.buf[3 + n:]
                if t == 3: self.send(4)
                elif t in (BUDDY, LOOK, GAZE, SAY): self.frames.append((t, p))
    def take(self, wait=0.2):
        nap(wait)
        f, self.frames = self.frames, []
        return f
    def close(self):
        if not self.closed: self.s.close()
        self.closed = True
        if self in AGENTS: AGENTS.remove(self)

def buddy(frames):
    """BUDDY frames as ("show", label) / ("hide", "")"""
    return [("show" if p[0] else "hide", p[1:].decode()) for t, p in frames if t == BUDDY]

def gazes(frames):
    return [(p[0], struct.unpack("b", p[1:2])[0], struct.unpack("b", p[2:3])[0]) for t, p in frames if t == GAZE]

def says(frames):
    return [p[1:].decode() for t, p in frames if t == SAY]

def place_of(lines):
    l = [x for x in lines if x.startswith("pet testpet arrived ")]
    return l[-1].split()[-1] if l else None

PACK = """name = Testy
[look]
shape = 1.0
gap = 0.4
pupil = 0.3
colors = yes
background = plain
paper = 102030
eye = ffffff
ink = 405060
lid = 0.2
blink = 3
mouth = 0.4
smile = -0.5
[wander]
every = %(every)s
stay = 0.04
[places]
* = 1
work = 0
[manners]
typing = 3
[travel]
time = %(trip)s
[looking]
glance = %(glance)s
follow = 0
wiggle = 3
[words]
arrive = hello
leave = bye
click = hey!
summoned = you called?
coming = on my way
"""

CONF = """
[hub]
port = %d
control_port = %d
panel_port = %d
device = none
pad = none
%s
[screen pc]
[screen tv]
[screen work]
[screen old]

[monitor left]
pos = 0,0
screen = pc
[monitor right]
pos = 1,0
screen = tv
[monitor above]
pos = 0,-1
screen = work
[monitor far]
pos = 2,0
screen = old
"""

def start_hub(tag, pet_line, every="0", glance="0", trip="1.5"):
    os.makedirs(TMP + "/pets/testpet", exist_ok=True)
    open(TMP + "/pets/testpet/pet.conf", "w").write(PACK % {"every": every, "glance": glance, "trip": trip})
    conf = TMP + "/%s.conf" % tag
    open(conf, "w").write(CONF % (PORT, CTL, PANEL, pet_line))
    log = open(TMP + "/%s.log" % tag, "w")
    hub = subprocess.Popen([ROOT + "/hub/retrokm-hub", "-v", "-c", conf], stdout=log, stderr=subprocess.STDOUT)
    time.sleep(0.4)
    return hub

def stop_hub(hub):
    hub.terminate()
    try: hub.wait(3)
    except subprocess.TimeoutExpired: hub.kill()
    for a in list(AGENTS): a.close()
    time.sleep(0.2)

# ===== A: one pet, no wandering of its own: arrival, travel, brain, manners, looking ====
hub = start_hub("a", "pet = testpet\npets = %s/pets" % TMP)
try:
    panel = Panel()
    ctl = Ctl()
    pc, tv, work = FakeAgent("pc"), FakeAgent("tv"), FakeAgent("work")
    old = FakeAgent("old", caps=0x03)                 # an agent without the buddy window
    ctl("goto pc"); ctl("warp 500 400")
    lines = panel.wait_for("pet testpet arrived", 4)
    fr = {a.name: a.take() for a in (pc, tv, work, old)}

    look = [p for t, p in fr["tv"] if t == LOOK]
    check("each buddy agent gets the pack's look", [len([1 for t, p in fr[n] if t == LOOK]) for n in ("pc", "tv", "work")], [1, 1, 1])
    check("the look's numbers (shape, gap, pupil, flags, lid, blink, mouth, smile)",
          tuple(look[0][:10]) if look else None, (10, 40, 30, 12, 16, 3, 20, 3, 40, 256 - 50))
    check("the look's colours and name", (look[0][10:19].hex(), look[0][20:].decode()) if look else None,
          ("102030ffffff405060", "Testy"))
    check("an agent without the buddy window gets none of it", fr["old"], [])
    first = place_of(lines)
    check("it turns up on a place it may wander to", first in ("pc", "tv"), True)
    check("... shown there with its name", buddy(fr.get(first, [])), [("show", "Testy")])
    check("... and says hello", says(fr.get(first, [])), ["hello"])
    check("never on a place of weight 0", buddy(fr["work"]), [])
    check("where says so", panel.ask("pet testpet where"), ["pet testpet at %s" % first])
    check("by its name too", panel.ask("pet Testy where"), ["pet testpet at %s" % first])
    check("another name is not a pet here", panel.ask("pet rex where"), ["error no pet rex"])

    # --- the brain sends it to the pc (from wherever it is) and then to the tv --------------
    if first != "pc":
        panel.send("pet testpet go pc"); panel.wait_for("pet testpet arrived pc", 3)
        for a in (pc, tv): a.take()
    panel.send("pet testpet go tv")
    nap(0.3)
    src = pc.take(0.1)
    check("the place it leaves looks the way it goes (right, to the tv)", gazes(src)[:1], [(1, 90, 0)])
    check("... and says goodbye", says(src), ["bye"])
    check("where: travelling", panel.ask("pet testpet where"), ["pet testpet travelling pc tv"])
    nap(0.9)
    src, dst = pc.take(0.1), tv.take(0.1)
    check("then the pc sees it off", buddy(src), [("hide", "")])
    check("the tv says it is on its way, looking left for it", (buddy(dst), gazes(dst)[:1]),
          ([("show", "on my way")], [(1, -90, 0)]))
    lines = panel.wait_for("pet testpet arrived", 3)
    dst = tv.take(0.1)
    check("after the trip it arrives on the tv", (place_of(lines), buddy(dst), says(dst)), ("tv", [("show", "Testy")], ["hello"]))
    check("where: at the tv", panel.ask("pet testpet where"), ["pet testpet at tv"])
    check("a place without a buddy window cannot have it", panel.ask("pet testpet go old"), ["error old cannot show the pet"])

    # --- say and look ----------------------------------------------------------------------
    panel.send("pet testpet say hello there, tv"); nap(0.2)
    check("say: a caption on the tv", says(tv.take()), ["hello there, tv"])
    panel.send("pet testpet look 0.5 -0.5"); nap(0.2)
    check("look: its eyes go there", gazes(tv.take()), [(1, 50, -50)])
    panel.send("pet testpet look pointer"); nap(0.2)
    check("look pointer", gazes(tv.take()), [(2, 0, 0)])
    panel.send("pet testpet look free"); nap(0.2)
    check("look free", gazes(tv.take()), [(0, 0, 0)])

    # --- clicked, summoned --------------------------------------------------------------------
    tv.send(BUDDY_EVENT, b"\x01")
    lines = panel.take(0.3)
    check("a click: the old buddy line and the pet's", lines, ["buddy tv click", "pet testpet clicked tv"])
    fr = tv.take()
    check("... it says something and looks at the pointer", (says(fr), gazes(fr)[:1]), (["hey!"], [(2, 0, 0)]))
    pc.send(BUDDY_EVENT, b"\x01")
    check("a click where it is not is only the old line", panel.take(0.3), ["buddy pc click"])
    pc.send(BUDDY_EVENT, b"\x02")
    lines = panel.wait_for("pet testpet arrived", 4)
    check("called from the pc: summoned, then it comes", [l for l in lines if l.startswith("pet")],
          ["pet testpet summoned pc", "pet testpet arrived pc"])
    pc.send(BUDDY_EVENT, b"\x02")
    nap(0.2)
    check("called where it already is: it answers", says(pc.take()), ["hello", "you called?"])

    # --- manners ----------------------------------------------------------------------------
    ctl("goto pc"); ctl("key a 1"); ctl("key a 0")
    nap(0.6)
    check("just called: typing does not chase it off", panel.ask("pet testpet where"), ["pet testpet at pc"])
    nap(3.0)                                   # stay (2.4 s) is over, the key is old
    ctl("goto tv"); ctl("key b 1"); ctl("key b 0")    # someone types on the tv
    ctl("goto pc"); ctl("key c 1"); ctl("key c 0")    # ... and on the pc
    nap(0.6)
    check("typing where it is, but the only other place is typed on: it stays",
          panel.ask("pet testpet where"), ["pet testpet at pc"])
    nap(3.0)                                   # the tv's typing is old now
    for a in (pc, tv): a.take()
    ctl("key d 1"); ctl("key d 0")                    # more typing on the pc
    lines = panel.wait_for("pet testpet arrived", 4)
    check("typing where it is: it moves on, to the tv", place_of(lines), "tv")
    check("never to the place of weight 0", buddy(work.take()), [])

    # --- looking: a wiggled mouse --------------------------------------------------------------
    ctl("goto pc"); ctl("warp 500 400"); nap(0.2); tv.take()
    for i in range(14): ctl("rel %d 0" % (40 if i % 2 else -40), 0.02)
    nap(0.3)
    g = gazes(tv.take())
    check("a wiggle on the pc: the pet on the tv looks left at it", bool(g) and g[0][0] == 1 and g[0][1] < -50, True)
    nap(6)
    check("then it looks about on its own again", gazes(tv.take())[-1:], [(0, 0, 0)])
    ctl("goto tv"); ctl("warp 500 400"); nap(0.2)
    check("the pointer coming over is not enough", gazes(tv.take()), [])
    for i in range(14): ctl("rel %d 0" % (40 if i % 2 else -40), 0.02)
    nap(0.3)
    check("a wiggle on its own machine: it watches the pointer", gazes(tv.take()), [(2, 0, 0)])
    nap(6); tv.take()

    # --- the machine it is on goes away --------------------------------------------------------
    tv.close()
    lines = panel.wait_for("pet testpet arrived", 4)
    check("its machine went: it says so and turns up elsewhere",
          ([l for l in lines if l.startswith("pet")][:1], place_of(lines)), (["pet testpet left tv"], "pc"))
    tv = FakeAgent("tv"); nap(0.3)
    check("a machine that comes back gets the look again", [t for t, p in tv.take()], [LOOK])
finally:
    stop_hub(hub)

# ===== B: wandering by weight, and glances ================================================
hub = start_hub("b", "pet = testpet\npets = %s/pets" % TMP, every="0.01", glance="0.4", trip="0.3")
try:
    panel = Panel()
    pc, tv, work = FakeAgent("pc"), FakeAgent("tv"), FakeAgent("work")
    seen, end = [], time.time() + 9
    while time.time() < end:
        seen += [l.split()[-1] for l in panel.take(0.2) if l.startswith("pet testpet arrived")]
        panel.send("ping")
    fr = {a.name: a.take() for a in (pc, tv, work)}
    check("it wanders between the pc and the tv", (seen.count("pc") >= 2, seen.count("tv") >= 2), (True, True))
    check("... and never to the place of weight 0", ("work" in seen, buddy(fr["work"])), (False, []))
    check("now and then it glances about", any(m == 1 for m, x, y in gazes(fr["pc"]) + gazes(fr["tv"])), True)
finally:
    stop_hub(hub)

# ===== C: no pet in the config: the buddy window works as before ===========================
hub = start_hub("c", "")
try:
    panel = Panel()
    pc = FakeAgent("pc"); nap(0.3)
    check("no pet: no look is sent", pc.take(), [])
    check("pet commands say there is none", panel.ask("pet testpet where"), ["error there is no pet (pet = in the config)"])
    panel.send("buddy pc show someone"); nap(0.2)
    check("buddy show still works", buddy(pc.take()), [("show", "someone")])
finally:
    stop_hub(hub)

print("%d failure(s)" % len(failures) if failures else "all passed")
sys.exit(1 if failures else 0)

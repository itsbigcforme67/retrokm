#!/usr/bin/env python3
"""Video devices, wires and routes (docs/SWITCHING.md), no X needed: three
simulated Extron switchers in a chain (mps -> mvx -> mtx), consoles on their
inputs, monitors on their outputs; "show" has to work out the ties.  Also
the old [extron] / input = / output = form.  Run after building
hub/retrokm-hub."""
import os, socket, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = tempfile.mkdtemp(prefix="rkm-switching-")
PORT, CTL = 24970, 24971
failures, procs = [], []

def check(name, got, want):
    ok = got == want
    print("%s %-58s %s" % ("PASS" if ok else "FAIL", name, "" if ok else "got %.90r want %.90r" % (got, want)))
    if not ok: failures.append(name)

class Ctl:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port)); self.s.settimeout(2)
    def __call__(self, cmd, settle=0.6):
        self.s.sendall(cmd.encode() + b"\n")
        time.sleep(settle)
        try: return self.s.recv(65536).decode()
        except socket.timeout: return ""

def shows(st, monitor):
    for line in st.splitlines():
        if line.startswith("monitor %s " % monitor) or line.startswith("monitor %s\t" % monitor):
            return line.split("shows=")[1].strip()
    return None

def device_ties(st, name):
    for line in st.splitlines():
        if line.split()[:2] == ["device", name]:
            return [t for t in line.split()[4:] if "*" in t]
    return None

def fake(name):
    link, fifo = TMP + "/" + name, TMP + "/" + name + ".fifo"
    procs.append(subprocess.Popen([sys.executable, ROOT + "/tests/fake_extron.py", link, fifo],
                                  stdout=open(TMP + "/" + name + ".log", "w"), stderr=subprocess.STDOUT))
    return link, fifo

def start_hub(conf):
    log = open(conf + ".log", "w")
    p = subprocess.Popen([ROOT + "/hub/retrokm-hub", "-v", "-c", conf], stdout=log, stderr=subprocess.STDOUT)
    procs.append(p)
    time.sleep(0.8)
    return p

links = {n: fake(n) for n in ("mps", "mvx", "mtx")}
time.sleep(0.4)
conf = TMP + "/retrokm.conf"
open(conf, "w").write("""[hub]
port = %d
control_port = %d
device = none
keyboard_lights = no

[device mps]            # composite / S-Video in front of the big switcher
port = serial:%s
inputs = 4
outputs = 1
poll = 0
[device mvx]
port = %s
poll = 0
[device mtx]            # the HDMI matrix, after the scaler
port = %s
poll = 0
[device dead]           # never connected
port = %s/nothing-here

[wire]
mps.out1 = mvx.in6
mvx.out4 = mtx.in4
mvx.out5 = mtx.in5

[screen pc]
video = mvx.in1
[screen ps2]
agent = no
video = mvx.in2
[screen dc]
agent = no
video = mvx.in3
[screen comp]
agent = no
video = mps.in1
[screen sv]
agent = no
video = mps.in3
[screen ghost]
agent = no
video = dead.in1

[monitor crt]
pos = 0,0
video = mvx.out2
[monitor cap]
pos = 1,0
video = mtx.out1
capture = yes
[monitor tv]
pos = 2,0
video = mtx.out2
[monitor cap2]
pos = 3,0
video = mtx.out3
capture = yes
[monitor gone]
pos = 4,0
video = dead.out1
""" % (PORT, CTL, links["mps"][0], links["mvx"][0], links["mtx"][0], TMP))

try:
    start_hub(conf)
    ctl = Ctl(CTL)
    st = ctl("status", 0.3)
    check("ports in the status", ("video=mps.in3" in st, "video=mtx.out1" in st), (True, True))
    dv = ctl("devices", 0.3)
    check("devices and wires listed", ("wire mps.out1 = mvx.in6" in dv, "device dead" in dv and "offline" in dv),
          (True, True))

    # one hop through a wire: mps 3*1, then mvx 6*2
    check("show through two switchers", ctl("show sv crt").strip(), "ok")
    st = ctl("status", 0.3); dv = ctl("devices", 0.3)
    check("... the monitor shows it", shows(st, "crt"), "sv")
    check("... ties on both", (device_ties(dv, "mps"), "6*2" in device_ties(dv, "mvx")), (["3*1"], True))

    ctl("show ps2 cap")
    st = ctl("status", 0.3)
    check("show through a wire into the matrix", shows(st, "cap"), "ps2")

    # dc to the tv: mvx out4 carries ps2 to cap, so out5 (free) is the way
    ctl("show dc tv")
    st = ctl("status", 0.3); dv = ctl("devices", 0.3)
    check("a free path is taken", (shows(st, "tv"), shows(st, "cap")), ("dc", "ps2"))
    check("... over the second wire", ("3*5" in device_ties(dv, "mvx"), "5*2" in device_ties(dv, "mtx")),
          (True, True))

    # both wires busy: cap2 takes one over, and the route still happens
    check("both wires busy: still routed", ctl("show pc cap2").strip(), "ok")
    st = ctl("status", 0.3)
    check("... the new monitor shows it", shows(st, "cap2"), "pc")
    check("... one other gave way", sorted([shows(st, "cap"), shows(st, "tv")]).count("pc"), 1)

    # the same machine twice shares one way, nothing taken
    ctl("show ps2 cap"); ctl("show dc tv")
    ctl("show ps2 cap2")
    st = ctl("status", 0.3)
    check("same machine on two monitors shares a wire",
          (shows(st, "cap"), shows(st, "tv"), shows(st, "cap2")), ("ps2", "dc", "ps2"))

    # front panel on the first switcher: crt now gets whatever is on mps in1
    open(links["mps"][1], "w").write("1 1\n")
    time.sleep(0.6)
    st = ctl("status", 0.3)
    check("a front-panel change upstream follows the wire", shows(st, "crt"), "comp")

    check("an offline device is an error", "dead is not connected" in ctl("show ghost gone"), True)
    check("no way through is an error", "no way" in ctl("show ghost crt"), True)
    check("a fixed or unknown monitor is an error", ctl("show sv nowhere").startswith("error"), True)
    ctl("tie mvx 0 2")
    check("tie by device and hand", shows(ctl("status", 0.3), "crt"), "-")

    # ---- the old form: [extron], input =, output = -----------------------
    for p in procs[-1:]: p.terminate()
    time.sleep(0.3)
    old = TMP + "/old.conf"
    open(old, "w").write("""[hub]
port = %d
control_port = %d
device = none
keyboard_lights = no
[extron]
device = %s
poll = 0
[screen pc]
input = 1
[screen octane]
agent = no
input = 12
[monitor left]
pos = 0,0
output = 2
""" % (PORT, CTL, links["mvx"][0]))
    start_hub(old)
    ctl = Ctl(CTL)
    st = ctl("status", 0.3)
    check("old form: ports on the device 'extron'", ("video=extron.in12" in st, "video=extron.out2" in st),
          (True, True))
    check("old form: show still works", ctl("show octane left").strip(), "ok")
    check("old form: ... and the monitor shows it", shows(ctl("status", 0.3), "left"), "octane")
    check("old form: plain 'tie in out' is the switcher", (ctl("tie 1 2").strip(), shows(ctl("status", 0.3), "left")),
          ("ok", "pc"))
except Exception:
    import traceback
    traceback.print_exc()
    failures.append("the test crashed")
finally:
    for p in procs: p.terminate()
    print("\nlogs in", TMP)
    print("%d failure(s)" % len(failures))
    sys.exit(1 if failures else 0)

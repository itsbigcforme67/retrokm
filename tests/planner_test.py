#!/usr/bin/env python3
"""Configs made by the setup planner (planner/) load in the hub: each
example plan, with and without the suggested hardware, goes through
planner/make-config.mjs and then hub/retrokm-hub -c; the hub must start
and list the screens, monitors, devices and wires the config has.  The
switchers' ports are pointed at files that do not exist, so they stay
offline.  Needs node and a built hub/retrokm-hub."""
import os, re, shutil, socket, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = tempfile.mkdtemp(prefix="rkm-planner-")
PORT, CTL = 24980, 24981
failures = []

def check(name, ok, detail=""):
    print("%s %-62s %s" % ("PASS" if ok else "FAIL", name, "" if ok else detail[:300]))
    if not ok: failures.append(name)

def ctl(cmd):
    s = socket.create_connection(("127.0.0.1", CTL), timeout=2)
    s.sendall(cmd.encode() + b"\n")
    time.sleep(0.3)
    data = b""
    s.settimeout(0.3)
    try:
        while True:
            d = s.recv(65536)
            if not d: break
            data += d
    except socket.timeout:
        pass
    s.close()
    return data.decode()

def for_test(text):
    """The generated config, made safe to run here: no keyboards, no
    uinput, our ports, and switcher ports that lead nowhere."""
    text = re.sub(r"(?m)^device = auto.*$", "device = none\nport = %d\ncontrol_port = %d\nkeyboard_lights = no\npad = none"
                  % (PORT, CTL), text, count=1)
    text = re.sub(r"(?m)^local = yes.*$", "", text)
    n = [0]
    def nowhere(m):
        n[0] += 1
        return "port = %s/nowhere%d" % (TMP, n[0])
    return re.sub(r"(?m)^port = (/dev/|tcp:).*$", nowhere, text)

def sections(text, kind):
    return re.findall(r"(?m)^\[%s ([^\]]+)\]" % kind, text)

if not shutil.which("node"):
    print("SKIP planner_test: node is not installed")
    sys.exit(0)

for plan in sorted(os.listdir(ROOT + "/planner/examples")):
    for owned in (False, True):
        tag = "%s%s" % (plan, " (owned only)" if owned else "")
        args = ["node", ROOT + "/planner/make-config.mjs", ROOT + "/planner/examples/" + plan] + (["--owned"] if owned else [])
        made = subprocess.run(args, capture_output=True, text=True)
        check(tag + ": the planner makes a config", made.returncode == 0 and "[screen " in made.stdout, made.stderr)
        if made.returncode: continue
        conf = os.path.join(TMP, plan.replace(".json", "") + ("-owned" if owned else "") + ".conf")
        open(conf, "w").write(for_test(made.stdout))
        log = open(conf + ".log", "w")
        hub = subprocess.Popen([ROOT + "/hub/retrokm-hub", "-c", conf], stdout=log, stderr=subprocess.STDOUT)
        time.sleep(0.8)
        try:
            running = hub.poll() is None
            check(tag + ": the hub loads it", running, open(conf + ".log").read())
            if not running: continue
            st, dv = ctl("status"), ctl("devices")
            for s in sections(made.stdout, "screen"):
                check(tag + ": screen " + s, re.search(r"(?m)^screen %s\s" % re.escape(s), st) is not None, st)
            for m in sections(made.stdout, "monitor"):
                check(tag + ": monitor " + m, re.search(r"(?m)^monitor %s\s" % re.escape(m), st) is not None, st)
            for d in sections(made.stdout, "device"):
                check(tag + ": device " + d, re.search(r"(?m)^device %s\s" % re.escape(d), dv) is not None, dv)
            wire = re.search(r"(?ms)^\[wire\]\n(.*?)^\s*$", made.stdout)
            for w in re.findall(r"(?m)^(\S+ = \S+)", wire.group(1)) if wire else []:
                check(tag + ": wire " + w, ("wire " + w) in dv, dv)
            check(tag + ": no complaints in the log", "cannot parse" not in open(conf + ".log").read(), open(conf + ".log").read())
        finally:
            hub.terminate()
            hub.wait()
        time.sleep(0.2)

print("\n%d failed" % len(failures) if failures else "\nall passed")
sys.exit(1 if failures else 0)

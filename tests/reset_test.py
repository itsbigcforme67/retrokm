#!/usr/bin/env python3
"""The reset command: the hub starts over in place (same process id, so
systemd keeps it), lets the agent go, takes it back when it reconnects, and
keeps the state saved from the panel.  Run after building hub/retrokm-hub."""
import os, socket, struct, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = tempfile.mkdtemp(prefix="rkm-reset-")
PORT, CTL, PANEL = 24970, 24971, 24972
failures = []

def check(name, got, want):
    ok = got == want
    print("%s %-58s %s" % ("PASS" if ok else "FAIL", name, "" if ok else "got %.90r want %.90r" % (got, want)))
    if not ok: failures.append(name)

def ctl(cmd):
    s = socket.create_connection(("127.0.0.1", CTL)); s.settimeout(1)
    s.sendall(cmd.encode() + b"\n"); time.sleep(0.2)
    try: return s.recv(65536).decode()
    except socket.timeout: return ""
    finally: s.close()

def agent(name):
    s = socket.create_connection(("127.0.0.1", PORT)); s.settimeout(0.3)
    p = bytes([1, 1, 0, 0]) + struct.pack(">HHH", 640, 480, 0) + name.encode()
    s.sendall(bytes([1]) + struct.pack(">H", len(p)) + p)
    return s

def closed(s, wait=2.0):
    end = time.time() + wait
    while time.time() < end:
        try:
            if s.recv(65536) == b"": return True
        except socket.timeout: pass
        except OSError: return True
    return False

conf = TMP + "/hub.conf"
open(conf, "w").write("""
[hub]
port = %d
control_port = %d
panel_port = %d
device = none
pad = none

[screen pc]
[screen dell]

[monitor left]
pos = 0,0
screen = pc
[monitor right]
pos = 1,0
screen = dell
""" % (PORT, CTL, PANEL))
log = open(TMP + "/hub.log", "w")
hub = subprocess.Popen([ROOT + "/hub/retrokm-hub", "-v", "-c", conf], stdout=log, stderr=subprocess.STDOUT)
try:
    time.sleep(0.4)
    a = agent("pc"); time.sleep(0.3)
    check("agent is ready", "pc " in ctl("status") and "ready" in ctl("status"), True)
    # a monitor move from the panel, saved to the state file
    p = socket.create_connection(("127.0.0.1", PANEL)); p.sendall(b"move left 1 0\n"); time.sleep(0.2); p.close()

    check("reset answers ok", ctl("reset").strip(), "ok")
    check("the agent is let go", closed(a), True)
    time.sleep(0.5)
    check("same process (systemd keeps it)", hub.poll(), None)
    st = ctl("status")
    check("control socket is back", st.startswith("active"), True)
    check("monitor move survived", [l.split()[2] for l in st.splitlines() if l.startswith("monitor left")], ["pos=1,0"])
    a = agent("pc"); time.sleep(0.3)
    check("agent comes back", "ready" in ctl("status"), True)

    # the panel can ask for it too
    p = socket.create_connection(("127.0.0.1", PANEL)); p.sendall(b"reset\n"); time.sleep(0.2); p.close()
    check("panel reset lets the agent go", closed(a), True)
    time.sleep(0.5)
    check("hub back after a panel reset", ctl("status").startswith("active"), True)
except Exception:
    import traceback; traceback.print_exc(); failures.append("exception")
finally:
    hub.terminate(); hub.wait()
    log.close()
    if failures: print(open(TMP + "/hub.log").read())

print("%d failure(s)" % len(failures) if failures else "all passed")
sys.exit(1 if failures else 0)

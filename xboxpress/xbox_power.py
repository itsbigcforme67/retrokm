#!/usr/bin/env python3
"""Turns the Xbox on or off with the presser's servo, and checks it worked.

    ./xbox_power.py state      on / off, from the network and the pad's USB
    ./xbox_power.py on         press only if it is off, then wait until it is on
    ./xbox_power.py off        press only if it is on, then wait until it is off

A press can miss, so it checks after each one and retries, each a little longer.

The Xbox is on when the Xbox pad (xboxpad/, powered by the console's USB) answers and the
console polls it (its "reports" count climbs, ~125 a second), or the Xbox's MAC answers on
the LAN (the address changes). Off: neither. The capture card can't tell: it keeps the last
frame. The presser (xboxpress/, on its own power) does the pressing.
Never power it off while a game is saving: this tool doesn't know what's on screen.
"""
import os
import re
import socket
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

XBOX_MAC = "00:0d:3a:31:08:ab"
PAD = ("xboxpad.local", 24854)
PRESSER = ("xboxpress.local", 24854)
SUBNET = "192.168.1."
KNOWN = [115, 116, 118, 165]  # addresses it has had; tried first
PRESS_MS = int(os.environ.get("PRESS_MS", "410"))  # first try; 380 clicked 10/10 on 29 Oct (battery 3.3-3.5 V)
STEP_MS = 30   # each retry presses this much longer: a press can miss (tape, battery)
TRIES = 4


_addr = {}


def udp(dev, cmd, wait=2.0):
    """A command to a board; "" if it doesn't answer (the pad is unpowered while the Xbox is off)."""
    if not _addr.get(dev[0]):  # found ones are kept; the pad appears only once the Xbox is on
        try:
            _addr[dev[0]] = socket.gethostbyname(dev[0])
        except OSError:  # a failing .local lookup takes ~5 s
            return ""
    addr = (_addr[dev[0]], dev[1])
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(wait)
    s.sendto(cmd.encode(), addr)
    try:
        return s.recv(4096).decode()
    except socket.timeout:
        return ""


def ping(ip):
    return subprocess.run(["ping", "-c1", "-W1", ip], stdout=subprocess.DEVNULL).returncode == 0


def xbox_ip():
    """The Xbox's address if it answers, else None (its MAC's known neighbour entry, or the
    addresses it has had; no subnet sweep: that took ~25 s every off check)."""
    neigh = subprocess.run(["ip", "neigh"], capture_output=True, text=True).stdout.lower()
    for line in neigh.splitlines():
        f = line.split()
        if XBOX_MAC in f and "failed" not in f and "incomplete" not in f and ping(f[0]):
            return f[0]
    with ThreadPoolExecutor(len(KNOWN)) as ex:
        list(ex.map(ping, [SUBNET + str(a) for a in KNOWN]))
    neigh = subprocess.run(["ip", "neigh"], capture_output=True, text=True).stdout.lower()
    for line in neigh.splitlines():
        f = line.split()
        if XBOX_MAC in f and "failed" not in f and "incomplete" not in f and ping(f[0]):
            return f[0]
    return None


def reports():
    m = re.search(r"reports=(\d+)", udp(PAD, "show", 1.0))
    return int(m.group(1)) if m else None


def state():
    a = reports()
    polling = False
    if a is not None:  # no answer: the pad is unpowered, so the Xbox is off
        time.sleep(1)
        b = reports()
        polling = b is not None and b > a
    ip = xbox_ip()
    return ("on" if polling or ip else "off"), ip, polling


def wait_for(want, seconds):
    end = time.time() + seconds
    while time.time() < end:
        s, ip, usb = state()
        if s == want:
            return s, ip, usb
        time.sleep(2)
    return state()


def main():
    want = sys.argv[1] if len(sys.argv) > 1 else "state"
    s, ip, usb = state()
    if want == "state" or want == s:
        print(f"xbox {s} (network {ip or 'none'}, pad {'polled' if usb else 'not polled'})")
        return 0
    if want not in ("on", "off"):
        print(__doc__)
        return 2
    for attempt in range(TRIES):
        r = udp(PRESSER, "press %d" % (PRESS_MS + attempt * STEP_MS)).strip()
        if not r:
            print("the presser (xboxpress.local) doesn't answer")
            return 1
        print(r)
        s, ip, usb = wait_for(want, 45 if want == "on" else 15)
        if s == want:
            print(f"xbox {s} (network {ip or 'none'}, pad {'polled' if usb else 'not polled'})")
            return 0
        print(f"still {s} after press {attempt + 1}")
    return 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Checks the Xbox pad end to end on the laptop, no Xbox needed.

Plug the Atom S3R (xbox mode) into the laptop: Linux's xpad driver takes it
as an original Xbox pad.  This script then plays the pad through the hub's
control socket ("pad ..."), exactly the path the real Elite takes, and
reads what arrives at the kernel's input device; then it sends a rumble
effect and waits for the pad to report it in its log (UDP 24853).

    xboxpad/xpadtest.py [screen]      (default screen: xbox)

The keyboard goes to that screen for a few seconds and comes back.
"""
import fcntl, glob, os, select, socket, struct, sys, time

SCREEN = sys.argv[1] if len(sys.argv) > 1 else "xbox"
EV_SYN, EV_KEY, EV_ABS, EV_FF = 0, 1, 3, 0x15
EVENT = struct.Struct("llHHi")
# RKM_PAD_* bit -> what xpad reports for an original Xbox pad
KEYS = [("a", 0x0001, 0x130), ("b", 0x0002, 0x131), ("x", 0x0004, 0x133), ("y", 0x0008, 0x134),
        ("rb = black", 0x0020, 0x132), ("lb = white", 0x0010, 0x135),
        ("back", 0x0040, 0x13a), ("start", 0x0080, 0x13b), ("ls", 0x0200, 0x13d), ("rs", 0x0400, 0x13e)]
HAT = [("up", 0x0800, 17, -1), ("down", 0x1000, 17, 1), ("left", 0x2000, 16, -1), ("right", 0x4000, 16, 1)]
ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ = 0, 1, 2, 3, 4, 5


def ctl(cmd):
    s = socket.create_connection(("127.0.0.1", 24851), timeout=2)
    s.sendall(cmd.encode() + b"\n")
    time.sleep(0.1)
    try:
        return s.recv(65536).decode()
    finally:
        s.close()


def find_pad():
    for d in sorted(glob.glob("/sys/class/input/event*")):
        try:
            ids = (open(d + "/device/id/vendor").read().strip(), open(d + "/device/id/product").read().strip())
            name = open(d + "/device/name").read().strip()
        except OSError:
            continue
        if ids == ("045e", "0289"):
            return "/dev/input/" + os.path.basename(d), name
    return None, None


def events(fd, seconds):
    out, end = [], time.monotonic() + seconds
    while (left := end - time.monotonic()) > 0:
        if select.select([fd], [], [], left)[0]:
            data = os.read(fd, EVENT.size * 64)
            for i in range(0, len(data) - EVENT.size + 1, EVENT.size):
                _, _, t, c, v = EVENT.unpack_from(data, i)
                if t != EV_SYN:
                    out.append((t, c, v))
    return out


def rumble(fd, strong, weak):
    eff = bytearray(48)  # struct ff_effect on 64-bit Linux
    struct.pack_into("HhHHHHH", eff, 0, 0x50, -1, 0, 0, 0, 500, 0)  # FF_RUMBLE, new, 500 ms
    struct.pack_into("HH", eff, 16, strong, weak)
    fcntl.ioctl(fd, (1 << 30) | (48 << 16) | (ord("E") << 8) | 0x80, eff)  # EVIOCSFF
    os.write(fd, EVENT.pack(0, 0, EV_FF, struct.unpack_from("h", eff, 2)[0], 1))


def main():
    path, name = find_pad()
    if not path:
        sys.exit("no original Xbox pad (045E:0289) on this laptop: is the Atom plugged in, in xbox mode?")
    st = ctl("status")
    line = [l for l in st.splitlines() if l.startswith("screen %s " % SCREEN)]
    if not line or "pad=" not in line[0]:
        sys.exit("the hub has no pad player on screen %r (is [screen %s] in the config, and the pad on WiFi?)"
                 % (SCREEN, SCREEN))
    before = st.split()[1] if st.startswith("active:") else None
    print("pad %s (%s); hub screen %s" % (path, name, SCREEN))
    fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
    log = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    log.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    log.bind(("", 24853))
    fails = 0

    def check(what, cmd, want):
        nonlocal fails
        ctl(cmd)
        got = events(fd, 0.25)
        ok = want in got
        fails += not ok
        print("%-5s %-14s %s" % ("ok" if ok else "FAIL", what, "" if ok else "wanted %s, got %s" % (want, got[:6])))
        ctl("pad 0")
        events(fd, 0.15)

    try:
        ctl("goto " + SCREEN)
        ctl("pad 0")
        events(fd, 0.3)
        for n, bit, code in KEYS:
            check(n, "pad %x" % bit, (EV_KEY, code, 1))
        for n, bit, code, v in HAT:
            check(n, "pad %x" % bit, (EV_ABS, code, v))
        check("lt", "pad 0 200 0", (EV_ABS, ABS_Z, 200))
        check("rt", "pad 0 0 77", (EV_ABS, ABS_RZ, 77))
        check("stick l right", "pad 0 0 0 20000 0", (EV_ABS, ABS_X, 20000))
        # xpad flips Y so down is positive, like other Linux pads
        check("stick l up", "pad 0 0 0 0 20000", (EV_ABS, ABS_Y, -20001))
        check("stick r left", "pad 0 0 0 0 0 -12345 0", (EV_ABS, ABS_RX, -12345))
        check("stick r down", "pad 0 0 0 0 0 0 -30000", (EV_ABS, ABS_RY, 29999))

        rumble(fd, 0xC000, 0x4000)
        got, end = None, time.monotonic() + 2
        while not got and time.monotonic() < end:
            if select.select([log], [], [], 0.2)[0]:
                for l in log.recv(65535).decode(errors="replace").splitlines():
                    if l.startswith("rumble ") and l != "rumble 0 0":
                        got = l
        ok = got == "rumble 49152 16384"
        fails += not ok
        print("%-5s %-14s %s" % ("ok" if ok else "FAIL", "rumble", got or "nothing in the pad's log"))
    finally:
        ctl("pad 0")
        if before and before != SCREEN:
            ctl("goto " + before)
        os.close(fd)
    print("all good" if not fails else "%d failed" % fails)
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()

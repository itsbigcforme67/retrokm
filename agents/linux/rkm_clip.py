#!/usr/bin/env python3
"""RetroKM clipboard helper for modern Linux desktops (clipboard only).

Run it inside your desktop session, with the same screen name the hub (or
rkm-uinput) uses for this machine.  It shells out to a copy and a paste
command: wl-copy / wl-paste under Wayland, xclip under X11, or your own.

    rkm_clip.py [-n NAME] [-p PORT] [--copy CMD] [--paste CMD] HUB
"""
import argparse, os, shlex, shutil, socket, struct, subprocess, sys, time

HELLO, WELCOME, PING, PONG, LEAVE = 1, 2, 3, 4, 0x11
CLIP_BEGIN, CLIP_DATA, CLIP_END = 0x20, 0x21, 0x22
CAP_CLIP, CHUNK, MAX_KB = 2, 480, 4096

def default_cmds():
    if os.environ.get("WAYLAND_DISPLAY") and shutil.which("wl-copy"):
        return "wl-copy", "wl-paste --no-newline"
    if shutil.which("xclip"):
        return "xclip -selection clipboard -in", "xclip -selection clipboard -out"
    sys.exit("rkm_clip: install wl-clipboard or xclip, or pass --copy and --paste")

def frame(t, payload=b""):
    return bytes([t]) + struct.pack(">H", len(payload)) + payload

def session(a):
    s = socket.create_connection((a.hub, a.port), timeout=10)
    s.settimeout(30)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    name = a.name.encode()[:31]
    s.sendall(frame(HELLO, bytes([1, CAP_CLIP, 0, 0]) + struct.pack(">HHH", 0, 0, MAX_KB) + name))
    last = paste(a)                              # whatever is there now is not news
    buf, incoming = b"", None
    while True:
        d = s.recv(65536)
        if not d:
            return
        buf += d
        while len(buf) >= 3:
            n = struct.unpack(">H", buf[1:3])[0]
            if len(buf) < 3 + n:
                break
            t, p, buf = buf[0], buf[3:3 + n], buf[3 + n:]
            if t == WELCOME and p[1:2] != b"\x00":
                sys.exit("rkm_clip: hub refused us (unknown screen name or version)")
            elif t == PING:
                s.sendall(frame(PONG))
            elif t == LEAVE:                     # pointer left this machine: report changes
                cur = paste(a)
                if cur and cur != last:
                    last = cur
                    out = frame(CLIP_BEGIN, b"\x01" + struct.pack(">I", len(cur)))
                    for i in range(0, len(cur), CHUNK):
                        out += frame(CLIP_DATA, cur[i:i + CHUNK])
                    s.sendall(out + frame(CLIP_END))
            elif t == CLIP_BEGIN:
                incoming = b""
            elif t == CLIP_DATA and incoming is not None:
                incoming += p
            elif t == CLIP_END and incoming is not None:
                last, incoming = incoming, None
                subprocess.run(shlex.split(a.copy), input=last, check=False)

def paste(a):
    try:
        r = subprocess.run(shlex.split(a.paste), capture_output=True, timeout=5)
        return r.stdout if r.returncode == 0 else b""
    except (OSError, subprocess.TimeoutExpired):
        return b""

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("hub")
    ap.add_argument("-n", "--name", default=socket.gethostname().split(".")[0])
    ap.add_argument("-p", "--port", type=int, default=24850)
    ap.add_argument("--copy")
    ap.add_argument("--paste")
    a = ap.parse_args()
    if not (a.copy and a.paste):
        a.copy, a.paste = default_cmds()
    while True:
        try:
            session(a)
        except OSError:
            pass
        time.sleep(3)

if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""RetroKM on a phone: a web page the hub machine serves on the LAN.

  python3 phone/rkm_phone.py [--port 24855]

Open http://<this machine>:24855/?k=<key> on the phone (the key is in
~/.config/retrokm/phone-key, made on first run, and the start-up log prints
the whole link).  On an iPhone, Share > Add to Home Screen makes it an app.

The page gets the desk as the touch panel does (the hub's panel port) and
can do what the panel does: focus a machine, put machines on monitors, flip
a shared monitor's input, lock edges, reset.  It is also a trackpad and
keyboard, through the hub's control port (127.0.0.1 only, so this server
must run on the hub machine).  One WebSocket carries it all.

Standard library only.
"""
import argparse
import base64
import hashlib
import io
import json
import os
import secrets
import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
KEY_FILE = os.path.expanduser("~/.config/retrokm/phone-key")

# what the page may send the hub, by first word
PANEL_CMDS = {"goto", "tie", "untie", "share", "lock", "reset", "plug", "unplug",
              "link", "unlink", "keep", "keepall", "layout"}
INPUT_CMDS = {"rel", "btn", "wheel", "key"}

# typing text: US layout, as tools/rkm-type.py
PLAIN = {" ": "space", "\t": "tab", "\n": "enter", "-": "minus", "=": "equal",
         "[": "leftbrace", "]": "rightbrace", "\\": "backslash", ";": "semicolon",
         "'": "apostrophe", "`": "grave", ",": "comma", ".": "dot", "/": "slash"}
SHIFTED = {"!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7",
           "*": "8", "(": "9", ")": "0", "_": "minus", "+": "equal", "{": "leftbrace",
           "}": "rightbrace", "|": "backslash", ":": "semicolon", '"': "apostrophe",
           "~": "grave", "<": "comma", ">": "dot", "?": "slash"}
# what phones type instead of plain ASCII
SMART = {"‘": "'", "’": "'", "“": '"', "”": '"', "—": "-",
         "–": "-", "…": "...", " ": " "}


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def char_keys(c):
    """The key presses for one character: [(key, shift)], or [] if none."""
    c = SMART.get(c, c)
    if len(c) > 1:
        return [k for ch in c for k in char_keys(ch)]
    if c.isascii() and c.isalpha():
        return [(c.lower(), c.isupper())]
    if c.isascii() and c.isdigit():
        return [(c, False)]
    if c in PLAIN:
        return [(PLAIN[c], False)]
    if c in SHIFTED:
        return [(SHIFTED[c], True)]
    return []


class HubLine:
    """A line connection to one of the hub's ports that comes back by itself."""

    def __init__(self, port, on_line=None, ping=None):
        self.port, self.on_line, self.ping = port, on_line, ping
        self.sock = None
        self.lock = threading.Lock()
        threading.Thread(target=self._run, daemon=True).start()

    def send(self, line):
        with self.lock:
            if not self.sock:
                return False
            try:
                self.sock.sendall((line + "\n").encode())
                return True
            except OSError:
                self._drop()
                return False

    def _drop(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
        self.sock = None

    def _run(self):
        while True:
            try:
                s = socket.create_connection(("127.0.0.1", self.port), timeout=3)
            except OSError:
                time.sleep(2)
                continue
            s.settimeout(1)
            with self.lock:
                self.sock = s
            log("hub port %d: connected" % self.port)
            if self.ping:
                self.send(self.ping[0])        # e.g. "layout": the first one now
            buf, last_ping = b"", time.time()
            while self.sock is s:
                try:
                    d = s.recv(65536)
                    if not d:
                        break
                    buf += d
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        if self.on_line:
                            self.on_line(line.decode("utf-8", "replace"))
                except socket.timeout:
                    pass
                except OSError:
                    break
                if self.ping and time.time() - last_ping > 3:   # the hub drops quiet panels
                    self.send(self.ping[1])
                    last_ping = time.time()
            with self.lock:
                if self.sock is s:
                    self._drop()
            log("hub port %d: lost, retrying" % self.port)
            time.sleep(1)


class Desk:
    """The latest layout, and the phones to tell when it changes."""

    def __init__(self, panel_port, ctl_port):
        self.layout = None
        self.clients = set()
        self.lock = threading.Lock()
        self.panel = HubLine(panel_port, self._panel_line, ping=("layout", "ping"))
        self.ctl = HubLine(ctl_port)

    def _panel_line(self, line):
        if line.startswith("layout "):
            self.layout = line[7:]
            self.broadcast('{"layout":%s}' % self.layout)
        elif line.startswith("error"):
            self.broadcast(json.dumps({"error": line[5:].strip()}))

    def broadcast(self, text):
        with self.lock:
            clients = list(self.clients)
        for c in clients:
            c.send_text(text)

    def layout_obj(self):
        try:
            return json.loads(self.layout) if self.layout else None
        except ValueError:
            return None

    def hub_up(self):
        return self.panel.sock is not None

    def command(self, line):
        """One line from a phone.  Returns an error string or None."""
        words = line.split()
        if not words:
            return None
        w = words[0].lower()
        if w == "text":
            return self.type_text(line[5:])
        if w == "combo":                       # combo ctrl+alt+delete
            return self.combo(words[1] if len(words) > 1 else "")
        if w in INPUT_CMDS:
            if not all(x.lstrip("-").isalnum() for x in words[1:]):
                return "bad input"
            return None if self.ctl.send(" ".join(words)) else "hub not connected"
        if w in PANEL_CMDS:
            return None if self.panel.send(" ".join(words)) else "hub not connected"
        return "unknown command " + w

    def combo(self, s):
        names = {"ctrl": "leftctrl", "shift": "leftshift", "alt": "leftalt", "win": "leftmeta"}
        parts = [names.get(p, p) for p in s.lower().split("+") if p]
        if not parts or not all(p.isalnum() for p in parts):
            return "bad combo"
        for p in parts:
            self.ctl.send("key %s 1" % p)
        for p in reversed(parts):
            self.ctl.send("key %s 0" % p)
        return None

    def type_text(self, text):
        for c in text[:2000]:
            for k, shift in char_keys(c):
                if shift:
                    self.ctl.send("key leftshift 1")
                self.ctl.send("key %s 1" % k)
                self.ctl.send("key %s 0" % k)
                if shift:
                    self.ctl.send("key leftshift 0")
                time.sleep(0.008)
        return None


class WebSocket:
    """Just enough RFC 6455: text frames, ping, close."""

    def __init__(self, rfile, wfile):
        self.rfile, self.wfile = rfile, wfile
        self.wlock = threading.Lock()
        self.open = True

    def send_text(self, text):
        data = text.encode()
        n = len(data)
        head = bytes([0x81]) + (bytes([n]) if n < 126 else
                                bytes([126]) + struct.pack(">H", n) if n < 65536 else
                                bytes([127]) + struct.pack(">Q", n))
        with self.wlock:
            if not self.open:
                return
            try:
                self.wfile.write(head + data)
                self.wfile.flush()
            except OSError:
                self.open = False

    def send_bytes(self, data):
        n = len(data)
        head = bytes([0x82]) + (bytes([n]) if n < 126 else
                                bytes([126]) + struct.pack(">H", n) if n < 65536 else
                                bytes([127]) + struct.pack(">Q", n))
        with self.wlock:
            if not self.open:
                return
            try:
                self.wfile.write(head + data)
                self.wfile.flush()
            except OSError:
                self.open = False

    def recv(self):
        """The next text message, or None when closed."""
        msg = b""
        while True:
            h = self.rfile.read(2)
            if len(h) < 2:
                return None
            op, n = h[0] & 0x0F, h[1] & 0x7F
            if n == 126:
                n = struct.unpack(">H", self.rfile.read(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self.rfile.read(8))[0]
            if n > 1 << 20:
                return None
            mask = self.rfile.read(4) if h[1] & 0x80 else b"\0\0\0\0"
            data = bytearray(self.rfile.read(n))
            for i in range(len(data)):
                data[i] ^= mask[i & 3]
            if op == 8:
                return None
            if op == 9:                        # ping -> pong
                with self.wlock:
                    self.wfile.write(bytes([0x8A, len(data)]) + bytes(data[:125]))
                    self.wfile.flush()
                continue
            if op in (0, 1, 2):
                msg += bytes(data)
                if h[0] & 0x80:
                    return msg.decode("utf-8", "replace")


class LaptopScreen:
    """The hub machine's own desktop, for the screen view: JPEG frames of
    one monitor at a time, with the pointer drawn in (X grabs leave it out),
    and pointer warps that keep the hub in step."""

    def __init__(self, desk, display):
        self.desk, self.display = desk, display
        self.lock = threading.Lock()
        self.mons, self.mons_at = [], 0
        self.x11 = self.xtst = None
        try:
            import ctypes
            x = ctypes.cdll.LoadLibrary("libX11.so.6")
            x.XOpenDisplay.restype = ctypes.c_void_p
            d = x.XOpenDisplay(display.encode())
            if d:
                self.x11 = (ctypes, x, d, x.XDefaultRootWindow(ctypes.c_void_p(d)))
                self.xtst = ctypes.cdll.LoadLibrary("libXtst.so.6")
        except OSError:
            pass

    def monitors(self):
        """[{name, x, y, w, h}] from xrandr, left to right."""
        if time.time() - self.mons_at > 3:
            import subprocess
            mons = []
            try:
                out = subprocess.run(["xrandr", "--listmonitors"], env=dict(os.environ, DISPLAY=self.display),
                                     capture_output=True, text=True, timeout=3).stdout
                for line in out.splitlines()[1:]:
                    # " 0: +*HDMI-1-1 1920/527x1080/297+1920+0  HDMI-1-1"
                    f = line.split()
                    g = f[2].replace("/", " ").replace("x", " ").replace("+", " ").split()
                    mons.append({"name": f[-1], "w": int(g[0]), "h": int(g[2]), "x": int(g[4]), "y": int(g[5])})
            except (OSError, ValueError, IndexError, subprocess.SubprocessError):
                pass
            self.mons = sorted(mons, key=lambda m: (m["x"], m["y"]))
            self.mons_at = time.time()
        return self.mons

    def pointer(self):
        if not self.x11:
            return None
        ct, x, d, root = self.x11
        a = [ct.c_ulong(), ct.c_ulong(), ct.c_int(), ct.c_int(), ct.c_int(), ct.c_int(), ct.c_uint()]
        with self.lock:
            x.XQueryPointer(ct.c_void_p(d), ct.c_ulong(root), *[ct.byref(v) for v in a])
        return a[2].value, a[3].value

    def frame(self, i, maxw, quality):
        from PIL import Image, ImageDraw, ImageGrab
        mons = self.monitors()
        if not mons:
            return None
        m = mons[max(0, min(i, len(mons) - 1))]
        im = ImageGrab.grab(bbox=(m["x"], m["y"], m["x"] + m["w"], m["y"] + m["h"]), xdisplay=self.display)
        k = min(1.0, maxw / m["w"])
        if k < 1:
            im = im.resize((int(m["w"] * k), int(m["h"] * k)), Image.BILINEAR)
        p = self.pointer()
        if p and m["x"] <= p[0] < m["x"] + m["w"] and m["y"] <= p[1] < m["y"] + m["h"]:
            cx, cy, z = (p[0] - m["x"]) * k, (p[1] - m["y"]) * k, max(k, im.width / 900)   # big enough on a phone
            arrow = [(0, 0), (0, 17), (4, 13), (7, 20), (10, 19), (7, 12), (12, 12)]
            ImageDraw.Draw(im).polygon([(cx + ax * z * 1.4, cy + ay * z * 1.4) for ax, ay in arrow],
                                       fill="white", outline="black")
        out = io.BytesIO()
        im.convert("RGB").save(out, "JPEG", quality=quality)
        return out.getvalue()

    def warp(self, x, y):
        """Put the laptop's pointer at x,y (desktop pixels) and give it control."""
        lay = self.desk.layout_obj()
        own = next((m["fixed"] for m in (lay or {}).get("monitors", []) if m.get("fixed")), None)
        if own and lay.get("active") != own:
            self.desk.ctl.send("goto " + own)
        self.desk.ctl.send("warp %d %d" % (x, y))
        if self.xtst:                          # a hub without warp: move it here too
            ct, x11, d, root = self.x11
            with self.lock:
                self.xtst.XTestFakeMotionEvent(ct.c_void_p(d), 0, int(x), int(y), ct.c_ulong(0))
                x11.XFlush(ct.c_void_p(d))


def make_handler(desk, key, screen):
    files = {"/": ("index.html", "text/html; charset=utf-8"),
             "/icon.png": ("icon.png", "image/png")}

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *a):
            pass

        def _keyed(self, q):
            return secrets.compare_digest(q.get("k", [""])[0], key)

        def _send(self, code, body, ctype="text/plain; charset=utf-8", cache=False):
            if isinstance(body, str):
                body = body.encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "max-age=3600" if cache else "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            u = urlparse(self.path)
            q = parse_qs(u.query)
            if u.path in files:
                name, ctype = files[u.path]
                with open(os.path.join(HERE, name), "rb") as f:
                    return self._send(200, f.read(), ctype, cache=name.endswith(".png"))
            if u.path == "/manifest.json":           # the home-screen app starts with the key
                k = q.get("k", [""])[0]
                return self._send(200, json.dumps({
                    "name": "RetroKM", "short_name": "RetroKM", "display": "standalone",
                    "start_url": "/?k=" + k if self._keyed(q) else "/",
                    "background_color": "#0e1116", "theme_color": "#0e1116",
                    "icons": [{"src": "/icon.png", "sizes": "180x180", "type": "image/png"}]}),
                    "application/manifest+json")
            if u.path == "/ws":
                if not self._keyed(q):
                    return self._send(403, "wrong key")
                return self._websocket()
            if u.path == "/screen":
                if not self._keyed(q):
                    return self._send(403, "wrong key")
                return self._screen()
            self._send(404, "not here")

        def _upgrade(self):
            wkey = self.headers.get("Sec-WebSocket-Key", "")
            accept = base64.b64encode(hashlib.sha1(
                (wkey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
            self.send_response(101)
            self.send_header("Upgrade", "websocket")
            self.send_header("Connection", "Upgrade")
            self.send_header("Sec-WebSocket-Accept", accept)
            self.end_headers()
            self.wfile.flush()
            self.close_connection = True
            return WebSocket(self.rfile, self.wfile)

        def _screen(self):
            """The screen view: the page asks for each frame when it has
            drawn the last, so a slow phone or Wi-Fi just gets fewer."""
            ws = self._upgrade()
            last = None
            ws.send_text(json.dumps({"mons": screen.monitors()}))
            try:
                while True:
                    m = ws.recv()
                    if m is None:
                        break
                    for line in m.split("\n"):
                        w = line.split()
                        if not w:
                            continue
                        if w[0] == "frame" and len(w) == 4:      # frame <monitor> <max width> <quality>
                            try:
                                f = screen.frame(int(w[1]), max(320, min(int(w[2]), 3840)),
                                                 max(20, min(int(w[3]), 90)))
                            except Exception as e:                 # X went away, a bad grab
                                log("screen grab failed:", e)
                                f = None
                            if f is None:
                                ws.send_text(json.dumps({"error": "cannot see the screen"}))
                            elif f == last:
                                ws.send_text('{"same":1}')
                            else:
                                ws.send_bytes(f)
                                last = f
                        elif w[0] == "mons":
                            ws.send_text(json.dumps({"mons": screen.monitors()}))
                        elif w[0] == "warp" and len(w) == 3 and w[1].isdigit() and w[2].isdigit():
                            screen.warp(int(w[1]), int(w[2]))
                        else:
                            err = desk.command(line)
                            if err:
                                ws.send_text(json.dumps({"error": err}))
            except OSError:
                pass
            finally:
                ws.open = False

        def _websocket(self):
            wkey = self.headers.get("Sec-WebSocket-Key", "")
            accept = base64.b64encode(hashlib.sha1(
                (wkey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
            self.send_response(101)
            self.send_header("Upgrade", "websocket")
            self.send_header("Connection", "Upgrade")
            self.send_header("Sec-WebSocket-Accept", accept)
            self.end_headers()
            self.wfile.flush()
            self.close_connection = True
            ws = WebSocket(self.rfile, self.wfile)
            with desk.lock:
                desk.clients.add(ws)
            log("phone connected from", self.client_address[0])
            ws.send_text(json.dumps({"hub": desk.hub_up()}))
            if desk.layout:
                ws.send_text('{"layout":%s}' % desk.layout)
            try:
                while True:
                    m = ws.recv()
                    if m is None:
                        break
                    for line in m.split("\n"):
                        err = desk.command(line)
                        if err:
                            ws.send_text(json.dumps({"error": err}))
            except OSError:
                pass
            finally:
                ws.open = False
                with desk.lock:
                    desk.clients.discard(ws)
                log("phone gone", self.client_address[0])

    return Handler


def phone_key():
    try:
        with open(KEY_FILE) as f:
            k = f.read().strip()
            if k:
                return k
    except OSError:
        pass
    k = secrets.token_urlsafe(9)
    os.makedirs(os.path.dirname(KEY_FILE), exist_ok=True)
    fd = os.open(KEY_FILE, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write(k + "\n")
    return k


def lan_address():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))            # no packet is sent; picks the LAN interface
        return s.getsockname()[0]
    except OSError:
        return "this-machine"
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, default=24855)
    ap.add_argument("--panel-port", type=int, default=24852)
    ap.add_argument("--control-port", type=int, default=24851)
    ap.add_argument("--display", default=os.environ.get("DISPLAY", ":0"), help="this machine's X display")
    a = ap.parse_args()
    key = phone_key()
    desk = Desk(a.panel_port, a.control_port)
    screen = LaptopScreen(desk, a.display)
    srv = ThreadingHTTPServer(("0.0.0.0", a.port), make_handler(desk, key, screen))
    srv.daemon_threads = True
    log("phone page: http://%s:%d/?k=%s" % (lan_address(), a.port, key))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())

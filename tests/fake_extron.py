#!/usr/bin/env python3
"""Simulated Extron matrix switcher on a pseudo-terminal, speaking SIS.

usage: fake_extron.py LINK [FIFO]
  LINK  symlink created for the serial device (point the hub's [extron] device at it)
  FIFO  optional named pipe: write "IN OUT" lines to simulate front-panel button presses
"""
import os, pty, re, select, sys, tty

link = sys.argv[1]
fifo = sys.argv[2] if len(sys.argv) > 2 else None
master, slave = pty.openpty()
tty.setraw(slave)
if os.path.lexists(link):
    os.unlink(link)
os.symlink(os.ttyname(slave), link)
ties = {o: 0 for o in range(1, 17)}
buf = b""
fifo_fd = None
if fifo:
    if not os.path.exists(fifo):
        os.mkfifo(fifo)
    fifo_fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)

def out(s):
    os.write(master, (s + "\r\n").encode())

while True:
    r, _, _ = select.select([master] + ([fifo_fd] if fifo_fd is not None else []), [], [])
    if fifo_fd in r:
        for line in os.read(fifo_fd, 256).decode().splitlines():
            i, o = map(int, line.split())
            ties[o] = i
            out("Out%02d In%02d All" % (o, i))      # unsolicited, like the front panel
    if master in r:
        buf += os.read(master, 256)
        while True:
            m = re.match(rb"\s*(\d+)\*(\d+)([!&%$])", buf)
            if m:
                i, o = int(m.group(1)), int(m.group(2))
                kind = {b"!": "All", b"&": "RGB", b"%": "Vid", b"$": "Aud"}[m.group(3)]
                if 1 <= o <= 16 and 0 <= i <= 16:
                    if kind != "Aud":
                        ties[o] = i
                    out("Out%02d In%02d %s" % (o, i, kind))
                else:
                    out("E01")
                buf = buf[m.end():]
                continue
            m = re.match(rb"\s*(\d+)([&%$])", buf)
            if m:
                o = int(m.group(1))
                out("%02d" % ties[o] if 1 <= o <= 16 else "E12")
                buf = buf[m.end():]
                continue
            if re.match(rb"\s*\d+\*?\d*$", buf):       # incomplete command
                break
            buf = buf[1:] if buf else buf
            if not buf:
                break

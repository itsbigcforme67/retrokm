#!/usr/bin/env python3
"""Simulated network video matrix speaking the commands in
devices/template-text.conf, on a TCP port.

usage: fake_text_matrix.py PORT [FIFO] [--quiet]
  FIFO     optional named pipe: "IN OUT" lines are front-panel presses
  --quiet  never answers (a box that does not say what it did)
"""
import os, re, select, socket, sys

port = int(sys.argv[1])
fifo = next((a for a in sys.argv[2:] if not a.startswith("--")), None)
quiet = "--quiet" in sys.argv
ties = {o: 0 for o in range(1, 9)}
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", port)); srv.listen(4)
clients, bufs = [], {}
fifo_fd = None
if fifo:
    if not os.path.exists(fifo): os.mkfifo(fifo)
    fifo_fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)

def say(line):
    if quiet: return
    for c in clients:
        try: c.sendall(("> " + line + "\r\n").encode())    # a prompt before it, as some boxes do
        except OSError: pass

while True:
    r, _, _ = select.select([srv] + clients + ([fifo_fd] if fifo_fd is not None else []), [], [])
    for s in r:
        if s is srv:
            c, _ = srv.accept(); clients.append(c); bufs[c] = b""
            print("client", flush=True)
        elif s is fifo_fd:
            for line in os.read(fifo_fd, 256).decode().splitlines():
                i, o = map(int, line.split()); ties[o] = i
                say("input %d -> output %d" % (i, o))
        else:
            d = s.recv(256)
            if not d: clients.remove(s); continue
            bufs[s] += d
            while b"\n" in bufs[s]:
                line, bufs[s] = bufs[s].split(b"\n", 1)
                line = line.decode().strip()
                print("got", repr(line), flush=True)
                m = re.fullmatch(r"s in (\d+) av out (\d+)!", line)
                if m:
                    ties[int(m.group(2))] = int(m.group(1)); say("input %s -> output %s" % m.groups()); continue
                m = re.fullmatch(r"s out (\d+) off!", line)
                if m:
                    ties[int(m.group(1))] = 0; say("input 0 -> output %s" % m.group(1)); continue
                m = re.fullmatch(r"r av out (\d+)!", line)
                if m:
                    o = int(m.group(1)); say("input %d -> output %d" % (ties[o], o)); continue
                say("error")

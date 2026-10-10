#!/usr/bin/env python3
"""Type into any RetroKM machine from a script, through the hub's control port.

  tools/rkm-type.py precision 'net share RetroKM=C:\\RetroKM'   types it, presses Enter
  tools/rkm-type.py -n precision 'no enter'
  tools/rkm-type.py -k precision ctrl+esc                      named keys / combos

It gives that machine the keyboard first (goto). US layout. Grown out of
sgi bringup/type-octane.py.
"""
import socket
import sys
import time

HUB = ("127.0.0.1", 24851)
DELAY = 0.04
PLAIN = {" ": "space", "\t": "tab", "\n": "enter", "-": "minus", "=": "equal",
         "[": "leftbrace", "]": "rightbrace", "\\": "backslash", ";": "semicolon",
         "'": "apostrophe", "`": "grave", ",": "comma", ".": "dot", "/": "slash"}
SHIFTED = {"!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7",
           "*": "8", "(": "9", ")": "0", "_": "minus", "+": "equal", "{": "leftbrace",
           "}": "rightbrace", "|": "backslash", ":": "semicolon", '"': "apostrophe",
           "~": "grave", "<": "comma", ">": "dot", "?": "slash"}
MODS = {"ctrl": "leftctrl", "shift": "leftshift", "alt": "leftalt", "win": "leftmeta", "meta": "leftmeta"}


class Hub:
    def __init__(self):
        self.s = socket.create_connection(HUB, timeout=2)
        self.s.settimeout(0.3)

    def cmd(self, line):
        self.s.sendall((line + "\n").encode())
        time.sleep(DELAY)
        try:
            r = self.s.recv(65536).decode()
        except socket.timeout:
            r = ""
        if "error" in r:
            sys.exit("hub: " + r.strip())

    def tap(self, name, mods=()):
        for m in mods:
            self.cmd("key %s 1" % m)
        self.cmd("key %s 1" % name)
        self.cmd("key %s 0" % name)
        for m in reversed(mods):
            self.cmd("key %s 0" % m)


def char_key(c):
    if c.isalpha():
        return c.lower(), (["leftshift"] if c.isupper() else [])
    if c.isdigit():
        return c, []
    if c in PLAIN:
        return PLAIN[c], []
    if c in SHIFTED:
        return SHIFTED[c], ["leftshift"]
    sys.exit("cannot type %r" % c)


def main():
    args = sys.argv[1:]
    enter, keys = True, False
    while args and args[0].startswith("-") and len(args[0]) == 2:
        if args[0] == "-n":
            enter = False
        elif args[0] == "-k":
            keys = True
        args.pop(0)
    if len(args) < 2:
        sys.exit(__doc__)
    screen, words = args[0], args[1:]
    hub = Hub()
    hub.cmd("goto " + screen)
    time.sleep(0.2)
    if keys:
        for combo in words:
            parts = combo.split("+")
            hub.tap(parts[-1], [MODS.get(m, m) for m in parts[:-1]])
        return
    for c in " ".join(words):
        name, mods = char_key(c)
        hub.tap(name, mods)
    if enter:
        hub.tap("enter")


main()

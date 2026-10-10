#!/usr/bin/env python3
"""Ask lil' C to listen for where things are (see lilc_locate.py).

  .venv/bin/python locate.py speakers stackchan   chirps from the speakers
  .venv/bin/python locate.py voice tab5           talk to it for 4 s
  ... speakers stackchan order=0,1,2,3,4,5 gap_ms=600   settings for this run
"""
import json
import os
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
cfg = json.load(open(os.path.join(HERE, "config.json")))
kind = sys.argv[1] if len(sys.argv) > 1 else "speakers"
dev = sys.argv[2] if len(sys.argv) > 2 else "stackchan"


def call(path, body=None):
    r = urllib.request.Request("http://127.0.0.1:%d%s" % (cfg.get("port", 8790), path),
                               data=None if body is None else json.dumps(body).encode(),
                               headers={"X-LilC-Key": cfg["key"]})
    return json.loads(urllib.request.urlopen(r, timeout=10).read())


settings = {}
for a in sys.argv[3:]:
    k, _, v = a.partition("=")
    settings[k] = int(v) if v.lstrip("-").isdigit() else v
n = call("/api/locate", {"dev": dev, "kind": kind, "settings": settings})["id"]
print("locate %d: %s on %s%s" % (n, kind, dev, "  (talk now, for 4 seconds)" if kind == "voice" else ""))
last = None
for _ in range(120):
    j = call("/api/locate/%d" % n)
    if j["state"] != last:
        print(" ", j["state"])
        last = j["state"]
    if j["state"] == "done":
        print(json.dumps(j["result"], indent=1))
        break
    time.sleep(0.5)
else:
    print("no answer (is the device idle, and on the new firmware?)")

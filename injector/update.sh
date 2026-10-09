#!/bin/sh
# Updates a NanoC6 keyboard over WiFi, wherever it is plugged in.
#   ./update.sh octane          (the hub's screen name: the hub knows its address)
#   ./update.sh octanekb        (or the board's own name, found as octanekb.local)
#   ./update.sh 192.168.1.120   (or its address)
set -e
cd "$(dirname "$0")"
target=${1:?usage: update.sh <screen name or IP>}
case "$target" in
*.*.*.*) ip=$target ;;
*) ip=$(printf 'status\n' | python3 -c "
import socket, sys, time
s = socket.create_connection(('127.0.0.1', 24851)); s.sendall(b'status\n'); time.sleep(0.5)
for line in s.recv(65536).decode().splitlines():
    f = line.split()
    if len(f) > 1 and f[0] == 'screen' and f[1] == '$target':
        for w in f:
            if w.startswith('keyboard='): print(w.split('=')[1].split(':')[0])
")
   [ -n "$ip" ] || ip=$(getent hosts "$target.local" | awk '{print $1; exit}')
   [ -n "$ip" ] || { echo "no keyboard called '$target' (not a hub screen, no $target.local)" >&2; exit 1; } ;;
esac
echo "updating the keyboard at $ip"
exec ~/.platformio/penv/bin/pio run -e ota -t upload --upload-port "$ip"

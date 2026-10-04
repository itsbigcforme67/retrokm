#!/bin/sh
# Starts the laptop's agent, then the hub (needs sudo to read the USB
# keyboard and trackball).  Ctrl+C here, or ctrl+alt+shift+escape on the USB
# keyboard, stops both.
cd "$(dirname "$0")"
build/rkm-x11 -n laptop 127.0.0.1 &
AGENT=$!
trap 'kill $AGENT 2>/dev/null' EXIT INT TERM
sudo hub/retrokm-hub -c retrokm.conf "$@"

#!/bin/sh
# Run lil' C's bridge as a user service: started at login, restarted if it
# stops.  Its output goes to the journal:  journalctl --user -u lilc-bridge -f
# (First run bash setup.sh.)
set -e
cd "$(dirname "$0")"
here=$(pwd)
[ -x .venv/bin/python ] || { echo "run bash setup.sh first" >&2; exit 1; }
mkdir -p "$HOME/.config/systemd/user"
cat > "$HOME/.config/systemd/user/lilc-bridge.service" <<UNIT
[Unit]
Description=lil' C bridge (desk buddy <-> Claude)
After=retrokm-agent.service

[Service]
WorkingDirectory=$here
Environment=PYTHONUNBUFFERED=1
Environment=DISPLAY=${DISPLAY:-:0}
Environment=XAUTHORITY=${XAUTHORITY:-$HOME/.Xauthority}
ExecStart="$here/.venv/bin/python" lilc_bridge.py
Restart=always
RestartSec=5

[Install]
WantedBy=default.target
UNIT
systemctl --user daemon-reload
systemctl --user enable --now lilc-bridge
systemctl --user --no-pager --lines=5 status lilc-bridge

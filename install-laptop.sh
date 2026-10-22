#!/bin/sh
# Makes the hub machine start everything by itself: the hub as a system
# service at boot, and this desktop's X11 agent as a user service at login.  Run it again
# after rebuilding or editing retrokm.conf.  Asks for your sudo password.
set -e
cd "$(dirname "$0")"
[ -x hub/retrokm-hub ] && [ -x build/rkm-x11 ] && [ -f retrokm.conf ] || {
    echo "build the hub and build/rkm-x11, and create retrokm.conf, first" >&2
    exit 1
}
name=$(sed -n 's/^\[screen \([^]]*\)\].*/\1/p' retrokm.conf | head -n 1)

# this desktop's agent: a user service, started at login and restarted if
# it stops (it waits for X by retrying)
mkdir -p "$HOME/.local/bin" "$HOME/.config/systemd/user"
install -m 755 build/rkm-x11 "$HOME/.local/bin/rkm-x11"
rm -f "$HOME/.config/autostart/retrokm.desktop"   # the old way: never restarted after a crash
cat > "$HOME/.config/systemd/user/retrokm-agent.service" <<UNIT
[Unit]
Description=RetroKM agent for this desktop ($name)

[Service]
Environment=DISPLAY=${DISPLAY:-:0}
Environment=XAUTHORITY=${XAUTHORITY:-$HOME/.Xauthority}
ExecStart=$HOME/.local/bin/rkm-x11 -n $name 127.0.0.1
Restart=always
RestartSec=3

[Install]
WantedBy=default.target
UNIT
pkill -u "$(id -u)" -x rkm-x11 || true

# the phone page (phone/rkm_phone.py): a user service too
cat > "$HOME/.config/systemd/user/retrokm-phone.service" <<UNIT
[Unit]
Description=RetroKM phone page (port 24855)
After=retrokm-agent.service

[Service]
Environment=PYTHONUNBUFFERED=1
Environment=DISPLAY=${DISPLAY:-:0}
Environment=XAUTHORITY=${XAUTHORITY:-$HOME/.Xauthority}
ExecStart=/usr/bin/python3 "$PWD/phone/rkm_phone.py"
Restart=always
RestartSec=3

[Install]
WantedBy=default.target
UNIT
systemctl --user daemon-reload
systemctl --user enable --now retrokm-agent
systemctl --user enable retrokm-phone
systemctl --user restart retrokm-phone

# the hub, started at boot
sudo install -m 755 hub/retrokm-hub /usr/local/bin/retrokm-hub
sudo install -m 644 retrokm.conf /etc/retrokm.conf
sudo install -m 644 install/retrokm-hub.service /etc/systemd/system/retrokm-hub.service
sudo systemctl daemon-reload
sudo systemctl enable retrokm-hub
sudo systemctl restart retrokm-hub
sleep 1
systemctl --no-pager --lines=8 status retrokm-hub

#!/bin/sh
# Makes the hub machine start everything by itself: the hub as a system
# service at boot, and this desktop's X11 agent at login.  Run it again
# after rebuilding or editing retrokm.conf.  Asks for your sudo password.
set -e
cd "$(dirname "$0")"
[ -x hub/retrokm-hub ] && [ -x build/rkm-x11 ] && [ -f retrokm.conf ] || {
    echo "build the hub and build/rkm-x11, and create retrokm.conf, first" >&2
    exit 1
}
name=$(sed -n 's/^\[screen \([^]]*\)\].*/\1/p' retrokm.conf | head -n 1)

# this desktop's agent, started at login
mkdir -p "$HOME/.local/bin" "$HOME/.config/autostart"
install -m 755 build/rkm-x11 "$HOME/.local/bin/rkm-x11"
cat > "$HOME/.config/autostart/retrokm.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=RetroKM agent
Comment=Shared keyboard, mouse and clipboard
Exec=$HOME/.local/bin/rkm-x11 -n $name 127.0.0.1
X-GNOME-Autostart-enabled=true
X-MATE-Autostart-enabled=true
DESKTOP
pgrep -u "$(id -u)" -x rkm-x11 >/dev/null || (setsid "$HOME/.local/bin/rkm-x11" -n "$name" 127.0.0.1 >/dev/null 2>&1 < /dev/null &)

# the hub, started at boot
sudo install -m 755 hub/retrokm-hub /usr/local/bin/retrokm-hub
sudo install -m 644 retrokm.conf /etc/retrokm.conf
sudo install -m 644 install/retrokm-hub.service /etc/systemd/system/retrokm-hub.service
sudo systemctl daemon-reload
sudo systemctl enable retrokm-hub
sudo systemctl restart retrokm-hub
sleep 1
systemctl --no-pager --lines=8 status retrokm-hub

# RetroKM: status and how to pick up (2026-10-09)

Read this first in a new session. The README describes the design; this
file is the state of *this* desk and what was learned getting there.

## The desk

| Machine (screen name) | Link to the hub | Video |
|---|---|---|
| IdeaPad Gaming 3 (`laptop`), runs the hub | X11 agent, autostarts at login | own panel (`lcd`) + HDMI into the tall Dell's DVI |
| Armbian H96 Max (`armbox`) 192.168.1.108 | X11 agent, XFCE autostart | Extron in 2 |
| Dell Precision 410 (`precision`), "DELL2000" 192.168.1.139, Win 2000 (+98) | `C:\RetroKM\rkm-win32.exe` + `rkm.ini` | Extron in 1 |
| SGI Octane (`octane`) | NanoC6 PS/2 keyboard "octanekb" 192.168.1.120 (no agent yet) | Extron in 4 |
| Dreamcast (`dreamcast`) | none (`agent = no`) | Extron in 5 |

- **Monitors:** `lcd` (laptop panel); `tall` (Dell P2414H, portrait):
  Extron out 1 on VGA, the laptop on DVI; `crt` (CTX): out 2; `capA`/`capB`:
  capture cards on out 7/8.
- **Extron MVX 88:** on `/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_A9C8NR2K-if00-port0`,
  9600 baud. The adapter is an FTDI, not the PL2303 the user expected.
- **Hub:** the systemd service `retrokm-hub`, config `/etc/retrokm.conf`. Its
  source is `retrokm.conf` (gitignored: it holds the keyboard serial) and the
  state is `/etc/retrokm.conf.state` (monitor moves, shared-monitor input,
  picked colours). Change things with `./install-laptop.sh` (sudo: the user
  types the password, so start it in a terminal tab for them).
- **Input:** only the USB Fnatic Streak keyboard and Logitech trackball go
  through the hub; the laptop's own keyboard and touchpad never do.
  Ctrl+Alt+Shift+Esc stops the hub.
- **Control socket:** `printf 'status\n' | nc -q1 127.0.0.1 24851`. It
  also takes goto, show, tie, lock, key, rel, btn, ddc and lights.

## Pieces and their state

- **Hub** (`hub/`). It does:
  - edges by monitor grid, with `area =` for L-shaped desktops
  - a shared monitor (`shared =`, `ddc =` with DDC/CI input switching via
    `model:DELL P2414H` on i2c-15)
  - capture-card monitors
  - the panel port 24852
  - hardware keyboards (`RKM_CAP_KEYS`)
  - a 4 s rejoin grace before moving the keyboard off a machine that dropped
  - keyboard lighting (`lights.c`, Fnatic Streak, confirmed)
  - per-machine colours
- **Tab5 panel** (`panel/`), 192.168.1.140 / `rkm-panel.local`:
  - desk map, drag machines onto monitors, drag monitors around
  - tap the tall monitor to switch its input (DDC)
  - dotted lines for a shared monitor's other input
  - long-press a machine for options (HSV colour picker, default, use it)
  - two-layer renderer, so finger frames are patch-only
  - Updates: **OTA** `pio run -e tab5-ota -t upload`. Desktop build:
    `pio run -e native`, with PANEL_SCRIPT/PANEL_SHOT for scripted touches
    and screenshots (convert the PPM with PIL).
- **NanoC6 PS/2 keyboard** (`injector/`). Works on the Octane: it passes the
  PROM check, in scan code set 3.
  - Updates: `injector/update.sh octanekb` (OTA).
  - Log: UDP to the laptop :24853 (listen with a python recvfrom(65535)).
  - Settings: UDP :24854 (`show`, `name`, `label`, `swap`, `trace`,
    `drive`, `reboot`).
  - Board #1 (MAC ...9F:08) still has the first, broken firmware and needs
    ONE USB flash before use; the plan is for it to become the Dell's keyboard
    (`name precision`, `label dellkb`).
- **Windows agent** (`agents/win32/`): built with Open Watcom
  (`agents/win32/build-watcom.sh`, `~/.local/watcom`). It works on Windows
  2000; 98 SE is untried, as is clipboard on Windows.
- **X11 agent:** used by the laptop and armbox. The laptop build needs X
  headers fetched with `apt-get download libx11-dev libxtst-dev x11proto-dev`
  into a scratch dir (no sudo); the armbox's are in `~/rkm-sdk`.

## Hard-won lessons

- **ESP32-C6:** `Serial.begin()` re-enables the USB pads and takes GPIO
  12/13 back. Never call it in PS/2 mode. The `drive` command checks the
  lines can be pulled low.
- **Passive USB-to-PS/2 adapters:** USB D+ = PS/2 clock, D- = data. The
  Octane holds clock low ~20 s while its PROM boots, and keeps the port
  powered across restarts.
- **SGI keyboard setup:** `F2`, `F0 03` (set 3), `FA`, `FC 14`, `FC 76`,
  `ED`, `F4`.
- **Arduino `WiFiClient::connected()`** judged healthy links dead (stale
  errno), so the NanoC6 and the panel use raw lwIP sockets.
- **The laptop once lost its `192.168.1.0/24 dev wlp0s20f3` route.** TCP to
  LAN hosts then hung half-open while ping worked. Fix:
  `nmcli device reapply wlp0s20f3`. Guarded now by
  `/etc/NetworkManager/dispatcher.d/90-keep-lan-route` (source in
  `sgi bringup/`). Keep the "Wired connection 1" profile (shared mode, ran a
  DHCP server on the wired port) at autoconnect=no; octane-link is disabled
  by the user.
- **Dell P2414H DDC:** it only answers reads while on DVI, but a set to DVI
  works from VGA.
- **The Dell sits behind a Linksys bridge** (MAC 00:25:9c:13:d4:72), and the
  "TN-200" box at .187 is a TRENDnet NAS. Find the Dell with a NetBIOS
  sweep: `nmblookup -A`.

## Handed over

The SGI Octane session ("SGI Octane bringup and data recovery") was given
`sgi bringup/RETROKM-KEYBOARD.md` and `sgi bringup/type-octane.py` (types
text into the Octane through the hub's control socket).

## lil' C desk buddy (2026-10-10, built, not installed)

Commits 01f0869 and 655097a. Hub: BUDDY frames, `activity` and `buddy`
panel commands. X11 + Windows agents show lil' C's window; the Windows tray
icon (left click) calls him. Panel code moved to `panel/lib/rkm_panel`
(lil' C's Tab5 firmware embeds it); monitors show numbers. To install:
- hub: `./install-laptop.sh` (sudo, user types the password)
- laptop agent: rebuild `build/rkm-x11` (X headers via apt-get download)
  and restart the autostarted one
- armbox: rebuild rkm-x11 on the box with `~/rkm-sdk`, add `-lm`
- Dell: copy `build/rkm-win32.exe` to `C:\RetroKM\`
- the stand-alone panel firmware still builds (`pio run -e tab5`), but the
  Tab5 is meant to run lil' C's firmware now, which includes the desk map.

## Open ideas / next steps

- **Set-3 keys:** verify key by key on the Octane (the table was written
  from memory).
- **Second NanoC6:** USB-flash it as the Dell's keyboard (BIOS, boot menu,
  Ctrl+Alt+Del), or as a PS/2 mouse for the Octane.
- **"Restart into 98 / 2000"** for the Dell: the agent edits `boot.ini`
  `default=` and reboots. Discussed, not built.
- **Octane agent:** an X11 agent on IRIX once root is recovered (the agent
  is strict C89 for MIPSpro).
- **3D desk view** on the panel (the user wants it later).
- **Untried:** Windows 98 SE, the classic Mac agent, clipboard on Windows.

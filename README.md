# RetroKM

One keyboard, one mouse and one clipboard across a desk of machines that
were never meant to meet: modern Linux, Windows 98 SE / 2000, 68k and
PowerPC classic Mac OS, and SGI IRIX. Optionally it watches and drives an
Extron matrix switcher, so the pointer always knows which machine is on
which monitor.

"RetroKM" is a working name; rename freely.

## How it works

```
 keyboard + mouse                     Extron matrix (RS-232, SIS)
        |                                      |
   +----v--------------------------------------v----+
   |  hub (Linux)                                   |
   |  evdev capture -> pointer position -> routing  |
   |  monitor layout <- switcher ties               |
   |  clipboard store, charset + line-end convert   |
   +---+--------+----------+----------+---------+---+
       | TCP    |          |          |         | uinput
   rkm-x11   rkm-win32  RetroKM    rkm-x11   the hub's own
   (IRIX)    (98SE/2K)  (Mac OS)   (Linux)   desktop
```

- **The hub owns everything.** It grabs the physical keyboard and mouse,
  keeps the pointer position itself, and sends absolute positions to
  whichever machine is active. Agents are deliberately dumb, which is what
  makes a 68k agent practical.
- **The layout is about monitors, not machines.** You describe where the
  monitors stand on the desk. Each monitor shows either a fixed machine or
  whatever the switcher ties to one of its outputs. Press a button on the
  Extron and the edges rearrange themselves.
- **Clipboard text is converted at the hub.** It is stored as UTF-8 and
  delivered as MacRoman with CR, CP1252 with CRLF, Latin-1 or UTF-8 with LF,
  depending on what each agent declares. Characters a target cannot hold
  degrade to ASCII stand-ins (curly quotes become straight quotes, and so on).
- **Hybrid by design.** A machine's input and clipboard can come from two
  different connections under one screen name, so a hardware injector
  (ADB / PS/2 / USB) can supply input while a small software agent supplies
  the clipboard. See [docs/PROTOCOL.md](docs/PROTOCOL.md).

## What has and has not been tested

This was built without access to the real hardware. Be clear-eyed about it:

| Part | State |
|---|---|
| Hub routing, layout, hotkeys, clipboard conversion | Tested end to end (`make test`, 36 checks), also under ASan/UBSan |
| Extron driver | Tested against a simulator written from the SIS manual, not a real switcher |
| X11 agent | Tested on Linux under Xvfb; compiles as strict C89; not yet run on IRIX |
| Hub keyboard/mouse capture (evdev) and local injection (uinput) | Compiles; not run, the build machine had no input devices |
| `rkm-uinput` Linux agent | Compiles; not run |
| `rkm_clip.py` | Tested with stand-in commands, not with wl-clipboard itself |
| Windows agent | Written and syntax-checked against stub headers only; never compiled with a real SDK or run |
| Classic Mac agent | Written and syntax-checked against stub headers only; never compiled with Retro68 or run |

Expect the Windows and Mac agents to need a debugging pass on real machines.
The Mac agent in particular pokes low-memory globals at interrupt time; try
it on a machine you do not mind rebooting.

## Quick start

### Hub (Linux)

```
# needs a C compiler plus the X11 and XTest headers (Debian: libx11-dev libxtst-dev)
make                    # builds hub/retrokm-hub, build/rkm-x11, build/rkm-uinput
cp retrokm.conf.example retrokm.conf     # then edit
sudo ./hub/retrokm-hub -c retrokm.conf   # -v for protocol and switcher chatter
```

The hub needs to read `/dev/input/event*` and, if a screen is `local = yes`,
to write `/dev/uinput`. Instead of root you can join the `input` group and
add a udev rule: `KERNEL=="uinput", GROUP="input", MODE="0660"`.

For the laptop's own clipboard, run a clipboard-only helper inside your
desktop session, under the same screen name as the local screen:

```
agents/linux/rkm_clip.py -n laptop 127.0.0.1      # Wayland (wl-clipboard) or X11 (xclip)
build/rkm-x11 -c -n laptop 127.0.0.1              # X11 alternative
```

Every screen name an agent uses must have a `[screen NAME]` section in the
hub config. `retrokm.conf.example` documents every option.

### IRIX (and any other X11 Unix)

```
cc -o rkm-x11 agents/x11/rkm_x11.c common/rkm_proto.c -lXtst -lXext -lX11
./rkm-x11 -n octane -s both 192.168.1.10
```

`-s both` shares PRIMARY as well as CLIPBOARD, which is what most IRIX
applications actually use. Clipboard text is Latin-1 by default on IRIX.

### Windows 98 SE / 2000

Build `agents/win32/rkm_win32.c` plus `common/rkm_proto.c` with Visual C++ 6,
Open Watcom or MinGW (command lines are at the top of the source file). Binaries
from a current MinGW-w64 generally will not start on Windows 98; use VC6 or
Open Watcom for that target. Run `rkm-win32.exe 192.168.1.10 pc`, or put an
`rkm.ini` next to it and drop a shortcut in Startup. It sits in the tray.

Both Windows installs on the dual-boot machine can use the same screen name.

### Classic Mac OS (68k System 6/7, PowerPC Mac OS 8/9)

Build `agents/macos` with [Retro68](https://github.com/autc04/Retro68) (see
`CMakeLists.txt`). It is one 68k application for both Macs; it needs MacTCP
or Open Transport. Put a text file named `RetroKM Config` next to it:

```
hub=192.168.1.10
name=quadra
```

Things to know:

- System 6 needs MultiFinder. Under the plain System 6 Finder the agent
  only works while it is the running application.
- Classic applications keep a private clipboard and only publish it when
  switched out. To get around that the agent briefly brings itself to the
  front and back ("flip") after you press Command-C / Command-X or click in
  the menu bar and then leave the Mac, and whenever new text arrives. That
  needs System 7. `flip=always` or `flip=never` in the config changes it.
- Right click is sent as control-click. There is no scroll wheel.
- Clipboard limit is 32,000 characters.

### Modern Linux as a client (once the hub is a separate box)

```
sudo build/rkm-uinput -n laptop -g 1920x1080 192.168.1.10    # input
agents/linux/rkm_clip.py -n laptop 192.168.1.10              # clipboard, in your session
```

## Using it

- Push the pointer off the edge of a monitor to move to the machine on the
  neighbouring monitor. Edges are blocked while a mouse button is held.
- Machines that are not on any monitor are still reachable with a `goto`
  hotkey.
- `show <screen>` hotkeys route a machine's video to the monitor under the
  pointer through the Extron, and (with `on_tie = follow`) control follows.
- Copy on one machine, move the pointer away, paste on another.
- **Ctrl+Alt+Shift+Escape** stops the hub and gives the keyboard back. The
  hub also lets go of the keyboard and mouse whenever no agent is connected.
- The control socket accepts the same commands as hotkeys, plus `status`:

  ```
  $ nc 127.0.0.1 24851
  status
  active: octane at 512,300 on monitor left
  screen octane       1280x1024 ready   input=192.168.1.21:1027 clipboard=192.168.1.21:1027 extron-in=1
  monitor left        pos=0,0 extron-out=1 (input 1) shows=octane
  ...
  ```

## Known limits

- **No encryption or authentication.** Keystrokes cross the LAN in the
  clear; that is the price of Windows 98 and System 6 clients. Keep it on a
  trusted network segment.
- Clipboard is plain text only.
- Agents need a booted OS with networking. BIOS screens, installers and the
  Windows 2000 logon / Ctrl+Alt+Del desktop need a hardware injector (below).
- Absolute pointer positioning does not suit games that read relative mouse
  motion.
- The hub's local screen is an absolute pointer device; on a multi-monitor
  laptop the desktop decides which output it maps to.
- One X screen per X11 agent (no dual-head Octane yet).
- Keyboard layouts: keys are sent as physical positions (USB HID usages) and
  each OS applies its own layout.

## Roadmap

1. **Shake out the Windows and Mac agents on real hardware.**
2. **Hardware injectors.** A microcontroller per machine speaking ADB, PS/2
   or USB HID. The protocol already has what they need: relative-only
   pointer mode (`RKM_CAP_REL`, tested in the suite with a simulated
   injector) and split input/clipboard connections. Missing: the firmware,
   and a serial transport in the hub so injectors can hang off USB serial
   ports (framing is specified in docs/PROTOCOL.md).
3. **Standalone hub.** The hub has no desktop dependencies, so it already
   suits a Raspberry Pi class box with the keyboard, mouse and USB-serial
   adapter plugged in; the laptop then becomes an ordinary client. A true
   microcontroller hub (USB host + Ethernet) is feasible because the hub
   core is small, but it means porting the evdev and socket layers.
4. Mac OS X on PowerPC agent, System 6 INIT for Finder-only use, dual-head
   X11, richer clipboard formats.

## Source layout

```
common/         wire protocol (C89, shared by every agent)
hub/            Linux hub
agents/x11/     IRIX and other X11 systems
agents/win32/   Windows 98 SE / 2000
agents/macos/   classic Mac OS (68k binary, also for PowerPC)
agents/linux/   uinput input agent and clipboard helper
tests/          end-to-end test, Extron simulator, build stubs
docs/           protocol description
```

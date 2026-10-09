# RetroKM hardware keyboard (M5Stack NanoC6)

A NanoC6 that plays a PS/2 keyboard, fed from the hub over WiFi. It works
where no software agent can: BIOS setup, the Windows boot menu, Safe Mode,
the Ctrl+Alt+Del login, an SGI PROM or miniroot prompt.

## Hookup (no soldering)

    NanoC6 USB-C -> USB-C-to-USB-A cable -> passive USB-to-PS/2 adapter -> keyboard port

The passive adapters (the little green ones that came with keyboards) just
connect USB D-/D+ to PS/2 data/clock, and 5 V/ground. The NanoC6's USB pins
are GPIO 12 and 13 on the chip, so the firmware uses them as PS/2 lines. It
is powered by the keyboard port.

**Plug it in with the computer off.** Computers (and SGI PROMs especially)
look for the keyboard at power-on.

At power-up the board looks at those pins: a PS/2 port holds them high, a
USB port holds them low. Plugged into the laptop it stays an ordinary USB
device, so it can be flashed and configured there.

## Setup

    cp include/secrets.h.example include/secrets.h   # WiFi, hub address
    pio run -t upload                                # NanoC6 plugged into the laptop
    pio device monitor                               # settings console

Console commands: `show`, `name <screen>` (the hub `[screen]` it types
into, default from secrets.h), `hub <host>`, `swap` (if keys come out as
garbage: the adapter has clock and data the other way round), `reboot`.
Settings are kept in flash.

The blue LED blinks while it looks for WiFi and the hub, and stays on once
the hub has accepted it.

## How it fits

It connects to the hub with `RKM_CAP_KEYS`: a hardware keyboard for that
screen. While it is connected, the screen's keys go to it; the pointer and
clipboard still go to the machine's software agent if one is running. With
no agent at all (booting, or a machine without one), the screen still takes
keys, so you can move the pointer onto its monitor and type.

Scan code sets 2 (PCs) and 3 (SGI Indy / Indigo2 / O2 / Octane, which also
ask for make/break on every key) are both supported; the computer picks.
`test/ps2kbd_test.cpp` checks the codes and command replies:

    g++ -std=c++17 -o /tmp/kt test/ps2kbd_test.cpp && /tmp/kt

## Caveats

- PS/2 lines idle at 5 V through the computer's pull-up resistors, and the
  C6 is a 3.3 V part. The firmware only ever pulls the lines low (open
  drain), and the pull-ups limit the current, but this is outside the
  chip's rating. A BSS138 level shifter is the proper fix.
- Not yet tried on real hardware (as of the first version).
- Keyboard only. Mouse emulation would be a second board on the mouse port.

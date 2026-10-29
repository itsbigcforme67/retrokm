# Xbox pad (RetroKM)

An **M5Stack Atom S3R** (or the screenless **Atom EchoS3R**, which is what
runs it on this desk) that plays an **original Xbox controller** (a
Microsoft "Controller S", USB 045E:0289, XID class) on the Xbox's pad port,
driven by the RetroKM hub. The game pad plugged into (or paired with) the
laptop follows the keyboard: while the `xbox` screen has the keyboard, the
Elite plays the Xbox, just as the keyboard types on the Octane through the
NanoC6. The game's rumble comes back and shakes the real pad.

Which controllers drive the Xbox is set on the panel's accessories page
(lines from controllers to machines). A controller drives the machine in
focus if linked; with "keep while out of focus" on for that link (hold the
Xbox or the controller on the panel), or the controller's "works out of
focus everywhere" switch, it drives it all the time. **Guide + RB / Guide + LB** moves the
focus to the next / previous machine the pad is linked to. While the laptop
is a target the hub leaves the pad alone, so laptop games see it as usual.

## Hookup

    Atom S3R USB-C -> USB-C-to-A cable -> "Xbox to USB" female adapter -> Xbox pad port

The Xbox port is USB 1.1 on another plug: pin 1 +5 V (red), 2 D- (white),
3 D+ (green), 4 GND (black), 5 yellow (light-gun sync, unused). A cheap
adapter, or a breakaway cable from a dead pad spliced to a USB-A socket.
The port powers the Atom.

## Setup

1. `include/secrets.h`: copy `secrets.h.example` (the WiFi and the hub's
   address; same as the PS/2 keyboard's).
2. Hub config: add a screen named `xbox` (or whatever `name` the pad uses),
   with its switcher `input =`:

       [screen xbox]
       input = 6
       label = Xbox

   then `./install-laptop.sh`.
3. First flash over USB (a fresh board is a serial port):
   `~/.platformio/penv/bin/pio run -e atoms3r -t upload`. A board whose
   firmware owns the USB port (the EchoS3R's audio firmware, or this one in
   xbox mode) still shows Espressif's serial/JTAG port for a few seconds
   when plugged in: start esptool waiting for /dev/ttyACM* and replug.
   After that: `./update.sh xbox` (over WiFi).

## Two USB modes

| mode | USB-C port is | for |
|------|---------------|-----|
| xbox (default) | the controller | the Xbox, or `xpadtest.py` on the laptop |
| pc | the ESP32-S3's own serial/JTAG port | flashing over USB, the serial console |

`mode pc` / `mode xbox` (saved, reboots), or hold the button (the screen)
while plugging in for pc mode just this once. Last resort: hold the side
reset button ~2 s until the green LED lights (ROM download mode).

## Log and settings

Like the PS/2 keyboard: the log goes as UDP to the hub's host port 24853,
settings come on UDP 24854 (and USB serial in pc mode):

    echo show | nc -u -w1 xboxpad.local 24854
    show | name <screen> | label <board name> | hub <host> | player 1-4 | mode pc|xbox | reboot
    i2c | servo <port 1-4> <deg> | servo stop|push|back <deg> | servo backms <ms> | press [ms]

`player` is which pad port of the console this dongle sits in (one dongle
per port). On the panel's accessories page, each controller linked to the
Xbox is a player there (P1, P2, ...; long-press to change, several on P1
merge into one).

## Power-button servo

The board (an Atom EchoS3R) sits in an M5Stack Atom Motion base: servo driver
at I2C 0x38 on G38/G39, with its own servo power and battery. The servo on port 1
is a continuous-rotation one (the value is speed: about 90 stops it), so `press`
is timed: spin toward the button (`servo push`, for `press <ms>`, default 250),
spin back for `servo backms` (default 400) so the arm ends against a backstop,
then stop (`servo stop`). Settings are saved. Code in src/servo.cpp.

## Driving it from scripts (or Claude)

The hub's control socket plays the pad as if the Elite did it:

    printf 'goto xbox\npad 1\n' | nc -q1 127.0.0.1 24851     # hold A
    printf 'pad 0\n' | nc -q1 127.0.0.1 24851                # let go
    pad <buttons hex> [lt rt lx ly rx ry]   (sticks -32768..32767, up +)

Buttons: A 1, B 2, X 4, Y 8, LB(white) 10, RB(black) 20, Back 40, Start 80,
LS 200, RS 400, up 800, down 1000, left 2000, right 4000.

## Testing without an Xbox

Plug the Atom (xbox mode) into the laptop; Linux's `xpad` driver takes it as
an original Xbox pad. `./xpadtest.py` then presses every button, moves the
sticks and triggers through the hub, checks what the kernel sees, and sends
a rumble effect back through it.

## Files

    src/xidpad.[ch]   report and descriptor bytes, hub pad -> Xbox mapping (PC-tested)
    src/xid_usb.cpp   TinyUSB class driver for the XID interface
    src/main.cpp      WiFi, hub link, log, settings, rumble back to the hub
    src/screen.cpp    status screen
    test/run.sh       tests for the report format and mapping
    xpadtest.py       end-to-end check on the laptop
    update.sh         update over WiFi

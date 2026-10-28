# Video switching: devices, wires and routes (design, 2026-10-28)

Status: design, not built yet. This replaces the single `[extron]` section
with a description of the whole video chain, so RetroKM works with whatever
switchers, scalers and monitors a desk has, and so the planner website (at
the end) can use the same descriptions.

## Why

Today the hub knows one switcher (Extron SIS on one serial port). A screen
has an `input =` on it, a monitor has an `output =`, and `via =` names a
middle box that the hub cannot drive (the RetroTINK: "plugs moved by hand").

Real desks, starting with this one, have chains:

```
PS2 (component) ─> Extron MVX in 2 ─> MVX out 4 ─> RetroTINK 5X ─> HDMI matrix in 4 ─> matrix out 1 ─> capture
S-Video console ─> Extron MPS 112 ─> MPS S-Video out ─> RetroTINK 5X S-Video in ─> ...
laptop (HDMI)   ─> HDMI matrix in 1 ─> matrix out 2 ─> Dell DVI   (Dell: VGA from MVX out 1, input picked by DDC)
```

Showing one machine on one monitor can mean setting three or four boxes.
Other people's desks will be different mixes of Extron, Kramer, cheap HDMI
matrices, scalers, KVM boxes and monitors with several inputs. The hub
should know the cables and work the route out itself.

## The model

- **Device**: a box with numbered or named **ports**, in and out. A
  switcher connects inputs to outputs; a converter (scaler, DAC, the 5X)
  turns one kind of signal into another; a monitor is a device with only
  inputs, and picking one is a switch too (DDC, IR, or by hand).
- **Wire**: a cable from an output port to an input port. Fixed; written in
  the config once.
- **Source**: a machine's video output (each `[screen]` has one or more).
- **Signal kinds**: `vga` (RGBHV/RGBS), `sog` (sync on green), `component`,
  `svideo`, `composite`, `hdmi` (HDMI and DVI). Each port says what it
  carries or accepts. This catches real mistakes: component into a VGA
  monitor gets wrong colours, and 480i never syncs on a PC monitor.

The hub builds a graph: device ports are nodes; wires and each device's
possible internal connections are edges. Asking for "ps2 on capA" is a path
search from the PS2's source port to the capture's input.

### Routing

- Breadth-first, cheapest path. Costs: each hop 1; a conversion 2; taking a
  port another monitor is using now: 10 (allowed, but a free path wins).
- A converter or switcher output carries one thing at a time. If the only
  path takes the 5X away from another monitor, the route still happens and
  the panel and `status` say what was taken over.
- Applying a route: set devices from the source end to the monitor end
  (switchers, then the converter's mode, then the monitor's input), so the
  monitor sees the final picture last and only re-syncs once.
- A monitor's "what is showing" is found by walking back along the current
  ties to a source. This replaces `extron_input_for()`.

### Devices that cannot report

Extron and DDC can be asked what they are showing. IR boxes and hand
switches cannot. Their state is *assumed* (the last thing the hub set) and
the panel draws it dimmed. A `manual` device makes the hub show a step to
do by hand ("set the Cintiq to VGA") instead of sending a command.

## Config

```
[device mvx]
driver = extron-sis
port = serial:/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_A9C8NR2K-if00-port0
baud = 9600
inputs = 8              # in1..in8
outputs = 8             # out1..out8
signal = vga component composite sog     # what passes through it

[device tink]
driver = retrotink5x    # from devices/retrotink5x.conf
port = serial:/dev/ttyACM0          # or: ir:lilc  /  ir:atom-ir
# its ports (named, from the device file): component, svideo -> hdmi

[device matrix]
driver = text
model = generic-hdmi-8x8           # devices/generic-hdmi-8x8.conf
port = tcp:192.168.1.50:23

[wire]
mvx.out4 = tink.component
mps.svideo = tink.svideo
mps.composite = mvx.in6
tink.hdmi = matrix.in4

[screen ps2]
video = mvx.in2 component

[monitor crt]
video = mvx.out2

[monitor tall]
video = mvx.out1, laptop       # two inputs: the switcher, and the laptop direct
ddc = model:DELL P2414H        # how its input gets picked (else: manual)
```

`input =`, `output =`, `via =` and `[extron]` go away. Until the user's
real `retrokm.conf` is moved over, the loader turns the old form into one
`extron-sis` device plus wires and logs that it did; then that code is
removed (git history keeps it).

## Drivers

Built into the hub, in C, each able to run several times (two Extrons):

| driver | what it is |
|---|---|
| `extron-sis` | today's `extron.c` made per-device: tie `in*out!`, read `out%`, front-panel changes arrive by themselves |
| `ddc` | monitor input switching over DDC/CI (today's `ddc.c`) |
| `text` | any box that takes text commands over serial or TCP; the commands come from a device file |
| `ir` | sends codes through an IR blaster (an ESP32, or lil' C's head) |
| `manual` | a hand switch: the panel shows the step |

### Device files (`devices/*.conf`)

Most AV gear is a few text commands. A device file says what the box is
and how to talk to it, so adding a new switcher is a text file, not C:

```
# devices/retrotink5x.conf
name = RetroTINK 5X Pro
kind = converter
in = component component composite      # the green jack also takes composite
in = svideo svideo
out = hdmi hdmi
# per input: what to send to make it the active one (a 5X profile)
select.component = remote prof1\n
select.svideo = remote prof2\n
ir.select.component = nec:0x..          # same, for the IR route (codes TBD)
```

```
# devices/generic-hdmi-8x8.conf
name = Generic 8x8 HDMI matrix (RS-232/Telnet)
kind = switch
inputs = 8
outputs = 8
signal = hdmi
tie = s in {in} av out {out}!\r\n
read = r av out {out}!\r\n
reply = input {in} -> output {out}
```

The same files describe the box for the planner site (ports, signals,
rough price), so one library serves both. People share new ones by pull
request.

## Panel, phone page, protocol

- The panel's JSON gains `devices` (name, kind, ports, ties, online,
  assumed) and each monitor's current route. Old panels keep working: the
  per-monitor fields stay.
- New control commands: `route <screen> <monitor>` (what `show` becomes),
  `devices`, `tie <device> <in> <out>`.
- `docs/PROTOCOL.md` gets a protocol version number.

## Order of work

1. **Device layer, same behaviour.** Move `extron.c` to a per-device
   driver; graph, wires and router; the old config converted on load.
   `tests/e2e.py` and `fake_extron.py` must pass unchanged, plus new tests
   with two fake switchers in a chain.
2. **`text` driver and device files.** Fake TCP matrix in `tests/`.
   MPS 112 (likely `extron-sis` too) and an HDMI matrix file.
3. **Converters with modes.** The 5X over USB serial if it answers; check
   on the real box first.
4. **`ir` and `manual` drivers.**
5. **Panel and phone page** draw devices and routes; the Tab5 firmware
   reads the new JSON.
6. **This desk moved over**, then the old-config code removed.
7. **Planner site** (below).

## Planner site (later)

A static web page (GitHub Pages, no server) where someone lays out a
desk: monitors and their inputs, machines and their outputs, the boxes they
already own. It uses the same device files and the same router (written
again in JavaScript, or the C built to WebAssembly) to show:

- which machine can reach which monitor today, and which cannot and why
  ("component into a VGA-only monitor");
- **suggestions**: the cheapest boxes from the library that fill the gaps
  ("a 5X-class scaler between the switcher and the capture card fixes 3 of
  4 consoles"), solved as "add one device, re-run the router, keep the
  additions that unlock the most pairs per dollar";
- a ready `retrokm.conf` and a wiring list to download.

Prices in device files are rough ranges and dated, and the site says so.

## Open questions

- Does the 5X take commands over USB serial (profile load)? Check on the
  box before step 3.
- Exact SIS for the MPS 112 (it has three separate 4x1 switchers).
- Audio: follow video by default (`!` on Extron); a separate audio route
  later if someone needs it.
- Routes that need two monitors' worth of a shared box at once: refuse, or
  take over? Taking over with a notice for now.

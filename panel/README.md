# RetroKM touch panel (M5Stack Tab5)

A 1280x720 touch screen with two pages (tabs in the header):

**displays**
- **Monitors** across the top, laid out as they stand. Drag one to a new spot
  and the hub's screen edges follow (saved in the hub's `.state` file).
- **Machines** along the bottom, computers and consoles alike, each with a
  little picture. Drag from a machine onto a monitor and the Extron switches
  that monitor to it; a console behind the RetroTINK is plugged into the
  tink and the tink goes on that monitor. Pull a cable's plug off a monitor
  to unplug it. Tap a machine to focus it (green box: keyboard, mouse and
  linked pads go there). A controller badge in a card's corner means a pad
  is linked to it.
- Each monitor has a **number** on its corner (screens left to right, then
  capture cards), so "monitor 2" means the same thing to you and to lil' C.

**accessories**
- The things you hold along the top (keyboard + mouse, each game pad the
  hub has seen), the machines along the bottom. Drag from an accessory to
  a machine to link them; pull a line's end off a machine to unlink it, or
  drop it on another machine. Many to many. Pads show their player (P1..P4)
  where their line ends.

**Seats** (`../docs/SEATS.md`): the chip in the header's corner ("James:
Laptop v") picks whose focus and wires the panel shows; each seat has a
ring in its colour on the machine it drives, and other seats' wires are
dim on the accessories page. The picker can also take a seat offline.

**Hold** a machine or an accessory for its options: colour (a machine's
cables, an accessory's lines), for a machine which of the things linked to
it it keeps while out of focus and the player of each linked pad, for an
accessory a "works out of focus everywhere" switch, its player on each
machine and where it is kept, and an "about" that says how it is attached.

Pages are `Page` subclasses in `lib/rkm_panel/rkm_panel.cpp` (the
`pages[]` list); kinds of accessory come from the hub, so a new kind
(mics, headsets) shows up in the options without panel changes.

## Inside lil' C

The panel is a component, `lib/rkm_panel` (`rkm_panel.h`). This firmware
(`src/main.cpp`) runs it full time. lil' C's Tab5 firmware links the same
folder in and opens it from the bottom bar of his desktop, with a "lil' C"
button in the header to go back. Desktop test of that mode:
`PANEL_EMBEDDED=1` (the button then quits).

## Hub side

Add to the `[hub]` section of the config:

```
panel_port = 24852
```

Give each `[screen]` a `label` and an `art` (`ideapad`, `precision`,
`armbox`, `octane`, `dreamcast`, `retrotink`, `xbox`, `ps2`; anything else
gets a plain box). A console behind the RetroTINK says `via = retrotink`. A monitor
with `shared = <machine>` has a second input; tap it on the panel to say
which input it is on. `desk.conf.example`
has a full desk behind an Extron MVX 88.

The panel port takes `layout`, `tie <screen> <monitor>`, `untie <monitor>`,
`move <monitor> <col> <row>`, `share <monitor> [on|off]`, `goto <screen>`, `lock`,
`plug <console>`, `unplug <box>`, `link <accessory> <screen> [player]`,
`unlink <accessory> <screen>`, `keep <accessory> <screen> on|off`,
`keepall <accessory> on|off`, `color <screen|accessory> <rrggbb|default>`,
`own <accessory> <seat>`, `seat <name> goto|lock|offline|pointer ...`,
`seat add|remove|color ...`,
`activity`, `buddy <screen> show|hide [label]` and `ping`, and pushes
`layout {json}` whenever anything changes. It cannot type keys or move the
pointer. Like the rest of RetroKM it has no password, so keep it on your
own network.

## Building

```
cp include/secrets.h.example include/secrets.h     # WiFi and the hub's address
pio run -e tab5 -t upload                          # flash the Tab5 over USB
```

The same UI runs in a window on Linux (needs SDL2), which is handy for
working on it without the tablet:

```
pio run -e native
PANEL_HUB=127.0.0.1 .pio/build/native/program
```

`PANEL_SCRIPT` and `PANEL_SHOT` drive it with a scripted finger and save
screenshots; see the end of `src/main.cpp`.

## Art

`src/art.h` draws each machine from rectangles, triangles and circles in a
160x120 box. Replace any of them freely.

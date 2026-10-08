# RetroKM touch panel (M5Stack Tab5)

A 1280x720 touch screen that shows the desk:

- **Monitors** across the top, laid out as they stand. Drag one to a new spot
  and the hub's screen edges follow (the move is saved in the hub's
  `.state` file next to its config).
- **Machines** along the bottom, each with a little picture. Drag from a
  machine onto a monitor and the Extron switches that monitor to it. Pull a
  cable's plug off a monitor to unplug it. Tap a machine to give it the
  keyboard and mouse.

## Hub side

Add to the `[hub]` section of the config:

```
panel_port = 24852
```

Give each `[screen]` a `label` and an `art` (`ideapad`, `precision`,
`armbox`, `octane`, `dreamcast`; anything else gets a plain box). A monitor
with `shared = <machine>` has a second input; tap it on the panel to say
which input it is on. `desk.conf.example`
has a full desk behind an Extron MVX 88.

The panel port takes `layout`, `tie <screen> <monitor>`, `untie <monitor>`,
`move <monitor> <col> <row>`, `share <monitor> [on|off]`, `goto <screen>`, `lock` and `ping`, and pushes
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

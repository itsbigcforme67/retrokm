# RetroKM setup planner

A web page for laying out a retro desk before buying anything: the machines,
the monitors and TVs, and the switchers and scalers you already own. It
works out the cables, shows what cannot reach what and why, suggests the
kinds of hardware that fill the gaps, and writes a starter `retrokm.conf`.

There are two pages on the same plan:

- **`desk.html`, the desk builder**: the simple, visual way. Your machines
  are tiles; drag them onto your screens (or tap a machine, then a
  screen) to draw a **mode**: one layout you want to switch to, with
  everything in it on screen at once. Make one mode for work, one for
  games, and so on. Each placed machine gets a mark (✓ works with what you
  own, ★ works with the suggested hardware, ✗ not possible, with why).
- **Try it out** (`desk.html#try`, the second tab of the desk builder): a
  pretend desk running RetroKM on the cables the planner laid, with what
  you own or with the suggestions. Drag machines onto screens or switch
  to a mode and it tells you what would happen: what the hub switches by
  itself (⚡), what you do by hand with a remote or buttons (✋), which
  screen loses its picture because a box only carries one (⚠), and what
  cannot work (✗). Point at a screen to see where the keyboard and mouse
  go, and type to see who gets the keys. A list of the setup's limits
  sits beside it.
- **`index.html`, the detailed planner**: lists and checkboxes for every
  setting, the cables to plug, the wiring picture and the config.

Open either in a browser: straight from disk, or on GitHub Pages.
There is no server, no build step and nothing downloaded from the
internet. The plan stays in your browser (it is remembered between visits)
until you save it as a JSON file.

## What it does

1. **Machines**: consoles and computers, the video each can send
   (composite, S-Video, component, RGB/SCART, VGA, VGA sync-on-green, DVI,
   HDMI), whether it runs a RetroKM agent, which screens it should show
   on, and which other machines it should be on screen *at the same time
   as* (two consoles side by side, say).
2. **Monitors and TVs**: their inputs, whether a VGA input takes
   sync-on-green, and how many pictures they show at once. A capture card
   counts as a monitor.
3. **Hardware you own**: picked from a short catalogue, read from a RetroKM
   device file (`devices/*.conf`), or described by hand.
4. **The plan**: how many machine/monitor pairs work now, what to add, with
   a plain reason for each ("SGI Octane sends sync-on-green VGA; Dell CRT
   can show it, but Capture card needs its sync-on-green VGA turned into
   HDMI, which a scaler that accepts RGB and sync-on-green does"), what is
   still not possible, a picture of the wiring, the list of cables, and the
   config with copy and download buttons.

Suggestions are kinds of hardware with well-known examples. There are no
prices or shop links: check a box's manual before you buy it.

## How it decides

- **Cables**: each wanted pair gets the cheapest path through what the
  plan has. An existing cable is almost free, a new one costs a little, a
  conversion more, and a machine taking a monitor input that other
  machines want costs a lot, so a switch gets used. Every jack holds one
  cable.
- **Modes**: each mode is routed before anything else, one show at a
  time, each with everything the earlier shows hold blocked (see Side by
  side below for what a route holds). Two orders are tried. Suggestions
  count every show that works, so a box that fixes part of a mode still
  counts. The config lists each mode as commands for the hub's control
  port (`plug ps2`, `show tink tv` ...): the hub has no single "mode"
  command yet, but a script or Claude's seat can send these lines.
- **Side by side**: two machines that should be on screen at the same
  time are routed first, the second one with everything the first holds
  blocked. A route holds each scaler or converter it passes (one picture
  at a time), each switch output, and its monitor (or just the input, on a
  monitor that shows two pictures at once). If that fails the plan says
  what they share, and suggestions count the pair like a connection.
- **Suggestions**: try adding each kind of box from the catalogue, keep the
  one that makes the most pairs work, and repeat. When two help equally,
  the one that could serve more of what is still missing wins, then the
  smaller step.
- **Config**: switchers the hub can drive (Extron SIS, or text commands
  from a device file) become `[device]`s with `[wire]`s between them;
  machines say `video = dev.inN` and monitors `video = dev.outN`. A box the
  hub cannot set (a RetroTINK, a hand switch) is either looked through,
  when a switcher feeds it, or becomes a `[screen]` with `agent = no` that
  the machines plugged into it name with `via =`. See `docs/SWITCHING.md`.

The try-out is a simulation of the plan, not of your real boxes: it
uses no new cables, treats every screen as showing one machine, and says
what the hub would do, the way the hub's own router does it (a free way
first, else take over what is in the way).

## Files

| file | what |
|---|---|
| `desk.html`, `desk.js` | the desk builder (drag machines onto screens, modes) and the try-out |
| `index.html`, `app.js` | the detailed planner |
| `style.css` | both pages |
| `logic.js` | routing, suggestions, reasons, the config (no page code) |
| `catalogue.js` | the kinds of hardware it knows; entries with a `deviceFile` match `devices/` |
| `examples/*.json`, `examples.js` | example plans (the `.js` copy is for opening from disk) |
| `make-config.mjs` | `node planner/make-config.mjs plan.json [--owned] > retrokm.conf` |
| `test.mjs` | `node planner/test.mjs` |

`make test-planner` runs `test.mjs` and `tests/planner_test.py`, which starts
the hub on the configs made from the example plans.

## Adding hardware

Add an entry to `catalogue.js`: its ports and signals, a `need` phrase used
in reasons, `examples`, and a `weight`. If the hub can drive it, add a
device file in `devices/` too (see `devices/README.md`) and point
`deviceFile` at it; `test.mjs` checks the two agree.

## Not done yet

- Signal kinds are not in the hub's config yet, so the config does not say
  them; the planner's checks are its own.
- A monitor with several inputs gets one `video =` (plus `shared =` for a
  machine cabled straight to it); further inputs are listed as comments.
- Cables cannot be drawn or moved by hand; the planner lays them all.
- Modes are not a hub feature yet: the config has them as lists of
  control commands to send, not as one hotkey each.
- Resolutions and refresh rates are only covered roughly (15 kHz console
  video on a PC monitor is flagged).

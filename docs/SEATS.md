# Seats: several people (or scripts) driving the desk at once

Status: built 2026-10-28 (hub, panel, tests/seats_test.py). Replaces the
one-off "Claude's keyboard" (`key-to`, an accessory that typed on one
machine out of focus).

## The idea

A **seat** is someone at the desk: a name, a colour and a **focus** (the
machine it is driving, its pointer there, the monitor that pointer is on).
The owner's seat (`james`, green: today's ring) and Claude's (`claude`,
orange) are the first two; there can be any number.

- Every **accessory** belongs to one seat, and its input follows that
  seat's focus. The USB keyboard + mouse and the pads are the owner's;
  Claude's scripted keyboard + mouse is Claude's.
- An accessory's **wires** (links on the accessories page) are where its
  seat may go: the hub refuses `goto` or an edge crossing to a machine none
  of the seat's accessories is wired to. A seat with no accessories at all
  is not limited (there is nothing to wire).
- Two seats on one machine is fine: two rings, and their input simply
  merges there.
- **One clipboard per seat.** What a seat copies is what that seat pastes.
- **Every command can name a seat:** `seat <name> <command ...>`. Without
  a seat, a command is the first seat's (so everything that works today
  keeps working). A seat driven by commands moves exactly like a hand on a
  mouse: `seat claude rel 400 0` crosses screen edges, `seat claude goto`
  jumps.
- A seat with no devices still has a ring when it is somewhere.
- A seat can **go offline**: its ring goes and its keys are let go of. Any
  input to it brings it back where it was: its devices (a key press, the
  mouse, a pad button) or a command (`seat claude key ...`). That keeps the
  owner from locking themselves out with the panel out of reach.
- "Keep while out of focus" stays, as an optional per-wire pin (a hardware
  keyboard pinned to one machine, a pad left on the Xbox). Claude no longer
  needs it: it has its own focus.

## Data model (hub)

```
Seat {                                   cfg.seats[MAX_SEATS], [0] = the first seat
  name, label, color
  active, mon, px, py, rel_fx, rel_fy    its focus (was: globals)
  mods, buttons, swallowed[256], down[256]  its keyboard + mouse state
  locked, offline, touched, lost_ms
  last_x/last_y/have_last[MAX_SCREENS]   where its pointer was on each machine
  clip, clip_len, clip_gen, clip_src     its clipboard
}
Accessory.seat                           index into cfg.seats
Screen.clip_seat, Screen.clip_gen        whose clipboard (which generation) it holds
Screen.clip_owner                        the seat that arrived there last: what the
                                         machine reports on its clipboard is that seat's
Screen.mover                             the seat whose pointer it is drawing
```

Per machine, ENTER is sent when the first seat arrives and LEAVE when the
last one goes (LEAVE means "let go of everything"). A seat leaving a
machine another seat is still on sends key-ups for its own held keys and
buttons instead.

Keyboard lights follow the focus of the seat that owns the USB keyboard.
lil' C's `pointer`/wiggle is the first seat's.

## Commands

Control port (127.0.0.1:24851):

```
seat <name> <cmd ...>        any command as that seat: goto, warp, rel, btn, wheel,
                             key, lock, pad, pointer, offline
seats                        one line per seat (also in status)
seat add <name> [rrggbb]     a new seat (saved in the state file)
seat remove <name>           (not the first seat); its accessories go to the first
seat color <name> <rrggbb>
own <accessory> <seat>       give an accessory to a seat
key-to <screen> <key> <0|1>  shortcut: seat claude goes there (if not already) and
                             presses the key. Kept for old scripts.
```

`status` keeps its first line (`active: ...` = the first seat) and adds a
`seat <name> on <screen> at x,y ...` line per seat. Panel port: the same
`seat <name> ...` prefix for goto, lock, offline and pointer; `own`;
`seat add/remove/color`.

Layout JSON: a `seats` array (`name`, `label`, `color`, `active`, `monitor`,
`locked`, `offline`) and each accessory's `seat`. The old `active`,
`locked` and per-screen `focus` stay, meaning the first seat, for old panels.

Limits: a machine's clipboard is reported by its agent on LEAVE, which is
sent only when the last seat leaves; a seat leaving a machine another seat
stays on does not hand over what it copied there. The phone page and lil'
C's `pointer` are the first seat's.

## Panel (Tab5 and the desktop build)

- A **seat picker** in the header's corner: a chip in the chosen seat's
  colour. Tap it for the list of seats; pick one to view and edit as that
  seat; each has a "go offline" button.
- Displays page: a ring per seat (its colour) on the machine it drives,
  several rings nested when seats share a machine. Tapping a machine moves
  the chosen seat there.
- Accessories page: the chosen seat's accessories and wires bright, other
  seats' wires dim behind. Hold an accessory for its options: "belongs to"
  lists the seats.

## Migration

- No `[seat]` in the config: the first seat is called `main` (the owner's
  config says `[seat james]`, `color = 22c55e`: no `#`, which starts a
  comment in the config). `claude` (orange) is always made if missing.
- The `claude` accessory (kind `keys`, 2026-10-28) becomes a scripted
  keyboard + mouse owned by seat `claude`; its "kept everywhere" is dropped
  (it would type into every machine at once now), its wires stay. A state
  file without a `seats` line is the old kind: this happens once.
- All other accessories belong to the first seat.

## Later (designed for, not built)

- Several controllers at once: Tab5s or phones signed in as different
  seats, each with its own devices on a shared desk. The panel connection
  would carry its seat (`iam <seat>`), the picker would show only what that
  login may switch to.
- Permissions beyond wires: who may move whose devices, hand control to
  another seat, force a device on or off for another seat.

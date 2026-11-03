# RetroKM wire protocol, version 1, revision 2

**Version** is the number in HELLO and WELCOME (`RKM_VERSION`, 1). It only
changes if old agents can no longer work with a new hub, which has not
happened yet. **Revision** (`RKM_REVISION`) counts additions: each one adds
messages or optional fields, which older peers ignore, and never changes
what an existing message means. Nobody needs to know the revision of the
other side: a hub sends new messages only to agents with the capability
they belong to, and an agent that never gets them keeps its old behaviour.

| Revision | Added |
|---|---|
| 1 | everything up to and including BUDDY, SOUND, PAD (to 2026-10) |
| 2 | BUDDY_LOOK, BUDDY_GAZE, BUDDY_SAY: the desk pet's eyes drawn from numbers (2026-11) |

Designed so the smallest client (a 68000 Mac running MacTCP, or a
microcontroller) can implement it in a page of C: fixed tiny frames,
big-endian integers, no negotiation, no text parsing. The reference
implementation is `common/rkm_proto.[ch]`.

## Transport

A reliable byte stream. Today that is TCP, agent connecting to the hub on
port 24850. Nothing in the framing depends on TCP.

## Frame

```
+--------+----------------+------------------+
| type   | length         | payload          |
| 1 byte | 2 bytes, BE    | 0..512 bytes     |
+--------+----------------+------------------+
```

A frame with a length above 512 is a protocol error; drop the connection.
Unknown types must be ignored, so new messages can be added later.

## Messages

`A>H` is agent to hub, `H>A` is hub to agent. Integers are big-endian;
`s16` is signed.

| Type | Name | Dir | Payload |
|---|---|---|---|
| 0x01 | HELLO | A>H | version u8 (=1), caps u8, charset u8, eol u8, width u16, height u16, clipmax u16 (KB), name (rest, up to 31 bytes ASCII) |
| 0x02 | WELCOME | H>A | version u8, status u8 (0 ok, 1 unknown name, 2 version) |
| 0x03 | PING | H>A | none. Sent every 5 s |
| 0x04 | PONG | A>H | none |
| 0x05 | SCREEN | A>H | width u16, height u16, then optionally count u8 and that many monitors (x u16, y u16, w u16, h u16, width mm u16, height mm u16; mm 0 = unknown). Sent after HELLO and whenever the desktop changes; the monitors replace the config file's `area` lines, and give the panel each monitor's shape and size |
| 0x10 | ENTER | H>A | x u16, y u16, modifiers u8 |
| 0x11 | LEAVE | H>A | none |
| 0x12 | MOVE | H>A | x u16, y u16, absolute, in the agent's pixels |
| 0x13 | MOVEREL | H>A | dx s16, dy s16 |
| 0x14 | BUTTON | H>A | button u8 (1 left, 2 right, 3 middle, 4 back, 5 forward), down u8 |
| 0x15 | WHEEL | H>A | dy s16, dx s16; 120 units per notch, positive = away from the user / right |
| 0x16 | KEY | H>A | usage u8, state u8 (0 up, 1 down, 2 repeat), modifiers u8 |
| 0x17 | RESET | H>A | none. Release everything |
| 0x18 | PAD | H>A | pad u8 (0), buttons u16, lt u8, rt u8, lx s16, ly s16, rx s16, ry s16 |
| 0x19 | RUMBLE | A>H | pad u8 (0), strong u16, weak u16 |
| 0x20 | CLIP_BEGIN | both | format u8 (1 = text), total length u32 |
| 0x21 | CLIP_DATA | both | up to 480 bytes |
| 0x22 | CLIP_END | both | none |
| 0x30 | BUDDY | H>A | cmd u8 (0 hide, 1 show), label (rest, up to 48 bytes) |
| 0x31 | BUDDY_EVENT | A>H | event u8 (1 its window was clicked, 2 called from the machine) |
| 0x32 | SOUND | H>A | id u16, rate u32, chirp ms u16, gap ms u16, low Hz u16, high Hz u16, level % u8, lead-in ms u16, count u8, channel u8 [count] |
| 0x33 | SOUND_EVENT | A>H | id u16, status u8 (0 playing, 1 done, 2 failed), channels u8, rate u32 |
| 0x34 | BUDDY_LOOK | H>A | how the eyes look: 20 bytes of numbers, then the name (below). Rev 2 |
| 0x35 | BUDDY_GAZE | H>A | mode u8 (0 look about freely, 1 look along gx, gy, 2 watch the pointer), gx s8, gy s8 (-100..100 across and down), hold u8 (tenths of a second; 0 until the next GAZE), blink u8 (1 blink now). Rev 2 |
| 0x36 | BUDDY_SAY | H>A | seconds u8 (0 = 4), text (rest, up to 120 bytes): a caption in the label strip. Rev 2 |

**caps**: bit 0 `INPUT` (can inject), bit 1 `CLIP` (has a clipboard),
bit 2 `REL` (pointer is relative only), bit 3 `KEYS` (a hardware keyboard),
bit 4 `BUDDY` (can show the desk pet's buddy window), bit 5 `SOUND` (can play test chirps),
bit 6 `PAD` (plays a game controller, see below).

**charset**: 0 UTF-8, 1 ISO 8859-1, 2 Windows-1252, 3 MacRoman.
**eol**: 0 LF, 1 CRLF, 2 CR. The agent always sends and receives clipboard
text in its own declared charset and line ending; the hub converts.

**usage** is a USB HID keyboard usage (page 0x07): 0x04 is A, 0xE0..0xE7 are
the modifiers. **modifiers** is the HID boot-report modifier byte (bit 0
left ctrl, 1 left shift, 2 left alt, 3 left GUI, 4..7 the right-hand ones)
describing the state after the event. It is redundant on purpose, so an
agent can resynchronise.

## Session

1. Agent connects and sends HELLO.
2. Hub answers WELCOME. On a non-zero status it closes the connection.
3. Hub sends PING every 5 s; the agent answers PONG. The hub drops an agent
   that is silent for 20 s; an agent should reconnect after 30 s of silence.

A new connection claiming a capability for a screen name replaces the old
holder of that capability, which is how a rebooted (or dual-booted) machine
takes its place back.

## Input

- ENTER means the pointer has arrived: move to x,y and press the listed
  modifiers. LEAVE means it has gone: release every key and button the
  agent is holding.
- The hub tracks the pointer and sends absolute MOVE frames, at most `rate`
  per second per screen (default 125), always flushing the latest position
  before a BUTTON or KEY.
- Agents that honour key repeats re-post the key on state 2; agents whose
  OS repeats held keys itself ignore state 2.
- A key-up for a key the agent does not hold must be ignored.

### Relative-only agents (hardware injectors)

A device that emulates a real mouse cannot be told an absolute position.
With the `REL` capability the hub sends MOVEREL deltas instead, and tracks
its own idea of the position using the size from HELLO. On ENTER it homes
the pointer: one MOVEREL of (-30000, -30000) to pin it in the top-left
corner, then one MOVEREL to the entry point. Target-side pointer
acceleration makes this approximate; turn acceleration off on the target
where possible.

## Accessories and game controllers

Accessories are the things you hold: the keyboard + mouse, and each game
pad plugged into or paired with the hub's machine. Each is linked to the
machines it may drive (lines on the panel's accessories page), many to
many; links are kept in the state file. An accessory's input goes to the
machine in focus (the green box: where the pointer is, or tapped on the
panel) if it is linked there, and also to every linked machine where it is
kept ("keep while out of focus", per link), or everywhere it is linked when
its own "works out of focus everywhere" switch is on. So with two pads, one
can stay on the Xbox while the other follows focus. The keyboard + mouse can
only be moved into machines it is linked to; out of focus a machine gets its
keys, not the pointer. Kinds are a table in the hub (`acc_kinds`).

A pad linked to a machine is a player there (P1..P4); several pads on the
same player are merged into one (buttons OR'ed, the larger stick
deflection wins). A device with the `PAD` capability (the Xbox dongle,
playing an original Xbox controller on a console's pad port) plays one
player and gets ENTER, LEAVE, PAD and RESET; all but PAD mean "let go of
everything, sticks to the centre". Up to four may connect for one screen.

PAD carries one player's whole state every time (`pad` = player - 1), so a
lost frame never leaves a button stuck: buttons (`RKM_PAD_*` bits: A 0001,
B 0002, X 0004, Y 0008, LB 0010, RB 0020, Back 0040, Start 0080, Guide
0100, LS 0200, RS 0400, up 0800, down 1000, left 2000, right 4000, Share
8000), the triggers 0..255, and the sticks -32768..32767 with right and
**up** positive. The hub sends one whenever that player's state changes,
and the current state on ENTER.

The agent sends RUMBLE (its player, strong and weak motor, 0..65535)
whenever the game changes it; the hub plays it on the real pads that are
that player there right now.

While the hub's own desktop is a target of a pad, the pad is left to the
desktop (games on the hub machine see it as usual) and others get copies;
otherwise the hub grabs it. Guide + RB / Guide + LB moves the focus to the
next / previous machine that pad is linked to.

Panel and control commands: `link <accessory> <screen> [player]`,
`unlink <accessory> <screen>`, `keep <accessory> <screen> on|off`,
`keepall <accessory> on|off`, `color <accessory> <rrggbb>|default`. The
layout JSON gives each accessory `keepAll` and each of its links `keep`. The control port's
`pad <hex> [lt rt lx ly rx ry]` plays a scripts-only pad: player 1 on the
machine in focus.

**Seats** (`docs/SEATS.md`): every accessory belongs to a seat (someone at
the desk, or a script) and follows that seat's focus; its wires are where
the seat may go. `own <accessory> <seat>` gives it to another seat. The
layout JSON lists `seats` (name, label, color, active, monitor, locked,
offline, devices) and each accessory's `seat`; `active`, `locked` and each
screen's `focus` are the first seat's. The `claude` accessory (kind `kbm`,
played by scripts) is Claude's seat's; a state file from before seats
names it kind `keys`, which loads as `kbm`.

## Consoles behind boxes (panel and control commands)

Some screens are not on the switcher directly but go through a box that
cannot be switched remotely: game consoles into the RetroTINK, by moving
plugs. A console says `via = <box>` in the config; boxes may chain (a
component switcher `via = retrotink`). Which console is wired into a box is
told to the hub with `plug <console>` / `unplug <box>` (panel or control
port) and kept in the state file. Wherever a box is shown, the hub treats
the console plugged into it (through any chain of boxes) as the machine on
that monitor, so the mouse, keyboard and game pad reach it. The layout JSON
gives each screen `via` and `plugged`.

## Clipboard

- An agent reports its clipboard (BEGIN, DATA..., END) when it changed
  locally, no later than when it receives LEAVE.
- The hub stores the newest clipboard and delivers it to a screen when the
  pointer enters it, or immediately if the pointer is already there. It is
  not sent back to the screen it came from, and not sent to an agent whose
  `clipmax` is smaller than the converted text.
- Clipboard-only connections (caps = `CLIP`) receive LEAVE for their screen
  as their cue to report, and nothing else from the input group.

## Split connections

Input and clipboard for one screen may arrive on separate connections that
use the same name: one with `INPUT`, one with `CLIP`. This is how the hub's
own desktop works (uinput in the hub plus a clipboard helper in the
session), and how a hardware injector pairs with a software clipboard agent.

## The desk pet (buddy window)

A desk pet (`docs/PETS.md`) can visit the desk's machines. An agent with
the `BUDDY` capability shows it as a small window with its eyes when it
gets BUDDY show, and sees it off on BUDDY hide; show while it is already
there just changes the label (empty: the name from BUDDY_LOOK). Clicking
its window sends BUDDY_EVENT 1; calling it from the machine (the Windows
agent's tray icon) sends BUDDY_EVENT 2. The hub passes events on to panel
connections as `buddy <screen> click|summon`, and takes
`buddy <screen> show|hide [label]` from them. Agents without the
capability never see BUDDY frames.

**BUDDY_LOOK** (revision 2) gives the eyes as numbers; the agent draws them
itself, so even a small one can. An agent that never gets one draws the
default, which is also what agents drew before: tall black and white eyes
on the X root weave. The hub sends it right after WELCOME when it runs a
pet, and again if the pack changes. Payload (`rkm_look` in
`common/rkm_proto.h`):

| Byte | Field | Default |
|---|---|---|
| 0 | eye height : width, tenths (10 round, 15 tall) | 15 |
| 1 | space between the eyes, % of an eye's half-width | 20 |
| 2 | pupil half-width, % of the eye's half-width | 24 |
| 3 | pupil height : width, tenths | 12 |
| 4 | rim thickness, % of the eye's half-width | 16 |
| 5 | flags: bit 0 use the colours (else black and white), bit 1 plain background (else the weave) | 0 |
| 6 | upper lid: how much of the eye it covers, % | 0 |
| 7 | seconds between blinks, about (0 never) | 0 |
| 8 | mouth line width, % of the window (0 none) | 0 |
| 9 | smile, s8: -100 frown .. 100 smile | 0 |
| 10..12 | background colour, RGB | ffffff |
| 13..15 | the whites, RGB | ffffff |
| 16..18 | ink (rims, pupils, lids, mouth, label strip), RGB | 000000 |
| 19 | name length (up to 31), then the name: window title and default label | "" |

Fields may be added after the name in later revisions. A shorter payload
than 20 bytes means the defaults. Zero shape and pupil sizes mean the
defaults too.

**BUDDY_GAZE** says where to look. With mode 1 or 2 the window stays where
it is while it looks (it does not dart off), and on hide it keeps looking
that way as it slides off (the way it is going). Mode 0, or the hold
running out, lets it look about on its own again. **BUDDY_SAY** shows a
caption in the label strip for a few seconds instead of the label; text
too long for the strip scrolls. Text in BUDDY, BUDDY_LOOK and BUDDY_SAY is
in the agent's charset from HELLO.

### The pet engine (panel port)

With `pet = <name>` in the config the hub runs a pet from
`pets/<name>/pet.conf` itself (`docs/PETS.md`): it sends the BUDDY frames,
and a brain can drive it on the panel port:

- `pet <name> where` answers `pet <name> at <screen>`,
  `pet <name> travelling <from> <to>` or `pet <name> nowhere`.
- `pet <name> go <screen>`: off it goes (and stays there a while).
- `pet <name> say <text>`: a caption where it is.
- `pet <name> look <dx> <dy>` (-1..1 across and down), `look pointer`,
  `look free`: its eyes; the engine leaves them to the brain for 3 s.

Events to every panel connection: `pet <name> arrived <screen>`,
`pet <name> clicked <screen>` (its window), `pet <name> summoned <screen>`
(called from that machine; it comes), `pet <name> left <screen>` (the
machine went away under it). `<name>` is the pack's folder name; the pet's
display name works in commands too. The old `buddy <screen> click|summon`
lines are still sent. Without `pet =` the hub only passes BUDDY frames on,
as before, so a pet whose own brain drives `buddy` is not disturbed.

The panel port also answers `activity` with
`activity {"screen":[s since input, s since a key],...}` (-1: never), so
a pet can stay out of the way of a machine someone is using, and `pointer`
with `pointer <screen> <monitor> <fx> <fy> <s since the mouse moved> <wiggle>`
(fx, fy: 0..1 across and down that monitor; wiggle: how hard the
mouse is being shaken - the motion is cut into short strokes and each turn
between strokes adds up to 1 (a full about-turn), fading over about 1.5 s:
a shake scores ~8, a slow wave ~4, ordinary pointing up to ~2; a pet pack's
`wiggle =` is on this scale; `pointer - - 0 0 -1 0`
when nothing has it), so it can watch the mouse go from monitor to monitor,
and notice it being shaken at it.

## Test chirps

For a pet's brain to work out where it is by sound, an agent with `SOUND` plays a
linear chirp (low to high Hz, Hann window) on each listed speaker in turn:
the first after the lead-in, then one every chirp + gap ms. Channels are in
WAVE order (0 front left, 1 front right, 2 centre, 3 sub, 4 rear left, 5
rear right). The agent builds the sound itself, opens 5.1, quad or stereo,
whichever the card takes, leaves out speakers the layout lacks, and reports
how many channels it got. The panel port takes
`sound <screen> <id> <rate> <chirp> <gap> <low> <high> <level> <lead> <ch,ch,..>`
and passes on `sound <screen> <id> playing|done|failed <channels> <rate>`.
The Windows agent has it.

## Planned: serial transport

For injectors attached to the hub by a serial or USB-CDC link, frames will
be wrapped for resynchronisation: each frame followed by a CRC-8 and
COBS-encoded, with 0x00 as the delimiter. Not implemented in the hub yet.

## Security

None. No authentication, no encryption. This is a protocol for a trusted
desk LAN that includes machines from 1989.

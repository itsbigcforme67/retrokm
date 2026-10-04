# RetroKM wire protocol, version 1

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
| 0x05 | SCREEN | A>H | width u16, height u16. Resolution changed |
| 0x10 | ENTER | H>A | x u16, y u16, modifiers u8 |
| 0x11 | LEAVE | H>A | none |
| 0x12 | MOVE | H>A | x u16, y u16, absolute, in the agent's pixels |
| 0x13 | MOVEREL | H>A | dx s16, dy s16 |
| 0x14 | BUTTON | H>A | button u8 (1 left, 2 right, 3 middle, 4 back, 5 forward), down u8 |
| 0x15 | WHEEL | H>A | dy s16, dx s16; 120 units per notch, positive = away from the user / right |
| 0x16 | KEY | H>A | usage u8, state u8 (0 up, 1 down, 2 repeat), modifiers u8 |
| 0x17 | RESET | H>A | none. Release everything |
| 0x20 | CLIP_BEGIN | both | format u8 (1 = text), total length u32 |
| 0x21 | CLIP_DATA | both | up to 480 bytes |
| 0x22 | CLIP_END | both | none |

**caps**: bit 0 `INPUT` (can inject), bit 1 `CLIP` (has a clipboard),
bit 2 `REL` (pointer is relative only).

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

## Planned: serial transport

For injectors attached to the hub by a serial or USB-CDC link, frames will
be wrapped for resynchronisation: each frame followed by a CRC-8 and
COBS-encoded, with 0x00 as the delimiter. Not implemented in the hub yet.

## Security

None. No authentication, no encryption. This is a protocol for a trusted
desk LAN that includes machines from 1989.

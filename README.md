# lil' C

A Stack-chan (M5Stack CoreS3 + Stack-chan base) that you can talk to Claude
through, and that keeps an eye on your Claude Code tasks.

```
 CoreS3 (WiFi)  --->  bridge on your PC  --->  claude CLI (your subscription)
  face, mic,          speech-to-text,          + reads Claude desktop app
  speaker, servos     text-to-speech           session status
```

The device never holds your Claude login. The bridge runs `claude -p` with the
login the Claude app already has, so questions count against your
subscription like any other Claude Code use. Answers use Sonnet by default
(`"model"` in `bridge/config.json`) and may use web search.

## Using it

- **Face screen:** hold anywhere on the face and talk (up to 10 s), then let go.
  lil' C looks up and "thinks", then answers out loud with a caption.
- **Reply:** the answer scrolls across a Speak & Spell style VFD (slanted cyan
  14-segment characters in a red bezel) in step with the voice, then keeps
  looping as a marquee. Drag it sideways to scrub; tap to close; hold the face
  to ask a follow-up.
  A follow-up question continues the same chat; after 10 quiet minutes a new
  chat starts.
- **Face:** a recreation of X11's xeyes. The pupils follow your finger, or
  wherever the head is looking.
- **Bottom bar, right half:** how many tasks are working / need you. Tap for
  the task list (drag to scroll, tap the header to go back).
- **Bottom bar, left half:** pictures and videos Claude sent you in any chat
  (found via SendUserFile in the transcripts). It turns yellow when something new
  arrives. Tap an item to view it; swipe left/right for the next one, and tap to
  hide the caption. Videos play at 10 fps with sound; tap to stop.
- **Project access:** lil' C can read (never change) everything in
  `~/claude projects`, Claude's memory notes, session transcripts and the
  live task list, so you can ask "how's the Rugrats port going?"
- When a task newly needs you, lil' C glances away and shows a sweat drop until
  it's handled (silently, with no beeps).

## One lil' C

There is only one lil' C. He lives on his two homes, the Stack-chan and the
Tab5, and wanders over to the desk's computers now and then. The bridge keeps
track of where he is (`bridge/lilc_presence.py`); each device asks it every
0.7 s (`GET /api/presence`).

- **Where he isn't**, the screen shows the empty desk with a swinging "BACK
  SOON!" sign and where he went ("lil' C is on the Tab5", "lil' C is visiting
  the Media box"). The Stack-chan's head turns to watch him go.
- **Calling him:** tap that screen. He leaves wherever he was and pops in
  (hold on to start talking straight away). Asking a question also calls him
  over. Clicking his window on a computer, or the RetroKM tray icon on
  Windows, calls him to that computer.
- **Wandering:** left alone for a few minutes he gets restless and may hop to
  his other home or visit a computer nobody has used for 2 minutes. He prefers
  the home that was used last. After being called he stays put for 5 minutes.
- **Getting out of the way:** if someone starts typing on a computer he's
  visiting (or uses its mouse for a few seconds), he goes home.
- **On computers** he is a small xeyes window that darts around the screen.
  On the laptop it's `bridge/lilc_popup.py`; on the other machines it's drawn
  by their RetroKM agent (X11 and Windows agents, "desk buddy" in MULTI KM's
  `docs/PROTOCOL.md`), shown and hidden through the hub.
- Every weight and time is in `bridge/config.json` under `"presence"` (the
  defaults are at the top of `lilc_presence.py`): stay times, how restless he
  is, how much he prefers the last-used home, where each home is for the
  Stack-chan's head (`homes`: yaw/pitch).

## The desk (RetroKM)

lil' C runs the RetroKM KVM desk (MULTI KM) through the hub's panel port
(`bridge/lilc_desk.py`, port 24852 on the bridge's PC; `"hub"` in config.json
to change it). Each question carries a note with the desk as it is now, and
Claude answers desk requests with tags the bridge carries out at once:

- "Bring up the Octane on monitor 2": `[[kvm:show octane tall]]`
- "Turn off the CRT": `[[kvm:blank crt]]`
- "Let me use the Dell": `[[kvm:keyboard precision]]`

Monitors are numbered left to right, capture cards last; the Tab5 desk map
shows the same numbers on each monitor.

On the Tab5, the middle of the bottom bar ("desk (KVM)") opens the RetroKM
desk map itself, full screen; the "lil' C" button at its top left comes back.

## Hopping over to a computer

Ask how something is going on a computer ("how's the Rugrats port going on the
laptop?") and Claude starts its answer with a `[[hop:laptop]]` tag. Then:
1. lil' C turns his head towards that machine (`machines` in
   `bridge/config.json`: yaw/pitch in degrees, + yaw = his left). For now the
   laptop is assumed to be behind him.
2. His screen shows a swinging "BE RIGHT BACK!" sign.
3. On the laptop, a small xeyes window of his face (`bridge/lilc_popup.py`,
   plain X11) slides up from the bottom corner of its screen (`monitor` = the
   xrandr output). Then it darts around every monitor, crossing between them
   only where they line up (never through dead space). It pauses to look around
   or follow the mouse, with its eyes leading each dart. On the other desk
   machines the RetroKM agent shows the same window.
4. When Claude has the answer, he darts back to the corner, the window slides away, lil' C turns back, his
   eyes pop in and he answers.

Claude can also check running work on the laptop (ps, pgrep, uptime, free, df,
sensors), read-only. Later: positions from MULTI KM's monitor layout, the
webcam for fine aiming.

## Voice

lil' C talks like a 1979 Speak & Spell. The bridge speaks the reply with
piper, re-encodes it into LPC frames quantized to the TMS5100/TMC0281 chip's own
tables, then plays those through an emulation of the chip (chirp voicing, noise,
10-stage lattice filter, 8 kHz, 8-bit DAC): `bridge/lilc_speakspell.py`. The
tables come from MAME (BSD-3-Clause). For a normal voice, set
`"voice_style": "plain"` in `bridge/config.json`.

## Speed

Measures that keep answers quick (2026-10-06):
- One long-running `claude` process (stream-json in/out) plus a warm spare,
  instead of starting the CLI for every question. That saves ~5 s each time.
- Answers are spoken sentence by sentence as Claude writes them, so lil' C
  starts talking after the first sentence.
- whisper `tiny.en` on 2 threads (steady when the PC is busy), primed with the
  project names so it hears them right.
- TMS5100 synth in C (`tms5100.c`, built on first use with gcc) and piper's
  `lessac-low` voice.

## Setup

### 1. Bridge (on the PC)

```
cd "LIL C/bridge"
sh setup.sh      # one time: venv + whisper + piper voice (~200 MB)
sh run.sh
```

The first run writes `bridge/config.json` with a random device key. The bridge
listens on port 8790. If the device can't find it, check that the PC firewall
allows that port on your LAN.

### 2. Firmware (one project, two targets)

Both devices build from the same code in `firmware/`. The per-target settings
are in `firmware/include/board.h`:

| | `cores3` | `tab5` |
|---|---|---|
| Device | CoreS3 in the Stack-chan body | M5Stack Tab5 (ESP32-P4) |
| Screen | 320x240 | 1280x720 X desktop: root weave, a twm-style "xeyes" window that roams to where he looks, the rest of the UI 3x on top, and a "claude tasks" window |
| Head | servos | none (hops still show the sign and the laptop pop-up) |
| Extra | | the RetroKM desk map (`lib/rkm_panel`, a link into MULTI KM), OTA updates |
| WiFi | built in | ESP32-C6 over SDIO (`WiFi.setPins`) |
| Platform | espressif32 6.13 (Arduino 2) | pioarduino 55.03.311 (Arduino 3.3) |

```
pio run -e cores3 -t upload     # Stack-chan
pio run -e tab5 -t upload       # Tab5 over USB
pio run -e tab5-ota -t upload   # Tab5 over WiFi (lilc-tab5.local)
pio run -e cores3-ota -t upload # Stack-chan over WiFi (lilc-stackchan.local)
pio run -e cores3 -e tab5       # build both after a change
```

Both talk to the same bridge, share one Claude conversation, and use the same
`secrets.h`.


1. Edit `firmware/include/secrets.h`: put in your WiFi name and password.
   The bridge IP (192.168.1.166) and key are already filled in. If the PC's IP
   changes, update `BRIDGE_HOST`.
2. Check `firmware/include/config.h` for your servos:
   - `SERVO_TYPE 3` (default): official M5Stack StackChan body. SCS0009 servos
     on G6/G7 at 1 Mbaud (yaw ID 1, pitch ID 2). Servo power is switched on through
     the body's PY32 IO expander (I2C 0x6F), and the centre calibration M5Stack's
     firmware saved in NVS is reused. Values come from M5Stack's StackChan-BSP.
   - `SERVO_TYPE 1`: SG90 PWM servos on Port C (pan G18, tilt G17), community boards.
   - `SERVO_TYPE 2`: SCS0009 on a community board.
   - `SERVO_TYPE 0`: face only.
3. Plug in the CoreS3 by USB-C and run:

```
cd "LIL C/firmware"
~/.platformio/penv/bin/pio run -t upload
```

## Tested / not tested

- Bridge: tested on this PC. Spoken question -> whisper -> Claude -> piper audio
  worked, follow-ups kept context, and the task list read real sessions.
- Firmware: working on the user's official M5Stack StackChan (2026-10-05). It hears,
  answers out loud, shows tasks and moves its head.

## Files

- `bridge/lilc_bridge.py`: HTTP bridge (stdlib + faster-whisper + piper)
- `bridge/lilc_presence.py`: where lil' C is, wandering, summons
- `bridge/lilc_desk.py`: link to the RetroKM hub (desk layout, KVM commands, his window on other machines)
- `bridge/lilc_popup.py`: his window on the laptop
- `firmware/src/main.cpp`: face, push-to-talk, tasks screen, network task
- `firmware/src/head.h`: servo driver (PWM or SCS0009)

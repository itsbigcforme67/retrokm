# lil' C: status (2026-10-10)

Read this first in a new session. README.md has the full feature and setup notes.

## What it is

A desk buddy that talks to Claude on the user's subscription, built as one
project with two firmware targets, plus a bridge on the PC:

- `bridge/`: Python bridge on the laptop (192.168.1.166, port 8790). Start it with
  `sh bridge/run.sh` (it does not auto-start; a session restart kills it). The
  user asks "start his bridge" and Claude runs it in a terminal tab.
- `firmware/`: PlatformIO, envs `cores3` (Stack-chan, 192.168.1.177) and `tab5`
  (M5Stack Tab5, 192.168.1.140). Build both after any change.

## Working on hardware

- Voice Q&A: push-to-talk, then whisper tiny.en, then a persistent `claude`
  stream-json process with a warm spare, then sentence-by-sentence Speak & Spell
  TTS (TMS5100 emulation in C).
- Speak & Spell VFD caption, xeyes face, task list, gallery of SendUserFile
  pictures/videos, read-only project access, and process checks.
- Hops: Claude's `[[hop:laptop]]` tag makes the head turn (yaw 115), a BRB sign
  show, and an xeyes pop-up dart across both laptop monitors.
- Official M5Stack StackChan body: SERVO_TYPE 3 (SCS0009 on G6/G7, PY32
  expander powers the servos).
- Tab5: an X desktop with a roaming twm "xeyes" window and a "claude tasks"
  window, at ~18 fps.
- Gaze: eyes decoupled from the neck (saccade, then the neck follows while the
  eyes counter-rotate); 63% of looks are glances; nearby targets with a centre
  pull; 8% of the time he tracks a floating object (the Tab5 shows an X cursor).

## New on 2026-10-10: one lil' C, wandering, desk control (flashed on both)

Built and tested on the PC (fake Claude + a test hub on ports 2595x, the X11
agent on :0, the Windows agent under Wine); not yet on the devices.

- **One lil' C** (`bridge/lilc_presence.py`): he is on the Stack-chan, the
  Tab5, or visiting a computer. Devices poll `GET /api/presence` (header
  `X-LilC-Dev: stackchan|tab5`); away, they show "BACK SOON!" and where he
  is; tap = `POST /api/summon` (hold = summon + talk). Wanders by weights in
  config.json `"presence"`; leaves a computer when it's typed on; pinned 5
  min after a summon. Laptop visits use `lilc_popup.py` (now with a calm
  mode and click = summon); other machines use the RetroKM agents' buddy
  window through the hub.
- **Desk control** (`bridge/lilc_desk.py`): bridge connects to the hub's
  panel port 24852. Each question gets a `[Note from the bridge: ...]` with
  the desk; Claude replies with `[[kvm:show M MON]]`, `[[kvm:blank MON]]`,
  `[[kvm:keyboard M]]`. Monitors numbered left to right, captures last.
- **Tab5 firmware** now embeds the RetroKM desk map (`firmware/lib/rkm_panel`
  is a symlink to `../../MULTI KM/panel/lib/rkm_panel`): bottom bar middle
  "desk (KVM)" opens it, its "lil' C" button returns. Tab5 has ArduinoOTA
  now (hostname lilc-tab5): `pio run -e tab5-ota -t upload` (IP .140; the
  first one goes over the current RetroKM panel firmware's OTA).
- Old pieces still fine without the new hub: KVM chat works with today's
  hub; visits to other machines need the new hub + agents (MULTI KM STATUS).

Both devices flashed 2026-10-10 and the new bridge is running; summons and
a hop to the laptop worked on hardware. Both now update over WiFi:
`pio run -e tab5-ota -t upload` (lilc-tab5.local) and
`pio run -e cores3-ota -t upload` (lilc-stackchan.local). DHCP moved them:
Tab5 .202, Stack-chan .204 on 2026-10-10.

Trips (2026-10-10, flashed): the place he leaves turns its neck to face
where he's going first, then his window slides off the top; the place he's
going shows "HOLD ON, COMING!" for `travel_s` (2.8 s, bridge sends
`eta_ms`/`from`), then his xeyes window slides down from the top with the
eyes already in it. The Stack-chan's face is now a twm "xeyes" window too
(covers the weave while he's there). Hops use the same neck-first/slide.

### Still to deploy
3. Hub + agents: see MULTI KM docs/STATUS.md.
4. Tune `homes.tab5.yaw` (-70 is a guess for where the Stack-chan turns to
   look at the Tab5).

## Gotchas

- Checking device connections: `ss -tn state all '( sport = :8790 )' | awk 'NR>1{print $5}'`.
  Use column **$5** (the peer); $4 is the PC itself.
- Device IPs move (DHCP); use the .local names.
- Tab5 builds and flashes sometimes fail once with a "Tool Manager" error; just
  run them again.
- Tab5 WiFi takes a few seconds (ESP-Hosted: C6 slave 1.4.1, host 2.12.11). It
  warns but works. Don't update the C6 without asking.
- Device serial: `[lilc]` lines (WiFi status; Tab5 fps/compose/push timing).

## Ideas the user has queued (later)

- Take machine positions (head yaw) from MULTI KM's monitor layout; turn
  to face the arm box (right of the main monitor).
- Summon from a classic Windows taskbar icon: done in the Windows agent
  (click the RetroKM tray icon), needs the new rkm-win32.exe on the Dell.
- Webcam colour-dot tracking to aim at things ("where's my PS2?").
- Pull files straight from git repos.
- Tab5 perf: only redraw the pupils when the window isn't moving.
- Tunables: NECK_DELAY, neck speed, glance odds, ranges (config.h,
  SERVO_X_RANGE 50 / Y 15).

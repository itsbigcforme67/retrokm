#!/usr/bin/env python3
"""lil' C "hops over" to this computer: a small xeyes-style window of his face
slides up from the bottom corner of the screen, then darts around all the
monitors (crossing between them where they line up), stopping to look around
or at the mouse. When the bridge writes "bye" (or closes stdin) he darts back
to the corner and slides away.

Plain X11 through python-xlib, black and white on the X root weave, the way
xeyes looked on a bare X server.

usage: lilc_popup.py X Y W H "label" ["x,y,w,h;x,y,w,h..."] [calm|busy]
       (the monitor to appear on, then every monitor he may visit; "calm"
       when he is just visiting, "busy" when he is checking on something)

Clicking the window prints "click" (the bridge takes that as a summons).
"""
import math
import random
import select
import sys
import time

from Xlib import X, display

MX, MY, MW, MH = (int(v) for v in sys.argv[1:5])
LABEL = sys.argv[5] if len(sys.argv) > 5 else "lil' C is checking..."
MONITORS = [tuple(int(v) for v in m.split(",")) for m in sys.argv[6].split(";")
            if m.count(",") == 3] if len(sys.argv) > 6 else []
MONITORS = MONITORS or [(MX, MY, MW, MH)]
CALM = len(sys.argv) > 7 and sys.argv[7] == "calm"

WW, WH = 260, 176      # window
FACE_H = 148           # eyes area; the rest is the label strip
MARGIN = 28
X0 = MX + MW - WW - MARGIN
Y_SHOWN = MY + MH - WH - 56
Y_HIDDEN = MY + MH + 4

d = display.Display()
scr = d.screen()
root = scr.root
black, white = scr.black_pixel, scr.white_pixel

win = root.create_window(X0, Y_HIDDEN, WW, WH, 2, scr.root_depth, X.InputOutput,
                         X.CopyFromParent, override_redirect=True,
                         background_pixel=white, border_pixel=black,
                         event_mask=X.ExposureMask | X.ButtonPressMask)
win.set_wm_name("lil' C")
buf = win.create_pixmap(WW, WH, scr.root_depth)

# X's root_weave as a 4x4 tile
weave = win.create_pixmap(4, 4, scr.root_depth)
gc = win.create_gc(foreground=white, background=black)
weave.fill_rectangle(gc, 0, 0, 4, 4)
gc.change(foreground=black)
for y, row in enumerate((0x07, 0x0D, 0x0B, 0x0E)):
    for x in range(4):
        if row >> x & 1:
            weave.point(gc, x, y)
tile_gc = win.create_gc(fill_style=X.FillTiled, tile=weave)
font = d.open_font("fixed")
text_gc = win.create_gc(foreground=white, background=black, font=font)

EYES = [(70, 74), (190, 74)]
RX, RY = 54, 66


def ellipse(gc, cx, cy, rx, ry):
    buf.fill_arc(gc, int(cx - rx), int(cy - ry), int(rx * 2), int(ry * 2), 0, 360 * 64)


def draw(gaze=None, pointer=None):
    """gaze: (-1..1, -1..1) both eyes together; pointer: root coordinates."""
    buf.fill_rectangle(tile_gc, 0, 0, WW, FACE_H)
    rim = int(RX * 0.16)
    prx = RX * 0.24
    pry = prx * 1.2
    wx, wy = pos
    for cx, cy in EYES:
        gc.change(foreground=black)
        ellipse(gc, cx, cy, RX, RY)
        gc.change(foreground=white)
        ellipse(gc, cx, cy, RX - rim, RY - rim)
        ax, ay = RX - rim - prx - 3, RY - rim - pry - 3
        if pointer:
            dx, dy = pointer[0] - (wx + cx), pointer[1] - (wy + cy)
        else:
            dx, dy = gaze[0] * ax, gaze[1] * ay
        k = math.sqrt((dx / ax) ** 2 + (dy / ay) ** 2)
        if k > 1:
            dx, dy = dx / k, dy / k
        gc.change(foreground=black)
        ellipse(gc, cx + dx, cy + dy, prx, pry)
    gc.change(foreground=black)
    buf.fill_rectangle(gc, 0, FACE_H, WW, WH - FACE_H)
    buf.image_text(text_gc, 10, FACE_H + 18, LABEL[:40].encode())
    win.copy_area(gc, buf, 0, 0, WW, WH, 0, 0)
    d.flush()


pos = [X0, Y_HIDDEN]


def move(x, y):
    pos[0], pos[1] = int(x), int(y)
    win.configure(x=pos[0], y=pos[1], stack_mode=X.Above)


def ease(t):
    return 1 - (1 - t) ** 3


def smooth(t):  # ease in and out, for darts
    return t * t * (3 - 2 * t)


def monitor_of(x, y):
    for m in MONITORS:
        if m[0] <= x < m[0] + m[2] and m[1] <= y < m[1] + m[3]:
            return m
    return MONITORS[0]


def random_spot(m):
    mx, my, mw, mh = m
    return (random.randint(mx + 20, max(mx + 20, mx + mw - WW - 20)),
            random.randint(my + 20, max(my + 20, my + mh - WH - 60)))


def plan_dart(start, goal):
    """Waypoints from start to goal; going to another monitor, pass through
    the shared edge at a height (or x) both monitors have, so the window never
    flies through the dead space outside the screens."""
    a, b = monitor_of(*start), monitor_of(*goal)
    if a == b:
        return [goal]
    # side by side: cross the vertical edge inside the overlapping rows
    lo, hi = max(a[1], b[1]) + 10, min(a[1] + a[3], b[1] + b[3]) - WH - 10
    if hi > lo and (a[0] + a[2] == b[0] or b[0] + b[2] == a[0]):
        edge = b[0] if a[0] < b[0] else a[0]
        y = min(max(start[1], lo), hi)
        return [(start[0], y), (edge - WW // 2, y), goal]
    # stacked: cross the horizontal edge inside the overlapping columns
    lo, hi = max(a[0], b[0]) + 10, min(a[0] + a[2], b[0] + b[2]) - WW - 10
    if hi > lo:
        edge = b[1] if a[1] < b[1] else a[1]
        x = min(max(start[0], lo), hi)
        return [(x, start[1]), (x, edge - WH // 2), goal]
    return [random_spot(a)]  # monitors don't touch: stay on this one


# Where to look while "arriving": glance around the screen, then up at it
LOOKS = [(0.0, 0.0, 0.3), (-0.9, -0.2, 0.5), (0.9, -0.2, 0.5), (0.0, -0.9, 0.4)]

win.map()
gaze = [0.0, 0.0]
t0 = time.time()
state, state_t = "enter", t0
look_i, look_t = 0, t0
path, seg_from, seg_t, seg_dur = [], (X0, Y_SHOWN), t0, 0.3
pause_until, follow_mouse = 0.0, False
stare = (0.0, 0.0)
leaving = False
while True:
    now = time.time()
    if state == "enter":  # slide up from the bottom corner, glance around
        move(X0, Y_HIDDEN + (Y_SHOWN - Y_HIDDEN) * ease(min(1.0, (now - t0) / 0.5)))
        tx, ty, dur = LOOKS[look_i]
        gaze[0] += (tx - gaze[0]) * 0.35
        gaze[1] += (ty - gaze[1]) * 0.35
        draw(gaze=gaze)
        if now - look_t > dur:
            look_i, look_t = look_i + 1, now
            if look_i >= len(LOOKS):
                state, pause_until = "pause", now
    elif state == "pause":  # look around this part of the screen
        if follow_mouse:
            p = root.query_pointer()
            draw(pointer=(p.root_x, p.root_y))
        else:
            if random.random() < 0.04:  # pick somewhere new to stare at
                stare = (random.uniform(-1, 1), random.uniform(-1, 1))
            gaze[0] += (stare[0] - gaze[0]) * 0.3
            gaze[1] += (stare[1] - gaze[1]) * 0.3
            draw(gaze=gaze)
        if now >= pause_until:
            if leaving:
                state, state_t = "exit", now
                continue
            here = monitor_of(pos[0] + WW // 2, pos[1] + WH // 2)
            others = [m for m in MONITORS if m != here]
            dest_mon = random.choice(others) if others and random.random() < (0.2 if CALM else 0.4) else here
            path = plan_dart(tuple(pos), random_spot(dest_mon))
            state, seg_from, seg_t = "dart", tuple(pos), now
            seg_dur = max(0.25, math.dist(seg_from, path[0]) / 2600)
    elif state == "dart":  # zip to the next waypoint, eyes leading
        tgt = path[0]
        f = min(1.0, (now - seg_t) / seg_dur)
        k = smooth(f)
        move(seg_from[0] + (tgt[0] - seg_from[0]) * k, seg_from[1] + (tgt[1] - seg_from[1]) * k)
        dx, dy = tgt[0] - seg_from[0], tgt[1] - seg_from[1]
        n = math.hypot(dx, dy) or 1
        gaze[0] += (dx / n - gaze[0]) * 0.5
        gaze[1] += (dy / n - gaze[1]) * 0.5
        draw(gaze=gaze)
        if f >= 1:
            path.pop(0)
            if path:
                seg_from, seg_t = tuple(pos), now
                seg_dur = max(0.15, math.dist(seg_from, path[0]) / 2600)
            else:
                state = "pause"
                follow_mouse = random.random() < (0.15 if CALM else 0.3)
                pause_until = now + (0.3 if leaving else
                                     random.uniform(2.0, 7.0) if CALM else random.uniform(0.7, 1.8))
    elif state == "exit":  # back at the corner: slide away
        f = min(1.0, (now - state_t) / 0.45)
        move(X0, Y_SHOWN + (Y_HIDDEN - Y_SHOWN) * f * f)
        draw(gaze=(0.0, 0.9))  # looking down, back towards the desk
        if f >= 1:
            break
    while d.pending_events():
        if d.next_event().type == X.ButtonPress:
            print("click", flush=True)
    # "bye" (or the bridge going away) sends lil' C home
    if not leaving and select.select([sys.stdin], [], [], 0)[0]:
        line = sys.stdin.readline()
        if not line or line.strip() == "bye":
            leaving = True
            path = plan_dart(tuple(pos), (X0, Y_SHOWN))
            state, seg_from, seg_t = "dart", tuple(pos), now
            seg_dur = max(0.25, math.dist(seg_from, path[0]) / 2600)
    time.sleep(1 / 40)

win.destroy()
d.flush()

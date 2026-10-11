/* Game pad capture through evdev: an Xbox-style controller plugged into or
 * paired with the hub's machine (xpad over USB, or hid-microsoft /
 * hid-generic over Bluetooth).  Its state is normalised to the wire format
 * (RKM_PAD) and handed to main.c, which sends it to the screen that has the
 * keyboard.  Rumble from that screen is played back on the pad. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include "hub.h"

#define BIT(arr, n) ((arr)[(n) / 8] & (1 << ((n) % 8)))

static int fd = -1;
static char path[64], name[128];
static int grabbed;
static int xpad_axes;                    /* ABS_RX/RY present: Z/RZ are the triggers */
static struct input_absinfo ai[ABS_MAX + 1];
static PadState st, sent;
static int dirty;
static int ff_id = -1;
static long long last_scan;

static void axis(int code, int v);

static void close_pad(const char *why)
{
    if (fd < 0) return;
    logmsg("pad: %s %s (%s)", why, path, name);
    close(fd);
    fd = -1;
    grabbed = 0;
    ff_id = -1;
    memset(&st, 0, sizeof st);
    if (memcmp(&st, &sent, sizeof st)) { sent = st; hub_pad(&st); }   /* let go of everything */
}

static int try_open(const char *p, int filter)
{
    unsigned char keybits[KEY_MAX / 8 + 1], absbits[ABS_MAX / 8 + 1];
    int f, a;

    f = open(p, O_RDWR | O_NONBLOCK | O_CLOEXEC);    /* write: force feedback */
    if (f < 0) f = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (f < 0) return -1;
    memset(keybits, 0, sizeof keybits);
    memset(absbits, 0, sizeof absbits);
    name[0] = 0;
    ioctl(f, EVIOCGNAME(sizeof name - 1), name);
    ioctl(f, EVIOCGBIT(EV_KEY, sizeof keybits), keybits);
    ioctl(f, EVIOCGBIT(EV_ABS, sizeof absbits), absbits);
    if (!strncmp(name, "RetroKM", 7) ||
        (filter && !(BIT(keybits, BTN_SOUTH) && BIT(absbits, ABS_X) && BIT(absbits, ABS_Y)))) {
        close(f);
        return -1;
    }
    fd = f;
    snprintf(path, sizeof path, "%s", p);
    xpad_axes = BIT(absbits, ABS_RX) && BIT(absbits, ABS_RY);
    memset(ai, 0, sizeof ai);
    for (a = 0; a <= ABS_MAX; a++)
        if (BIT(absbits, a)) ioctl(f, EVIOCGABS(a), &ai[a]);
    memset(&st, 0, sizeof st);
    for (a = 0; a <= ABS_MAX; a++)               /* where the sticks are now */
        if (BIT(absbits, a)) axis(a, ai[a].value);
    logmsg("pad: using %s (%s)", path, name);
    return 0;
}

static void scan(void)
{
    char p[64];
    int i;
    if (cfg.pad[0] && strcmp(cfg.pad, "auto")) { try_open(cfg.pad, 0); return; }
    for (i = 0; i < 64 && fd < 0; i++) {
        snprintf(p, sizeof p, "/dev/input/event%d", i);
        try_open(p, 1);
    }
}

/* -32768..32767 from the axis' own range; flip = make "up" positive */
static short stick(int code, int v, int flip)
{
    const struct input_absinfo *a = &ai[code];
    double mid = (a->minimum + a->maximum) / 2.0, half = (a->maximum - a->minimum) / 2.0, r;
    if (half <= 0) return 0;
    r = (v - mid) / half;
    if (flip) r = -r;
    if (r > 1) r = 1;
    if (r < -1) r = -1;
    return (short)(r >= 0 ? r * 32767 : r * 32768);
}

static unsigned char trigger(int code, int v)
{
    const struct input_absinfo *a = &ai[code];
    int span = a->maximum - a->minimum;
    if (span <= 0) return v ? 255 : 0;
    v = (v - a->minimum) * 255 / span;
    return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static void set_btn(unsigned bit, int down)
{
    if (down) st.buttons |= bit;
    else st.buttons &= ~bit;
}

static void key(int code, int v)
{
    switch (code) {
    case BTN_SOUTH: set_btn(RKM_PAD_A, v); break;
    case BTN_EAST: set_btn(RKM_PAD_B, v); break;
    case BTN_NORTH: set_btn(RKM_PAD_X, v); break;    /* Linux: BTN_X == BTN_NORTH */
    case BTN_WEST: set_btn(RKM_PAD_Y, v); break;
    case BTN_TL: set_btn(RKM_PAD_LB, v); break;
    case BTN_TR: set_btn(RKM_PAD_RB, v); break;
    case BTN_SELECT: set_btn(RKM_PAD_BACK, v); break;
    case BTN_START: set_btn(RKM_PAD_START, v); break;
    case BTN_MODE: set_btn(RKM_PAD_GUIDE, v); break;
    case BTN_THUMBL: set_btn(RKM_PAD_LS, v); break;
    case BTN_THUMBR: set_btn(RKM_PAD_RS, v); break;
    case BTN_DPAD_UP: set_btn(RKM_PAD_UP, v); break;
    case BTN_DPAD_DOWN: set_btn(RKM_PAD_DOWN, v); break;
    case BTN_DPAD_LEFT: set_btn(RKM_PAD_LEFT, v); break;
    case BTN_DPAD_RIGHT: set_btn(RKM_PAD_RIGHT, v); break;
    case KEY_RECORD: set_btn(RKM_PAD_SHARE, v); break;   /* xpad's Share button */
    case BTN_TL2: st.lt = v ? 255 : 0; break;            /* digital triggers */
    case BTN_TR2: st.rt = v ? 255 : 0; break;
    default: return;
    }
}

static void axis(int code, int v)
{
    switch (code) {
    case ABS_X: st.lx = stick(code, v, 0); break;
    case ABS_Y: st.ly = stick(code, v, 1); break;
    case ABS_RX: st.rx = stick(code, v, 0); break;
    case ABS_RY: st.ry = stick(code, v, 1); break;
    case ABS_Z: if (xpad_axes) st.lt = trigger(code, v); else st.rx = stick(code, v, 0); break;
    case ABS_RZ: if (xpad_axes) st.rt = trigger(code, v); else st.ry = stick(code, v, 1); break;
    case ABS_BRAKE: st.lt = trigger(code, v); break;
    case ABS_GAS: st.rt = trigger(code, v); break;
    case ABS_HAT0X:
        set_btn(RKM_PAD_LEFT, v < 0);
        set_btn(RKM_PAD_RIGHT, v > 0);
        break;
    case ABS_HAT0Y:
        set_btn(RKM_PAD_UP, v < 0);
        set_btn(RKM_PAD_DOWN, v > 0);
        break;
    }
}

int pad_fd(void)
{
    return fd;
}

void pad_readable(void)
{
    struct input_event ev[64];
    int i, n;

    n = (int)read(fd, ev, sizeof ev);
    if (n < 0) {
        if (errno != EAGAIN && errno != EINTR) close_pad("lost");
        return;
    }
    n /= (int)sizeof ev[0];
    for (i = 0; i < n; i++) {
        if (ev[i].type == EV_KEY) { key(ev[i].code, ev[i].value); dirty = 1; }
        else if (ev[i].type == EV_ABS) { axis(ev[i].code, ev[i].value); dirty = 1; }
        else if (ev[i].type == EV_SYN && ev[i].code == SYN_REPORT && dirty) {
            dirty = 0;
            if (memcmp(&st, &sent, sizeof st)) {
                sent = st;
                hub_pad(&st);
            }
        }
    }
}

/* Grab it while another machine has it, so the hub's desktop does not see
 * the same presses; release it while the desktop is in charge. */
void pad_grab(int on)
{
    if (fd < 0 || on == grabbed) return;
    if (ioctl(fd, EVIOCGRAB, on) == 0) {
        grabbed = on;
        if (verbose) logmsg("pad: %s", on ? "grabbed" : "released");
    }
}

void pad_rumble(unsigned strong, unsigned weak)
{
    struct ff_effect e;
    struct input_event play;

    if (fd < 0) return;
    memset(&play, 0, sizeof play);
    play.type = EV_FF;
    if (!strong && !weak) {
        if (ff_id >= 0) {
            play.code = (unsigned short)ff_id;
            play.value = 0;
            if (write(fd, &play, sizeof play) < 0) { /* not fatal */ }
        }
        return;
    }
    memset(&e, 0, sizeof e);
    e.type = FF_RUMBLE;
    e.id = (short)ff_id;                         /* -1: a new effect */
    e.u.rumble.strong_magnitude = (unsigned short)strong;
    e.u.rumble.weak_magnitude = (unsigned short)weak;
    e.replay.length = 0;                         /* until told otherwise */
    if (ioctl(fd, EVIOCSFF, &e) < 0) {
        if (ff_id >= 0) { ff_id = -1; pad_rumble(strong, weak); }   /* stale id: once more */
        return;
    }
    ff_id = e.id;
    play.code = (unsigned short)ff_id;
    play.value = 1;
    if (write(fd, &play, sizeof play) < 0) { /* not fatal */ }
}

void pad_tick(void)
{
    long long now = now_ms();
    if (fd >= 0 || !strcasecmp(cfg.pad, "none") || now - last_scan < 2000) return;
    last_scan = now;
    scan();
}

const char *pad_name(void)
{
    return fd >= 0 ? name : NULL;
}

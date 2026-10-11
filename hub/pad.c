/* Game pads through evdev: Xbox-style controllers plugged into or paired
 * with the hub's machine (xpad over USB, or hid-microsoft / hid-generic over
 * Bluetooth).  Each one is an Accessory, known by a stable id so its links
 * survive unplugging.  Its state is normalised to the wire format (RKM_PAD)
 * and handed to main.c, which sends it where it is linked; rumble from
 * there is played back on it. */
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

typedef struct {
    int fd;
    char path[64];
    Accessory *acc;
    int grabbed;
    int xpad_axes;               /* ABS_RX/RY present: Z/RZ are the triggers */
    struct input_absinfo ai[ABS_MAX + 1];
    PadState st;                 /* being built from events, until SYN_REPORT */
    int dirty;
    int ff_id;
} Pad;

static Pad pads[MAX_PADS];
static int npads;
static long long last_scan;

static void axis(Pad *d, int code, int v);

static void drop(Pad *d, const char *why)
{
    Accessory *a = d->acc;
    logmsg("pad: %s %s (%s)", why, d->path, a->label);
    close(d->fd);
    *d = pads[--npads];
    a->present = 0;
    memset(&a->st, 0, sizeof a->st);
    hub_pad(a);                                  /* let go of everything */
}

static int open_already(const char *path)
{
    int i;
    for (i = 0; i < npads; i++)
        if (!strcmp(pads[i].path, path)) return 1;
    return 0;
}

static unsigned fnv(const char *s)
{
    unsigned h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static int try_open(const char *p, int filter)
{
    unsigned char keybits[KEY_MAX / 8 + 1], absbits[ABS_MAX / 8 + 1];
    char name[128] = "", uniq[64] = "", phys[64] = "", id[32];
    struct input_id iid;
    Pad *d;
    int f, a;

    if (npads >= MAX_PADS || open_already(p)) return -1;
    f = open(p, O_RDWR | O_NONBLOCK | O_CLOEXEC);    /* write: force feedback */
    if (f < 0) f = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (f < 0) return -1;
    memset(keybits, 0, sizeof keybits);
    memset(absbits, 0, sizeof absbits);
    memset(&iid, 0, sizeof iid);
    ioctl(f, EVIOCGNAME(sizeof name - 1), name);
    ioctl(f, EVIOCGUNIQ(sizeof uniq - 1), uniq);
    ioctl(f, EVIOCGPHYS(sizeof phys - 1), phys);
    ioctl(f, EVIOCGID, &iid);
    ioctl(f, EVIOCGBIT(EV_KEY, sizeof keybits), keybits);
    ioctl(f, EVIOCGBIT(EV_ABS, sizeof absbits), absbits);
    /* never our own output: the Xbox dongle plugged into this machine for
     * testing shows up as an original Xbox pad (045E:0289) */
    if (!strncmp(name, "RetroKM", 7) || (iid.vendor == 0x045E && iid.product == 0x0289) ||
        (filter && !(BIT(keybits, BTN_SOUTH) && BIT(absbits, ABS_X) && BIT(absbits, ABS_Y)))) {
        close(f);
        return -1;
    }
    /* the same pad gets the same id next time: Bluetooth address, else the port */
    snprintf(id, sizeof id, "pad-%04x%04x-%06x", iid.vendor, iid.product,
             fnv(uniq[0] ? uniq : phys) & 0xFFFFFF);
    d = &pads[npads];
    memset(d, 0, sizeof *d);
    d->fd = f;
    d->ff_id = -1;
    snprintf(d->path, sizeof d->path, "%s", p);
    d->acc = acc_add(id, ACC_PAD, name);
    if (!d->acc) { close(f); return -1; }
    snprintf(d->acc->about, sizeof d->acc->about, "%s on the hub, %s",
             iid.bustype == BUS_BLUETOOTH ? "Bluetooth" : iid.bustype == BUS_USB ? "USB" : "input device", p);
    d->xpad_axes = BIT(absbits, ABS_RX) && BIT(absbits, ABS_RY);
    for (a = 0; a <= ABS_MAX; a++)
        if (BIT(absbits, a)) ioctl(f, EVIOCGABS(a), &d->ai[a]);
    for (a = 0; a <= ABS_MAX; a++)               /* where the sticks are now */
        if (BIT(absbits, a)) axis(d, a, d->ai[a].value);
    npads++;
    d->acc->present = 1;
    d->acc->st = d->st;
    logmsg("pad: using %s (%s) as %s", p, name, id);
    return 0;
}

static void scan(void)
{
    char p[64];
    int i;
    if (cfg.pad[0] && strcmp(cfg.pad, "auto")) { try_open(cfg.pad, 0); return; }
    for (i = 0; i < 64; i++) {
        snprintf(p, sizeof p, "/dev/input/event%d", i);
        try_open(p, 1);
    }
}

/* -32768..32767 from the axis' own range; flip = make "up" positive */
static short stick(const Pad *d, int code, int v, int flip)
{
    const struct input_absinfo *a = &d->ai[code];
    double mid = (a->minimum + a->maximum) / 2.0, half = (a->maximum - a->minimum) / 2.0, r;
    if (half <= 0) return 0;
    r = (v - mid) / half;
    if (flip) r = -r;
    if (r > 1) r = 1;
    if (r < -1) r = -1;
    return (short)(r >= 0 ? r * 32767 : r * 32768);
}

static unsigned char trigger(const Pad *d, int code, int v)
{
    const struct input_absinfo *a = &d->ai[code];
    int span = a->maximum - a->minimum;
    if (span <= 0) return v ? 255 : 0;
    v = (v - a->minimum) * 255 / span;
    return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static void set_btn(Pad *d, unsigned bit, int down)
{
    if (down) d->st.buttons |= bit;
    else d->st.buttons &= ~bit;
}

static void key(Pad *d, int code, int v)
{
    switch (code) {
    case BTN_SOUTH: set_btn(d, RKM_PAD_A, v); break;
    case BTN_EAST: set_btn(d, RKM_PAD_B, v); break;
    case BTN_NORTH: set_btn(d, RKM_PAD_X, v); break;    /* Linux: BTN_X == BTN_NORTH */
    case BTN_WEST: set_btn(d, RKM_PAD_Y, v); break;
    case BTN_TL: set_btn(d, RKM_PAD_LB, v); break;
    case BTN_TR: set_btn(d, RKM_PAD_RB, v); break;
    case BTN_SELECT: set_btn(d, RKM_PAD_BACK, v); break;
    case BTN_START: set_btn(d, RKM_PAD_START, v); break;
    case BTN_MODE: set_btn(d, RKM_PAD_GUIDE, v); break;
    case BTN_THUMBL: set_btn(d, RKM_PAD_LS, v); break;
    case BTN_THUMBR: set_btn(d, RKM_PAD_RS, v); break;
    case BTN_DPAD_UP: set_btn(d, RKM_PAD_UP, v); break;
    case BTN_DPAD_DOWN: set_btn(d, RKM_PAD_DOWN, v); break;
    case BTN_DPAD_LEFT: set_btn(d, RKM_PAD_LEFT, v); break;
    case BTN_DPAD_RIGHT: set_btn(d, RKM_PAD_RIGHT, v); break;
    case KEY_RECORD: set_btn(d, RKM_PAD_SHARE, v); break;   /* xpad's Share button */
    case BTN_TL2: d->st.lt = v ? 255 : 0; break;            /* digital triggers */
    case BTN_TR2: d->st.rt = v ? 255 : 0; break;
    default: return;
    }
}

static void axis(Pad *d, int code, int v)
{
    switch (code) {
    case ABS_X: d->st.lx = stick(d, code, v, 0); break;
    case ABS_Y: d->st.ly = stick(d, code, v, 1); break;
    case ABS_RX: d->st.rx = stick(d, code, v, 0); break;
    case ABS_RY: d->st.ry = stick(d, code, v, 1); break;
    case ABS_Z: if (d->xpad_axes) d->st.lt = trigger(d, code, v); else d->st.rx = stick(d, code, v, 0); break;
    case ABS_RZ: if (d->xpad_axes) d->st.rt = trigger(d, code, v); else d->st.ry = stick(d, code, v, 1); break;
    case ABS_BRAKE: d->st.lt = trigger(d, code, v); break;
    case ABS_GAS: d->st.rt = trigger(d, code, v); break;
    case ABS_HAT0X:
        set_btn(d, RKM_PAD_LEFT, v < 0);
        set_btn(d, RKM_PAD_RIGHT, v > 0);
        break;
    case ABS_HAT0Y:
        set_btn(d, RKM_PAD_UP, v < 0);
        set_btn(d, RKM_PAD_DOWN, v > 0);
        break;
    }
}

int pad_fds(int *fds, int max)
{
    int i;
    for (i = 0; i < npads && i < max; i++) fds[i] = pads[i].fd;
    return i;
}

void pad_readable(int fd)
{
    struct input_event ev[64];
    Pad *d = NULL;
    int i, n;

    for (i = 0; i < npads; i++)
        if (pads[i].fd == fd) d = &pads[i];
    if (!d) return;
    n = (int)read(fd, ev, sizeof ev);
    if (n < 0) {
        if (errno != EAGAIN && errno != EINTR) drop(d, "lost");
        return;
    }
    n /= (int)sizeof ev[0];
    for (i = 0; i < n; i++) {
        if (ev[i].type == EV_KEY) { key(d, ev[i].code, ev[i].value); d->dirty = 1; }
        else if (ev[i].type == EV_ABS) { axis(d, ev[i].code, ev[i].value); d->dirty = 1; }
        else if (ev[i].type == EV_SYN && ev[i].code == SYN_REPORT && d->dirty) {
            d->dirty = 0;
            if (memcmp(&d->st, &d->acc->st, sizeof d->st)) {
                d->acc->st = d->st;
                hub_pad(d->acc);
            }
        }
    }
}

void pad_rumble(Accessory *a, unsigned strong, unsigned weak)
{
    struct ff_effect e;
    struct input_event play;
    Pad *d = NULL;
    int i;

    for (i = 0; i < npads; i++)
        if (pads[i].acc == a) d = &pads[i];
    if (!d) return;
    memset(&play, 0, sizeof play);
    play.type = EV_FF;
    if (!strong && !weak) {
        if (d->ff_id >= 0) {
            play.code = (unsigned short)d->ff_id;
            if (write(d->fd, &play, sizeof play) < 0) { /* not fatal */ }
        }
        return;
    }
    memset(&e, 0, sizeof e);
    e.type = FF_RUMBLE;
    e.id = (short)d->ff_id;                      /* -1: a new effect */
    e.u.rumble.strong_magnitude = (unsigned short)strong;
    e.u.rumble.weak_magnitude = (unsigned short)weak;
    e.replay.length = 0;                         /* until told otherwise */
    if (ioctl(d->fd, EVIOCSFF, &e) < 0) {
        if (d->ff_id >= 0) { d->ff_id = -1; pad_rumble(a, strong, weak); }   /* stale id: once more */
        return;
    }
    d->ff_id = e.id;
    play.code = (unsigned short)d->ff_id;
    play.value = 1;
    if (write(d->fd, &play, sizeof play) < 0) { /* not fatal */ }
}

/* Grab each pad while only other machines get it, so the hub's desktop
 * does not see the same presses; leave it to the desktop when that is a
 * target (other targets still get copies: reading does not need the grab). */
void pad_tick(void)
{
    long long now = now_ms();
    int i;

    for (i = 0; i < npads; i++) {
        Pad *d = &pads[i];
        int want = !hub_pad_on_desktop(d->acc);
        if (want != d->grabbed && ioctl(d->fd, EVIOCGRAB, want) == 0) {
            d->grabbed = want;
            if (verbose) logmsg("pad: %s %s", want ? "grabbed" : "released", d->path);
        }
    }
    if (!strcasecmp(cfg.pad, "none") || now - last_scan < 2000) return;
    last_scan = now;
    scan();
}

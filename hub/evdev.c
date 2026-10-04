/* Physical keyboard and mouse capture through evdev.  Devices are grabbed
 * exclusively so the hub alone decides where each event goes; that is also
 * what makes the hub work on a headless box with no desktop at all. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include "hub.h"

typedef struct {
    int fd;
    char path[128];
    int grabbed;
    int dx, dy;
} Dev;

static Dev devs[MAX_DEVICES];
static int ndevs;
static long long last_scan;

#define BIT(arr, n) ((arr)[(n) / 8] & (1 << ((n) % 8)))

static int already_open(const char *path)
{
    int i;
    for (i = 0; i < ndevs; i++)
        if (!strcmp(devs[i].path, path)) return 1;
    return 0;
}

static int try_open(const char *path, int filter)
{
    unsigned char evbits[EV_MAX / 8 + 1], keybits[KEY_MAX / 8 + 1], relbits[REL_MAX / 8 + 1];
    char name[128] = "";
    int fd;

    if (ndevs >= MAX_DEVICES || already_open(path)) return -1;
    fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;

    ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
    if (!strncmp(name, "RetroKM", 7)) { close(fd); return -1; }   /* our own output */

    if (filter) {
        int is_kbd, is_mouse;
        memset(evbits, 0, sizeof evbits);
        memset(keybits, 0, sizeof keybits);
        memset(relbits, 0, sizeof relbits);
        ioctl(fd, EVIOCGBIT(0, sizeof evbits), evbits);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits);
        ioctl(fd, EVIOCGBIT(EV_REL, sizeof relbits), relbits);
        is_kbd = BIT(keybits, KEY_A) && BIT(keybits, KEY_ENTER);
        is_mouse = BIT(evbits, EV_REL) && BIT(relbits, REL_X) && BIT(keybits, BTN_LEFT);
        /* touchpads, tablets and touchscreens stay with the desktop */
        if (BIT(evbits, EV_ABS) || !(is_kbd || is_mouse)) { close(fd); return -1; }
    }

    memset(&devs[ndevs], 0, sizeof devs[0]);
    devs[ndevs].fd = fd;
    strncpy(devs[ndevs].path, path, sizeof devs[0].path - 1);
    ndevs++;
    logmsg("input: using %s (%s)", path, name);
    return 0;
}

static void scan(void)
{
    char path[64];
    int i;
    for (i = 0; i < 64; i++) {
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        try_open(path, 1);
    }
}

/* Grab only once no key is held, so the Enter that launched the hub does
 * not get stuck down in the desktop. */
static void try_grab(Dev *d)
{
    unsigned char keys[KEY_MAX / 8 + 1];
    size_t i;

    memset(keys, 0, sizeof keys);
    if (ioctl(d->fd, EVIOCGKEY(sizeof keys), keys) < 0) return;
    for (i = 0; i < sizeof keys; i++)
        if (keys[i]) return;
    if (ioctl(d->fd, EVIOCGRAB, 1) == 0) {
        d->grabbed = 1;
        logmsg("input: grabbed %s", d->path);
    }
}

int input_init(void)
{
    int i;
    if (cfg.ndevices < 0) return 0;
    if (cfg.ndevices == 0) scan();
    for (i = 0; i < cfg.ndevices; i++)
        if (try_open(cfg.devices[i], 0) < 0)
            logmsg("input: cannot open %s: %s", cfg.devices[i], strerror(errno));
    if (ndevs == 0) logmsg("input: no keyboard or mouse found yet (need read access to /dev/input)");
    return 0;
}

int input_fds(int *fds, int max)
{
    int i;
    for (i = 0; i < ndevs && i < max; i++) fds[i] = devs[i].fd;
    return i;
}

static void drop(Dev *d)
{
    logmsg("input: lost %s", d->path);
    close(d->fd);
    *d = devs[--ndevs];
}

void input_readable(int fd)
{
    struct input_event ev[32];
    Dev *d = NULL;
    int i, n;

    for (i = 0; i < ndevs; i++)
        if (devs[i].fd == fd) d = &devs[i];
    if (!d) return;

    n = (int)read(fd, ev, sizeof ev);
    if (n < 0) {
        if (errno != EAGAIN && errno != EINTR) drop(d);
        return;
    }
    if (cfg.grab && !d->grabbed) return;       /* desktop still owns these */

    n /= (int)sizeof ev[0];
    for (i = 0; i < n; i++) {
        struct input_event *e = &ev[i];
        switch (e->type) {
        case EV_REL:
            if (e->code == REL_X) d->dx += e->value;
            else if (e->code == REL_Y) d->dy += e->value;
            else if (e->code == REL_WHEEL) hub_wheel(e->value * 120, 0);
            else if (e->code == REL_HWHEEL) hub_wheel(0, e->value * 120);
            break;
        case EV_KEY:
            if (d->dx || d->dy) { hub_motion(d->dx, d->dy); d->dx = d->dy = 0; }
            if (e->code >= BTN_LEFT && e->code <= BTN_EXTRA) {
                static const int map[] = { RKM_BTN_LEFT, RKM_BTN_RIGHT, RKM_BTN_MIDDLE,
                                           RKM_BTN_BACK, RKM_BTN_FWD };
                if (e->value < 2) hub_button(map[e->code - BTN_LEFT], e->value);
            } else {
                hub_key(e->code, evdev_to_hid(e->code), e->value);
            }
            break;
        case EV_SYN:
            if (e->code == SYN_REPORT && (d->dx || d->dy)) {
                hub_motion(d->dx, d->dy);
                d->dx = d->dy = 0;
            }
            break;
        }
    }
}

void input_tick(void)
{
    long long now = now_ms();
    int i;

    if (cfg.grab)
        for (i = 0; i < ndevs; i++)
            if (!devs[i].grabbed) try_grab(&devs[i]);
    if (cfg.ndevices == 0 && now - last_scan > 2000) {   /* hotplug */
        last_scan = now;
        scan();
    }
}

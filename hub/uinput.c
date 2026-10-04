/* Local injection through /dev/uinput.  Works the same under X11, Wayland
 * and the console because the kernel sees ordinary input devices.
 *
 * Three virtual devices: an absolute pointer (like a VM tablet), a relative
 * mouse, and a keyboard.  Used by the hub for its own screen and by the
 * standalone Linux agent. */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>
#include "hub.h"

#define ABS_RANGE 65535

static int fd_abs = -1, fd_rel = -1, fd_kbd = -1;
static unsigned char down[KEY_CNT / 8 + 1];

static void emit(int fd, int type, int code, int value)
{
    struct input_event ev;
    if (fd < 0) return;
    memset(&ev, 0, sizeof ev);
    ev.type = (unsigned short)type;
    ev.code = (unsigned short)code;
    ev.value = value;
    if (write(fd, &ev, sizeof ev) < 0) { /* device gone; nothing useful to do */ }
}

static int make(const char *base, const char *suffix, int kind)
{
    struct uinput_setup us;
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC), i;

    if (fd < 0) return -1;
    memset(&us, 0, sizeof us);
    snprintf(us.name, sizeof us.name, "%s %s", base, suffix);
    us.id.bustype = BUS_VIRTUAL;
    us.id.vendor = 0x524B;      /* "RK" */
    us.id.product = (unsigned short)(1 + kind);

    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    if (kind == 2) {                           /* keyboard */
        for (i = 1; i < BTN_MISC; i++) ioctl(fd, UI_SET_KEYBIT, i);
        for (i = KEY_OK; i < KEY_MAX; i++) ioctl(fd, UI_SET_KEYBIT, i);
    } else {
        for (i = BTN_LEFT; i <= BTN_TASK; i++) ioctl(fd, UI_SET_KEYBIT, i);
        ioctl(fd, UI_SET_EVBIT, EV_REL);
        ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
        ioctl(fd, UI_SET_RELBIT, REL_HWHEEL);
        if (kind == 0) {                       /* absolute pointer */
            struct uinput_abs_setup as;
            ioctl(fd, UI_SET_EVBIT, EV_ABS);
            for (i = 0; i < 2; i++) {
                memset(&as, 0, sizeof as);
                as.code = (unsigned short)(i ? ABS_Y : ABS_X);
                as.absinfo.maximum = ABS_RANGE;
                ioctl(fd, UI_SET_ABSBIT, as.code);
                ioctl(fd, UI_ABS_SETUP, &as);
            }
        } else {                               /* relative mouse */
            ioctl(fd, UI_SET_RELBIT, REL_X);
            ioctl(fd, UI_SET_RELBIT, REL_Y);
        }
    }
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Device names all start with `name` so input capture can skip them. */
int uinput_open(const char *name)
{
    fd_abs = make(name, "pointer", 0);
    fd_rel = make(name, "mouse", 1);
    fd_kbd = make(name, "keyboard", 2);
    if (fd_abs < 0 || fd_rel < 0 || fd_kbd < 0) {
        perror("/dev/uinput");
        return -1;
    }
    return 0;
}

void uinput_abs(int x, int y, int w, int h)
{
    if (w < 2) w = 2;
    if (h < 2) h = 2;
    emit(fd_abs, EV_ABS, ABS_X, (int)((long long)x * ABS_RANGE / (w - 1)));
    emit(fd_abs, EV_ABS, ABS_Y, (int)((long long)y * ABS_RANGE / (h - 1)));
    emit(fd_abs, EV_SYN, SYN_REPORT, 0);
}

void uinput_rel(int dx, int dy)
{
    if (dx) emit(fd_rel, EV_REL, REL_X, dx);
    if (dy) emit(fd_rel, EV_REL, REL_Y, dy);
    emit(fd_rel, EV_SYN, SYN_REPORT, 0);
}

static int fd_for(int evcode)
{
    return (evcode >= BTN_MOUSE && evcode <= BTN_TASK) ? fd_abs : fd_kbd;
}

/* value: 0 up, 1 down.  Repeats are left to the desktop's own autorepeat. */
void uinput_key(int evcode, int value)
{
    int fd;
    if (evcode <= 0 || evcode >= KEY_CNT || value > 1) return;
    fd = fd_for(evcode);
    if (value) down[evcode / 8] |= (unsigned char)(1 << (evcode % 8));
    else down[evcode / 8] &= (unsigned char)~(1 << (evcode % 8));
    emit(fd, EV_KEY, evcode, value);
    emit(fd, EV_SYN, SYN_REPORT, 0);
}

void uinput_wheel(int dy, int dx)
{
    /* whole notches only; the wire unit is 1/120 notch */
    static int accy, accx;
    accy += dy;
    accx += dx;
    if (accy / 120) { emit(fd_abs, EV_REL, REL_WHEEL, accy / 120); accy %= 120; }
    if (accx / 120) { emit(fd_abs, EV_REL, REL_HWHEEL, accx / 120); accx %= 120; }
    emit(fd_abs, EV_SYN, SYN_REPORT, 0);
}

void uinput_release_all(void)
{
    int code;
    for (code = 1; code < KEY_CNT; code++)
        if (down[code / 8] & (1 << (code % 8)))
            uinput_key(code, 0);
}

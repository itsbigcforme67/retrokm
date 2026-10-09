/* Keyboard lighting: colour the keyboard after the machine it is typing on.
 *
 * Supported: Fnatic Gear Streak (2f0e:0101) and miniStreak (2f0e:0102),
 * whose lighting is on HID interface 1 (protocol from OpenRGB's
 * FnaticStreakController, after Hanna Czenczek's leddy).  Packets are 64
 * bytes plus report ID 0:
 *
 *   [0] report ID 0   [1] command   [2..4] total length   [5..7] offset
 *   [8..64] up to 57 bytes of the request, which starts with the command
 *
 * 0F 03 + RGB per LED sets every key directly; the keyboard falls back to
 * its own profile unless something arrives every half second, so 07 is
 * sent as a keepalive; 04 <profile> hands the lighting back. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "hub.h"

static int fd = -1;
static int nleds;
static char path[300];
static long long last_send, last_look;
static unsigned long want = 0xFFFFFFFF;   /* colour shown now; ~0 = keyboard's own */

static int request(const unsigned char *data, size_t len)
{
    unsigned char pkt[65];
    size_t off;

    if (fd < 0) return -1;
    for (off = 0; off < len || off == 0; off += 57) {
        size_t k = len - off < 57 ? len - off : 57;
        memset(pkt, 0, sizeof pkt);
        pkt[1] = data[0];
        pkt[2] = (unsigned char)len;
        pkt[3] = (unsigned char)(len >> 8);
        pkt[4] = (unsigned char)(len >> 16);
        pkt[5] = (unsigned char)off;
        pkt[6] = (unsigned char)(off >> 8);
        pkt[7] = (unsigned char)(off >> 16);
        memcpy(pkt + 8, data + off, k);
        if (write(fd, pkt, sizeof pkt) != (ssize_t)sizeof pkt) {
            logmsg("lights: lost %s", path);
            close(fd);
            fd = -1;
            return -1;
        }
        if (len == 0) break;
    }
    last_send = now_ms();
    return 0;
}

/* Find the Streak's lighting interface among /dev/hidraw* */
static void look(void)
{
    DIR *d = opendir("/sys/class/hidraw");
    struct dirent *e;

    last_look = now_ms();
    if (!d) return;
    while ((e = readdir(d)) && fd < 0) {
        char p[600], line[128];
        unsigned bus, vid, pid;
        int iface = -1;
        FILE *f;

        if (strncmp(e->d_name, "hidraw", 6)) continue;
        snprintf(p, sizeof p, "/sys/class/hidraw/%s/device/uevent", e->d_name);
        if (!(f = fopen(p, "r"))) continue;
        vid = pid = 0;
        while (fgets(line, sizeof line, f))
            if (sscanf(line, "HID_ID=%x:%x:%x", &bus, &vid, &pid) == 3) break;
        fclose(f);
        if (vid != 0x2F0E || (pid != 0x0101 && pid != 0x0102)) continue;
        snprintf(p, sizeof p, "/sys/class/hidraw/%s/device/../bInterfaceNumber", e->d_name);
        if ((f = fopen(p, "r"))) { if (fscanf(f, "%x", (unsigned *)&iface) != 1) iface = -1; fclose(f); }
        if (iface != 1) continue;
        snprintf(path, sizeof path, "/dev/%s", e->d_name);
        if ((fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC)) < 0) continue;
        nleds = pid == 0x0102 ? 106 : 124;
        logmsg("lights: Fnatic %s on %s", pid == 0x0102 ? "miniStreak" : "Streak", path);
        want = 0xFFFFFFFF;
    }
    closedir(d);
}

static void show(unsigned long rgb)
{
    unsigned char buf[2 + 124 * 3];
    int i, pct = cfg.lights_brightness;
    unsigned char r = (unsigned char)(((rgb >> 16) & 255) * pct / 100);
    unsigned char g = (unsigned char)(((rgb >> 8) & 255) * pct / 100);
    unsigned char b = (unsigned char)((rgb & 255) * pct / 100);

    buf[0] = 0x0F;
    buf[1] = 0x03;
    for (i = 0; i < nleds; i++) {
        buf[2 + i * 3] = r;
        buf[3 + i * 3] = g;
        buf[4 + i * 3] = b;
    }
    request(buf, 2 + (size_t)nleds * 3);
}

/* Colour the keyboard: rgb 0xRRGGBB, or ~0 to hand it back to its own lighting */
void lights_set(unsigned long rgb)
{
    if (!cfg.lights) return;
    if (fd < 0) look();
    if (fd < 0 || rgb == want) return;
    want = rgb;
    if (rgb == 0xFFFFFFFF) {
        unsigned char back[2] = { 0x04, 1 };   /* profile 1 */
        request(back, 2);
    } else {
        show(rgb);
    }
}

void lights_tick(void)
{
    long long now = now_ms();
    if (!cfg.lights) return;
    if (fd < 0) {
        if (now - last_look > 5000) look();   /* plugged in later */
        return;
    }
    if (want != 0xFFFFFFFF && now - last_send > 300) {   /* it reverts after 500 ms */
        unsigned char ka[1] = { 0x07 };
        request(ka, 1);
    }
}

/* A screen's colour: its own, or one picked from its picture */
unsigned long lights_color(const Screen *s)
{
    if (s->color >= 0) return (unsigned long)s->color;
    if (!strcmp(s->art, "ideapad")) return 0x4F8DF7;
    if (!strcmp(s->art, "precision")) return 0xEAB308;
    if (!strcmp(s->art, "armbox")) return 0xE5E7EB;
    if (!strcmp(s->art, "octane")) return 0x2EC4B6;
    if (!strcmp(s->art, "dreamcast")) return 0xF97316;
    return 0xA78BFA;
}

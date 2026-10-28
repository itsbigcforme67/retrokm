/* Video devices, the wires between them, and routes through them
 * (docs/SWITCHING.md).
 *
 * A screen's video goes into a device input; a monitor watches a device
 * output; wires run from outputs to other devices' inputs.  Showing a
 * screen on a monitor is a path search: at each device, any input can be
 * tied to any output, and an output carries on along its wire.  Ties are
 * then set from the screen's end towards the monitor. */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>
#include "hub.h"

/* ---- talking to a device: a serial port or a TCP connection ------------- */

static speed_t baud_const(int baud)
{
    switch (baud) {
    case 1200: return B1200;
    case 2400: return B2400;
    case 4800: return B4800;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    }
    return B9600;
}

static int tcp_open(VDevice *d)
{
    char host[128], *colon;
    struct addrinfo hints, *ai = NULL;
    int fd;

    snprintf(host, sizeof host, "%s", d->path + 4);
    colon = strrchr(host, ':');
    if (!colon) { errno = EINVAL; return -1; }
    *colon = 0;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, colon + 1, &hints, &ai) != 0 || !ai) { errno = EHOSTUNREACH; return -1; }
    fd = socket(ai->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd >= 0 && connect(fd, ai->ai_addr, ai->ai_addrlen) < 0 && errno != EINPROGRESS) {
        int e = errno;
        close(fd);
        fd = -1;
        errno = e;
    }
    freeaddrinfo(ai);
    if (fd >= 0) d->connecting = 1;
    return fd;
}

int vdev_open(VDevice *d)
{
    struct termios t;

    d->llen = 0;
    d->pending_out = 0;
    d->connecting = 0;
    d->last_open_try = now_ms();
    if (!strncasecmp(d->path, "tcp:", 4)) {
        d->fd = tcp_open(d);
        return d->fd < 0 ? -1 : 0;
    }
    d->fd = open(d->path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (d->fd < 0) return -1;
    if (tcgetattr(d->fd, &t) == 0) {
        cfmakeraw(&t);
        cfsetspeed(&t, baud_const(d->baud));
        t.c_cflag |= CLOCAL | CREAD;
        t.c_cflag &= ~(tcflag_t)(CRTSCTS | CSTOPB | PARENB);
        tcsetattr(d->fd, TCSANOW, &t);
    }
    tcflush(d->fd, TCIOFLUSH);
    logmsg("%s: opened %s at %d baud", d->name, d->path, d->baud);
    return 0;
}

void vdev_close(VDevice *d)
{
    if (d->fd < 0) return;
    if (!d->connecting) logmsg("%s: lost %s", d->name, d->path);
    close(d->fd);
    d->fd = -1;
    d->connecting = 0;
}

void vdev_write(VDevice *d, const char *s)
{
    if (d->fd < 0 || d->connecting) return;
    if (verbose) logmsg("%s: > %s", d->name, s);
    if (write(d->fd, s, strlen(s)) < 0 && errno != EAGAIN) vdev_close(d);
}

void vdev_read(VDevice *d, void (*line)(VDevice *d, char *s))
{
    char buf[256];
    int n, k;

    if (d->fd < 0 || d->connecting) return;
    n = (int)read(d->fd, buf, sizeof buf);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) { vdev_close(d); return; }
    for (k = 0; k < n; k++) {
        char c = buf[k];
        if (c == '\r' || c == '\n') {
            d->lbuf[d->llen] = 0;
            if (d->llen) {
                if (verbose) logmsg("%s: < %s", d->name, d->lbuf);
                line(d, d->lbuf);
            }
            d->llen = 0;
        } else if (d->llen < (int)sizeof d->lbuf - 1) {
            d->lbuf[d->llen++] = c;
        }
    }
}

/* Every tick: reopen a lost port every 5 s, and see whether a TCP connect
 * has finished; opened() runs once it is usable. */
void vdev_keep_open(VDevice *d, void (*opened)(VDevice *d))
{
    if (!d->path[0]) return;
    if (d->fd < 0) {
        if (now_ms() - d->last_open_try > 5000 && vdev_open(d) == 0 && !d->connecting) opened(d);
        return;
    }
    if (d->connecting) {
        struct pollfd p = { d->fd, POLLOUT, 0 };
        int err = 0;
        socklen_t len = sizeof err;
        if (poll(&p, 1, 0) <= 0) return;
        if (getsockopt(d->fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err) {
            close(d->fd);
            d->fd = -1;
            d->connecting = 0;
            return;
        }
        d->connecting = 0;
        logmsg("%s: connected to %s", d->name, d->path + 4);
        opened(d);
    }
}

VDevice *vdev_by_name(const char *name)
{
    int i;
    for (i = 0; i < cfg.nvdevs; i++)
        if (!strcasecmp(cfg.vdevs[i].name, name)) return &cfg.vdevs[i];
    return NULL;
}

/* Any device with a port to reach it (the old form makes one without, for
 * configs whose machines say "input =" but have no switcher) */
int video_any(void)
{
    int i;
    for (i = 0; i < cfg.nvdevs; i++)
        if (cfg.vdevs[i].path[0]) return 1;
    return 0;
}

int video_online(int dev)
{
    return dev >= 0 && dev < cfg.nvdevs && cfg.vdevs[dev].fd >= 0 && !cfg.vdevs[dev].connecting;
}

int video_all_online(void)
{
    int i;
    for (i = 0; i < cfg.nvdevs; i++)
        if (cfg.vdevs[i].path[0] && !video_online(i)) return 0;
    return video_any();
}

int video_input_for(int dev, int out)
{
    if (dev < 0 || dev >= cfg.nvdevs || out < 1 || out > VDEV_MAX_IO) return 0;
    return cfg.vdevs[dev].tie[out];
}

int video_tie(int dev, int in, int out)
{
    if (dev < 0 || dev >= cfg.nvdevs) return -1;
    switch (cfg.vdevs[dev].driver) {
    case VDRV_EXTRON_SIS: extron_tie(&cfg.vdevs[dev], in, out); return 0;
    case VDRV_TEXT: return cmdtext_tie(&cfg.vdevs[dev], in, out);
    }
    return -1;
}

const char *video_driver_name(int driver)
{
    switch (driver) {
    case VDRV_EXTRON_SIS: return "extron-sis";
    case VDRV_TEXT: return "text";
    }
    return "?";
}

const char *video_port_name(int dev, int port, int out, char *buf, size_t n)
{
    snprintf(buf, n, "%s.%s%d", dev >= 0 && dev < cfg.nvdevs ? cfg.vdevs[dev].name : "?", out ? "out" : "in", port);
    return buf;
}

static const Wire *wire_into(int dev, int in)
{
    int i;
    for (i = 0; i < cfg.nwires; i++)
        if (cfg.wires[i].to_dev == dev && cfg.wires[i].to_in == in) return &cfg.wires[i];
    return NULL;
}

int video_source(int dev, int out, int *sdev, int *sin)
{
    int hops;
    for (hops = 0; hops <= MAX_WIRES; hops++) {
        int in = video_input_for(dev, out);
        const Wire *w;
        if (in <= 0) return 0;
        if (!(w = wire_into(dev, in))) { *sdev = dev; *sin = in; return 1; }
        dev = w->from_dev;
        out = w->from_out;
    }
    return 0;                                   /* the wires loop */
}

/* ---- routes ------------------------------------------------------------- */

/* The outputs each other monitor's picture comes through now */
static void mark_busy(unsigned char busy[MAX_VDEVS][VDEV_MAX_IO + 1], const Monitor *skip)
{
    int i, hops;
    memset(busy, 0, sizeof(unsigned char) * MAX_VDEVS * (VDEV_MAX_IO + 1));
    for (i = 0; i < cfg.nmonitors; i++) {
        const Monitor *m = &cfg.monitors[i];
        int dev = m->vdev, out = m->vout;
        if (m == skip || m->fixed || !out) continue;
        for (hops = 0; hops <= MAX_WIRES; hops++) {
            int in = video_input_for(dev, out);
            const Wire *w;
            if (in <= 0) break;
            busy[dev][out] = 1;
            if (!(w = wire_into(dev, in))) break;
            dev = w->from_dev;
            out = w->from_out;
        }
    }
}

/* Tying in to out on dev: already so is cheapest; taking an output another
 * monitor's picture uses costs a lot, so a free way round wins. */
static int tie_cost(unsigned char busy[MAX_VDEVS][VDEV_MAX_IO + 1], int dev, int in, int out)
{
    int now = video_input_for(dev, out);
    if (now == in) return 1;
    return busy[dev][out] ? 22 : 2;
}

int video_route(const Screen *s, const Monitor *m, char *err, size_t n)
{
    static int dist[MAX_VDEVS][VDEV_MAX_IO + 1], via_in[MAX_VDEVS][VDEV_MAX_IO + 1];
    static int prev[MAX_VDEVS][VDEV_MAX_IO + 1];       /* dev * 1000 + out, -1 = from the screen */
    static unsigned char done[MAX_VDEVS][VDEV_MAX_IO + 1], busy[MAX_VDEVS][VDEV_MAX_IO + 1];
    int path_dev[MAX_WIRES + 1], path_in[MAX_WIRES + 1], path_out[MAX_WIRES + 1], np = 0;
    int d, o, k;
    const int INF = 1 << 29;

    if (!s->vin) { snprintf(err, n, "%s is not wired to a switcher", s->name); return -1; }
    if (m->fixed || !m->vout) { snprintf(err, n, "%s is not a switched monitor", m->name); return -1; }
    mark_busy(busy, m);
    for (d = 0; d < MAX_VDEVS; d++)
        for (o = 0; o <= VDEV_MAX_IO; o++) { dist[d][o] = INF; done[d][o] = 0; prev[d][o] = -1; }
    for (o = 1; o <= cfg.vdevs[s->vdev].outputs; o++) {
        dist[s->vdev][o] = tie_cost(busy, s->vdev, s->vin, o);
        via_in[s->vdev][o] = s->vin;
    }
    for (;;) {
        int bd = -1, bo = 0, best = INF, w;
        for (d = 0; d < cfg.nvdevs; d++)
            for (o = 1; o <= cfg.vdevs[d].outputs; o++)
                if (!done[d][o] && dist[d][o] < best) { best = dist[d][o]; bd = d; bo = o; }
        if (bd < 0) break;
        done[bd][bo] = 1;
        if (bd == m->vdev && bo == m->vout) break;
        for (w = 0; w < cfg.nwires; w++) {
            const Wire *wi = &cfg.wires[w];
            if (wi->from_dev != bd || wi->from_out != bo) continue;
            for (o = 1; o <= cfg.vdevs[wi->to_dev].outputs; o++) {
                int c = best + tie_cost(busy, wi->to_dev, wi->to_in, o);
                if (c < dist[wi->to_dev][o]) {
                    dist[wi->to_dev][o] = c;
                    via_in[wi->to_dev][o] = wi->to_in;
                    prev[wi->to_dev][o] = bd * 1000 + bo;
                }
            }
        }
    }
    if (dist[m->vdev][m->vout] >= INF) {
        snprintf(err, n, "no way through the switchers from %s to %s", s->name, m->name);
        return -1;
    }
    for (d = m->vdev, o = m->vout; np <= MAX_WIRES; np++) {
        int p = prev[d][o];
        path_dev[np] = d; path_in[np] = via_in[d][o]; path_out[np] = o;
        if (p < 0) { np++; break; }
        d = p / 1000;
        o = p % 1000;
    }
    for (k = 0; k < np; k++)
        if (!video_online(path_dev[k])) {
            snprintf(err, n, "%s is not connected", cfg.vdevs[path_dev[k]].name);
            return -1;
        }
    for (k = np - 1; k >= 0; k--) {             /* from the screen's end to the monitor */
        if (video_input_for(path_dev[k], path_out[k]) == path_in[k]) continue;
        if (verbose) logmsg("route %s -> %s: %s %d*%d", s->name, m->name,
                            cfg.vdevs[path_dev[k]].name, path_in[k], path_out[k]);
        if (video_tie(path_dev[k], path_in[k], path_out[k]) < 0) {
            snprintf(err, n, "%s has no command to tie with", cfg.vdevs[path_dev[k]].name);
            return -1;
        }
    }
    return 0;
}

int video_unroute(const Monitor *m, char *err, size_t n)
{
    if (m->fixed || !m->vout) { snprintf(err, n, "%s is not a switched monitor", m->name); return -1; }
    if (!video_online(m->vdev)) { snprintf(err, n, "%s is not connected", cfg.vdevs[m->vdev].name); return -1; }
    if (video_tie(m->vdev, 0, m->vout) < 0) {
        snprintf(err, n, "%s cannot show nothing on an output", cfg.vdevs[m->vdev].name);
        return -1;
    }
    return 0;
}

/* ---- driving the devices ------------------------------------------------- */

int video_init(void)
{
    int i;
    for (i = 0; i < cfg.nvdevs; i++) cfg.vdevs[i].fd = -1;
    for (i = 0; i < cfg.nmonitors; i++) {      /* read back what the monitors see ... */
        const Monitor *m = &cfg.monitors[i];
        if (!m->fixed && m->vout) cfg.vdevs[m->vdev].watched[m->vout] = cfg.vdevs[m->vdev].need[m->vout] = 1;
    }
    for (i = 0; i < cfg.nwires; i++)            /* ... and what goes down each wire */
    {
        VDevice *d = &cfg.vdevs[cfg.wires[i].from_dev];
        d->watched[cfg.wires[i].from_out] = d->need[cfg.wires[i].from_out] = 1;
    }
    for (i = 0; i < cfg.nvdevs; i++)
        switch (cfg.vdevs[i].driver) {
        case VDRV_EXTRON_SIS: extron_init(&cfg.vdevs[i]); break;
        case VDRV_TEXT: cmdtext_init(&cfg.vdevs[i]); break;
        }
    return 0;
}

int video_fds(int *fds, int *devs, int max)
{
    int i, n = 0;
    for (i = 0; i < cfg.nvdevs && n < max; i++)
        if (cfg.vdevs[i].fd >= 0) { fds[n] = cfg.vdevs[i].fd; devs[n++] = i; }
    return n;
}

void video_readable(int dev)
{
    if (dev < 0 || dev >= cfg.nvdevs) return;
    switch (cfg.vdevs[dev].driver) {
    case VDRV_EXTRON_SIS: extron_readable(&cfg.vdevs[dev]); break;
    case VDRV_TEXT: cmdtext_readable(&cfg.vdevs[dev]); break;
    }
}

void video_tick(void)
{
    int i;
    for (i = 0; i < cfg.nvdevs; i++)
        switch (cfg.vdevs[i].driver) {
        case VDRV_EXTRON_SIS: extron_tick(&cfg.vdevs[i]); break;
        case VDRV_TEXT: cmdtext_tick(&cfg.vdevs[i]); break;
        }
}

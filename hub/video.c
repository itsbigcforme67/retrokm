/* Video devices, the wires between them, and routes through them
 * (docs/SWITCHING.md).
 *
 * A screen's video goes into a device input; a monitor watches a device
 * output; wires run from outputs to other devices' inputs.  Showing a
 * screen on a monitor is a path search: at each device, any input can be
 * tied to any output, and an output carries on along its wire.  Ties are
 * then set from the screen's end towards the monitor. */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "hub.h"

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
    return dev >= 0 && dev < cfg.nvdevs && cfg.vdevs[dev].fd >= 0;
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

void video_tie(int dev, int in, int out)
{
    if (dev < 0 || dev >= cfg.nvdevs) return;
    switch (cfg.vdevs[dev].driver) {
    case VDRV_EXTRON_SIS: extron_tie(&cfg.vdevs[dev], in, out); break;
    }
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
        video_tie(path_dev[k], path_in[k], path_out[k]);
    }
    return 0;
}

int video_unroute(const Monitor *m, char *err, size_t n)
{
    if (m->fixed || !m->vout) { snprintf(err, n, "%s is not a switched monitor", m->name); return -1; }
    if (!video_online(m->vdev)) { snprintf(err, n, "%s is not connected", cfg.vdevs[m->vdev].name); return -1; }
    video_tie(m->vdev, 0, m->vout);
    return 0;
}

/* ---- driving the devices ------------------------------------------------- */

int video_init(void)
{
    int i;
    for (i = 0; i < cfg.nvdevs; i++) cfg.vdevs[i].fd = -1;
    for (i = 0; i < cfg.nmonitors; i++) {      /* read back what the monitors see ... */
        const Monitor *m = &cfg.monitors[i];
        if (!m->fixed && m->vout) extron_watch(&cfg.vdevs[m->vdev], m->vout);
    }
    for (i = 0; i < cfg.nwires; i++)            /* ... and what goes down each wire */
        extron_watch(&cfg.vdevs[cfg.wires[i].from_dev], cfg.wires[i].from_out);
    for (i = 0; i < cfg.nvdevs; i++)
        switch (cfg.vdevs[i].driver) {
        case VDRV_EXTRON_SIS: extron_init(&cfg.vdevs[i]); break;
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
    }
}

void video_tick(void)
{
    int i;
    for (i = 0; i < cfg.nvdevs; i++)
        switch (cfg.vdevs[i].driver) {
        case VDRV_EXTRON_SIS: extron_tick(&cfg.vdevs[i]); break;
        }
}

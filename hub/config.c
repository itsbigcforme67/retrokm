/* INI-style configuration.  See retrokm.conf.example. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "hub.h"

Config cfg;

static void copy(char *dst, size_t n, const char *src);
static char *trim(char *s);

/* Kinds of accessory: id (config, state file, panel) and what the panel calls it */
const AccKind acc_kinds[ACC_KINDS] = {
    { "kbm", "keyboard + mouse" },
    { "pad", "gamepads" },
};

int acc_kind(const char *id)
{
    int k;
    for (k = 0; k < ACC_KINDS; k++)
        if (!strcmp(acc_kinds[k].id, id)) return k;
    if (!strcmp(id, "keys")) return ACC_KBM;   /* Claude's keyboard, before seats (2026-10-28) */
    return -1;
}

Seat *seat_by_name(const char *name)
{
    int i;
    for (i = 0; i < cfg.nseats; i++)
        if (!strcasecmp(cfg.seats[i].name, name)) return &cfg.seats[i];
    return NULL;
}

Seat *seat_add(const char *name, long color)
{
    Seat *z = seat_by_name(name);
    if (z) return z;
    if (cfg.nseats >= MAX_SEATS || !*name) return NULL;
    z = &cfg.seats[cfg.nseats++];
    memset(z, 0, sizeof *z);
    copy(z->name, sizeof z->name, name);
    z->color = color;
    return z;
}

Accessory *acc_by_name(const char *name)
{
    int i;
    for (i = 0; i < cfg.nacc; i++)
        if (!strcmp(cfg.acc[i].name, name)) return &cfg.acc[i];
    return NULL;
}

static Accessory *acc_new(const char *name, int kind, const char *label)
{
    Accessory *a;
    if (cfg.nacc >= MAX_ACC) return NULL;
    a = &cfg.acc[cfg.nacc++];
    memset(a, 0, sizeof *a);
    copy(a->name, sizeof a->name, name);
    copy(a->label, sizeof a->label, label);
    a->kind = kind;
    a->color = -1;
    return a;
}

/* A pad seen for the first time is linked to the hub's own desktop, where
 * it is plugged in; draw lines on the panel to send it elsewhere. */
Accessory *acc_add(const char *name, int kind, const char *label)
{
    Accessory *a = acc_by_name(name);
    int i;
    if (a) {
        copy(a->label, sizeof a->label, label);
        return a;
    }
    if (!(a = acc_new(name, kind, label))) return NULL;
    for (i = 0; i < cfg.nscreens; i++)
        if (screen_is_desktop(&cfg.screens[i])) a->player[i] = 1;
    config_save_state();
    return a;
}

static void load_state(void);

static char mon_fixed[MAX_MONITORS][32];   /* resolved after the whole file is read */
static char mon_shared[MAX_MONITORS][32];
static char area_mon[MAX_SCREENS][MAX_AREAS][32];
static char scr_via[MAX_SCREENS][32];
static char scr_video[MAX_SCREENS][48], mon_video[MAX_MONITORS][48];
static int scr_old_in[MAX_SCREENS], mon_old_out[MAX_MONITORS];   /* the old "input =" / "output =" */

/* A video device with the defaults of an Extron SIS switcher */
static VDevice *vdev_new(const char *name)
{
    VDevice *d;
    if (cfg.nvdevs >= MAX_VDEVS) return NULL;
    d = &cfg.vdevs[cfg.nvdevs++];
    copy(d->name, sizeof d->name, name);
    d->driver = VDRV_EXTRON_SIS;
    d->baud = 9600;
    d->poll = 5;
    d->tie_cmd = '!';
    d->read_cmd = '%';
    d->inputs = d->outputs = 8;
    d->fd = -1;
    return d;
}

/* One "key = value" for a [device], from the config or from its device
 * file.  Device files also describe the box for people (name, kind, price
 * ...): those keys mean nothing to the hub and are skipped there. */
static int device_key(VDevice *d, const char *k, char *v, const char *conf_path, int from_file);

/* devices/<model>.conf: next to the config, then next to the hub's source
 * tree (hub/../devices), then /usr/local/share/retrokm/devices */
static int load_model(VDevice *d, const char *model, const char *conf_path)
{
    char dirs[3][540], path[600], line[512], exe[512];  /* dirs are under 540, so path fits */
    const char *slash = strrchr(conf_path, '/');
    ssize_t n;
    FILE *f = NULL;
    int i, lineno = 0;

    if (!*model || strchr(model, '/') || model[0] == '.') return -1;
    snprintf(dirs[0], sizeof dirs[0], "%.*sdevices", slash ? (int)(slash - conf_path + 1) : 0, conf_path);
    n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[n > 0 ? n : 0] = 0;
    if (n > 0 && strrchr(exe, '/')) *strrchr(exe, '/') = 0;
    snprintf(dirs[1], sizeof dirs[1], "%.511s/../devices", exe);
    snprintf(dirs[2], sizeof dirs[2], "/usr/local/share/retrokm/devices");
    for (i = 0; i < 3 && !f; i++) {
        snprintf(path, sizeof path, "%.539s/%.47s.conf", dirs[i], model);
        f = fopen(path, "r");
    }
    if (!f) {
        fprintf(stderr, "device %s: no device file %s.conf (looked in %s and %s)\n", d->name, model, dirs[0], dirs[2]);
        return -1;
    }
    copy(d->model, sizeof d->model, model);
    while (fgets(line, sizeof line, f)) {
        char *t = trim(line), *eq, *c;
        lineno++;
        if (!*t || *t == '#' || *t == ';') continue;
        for (c = t + 1; *c; c++)
            if ((*c == '#' || *c == ';') && isspace((unsigned char)c[-1])) { *c = 0; break; }
        if (!(eq = strchr(t, '='))) continue;
        *eq = 0;
        if (device_key(d, trim(t), trim(eq + 1), conf_path, 1) < 0) {
            fprintf(stderr, "%s:%d: cannot parse this line\n", path, lineno);
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return 0;
}

static int device_key(VDevice *d, const char *k, char *v, const char *conf_path, int from_file)
{
    if (!strcasecmp(k, "driver")) {
        if (!strcasecmp(v, "extron-sis")) d->driver = VDRV_EXTRON_SIS;
        else if (!strcasecmp(v, "text")) d->driver = VDRV_TEXT;
        else return -1;
    }
    else if (!strcasecmp(k, "model")) {
        if (from_file || load_model(d, v, conf_path) < 0) return -1;
    }
    else if (!strcasecmp(k, "port") || !strcasecmp(k, "device"))
        copy(d->path, sizeof d->path, strncasecmp(v, "serial:", 7) ? v : v + 7);
    else if (!strcasecmp(k, "baud")) d->baud = atoi(v);
    else if (!strcasecmp(k, "poll")) d->poll = atoi(v);
    else if (!strcasecmp(k, "tie_command") && *v) d->tie_cmd = *v;
    else if (!strcasecmp(k, "read_command") && *v) d->read_cmd = *v;
    else if (!strcasecmp(k, "inputs")) d->inputs = atoi(v);
    else if (!strcasecmp(k, "outputs")) d->outputs = atoi(v);
    else if (!strcasecmp(k, "tie")) copy(d->tie_fmt, sizeof d->tie_fmt, v);
    else if (!strcasecmp(k, "untie")) copy(d->untie_fmt, sizeof d->untie_fmt, v);
    else if (!strcasecmp(k, "read")) copy(d->read_fmt, sizeof d->read_fmt, v);
    else if (!strcasecmp(k, "reply")) copy(d->reply_fmt, sizeof d->reply_fmt, v);
    else if (!from_file) return -1;
    if (d->inputs < 1 || d->inputs > VDEV_MAX_IO || d->outputs < 1 || d->outputs > VDEV_MAX_IO) return -1;
    return 0;
}

/* "mvx.in3" / "mvx.out2": which device, which port, and whether it is an
 * output; -1 if it is not one */
static int parse_port(const char *v, int *dev, int *port, int *out)
{
    const char *dot = strrchr(v, '.');
    char name[32];
    VDevice *d;
    if (!dot || dot == v || (size_t)(dot - v) >= sizeof name) return -1;
    memcpy(name, v, (size_t)(dot - v));
    name[dot - v] = 0;
    if (!(d = vdev_by_name(name))) return -1;
    if (!strncasecmp(dot + 1, "out", 3)) { *out = 1; *port = atoi(dot + 4); }
    else if (!strncasecmp(dot + 1, "in", 2)) { *out = 0; *port = atoi(dot + 3); }
    else return -1;
    *dev = (int)(d - cfg.vdevs);
    if (*port < 1 || *port > (*out ? d->outputs : d->inputs) || *port > VDEV_MAX_IO) return -1;
    return 0;
}

static char *trim(char *s)
{
    char *e;
    while (isspace((unsigned char)*s)) s++;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static int truthy(const char *v)
{
    return !strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcasecmp(v, "on") || !strcmp(v, "1");
}

static void copy(char *dst, size_t n, const char *src)
{
    strncpy(dst, src, n - 1);
    dst[n - 1] = 0;
}

/* A monitor input, by name or MCCS number (VCP 0x60 values) */
static int input_code(const char *v)
{
    static const struct { const char *name; int code; } names[] = {
        { "vga", 1 }, { "vga2", 2 }, { "dvi", 3 }, { "dvi2", 4 }, { "composite", 5 },
        { "svideo", 7 }, { "component", 12 }, { "dp", 15 }, { "displayport", 15 },
        { "dp2", 16 }, { "hdmi", 17 }, { "hdmi2", 18 },
    };
    size_t i;
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcasecmp(v, names[i].name)) return names[i].code;
    return (int)strtol(v, NULL, 0);
}

static int parse_remap(Screen *s, char *v)
{
    char *tok, *save = NULL;
    for (tok = strtok_r(v, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char *colon = strchr(tok, ':');
        int a, b;
        if (!colon || s->nremap >= MAX_REMAP) return -1;
        *colon = 0;
        a = key_by_name(trim(tok));
        b = key_by_name(trim(colon + 1));
        if (a < 0 || b < 0) return -1;
        s->remap_from[s->nremap] = (unsigned char)a;
        s->remap_to[s->nremap] = (unsigned char)b;
        s->nremap++;
    }
    return 0;
}

int config_load(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[512];
    enum { S_NONE, S_HUB, S_DEVICE, S_WIRE, S_SCREEN, S_MONITOR, S_HOTKEYS, S_SEAT } sec = S_NONE;
    VDevice *dev = NULL;
    char wire_from[MAX_WIRES][48], wire_to[MAX_WIRES][48];
    int nwire = 0;
    Seat *seat = NULL;
    Screen *scr = NULL;
    Monitor *mon = NULL;
    int lineno = 0, i;

    if (!f) { perror(path); return -1; }

    memset(&cfg, 0, sizeof cfg);
    memset(scr_video, 0, sizeof scr_video);
    memset(mon_video, 0, sizeof mon_video);
    memset(scr_old_in, 0, sizeof scr_old_in);
    memset(mon_old_out, 0, sizeof mon_old_out);
    copy(cfg.listen, sizeof cfg.listen, "0.0.0.0");
    copy(cfg.pad, sizeof cfg.pad, "auto");
    cfg.port = RKM_PORT;
    cfg.ctl_port = RKM_PORT + 1;
    cfg.grab = 1;
    cfg.speed = 1.0;
    cfg.accel = 0.5;
    cfg.wiggle = 400;
    cfg.follow_tie = 1;
    cfg.lights = 1;
    cfg.lights_brightness = 80;
    snprintf(cfg.state_path, sizeof cfg.state_path, "%s.state", path);

    while (fgets(line, sizeof line, f)) {
        char *s = trim(line), *eq, *k, *v, *c;

        lineno++;
        if (!*s || *s == '#' || *s == ';') continue;
        for (c = s + 1; *c; c++)               /* trailing "  # comment" */
            if ((*c == '#' || *c == ';') && isspace((unsigned char)c[-1])) { *c = 0; break; }
        s = trim(s);

        if (*s == '[') {
            char *end = strchr(s, ']'), *kind, *name;
            if (!end) goto bad;
            *end = 0;
            kind = trim(s + 1);
            name = kind;
            while (*name && !isspace((unsigned char)*name)) name++;
            if (*name) { *name++ = 0; name = trim(name); }

            if (!strcasecmp(kind, "hub")) sec = S_HUB;
            else if (!strcasecmp(kind, "extron") || !strcasecmp(kind, "device")) {
                /* [device mvx]: a switcher; [extron] is the old way to
                 * write the one switcher, a device named "extron" */
                const char *dn = !strcasecmp(kind, "extron") ? "extron" : name;
                if (!*dn || vdev_by_name(dn) || !(dev = vdev_new(dn))) goto bad;
                sec = S_DEVICE;
            }
            else if (!strcasecmp(kind, "wire")) sec = S_WIRE;
            else if (!strcasecmp(kind, "hotkeys")) sec = S_HOTKEYS;
            else if (!strcasecmp(kind, "seat")) {
                /* [seat james]: someone at the desk; the first one is the
                 * keyboard + mouse's and gets commands that name no seat */
                if (!*name || !(seat = seat_add(name, 0x22C55E))) goto bad;
                seat->from_config = 1;
                sec = S_SEAT;
            }
            else if (!strcasecmp(kind, "screen")) {
                if (!*name || cfg.nscreens >= MAX_SCREENS) goto bad;
                sec = S_SCREEN;
                scr = &cfg.screens[cfg.nscreens++];
                copy(scr->name, sizeof scr->name, name);
                scr->speed = 1.0;
                scr->min_move_ms = 8;
                scr->w = 1024;
                scr->h = 768;
                scr->color = -1;
            } else if (!strcasecmp(kind, "monitor")) {
                if (!*name || cfg.nmonitors >= MAX_MONITORS) goto bad;
                sec = S_MONITOR;
                mon = &cfg.monitors[cfg.nmonitors++];
                copy(mon->name, sizeof mon->name, name);
                mon->aspect_w = 16;              /* until the config or the panel says */
                mon->aspect_h = 9;
            } else goto bad;
            continue;
        }

        eq = strchr(s, '=');
        if (!eq) goto bad;
        *eq = 0;
        k = trim(s);
        v = trim(eq + 1);

        switch (sec) {
        case S_HUB:
            if (!strcasecmp(k, "listen")) copy(cfg.listen, sizeof cfg.listen, v);
            else if (!strcasecmp(k, "port")) cfg.port = atoi(v);
            else if (!strcasecmp(k, "control_port")) cfg.ctl_port = atoi(v);
            else if (!strcasecmp(k, "panel_port")) cfg.panel_port = atoi(v);
            else if (!strcasecmp(k, "keyboard_lights")) cfg.lights = truthy(v);
            else if (!strcasecmp(k, "lights_brightness")) cfg.lights_brightness = atoi(v);
            else if (!strcasecmp(k, "state")) copy(cfg.state_path, sizeof cfg.state_path, v);
            else if (!strcasecmp(k, "grab")) cfg.grab = truthy(v);
            else if (!strcasecmp(k, "speed")) cfg.speed = atof(v);
            else if (!strcasecmp(k, "accel")) cfg.accel = atof(v);
            else if (!strcasecmp(k, "wiggle")) cfg.wiggle = atof(v);
            else if (!strcasecmp(k, "pad")) copy(cfg.pad, sizeof cfg.pad, v);
            else if (!strcasecmp(k, "on_tie")) cfg.follow_tie = !strcasecmp(v, "follow");
            else if (!strcasecmp(k, "device")) {
                if (!strcasecmp(v, "auto")) cfg.ndevices = 0;
                else if (!strcasecmp(v, "none")) cfg.ndevices = -1;
                else if (cfg.ndevices >= 0 && cfg.ndevices < MAX_DEVICES)
                    copy(cfg.devices[cfg.ndevices++], sizeof cfg.devices[0], v);
                else goto bad;
            } else goto bad;
            break;
        case S_DEVICE:
            if (device_key(dev, k, v, path, 0) < 0) goto bad;
            break;
        case S_WIRE:
            /* mvx.out4 = tink.in1: a cable from an output to an input */
            if (nwire >= MAX_WIRES) goto bad;
            copy(wire_from[nwire], sizeof wire_from[0], k);
            copy(wire_to[nwire], sizeof wire_to[0], v);
            nwire++;
            break;
        case S_SCREEN:
            if (!strcasecmp(k, "local")) scr->local = truthy(v);
            else if (!strcasecmp(k, "label")) copy(scr->label, sizeof scr->label, v);
            else if (!strcasecmp(k, "art")) copy(scr->art, sizeof scr->art, v);
            else if (!strcasecmp(k, "agent")) scr->no_agent = !truthy(v);
            else if (!strcasecmp(k, "color")) scr->color = strtol(v[0] == '#' ? v + 1 : v, NULL, 16);
            else if (!strcasecmp(k, "size")) {
                if (sscanf(v, "%dx%d", &scr->w, &scr->h) != 2) goto bad;
            }
            else if (!strcasecmp(k, "video")) copy(scr_video[scr - cfg.screens], sizeof scr_video[0], v);
            else if (!strcasecmp(k, "input")) scr_old_in[scr - cfg.screens] = atoi(v);
            else if (!strcasecmp(k, "via")) copy(scr_via[scr - cfg.screens], sizeof scr_via[0], v);
            else if (!strcasecmp(k, "speed")) scr->speed = atof(v);
            else if (!strcasecmp(k, "rate")) {
                int hz = atoi(v);
                if (hz < 1) goto bad;
                scr->min_move_ms = 1000 / hz;
            }
            else if (!strcasecmp(k, "remap")) { if (parse_remap(scr, v) < 0) goto bad; }
            else if (!strcasecmp(k, "area")) {
                /* area = WxH+X+Y [monitor]: with a monitor named, the area
                 * only exists while that (shared) monitor shows this screen */
                int *a = scr->area[scr->narea];
                char mname[32] = "";
                if (scr->narea >= MAX_AREAS ||
                    sscanf(v, "%dx%d+%d+%d %31s", &a[2], &a[3], &a[0], &a[1], mname) < 4 || a[2] < 1 || a[3] < 1)
                    goto bad;
                copy(area_mon[scr - cfg.screens][scr->narea], sizeof area_mon[0][0], mname);
                scr->narea++;
            }
            else goto bad;
            break;
        case S_MONITOR:
            if (!strcasecmp(k, "pos")) {
                if (sscanf(v, "%d , %d", &mon->col, &mon->row) != 2) goto bad;
            }
            else if (!strcasecmp(k, "video")) copy(mon_video[mon - cfg.monitors], sizeof mon_video[0], v);
            else if (!strcasecmp(k, "output")) mon_old_out[mon - cfg.monitors] = atoi(v);
            else if (!strcasecmp(k, "screen"))
                copy(mon_fixed[mon - cfg.monitors], sizeof mon_fixed[0], v);
            else if (!strcasecmp(k, "shared")) {
                copy(mon_shared[mon - cfg.monitors], sizeof mon_shared[0], v);
                mon->shared_on = 1;              /* until the panel says otherwise */
            }
            else if (!strcasecmp(k, "portrait")) mon->portrait = truthy(v);
            else if (!strcasecmp(k, "aspect")) {
                /* aspect = 16:9, 4:3, 16:10 ...: its shape before any turn */
                if (sscanf(v, "%d:%d", &mon->aspect_w, &mon->aspect_h) != 2 ||
                    mon->aspect_w < 1 || mon->aspect_h < 1 || mon->aspect_w > 64 || mon->aspect_h > 64) goto bad;
            }
            else if (!strcasecmp(k, "size")) {
                /* size = 24: screen diagonal in inches */
                double d = atof(v);
                if (d < 1 || d > 200) goto bad;
                mon->inches10 = (int)(d * 10 + 0.5);
            }
            else if (!strcasecmp(k, "capture")) mon->capture = truthy(v);
            else if (!strcasecmp(k, "ddc")) copy(mon->ddc, sizeof mon->ddc, v);
            else if (!strcasecmp(k, "ddc_shared") || !strcasecmp(k, "ddc_switcher")) {
                int code = input_code(v);
                if (code <= 0) goto bad;
                if (!strcasecmp(k, "ddc_shared")) mon->ddc_shared = code;
                else mon->ddc_switcher = code;
            }
            else if (!strcasecmp(k, "label")) copy(mon->label, sizeof mon->label, v);
            else goto bad;
            break;
        case S_SEAT:
            if (!strcasecmp(k, "color") && *v) seat->color = strtol(v[0] == '#' ? v + 1 : v, NULL, 16) & 0xFFFFFF;
            else if (!strcasecmp(k, "label")) copy(seat->label, sizeof seat->label, v);
            else goto bad;
            break;
        case S_HOTKEYS: {
            Hotkey *h;
            if (cfg.nhotkeys >= MAX_HOTKEYS) goto bad;
            h = &cfg.hotkeys[cfg.nhotkeys];
            if (parse_combo(k, &h->mods, &h->usage) < 0) goto bad;
            copy(h->cmd, sizeof h->cmd, v);
            cfg.nhotkeys++;
            break;
        }
        default:
            goto bad;
        }
        continue;
bad:
        fprintf(stderr, "%s:%d: cannot parse this line\n", path, lineno);
        fclose(f);
        return -1;
    }
    fclose(f);

    {   /* the old form: "input = N" / "output = N" are ports on the one
         * switcher, which is the device named "extron" */
        int old = 0;
        VDevice *x;
        for (i = 0; i < cfg.nscreens; i++) old |= scr_old_in[i] > 0;
        for (i = 0; i < cfg.nmonitors; i++) old |= mon_old_out[i] > 0;
        if (old) {
            if (!(x = vdev_by_name("extron")) && !(x = vdev_new("extron"))) {
                fprintf(stderr, "%s: too many devices\n", path);
                return -1;
            }
            logmsg("config: 'input =' / 'output =' are ports on the device \"extron\" (old form; "
                   "see docs/SWITCHING.md for 'video =')");
            for (i = 0; i < cfg.nscreens; i++)
                if (scr_old_in[i] > 0 && !scr_video[i][0])
                    snprintf(scr_video[i], sizeof scr_video[i], "%s.in%d", x->name, scr_old_in[i]);
            for (i = 0; i < cfg.nmonitors; i++)
                if (mon_old_out[i] > 0 && !mon_video[i][0])
                    snprintf(mon_video[i], sizeof mon_video[i], "%s.out%d", x->name, mon_old_out[i]);
            for (i = 0; i < cfg.nscreens; i++)       /* its size was only for the panel then */
                if (scr_old_in[i] > x->inputs && scr_old_in[i] <= VDEV_MAX_IO) x->inputs = scr_old_in[i];
            for (i = 0; i < cfg.nmonitors; i++)
                if (mon_old_out[i] > x->outputs && mon_old_out[i] <= VDEV_MAX_IO) x->outputs = mon_old_out[i];
        }
    }
    for (i = 0; i < cfg.nvdevs; i++)
        if (cfg.vdevs[i].driver == VDRV_TEXT && !cfg.vdevs[i].tie_fmt[0]) {
            fprintf(stderr, "%s: device %s: the text driver needs 'tie =' (or a 'model =' that has one)\n",
                    path, cfg.vdevs[i].name);
            return -1;
        }
    for (i = 0; i < nwire; i++) {
        Wire *w = &cfg.wires[cfg.nwires];
        int fo, ti;
        if (parse_port(wire_from[i], &w->from_dev, &w->from_out, &fo) < 0 || !fo ||
            parse_port(wire_to[i], &w->to_dev, &w->to_in, &ti) < 0 || ti) {
            fprintf(stderr, "%s: wire %s = %s: needs device.outN = device.inN, on [device]s\n",
                    path, wire_from[i], wire_to[i]);
            return -1;
        }
        cfg.nwires++;
    }
    for (i = 0; i < cfg.nscreens; i++) {
        Screen *s = &cfg.screens[i];
        int out;
        if (!scr_video[i][0]) continue;
        if (parse_port(scr_video[i], &s->vdev, &s->vin, &out) < 0 || out) {
            fprintf(stderr, "%s: screen %s: video = %s is not an input of a [device]\n", path, s->name, scr_video[i]);
            return -1;
        }
    }
    for (i = 0; i < cfg.nmonitors; i++) {
        Monitor *m = &cfg.monitors[i];
        int out;
        if (!mon_video[i][0]) continue;
        if (parse_port(mon_video[i], &m->vdev, &m->vout, &out) < 0 || !out) {
            fprintf(stderr, "%s: monitor %s: video = %s is not an output of a [device]\n", path, m->name, mon_video[i]);
            return -1;
        }
    }

    for (i = 0; i < cfg.nmonitors; i++) {
        Monitor *m = &cfg.monitors[i];
        if (mon_fixed[i][0]) {
            m->fixed = screen_by_name(mon_fixed[i]);
            if (!m->fixed) {
                fprintf(stderr, "%s: monitor %s names unknown screen %s\n", path, m->name, mon_fixed[i]);
                return -1;
            }
        } else if (!m->vout) {
            fprintf(stderr, "%s: monitor %s needs 'screen =' or 'video ='\n", path, m->name);
            return -1;
        }
    }
    for (i = 0; i < cfg.nmonitors; i++) {
        Monitor *m = &cfg.monitors[i];
        if (mon_shared[i][0] && !(m->shared = screen_by_name(mon_shared[i]))) {
            fprintf(stderr, "%s: monitor %s shares with unknown screen %s\n", path, m->name, mon_shared[i]);
            return -1;
        }
        if (!mon_shared[i][0]) m->shared_on = 0;
    }
    for (i = 0; i < cfg.nscreens; i++) {
        Screen *s = &cfg.screens[i];
        int k;
        for (k = 0; k < s->narea; k++) {
            Monitor *m = NULL;
            s->area_mon[k] = -1;
            if (!area_mon[i][k][0]) continue;
            if (!(m = monitor_by_name(area_mon[i][k])) || !m->shared) {
                fprintf(stderr, "%s: screen %s: area names %s, which is not a shared monitor\n",
                        path, s->name, area_mon[i][k]);
                return -1;
            }
            s->area_mon[k] = (int)(m - cfg.monitors);
        }
        memcpy(s->cfg_area, s->area, sizeof s->area);
        memcpy(s->cfg_area_mon, s->area_mon, sizeof s->area_mon);
        s->cfg_narea = s->narea;
    }
    for (i = 0; i < cfg.nscreens; i++) {
        Screen *s = &cfg.screens[i];
        if (!scr_via[i][0]) continue;
        if (!(s->via = screen_by_name(scr_via[i]))) {
            fprintf(stderr, "%s: screen %s: via names unknown screen %s\n", path, s->name, scr_via[i]);
            return -1;
        }
    }
    for (i = 0; i < cfg.nscreens; i++) {     /* boxes may chain (switcher -> tink), but not loop */
        const Screen *s = &cfg.screens[i];
        int n = 0;
        while (s->via && n++ <= MAX_SCREENS) s = s->via;
        if (n > MAX_SCREENS) {
            fprintf(stderr, "%s: screen %s: its via chain loops\n", path, cfg.screens[i].name);
            return -1;
        }
    }
    if (cfg.nscreens == 0) {
        fprintf(stderr, "%s: no [screen] sections\n", path);
        return -1;
    }
    for (i = 0; i < cfg.nscreens; i++) cfg.screens[i].config_color = cfg.screens[i].color;
    {   /* the keyboard + mouse: may reach every machine until told otherwise */
        Accessory *k = acc_new("keyboard", ACC_KBM, "Keyboard + mouse");
        copy(k->about, sizeof k->about, "the USB keyboard and mouse on the hub");
        for (i = 0; i < cfg.nscreens; i++) k->player[i] = 1;
    }
    if (cfg.nseats == 0) {
        Seat *z = seat_add("main", 0x22C55E);
        z->from_config = 1;
    }
    {   /* Claude's seat and its keyboard + mouse, played by scripts on the
         * control port ("seat claude ..."); wired to every machine but the
         * hub's own desktop until told otherwise */
        Seat *z = seat_add("claude", 0xD97757);
        Accessory *k = acc_new("claude", ACC_KBM, "Claude's keyboard + mouse");
        if (!z->label[0]) copy(z->label, sizeof z->label, "Claude");
        copy(k->about, sizeof k->about, "scripts on the hub's control port (seat claude ...)");
        for (i = 0; i < cfg.nscreens; i++) k->player[i] = !cfg.screens[i].local;
        k->present = 1;
        k->seat = (int)(z - cfg.seats);
    }
    load_state();
    return 0;
}

/* Monitor positions moved from the touch panel override the config's, and
 * live in a small file of their own so the config is never rewritten. */
static void load_state(void)
{
    FILE *f = fopen(cfg.state_path, "r");
    char line[256], name[32];
    int col, row, on, k;
    unsigned char seen[MAX_ACC];        /* accessories whose links the file lists */
    int has_seats = 0;

    memset(seen, 0, sizeof seen);

    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        Monitor *m;
        if (sscanf(line, "monitor %31s %d %d", name, &col, &row) == 3 && (m = monitor_by_name(name))) {
            m->col = col;
            m->row = row;
        } else if (sscanf(line, "shape %31s %d %d %d %d", name, &col, &row, &on, &k) == 5 && (m = monitor_by_name(name)) &&
                   col >= 1 && col <= 64 && row >= 1 && row <= 64 && on >= 0 && on <= 2000) {
            m->aspect_w = col;
            m->aspect_h = row;
            m->inches10 = on;
            m->portrait = k != 0;
            m->shape_set = 1;
        } else if (sscanf(line, "shared %31s %d", name, &on) == 2 && (m = monitor_by_name(name)) && m->shared) {
            m->shared_on = on != 0;
        } else {
            unsigned rgb;
            Screen *s;
            char what[32];
            if (sscanf(line, "color %31s %x", name, &rgb) == 2 && (s = screen_by_name(name)))
                s->color = (long)(rgb & 0xFFFFFF);
            else if (sscanf(line, "plug %31s %31s", name, what) == 2 && (s = screen_by_name(what)) &&
                     s->via && !strcmp(s->via->name, name))
                s->via->plugged = s;
            else if (sscanf(line, "accessory %31s %31s %n", name, what, &col) == 2 && acc_kind(what) >= 0) {
                char *label = trim(line + col);
                Accessory *a = acc_by_name(name);
                if (!a) a = acc_new(name, acc_kind(what), label);
                if (a && !strcmp(a->name, "script")) a->scripted = 1;   /* the control port's pad (main.c) */
                if (a && !seen[a - cfg.acc]) {      /* the file says where it is linked */
                    seen[a - cfg.acc] = 1;
                    memset(a->player, 0, sizeof a->player);
                    memset(a->keep, 0, sizeof a->keep);
                    a->keep_all = 0;
                }
            }
            else if (sscanf(line, "link %31s %31s %d", name, what, &on) == 3) {
                Accessory *a = acc_by_name(name);
                if (a && (s = screen_by_name(what)) && on >= 1 && on <= MAX_PLAYERS)
                    a->player[s - cfg.screens] = (unsigned char)on;
            }
            else if (sscanf(line, "keepall %31s", name) == 1) {   /* before "keep": it matches too */
                Accessory *a = acc_by_name(name);
                if (a) a->keep_all = 1;
            }
            else if (sscanf(line, "keep %31s %31s", name, what) == 2) {
                Accessory *a = acc_by_name(name);
                if (a && (s = screen_by_name(what))) a->keep[s - cfg.screens] = 1;
            }
            /* before 2026-10-16, "always <screen> <kind>": now each accessory of
             * that kind linked there is kept there */
            else if (sscanf(line, "always %31s %31s", name, what) == 2 && (s = screen_by_name(name)) &&
                     acc_kind(what) >= 0)
            {
                int k = acc_kind(what), j;
                for (j = 0; j < cfg.nacc; j++)
                    if (cfg.acc[j].kind == k && cfg.acc[j].player[s - cfg.screens]) cfg.acc[j].keep[s - cfg.screens] = 1;
            }
            else if (sscanf(line, "acolor %31s %x", name, &rgb) == 2) {
                Accessory *a = acc_by_name(name);
                if (a) a->color = (long)(rgb & 0xFFFFFF);
            }
            else if (!strncmp(line, "seats ", 6)) has_seats = 1;
            else if (sscanf(line, "seat %31s %x %n", name, &rgb, &col) == 2) {
                Seat *z = seat_add(name, (long)(rgb & 0xFFFFFF));
                if (z) z->color = (long)(rgb & 0xFFFFFF);
                if (z && *trim(line + col)) copy(z->label, sizeof z->label, trim(line + col));
            }
            else if (sscanf(line, "aseat %31s %31s", name, what) == 2) {
                Accessory *a = acc_by_name(name);
                Seat *z = seat_by_name(what);
                if (a && z) a->seat = (int)(z - cfg.seats);
            }
        }
    }
    fclose(f);
    if (!has_seats) {   /* a state file from before seats: Claude's keyboard was kept everywhere */
        Accessory *a = acc_by_name("claude");
        if (a) {
            a->keep_all = 0;
            memset(a->keep, 0, sizeof a->keep);
        }
    }
}

int config_save_state(void)
{
    char tmp[sizeof cfg.state_path + 4];
    FILE *f;
    int i;

    snprintf(tmp, sizeof tmp, "%s.new", cfg.state_path);
    if (!(f = fopen(tmp, "w"))) {
        logmsg("cannot save %s", cfg.state_path);
        return -1;
    }
    fprintf(f, "# monitor positions set from the touch panel; delete to go back to the config\n");
    for (i = 0; i < cfg.nmonitors; i++)
        fprintf(f, "monitor %s %d %d\n", cfg.monitors[i].name, cfg.monitors[i].col, cfg.monitors[i].row);
    for (i = 0; i < cfg.nmonitors; i++)
        if (cfg.monitors[i].shared) fprintf(f, "shared %s %d\n", cfg.monitors[i].name, cfg.monitors[i].shared_on);
    for (i = 0; i < cfg.nmonitors; i++) {           /* shape aw ah inches*10 turned */
        const Monitor *m = &cfg.monitors[i];
        if (m->shape_set)
            fprintf(f, "shape %s %d %d %d %d\n", m->name, m->aspect_w, m->aspect_h, m->inches10, m->portrait);
    }
    for (i = 0; i < cfg.nscreens; i++)
        if (cfg.screens[i].color != cfg.screens[i].config_color && cfg.screens[i].color >= 0)
            fprintf(f, "color %s %06lx\n", cfg.screens[i].name, (unsigned long)cfg.screens[i].color);
    fprintf(f, "seats 1\n");                        /* seats, and whose each accessory is */
    for (i = 0; i < cfg.nseats; i++)
        fprintf(f, "seat %s %06lx %s\n", cfg.seats[i].name, (unsigned long)cfg.seats[i].color, cfg.seats[i].label);
    for (i = 0; i < cfg.nacc; i++) {                 /* accessories and where they are linked */
        const Accessory *a = &cfg.acc[i];
        int k;
        fprintf(f, "accessory %s %s %s\n", a->name, acc_kinds[a->kind].id, a->label);
        fprintf(f, "aseat %s %s\n", a->name, cfg.seats[a->seat].name);
        for (k = 0; k < cfg.nscreens; k++)
            if (a->player[k]) fprintf(f, "link %s %s %d\n", a->name, cfg.screens[k].name, a->player[k]);
        if (a->color >= 0) fprintf(f, "acolor %s %06lx\n", a->name, (unsigned long)a->color);
        for (k = 0; k < cfg.nscreens; k++)
            if (a->keep[k] && a->player[k]) fprintf(f, "keep %s %s\n", a->name, cfg.screens[k].name);
        if (a->keep_all) fprintf(f, "keepall %s\n", a->name);
    }
    for (i = 0; i < cfg.nscreens; i++)               /* which console is wired into each box */
        if (cfg.screens[i].plugged) fprintf(f, "plug %s %s\n", cfg.screens[i].name, cfg.screens[i].plugged->name);
    if (fclose(f) != 0 || rename(tmp, cfg.state_path) != 0) {
        logmsg("cannot save %s", cfg.state_path);
        return -1;
    }
    return 0;
}

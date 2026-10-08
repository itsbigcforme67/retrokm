/* INI-style configuration.  See retrokm.conf.example. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "hub.h"

Config cfg;

static void load_state(void);

static char mon_fixed[MAX_MONITORS][32];   /* resolved after the whole file is read */
static char mon_shared[MAX_MONITORS][32];
static char area_mon[MAX_SCREENS][MAX_AREAS][32];

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
    enum { S_NONE, S_HUB, S_EXTRON, S_SCREEN, S_MONITOR, S_HOTKEYS } sec = S_NONE;
    Screen *scr = NULL;
    Monitor *mon = NULL;
    int lineno = 0, i;

    if (!f) { perror(path); return -1; }

    memset(&cfg, 0, sizeof cfg);
    copy(cfg.listen, sizeof cfg.listen, "0.0.0.0");
    cfg.port = RKM_PORT;
    cfg.ctl_port = RKM_PORT + 1;
    cfg.grab = 1;
    cfg.speed = 1.0;
    cfg.accel = 0.5;
    cfg.follow_tie = 1;
    cfg.extron_baud = 9600;
    cfg.extron_poll = 5;
    cfg.extron_tie_cmd = '!';
    cfg.extron_read_cmd = '%';
    cfg.extron_inputs = 8;
    cfg.extron_outputs = 8;
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
            else if (!strcasecmp(kind, "extron")) sec = S_EXTRON;
            else if (!strcasecmp(kind, "hotkeys")) sec = S_HOTKEYS;
            else if (!strcasecmp(kind, "screen")) {
                if (!*name || cfg.nscreens >= MAX_SCREENS) goto bad;
                sec = S_SCREEN;
                scr = &cfg.screens[cfg.nscreens++];
                copy(scr->name, sizeof scr->name, name);
                scr->speed = 1.0;
                scr->min_move_ms = 8;
                scr->w = 1024;
                scr->h = 768;
            } else if (!strcasecmp(kind, "monitor")) {
                if (!*name || cfg.nmonitors >= MAX_MONITORS) goto bad;
                sec = S_MONITOR;
                mon = &cfg.monitors[cfg.nmonitors++];
                copy(mon->name, sizeof mon->name, name);
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
            else if (!strcasecmp(k, "state")) copy(cfg.state_path, sizeof cfg.state_path, v);
            else if (!strcasecmp(k, "grab")) cfg.grab = truthy(v);
            else if (!strcasecmp(k, "speed")) cfg.speed = atof(v);
            else if (!strcasecmp(k, "accel")) cfg.accel = atof(v);
            else if (!strcasecmp(k, "on_tie")) cfg.follow_tie = !strcasecmp(v, "follow");
            else if (!strcasecmp(k, "device")) {
                if (!strcasecmp(v, "auto")) cfg.ndevices = 0;
                else if (!strcasecmp(v, "none")) cfg.ndevices = -1;
                else if (cfg.ndevices >= 0 && cfg.ndevices < MAX_DEVICES)
                    copy(cfg.devices[cfg.ndevices++], sizeof cfg.devices[0], v);
                else goto bad;
            } else goto bad;
            break;
        case S_EXTRON:
            if (!strcasecmp(k, "device")) copy(cfg.extron_dev, sizeof cfg.extron_dev, v);
            else if (!strcasecmp(k, "baud")) cfg.extron_baud = atoi(v);
            else if (!strcasecmp(k, "poll")) cfg.extron_poll = atoi(v);
            else if (!strcasecmp(k, "tie_command") && *v) cfg.extron_tie_cmd = *v;
            else if (!strcasecmp(k, "read_command") && *v) cfg.extron_read_cmd = *v;
            else if (!strcasecmp(k, "inputs")) cfg.extron_inputs = atoi(v);
            else if (!strcasecmp(k, "outputs")) cfg.extron_outputs = atoi(v);
            else goto bad;
            break;
        case S_SCREEN:
            if (!strcasecmp(k, "local")) scr->local = truthy(v);
            else if (!strcasecmp(k, "label")) copy(scr->label, sizeof scr->label, v);
            else if (!strcasecmp(k, "art")) copy(scr->art, sizeof scr->art, v);
            else if (!strcasecmp(k, "agent")) scr->no_agent = !truthy(v);
            else if (!strcasecmp(k, "size")) {
                if (sscanf(v, "%dx%d", &scr->w, &scr->h) != 2) goto bad;
            }
            else if (!strcasecmp(k, "input")) scr->extron_input = atoi(v);
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
            else if (!strcasecmp(k, "output")) mon->extron_output = atoi(v);
            else if (!strcasecmp(k, "screen"))
                copy(mon_fixed[mon - cfg.monitors], sizeof mon_fixed[0], v);
            else if (!strcasecmp(k, "shared")) {
                copy(mon_shared[mon - cfg.monitors], sizeof mon_shared[0], v);
                mon->shared_on = 1;              /* until the panel says otherwise */
            }
            else if (!strcasecmp(k, "portrait")) mon->portrait = truthy(v);
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

    for (i = 0; i < cfg.nmonitors; i++) {
        Monitor *m = &cfg.monitors[i];
        if (mon_fixed[i][0]) {
            m->fixed = screen_by_name(mon_fixed[i]);
            if (!m->fixed) {
                fprintf(stderr, "%s: monitor %s names unknown screen %s\n", path, m->name, mon_fixed[i]);
                return -1;
            }
        } else if (m->extron_output < 1 || m->extron_output > EXTRON_MAX_IO) {
            fprintf(stderr, "%s: monitor %s needs 'screen =' or 'output ='\n", path, m->name);
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
    }
    if (cfg.nscreens == 0) {
        fprintf(stderr, "%s: no [screen] sections\n", path);
        return -1;
    }
    load_state();
    return 0;
}

/* Monitor positions moved from the touch panel override the config's, and
 * live in a small file of their own so the config is never rewritten. */
static void load_state(void)
{
    FILE *f = fopen(cfg.state_path, "r");
    char line[128], name[32];
    int col, row, on;

    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        Monitor *m;
        if (sscanf(line, "monitor %31s %d %d", name, &col, &row) == 3 && (m = monitor_by_name(name))) {
            m->col = col;
            m->row = row;
        } else if (sscanf(line, "shared %31s %d", name, &on) == 2 && (m = monitor_by_name(name)) && m->shared) {
            m->shared_on = on != 0;
        }
    }
    fclose(f);
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
    if (fclose(f) != 0 || rename(tmp, cfg.state_path) != 0) {
        logmsg("cannot save %s", cfg.state_path);
        return -1;
    }
    return 0;
}

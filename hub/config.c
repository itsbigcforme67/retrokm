/* INI-style configuration.  See retrokm.conf.example. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "hub.h"

Config cfg;

static char mon_fixed[MAX_MONITORS][32];   /* resolved after the whole file is read */

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
            else goto bad;
            break;
        case S_SCREEN:
            if (!strcasecmp(k, "local")) scr->local = truthy(v);
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
            else goto bad;
            break;
        case S_MONITOR:
            if (!strcasecmp(k, "pos")) {
                if (sscanf(v, "%d , %d", &mon->col, &mon->row) != 2) goto bad;
            }
            else if (!strcasecmp(k, "output")) mon->extron_output = atoi(v);
            else if (!strcasecmp(k, "screen"))
                copy(mon_fixed[mon - cfg.monitors], sizeof mon_fixed[0], v);
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
    if (cfg.nscreens == 0) {
        fprintf(stderr, "%s: no [screen] sections\n", path);
        return -1;
    }
    return 0;
}

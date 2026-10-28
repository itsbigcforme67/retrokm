/* Extron matrix switcher over RS-232, using SIS (Simple Instruction Set).
 *
 *   tie input to output     "<in>*<out>!"   ->  "Out<out> In<in> All"
 *   read an output's tie    "<out>%"        ->  "<in>"
 *   front panel changes arrive unsolicited: "Out.. In.. ..", or "Qik" when
 *   several ties changed at once, in which case every output is re-read.
 *
 * Responses to reads carry no label, so reads go out one at a time.  Each
 * switcher is a VDevice; its state lives there.  It may be on a serial port
 * or on the network (Extron's IP models take SIS over TCP port 23). */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hub.h"

static void requery_all(VDevice *d)
{
    int o;
    for (o = 1; o <= VDEV_MAX_IO; o++)
        if (d->watched[o]) d->need[o] = 1;
}

static void opened(VDevice *d) { requery_all(d); }

void extron_init(VDevice *d)
{
    d->fd = -1;
    if (!d->path[0]) return;
    if (vdev_open(d) < 0)
        logmsg("%s: cannot open %s: %s (will keep trying)", d->name, d->path, strerror(errno));
    else if (!d->connecting) opened(d);
}

void extron_tie(VDevice *d, int input, int output)
{
    char cmd[32];
    if (input < 0 || output < 1 || input > VDEV_MAX_IO || output > VDEV_MAX_IO) return;
    snprintf(cmd, sizeof cmd, "%d*%d%c", input, output, d->tie_cmd);
    vdev_write(d, cmd);
    d->need[output] = d->watched[output];      /* confirm even if the reply is lost */
}

static void set_tie(VDevice *d, int output, int input)
{
    if (output < 1 || output > VDEV_MAX_IO || d->tie[output] == input) return;
    d->tie[output] = input;
    logmsg("%s: output %d now shows input %d", d->name, output, input);
    hub_video_changed();
}

static int all_digits(const char *s)
{
    if (!*s) return 0;
    for (; *s; s++)
        if (*s < '0' || *s > '9') return 0;
    return 1;
}

static void handle_line(VDevice *d, char *s)
{
    int o, i;
    char kind[8] = "";

    if (sscanf(s, "Out%d In%d %7s", &o, &i, kind) >= 2) {
        if (strcmp(kind, "Aud") != 0) set_tie(d, o, i);
    } else if (sscanf(s, "In%d %7s", &i, kind) == 2) {  /* tied to every output */
        if (strcmp(kind, "Aud") != 0)
            for (o = 1; o <= VDEV_MAX_IO; o++)
                if (d->watched[o]) set_tie(d, o, i);
    } else if (sscanf(s, "Chn%d", &i) == 1) {           /* single-output switchers */
        set_tie(d, 1, i);
    } else if (all_digits(s)) {
        if (d->pending_out) {
            set_tie(d, d->pending_out, atoi(s));
            d->pending_out = 0;
        }
    } else if (!strncmp(s, "Qik", 3) || !strncmp(s, "Rpr", 3) || !strncmp(s, "Reconfig", 8)) {
        requery_all(d);
    } else if (s[0] == 'E' && all_digits(s + 1)) {
        logmsg("%s: switcher reported error %s", d->name, s);
        d->pending_out = 0;
    }
}

void extron_readable(VDevice *d)
{
    vdev_read(d, handle_line);
}

void extron_tick(VDevice *d)
{
    long long now = now_ms();
    int o;

    vdev_keep_open(d, opened);
    if (d->fd < 0 || d->connecting) return;
    if (d->poll > 0 && now - d->last_poll > d->poll * 1000LL) {
        d->last_poll = now;
        requery_all(d);
    }
    if (d->pending_out && now - d->pending_since > 1000) d->pending_out = 0;   /* no reply */
    if (!d->pending_out) {
        for (o = 1; o <= VDEV_MAX_IO; o++) {
            if (d->need[o]) {
                char cmd[16];
                snprintf(cmd, sizeof cmd, "%d%c", o, d->read_cmd);
                d->need[o] = 0;
                d->pending_out = o;
                d->pending_since = now;
                vdev_write(d, cmd);
                break;
            }
        }
    }
}

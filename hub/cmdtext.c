/* Any switcher that takes text commands over serial or TCP: the commands
 * come from its device file (devices/<model>.conf) or its [device]
 * section, so a new box is a few lines of text, not C.
 *
 *   tie   = s in {in} av out {out}!\r\n      ties an input to an output
 *   untie = s out {out} off!\r\n             (optional) shows nothing there
 *   read  = r av out {out}!\r\n              (optional) asks what an output shows
 *   reply = input {in} -> output {out}       a line that says a tie
 *
 * Escapes are as in C (\r \n \t \\ \xHH).  In "reply", {in} and {out} match
 * numbers, a space matches any run of spaces, * matches anything, letters
 * match either case, and the pattern may start anywhere in the line.  A box
 * that never says what it did (no reply) is taken at its word: the hub
 * remembers the tie it sent. */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hub.h"

/* fmt with {in} / {out} filled in and escapes turned into bytes */
static void expand(const char *fmt, int in, int out, char *buf, size_t n)
{
    size_t k = 0;
    while (*fmt && k + 12 < n) {
        if (!strncmp(fmt, "{in}", 4)) { k += (size_t)snprintf(buf + k, n - k, "%d", in); fmt += 4; }
        else if (!strncmp(fmt, "{out}", 5)) { k += (size_t)snprintf(buf + k, n - k, "%d", out); fmt += 5; }
        else if (*fmt == '\\' && fmt[1]) {
            fmt++;
            switch (*fmt) {
            case 'r': buf[k++] = '\r'; break;
            case 'n': buf[k++] = '\n'; break;
            case 't': buf[k++] = '\t'; break;
            case 'x': {
                char hex[3] = { 0, 0, 0 };
                if (isxdigit((unsigned char)fmt[1])) hex[0] = *++fmt;
                if (hex[0] && isxdigit((unsigned char)fmt[1])) hex[1] = *++fmt;
                buf[k++] = (char)strtol(hex, NULL, 16);
                break;
            }
            default: buf[k++] = *fmt;
            }
            fmt++;
        } else buf[k++] = *fmt++;
    }
    buf[k] = 0;
}

/* Does fmt match line starting exactly at s?  Fills *in / *out. */
static int match_at(const char *f, const char *s, int *in, int *out)
{
    while (*f) {
        if (!strncmp(f, "{in}", 4) || !strncmp(f, "{out}", 5)) {
            int *v = f[1] == 'i' ? in : out;
            if (!isdigit((unsigned char)*s)) return 0;
            *v = (int)strtol(s, (char **)&s, 10);
            f += f[1] == 'i' ? 4 : 5;
        } else if (*f == '*') {
            const char *t;
            f++;
            for (t = s; ; t++) {
                if (match_at(f, t, in, out)) return 1;
                if (!*t) return 0;
            }
        } else if (*f == ' ') {
            if (!isspace((unsigned char)*s)) return 0;
            while (isspace((unsigned char)*s)) s++;
            while (*f == ' ') f++;
        } else {
            char c = *f;
            if (c == '\\' && f[1]) c = *++f;
            if (tolower((unsigned char)c) != tolower((unsigned char)*s)) return 0;
            f++;
            s++;
        }
    }
    return 1;
}

int cmdtext_match(const char *fmt, const char *line, int *in, int *out)
{
    const char *s;
    for (s = line; ; s++) {
        int i = -1, o = -1;
        if (match_at(fmt, s, &i, &o)) { *in = i; *out = o; return 1; }
        if (!*s) return 0;
    }
}

static void set_tie(VDevice *d, int output, int input)
{
    if (output < 1 || output > VDEV_MAX_IO || input < 0 || input > VDEV_MAX_IO || d->tie[output] == input) return;
    d->tie[output] = input;
    logmsg("%s: output %d now shows input %d", d->name, output, input);
    hub_video_changed();
}

static void handle_line(VDevice *d, char *s)
{
    int in, out;
    if (!d->reply_fmt[0] || !cmdtext_match(d->reply_fmt, s, &in, &out)) return;
    if (out < 0) out = d->pending_out;          /* a reply that does not say which output */
    if (in < 0 || out < 1) return;
    if (out == d->pending_out) d->pending_out = 0;
    set_tie(d, out, in);
}

static void requery_all(VDevice *d)
{
    int o;
    for (o = 1; o <= VDEV_MAX_IO; o++)
        if (d->watched[o]) d->need[o] = 1;
}

static void opened(VDevice *d)
{
    if (!strncasecmp(d->path, "tcp:", 4)) logmsg("%s: talking to %s", d->name, d->path + 4);
    requery_all(d);
}

void cmdtext_init(VDevice *d)
{
    d->fd = -1;
    if (!d->path[0]) return;
    if (vdev_open(d) < 0)
        logmsg("%s: cannot open %s: %s (will keep trying)", d->name, d->path, strerror(errno));
    else if (!d->connecting) opened(d);
}

void cmdtext_readable(VDevice *d)
{
    vdev_read(d, handle_line);
}

int cmdtext_tie(VDevice *d, int input, int output)
{
    char cmd[160];
    const char *fmt = input ? d->tie_fmt : d->untie_fmt;
    if (output < 1 || output > d->outputs || input < 0 || input > d->inputs) return -1;
    if (!fmt[0]) return -1;
    expand(fmt, input, output, cmd, sizeof cmd);
    vdev_write(d, cmd);
    if (!d->reply_fmt[0]) set_tie(d, output, input);   /* it will not say: take it at its word */
    else d->need[output] = d->watched[output] && d->read_fmt[0];
    return 0;
}

void cmdtext_tick(VDevice *d)
{
    long long now = now_ms();
    int o;

    vdev_keep_open(d, opened);
    if (d->fd < 0 || d->connecting || !d->read_fmt[0]) return;
    if (d->poll > 0 && now - d->last_poll > d->poll * 1000LL) {
        d->last_poll = now;
        requery_all(d);
    }
    if (d->pending_out && now - d->pending_since > 1000) d->pending_out = 0;   /* no reply */
    if (!d->pending_out) {
        for (o = 1; o <= d->outputs; o++) {
            if (d->need[o]) {
                char cmd[160];
                expand(d->read_fmt, 0, o, cmd, sizeof cmd);
                d->need[o] = 0;
                d->pending_out = o;
                d->pending_since = now;
                vdev_write(d, cmd);
                break;
            }
        }
    }
}

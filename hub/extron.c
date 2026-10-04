/* Extron matrix switcher over RS-232, using SIS (Simple Instruction Set).
 *
 *   tie input to output     "<in>*<out>!"   ->  "Out<out> In<in> All"
 *   read an output's tie    "<out>%"        ->  "<in>"
 *   front panel changes arrive unsolicited: "Out.. In.. ..", or "Qik" when
 *   several ties changed at once, in which case every output is re-read.
 *
 * Responses to reads carry no label, so reads go out one at a time. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include "hub.h"

static int fd = -1;
static int tie[EXTRON_MAX_IO + 1];         /* tie[output] = input, 0 unknown */
static unsigned char watched[EXTRON_MAX_IO + 1], need[EXTRON_MAX_IO + 1];
static int pending_out;
static long long pending_since, last_poll, last_open_try;
static char lbuf[128];
static int llen;

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

static void requery_all(void)
{
    int o;
    for (o = 1; o <= EXTRON_MAX_IO; o++)
        if (watched[o]) need[o] = 1;
}

static int port_open(void)
{
    struct termios t;

    fd = open(cfg.extron_dev, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    if (tcgetattr(fd, &t) == 0) {
        cfmakeraw(&t);
        cfsetspeed(&t, baud_const(cfg.extron_baud));
        t.c_cflag |= CLOCAL | CREAD;
        t.c_cflag &= ~(tcflag_t)(CRTSCTS | CSTOPB | PARENB);
        tcsetattr(fd, TCSANOW, &t);
    }
    tcflush(fd, TCIOFLUSH);
    llen = 0;
    pending_out = 0;
    requery_all();
    logmsg("extron: opened %s at %d baud", cfg.extron_dev, cfg.extron_baud);
    return 0;
}

static void port_close(void)
{
    logmsg("extron: lost %s", cfg.extron_dev);
    close(fd);
    fd = -1;
}

void extron_watch(int output)
{
    if (output >= 1 && output <= EXTRON_MAX_IO) watched[output] = need[output] = 1;
}

int extron_init(void)
{
    if (!cfg.extron_dev[0]) return 0;
    last_open_try = now_ms();
    if (port_open() < 0)
        logmsg("extron: cannot open %s: %s (will keep trying)", cfg.extron_dev, strerror(errno));
    return 0;
}

int extron_fd(void) { return fd; }

int extron_input_for(int output)
{
    return (output >= 1 && output <= EXTRON_MAX_IO) ? tie[output] : 0;
}

static void port_write(const char *s)
{
    if (fd < 0) return;
    if (verbose) logmsg("extron: > %s", s);
    if (write(fd, s, strlen(s)) < 0 && errno != EAGAIN) port_close();
}

void extron_tie(int input, int output)
{
    char cmd[32];
    if (input < 1 || output < 1 || input > EXTRON_MAX_IO || output > EXTRON_MAX_IO) return;
    snprintf(cmd, sizeof cmd, "%d*%d%c", input, output, cfg.extron_tie_cmd);
    port_write(cmd);
    need[output] = watched[output];            /* confirm even if the reply is lost */
}

static void set_tie(int output, int input)
{
    if (output < 1 || output > EXTRON_MAX_IO || tie[output] == input) return;
    tie[output] = input;
    logmsg("extron: output %d now shows input %d", output, input);
    hub_extron_changed();
}

static int all_digits(const char *s)
{
    if (!*s) return 0;
    for (; *s; s++)
        if (*s < '0' || *s > '9') return 0;
    return 1;
}

static void handle_line(char *s)
{
    int o, i;
    char kind[8] = "";

    if (verbose) logmsg("extron: < %s", s);

    if (sscanf(s, "Out%d In%d %7s", &o, &i, kind) >= 2) {
        if (strcmp(kind, "Aud") != 0) set_tie(o, i);
    } else if (sscanf(s, "In%d %7s", &i, kind) == 2) {  /* tied to every output */
        if (strcmp(kind, "Aud") != 0)
            for (o = 1; o <= EXTRON_MAX_IO; o++)
                if (watched[o]) set_tie(o, i);
    } else if (sscanf(s, "Chn%d", &i) == 1) {           /* single-output switchers */
        set_tie(1, i);
    } else if (all_digits(s)) {
        if (pending_out) {
            set_tie(pending_out, atoi(s));
            pending_out = 0;
        }
    } else if (!strncmp(s, "Qik", 3) || !strncmp(s, "Rpr", 3) || !strncmp(s, "Reconfig", 8)) {
        requery_all();
    } else if (s[0] == 'E' && all_digits(s + 1)) {
        logmsg("extron: switcher reported error %s", s);
        pending_out = 0;
    }
}

void extron_readable(void)
{
    char buf[128];
    int n, k;

    if (fd < 0) return;
    n = (int)read(fd, buf, sizeof buf);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) { port_close(); return; }
    for (k = 0; k < n; k++) {
        char c = buf[k];
        if (c == '\r' || c == '\n') {
            lbuf[llen] = 0;
            if (llen) handle_line(lbuf);
            llen = 0;
        } else if (llen < (int)sizeof lbuf - 1) {
            lbuf[llen++] = c;
        }
    }
}

void extron_tick(void)
{
    long long now = now_ms();
    int o;

    if (!cfg.extron_dev[0]) return;
    if (fd < 0) {
        if (now - last_open_try > 5000) { last_open_try = now; port_open(); }
        return;
    }
    if (cfg.extron_poll > 0 && now - last_poll > cfg.extron_poll * 1000LL) {
        last_poll = now;
        requery_all();
    }
    if (pending_out && now - pending_since > 1000) pending_out = 0;   /* no reply */
    if (!pending_out) {
        for (o = 1; o <= EXTRON_MAX_IO; o++) {
            if (need[o]) {
                char cmd[16];
                snprintf(cmd, sizeof cmd, "%d%c", o, cfg.extron_read_cmd);
                need[o] = 0;
                pending_out = o;
                pending_since = now;
                port_write(cmd);
                break;
            }
        }
    }
}

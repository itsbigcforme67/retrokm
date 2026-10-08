/* DDC/CI: talk to a monitor over its video cable's I2C wires, to read and
 * set which input it shows (MCCS VCP feature 0x60: VGA 1, DVI 3, DP 15,
 * HDMI 17).  Used for a shared monitor, one with the laptop on one input and
 * the switcher on another, so the hub can follow and flip it.
 *
 * I2C is slow (each exchange needs ~50 ms of quiet), so a worker thread does
 * the talking and the main loop only picks up results. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include "hub.h"

#define DDC_ADDR   0x37
#define EDID_ADDR  0x50
#define VCP_INPUT  0x60
#define POLL_MS    3000

typedef struct {
    int bus;                      /* /dev/i2c-N, -1 not found */
    int want;                     /* input to set, -1 none (main -> worker) */
    int value;                    /* last input read, -1 unknown (worker -> main) */
    int ok;                       /* the monitor answered the last read */
    int changed;                  /* worker has news for main */
} Link;

static Link links[MAX_MONITORS];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int wake_pipe[2] = { -1, -1 };
static int nlinks;

static void nap(int ms) { usleep((useconds_t)ms * 1000); }

/* ---- I2C ---------------------------------------------------------------- */

static int bus_open(int bus, int addr)
{
    char path[32];
    int fd;
    snprintf(path, sizeof path, "/dev/i2c-%d", bus);
    if ((fd = open(path, O_RDWR | O_CLOEXEC)) < 0) return -1;
    if (ioctl(fd, I2C_SLAVE, addr) < 0) { close(fd); return -1; }
    return fd;
}

static int read_edid(int bus, unsigned char out[128])
{
    unsigned char off = 0;
    int fd = bus_open(bus, EDID_ADDR), ok;
    if (fd < 0) return -1;
    ok = write(fd, &off, 1) == 1 && read(fd, out, 128) == 128;
    close(fd);
    return ok ? 0 : -1;
}

/* Does this EDID name the monitor "model" (its 0xFC descriptor)? */
static int edid_names(const unsigned char *e, const char *model)
{
    int i;
    for (i = 54; i < 126; i += 18) {
        char nm[14];
        int k;
        if (e[i] || e[i + 1] || e[i + 3] != 0xFC) continue;
        memcpy(nm, e + i + 5, 13);
        nm[13] = 0;
        for (k = 0; k < 13; k++) if (nm[k] == '\n') nm[k] = 0;
        if (!strcasecmp(nm, model)) return 1;
    }
    return 0;
}

/* Which /dev/i2c-N reaches the monitor.  "ddc =" is a DRM connector (the
 * bus whose EDID matches the kernel's copy), "model:NAME" (the bus whose
 * EDID carries that monitor name; survives the GPUs being renumbered), or
 * /dev/i2c-N. */
static int find_bus(const char *connector)
{
    const char *model = !strncmp(connector, "model:", 6) ? connector + 6 : NULL;
    char path[320], name[128];
    unsigned char want[128], got[128];
    FILE *f;
    DIR *d;
    struct dirent *e;
    int bus = -1;

    if (!strncmp(connector, "/dev/i2c-", 9)) return atoi(connector + 9);
    if (!model) {
        snprintf(path, sizeof path, "/sys/class/drm/%s/edid", connector);
        if (!(f = fopen(path, "rb"))) return -1;
        if (fread(want, 1, 128, f) != 128) { fclose(f); return -1; }
        fclose(f);
    }
    if (!(d = opendir("/sys/bus/i2c/devices"))) return -1;
    while (bus < 0 && (e = readdir(d))) {
        int n;
        if (sscanf(e->d_name, "i2c-%d", &n) != 1) continue;
        snprintf(path, sizeof path, "/sys/bus/i2c/devices/%s/name", e->d_name);
        name[0] = 0;
        if ((f = fopen(path, "r"))) { if (!fgets(name, sizeof name, f)) name[0] = 0; fclose(f); }
        /* only video buses: 0x50 on an SMBus is the RAM's SPD chip */
        if (strstr(name, "SMBus") || strstr(name, "DesignWare") || strstr(name, "MSFT")) continue;
        if (read_edid(n, got) == 0 && (model ? edid_names(got, model) : !memcmp(want, got, 128))) bus = n;
    }
    closedir(d);
    return bus;
}

static unsigned char xsum(const unsigned char *p, int n, unsigned char seed)
{
    while (n--) seed ^= *p++;
    return seed;
}

static int vcp_get(int bus, int code)
{
    unsigned char q[5] = { 0x51, 0x82, 0x01, (unsigned char)code, 0 }, r[16];
    int fd, n, i, tries;

    q[4] = xsum(q, 4, 0x6E);
    for (tries = 0; tries < 3; tries++) {
        if ((fd = bus_open(bus, DDC_ADDR)) < 0) return -1;
        n = -1;
        if (write(fd, q, 5) == 5) {
            nap(50);
            n = (int)read(fd, r, sizeof r);
        }
        close(fd);
        /* reply: [6E] 88 02 result code type maxH maxL curH curL chk */
        for (i = 0; n > 0 && i + 8 < n; i++)
            if (r[i] == 0x88 && r[i + 1] == 0x02 && r[i + 2] == 0x00 && r[i + 3] == code)
                return (r[i + 7] << 8) | r[i + 8];
        nap(60);
    }
    return -1;
}

static int vcp_set(int bus, int code, int value)
{
    unsigned char q[7] = { 0x51, 0x84, 0x03, (unsigned char)code, (unsigned char)(value >> 8),
                           (unsigned char)value, 0 };
    int fd, ok;

    q[6] = xsum(q, 6, 0x6E);
    if ((fd = bus_open(bus, DDC_ADDR)) < 0) return -1;
    ok = write(fd, q, 7) == 7;
    close(fd);
    nap(60);
    return ok ? 0 : -1;
}

/* ---- worker ------------------------------------------------------------- */

static void *worker(void *arg)
{
    long long next_poll = 0;
    (void)arg;

    for (;;) {
        int i, any_set = 0;
        long long now = now_ms();

        for (i = 0; i < cfg.nmonitors; i++) {
            Link *l = &links[i];
            int want, v;
            if (l->bus < 0) continue;
            pthread_mutex_lock(&mu);
            want = l->want;
            l->want = -1;
            pthread_mutex_unlock(&mu);
            if (want >= 0) {
                any_set = 1;
                if (vcp_set(l->bus, VCP_INPUT, want) < 0) logmsg("ddc: %s: could not send", cfg.monitors[i].name);
                nap(1500);                    /* the monitor takes a moment to switch */
            }
            if (want < 0 && now < next_poll) continue;
            v = vcp_get(l->bus, VCP_INPUT);
            pthread_mutex_lock(&mu);
            if ((v >= 0) != l->ok || (v >= 0 && (v & 0xFF) != l->value)) {
                l->ok = v >= 0;
                if (v >= 0) l->value = v & 0xFF;
                l->changed = 1;
                if (wake_pipe[1] >= 0 && write(wake_pipe[1], "!", 1) < 0) { /* full: fine */ }
            }
            pthread_mutex_unlock(&mu);
        }
        if (!any_set && now >= next_poll) next_poll = now + POLL_MS;

        pthread_mutex_lock(&mu);
        {
            int pending = 0;
            for (i = 0; i < cfg.nmonitors; i++) if (links[i].want >= 0) pending = 1;
            if (!pending) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 500 * 1000000L;
                if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
                pthread_cond_timedwait(&cv, &mu, &ts);
            }
        }
        pthread_mutex_unlock(&mu);
    }
    return NULL;
}

/* ---- main-thread side --------------------------------------------------- */

int ddc_init(void)
{
    pthread_t t;
    int i;

    for (i = 0; i < MAX_MONITORS; i++) {
        links[i].bus = -1;
        links[i].want = links[i].value = -1;
    }
    for (i = 0; i < cfg.nmonitors; i++) {
        Monitor *m = &cfg.monitors[i];
        if (!m->ddc[0]) continue;
        links[i].bus = find_bus(m->ddc);
        if (links[i].bus < 0) {
            logmsg("ddc: %s: no I2C bus found for %s", m->name, m->ddc);
            continue;
        }
        logmsg("ddc: %s on /dev/i2c-%d (%s)", m->name, links[i].bus, m->ddc);
        nlinks++;
    }
    if (!nlinks) return 0;
    if (pipe2(wake_pipe, O_NONBLOCK | O_CLOEXEC) < 0) return -1;
    return pthread_create(&t, NULL, worker, NULL) == 0 ? 0 : -1;
}

int ddc_fd(void) { return wake_pipe[0]; }

void ddc_set_input(Monitor *m, int input)
{
    int i = (int)(m - cfg.monitors);
    if (links[i].bus < 0 || input <= 0) return;
    pthread_mutex_lock(&mu);
    links[i].want = input;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
    logmsg("ddc: %s: switching to input %d", m->name, input);
}

/* For the status and panel: 1 answering, 0 silent, -1 no DDC for it */
int ddc_state(const Monitor *m, int *input)
{
    int i = (int)(m - cfg.monitors), ok;
    if (links[i].bus < 0) return -1;
    pthread_mutex_lock(&mu);
    ok = links[i].ok;
    if (input) *input = links[i].value;
    pthread_mutex_unlock(&mu);
    return ok;
}

/* Drain the wake pipe and report each monitor whose input changed. */
void ddc_readable(void (*changed)(Monitor *m, int ok, int input))
{
    char buf[64];
    int i;

    while (read(wake_pipe[0], buf, sizeof buf) > 0) { }
    for (i = 0; i < cfg.nmonitors; i++) {
        int ch, ok, v;
        pthread_mutex_lock(&mu);
        ch = links[i].changed;
        links[i].changed = 0;
        ok = links[i].ok;
        v = links[i].value;
        pthread_mutex_unlock(&mu);
        if (ch) changed(&cfg.monitors[i], ok, v);
    }
}

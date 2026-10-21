/* RetroKM hub: owns the keyboard, the mouse, the pointer position and the
 * clipboard, and routes them to whichever machine the pointer is on. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/input-event-codes.h>
#include "hub.h"

#define MAX_CLIP      (16u * 1024 * 1024)
#define MAX_OUTBUF    (24u * 1024 * 1024)
#define PING_MS       5000
#define DEAD_MS       20000
#define HELLO_MS      10000

int verbose;

static Conn conns[MAX_CONNS];
static int lfd = -1, cfd = -1, pfd_listen = -1;
static volatile sig_atomic_t quit;
static int restart;                        /* "reset": start over once stopped */

static Screen *active;           /* screen receiving input, or NULL */
static Monitor *active_mon;      /* monitor the pointer is on, or NULL */
static double px, py;            /* pointer, in the active screen's pixels */
static long long ptr_moved_ms;   /* when the mouse last moved */
static double wiggle;            /* how much the mouse swung about lately */
static long long wiggle_ms;

/* Wiggling the mouse gets lil' C's eye.  Loosely, so a trackball needs no
 * clean reversals: the motion is cut into short strokes, and each one adds
 * how far it swung from the one before (a full about-turn = 1; under 30
 * degrees is just a wobbly line).  It fades over about 1.5 s: a shake scores
 * ~8, a slow sloppy wave ~4, ordinary pointing up to ~2. */
#define WIGGLE_STROKE 10         /* counts of motion that make one stroke */
static double wiggle_now(long long now)
{
    return wiggle * exp(-(now - wiggle_ms) / 1500.0);
}

static void note_wiggle(int dx, int dy, long long now)
{
    static double ax, ay, lx, ly;    /* the stroke so far, the last one */
    static long long last;
    if (now - last > 300) ax = ay = lx = ly = 0;   /* it rested: start afresh */
    last = now;
    ax += dx;
    ay += dy;
    if (ax * ax + ay * ay < WIGGLE_STROKE * WIGGLE_STROKE) return;
    if (lx || ly) {
        double c = (ax * lx + ay * ly) / sqrt((ax * ax + ay * ay) * (lx * lx + ly * ly));
        double turn = acos(c > 1 ? 1 : c < -1 ? -1 : c) / M_PI;   /* 0..1 of an about-turn */
        if (turn > 30.0 / 180) {
            wiggle = wiggle_now(now) + turn;
            wiggle_ms = now;
        }
    }
    lx = ax;
    ly = ay;
    ax = ay = 0;
}
static double rel_fx, rel_fy;    /* fractional remainder for relative agents */
static unsigned char mods;       /* physical modifier state, HID bit order */
static int buttons;              /* physical mouse buttons held (bit per button) */
static int locked;               /* edge switching disabled */
static int touched;              /* any input yet; until then the first screen wins */
static long long active_lost_ms; /* the active screen's agent dropped: when */
#define REJOIN_MS 4000           /* how long to wait for it to come back */
static unsigned char swallowed[256];

static unsigned char *clip;      /* UTF-8, LF line endings */
static size_t clip_len;
static unsigned clip_gen;
static Screen *clip_src;

/* ---- utilities ---------------------------------------------------------- */

long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void logmsg(const char *fmt, ...)
{
    va_list ap;
    time_t t = time(NULL);
    char stamp[16];

    strftime(stamp, sizeof stamp, "%H:%M:%S", localtime(&t));
    fprintf(stderr, "%s ", stamp);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

Screen *screen_by_name(const char *name)
{
    int i;
    for (i = 0; i < cfg.nscreens; i++)
        if (!strcasecmp(cfg.screens[i].name, name)) return &cfg.screens[i];
    return NULL;
}

Monitor *monitor_by_name(const char *name)
{
    int i;
    for (i = 0; i < cfg.nmonitors; i++)
        if (!strcasecmp(cfg.monitors[i].name, name)) return &cfg.monitors[i];
    return NULL;
}

static int has_pads(const Screen *s)
{
    int k;
    for (k = 0; k < MAX_PLAYERS; k++)
        if (s->pads[k]) return 1;
    return 0;
}

static int ready(const Screen *s)
{
    return s && !s->no_agent && (s->local || s->in || s->kbd || has_pads(s));
}

int screen_is_desktop(const Screen *s)
{
    return s->local || (s->in && !strncmp(s->in->peer, "127.0.0.1", 9));
}

/* The keyboard + mouse may reach this machine (a line on the panel) */
static int kbm_ok(const Screen *s)
{
    return s && cfg.nacc > 0 && cfg.acc[0].player[s - cfg.screens];
}

/* Does s get this accessory even while out of focus? (linked, and kept there
 * or kept everywhere) */
static int acc_keeps(const Accessory *a, const Screen *s)
{
    int i = (int)(s - cfg.screens);
    return a->player[i] && (a->keep_all || a->keep[i]);
}

/* Does s get some pad out of focus? (then its dongles keep the pads) */
static int pads_keep(const Screen *s)
{
    int i;
    for (i = 0; i < cfg.nacc; i++)
        if (cfg.acc[i].kind == ACC_PAD && acc_keeps(&cfg.acc[i], s)) return 1;
    return 0;
}

static int is_rel(const Screen *s)
{
    return !s->local && s->in && (s->in->caps & RKM_CAP_REL);
}

/* ---- connections -------------------------------------------------------- */

static void conn_flush(Conn *c)
{
    while (c->out_len > 0 && !c->dead) {
        ssize_t n = send(c->fd, c->out, c->out_len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) c->dead = 1;
            return;
        }
        memmove(c->out, c->out + n, c->out_len - (size_t)n);
        c->out_len -= (size_t)n;
    }
}

static void conn_write(Conn *c, const void *data, size_t n)
{
    if (c->dead) return;
    if (c->out_len + n > c->out_cap) {
        size_t cap = (c->out_len + n) * 2 + 256;
        unsigned char *p;
        if (cap > MAX_OUTBUF || !(p = realloc(c->out, cap))) {
            logmsg("%s: output backlog too large, dropping", c->peer);
            c->dead = 1;
            return;
        }
        c->out = p;
        c->out_cap = cap;
    }
    memcpy(c->out + c->out_len, data, n);
    c->out_len += n;
    conn_flush(c);
}

static void send_frame(Conn *c, int type, const unsigned char *payload, unsigned len)
{
    unsigned char buf[RKM_HDR + RKM_MAX_PAYLOAD];
    if (!c) return;
    conn_write(c, buf, rkm_pack(buf, type, payload, len));
}

static void ctl_printf(Conn *c, const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (c) conn_write(c, buf, strlen(buf));
    else {
        size_t n = strlen(buf);
        if (n && buf[n - 1] == '\n') buf[n - 1] = 0;
        logmsg("%s", buf);
    }
}

/* ---- pointer delivery --------------------------------------------------- */

static void flush_move(Screen *s)
{
    unsigned char p[4];

    if (!s || !s->move_pending) return;
    s->move_pending = 0;
    s->last_move_ms = now_ms();
    if (s->local) {
        uinput_abs((int)px, (int)py, s->w, s->h);
    } else if (is_rel(s)) {
        RKM_PUT16(p, (unsigned)s->rel_dx & 0xFFFF);
        RKM_PUT16(p + 2, (unsigned)s->rel_dy & 0xFFFF);
        s->rel_dx = s->rel_dy = 0;
        send_frame(s->in, RKM_MOVEREL, p, 4);
    } else if (s->in) {
        RKM_PUT16(p, (unsigned)px);
        RKM_PUT16(p + 2, (unsigned)py);
        send_frame(s->in, RKM_MOVE, p, 4);
    }
}

static void queue_move(Screen *s)
{
    s->move_pending = 1;
    if (now_ms() - s->last_move_ms >= s->min_move_ms) flush_move(s);
}

/* ---- clipboard ---------------------------------------------------------- */

static void clip_send(Screen *s)
{
    Conn *c = s->clip;
    unsigned char *buf, hdr[5];
    size_t n, off;

    s->clip_gen = clip_gen;
    if (!c || !clip) return;
    buf = text_from_utf8(clip, clip_len, c->charset, c->eol, &n);
    if (!buf) return;
    if (n > (size_t)c->clipmax_kb * 1024) {
        logmsg("clipboard: %lu bytes is more than %s accepts, not sent", (unsigned long)n, s->name);
        free(buf);
        return;
    }
    hdr[0] = RKM_CLIP_TEXT;
    RKM_PUT32(hdr + 1, (unsigned long)n);
    send_frame(c, RKM_CLIP_BEGIN, hdr, 5);
    for (off = 0; off < n; off += RKM_CLIP_CHUNK) {
        size_t k = n - off < RKM_CLIP_CHUNK ? n - off : RKM_CLIP_CHUNK;
        send_frame(c, RKM_CLIP_DATA, buf + off, (unsigned)k);
    }
    send_frame(c, RKM_CLIP_END, NULL, 0);
    free(buf);
    if (verbose) logmsg("clipboard: sent %lu bytes to %s", (unsigned long)n, s->name);
}

static void clip_sync(Screen *s)
{
    if (s && s->clip && clip && s != clip_src && s->clip_gen != clip_gen) clip_send(s);
}

static void clip_received(Conn *c)
{
    size_t n;
    unsigned char *u = text_to_utf8(c->clip_in, c->clip_in_len, c->charset, &n);

    free(c->clip_in);
    c->clip_in = NULL;
    c->clip_in_active = 0;
    if (!u) return;
    free(clip);
    clip = u;
    clip_len = n;
    clip_gen++;
    clip_src = c->screen;
    c->screen->clip_gen = clip_gen;
    logmsg("clipboard: %lu bytes from %s", (unsigned long)n, c->screen->name);
    clip_sync(active);           /* everyone else gets it when the pointer arrives */
}

/* ---- layout ------------------------------------------------------------- */

enum { DIR_LEFT = 1, DIR_RIGHT, DIR_UP, DIR_DOWN };

static void resolve_monitors(void)
{
    int i, k;
    for (i = 0; i < cfg.nmonitors; i++) {
        Monitor *m = &cfg.monitors[i];
        int in;
        if (m->fixed) { m->cur = m->fixed; continue; }
        if (m->shared && m->shared_on) { m->cur = m->shared; continue; }
        m->cur = NULL;
        in = extron_input_for(m->extron_output);
        if (in <= 0) continue;
        /* several screens may share an input (dual boot): prefer the live one */
        for (k = 0; k < cfg.nscreens; k++) {
            Screen *s = &cfg.screens[k];
            if (s->extron_input != in) continue;
            if (!m->cur || (!ready(m->cur) && ready(s))) m->cur = s;
        }
    }
    /* a box (the RetroTINK, a switcher in front of it) shows whichever
     * console is wired into it, through any number of boxes */
    for (i = 0; i < cfg.nmonitors; i++) {
        Monitor *m = &cfg.monitors[i];
        for (k = 0; m->cur && m->cur->plugged && k < MAX_SCREENS; k++) m->cur = m->cur->plugged;
    }
}

static Monitor *monitor_showing(const Screen *s)
{
    int i;
    for (i = 0; s && i < cfg.nmonitors; i++)   /* capture cards are not on the desk */
        if (cfg.monitors[i].cur == s && !cfg.monitors[i].capture) return &cfg.monitors[i];
    return NULL;
}

static Monitor *neighbor(const Monitor *m, int dir)
{
    Monitor *best = NULL;
    int i;

    for (i = 0; i < cfg.nmonitors; i++) {
        Monitor *k = &cfg.monitors[i];
        if (k == m || k->capture || !ready(k->cur) || k->cur == active || !kbm_ok(k->cur)) continue;
        switch (dir) {
        case DIR_LEFT:
            if (k->row == m->row && k->col < m->col && (!best || k->col > best->col)) best = k;
            break;
        case DIR_RIGHT:
            if (k->row == m->row && k->col > m->col && (!best || k->col < best->col)) best = k;
            break;
        case DIR_UP:
            if (k->col == m->col && k->row < m->row && (!best || k->row > best->row)) best = k;
            break;
        case DIR_DOWN:
            if (k->col == m->col && k->row > m->row && (!best || k->row < best->row)) best = k;
            break;
        }
    }
    return best;
}

/* ---- visible areas ------------------------------------------------------ */

/* An area tied to a shared monitor only counts while that monitor shows
 * this screen. */
static int area_on(const Screen *s, int i)
{
    return s->area_mon[i] < 0 || cfg.monitors[s->area_mon[i]].shared_on;
}

static int has_areas(const Screen *s)
{
    int i;
    for (i = 0; i < s->narea; i++)
        if (area_on(s, i)) return 1;
    return 0;
}

/* Index of the area holding x,y; -1 if none.  No areas: all of it. */
static int area_at(const Screen *s, double x, double y)
{
    int i;
    for (i = 0; i < s->narea; i++) {
        const int *a = s->area[i];
        if (!area_on(s, i)) continue;
        if (x >= a[0] && x <= a[0] + a[2] - 1 && y >= a[1] && y <= a[1] + a[3] - 1) return i;
    }
    return has_areas(s) ? -1 : 0;
}

static double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* Keep the pointer out of the parts of an irregular desktop (monitors of
 * different sizes) that no monitor shows.  The pointer slides along the
 * edge it ran into, as it would on the machine itself. */
static void fit_area(const Screen *s, double *x, double *y, double ox, double oy)
{
    double best = -1;
    int i;

    if (area_at(s, *x, *y) >= 0) return;
    if ((i = area_at(s, *x, oy)) >= 0) {
        *y = clampd(*y, s->area[i][1], s->area[i][1] + s->area[i][3] - 1);
        return;
    }
    if ((i = area_at(s, ox, *y)) >= 0) {
        *x = clampd(*x, s->area[i][0], s->area[i][0] + s->area[i][2] - 1);
        return;
    }
    for (i = 0; i < s->narea; i++) {            /* last resort: nearest area */
        const int *a = s->area[i];
        if (!area_on(s, i)) continue;
        double cx = clampd(*x, a[0], a[0] + a[2] - 1), cy = clampd(*y, a[1], a[1] + a[3] - 1);
        double d = (cx - *x) * (cx - *x) + (cy - *y) * (cy - *y);
        if (best < 0 || d < best) { best = d; ox = cx; oy = cy; }
    }
    *x = ox;
    *y = oy;
}

/* Where the pointer lands on screen t when it arrives moving in direction
 * dir: on the side it came in from, at the same fraction along the edge. */
static void entry_point(const Screen *t, int dir, double fx, double fy, double *tx, double *ty)
{
    int r[4] = { 0, 0, t->w, t->h }, i, best = -1;

    for (i = 0; i < t->narea; i++) {      /* the area that edge belongs to */
        const int *a = t->area[i];
        if (!area_on(t, i)) continue;
        if (best < 0 ||
            (dir == DIR_RIGHT && a[0] < t->area[best][0]) ||
            (dir == DIR_LEFT && a[0] + a[2] > t->area[best][0] + t->area[best][2]) ||
            (dir == DIR_DOWN && a[1] < t->area[best][1]) ||
            (dir == DIR_UP && a[1] + a[3] > t->area[best][1] + t->area[best][3]))
            best = i;
    }
    if (best >= 0) memcpy(r, t->area[best], sizeof r);
    *tx = r[0] + fx * (r[2] - 1);
    *ty = r[1] + fy * (r[3] - 1);
    if (dir == DIR_LEFT) *tx = r[0] + r[2] - 3;
    else if (dir == DIR_RIGHT) *tx = r[0] + 2;
    else if (dir == DIR_UP) *ty = r[1] + r[3] - 3;
    else *ty = r[1] + 2;
}

/* ---- switching ---------------------------------------------------------- */

static unsigned char remap_usage(const Screen *s, unsigned char usage)
{
    int i;
    for (i = 0; i < s->nremap; i++)
        if (s->remap_from[i] == usage) return s->remap_to[i];
    return usage;
}

static unsigned char remap_mods(const Screen *s, unsigned char m)
{
    unsigned char out = 0;
    int b;
    if (!s->nremap) return m;
    for (b = 0; b < 8; b++) {
        if (m & (1 << b)) {
            unsigned char u = remap_usage(s, (unsigned char)(HID_LCTRL + b));
            if (u >= HID_LCTRL && u <= HID_RGUI) out |= (unsigned char)(1 << (u - HID_LCTRL));
        }
    }
    return out;
}

static void pads_leave(Screen *s);
static void pads_enter(Screen *s);
static void pads_update(Screen *s, int force);

static void do_leave(Screen *s)
{
    flush_move(s);
    s->last_x = px;
    s->last_y = py;
    s->have_last = 1;
    if (s->local) uinput_release_all();
    else send_frame(s->in, RKM_LEAVE, NULL, 0);
    /* a separate clipboard helper uses LEAVE as its cue to report changes */
    if (s->clip && s->clip != s->in) send_frame(s->clip, RKM_LEAVE, NULL, 0);
    send_frame(s->kbd, RKM_LEAVE, NULL, 0);        /* lets go of any keys it holds */
    if (!pads_keep(s)) pads_leave(s);
}

static void do_enter(Screen *s)
{
    unsigned char p[5];
    int b;

    if (s->local) {
        uinput_abs((int)px, (int)py, s->w, s->h);
        for (b = 0; b < 8; b++)                /* carry held modifiers across */
            if (mods & (1 << b)) uinput_key(hid_to_evdev(HID_LCTRL + b), 1);
    } else {
        RKM_PUT16(p, (unsigned)px);
        RKM_PUT16(p + 2, (unsigned)py);
        p[4] = remap_mods(s, mods);
        if (kbm_ok(s)) {
            send_frame(s->in, RKM_ENTER, p, 5);
            send_frame(s->kbd, RKM_ENTER, p, 5);
        }
        if (is_rel(s)) {
            /* No absolute positioning: slam into the top-left corner, then
             * walk out to the entry point. */
            RKM_PUT16(p, (unsigned)(-30000) & 0xFFFF);
            RKM_PUT16(p + 2, (unsigned)(-30000) & 0xFFFF);
            send_frame(s->in, RKM_MOVEREL, p, 4);
            RKM_PUT16(p, (unsigned)px);
            RKM_PUT16(p + 2, (unsigned)py);
            send_frame(s->in, RKM_MOVEREL, p, 4);
        }
    }
    clip_sync(s);
    pads_enter(s);
}

static void switch_to(Screen *t, Monitor *m, double x, double y)
{
    if (active == t) { active_mon = m; return; }
    Screen *was = active;
    if (active) do_leave(active);
    active = t;
    active_mon = m;
    if (was) pads_update(was, 0);            /* pads it only had through focus let go there */
    if (!t) return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > t->w - 1) x = t->w - 1;
    if (y > t->h - 1) y = t->h - 1;
    fit_area(t, &x, &y, x, y);
    px = x;
    py = y;
    rel_fx = rel_fy = 0;
    t->move_pending = 0;
    t->rel_dx = t->rel_dy = 0;
    logmsg("-> %s%s%s", t->name, m ? " on monitor " : "", m ? m->name : "");
    lights_set(lights_color(t));
    do_enter(t);
}

static void goto_screen(Screen *t)
{
    if (!t || t == active) return;
    if (t->have_last) switch_to(t, monitor_showing(t), t->last_x, t->last_y);
    else switch_to(t, monitor_showing(t), t->w / 2.0, t->h / 2.0);
}

/* The active screen went away, or nothing is active yet. */
static void fallback(void)
{
    Screen *t = NULL;
    int i;

    for (i = 0; i < cfg.nscreens && !t; i++)
        if (cfg.screens[i].local) t = &cfg.screens[i];
    for (i = 0; i < cfg.nscreens && !t; i++)
        if (ready(&cfg.screens[i])) t = &cfg.screens[i];
    if (t) goto_screen(t);
}

/* Called whenever ties or agent connections change. */
static void relayout(void)
{
    resolve_monitors();
    if (active && !ready(active)) {
        /* Agents reconnect (WiFi blips, a hardware keyboard re-joining): keep
         * the keyboard where it is for a moment rather than yank it away. */
        if (!active_lost_ms) {
            active_lost_ms = now_ms();
            logmsg("%s: dropped, waiting %d s for it to come back", active->name, REJOIN_MS / 1000);
        }
        if (now_ms() - active_lost_ms < REJOIN_MS) return;
        logmsg("%s: did not come back", active->name);
        active = NULL;                          /* nothing to say goodbye to */
        active_mon = NULL;
        lights_set(0xFFFFFFFF);
    }
    if (active) active_lost_ms = 0;
    if (active_mon && active && active_mon->cur != active) {
        Screen *t = active_mon->cur;
        if (cfg.follow_tie && ready(t)) {
            /* the monitor under the pointer now shows another machine */
            double fx = px / (active->w > 1 ? active->w - 1 : 1);
            double fy = py / (active->h > 1 ? active->h - 1 : 1);
            switch_to(t, active_mon, fx * (t->w - 1), fy * (t->h - 1));
        } else {
            active_mon = monitor_showing(active);
        }
    } else if (active && !active_mon) {
        active_mon = monitor_showing(active);
    }
    /* At boot agents connect in any order; until someone uses the keyboard
     * or mouse, keep handing control to the first screen in the config. */
    if (!active || !touched) fallback();
}

void hub_extron_changed(void)
{
    relayout();
}

int hub_routing(void)
{
    return active != NULL;
}

/* ---- input routing ------------------------------------------------------ */

void hub_motion(int dx, int dy)
{
    Screen *s = active;
    double mag, gain, nx, ny;
    int dir = 0, r[4];
    touched = 1;

    if (!s) return;
    ptr_moved_ms = now_ms();
    note_wiggle(dx, dy, ptr_moved_ms);
    {   /* "trace on": m <ms> <dx> <dy> <wiggle> for each motion, raw */
        int i;
        char t[80];
        int n = snprintf(t, sizeof t, "m %lld %d %d %.2f\n", ptr_moved_ms, dx, dy, wiggle_now(ptr_moved_ms));
        for (i = 0; i < MAX_CONNS; i++)
            if (conns[i].kind == CONN_PANEL && conns[i].trace && !conns[i].dead) conn_write(&conns[i], t, (size_t)n);
    }
    s->last_input_ms = now_ms();
    mag = sqrt((double)dx * dx + (double)dy * dy);
    gain = cfg.speed * s->speed * (1.0 + cfg.accel * (mag > 30 ? 30 : mag) / 10.0);
    nx = px + dx * gain;
    ny = py + dy * gain;

    /* the rectangle the pointer is on: an area, or the whole screen */
    {
        int ai = area_at(s, px, py);
        if (has_areas(s) && ai >= 0) { r[0] = s->area[ai][0]; r[1] = s->area[ai][1]; r[2] = s->area[ai][2]; r[3] = s->area[ai][3]; }
        else { r[0] = 0; r[1] = 0; r[2] = s->w; r[3] = s->h; }
    }
    if (nx < 0 || ny < 0 || nx > s->w - 1 || ny > s->h - 1 || area_at(s, nx, ny) < 0) {
        /* leaving the screen, or running off an area into a gap */
        if (nx < r[0]) dir = DIR_LEFT;
        else if (nx > r[0] + r[2] - 1) dir = DIR_RIGHT;
        else if (ny < r[1]) dir = DIR_UP;
        else if (ny > r[1] + r[3] - 1) dir = DIR_DOWN;
    }

    if (dir && !buttons && !locked && active_mon) {
        Monitor *m = neighbor(active_mon, dir);
        if (m) {
            double fx = (px - r[0]) / (r[2] > 1 ? r[2] - 1 : 1), fy = (py - r[1]) / (r[3] > 1 ? r[3] - 1 : 1);
            double tx, ty;
            entry_point(m->cur, dir, fx, fy, &tx, &ty);
            switch_to(m->cur, m, tx, ty);
            return;
        }
    }

    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx > s->w - 1) nx = s->w - 1;
    if (ny > s->h - 1) ny = s->h - 1;
    fit_area(s, &nx, &ny, px, py);

    if (is_rel(s)) {
        double ax = dx * gain + rel_fx, ay = dy * gain + rel_fy;
        int ix = (int)ax, iy = (int)ay;
        rel_fx = ax - ix;
        rel_fy = ay - iy;
        s->rel_dx += ix;
        s->rel_dy += iy;
    }
    px = nx;
    py = ny;
    if (kbm_ok(s)) queue_move(s);
}

void hub_button(int btn, int down)
{
    static const int evbtn[] = { 0, BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA };
    Screen *s = active;
    unsigned char p[2];
    touched = 1;

    if (btn < 1 || btn > 5) return;
    if (down) buttons |= 1 << btn;
    else buttons &= ~(1 << btn);
    if (!s || !kbm_ok(s)) return;
    s->last_input_ms = now_ms();
    flush_move(s);
    if (s->local) { uinput_key(evbtn[btn], down); return; }
    p[0] = (unsigned char)btn;
    p[1] = (unsigned char)down;
    send_frame(s->in, RKM_BUTTON, p, 2);
}

void hub_wheel(int dy, int dx)
{
    Screen *s = active;
    unsigned char p[4];
    touched = 1;

    if (!s || !kbm_ok(s)) return;
    s->last_input_ms = now_ms();
    if (s->local) { uinput_wheel(dy, dx); return; }
    RKM_PUT16(p, (unsigned)dy & 0xFFFF);
    RKM_PUT16(p + 2, (unsigned)dx & 0xFFFF);
    send_frame(s->in, RKM_WHEEL, p, 4);
}

static void run_command(char *cmd, Conn *reply);

void hub_key(int evcode, int usage, int state)
{
    Screen *s;
    unsigned char p[3];
    int is_mod = usage >= HID_LCTRL && usage <= HID_RGUI, i;
    touched = 1;

    if (is_mod && state != RKM_KEY_REPEAT) {
        unsigned char bit = (unsigned char)(1 << (usage - HID_LCTRL));
        if (state) mods |= bit;
        else mods &= (unsigned char)~bit;
    }

    if (usage && !is_mod && state == RKM_KEY_DOWN) {
        unsigned char held = (unsigned char)((mods | (mods >> 4)) & 0x0F);
        /* emergency exit, whatever the config says: ctrl+alt+shift+escape */
        if (usage == HID_ESC && held == (RKM_MOD_LCTRL | RKM_MOD_LSHIFT | RKM_MOD_LALT)) {
            logmsg("emergency exit key pressed");
            quit = 1;
            return;
        }
        /* reset, whatever the config says: ctrl+alt+shift+backspace */
        if (usage == HID_BACKSPACE && held == (RKM_MOD_LCTRL | RKM_MOD_LSHIFT | RKM_MOD_LALT)) {
            logmsg("reset key pressed");
            quit = restart = 1;
            return;
        }
        for (i = 0; i < cfg.nhotkeys; i++) {
            Hotkey *h = &cfg.hotkeys[i];
            if (h->usage == usage && (h->mods & 0x0F) == held) {
                char cmd[sizeof h->cmd];
                swallowed[usage] = 1;
                memcpy(cmd, h->cmd, sizeof cmd);
                run_command(cmd, NULL);
                return;
            }
        }
    }
    if (usage && swallowed[usage]) {
        if (state == RKM_KEY_UP) swallowed[usage] = 0;
        return;
    }

    /* the machine in focus, and linked machines that take keys out of focus */
    for (i = 0; i < cfg.nscreens; i++) {
        s = &cfg.screens[i];
        if (!kbm_ok(s) || !ready(s) || (s != active && !acc_keeps(&cfg.acc[0], s))) continue;
        s->last_input_ms = s->last_key_ms = now_ms();
        if (s->local) { uinput_key(evcode, state); continue; }
        if (!usage || !(s->kbd || s->in)) continue;
        if (s == active) flush_move(s);
        p[0] = remap_usage(s, (unsigned char)usage);
        p[1] = (unsigned char)state;
        p[2] = remap_mods(s, mods);
        send_frame(s->kbd ? s->kbd : s->in, RKM_KEY, p, 3);   /* a hardware keyboard wins */
    }
}

/* ---- game pads ------------------------------------------------------------
 *
 * Each pad (an Accessory) is linked to machines as player 1..4 there.  Its
 * input goes to the machine in focus, if linked, and to linked machines
 * that take pad input out of focus.  Several pads on one player of one
 * machine are merged into one. */

static unsigned pad_prev[MAX_ACC];   /* buttons last time, for the Guide combos */
static int pad_latched[MAX_ACC];     /* a Guide combo fired: hold its buttons back */
static PadState pad_eff[MAX_ACC];    /* what the pad sends (combo buttons held back) */
static PadState pad_sent[MAX_SCREENS][MAX_PLAYERS];

static int pad_player(const Accessory *a, const Screen *s)
{
    return a->player[s - cfg.screens];
}

/* Does this pad's input go to s right now? */
static int pad_goes(const Accessory *a, const Screen *s)
{
    return a->kind == ACC_PAD && a->present && pad_player(a, s) && (s == active || acc_keeps(a, s));
}

int hub_pad_on_desktop(const Accessory *a)
{
    int i;
    for (i = 0; i < cfg.nscreens; i++)
        if (screen_is_desktop(&cfg.screens[i]) && pad_goes(a, &cfg.screens[i])) return 1;
    return 0;
}

static short bigger(short a, short b)
{
    return abs(a) >= abs(b) ? a : b;
}

static void send_pad_player(Screen *s, int player, const PadState *ps)
{
    unsigned char p[RKM_PAD_LEN];
    int k;
    p[0] = (unsigned char)(player - 1);
    RKM_PUT16(p + 1, ps->buttons);
    p[3] = ps->lt;
    p[4] = ps->rt;
    RKM_PUT16(p + 5, (unsigned)(ps->lx & 0xFFFF));
    RKM_PUT16(p + 7, (unsigned)(ps->ly & 0xFFFF));
    RKM_PUT16(p + 9, (unsigned)(ps->rx & 0xFFFF));
    RKM_PUT16(p + 11, (unsigned)(ps->ry & 0xFFFF));
    for (k = 0; k < MAX_PLAYERS; k++) send_frame(s->pads[k], RKM_PAD, p, RKM_PAD_LEN);
}

/* Work out each player's state on s from the pads going there; send what changed */
static void pads_update(Screen *s, int force)
{
    int si = (int)(s - cfg.screens), pl, i;
    if (!has_pads(s)) return;
    for (pl = 1; pl <= MAX_PLAYERS; pl++) {
        PadState m;
        int linked = pl == 1;            /* a dongle is player 1 unless told otherwise */
        memset(&m, 0, sizeof m);
        for (i = 0; i < cfg.nacc; i++) {
            const Accessory *a = &cfg.acc[i];
            const PadState *e = &pad_eff[i];
            if (pad_player(a, s) == pl) linked = 1;
            if (!pad_goes(a, s) || pad_player(a, s) != pl) continue;
            m.buttons |= e->buttons;
            if (e->lt > m.lt) m.lt = e->lt;
            if (e->rt > m.rt) m.rt = e->rt;
            m.lx = bigger(m.lx, e->lx);
            m.ly = bigger(m.ly, e->ly);
            m.rx = bigger(m.rx, e->rx);
            m.ry = bigger(m.ry, e->ry);
        }
        if (!memcmp(&m, &pad_sent[si][pl - 1], sizeof m) && !(force && linked)) continue;
        pad_sent[si][pl - 1] = m;
        send_pad_player(s, pl, &m);
    }
}

/* s lost focus: players that only had it through focus let go (and their
 * pads stop shaking) */
static void pads_leave(Screen *s)
{
    int i, k;
    for (k = 0; k < MAX_PLAYERS; k++) send_frame(s->pads[k], RKM_LEAVE, NULL, 0);
    memset(pad_sent[s - cfg.screens], 0, sizeof pad_sent[0]);
    for (i = 0; i < cfg.nacc; i++)
        if (cfg.acc[i].kind == ACC_PAD && pad_player(&cfg.acc[i], s)) pad_rumble(&cfg.acc[i], 0, 0);
}

/* s takes the pads: its dongles hear ENTER (without it they ignore PAD
 * frames), then what the pads are doing right now */
static void pads_take(Screen *s)
{
    int k;
    for (k = 0; k < MAX_PLAYERS; k++) send_frame(s->pads[k], RKM_ENTER, NULL, 0);
    pads_update(s, 1);
}

static void pads_enter(Screen *s)
{
    if (pads_keep(s)) pads_update(s, 1);     /* it has had them all along; the rest join */
    else pads_take(s);
}

/* Hand focus to the next / previous machine this pad may drive */
static void pad_cycle(const Accessory *a, int dir)
{
    int i, n = cfg.nscreens, cur = active ? (int)(active - cfg.screens) : -1;
    for (i = 1; i <= n; i++) {
        Screen *s = &cfg.screens[((cur + dir * i) % n + n) % n];
        if (s != active && ready(s) && pad_player(a, s) && (has_pads(s) || screen_is_desktop(s))) {
            goto_screen(s);
            return;
        }
    }
}

void hub_pad(Accessory *a)
{
    int ai = (int)(a - cfg.acc), i;
    unsigned pressed = a->st.buttons & ~pad_prev[ai];

    pad_prev[ai] = a->st.buttons;
    touched = 1;
    pad_eff[ai] = a->st;
    if ((a->st.buttons & RKM_PAD_GUIDE) && (pressed & (RKM_PAD_RB | RKM_PAD_LB))) {
        pad_latched[ai] = 1;
        pad_eff[ai].buttons &= ~(unsigned)(RKM_PAD_GUIDE | RKM_PAD_LB | RKM_PAD_RB);
        pad_cycle(a, pressed & RKM_PAD_RB ? 1 : -1);
        return;
    }
    if (pad_latched[ai]) {
        if (!(a->st.buttons & RKM_PAD_GUIDE)) pad_latched[ai] = 0;
        pad_eff[ai].buttons &= ~(unsigned)(RKM_PAD_GUIDE | RKM_PAD_LB | RKM_PAD_RB);
    }
    for (i = 0; i < cfg.nscreens; i++) {
        Screen *s = &cfg.screens[i];
        if (!pad_player(a, s)) continue;
        if (pad_goes(a, s)) s->last_input_ms = now_ms();
        pads_update(s, 0);
    }
}

/* The control port's pad: scripts drive the machine in focus as player 1 */
static Accessory *script_pad(void)
{
    static Accessory *a;
    int i;
    if (!a && (a = acc_by_name("script")) == NULL && cfg.nacc < MAX_ACC) {
        a = &cfg.acc[cfg.nacc++];
        memset(a, 0, sizeof *a);
        strcpy(a->name, "script");
        strcpy(a->label, "Scripts (control port)");
        a->kind = ACC_PAD;
        a->color = -1;
        a->scripted = 1;
    }
    if (a) {
        a->present = 1;
        for (i = 0; i < cfg.nscreens; i++) a->player[i] = 1;
    }
    return a;
}

/* ---- commands (hotkeys and the control socket) -------------------------- */

/* Lines on the panel's accessories page, and the settings in the machines'
 * options sheet.  Shared by the panel and control ports.  Returns 1 if the
 * command was one of these. */
static int accessory_command(int argc, char **argv, Conn *r, int panel)
{
    const char *err = panel ? "error " : "error: ";
    Accessory *a;
    Screen *s;
    if (!strcasecmp(argv[0], "link") || !strcasecmp(argv[0], "unlink") || !strcasecmp(argv[0], "player")) {
        /* link <accessory> <screen> [player]   unlink <accessory> <screen>   player <accessory> <screen> <n> */
        int on = strcasecmp(argv[0], "unlink") != 0, pl = 0;
        if (argc < 3 || !(a = acc_by_name(argv[1])) || a->scripted || !(s = screen_by_name(argv[2]))) {
            ctl_printf(r, "%sno such accessory or screen\n", err);
            return 1;
        }
        if (on && a->kind == ACC_PAD) {
            int used[MAX_PLAYERS + 1] = { 0 }, i;
            if (argc >= 4) pl = atoi(argv[3]);
            else if (pad_player(a, s)) pl = pad_player(a, s);
            else {   /* the next free player there: P1, P2, ... */
                for (i = 0; i < cfg.nacc; i++)
                    if (&cfg.acc[i] != a && cfg.acc[i].kind == ACC_PAD && !cfg.acc[i].scripted)
                        used[pad_player(&cfg.acc[i], s)] = 1;
                for (pl = 1; pl < MAX_PLAYERS && used[pl]; pl++) {}
            }
            if (pl < 1 || pl > MAX_PLAYERS) { ctl_printf(r, "%splayers are 1 to %d\n", err, MAX_PLAYERS); return 1; }
        } else if (on) {
            pl = 1;
        }
        a->player[s - cfg.screens] = (unsigned char)pl;
        logmsg("%s %s %s%s", a->name, on ? "linked to" : "unlinked from", s->name,
               on && a->kind == ACC_PAD ? (pl == 1 ? " as P1" : pl == 2 ? " as P2" : pl == 3 ? " as P3" : " as P4") : "");
        if (a->kind == ACC_PAD) {
            if (!pl) pad_rumble(a, 0, 0);
            pads_update(s, 0);
        }
        config_save_state();
        if (!panel) ctl_printf(r, "ok\n");
        return 1;
    }
    if (!strcasecmp(argv[0], "keep") || !strcasecmp(argv[0], "keepall")) {
        /* keep <accessory> <screen> on|off: that machine gets it out of focus too
         * keepall <accessory> on|off: every machine it is linked to does */
        int all = !strcasecmp(argv[0], "keepall"), on, i;
        if (argc != (all ? 3 : 4) || !(a = acc_by_name(argv[1])) || (!all && !(s = screen_by_name(argv[2])))) {
            ctl_printf(r, "%skeep <accessory> <screen> on|off, keepall <accessory> on|off\n", err);
            return 1;
        }
        on = !strcasecmp(argv[all ? 2 : 3], "on");
        if (all) a->keep_all = on;
        else a->keep[s - cfg.screens] = (unsigned char)on;
        logmsg("%s: kept out of focus %s%s", a->name, on ? "on " : "no longer on ", all ? "everywhere" : s->name);
        for (i = 0; a->kind == ACC_PAD && i < cfg.nscreens; i++) {
            Screen *t = &cfg.screens[i];
            if (!pad_player(a, t) || t == active) continue;
            if (pads_keep(t)) pads_take(t);
            else pads_leave(t);
        }
        config_save_state();
        if (!panel) ctl_printf(r, "ok\n");
        return 1;
    }
    return 0;
}

/* Say which console is wired into a box (it cannot tell us itself). */
static int plug_console(Screen *c, Screen *box, Conn *r, int panel)
{
    if (!box) {
        ctl_printf(r, panel ? "error no such box\n" : "error: no such box\n");
        return -1;
    }
    if (c && c->via != box) {
        ctl_printf(r, panel ? "error %s cannot go into %s\n" : "error: %s cannot go into %s\n", c->name, box->name);
        return -1;
    }
    if (box->plugged == c) return 0;
    box->plugged = c;
    logmsg("%s: %s plugged in", box->name, c ? c->name : "nothing");
    config_save_state();
    relayout();
    return 0;
}

static void cmd_status(Conn *r)
{
    int i;

    ctl_printf(r, "active: %s", active ? active->name : "(none)");
    if (active) ctl_printf(r, " at %d,%d", (int)px, (int)py);
    ctl_printf(r, "%s%s%s\n", active_mon ? " on monitor " : "", active_mon ? active_mon->name : "",
               locked ? " [locked]" : "");
    for (i = 0; i < cfg.nscreens; i++) {
        Screen *s = &cfg.screens[i];
        ctl_printf(r, "screen %-12s %4dx%-4d %s input=%s clipboard=%s", s->name, s->w, s->h,
                   ready(s) ? "ready  " : "offline",
                   s->local ? "uinput" : s->in ? s->in->peer : "-",
                   s->clip ? s->clip->peer : "-");
        if (s->kbd) ctl_printf(r, " keyboard=%s", s->kbd->peer);
        {
            int k;
            for (k = 0; k < MAX_PLAYERS; k++)
                if (s->pads[k]) ctl_printf(r, " pad=%s", s->pads[k]->peer);
        }
        if (s->via) ctl_printf(r, " via=%s", s->via->name);
        if (s->plugged) ctl_printf(r, " plugged=%s", s->plugged->name);
        if (s->extron_input) ctl_printf(r, " extron-in=%d", s->extron_input);
        ctl_printf(r, "\n");
    }
    for (i = 0; i < cfg.nmonitors; i++) {
        Monitor *m = &cfg.monitors[i];
        ctl_printf(r, "monitor %-11s pos=%d,%d ", m->name, m->col, m->row);
        if (m->fixed) ctl_printf(r, "fixed");
        else ctl_printf(r, "extron-out=%d (input %d)", m->extron_output, extron_input_for(m->extron_output));
        ctl_printf(r, " shows=%s\n", m->cur ? m->cur->name : "-");
    }
    for (i = 0; i < cfg.nacc; i++) {
        const Accessory *a = &cfg.acc[i];
        int k, any = 0;
        if (a->scripted) continue;
        ctl_printf(r, "accessory %-24s %s %s ->", a->name, acc_kinds[a->kind].id, a->present || a->kind == ACC_KBM ? "here" : "away");
        for (k = 0; k < cfg.nscreens; k++)
            if (a->player[k]) {
                ctl_printf(r, a->kind == ACC_PAD ? " %s:P%d" : " %s", cfg.screens[k].name, a->player[k]);
                any = 1;
            }
        ctl_printf(r, "%s  (%s)\n", any ? "" : " nothing", a->label);
    }
    ctl_printf(r, "clipboard: %lu bytes, generation %u, from %s\n", (unsigned long)clip_len,
               clip_gen, clip_src ? clip_src->name : "-");
}

static void run_command(char *cmd, Conn *r)
{
    char *argv[10] = { 0 }, *save = NULL, *tok;
    int argc = 0;

    for (tok = strtok_r(cmd, " \t\r\n", &save); tok && argc < 10; tok = strtok_r(NULL, " \t\r\n", &save))
        argv[argc++] = tok;
    if (argc == 0) return;

    if (!strcasecmp(argv[0], "status")) {
        cmd_status(r);
    } else if (!strcasecmp(argv[0], "goto") && argc == 2) {
        Screen *s = screen_by_name(argv[1]);
        if (!s) ctl_printf(r, "error: no screen %s\n", argv[1]);
        else if (!ready(s)) ctl_printf(r, "error: %s is offline\n", s->name);
        else { goto_screen(s); ctl_printf(r, "ok\n"); }
    } else if (!strcasecmp(argv[0], "show") && argc >= 2) {
        /* route a machine's video to a monitor (default: the one under the pointer) */
        Screen *s = screen_by_name(argv[1]);
        Monitor *m = argc >= 3 ? monitor_by_name(argv[2]) : active_mon;
        if (!s || !s->extron_input) ctl_printf(r, "error: %s has no switcher input\n", argv[1]);
        else if (!m || m->fixed) ctl_printf(r, "error: no switched monitor to show it on\n");
        else { extron_tie(s->extron_input, m->extron_output); ctl_printf(r, "ok\n"); }
    } else if (!strcasecmp(argv[0], "tie") && argc == 3) {
        extron_tie(atoi(argv[1]), atoi(argv[2]));
        ctl_printf(r, "ok\n");
    } else if (!strcasecmp(argv[0], "lock")) {
        locked = argc >= 2 ? !strcasecmp(argv[1], "on") : !locked;
        ctl_printf(r, "edge switching %s\n", locked ? "locked" : "unlocked");
    } else if (!strcasecmp(argv[0], "rel") && argc == 3) {          /* synthetic input */
        hub_motion(atoi(argv[1]), atoi(argv[2]));
        ctl_printf(r, "ok\n");
    } else if (!strcasecmp(argv[0], "btn") && argc == 3) {
        hub_button(atoi(argv[1]), atoi(argv[2]));
        ctl_printf(r, "ok\n");
    } else if (!strcasecmp(argv[0], "wheel") && argc == 2) {
        hub_wheel(atoi(argv[1]), 0);
        ctl_printf(r, "ok\n");
    } else if (!strcasecmp(argv[0], "key") && argc == 3) {
        int u = key_by_name(argv[1]);
        if (u < 0) ctl_printf(r, "error: unknown key %s\n", argv[1]);
        else { hub_key(hid_to_evdev(u), u, atoi(argv[2])); ctl_printf(r, "ok\n"); }
    } else if (!strcasecmp(argv[0], "plug") && argc == 2) {
        /* plug <console>: it is the one wired into its box now */
        Screen *s = screen_by_name(argv[1]);
        if (!s || !s->via) ctl_printf(r, "error: %s is not a console behind a box\n", argv[1]);
        else if (plug_console(s, s->via, r, 0) == 0) ctl_printf(r, "ok\n");
    } else if (!strcasecmp(argv[0], "unplug") && argc == 2) {
        if (plug_console(NULL, screen_by_name(argv[1]), r, 0) == 0) ctl_printf(r, "ok\n");
    } else if (accessory_command(argc, argv, r, 0)) {
    } else if (!strcasecmp(argv[0], "pad") && argc >= 2) {
        /* pad <buttons hex> [lt rt lx ly rx ry]: as if the real pad did it */
        PadState ps;
        memset(&ps, 0, sizeof ps);
        ps.buttons = (unsigned)strtoul(argv[1], NULL, 16) & 0xFFFF;
        if (argc >= 4) { ps.lt = (unsigned char)atoi(argv[2]); ps.rt = (unsigned char)atoi(argv[3]); }
        if (argc >= 6) { ps.lx = (short)atoi(argv[4]); ps.ly = (short)atoi(argv[5]); }
        if (argc >= 8) { ps.rx = (short)atoi(argv[6]); ps.ry = (short)atoi(argv[7]); }
        {
            Accessory *a = script_pad();
            if (a) { a->st = ps; hub_pad(a); }
        }
        ctl_printf(r, "ok\n");
    } else if (!strcasecmp(argv[0], "ddc")) {
        /* ddc: what each DDC monitor reports; ddc <monitor> <input>: switch it */
        int i, v;
        if (argc == 3) {
            Monitor *m = monitor_by_name(argv[1]);
            if (!m || ddc_state(m, NULL) < 0) ctl_printf(r, "error: no DDC link to %s\n", argv[1]);
            else { ddc_set_input(m, atoi(argv[2])); ctl_printf(r, "ok\n"); }
        } else {
            for (i = 0; i < cfg.nmonitors; i++) {
                int st = ddc_state(&cfg.monitors[i], &v);
                if (st < 0) continue;
                ctl_printf(r, "ddc %s: %s, input %d\n", cfg.monitors[i].name, st ? "answering" : "silent", v);
            }
            ctl_printf(r, "ok\n");
        }
    } else if (!strcasecmp(argv[0], "lights") && argc == 2) {
        /* lights #rrggbb | off (keyboard's own) | auto (the active machine's) */
        if (!strcasecmp(argv[1], "off")) lights_set(0xFFFFFFFF);
        else if (!strcasecmp(argv[1], "auto")) lights_set(active ? lights_color(active) : 0xFFFFFFFF);
        else lights_set(strtoul(argv[1][0] == '#' ? argv[1] + 1 : argv[1], NULL, 16));
        ctl_printf(r, "ok\n");
    } else if (!strcasecmp(argv[0], "quit")) {
        quit = 1;
    } else if (!strcasecmp(argv[0], "reset")) {
        ctl_printf(r, "ok\n");
        logmsg("reset asked for");
        quit = restart = 1;
    } else {
        ctl_printf(r, "error: commands are status, goto <screen>, show <screen> [monitor], "
                      "tie <in> <out>, lock [on|off], rel <dx> <dy>, btn <n> <0|1>, "
                      "key <name> <0|1>, wheel <n>, pad <hex> [lt rt lx ly rx ry], plug <console>, "
                      "unplug <box>, link <acc> <screen> [player], unlink <acc> <screen>, "
                      "keep <acc> <screen> on|off, keepall <acc> on|off, reset, quit\n");
    }
}

/* ---- touch panel -------------------------------------------------------- */

/* The panel (an M5Stack Tab5 on the LAN) sees the layout and may rearrange
 * monitors and switcher ties, and pick which machine gets the keyboard.  It
 * cannot inject input.  Each change to the layout is pushed to it as one
 * line: "layout {json}". */

typedef struct { char *p; size_t len, cap; } Sbuf;

static void sb_printf(Sbuf *b, const char *fmt, ...)
{
    va_list ap;
    int n;

    for (;;) {
        va_start(ap, fmt);
        n = vsnprintf(b->p ? b->p + b->len : NULL, b->p ? b->cap - b->len : 0, fmt, ap);
        va_end(ap);
        if (n < 0) return;
        if (b->p && b->len + (size_t)n < b->cap) { b->len += (size_t)n; return; }
        b->cap = (b->cap + (size_t)n + 1) * 2;
        b->p = realloc(b->p, b->cap);
        if (!b->p) { b->len = b->cap = 0; return; }
    }
}

static void sb_str(Sbuf *b, const char *s)   /* JSON string */
{
    sb_printf(b, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') sb_printf(b, "\\%c", *s);
        else if (*s == '\n') sb_printf(b, "\\n");
        else if ((unsigned char)*s < 0x20) sb_printf(b, " ");
        else sb_printf(b, "%c", *s);
    }
    sb_printf(b, "\"");
}

/* How a machine is attached, a line at a time, for the panel's "about" */
static void screen_about(const Screen *s, char *out, size_t n)
{
    size_t len = 0;
    int i, k;
#define ADD(...) do { if (len < n) len += (size_t)snprintf(out + len, n - len, __VA_ARGS__); } while (0)
    out[0] = 0;
    if (s->local) ADD("the hub's own desktop\n");
    else if (s->in) ADD("agent at %s\n", s->in->peer);
    if (s->kbd) ADD("PS/2 keyboard at %s\n", s->kbd->peer);
    for (k = 0; k < MAX_PLAYERS; k++)
        if (s->pads[k]) ADD("pad dongle at %s\n", s->pads[k]->peer);
    if (s->via) {
        const Screen *b = s->via;
        ADD("into the %s%s", b->label[0] ? b->label : b->name, b->plugged == s ? "" : " (not plugged in now)");
        while (b->via) { b = b->via; ADD(", then the %s", b->label[0] ? b->label : b->name); }
        if (b->extron_input) ADD(", switcher input %d", b->extron_input);
        ADD("\n");
    }
    if (s->extron_input) ADD("switcher input %d\n", s->extron_input);
    for (i = 0; i < cfg.nmonitors; i++) {
        const Monitor *m = &cfg.monitors[i];
        const char *mn = m->label[0] ? m->label : m->name;
        if (m->cur != s) continue;
        if (m->fixed == s) ADD("its own screen: %s\n", mn);
        else if (m->shared == s && m->shared_on) ADD("on %s (its other input)\n", mn);
        else ADD("on %s (switcher output %d)\n", mn, m->extron_output);
    }
    if (!len) ADD("%s\n", s->no_agent ? "video only" : "nothing connected");
#undef ADD
    if (len && len <= n && out[len - 1] == '\n') out[len - 1] = 0;
}

static void layout_json(Sbuf *b)
{
    int i, k;
    char about[sizeof cfg.screens[0].about];

    sb_printf(b, "{\"active\":");
    sb_str(b, active ? active->name : "");
    sb_printf(b, ",\"locked\":%d,\"switcher\":{\"present\":%d,\"online\":%d,\"inputs\":%d,\"outputs\":%d}",
              locked, cfg.extron_dev[0] != 0, extron_online(), cfg.extron_inputs, cfg.extron_outputs);
    sb_printf(b, ",\"screens\":[");
    for (i = 0; i < cfg.nscreens; i++) {
        Screen *s = &cfg.screens[i];
        sb_printf(b, "%s{\"name\":", i ? "," : "");
        sb_str(b, s->name);
        sb_printf(b, ",\"label\":");
        sb_str(b, s->label[0] ? s->label : s->name);
        sb_printf(b, ",\"art\":");
        sb_str(b, s->art);
        sb_printf(b, ",\"input\":%d,\"ready\":%d,\"agent\":%d,\"kbd\":%d,\"soft\":%d,\"w\":%d,\"h\":%d,"
                  "\"color\":%lu,\"colorSet\":%d,\"buddy\":%d,\"pads\":%d,\"focus\":%d,\"via\":",
                  s->extron_input, ready(s), !s->no_agent, s->kbd != NULL, s->in != NULL || s->local, s->w, s->h,
                  lights_color(s), s->color >= 0, s->buddy != NULL, has_pads(s), s == active);
        sb_str(b, s->via ? s->via->name : "");
        sb_printf(b, ",\"plugged\":");
        sb_str(b, s->plugged ? s->plugged->name : "");
        screen_about(s, about, sizeof about);
        sb_printf(b, ",\"about\":");
        sb_str(b, about);
        sb_printf(b, "}");
    }
    sb_printf(b, "],\"accessories\":[");
    for (i = 0, k = 0; i < cfg.nacc; i++) {
        const Accessory *a = &cfg.acc[i];
        int j, nl = 0;
        if (a->scripted) continue;
        sb_printf(b, "%s{\"name\":", k++ ? "," : "");
        sb_str(b, a->name);
        sb_printf(b, ",\"label\":");
        sb_str(b, a->label);
        sb_printf(b, ",\"kind\":");
        sb_str(b, acc_kinds[a->kind].id);
        sb_printf(b, ",\"present\":%d,\"color\":%ld,\"keepAll\":%d,\"about\":", a->present || a->kind == ACC_KBM,
                  a->color, a->keep_all);
        sb_str(b, a->about);
        sb_printf(b, ",\"links\":[");
        for (j = 0; j < cfg.nscreens; j++)
            if (a->player[j]) {
                sb_printf(b, "%s{\"screen\":", nl++ ? "," : "");
                sb_str(b, cfg.screens[j].name);
                sb_printf(b, ",\"player\":%d,\"keep\":%d}", a->player[j], a->keep[j]);
            }
        sb_printf(b, "]}");
    }
    sb_printf(b, "],\"monitors\":[");
    for (i = 0; i < cfg.nmonitors; i++) {
        Monitor *m = &cfg.monitors[i];
        sb_printf(b, "%s{\"name\":", i ? "," : "");
        sb_str(b, m->name);
        sb_printf(b, ",\"col\":%d,\"row\":%d,\"fixed\":", m->col, m->row);
        sb_str(b, m->fixed ? m->fixed->name : "");
        sb_printf(b, ",\"output\":%d,\"input\":%d,\"shows\":", m->extron_output,
                  m->fixed ? 0 : extron_input_for(m->extron_output));
        sb_str(b, m->cur ? m->cur->name : "");
        sb_printf(b, ",\"shared\":");
        sb_str(b, m->shared ? m->shared->name : "");
        sb_printf(b, ",\"sharedOn\":%d,\"portrait\":%d,\"aw\":%d,\"ah\":%d,\"inches10\":%d,"
                  "\"capture\":%d,\"ddc\":%d,\"label\":", m->shared_on, m->portrait, m->aspect_w, m->aspect_h,
                  m->inches10, m->capture, ddc_state(m, NULL));
        sb_str(b, m->label[0] ? m->label : m->name);
        sb_printf(b, "}");
    }
    sb_printf(b, "]}");
}

static unsigned fnv(const char *s, size_t n)
{
    unsigned h = 2166136261u;
    while (n--) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

/* Called every pass of the main loop: send the layout to panels that have
 * not seen this version of it. */
static void panel_push(int force)
{
    Sbuf b = { 0, 0, 0 };
    unsigned h;
    int i, any = 0;

    for (i = 0; i < MAX_CONNS; i++)
        if (conns[i].kind == CONN_PANEL && !conns[i].dead) any = 1;
    if (!any) return;
    sb_printf(&b, "layout ");
    layout_json(&b);
    sb_printf(&b, "\n");
    if (!b.p) return;
    h = fnv(b.p, b.len);
    for (i = 0; i < MAX_CONNS; i++) {
        Conn *c = &conns[i];
        if (c->kind != CONN_PANEL || c->dead || (!force && c->layout_sent == h)) continue;
        c->layout_sent = h;
        conn_write(c, b.p, b.len);
    }
    free(b.p);
}

static Monitor *monitor_at(int col, int row)
{
    int i;
    for (i = 0; i < cfg.nmonitors; i++)
        if (cfg.monitors[i].col == col && cfg.monitors[i].row == row) return &cfg.monitors[i];
    return NULL;
}

/* activity {"screen":[seconds since input, seconds since a key],...}; -1 = never.
 * lil' C's bridge asks for this to stay out of the way of machines in use. */
/* pointer <screen> <monitor> <fx> <fy> <idle s> <wiggle>: where the pointer
 * is, as a fraction across and down the monitor it is on, and how hard it is
 * being wiggled (about-turns, fading over ~1.5 s) - lil' C watches it */
static void cmd_pointer(Conn *r)
{
    const Screen *s = active;
    double x0 = 0, y0 = 0, w, h;
    int ai;
    if (!s || !active_mon) { ctl_printf(r, "pointer - - 0 0 -1 0\n"); return; }
    w = s->w > 1 ? s->w : 1;
    h = s->h > 1 ? s->h : 1;
    if (has_areas(s) && (ai = area_at(s, px, py)) >= 0) {
        x0 = s->area[ai][0]; y0 = s->area[ai][1];
        w = s->area[ai][2]; h = s->area[ai][3];
    }
    ctl_printf(r, "pointer %s %s %.3f %.3f %.1f %.1f\n", s->name, active_mon->name,
               clampd((px - x0) / w, 0, 1), clampd((py - y0) / h, 0, 1),
               ptr_moved_ms ? (now_ms() - ptr_moved_ms) / 1000.0 : -1.0, wiggle_now(now_ms()));
}

static void cmd_activity(Conn *r)
{
    Sbuf b = { 0, 0, 0 };
    long long now = now_ms();
    int i;

    sb_printf(&b, "activity {");
    for (i = 0; i < cfg.nscreens; i++) {
        Screen *s = &cfg.screens[i];
        sb_printf(&b, "%s", i ? "," : "");
        sb_str(&b, s->name);
        sb_printf(&b, ":[%.1f,%.1f]", s->last_input_ms ? (now - s->last_input_ms) / 1000.0 : -1.0,
                  s->last_key_ms ? (now - s->last_key_ms) / 1000.0 : -1.0);
    }
    sb_printf(&b, "}\n");
    if (b.p) conn_write(r, b.p, b.len);
    free(b.p);
}

/* The rest of the command line from the word `from` on: strtok_r cut it up
 * with NULs, so put spaces back up to the line's real end. */
static const char *cmd_rest(char *from, const char *line_end)
{
    char *q;
    for (q = from; q < line_end; q++)
        if (*q == 0 || *q == '\r' || *q == '\n' || *q == '\t') *q = ' ';
    while (q > from && q[-1] == ' ') q--;
    *q = 0;
    return from;
}

static void run_panel_command(char *cmd, Conn *r)
{
    char *argv[12] = { 0 }, *save = NULL, *tok;
    const char *cmd_end = cmd + strlen(cmd);
    int argc = 0;

    for (tok = strtok_r(cmd, " \t\r\n", &save); tok && argc < 12; tok = strtok_r(NULL, " \t\r\n", &save))
        argv[argc++] = tok;
    if (argc == 0) return;

    if (!strcasecmp(argv[0], "layout")) {
        r->layout_sent = 0;                       /* send it again, even if unchanged */
    } else if (!strcasecmp(argv[0], "tie") && argc == 3) {
        /* tie <screen> <monitor>: put that machine's video on that monitor */
        Screen *s = screen_by_name(argv[1]);
        Monitor *m = monitor_by_name(argv[2]);
        if (!s || !s->extron_input) ctl_printf(r, "error %s is not wired to the switcher\n", argv[1]);
        else if (!m || m->fixed) ctl_printf(r, "error %s is not a switched monitor\n", argv[2]);
        else if (!extron_online()) ctl_printf(r, "error the switcher is not connected\n");
        else {
            extron_tie(s->extron_input, m->extron_output);
            if (m->shared_on) {                    /* it must be on its switcher input now */
                m->shared_on = 0;
                ddc_set_input(m, m->ddc_switcher);
                config_save_state();
                relayout();
            }
        }
    } else if (!strcasecmp(argv[0], "untie") && argc == 2) {
        Monitor *m = monitor_by_name(argv[1]);
        if (!m || m->fixed) ctl_printf(r, "error %s is not a switched monitor\n", argv[1]);
        else if (!extron_online()) ctl_printf(r, "error the switcher is not connected\n");
        else extron_tie(0, m->extron_output);
    } else if (!strcasecmp(argv[0], "move") && argc == 4) {
        /* move <monitor> <col> <row>; whatever stood there takes its old place */
        Monitor *m = monitor_by_name(argv[1]), *o;
        int col = atoi(argv[2]), row = atoi(argv[3]);
        if (!m || col < -20 || col > 20 || row < -20 || row > 20) {
            ctl_printf(r, "error bad move\n");
            return;
        }
        if ((o = monitor_at(col, row)) && o != m) {
            o->col = m->col;
            o->row = m->row;
        }
        m->col = col;
        m->row = row;
        logmsg("panel: monitor %s moved to %d,%d", m->name, col, row);
        config_save_state();
    } else if (!strcasecmp(argv[0], "shape") && argc == 6) {
        /* shape <monitor> <aw> <ah> <inches*10> <turned 0|1>: how the panel draws it */
        Monitor *m = monitor_by_name(argv[1]);
        int aw = atoi(argv[2]), ah = atoi(argv[3]), in = atoi(argv[4]);
        if (!m || aw < 1 || aw > 64 || ah < 1 || ah > 64 || in < 0 || in > 2000) {
            ctl_printf(r, "error bad shape\n");
            return;
        }
        m->aspect_w = aw;
        m->aspect_h = ah;
        m->inches10 = in;
        m->portrait = atoi(argv[5]) != 0;
        m->shape_set = 1;
        logmsg("panel: monitor %s is %d:%d, %d.%d\"%s", m->name, aw, ah, in / 10, in % 10, m->portrait ? ", turned" : "");
        config_save_state();
    } else if (!strcasecmp(argv[0], "share") && argc >= 2) {
        /* share <monitor> [on|off]: which of its inputs the monitor shows */
        Monitor *m = monitor_by_name(argv[1]);
        if (!m || !m->shared) ctl_printf(r, "error %s has no second input\n", argv[1]);
        else {
            m->shared_on = argc >= 3 ? !strcasecmp(argv[2], "on") : !m->shared_on;
            ddc_set_input(m, m->shared_on ? m->ddc_shared : m->ddc_switcher);
            logmsg("panel: monitor %s now shows %s", m->name, m->shared_on ? m->shared->name : "the switcher");
            config_save_state();
            relayout();
        }
    } else if (!strcasecmp(argv[0], "color") && argc == 3) {
        /* color <screen> <RRGGBB | default>: its colour on the panel and keyboard */
        Screen *s = screen_by_name(argv[1]);
        Accessory *a = acc_by_name(argv[1]);
        if (!s && a) {   /* an accessory's lines */
            a->color = !strcasecmp(argv[2], "default") ? -1 : (long)(strtoul(argv[2], NULL, 16) & 0xFFFFFF);
            config_save_state();
            return;
        }
        if (!s) { ctl_printf(r, "error no screen %s\n", argv[1]); return; }
        s->color = !strcasecmp(argv[2], "default") ? s->config_color : (long)(strtoul(argv[2], NULL, 16) & 0xFFFFFF);
        config_save_state();
        if (s == active) lights_set(lights_color(s));
    } else if (accessory_command(argc, argv, r, 1)) {
    } else if (!strcasecmp(argv[0], "plug") && argc == 2) {
        /* plug <console>: the cable from it now goes into its box */
        Screen *s = screen_by_name(argv[1]);
        if (!s || !s->via) ctl_printf(r, "error %s is not a console behind a box\n", argv[1]);
        else plug_console(s, s->via, r, 1);
    } else if (!strcasecmp(argv[0], "unplug") && argc == 2) {
        plug_console(NULL, screen_by_name(argv[1]), r, 1);
    } else if (!strcasecmp(argv[0], "goto") && argc == 2) {
        Screen *s = screen_by_name(argv[1]);
        if (!s || !ready(s)) ctl_printf(r, "error %s is offline\n", argv[1]);
        else goto_screen(s);
    } else if (!strcasecmp(argv[0], "lock")) {
        locked = argc >= 2 ? !strcasecmp(argv[1], "on") : !locked;
    } else if (!strcasecmp(argv[0], "activity")) {
        cmd_activity(r);
    } else if (!strcasecmp(argv[0], "pointer")) {
        cmd_pointer(r);
    } else if (!strcasecmp(argv[0], "trace") && argc == 2) {
        /* trace on|off: stream raw mouse motion to this connection */
        r->trace = !strcasecmp(argv[1], "on");
    } else if (!strcasecmp(argv[0], "buddy") && argc >= 3) {
        /* buddy <screen> show|hide [label]: lil' C's window on that machine */
        Screen *s = screen_by_name(argv[1]);
        unsigned char p[1 + 48];
        size_t n = 0;
        if (!s || !s->buddy) { ctl_printf(r, "error %s cannot show lil' C\n", argv[1]); return; }
        p[0] = !strcasecmp(argv[2], "show") ? RKM_BUDDY_SHOW : RKM_BUDDY_HIDE;
        if (argc >= 4) {                          /* the label is the rest of the line */
            const char *lab = cmd_rest(argv[3], cmd_end);
            n = strlen(lab);
            if (n > 48) n = 48;
            memcpy(p + 1, lab, n);
        }
        send_frame(s->buddy, RKM_BUDDY, p, (unsigned)(1 + n));
    } else if (!strcasecmp(argv[0], "sound") && argc == 11) {
        /* sound <screen> <id> <rate> <chirp ms> <gap ms> <lo Hz> <hi Hz> <level %> <lead ms> <ch,ch,..>:
         * play a chirp on each listed channel in turn (lil' C locating himself) */
        Screen *s = screen_by_name(argv[1]);
        unsigned char p[32];
        unsigned n = 0, k;
        char *c;
        if (!s || !s->sound) { ctl_printf(r, "error %s cannot play sounds\n", argv[1]); return; }
        RKM_PUT16(p, (unsigned)atoi(argv[2]));
        RKM_PUT32(p + 2, (unsigned long)atol(argv[3]));
        RKM_PUT16(p + 6, (unsigned)atoi(argv[4]));
        RKM_PUT16(p + 8, (unsigned)atoi(argv[5]));
        RKM_PUT16(p + 10, (unsigned)atoi(argv[6]));
        RKM_PUT16(p + 12, (unsigned)atoi(argv[7]));
        p[14] = (unsigned char)atoi(argv[8]);
        RKM_PUT16(p + 15, (unsigned)atoi(argv[9]));
        for (c = argv[10], k = 18; *c && k < sizeof p; c++)
            if (*c >= '0' && *c <= '5') { p[k++] = (unsigned char)(*c - '0'); n++; }
        p[17] = (unsigned char)n;
        send_frame(s->sound, RKM_SOUND, p, 18 + n);
    } else if (!strcasecmp(argv[0], "reset")) {
        logmsg("panel: reset");
        quit = restart = 1;
    } else if (!strcasecmp(argv[0], "ping")) {
        ctl_printf(r, "pong\n");
    } else {
        ctl_printf(r, "error commands are layout, tie <screen> <monitor>, untie <monitor>, "
                      "move <monitor> <col> <row>, share <monitor> [on|off], color <screen> <RRGGBB|default>, "
                      "goto <screen>, lock [on|off], activity, buddy <screen> show|hide [label], "
                      "sound <screen> <id> <rate> <chirp> <gap> <lo> <hi> <level> <lead> <channels>, ping\n");
    }
}

/* ---- agent protocol ----------------------------------------------------- */

static void conn_detach(Conn *c)
{
    Screen *s = c->screen;
    if (!s) return;
    if (s->in == c) s->in = NULL;
    if (s->kbd == c) s->kbd = NULL;
    {
        int k;
        for (k = 0; k < MAX_PLAYERS; k++)
            if (s->pads[k] == c) s->pads[k] = NULL;
    }
    if (s->clip == c) s->clip = NULL;
    if (s->buddy == c) s->buddy = NULL;
    if (s->sound == c) s->sound = NULL;
    c->screen = NULL;
    logmsg("%s: disconnected (%s)", s->name, c->peer);
}

/* Replace a stale connection for a screen without logging a disconnect. */
static void kick(Conn *old)
{
    Screen *s = old->screen;
    if (s && s->in == old) s->in = NULL;
    if (s && s->kbd == old) s->kbd = NULL;
    if (s) {
        int k;
        for (k = 0; k < MAX_PLAYERS; k++)
            if (s->pads[k] == old) s->pads[k] = NULL;
    }
    if (s && s->clip == old) s->clip = NULL;
    if (s && s->buddy == old) s->buddy = NULL;
    if (s && s->sound == old) s->sound = NULL;
    old->screen = NULL;
    old->dead = 1;
}

static void on_hello(Conn *c, const unsigned char *p, unsigned len)
{
    char name[RKM_NAME_MAX + 1];
    unsigned char w[2];
    Screen *s;
    unsigned n;

    w[0] = RKM_VERSION;
    if (len < 10 || p[0] != RKM_VERSION) {
        w[1] = RKM_ERR_VERSION;
        send_frame(c, RKM_WELCOME, w, 2);
        c->dead = 1;
        return;
    }
    n = len - 10 > RKM_NAME_MAX ? RKM_NAME_MAX : len - 10;
    memcpy(name, p + 10, n);
    name[n] = 0;
    s = screen_by_name(name);
    if (!s) {
        logmsg("%s: unknown screen name \"%s\" (add a [screen %s] section)", c->peer, name, name);
        w[1] = RKM_ERR_NAME;
        send_frame(c, RKM_WELCOME, w, 2);
        c->dead = 1;
        return;
    }
    c->caps = p[1];
    c->charset = p[2];
    c->eol = p[3];
    c->clipmax_kb = RKM_GET16(p + 8);
    c->screen = s;
    if (s->local) c->caps &= ~RKM_CAP_INPUT;   /* the hub injects locally itself */
    if (c->caps & RKM_CAP_KEYS) {               /* a hardware keyboard: keys only */
        c->caps &= ~(RKM_CAP_INPUT | RKM_CAP_CLIP);
        if (s->kbd && s->kbd != c) kick(s->kbd);
        s->kbd = c;
    }
    if (c->caps & RKM_CAP_PAD) {                /* a game pad player: the pad only */
        c->caps &= ~(RKM_CAP_INPUT | RKM_CAP_CLIP);
        /* one per player port; the same dongle coming back replaces itself */
        int k, slot = -1;
        size_t hl = strcspn(c->peer, ":");
        for (k = 0; k < MAX_PLAYERS && slot < 0; k++)
            if (s->pads[k] && s->pads[k] != c && !strncmp(s->pads[k]->peer, c->peer, hl) &&
                s->pads[k]->peer[hl] == ':') { kick(s->pads[k]); slot = k; }
        for (k = 0; k < MAX_PLAYERS && slot < 0; k++)
            if (!s->pads[k]) slot = k;
        if (slot < 0) { kick(s->pads[0]); slot = 0; }
        s->pads[slot] = c;
        memset(pad_sent[s - cfg.screens], 0, sizeof pad_sent[0]);
    }

    if (c->caps & RKM_CAP_INPUT) {
        if (s->in && s->in != c) kick(s->in);       /* same machine came back */
        s->in = c;
        if (RKM_GET16(p + 4) && RKM_GET16(p + 6)) {
            s->w = (int)RKM_GET16(p + 4);
            s->h = (int)RKM_GET16(p + 6);
        }
    }
    if (c->caps & RKM_CAP_CLIP) {
        if (s->clip && s->clip != c) {
            if (s->clip == s->in) s->clip->caps &= ~RKM_CAP_CLIP;   /* a helper takes over */
            else kick(s->clip);
        }
        s->clip = c;
        s->clip_gen = 0;
    }
    if (c->caps & RKM_CAP_BUDDY) s->buddy = c;
    if (c->caps & RKM_CAP_SOUND) s->sound = c;
    w[1] = RKM_OK;
    send_frame(c, RKM_WELCOME, w, 2);
    logmsg("%s: connected from %s, %dx%d%s%s%s%s%s", s->name, c->peer, s->w, s->h,
           c->caps & RKM_CAP_INPUT ? " input" : "", c->caps & RKM_CAP_CLIP ? " clipboard" : "",
           c->caps & RKM_CAP_REL ? " relative" : "", c->caps & RKM_CAP_KEYS ? " hardware-keyboard" : "",
           c->caps & RKM_CAP_PAD ? " game-pad" : "");

    if (s == active) {
        if (c->caps & (RKM_CAP_INPUT | RKM_CAP_KEYS | RKM_CAP_PAD)) do_enter(s);   /* agent restarted under the pointer */
        else clip_sync(s);
    } else if ((c->caps & RKM_CAP_PAD) && pads_keep(s)) {
        pads_take(s);                        /* a dongle (re)joined a machine that keeps a pad */
    }
    relayout();
}

/* A monitor the panel has not been told about takes its shape, which way
 * up it stands and its size from what its machine says: rectangle r in
 * pixels, mm = width and height in millimetres (0 = unknown). */
static void monitor_shape_from(Monitor *m, const int *r, const unsigned char *mm)
{
    static const int shapes[][2] = { {4, 3}, {5, 4}, {3, 2}, {16, 10}, {16, 9}, {21, 9}, {32, 9} };
    int w = r[2] > r[3] ? r[2] : r[3], h = r[2] > r[3] ? r[3] : r[2], k, best = 0;
    double mw = RKM_GET16(mm), mh = RKM_GET16(mm + 2);
    for (k = 1; k < 7; k++)              /* the nearest common shape */
        if (fabs((double)w / h - (double)shapes[k][0] / shapes[k][1]) <
            fabs((double)w / h - (double)shapes[best][0] / shapes[best][1])) best = k;
    m->aspect_w = shapes[best][0];
    m->aspect_h = shapes[best][1];
    m->portrait = r[3] > r[2];
    /* X reports 0, or made-up sizes like 160x90 mm (a projector, a capture
     * card); only trust something the shape of the picture */
    if (mw > 50 && mh > 50 && fabs(mw / mh - (double)r[2] / r[3]) < 0.1 * r[2] / r[3])
        m->inches10 = (int)(sqrt(mw * mw + mh * mh) / 2.54 + 0.5);
}

/* The agent says where its monitors are (x16,y16,w16,h16 each): they become
 * the screen's areas, replacing any in the config file.  A rectangle that is
 * a shared monitor (one that only counts while that monitor is set to this
 * machine) is found by desk order: the machine's rectangles left to right
 * against the monitors that can show it, left to right.  When those counts
 * differ, a config file area it overlaps says which monitor it is. */
static void screen_monitors(Screen *s, const unsigned char *p, int n)
{
    int i, k, order[MAX_AREAS], nmon = 0;
    Monitor *mons[MAX_AREAS + 1];

    if (n > MAX_AREAS) n = MAX_AREAS;
    for (i = 0; i < n; i++) {
        int *a = s->area[i];
        a[0] = (int)RKM_GET16(p + 12 * i);
        a[1] = (int)RKM_GET16(p + 12 * i + 2);
        a[2] = (int)RKM_GET16(p + 12 * i + 4);
        a[3] = (int)RKM_GET16(p + 12 * i + 6);
        s->area_mon[i] = -1;
        order[i] = i;
    }
    s->narea = n;
    /* rectangles and monitors, each left to right (then top to bottom) */
    for (i = 1; i < n; i++)
        for (k = i; k > 0; k--) {
            const int *a = s->area[order[k - 1]], *b = s->area[order[k]];
            int t;
            if (a[0] < b[0] || (a[0] == b[0] && a[1] <= b[1])) break;
            t = order[k]; order[k] = order[k - 1]; order[k - 1] = t;
        }
    for (i = 0; i < cfg.nmonitors && nmon <= MAX_AREAS; i++) {
        Monitor *m = &cfg.monitors[i];
        if (m->capture || (m->fixed != s && m->shared != s)) continue;
        for (k = nmon++; k > 0 && (mons[k - 1]->col > m->col ||
                                   (mons[k - 1]->col == m->col && mons[k - 1]->row > m->row)); k--)
            mons[k] = mons[k - 1];
        mons[k] = m;
    }
    if (nmon == n) {
        for (i = 0; i < n; i++) {
            if (mons[i]->shared == s) s->area_mon[order[i]] = (int)(mons[i] - cfg.monitors);
            if (!mons[i]->shape_set)
                monitor_shape_from(mons[i], s->area[order[i]], p + 12 * order[i] + 8);
        }
    } else {
        for (i = 0; i < n; i++) {
            const int *a = s->area[i];
            long best = 0;
            for (k = 0; k < s->cfg_narea; k++) {
                const int *b = s->cfg_area[k];
                long ow = (long)(a[0] + a[2] < b[0] + b[2] ? a[0] + a[2] : b[0] + b[2]) - (a[0] > b[0] ? a[0] : b[0]);
                long oh = (long)(a[1] + a[3] < b[1] + b[3] ? a[1] + a[3] : b[1] + b[3]) - (a[1] > b[1] ? a[1] : b[1]);
                if (ow > 0 && oh > 0 && ow * oh > best) { best = ow * oh; s->area_mon[i] = s->cfg_area_mon[k]; }
            }
        }
    }
    for (i = 0; i < n; i++) {
        const int *a = s->area[i];
        logmsg("%s: monitor %dx%d+%d+%d%s%s", s->name, a[2], a[3], a[0], a[1],
               s->area_mon[i] >= 0 ? " is " : "", s->area_mon[i] >= 0 ? cfg.monitors[s->area_mon[i]].name : "");
    }
}

static void agent_frame(void *ctx, int type, const unsigned char *p, unsigned len)
{
    Conn *c = ctx;

    if (c->dead) return;
    if (type == RKM_HELLO) { on_hello(c, p, len); return; }
    if (!c->screen) return;

    switch (type) {
    case RKM_SCREEN:
        if (len >= 4 && c == c->screen->in && RKM_GET16(p) && RKM_GET16(p + 2)) {
            Screen *s = c->screen;
            s->w = (int)RKM_GET16(p);
            s->h = (int)RKM_GET16(p + 2);
            if (len >= 5 && p[4] && len >= 5 + 12u * p[4]) screen_monitors(s, p + 5, p[4]);
            if (s == active) {
                if (px > s->w - 1) px = s->w - 1;
                if (py > s->h - 1) py = s->h - 1;
                if (has_areas(s) && area_at(s, px, py) < 0) fit_area(s, &px, &py, px, py);
            }
            logmsg("%s: resolution is now %dx%d", s->name, s->w, s->h);
        }
        break;
    case RKM_CLIP_BEGIN:
        free(c->clip_in);
        c->clip_in = NULL;
        c->clip_in_active = 0;
        if (len >= 5 && p[0] == RKM_CLIP_TEXT && RKM_GET32(p + 1) <= MAX_CLIP) {
            c->clip_in_total = RKM_GET32(p + 1);
            c->clip_in = malloc(c->clip_in_total + 1);
            c->clip_in_len = 0;
            c->clip_in_active = c->clip_in != NULL;
        }
        break;
    case RKM_CLIP_DATA:
        if (c->clip_in_active && c->clip_in_len + len <= c->clip_in_total) {
            memcpy(c->clip_in + c->clip_in_len, p, len);
            c->clip_in_len += len;
        }
        break;
    case RKM_CLIP_END:
        if (c->clip_in_active) clip_received(c);
        break;
    case RKM_RUMBLE:                           /* the game shakes the pad */
        if (len >= 5 && p[0] < MAX_PLAYERS) {    /* to the pads playing that player there */
            int i;
            for (i = 0; i < cfg.nacc; i++)
                if (pad_goes(&cfg.acc[i], c->screen) && pad_player(&cfg.acc[i], c->screen) == p[0] + 1)
                    pad_rumble(&cfg.acc[i], RKM_GET16(p + 1), RKM_GET16(p + 3));
        }
        break;
    case RKM_SOUND_EVENT:                      /* how the chirps went: to the panels too */
        if (len >= 8) {
            char line[96];
            int i;
            snprintf(line, sizeof line, "sound %s %u %s %u %lu\n", c->screen->name, RKM_GET16(p),
                     p[2] == RKM_SOUND_PLAYING ? "playing" : p[2] == RKM_SOUND_DONE ? "done" : "failed",
                     (unsigned)p[3], (unsigned long)RKM_GET32(p + 4));
            for (i = 0; i < MAX_CONNS; i++)
                if (conns[i].kind == CONN_PANEL && !conns[i].dead) conn_write(&conns[i], line, strlen(line));
        }
        break;
    case RKM_BUDDY_EVENT:                      /* tell the panels (lil' C's bridge) */
        if (len >= 1) {
            char line[64];
            int i;
            snprintf(line, sizeof line, "buddy %s %s\n", c->screen->name,
                     p[0] == RKM_BUDDY_SUMMON ? "summon" : "click");
            for (i = 0; i < MAX_CONNS; i++)
                if (conns[i].kind == CONN_PANEL && !conns[i].dead) conn_write(&conns[i], line, strlen(line));
        }
        break;
    default:
        break;                                 /* PONG and anything from the future */
    }
}

/* ---- sockets ------------------------------------------------------------ */

static int listen_on(const char *addr, int port)
{
    struct sockaddr_in sa;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), one = 1;

    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1 ||
        bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(fd, 8) < 0) {
        logmsg("cannot listen on %s:%d: %s", addr, port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void accept_conn(int from, int kind)
{
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    int fd = accept4(from, (struct sockaddr *)&sa, &sl, SOCK_NONBLOCK | SOCK_CLOEXEC), one = 1, i;
    Conn *c = NULL;

    if (fd < 0) return;
    for (i = 0; i < MAX_CONNS; i++)
        if (conns[i].kind == CONN_FREE) { c = &conns[i]; break; }
    if (!c) { close(fd); return; }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    memset(c, 0, sizeof *c);
    c->kind = kind;
    c->fd = fd;
    c->last_rx_ms = now_ms();
    rkm_parser_init(&c->ps);
    snprintf(c->peer, sizeof c->peer, "%s:%d", inet_ntoa(sa.sin_addr), ntohs(sa.sin_port));
    if (verbose) logmsg("%s: %s connection", c->peer, kind == CONN_CTL ? "control" : kind == CONN_PANEL ? "panel" : "agent");
}

static void conn_readable(Conn *c)
{
    unsigned char buf[4096];
    ssize_t n = recv(c->fd, buf, sizeof buf, 0);
    ssize_t i;

    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) { c->dead = 1; return; }
    if (n < 0) return;
    c->last_rx_ms = now_ms();

    if (c->kind == CONN_AGENT) {
        if (rkm_feed(&c->ps, buf, (unsigned long)n, agent_frame, c) < 0) {
            logmsg("%s: protocol error", c->peer);
            c->dead = 1;
        }
        return;
    }
    for (i = 0; i < n; i++) {                  /* control: one command per line */
        if (buf[i] == '\n') {
            c->line[c->line_len] = 0;
            c->line_len = 0;
            if (c->kind == CONN_PANEL) run_panel_command(c->line, c);
            else run_command(c->line, c);
        } else if (c->line_len < (int)sizeof c->line - 1) {
            c->line[c->line_len++] = (char)buf[i];
        }
    }
}

static void reap(void)
{
    int i, changed = 0;
    for (i = 0; i < MAX_CONNS; i++) {
        Conn *c = &conns[i];
        if (c->kind == CONN_FREE || !c->dead) continue;
        if (c->screen) { conn_detach(c); changed = 1; }
        close(c->fd);
        free(c->out);
        free(c->clip_in);
        memset(c, 0, sizeof *c);
    }
    if (changed) relayout();
}

static void housekeeping(void)
{
    static long long last_ping;
    long long now = now_ms();
    int i;

    if (active_lost_ms && active && !ready(active) && now - active_lost_ms >= REJOIN_MS) relayout();
    if (now - last_ping < 1000) return;
    for (i = 0; i < MAX_CONNS; i++) {
        Conn *c = &conns[i];
        if (c->kind == CONN_PANEL && !c->dead && now - c->last_rx_ms > DEAD_MS) {
            logmsg("%s: panel went quiet, dropping", c->peer);   /* it pings every few seconds */
            c->dead = 1;
        }
        if (c->kind != CONN_AGENT || c->dead) continue;
        if (!c->screen && now - c->last_rx_ms > HELLO_MS) c->dead = 1;
        else if (now - c->last_rx_ms > DEAD_MS) {
            logmsg("%s: no reply, dropping", c->peer);
            c->dead = 1;
        }
    }
    if (now - last_ping >= PING_MS) {
        last_ping = now;
        for (i = 0; i < MAX_CONNS; i++)
            if (conns[i].kind == CONN_AGENT && conns[i].screen && !conns[i].dead)
                send_frame(&conns[i], RKM_PING, NULL, 0);
    }
}

/* The monitor reported (or was found on) another input: if that input is
 * the shared machine's or the switcher's, follow it. */
static void ddc_changed(Monitor *m, int ok, int input)
{
    int on;
    if (!ok) { logmsg("ddc: %s stopped answering", m->name); return; }
    if (!m->shared || (input != m->ddc_shared && input != m->ddc_switcher)) {
        logmsg("ddc: %s is on input %d", m->name, input);
        return;
    }
    on = input == m->ddc_shared;
    if (on == m->shared_on) return;
    m->shared_on = on;
    logmsg("ddc: %s switched to %s", m->name, on ? m->shared->name : "the switcher");
    config_save_state();
    relayout();
}

static void on_signal(int sig)
{
    (void)sig;
    quit = 1;
}

int main(int argc, char **argv)
{
    const char *path = "retrokm.conf";
    int i, need_uinput = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) path = argv[++i];
        else {
            fprintf(stderr, "usage: %s [-v] [-c retrokm.conf]\n", argv[0]);
            return 2;
        }
    }
    if (config_load(path) < 0) return 1;

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    for (i = 0; i < cfg.nscreens; i++)
        if (cfg.screens[i].local) need_uinput = 1;
    if (need_uinput && uinput_open("RetroKM") < 0) {
        fprintf(stderr, "A screen is marked 'local = yes' but /dev/uinput is not usable.\n"
                        "Run as root or grant access to /dev/uinput.\n");
        return 1;
    }
    for (i = 0; i < cfg.nmonitors; i++)
        if (!cfg.monitors[i].fixed) extron_watch(cfg.monitors[i].extron_output);

    lfd = listen_on(cfg.listen, cfg.port);
    if (lfd < 0) return 1;
    if (cfg.ctl_port > 0 && (cfd = listen_on("127.0.0.1", cfg.ctl_port)) < 0) return 1;
    if (cfg.panel_port > 0 && (pfd_listen = listen_on(cfg.listen, cfg.panel_port)) < 0) return 1;
    extron_init();
    ddc_init();
    input_init();
    logmsg("hub ready: agents on %s:%d, control on 127.0.0.1:%d", cfg.listen, cfg.port, cfg.ctl_port);
    if (pfd_listen >= 0) logmsg("touch panel on %s:%d", cfg.listen, cfg.panel_port);
    relayout();

    while (!quit) {
        struct pollfd pfd[MAX_CONNS + MAX_DEVICES + MAX_PADS + 5];
        Conn *pc[MAX_CONNS + MAX_DEVICES + MAX_PADS + 5];
        int padfds[MAX_PADS], npad, gi;
        int infds[MAX_DEVICES], nin, n = 0, timeout = 100, base_in, xfd, xi = -1, ci = -1, pi = -1, di = -1;


        pfd[n].fd = lfd; pfd[n].events = POLLIN; pc[n++] = NULL;
        if (cfd >= 0) { ci = n; pfd[n].fd = cfd; pfd[n].events = POLLIN; pc[n++] = NULL; }
        if (pfd_listen >= 0) { pi = n; pfd[n].fd = pfd_listen; pfd[n].events = POLLIN; pc[n++] = NULL; }
        if (ddc_fd() >= 0) { di = n; pfd[n].fd = ddc_fd(); pfd[n].events = POLLIN; pc[n++] = NULL; }
        xfd = extron_fd();
        if (xfd >= 0) { xi = n; pfd[n].fd = xfd; pfd[n].events = POLLIN; pc[n++] = NULL; }
        gi = n;
        npad = pad_fds(padfds, MAX_PADS);
        for (i = 0; i < npad; i++) { pfd[n].fd = padfds[i]; pfd[n].events = POLLIN; pc[n++] = NULL; }
        base_in = n;
        nin = input_fds(infds, MAX_DEVICES);
        for (i = 0; i < nin; i++) { pfd[n].fd = infds[i]; pfd[n].events = POLLIN; pc[n++] = NULL; }
        for (i = 0; i < MAX_CONNS; i++) {
            if (conns[i].kind == CONN_FREE) continue;
            pfd[n].fd = conns[i].fd;
            pfd[n].events = (short)(POLLIN | (conns[i].out_len ? POLLOUT : 0));
            pc[n++] = &conns[i];
        }
        if (active && active->move_pending) {
            long long wait = active->min_move_ms - (now_ms() - active->last_move_ms);
            timeout = wait < 1 ? 1 : (int)wait;
        }

        if (poll(pfd, (nfds_t)n, timeout) < 0 && errno != EINTR) break;

        if (pfd[0].revents & POLLIN) accept_conn(lfd, CONN_AGENT);
        if (ci >= 0 && (pfd[ci].revents & POLLIN)) accept_conn(cfd, CONN_CTL);
        if (pi >= 0 && (pfd[pi].revents & POLLIN)) accept_conn(pfd_listen, CONN_PANEL);
        if (di >= 0 && (pfd[di].revents & POLLIN)) ddc_readable(ddc_changed);
        for (i = 0; i < npad; i++)
            if (pfd[gi + i].revents) pad_readable(padfds[i]);
        if (xi >= 0 && pfd[xi].revents) extron_readable();
        for (i = 0; i < nin; i++)
            if (pfd[base_in + i].revents) input_readable(infds[i]);
        for (i = base_in + nin; i < n; i++) {
            Conn *c = pc[i];
            if (c->dead) continue;
            if (pfd[i].revents & (POLLERR | POLLHUP | POLLNVAL)) c->dead = 1;
            else {
                if (pfd[i].revents & POLLIN) conn_readable(c);
                if (pfd[i].revents & POLLOUT) conn_flush(c);
            }
        }

        if (active && active->move_pending &&
            now_ms() - active->last_move_ms >= active->min_move_ms)
            flush_move(active);
        input_tick();
        pad_tick();
        lights_tick();
        extron_tick();
        housekeeping();
        reap();
        panel_push(0);
    }

    if (active) do_leave(active);
    if (restart) {                              /* agents redo their displays too */
        unsigned char f = RKM_RESET_DISPLAYS;
        for (i = 0; i < MAX_CONNS; i++)
            if (conns[i].kind == CONN_AGENT && !conns[i].dead && conns[i].screen) send_frame(&conns[i], RKM_RESET, &f, 1);
    }
    lights_set(0xFFFFFFFF);                     /* the keyboard's own lighting again */
    for (i = 0; i < MAX_CONNS; i++)
        if (conns[i].kind != CONN_FREE) { conn_flush(&conns[i]); close(conns[i].fd); }
    if (restart) {
        /* Start over as a fresh hub: the devices are let go of and taken
         * again, agents reconnect and send their monitors again.  Saved
         * state (monitor moves, colours, links) comes back from the file. */
        long fd, maxfd = sysconf(_SC_OPEN_MAX);
        for (fd = 3; fd < (maxfd > 0 && maxfd < 65536 ? maxfd : 1024); fd++)
            fcntl((int)fd, F_SETFD, FD_CLOEXEC);
        logmsg("hub resetting");
        execv("/proc/self/exe", argv);
        logmsg("reset failed: %s", strerror(errno));
        return 1;                              /* systemd starts it again */
    }
    logmsg("hub stopped");
    return 0;
}

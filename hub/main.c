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

static Screen *active;           /* screen receiving input, or NULL */
static Monitor *active_mon;      /* monitor the pointer is on, or NULL */
static double px, py;            /* pointer, in the active screen's pixels */
static double rel_fx, rel_fy;    /* fractional remainder for relative agents */
static unsigned char mods;       /* physical modifier state, HID bit order */
static int buttons;              /* physical mouse buttons held (bit per button) */
static int locked;               /* edge switching disabled */
static int touched;              /* any input yet; until then the first screen wins */
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

static int ready(const Screen *s)
{
    return s && !s->no_agent && (s->local || s->in);
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
}

static Monitor *monitor_showing(const Screen *s)
{
    int i;
    for (i = 0; s && i < cfg.nmonitors; i++)
        if (cfg.monitors[i].cur == s) return &cfg.monitors[i];
    return NULL;
}

static Monitor *neighbor(const Monitor *m, int dir)
{
    Monitor *best = NULL;
    int i;

    for (i = 0; i < cfg.nmonitors; i++) {
        Monitor *k = &cfg.monitors[i];
        if (k == m || !ready(k->cur) || k->cur == active) continue;
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
        send_frame(s->in, RKM_ENTER, p, 5);
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
}

static void switch_to(Screen *t, Monitor *m, double x, double y)
{
    if (active == t) { active_mon = m; return; }
    if (active) do_leave(active);
    active = t;
    active_mon = m;
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
        active = NULL;                          /* nothing to say goodbye to */
        active_mon = NULL;
    }
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
    queue_move(s);
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
    if (!s) return;
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

    if (!s) return;
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

    s = active;
    if (!s) return;
    if (s->local) { uinput_key(evcode, state); return; }
    if (!usage || !s->in) return;
    flush_move(s);
    p[0] = remap_usage(s, (unsigned char)usage);
    p[1] = (unsigned char)state;
    p[2] = remap_mods(s, mods);
    send_frame(s->in, RKM_KEY, p, 3);
}

/* ---- commands (hotkeys and the control socket) -------------------------- */

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
    ctl_printf(r, "clipboard: %lu bytes, generation %u, from %s\n", (unsigned long)clip_len,
               clip_gen, clip_src ? clip_src->name : "-");
}

static void run_command(char *cmd, Conn *r)
{
    char *argv[4] = { 0, 0, 0, 0 }, *save = NULL, *tok;
    int argc = 0;

    for (tok = strtok_r(cmd, " \t\r\n", &save); tok && argc < 4; tok = strtok_r(NULL, " \t\r\n", &save))
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
    } else if (!strcasecmp(argv[0], "quit")) {
        quit = 1;
    } else {
        ctl_printf(r, "error: commands are status, goto <screen>, show <screen> [monitor], "
                      "tie <in> <out>, lock [on|off], rel <dx> <dy>, btn <n> <0|1>, "
                      "key <name> <0|1>, wheel <n>, quit\n");
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
        else if ((unsigned char)*s < 0x20) sb_printf(b, " ");
        else sb_printf(b, "%c", *s);
    }
    sb_printf(b, "\"");
}

static void layout_json(Sbuf *b)
{
    int i;

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
        sb_printf(b, ",\"input\":%d,\"ready\":%d,\"agent\":%d,\"w\":%d,\"h\":%d}", s->extron_input,
                  ready(s), !s->no_agent, s->w, s->h);
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
        sb_printf(b, ",\"sharedOn\":%d,\"portrait\":%d}", m->shared_on, m->portrait);
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

static void run_panel_command(char *cmd, Conn *r)
{
    char *argv[5] = { 0, 0, 0, 0, 0 }, *save = NULL, *tok;
    int argc = 0;

    for (tok = strtok_r(cmd, " \t\r\n", &save); tok && argc < 5; tok = strtok_r(NULL, " \t\r\n", &save))
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
    } else if (!strcasecmp(argv[0], "share") && argc >= 2) {
        /* share <monitor> [on|off]: which of its inputs the monitor shows */
        Monitor *m = monitor_by_name(argv[1]);
        if (!m || !m->shared) ctl_printf(r, "error %s has no second input\n", argv[1]);
        else {
            m->shared_on = argc >= 3 ? !strcasecmp(argv[2], "on") : !m->shared_on;
            logmsg("panel: monitor %s now shows %s", m->name, m->shared_on ? m->shared->name : "the switcher");
            config_save_state();
            relayout();
        }
    } else if (!strcasecmp(argv[0], "goto") && argc == 2) {
        Screen *s = screen_by_name(argv[1]);
        if (!s || !ready(s)) ctl_printf(r, "error %s is offline\n", argv[1]);
        else goto_screen(s);
    } else if (!strcasecmp(argv[0], "lock")) {
        locked = argc >= 2 ? !strcasecmp(argv[1], "on") : !locked;
    } else if (!strcasecmp(argv[0], "ping")) {
        ctl_printf(r, "pong\n");
    } else {
        ctl_printf(r, "error commands are layout, tie <screen> <monitor>, untie <monitor>, "
                      "move <monitor> <col> <row>, share <monitor> [on|off], goto <screen>, lock [on|off], ping\n");
    }
}

/* ---- agent protocol ----------------------------------------------------- */

static void conn_detach(Conn *c)
{
    Screen *s = c->screen;
    if (!s) return;
    if (s->in == c) s->in = NULL;
    if (s->clip == c) s->clip = NULL;
    c->screen = NULL;
    logmsg("%s: disconnected (%s)", s->name, c->peer);
}

/* Replace a stale connection for a screen without logging a disconnect. */
static void kick(Conn *old)
{
    Screen *s = old->screen;
    if (s && s->in == old) s->in = NULL;
    if (s && s->clip == old) s->clip = NULL;
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
    w[1] = RKM_OK;
    send_frame(c, RKM_WELCOME, w, 2);
    logmsg("%s: connected from %s, %dx%d%s%s%s", s->name, c->peer, s->w, s->h,
           c->caps & RKM_CAP_INPUT ? " input" : "", c->caps & RKM_CAP_CLIP ? " clipboard" : "",
           c->caps & RKM_CAP_REL ? " relative" : "");

    if (s == active) {
        if (c->caps & RKM_CAP_INPUT) do_enter(s);   /* agent restarted under the pointer */
        else clip_sync(s);
    }
    relayout();
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
            if (s == active) {
                if (px > s->w - 1) px = s->w - 1;
                if (py > s->h - 1) py = s->h - 1;
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
    input_init();
    logmsg("hub ready: agents on %s:%d, control on 127.0.0.1:%d", cfg.listen, cfg.port, cfg.ctl_port);
    if (pfd_listen >= 0) logmsg("touch panel on %s:%d", cfg.listen, cfg.panel_port);
    relayout();

    while (!quit) {
        struct pollfd pfd[MAX_CONNS + MAX_DEVICES + 5];
        Conn *pc[MAX_CONNS + MAX_DEVICES + 5];
        int infds[MAX_DEVICES], nin, n = 0, timeout = 100, base_in, xfd, xi = -1, ci = -1, pi = -1;

        pfd[n].fd = lfd; pfd[n].events = POLLIN; pc[n++] = NULL;
        if (cfd >= 0) { ci = n; pfd[n].fd = cfd; pfd[n].events = POLLIN; pc[n++] = NULL; }
        if (pfd_listen >= 0) { pi = n; pfd[n].fd = pfd_listen; pfd[n].events = POLLIN; pc[n++] = NULL; }
        xfd = extron_fd();
        if (xfd >= 0) { xi = n; pfd[n].fd = xfd; pfd[n].events = POLLIN; pc[n++] = NULL; }
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
        extron_tick();
        housekeeping();
        reap();
        panel_push(0);
    }

    if (active) do_leave(active);
    for (i = 0; i < MAX_CONNS; i++)
        if (conns[i].kind != CONN_FREE) { conn_flush(&conns[i]); close(conns[i].fd); }
    logmsg("hub stopped");
    return 0;
}

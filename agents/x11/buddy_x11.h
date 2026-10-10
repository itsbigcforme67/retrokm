/*
 * lil' C's window for the X11 agent (the "desk buddy", RKM_BUDDY).
 *
 * lil' C is a desk robot (see the lil' C project) who sometimes wanders over
 * to a machine on the desk.  When the hub says RKM_BUDDY show, a small xeyes
 * window slides up from the bottom right corner and darts around the screen,
 * stopping to look about or at the pointer; on hide it darts back and slides
 * away.  Clicking it sends RKM_BUDDY_EVENT click back to the hub.
 *
 * Plain Xlib, black and white on the X root weave like xeyes on a bare X
 * server.  Strict C89 for MIPSpro, included by rkm_x11.c.
 */
#include <math.h>

#define BUDDY_FACE_FRAC 0.84   /* the eyes; the rest is the label strip */

static struct {
    Display *d;
    Window w;
    Pixmap buf, weave;
    GC gc, tile_gc, text_gc;
    XFontStruct *font;
    int sw, sh;                /* the screen */
    int ww, wh, face_h;        /* the window */
    int shown;                 /* window mapped */
    int leaving;
    int state;                 /* B_ENTER ... */
    double t0, state_t, seg_t, seg_dur, pause_until, look_t;
    double x, y, from_x, from_y;
    double path_x[3], path_y[3];
    int npath, look_i, follow;
    double gx, gy, stare_x, stare_y;
    char label[64];
} bd;

enum { B_OFF, B_ENTER, B_PAUSE, B_DART, B_EXIT };

static double bd_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static double bd_rand(double lo, double hi)
{
    return lo + (hi - lo) * (rand() / (RAND_MAX + 1.0));
}

static void buddy_init(Display *d)
{
    static const unsigned char weave_rows[4] = { 0x07, 0x0D, 0x0B, 0x0E };  /* X's root_weave */
    int scr = DefaultScreen(d), x, y;
    XSetWindowAttributes a;
    XGCValues v;

    memset(&bd, 0, sizeof bd);
    bd.d = d;
    bd.sw = DisplayWidth(d, scr);
    bd.sh = DisplayHeight(d, scr);
    bd.ww = 260;
    if (bd.ww > bd.sw / 4) bd.ww = bd.sw / 4;
    if (bd.ww < 120) bd.ww = 120;
    bd.wh = bd.ww * 176 / 260;
    bd.face_h = (int)(bd.wh * BUDDY_FACE_FRAC);
    a.override_redirect = True;
    a.background_pixel = WhitePixel(d, scr);
    a.border_pixel = BlackPixel(d, scr);
    a.event_mask = ExposureMask | ButtonPressMask;
    bd.w = XCreateWindow(d, RootWindow(d, scr), bd.sw, bd.sh, (unsigned)bd.ww, (unsigned)bd.wh, 2,
                         CopyFromParent, InputOutput, CopyFromParent,
                         CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWEventMask, &a);
    XStoreName(d, bd.w, "lil' C");
    bd.buf = XCreatePixmap(d, bd.w, (unsigned)bd.ww, (unsigned)bd.wh, (unsigned)DefaultDepth(d, scr));
    bd.weave = XCreatePixmap(d, bd.w, 4, 4, (unsigned)DefaultDepth(d, scr));
    v.foreground = WhitePixel(d, scr);
    v.background = BlackPixel(d, scr);
    bd.gc = XCreateGC(d, bd.w, GCForeground | GCBackground, &v);
    XFillRectangle(d, bd.weave, bd.gc, 0, 0, 4, 4);
    XSetForeground(d, bd.gc, BlackPixel(d, scr));
    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++)
            if (weave_rows[y] >> x & 1) XDrawPoint(d, bd.weave, bd.gc, x, y);
    v.fill_style = FillTiled;
    v.tile = bd.weave;
    bd.tile_gc = XCreateGC(d, bd.w, GCFillStyle | GCTile, &v);
    bd.font = XLoadQueryFont(d, "fixed");
    v.foreground = WhitePixel(d, scr);
    v.background = BlackPixel(d, scr);
    bd.text_gc = XCreateGC(d, bd.w, GCForeground | GCBackground, &v);
    if (bd.font) XSetFont(d, bd.text_gc, bd.font->fid);
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
}

static void bd_ellipse(double cx, double cy, double rx, double ry)
{
    XFillArc(bd.d, bd.buf, bd.gc, (int)(cx - rx), (int)(cy - ry), (unsigned)(rx * 2), (unsigned)(ry * 2),
             0, 360 * 64);
}

/* Draw the face; the pupils look along (gx, gy) in -1..1, or at the pointer */
static void bd_draw(int at_pointer)
{
    int scr = DefaultScreen(bd.d), i;
    unsigned long black = BlackPixel(bd.d, scr), white = WhitePixel(bd.d, scr);
    /* lil' C's eyes are the same shape everywhere: 1.5 times as tall as wide */
    /* (1.5 times as tall as wide, 0.2 of a radius apart, as big as fit) */
    double rx = (bd.ww - 16) / 4.2 < (bd.face_h / 2.0 - 8) / 1.5 ? (bd.ww - 16) / 4.2 : (bd.face_h / 2.0 - 8) / 1.5;
    double ry = rx * 1.5, rim = rx * 0.16;
    double prx = rx * 0.24, pry = prx * 1.2;
    int px = 0, py = 0;

    if (at_pointer) {
        Window r, c;
        int wx, wy;
        unsigned int m;
        XQueryPointer(bd.d, RootWindow(bd.d, scr), &r, &c, &px, &py, &wx, &wy, &m);
    }
    XFillRectangle(bd.d, bd.buf, bd.tile_gc, 0, 0, (unsigned)bd.ww, (unsigned)bd.face_h);
    for (i = 0; i < 2; i++) {
        double cx = bd.ww / 2.0 + (i ? 1 : -1) * rx * 1.1, cy = bd.face_h / 2.0;
        double ax = rx - rim - prx - 3, ay = ry - rim - pry - 3, dx, dy, k;
        XSetForeground(bd.d, bd.gc, black);
        bd_ellipse(cx, cy, rx, ry);
        XSetForeground(bd.d, bd.gc, white);
        bd_ellipse(cx, cy, rx - rim, ry - rim);
        if (at_pointer) {
            dx = px - (bd.x + cx);
            dy = py - (bd.y + cy);
        } else {
            dx = bd.gx * ax;
            dy = bd.gy * ay;
        }
        k = sqrt((dx / ax) * (dx / ax) + (dy / ay) * (dy / ay));
        if (k > 1) { dx /= k; dy /= k; }
        XSetForeground(bd.d, bd.gc, black);
        bd_ellipse(cx + dx, cy + dy, prx, pry);
    }
    XSetForeground(bd.d, bd.gc, black);
    XFillRectangle(bd.d, bd.buf, bd.gc, 0, bd.face_h, (unsigned)bd.ww, (unsigned)(bd.wh - bd.face_h));
    if (bd.font)
        XDrawImageString(bd.d, bd.buf, bd.text_gc, 8, bd.face_h + (bd.wh - bd.face_h) / 2 + 4, bd.label,
                         (int)strlen(bd.label));
    XCopyArea(bd.d, bd.buf, bd.w, bd.gc, 0, 0, (unsigned)bd.ww, (unsigned)bd.wh, 0, 0);
}

static void bd_move(double x, double y)
{
    XWindowChanges ch;
    bd.x = x;
    bd.y = y;
    ch.x = (int)x;
    ch.y = (int)y;
    ch.stack_mode = Above;
    XConfigureWindow(bd.d, bd.w, CWX | CWY | CWStackMode, &ch);
}

static double bd_corner_x(void) { return bd.sw - bd.ww - 28; }
static double bd_shown_y(void) { return bd.sh - bd.wh - 56; }
static double bd_hidden_y(void) { return bd.sh + 4; }

static void bd_dart_to(double x, double y)
{
    double dx = x - bd.x, dy = y - bd.y;
    bd.path_x[0] = x;
    bd.path_y[0] = y;
    bd.npath = 1;
    bd.from_x = bd.x;
    bd.from_y = bd.y;
    bd.seg_t = bd_now();
    bd.seg_dur = sqrt(dx * dx + dy * dy) / 2600.0;
    if (bd.seg_dur < 0.25) bd.seg_dur = 0.25;
    bd.state = B_DART;
}

static void buddy_show(const char *label, unsigned int n)
{
    if (n > sizeof bd.label - 1) n = sizeof bd.label - 1;
    memcpy(bd.label, label, n);
    bd.label[n] = 0;
    if (!n) strcpy(bd.label, "lil' C");
    bd.leaving = 0;
    if (bd.shown) return;                      /* already here: just the new label */
    bd.shown = 1;
    bd.t0 = bd.look_t = bd_now();
    bd.look_i = 0;
    bd.gx = bd.gy = 0;
    bd_move(bd_corner_x(), bd_hidden_y());
    XMapRaised(bd.d, bd.w);
    bd.state = B_ENTER;
}

static void buddy_hide(void)
{
    if (!bd.shown || bd.leaving) return;
    bd.leaving = 1;
    if (bd.state == B_ENTER || bd.state == B_PAUSE) bd_dart_to(bd_corner_x(), bd_shown_y());
}

/* Advance the animation; returns how many ms until it wants the next call
 * (-1: nothing to do, the window is gone) */
static int buddy_tick(void)
{
    static const double looks[4][3] = { { 0, 0, 0.3 }, { -0.9, -0.2, 0.5 }, { 0.9, -0.2, 0.5 }, { 0, -0.9, 0.4 } };
    double now = bd_now(), f;

    if (!bd.shown) return -1;
    switch (bd.state) {
    case B_ENTER:                              /* slide up, glance around */
        f = (now - bd.t0) / 0.5;
        if (f > 1) f = 1;
        f = 1 - (1 - f) * (1 - f) * (1 - f);
        bd_move(bd_corner_x(), bd_hidden_y() + (bd_shown_y() - bd_hidden_y()) * f);
        bd.gx += (looks[bd.look_i][0] - bd.gx) * 0.35;
        bd.gy += (looks[bd.look_i][1] - bd.gy) * 0.35;
        bd_draw(0);
        if (now - bd.look_t > looks[bd.look_i][2]) {
            bd.look_t = now;
            if (++bd.look_i >= 4) { bd.state = B_PAUSE; bd.pause_until = now; }
        }
        break;
    case B_PAUSE:                              /* look around here */
        if (bd.follow) bd_draw(1);
        else {
            if (rand() % 25 == 0) { bd.stare_x = bd_rand(-1, 1); bd.stare_y = bd_rand(-1, 1); }
            bd.gx += (bd.stare_x - bd.gx) * 0.3;
            bd.gy += (bd.stare_y - bd.gy) * 0.3;
            bd_draw(0);
        }
        if (now >= bd.pause_until) {
            if (bd.leaving) { bd.state = B_EXIT; bd.state_t = now; break; }
            bd_dart_to(bd_rand(20, bd.sw - bd.ww - 20 > 20 ? bd.sw - bd.ww - 20 : 20),
                       bd_rand(20, bd.sh - bd.wh - 60 > 20 ? bd.sh - bd.wh - 60 : 20));
        }
        break;
    case B_DART: {                             /* zip there, eyes leading */
        double k, dx = bd.path_x[0] - bd.from_x, dy = bd.path_y[0] - bd.from_y, n;
        f = (now - bd.seg_t) / bd.seg_dur;
        if (f > 1) f = 1;
        k = f * f * (3 - 2 * f);
        bd_move(bd.from_x + dx * k, bd.from_y + dy * k);
        n = sqrt(dx * dx + dy * dy);
        if (n < 1) n = 1;
        bd.gx += (dx / n - bd.gx) * 0.5;
        bd.gy += (dy / n - bd.gy) * 0.5;
        bd_draw(0);
        if (f >= 1) {
            if (bd.leaving && (fabs(bd.x - bd_corner_x()) > 2 || fabs(bd.y - bd_shown_y()) > 2)) {
                bd_dart_to(bd_corner_x(), bd_shown_y());   /* home to the corner first */
                break;
            }
            bd.state = B_PAUSE;
            bd.follow = rand() % 100 < 20;
            bd.pause_until = now + (bd.leaving ? 0.3 : bd_rand(2.0, 7.0));
        }
        break;
    }
    case B_EXIT:                               /* back in the corner: slide away */
        f = (now - bd.state_t) / 0.45;
        if (f > 1) f = 1;
        bd_move(bd_corner_x(), bd_shown_y() + (bd_hidden_y() - bd_shown_y()) * f * f);
        bd.gx = 0;
        bd.gy = 0.9;                           /* looking down, back towards the desk */
        bd_draw(0);
        if (f >= 1) {
            XUnmapWindow(bd.d, bd.w);
            bd.shown = 0;
            bd.state = B_OFF;
            return -1;
        }
        break;
    default:
        break;
    }
    return 25;
}

/* An X event for lil' C's window?  Returns 1 if it was his, 2 if he was clicked. */
static int buddy_event(XEvent *ev)
{
    if (ev->xany.window != bd.w || !bd.w) return 0;
    if (ev->type == Expose && bd.shown) bd_draw(0);
    return ev->type == ButtonPress ? 2 : 1;
}

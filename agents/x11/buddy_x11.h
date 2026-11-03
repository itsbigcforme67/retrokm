/*
 * The desk pet's window for the X11 agent (the "buddy", RKM_BUDDY).
 *
 * A desk pet sometimes wanders over to a machine on the desk.  When the hub
 * says RKM_BUDDY show, a small window with its eyes slides up from the
 * bottom right corner and darts around the screen, stopping to look about
 * or at the pointer; on hide it darts back and slides away.  Clicking it
 * sends RKM_BUDDY_EVENT click back to the hub.
 *
 * The eyes are drawn from numbers (RKM_BUDDY_LOOK, rkm_look in
 * rkm_proto.h); without them they are the default: black and white on the
 * X root weave, like xeyes on a bare X server.  RKM_BUDDY_GAZE tells it
 * where to look, RKM_BUDDY_SAY puts a caption in the label strip.
 *
 * Plain Xlib.  Strict C89 for MIPSpro, included by rkm_x11.c.
 */
#include <math.h>

#define BUDDY_FACE_FRAC 0.84   /* the eyes; the rest is the label strip */
#define BUDDY_BLINK_S   0.22   /* one blink, closed and open again */

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
    rkm_look look;             /* how it looks */
    unsigned long paper, eye, ink;   /* its colours as pixels */
    unsigned long owned[3];    /* ... the ones we allocated */
    int nowned;
    int gmode;                 /* RKM_GAZE_*: what the hub said */
    double hgx, hgy, gaze_until;   /* gaze_until 0: until told otherwise */
    double blink_t, next_blink;    /* a blink started at; when the next is due */
    char say[128];
    double say_t, say_until;
} bd;

enum { B_OFF, B_ENTER, B_PAUSE, B_DART, B_EXIT };

/* The pet's name when the hub gives none (set before buddy_init) */
static char buddy_name[RKM_NAME_MAX + 1];

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

static unsigned long bd_pixel(const unsigned char *rgb, int white)
{
    int scr = DefaultScreen(bd.d);
    XColor c;
    if (!(bd.look.flags & RKM_LOOK_COLOR) || DefaultDepth(bd.d, scr) < 4 || bd.nowned >= 3)
        return white ? WhitePixel(bd.d, scr) : BlackPixel(bd.d, scr);
    c.red = (unsigned short)(rgb[0] * 257);
    c.green = (unsigned short)(rgb[1] * 257);
    c.blue = (unsigned short)(rgb[2] * 257);
    c.flags = DoRed | DoGreen | DoBlue;
    if (!XAllocColor(bd.d, DefaultColormap(bd.d, scr), &c))
        return white ? WhitePixel(bd.d, scr) : BlackPixel(bd.d, scr);
    bd.owned[bd.nowned++] = c.pixel;
    return c.pixel;
}

/* The colours and the weave for the current look */
static void bd_colors(void)
{
    static const unsigned char weave_rows[4] = { 0x07, 0x0D, 0x0B, 0x0E };  /* X's root_weave */
    int scr = DefaultScreen(bd.d), x, y;

    if (bd.nowned) XFreeColors(bd.d, DefaultColormap(bd.d, scr), bd.owned, bd.nowned, 0);
    bd.nowned = 0;
    bd.paper = bd_pixel(bd.look.paper, 1);
    bd.eye = bd_pixel(bd.look.eye, 1);
    bd.ink = bd_pixel(bd.look.ink, 0);
    XSetForeground(bd.d, bd.gc, bd.paper);
    XFillRectangle(bd.d, bd.weave, bd.gc, 0, 0, 4, 4);
    XSetForeground(bd.d, bd.gc, bd.ink);
    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++)
            if (weave_rows[y] >> x & 1) XDrawPoint(bd.d, bd.weave, bd.gc, x, y);
    XSetTile(bd.d, bd.tile_gc, bd.weave);
    XSetForeground(bd.d, bd.text_gc, bd.eye);
    XSetBackground(bd.d, bd.text_gc, bd.ink);
}

static void buddy_init(Display *d)
{
    int scr = DefaultScreen(d);
    XSetWindowAttributes a;
    XGCValues v;

    memset(&bd, 0, sizeof bd);
    bd.d = d;
    rkm_look_default(&bd.look);
    strcpy(bd.look.name, buddy_name);
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
    XStoreName(d, bd.w, buddy_name[0] ? buddy_name : "desk pet");
    bd.buf = XCreatePixmap(d, bd.w, (unsigned)bd.ww, (unsigned)bd.wh, (unsigned)DefaultDepth(d, scr));
    bd.weave = XCreatePixmap(d, bd.w, 4, 4, (unsigned)DefaultDepth(d, scr));
    v.foreground = BlackPixel(d, scr);
    v.background = WhitePixel(d, scr);
    v.arc_mode = ArcChord;
    bd.gc = XCreateGC(d, bd.w, GCForeground | GCBackground | GCArcMode, &v);
    v.fill_style = FillTiled;
    v.tile = bd.weave;
    bd.tile_gc = XCreateGC(d, bd.w, GCFillStyle | GCTile, &v);
    bd.font = XLoadQueryFont(d, "fixed");
    bd.text_gc = XCreateGC(d, bd.w, 0, &v);
    if (bd.font) XSetFont(d, bd.text_gc, bd.font->fid);
    bd_colors();
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
}

/* RKM_BUDDY_LOOK: new numbers for the eyes */
static void buddy_look(const unsigned char *p, unsigned int n)
{
    rkm_look_parse(&bd.look, p, n);
    if (!bd.look.name[0]) strcpy(bd.look.name, buddy_name);
    bd_colors();
    XStoreName(bd.d, bd.w, bd.look.name[0] ? bd.look.name : "desk pet");
    bd.next_blink = 0;
}

/* RKM_BUDDY_GAZE: where to look, from the hub */
static void buddy_gaze(const unsigned char *p, unsigned int n)
{
    if (n < 3) return;
    bd.gmode = p[0];
    bd.hgx = (signed char)p[1] / 100.0;
    bd.hgy = (signed char)p[2] / 100.0;
    bd.gaze_until = n >= 4 && p[3] ? bd_now() + p[3] / 10.0 : 0;
    if (n >= 5 && p[4]) bd.blink_t = bd_now();
}

/* RKM_BUDDY_SAY: a caption in the label strip for a while */
static void buddy_say(const unsigned char *p, unsigned int n)
{
    if (n < 1) return;
    if (n - 1 > sizeof bd.say - 1) n = sizeof bd.say;
    memcpy(bd.say, p + 1, n - 1);
    bd.say[n - 1] = 0;
    bd.say_t = bd_now();
    bd.say_until = bd.say_t + (p[0] ? p[0] : 4);
}

static void bd_ellipse(double cx, double cy, double rx, double ry)
{
    XFillArc(bd.d, bd.buf, bd.gc, (int)(cx - rx), (int)(cy - ry), (unsigned)(rx * 2), (unsigned)(ry * 2),
             0, 360 * 64);
}

/* The top `cover` (0..1) of an ellipse: an upper lid */
static void bd_lid(double cx, double cy, double rx, double ry, double cover)
{
    double t;
    if (cover <= 0.01) return;
    if (cover >= 0.99) { bd_ellipse(cx, cy, rx, ry); return; }
    /* X arc angles are the ellipse's own (skewed), so the chord at height
     * ry * (1 - 2 cover) above the centre starts at asin(1 - 2 cover) */
    t = asin(1 - 2 * cover) * 57.29578;   /* in degrees */
    XFillArc(bd.d, bd.buf, bd.gc, (int)(cx - rx), (int)(cy - ry), (unsigned)(rx * 2), (unsigned)(ry * 2),
             (int)(t * 64), (int)((180 - 2 * t) * 64));
}

/* How closed the lids are now: the look's lid plus any blink */
static double bd_cover(double now)
{
    double l = bd.look.lid / 100.0, b = 0, f;
    if (bd.look.blink && now >= bd.next_blink) {
        if (bd.next_blink) bd.blink_t = now;
        bd.next_blink = now + bd.look.blink * bd_rand(0.5, 1.5);
    }
    if (bd.blink_t && (f = (now - bd.blink_t) / BUDDY_BLINK_S) < 1) b = 1 - fabs(2 * f - 1);
    return l + (1 - l) * b;
}

/* Draw the face; the pupils look along (gx, gy) in -1..1, or at the pointer */
static void bd_draw(int at_pointer)
{
    const rkm_look *l = &bd.look;
    int scr = DefaultScreen(bd.d), i;
    double eh = bd.face_h * (l->mouth ? 0.78 : 1.0), shape = l->shape / 10.0, g = l->gap / 100.0;
    double rx = (bd.ww - 16) / (4 + g) < (eh / 2.0 - 8) / shape ? (bd.ww - 16) / (4 + g) : (eh / 2.0 - 8) / shape;
    double ry = rx * shape, rim = rx * l->rim / 100.0;
    double prx = rx * l->pupil / 100.0, pry = prx * l->pupil_shape / 10.0;
    double cover = bd_cover(bd_now());
    const char *text = bd.say_until > bd_now() ? bd.say : bd.label;
    int px = 0, py = 0;

    if (at_pointer) {
        Window r, c;
        int wx, wy;
        unsigned int m;
        XQueryPointer(bd.d, RootWindow(bd.d, scr), &r, &c, &px, &py, &wx, &wy, &m);
    }
    if (l->flags & RKM_LOOK_PLAIN) {
        XSetForeground(bd.d, bd.gc, bd.paper);
        XFillRectangle(bd.d, bd.buf, bd.gc, 0, 0, (unsigned)bd.ww, (unsigned)bd.face_h);
    } else {
        XFillRectangle(bd.d, bd.buf, bd.tile_gc, 0, 0, (unsigned)bd.ww, (unsigned)bd.face_h);
    }
    for (i = 0; i < 2; i++) {
        double cx = bd.ww / 2.0 + (i ? 1 : -1) * rx * (1 + g / 2), cy = eh / 2.0;
        double ax = rx - rim - prx - 3, ay = ry - rim - pry - 3, dx, dy, k;
        if (ax < 1) ax = 1;
        if (ay < 1) ay = 1;
        XSetForeground(bd.d, bd.gc, bd.ink);
        bd_ellipse(cx, cy, rx, ry);
        XSetForeground(bd.d, bd.gc, bd.eye);
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
        XSetForeground(bd.d, bd.gc, bd.ink);
        bd_ellipse(cx + dx, cy + dy, prx, pry);
        bd_lid(cx, cy, rx, ry, cover);
    }
    if (l->mouth) {                            /* a line, or a curve for a smile / frown */
        double mw = bd.ww * l->mouth / 100.0, my = eh + (bd.face_h - eh) / 2.0;
        double mh = mw * (l->smile < 0 ? -l->smile : l->smile) / 200.0;
        int lw = (int)(rx * 0.12) < 2 ? 2 : (int)(rx * 0.12);
        XSetForeground(bd.d, bd.gc, bd.ink);
        XSetLineAttributes(bd.d, bd.gc, (unsigned)lw, LineSolid, CapRound, JoinRound);
        if (mh < 1)
            XDrawLine(bd.d, bd.buf, bd.gc, (int)(bd.ww / 2.0 - mw / 2), (int)my, (int)(bd.ww / 2.0 + mw / 2), (int)my);
        else
            XDrawArc(bd.d, bd.buf, bd.gc, (int)(bd.ww / 2.0 - mw / 2), (int)(l->smile > 0 ? my - 2 * mh : my),
                     (unsigned)mw, (unsigned)(2 * mh), l->smile > 0 ? 180 * 64 : 0, 180 * 64);
        XSetLineAttributes(bd.d, bd.gc, 0, LineSolid, CapButt, JoinMiter);
    }
    XSetForeground(bd.d, bd.gc, bd.ink);
    XFillRectangle(bd.d, bd.buf, bd.gc, 0, bd.face_h, (unsigned)bd.ww, (unsigned)(bd.wh - bd.face_h));
    if (bd.font) {
        int n = (int)strlen(text), tw = XTextWidth(bd.font, text, n), x = 8;
        if (tw > bd.ww - 16) {                 /* too long: scroll it, after a second */
            double s = (bd_now() - bd.say_t - 1) * 40;
            if (s > 0) x -= (int)s % (tw + 40);
            XDrawImageString(bd.d, bd.buf, bd.text_gc, x + tw + 40, bd.face_h + (bd.wh - bd.face_h) / 2 + 4, text, n);
        }
        XDrawImageString(bd.d, bd.buf, bd.text_gc, x, bd.face_h + (bd.wh - bd.face_h) / 2 + 4, text, n);
    }
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
    if (!n) strcpy(bd.label, bd.look.name);
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

/* The hub said where to look, and that still holds */
static int bd_hub_gaze(double now)
{
    if (bd.gmode != RKM_GAZE_FREE && bd.gaze_until && now >= bd.gaze_until) bd.gmode = RKM_GAZE_FREE;
    return bd.gmode;
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
        switch (bd_hub_gaze(now)) {
        case RKM_GAZE_POINTER:                 /* told to watch: stay and watch */
            bd_draw(1);
            if (!bd.leaving && bd.pause_until < now + 0.5) bd.pause_until = now + 0.5;
            break;
        case RKM_GAZE_AT:
            bd.gx += (bd.hgx - bd.gx) * 0.3;
            bd.gy += (bd.hgy - bd.gy) * 0.3;
            bd_draw(0);
            if (!bd.leaving && bd.pause_until < now + 0.5) bd.pause_until = now + 0.5;
            break;
        default:
            if (bd.follow) bd_draw(1);
            else {
                if (rand() % 25 == 0) { bd.stare_x = bd_rand(-1, 1); bd.stare_y = bd_rand(-1, 1); }
                bd.gx += (bd.stare_x - bd.gx) * 0.3;
                bd.gy += (bd.stare_y - bd.gy) * 0.3;
                bd_draw(0);
            }
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
        if (bd_hub_gaze(now) == RKM_GAZE_AT) { /* looking where it is going */
            bd.gx = bd.hgx;
            bd.gy = bd.hgy;
        } else {
            bd.gx = 0;
            bd.gy = 0.9;                       /* looking down, back towards the desk */
        }
        bd_draw(0);
        if (f >= 1) {
            XUnmapWindow(bd.d, bd.w);
            bd.shown = 0;
            bd.state = B_OFF;
            bd.gmode = RKM_GAZE_FREE;
            bd.say_until = 0;
            return -1;
        }
        break;
    default:
        break;
    }
    return 25;
}

/* An X event for the pet's window?  Returns 1 if it was its, 2 if it was clicked. */
static int buddy_event(XEvent *ev)
{
    if (ev->xany.window != bd.w || !bd.w) return 0;
    if (ev->type == Expose && bd.shown) bd_draw(0);
    return ev->type == ButtonPress ? 2 : 1;
}

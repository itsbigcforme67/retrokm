/*
 * The desk pet's window for the Windows agent (the "buddy", RKM_BUDDY).
 *
 * A desk pet sometimes wanders over to a machine on the desk.  When the hub
 * says RKM_BUDDY show, a small window with its eyes slides up from the
 * bottom right corner (above the taskbar) and darts around the screen,
 * stopping to look about or at the pointer; on hide it darts back and
 * slides away.  Clicking it sends RKM_BUDDY_EVENT click.
 *
 * The eyes are drawn from numbers (RKM_BUDDY_LOOK, rkm_look in
 * rkm_proto.h), like the X11 version; without them, black and white on the
 * X root weave.  RKM_BUDDY_GAZE says where to look, RKM_BUDDY_SAY puts a
 * caption in the label strip.
 *
 * Plain GDI.  Never takes the focus.  C89, included by rkm_win32.c.
 */
#include <math.h>

#define BUDDY_TIMER 2
#define BUDDY_BLINK_MS 220

static struct {
    HWND w;
    HBRUSH weave;
    int sw, sh;                  /* the work area (screen minus taskbar) */
    int sx, sy;
    int ww, wh, face_h;
    int shown, leaving, state, look_i, follow;
    DWORD t0, state_t, seg_t, look_t, pause_until;
    double seg_dur;
    double x, y, from_x, from_y, to_x, to_y;
    double gx, gy, stare_x, stare_y;
    char label[64];
    rkm_look look;               /* how it looks */
    COLORREF paper, eye, ink;
    HBRUSH br_paper, br_eye, br_ink;
    int gmode;                   /* RKM_GAZE_*: what the hub said */
    double hgx, hgy;
    DWORD gaze_until;            /* 0: until told otherwise */
    DWORD blink_t, next_blink;
    char say[128];
    DWORD say_t, say_until;
} bd;

enum { B_OFF, B_ENTER, B_PAUSE, B_DART, B_EXIT };

static void buddy_clicked(void);   /* in rkm_win32.c */
static void buddy_tick(void);

static double bd_rand(double lo, double hi)
{
    return lo + (hi - lo) * (rand() / (RAND_MAX + 1.0));
}

static double bd_corner_x(void) { return bd.sx + bd.sw - bd.ww - 28; }
static double bd_shown_y(void) { return bd.sy + bd.sh - bd.wh - 24; }
static double bd_hidden_y(void) { return GetSystemMetrics(SM_CYSCREEN) + 4; }   /* below the taskbar too */

/* The brushes for the current look */
static void bd_colors(void)
{
    const rkm_look *l = &bd.look;
    if (bd.br_paper) { DeleteObject(bd.br_paper); DeleteObject(bd.br_eye); DeleteObject(bd.br_ink); }
    if (l->flags & RKM_LOOK_COLOR) {
        bd.paper = RGB(l->paper[0], l->paper[1], l->paper[2]);
        bd.eye = RGB(l->eye[0], l->eye[1], l->eye[2]);
        bd.ink = RGB(l->ink[0], l->ink[1], l->ink[2]);
    } else {
        bd.paper = bd.eye = RGB(255, 255, 255);
        bd.ink = RGB(0, 0, 0);
    }
    bd.br_paper = CreateSolidBrush(bd.paper);
    bd.br_eye = CreateSolidBrush(bd.eye);
    bd.br_ink = CreateSolidBrush(bd.ink);
}

static void bd_ellipse(HDC dc, double cx, double cy, double rx, double ry, HBRUSH b)
{
    SelectObject(dc, b);
    Ellipse(dc, (int)(cx - rx), (int)(cy - ry), (int)(cx + rx), (int)(cy + ry));
}

/* The top `cover` (0..1) of an ellipse: an upper lid */
static void bd_lid(HDC dc, double cx, double cy, double rx, double ry, double cover)
{
    double yl, xw;
    if (cover <= 0.01) return;
    if (cover >= 0.99) { bd_ellipse(dc, cx, cy, rx, ry, bd.br_ink); return; }
    yl = ry * (1 - 2 * cover);                 /* the lid's edge, above the centre */
    xw = rx * sqrt(1 - (yl / ry) * (yl / ry));
    SelectObject(dc, bd.br_ink);
    Chord(dc, (int)(cx - rx), (int)(cy - ry), (int)(cx + rx), (int)(cy + ry),
          (int)(cx + xw), (int)(cy - yl), (int)(cx - xw), (int)(cy - yl));
}

/* How closed the lids are now: the look's lid plus any blink */
static double bd_cover(DWORD now)
{
    double l = bd.look.lid / 100.0, b = 0, f;
    if (bd.look.blink && (!bd.next_blink || (long)(now - bd.next_blink) >= 0)) {
        if (bd.next_blink) bd.blink_t = now;
        bd.next_blink = now + (DWORD)(bd.look.blink * bd_rand(500, 1500));
    }
    if (bd.blink_t && (f = (double)(now - bd.blink_t) / BUDDY_BLINK_MS) < 1) b = 1 - fabs(2 * f - 1);
    return l + (1 - l) * b;
}

static void bd_paint(HDC out)
{
    const rkm_look *l = &bd.look;
    HDC dc = CreateCompatibleDC(out);
    HBITMAP bm = CreateCompatibleBitmap(out, bd.ww, bd.wh), oldbm;
    RECT r;
    POINT pt;
    DWORD now = GetTickCount();
    double eh = bd.face_h * (l->mouth ? 0.78 : 1.0), shape = l->shape / 10.0, g = l->gap / 100.0;
    double rx = (bd.ww - 16) / (4 + g) < (eh / 2.0 - 8) / shape ? (bd.ww - 16) / (4 + g) : (eh / 2.0 - 8) / shape;
    double ry = rx * shape, rim = rx * l->rim / 100.0;
    double prx = rx * l->pupil / 100.0, pry = prx * l->pupil_shape / 10.0;
    double cover = bd_cover(now);
    const char *text = bd.say_until && (long)(now - bd.say_until) < 0 ? bd.say : bd.label;
    int i, at_pointer = (bd.state == B_PAUSE && bd.gmode == RKM_GAZE_POINTER) ||
                        (bd.follow && bd.state == B_PAUSE && bd.gmode == RKM_GAZE_FREE);
    SIZE ts;

    oldbm = (HBITMAP)SelectObject(dc, bm);
    SelectObject(dc, GetStockObject(NULL_PEN));
    r.left = 0; r.top = 0; r.right = bd.ww; r.bottom = bd.face_h;
    if (l->flags & RKM_LOOK_PLAIN) FillRect(dc, &r, bd.br_paper);
    else {                                     /* the weave: 0 bits in the text colour */
        SetTextColor(dc, bd.ink);
        SetBkColor(dc, bd.paper);
        FillRect(dc, &r, bd.weave);
    }
    GetCursorPos(&pt);
    for (i = 0; i < 2; i++) {
        double cx = bd.ww / 2.0 + (i ? 1 : -1) * rx * (1 + g / 2), cy = eh / 2.0;
        double ax = rx - rim - prx - 3, ay = ry - rim - pry - 3, dx, dy, k;
        if (ax < 1) ax = 1;
        if (ay < 1) ay = 1;
        bd_ellipse(dc, cx, cy, rx, ry, bd.br_ink);
        bd_ellipse(dc, cx, cy, rx - rim, ry - rim, bd.br_eye);
        if (at_pointer) {
            dx = pt.x - (bd.x + cx);
            dy = pt.y - (bd.y + cy);
        } else {
            dx = bd.gx * ax;
            dy = bd.gy * ay;
        }
        k = sqrt((dx / ax) * (dx / ax) + (dy / ay) * (dy / ay));
        if (k > 1) { dx /= k; dy /= k; }
        bd_ellipse(dc, cx + dx, cy + dy, prx, pry, bd.br_ink);
        bd_lid(dc, cx, cy, rx, ry, cover);
    }
    if (l->mouth) {                            /* a line, or a curve for a smile / frown */
        double mw = bd.ww * l->mouth / 100.0, my = eh + (bd.face_h - eh) / 2.0;
        double mh = mw * (l->smile < 0 ? -l->smile : l->smile) / 200.0;
        int lw = (int)(rx * 0.12) < 2 ? 2 : (int)(rx * 0.12);
        HPEN pen = CreatePen(PS_SOLID, lw, bd.ink), oldpen = (HPEN)SelectObject(dc, pen);
        int x0 = (int)(bd.ww / 2.0 - mw / 2), x1 = (int)(bd.ww / 2.0 + mw / 2);
        if (mh < 1) {
            MoveToEx(dc, x0, (int)my, NULL);
            LineTo(dc, x1, (int)my);
        } else if (l->smile > 0) {             /* the bottom half of an ellipse */
            Arc(dc, x0, (int)(my - 2 * mh), x1, (int)my, x0, (int)(my - mh), x1, (int)(my - mh));
        } else {                               /* the top half */
            Arc(dc, x0, (int)my, x1, (int)(my + 2 * mh), x1, (int)(my + mh), x0, (int)(my + mh));
        }
        SelectObject(dc, oldpen);
        DeleteObject(pen);
    }
    r.top = bd.face_h; r.bottom = bd.wh;
    FillRect(dc, &r, bd.br_ink);
    SetBkColor(dc, bd.ink);
    SetTextColor(dc, bd.eye);
    SelectObject(dc, GetStockObject(ANSI_FIXED_FONT));
    GetTextExtentPoint32(dc, text, lstrlen(text), &ts);
    {
        int x = 8, y = bd.face_h + (bd.wh - bd.face_h - 12) / 2;
        if (ts.cx > bd.ww - 16) {              /* too long: scroll it, after a second */
            long s = ((long)(now - bd.say_t) - 1000) * 40 / 1000;
            if (s > 0) x -= (int)(s % (ts.cx + 40));
            TextOut(dc, x + ts.cx + 40, y, text, lstrlen(text));
        }
        TextOut(dc, x, y, text, lstrlen(text));
    }
    /* a black border, like the X window's */
    SelectObject(dc, GetStockObject(BLACK_PEN));
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, 0, 0, bd.ww, bd.wh);
    BitBlt(out, 0, 0, bd.ww, bd.wh, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldbm);
    DeleteObject(bm);
    DeleteDC(dc);
}

static void bd_redraw(void)
{
    HDC dc = GetDC(bd.w);
    bd_paint(dc);
    ReleaseDC(bd.w, dc);
}

static void bd_move(double x, double y)
{
    bd.x = x;
    bd.y = y;
    SetWindowPos(bd.w, HWND_TOPMOST, (int)x, (int)y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

static void bd_dart_to(double x, double y)
{
    double dx = x - bd.x, dy = y - bd.y;
    bd.from_x = bd.x;
    bd.from_y = bd.y;
    bd.to_x = x;
    bd.to_y = y;
    bd.seg_t = GetTickCount();
    bd.seg_dur = sqrt(dx * dx + dy * dy) / 2600.0;
    if (bd.seg_dur < 0.25) bd.seg_dur = 0.25;
    bd.state = B_DART;
}

static LRESULT CALLBACK bd_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        bd_paint(dc);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;                  /* never steal the focus */
    case WM_LBUTTONDOWN:
        buddy_clicked();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (wp == BUDDY_TIMER) buddy_tick();
        return 0;
    }
    return DefWindowProc(w, msg, wp, lp);
}

static void buddy_init(HINSTANCE inst)
{
    static const WORD weave_rows[8] = { 0x07, 0x0D, 0x0B, 0x0E, 0x07, 0x0D, 0x0B, 0x0E };
    WORD bits[8];
    HBITMAP pat;
    WNDCLASS wc;
    int i;

    memset(&bd, 0, sizeof bd);
    rkm_look_default(&bd.look);
    bd_colors();
    for (i = 0; i < 8; i++) {                  /* rows of 16 bits, leftmost pixel in the top bit; 1 = paper */
        int x;
        bits[i] = 0;
        for (x = 0; x < 8; x++)
            if (!(weave_rows[i] >> (x & 3) & 1)) bits[i] |= (WORD)(0x80 >> x);
    }
    pat = CreateBitmap(8, 8, 1, 1, bits);
    bd.weave = CreatePatternBrush(pat);
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = bd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "rkmBuddy";
    RegisterClass(&wc);
    bd.ww = 260;
    if (bd.ww > GetSystemMetrics(SM_CXSCREEN) / 4) bd.ww = GetSystemMetrics(SM_CXSCREEN) / 4;
    if (bd.ww < 120) bd.ww = 120;
    bd.wh = bd.ww * 176 / 260;
    bd.face_h = bd.wh * 84 / 100;
    bd.w = CreateWindowEx(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, "rkmBuddy", "desk pet", WS_POPUP,
                          0, 0, bd.ww, bd.wh, NULL, NULL, inst, NULL);
    srand((unsigned)GetTickCount());
}

/* RKM_BUDDY_LOOK: new numbers for the eyes */
static void buddy_look(const unsigned char *p, unsigned int n)
{
    rkm_look_parse(&bd.look, p, n);
    bd_colors();
    if (bd.w) SetWindowText(bd.w, bd.look.name[0] ? bd.look.name : "desk pet");
    bd.next_blink = 0;
    if (bd.shown) bd_redraw();
}

/* RKM_BUDDY_GAZE: where to look, from the hub */
static void buddy_gaze(const unsigned char *p, unsigned int n)
{
    if (n < 3) return;
    bd.gmode = p[0];
    bd.hgx = (signed char)p[1] / 100.0;
    bd.hgy = (signed char)p[2] / 100.0;
    bd.gaze_until = n >= 4 && p[3] ? GetTickCount() + p[3] * 100UL : 0;
    if (!bd.gaze_until && n >= 4 && p[3]) bd.gaze_until = 1;   /* 0 means "until told" */
    if (n >= 5 && p[4]) bd.blink_t = GetTickCount();
}

/* RKM_BUDDY_SAY: a caption in the label strip for a while */
static void buddy_say(const unsigned char *p, unsigned int n)
{
    if (n < 1) return;
    if (n - 1 > sizeof bd.say - 1) n = sizeof bd.say;
    memcpy(bd.say, p + 1, n - 1);
    bd.say[n - 1] = 0;
    bd.say_t = GetTickCount();
    bd.say_until = bd.say_t + (p[0] ? p[0] : 4) * 1000UL;
    if (!bd.say_until) bd.say_until = 1;
}

static void buddy_show(const char *label, unsigned int n)
{
    RECT wa;
    if (!bd.w) return;
    if (n > sizeof bd.label - 1) n = sizeof bd.label - 1;
    memcpy(bd.label, label, n);
    bd.label[n] = 0;
    if (!n) lstrcpy(bd.label, bd.look.name);
    bd.leaving = 0;
    if (bd.shown) { bd_redraw(); return; }
    if (!SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0)) {
        wa.left = wa.top = 0;
        wa.right = GetSystemMetrics(SM_CXSCREEN);
        wa.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    bd.sx = wa.left; bd.sy = wa.top;
    bd.sw = wa.right - wa.left; bd.sh = wa.bottom - wa.top;
    bd.shown = 1;
    bd.t0 = bd.look_t = GetTickCount();
    bd.look_i = 0;
    bd.gx = bd.gy = 0;
    bd.follow = 0;
    bd.state = B_ENTER;
    bd_move(bd_corner_x(), bd_hidden_y());
    ShowWindow(bd.w, SW_SHOWNOACTIVATE);
    SetTimer(bd.w, BUDDY_TIMER, 30, NULL);
}

static void buddy_hide(void)
{
    if (!bd.shown || bd.leaving) return;
    bd.leaving = 1;
    if (bd.state == B_ENTER || bd.state == B_PAUSE) bd_dart_to(bd_corner_x(), bd_shown_y());
}

/* The hub said where to look, and that still holds */
static int bd_hub_gaze(DWORD now)
{
    if (bd.gmode != RKM_GAZE_FREE && bd.gaze_until && (long)(now - bd.gaze_until) >= 0) bd.gmode = RKM_GAZE_FREE;
    return bd.gmode;
}

/* Call on every BUDDY_TIMER tick */
static void buddy_tick(void)
{
    static const double looks[4][3] = { { 0, 0, 0.3 }, { -0.9, -0.2, 0.5 }, { 0.9, -0.2, 0.5 }, { 0, -0.9, 0.4 } };
    DWORD now = GetTickCount();
    double f, k;

    if (!bd.shown) return;
    switch (bd.state) {
    case B_ENTER:
        f = (now - bd.t0) / 500.0;
        if (f > 1) f = 1;
        f = 1 - (1 - f) * (1 - f) * (1 - f);
        bd_move(bd_corner_x(), bd_hidden_y() + (bd_shown_y() - bd_hidden_y()) * f);
        bd.gx += (looks[bd.look_i][0] - bd.gx) * 0.35;
        bd.gy += (looks[bd.look_i][1] - bd.gy) * 0.35;
        if (now - bd.look_t > looks[bd.look_i][2] * 1000) {
            bd.look_t = now;
            if (++bd.look_i >= 4) { bd.state = B_PAUSE; bd.pause_until = now; }
        }
        break;
    case B_PAUSE:
        switch (bd_hub_gaze(now)) {
        case RKM_GAZE_POINTER:                 /* told to watch: stay and watch (bd_paint) */
            if (!bd.leaving && (long)(bd.pause_until - now) < 500) bd.pause_until = now + 500;
            break;
        case RKM_GAZE_AT:
            bd.gx += (bd.hgx - bd.gx) * 0.3;
            bd.gy += (bd.hgy - bd.gy) * 0.3;
            if (!bd.leaving && (long)(bd.pause_until - now) < 500) bd.pause_until = now + 500;
            break;
        default:
            if (!bd.follow) {
                if (rand() % 25 == 0) { bd.stare_x = bd_rand(-1, 1); bd.stare_y = bd_rand(-1, 1); }
                bd.gx += (bd.stare_x - bd.gx) * 0.3;
                bd.gy += (bd.stare_y - bd.gy) * 0.3;
            }
        }
        if ((long)(now - bd.pause_until) >= 0) {
            if (bd.leaving) { bd.state = B_EXIT; bd.state_t = now; break; }
            bd_dart_to(bd.sx + bd_rand(20, bd.sw - bd.ww - 20 > 20 ? bd.sw - bd.ww - 20 : 20),
                       bd.sy + bd_rand(20, bd.sh - bd.wh - 40 > 20 ? bd.sh - bd.wh - 40 : 20));
        }
        break;
    case B_DART: {
        double dx = bd.to_x - bd.from_x, dy = bd.to_y - bd.from_y, n;
        f = (now - bd.seg_t) / (bd.seg_dur * 1000);
        if (f > 1) f = 1;
        k = f * f * (3 - 2 * f);
        bd_move(bd.from_x + dx * k, bd.from_y + dy * k);
        n = sqrt(dx * dx + dy * dy);
        if (n < 1) n = 1;
        bd.gx += (dx / n - bd.gx) * 0.5;
        bd.gy += (dy / n - bd.gy) * 0.5;
        if (f >= 1) {
            if (bd.leaving && (fabs(bd.x - bd_corner_x()) > 2 || fabs(bd.y - bd_shown_y()) > 2)) {
                bd_dart_to(bd_corner_x(), bd_shown_y());
                break;
            }
            bd.state = B_PAUSE;
            bd.follow = rand() % 100 < 20;
            bd.pause_until = now + (bd.leaving ? 300 : (DWORD)bd_rand(2000, 7000));
        }
        break;
    }
    case B_EXIT:
        f = (now - bd.state_t) / 450.0;
        if (f > 1) f = 1;
        bd_move(bd_corner_x(), bd_shown_y() + (bd_hidden_y() - bd_shown_y()) * f * f);
        if (bd_hub_gaze(now) == RKM_GAZE_AT) { /* looking where it is going */
            bd.gx = bd.hgx;
            bd.gy = bd.hgy;
        } else {
            bd.gx = 0;
            bd.gy = 0.9;
        }
        if (f >= 1) {
            KillTimer(bd.w, BUDDY_TIMER);
            ShowWindow(bd.w, SW_HIDE);
            bd.shown = 0;
            bd.state = B_OFF;
            bd.gmode = RKM_GAZE_FREE;
            bd.say_until = 0;
            return;
        }
        break;
    default:
        break;
    }
    bd_redraw();
}

/*
 * lil' C's window for the Windows agent (the "desk buddy", RKM_BUDDY).
 *
 * lil' C is a desk robot (see the lil' C project) who sometimes wanders over
 * to a machine on the desk.  When the hub says RKM_BUDDY show, a small xeyes
 * window slides up from the bottom right corner (above the taskbar) and darts
 * around the screen, stopping to look about or at the pointer; on hide it
 * darts back and slides away.  Clicking him sends RKM_BUDDY_EVENT click.
 *
 * Plain GDI, black and white on the X root weave like the X11 version.
 * Never takes the focus.  C89, included by rkm_win32.c.
 */
#include <math.h>

#define BUDDY_TIMER 2

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

static void bd_ellipse(HDC dc, double cx, double cy, double rx, double ry, int white)
{
    SelectObject(dc, GetStockObject(white ? WHITE_BRUSH : BLACK_BRUSH));
    Ellipse(dc, (int)(cx - rx), (int)(cy - ry), (int)(cx + rx), (int)(cy + ry));
}

static void bd_paint(HDC out)
{
    HDC dc = CreateCompatibleDC(out);
    HBITMAP bm = CreateCompatibleBitmap(out, bd.ww, bd.wh), oldbm;
    RECT r;
    POINT pt;
    double rx = bd.ww * 54.0 / 260, ry = bd.face_h * 66.0 / 148, rim = rx * 0.16;
    double prx = rx * 0.24, pry = prx * 1.2;
    int i;

    oldbm = (HBITMAP)SelectObject(dc, bm);
    SelectObject(dc, GetStockObject(NULL_PEN));
    r.left = 0; r.top = 0; r.right = bd.ww; r.bottom = bd.face_h;
    FillRect(dc, &r, bd.weave);
    GetCursorPos(&pt);
    for (i = 0; i < 2; i++) {
        double cx = bd.ww * (i ? 190.0 : 70.0) / 260, cy = bd.face_h / 2.0;
        double ax = rx - rim - prx - 3, ay = ry - rim - pry - 3, dx, dy, k;
        bd_ellipse(dc, cx, cy, rx, ry, 0);
        bd_ellipse(dc, cx, cy, rx - rim, ry - rim, 1);
        if (bd.follow && bd.state == B_PAUSE) {
            dx = pt.x - (bd.x + cx);
            dy = pt.y - (bd.y + cy);
        } else {
            dx = bd.gx * ax;
            dy = bd.gy * ay;
        }
        k = sqrt((dx / ax) * (dx / ax) + (dy / ay) * (dy / ay));
        if (k > 1) { dx /= k; dy /= k; }
        bd_ellipse(dc, cx + dx, cy + dy, prx, pry, 0);
    }
    r.top = bd.face_h; r.bottom = bd.wh;
    FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SetBkColor(dc, RGB(0, 0, 0));
    SetTextColor(dc, RGB(255, 255, 255));
    SelectObject(dc, GetStockObject(ANSI_FIXED_FONT));
    TextOut(dc, 8, bd.face_h + (bd.wh - bd.face_h - 12) / 2, bd.label, lstrlen(bd.label));
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
    for (i = 0; i < 8; i++) {                  /* rows of 16 bits, leftmost pixel in the top bit; 1 = white */
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
    wc.lpszClassName = "lilC";
    RegisterClass(&wc);
    bd.ww = 260;
    if (bd.ww > GetSystemMetrics(SM_CXSCREEN) / 4) bd.ww = GetSystemMetrics(SM_CXSCREEN) / 4;
    if (bd.ww < 120) bd.ww = 120;
    bd.wh = bd.ww * 176 / 260;
    bd.face_h = bd.wh * 84 / 100;
    bd.w = CreateWindowEx(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, "lilC", "lil' C", WS_POPUP,
                          0, 0, bd.ww, bd.wh, NULL, NULL, inst, NULL);
    srand((unsigned)GetTickCount());
}

static void buddy_show(const char *label, unsigned int n)
{
    RECT wa;
    if (!bd.w) return;
    if (n > sizeof bd.label - 1) n = sizeof bd.label - 1;
    memcpy(bd.label, label, n);
    bd.label[n] = 0;
    if (!n) lstrcpy(bd.label, "lil' C");
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
        if (!bd.follow) {
            if (rand() % 25 == 0) { bd.stare_x = bd_rand(-1, 1); bd.stare_y = bd_rand(-1, 1); }
            bd.gx += (bd.stare_x - bd.gx) * 0.3;
            bd.gy += (bd.stare_y - bd.gy) * 0.3;
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
        bd.gx = 0;
        bd.gy = 0.9;
        if (f >= 1) {
            KillTimer(bd.w, BUDDY_TIMER);
            ShowWindow(bd.w, SW_HIDE);
            bd.shown = 0;
            bd.state = B_OFF;
            return;
        }
        break;
    default:
        break;
    }
    bd_redraw();
}

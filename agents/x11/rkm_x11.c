/*
 * RetroKM agent for X11: SGI IRIX, and any other Unix running an X server.
 *
 * Input is injected with the XTEST extension; the clipboard is the X
 * selection mechanism.  Strict C89 so MIPSpro cc builds it as-is.
 *
 *   IRIX:   cc -o rkm-x11 rkm_x11.c ../../common/rkm_proto.c -lXtst -lXext -lX11
 *   Linux:  cc -o rkm-x11 rkm_x11.c ../../common/rkm_proto.c -lXtst -lX11
 *
 *   rkm-x11 [-n name] [-p port] [-s clipboard|primary|both] [-l|-u] [-c] hub-host
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#ifdef __sgi
#include <bstring.h>
#endif
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include "../../common/rkm_proto.h"

#define MAX_CLIP_OUT (4UL * 1024 * 1024)
#define HUB_SILENCE  30           /* seconds without traffic before reconnecting */

static const char *hub_host;
static int hub_port = RKM_PORT;
static char my_name[RKM_NAME_MAX + 1];
static int clip_only;
#ifdef __sgi
static int latin1 = 1;            /* IRIX apps speak ISO 8859-1, not UTF-8 */
#else
static int latin1 = 0;
#endif
static int use_clipboard = 1, use_primary = 0;

static int sock = -1;
static time_t last_rx;
static rkm_parser parser;
static volatile int stop;

static Display *dpy;
static Window win;
static Atom a_clipboard, a_targets, a_utf8, a_text, a_incr, a_prop;

static unsigned char keycode_for[256];       /* HID usage -> X keycode */
static unsigned char key_down[256];          /* by X keycode */
static unsigned int btn_down;
static int wheel_y, wheel_x;

static unsigned char *clip_data;             /* what we serve when we own a selection */
static unsigned long clip_len;
static unsigned char *clip_in;               /* being received from the hub */
static unsigned long clip_in_len, clip_in_total;
static unsigned char *last_text[2];          /* last content seen, per selection */
static unsigned long last_len[2];

/* ---- network ------------------------------------------------------------ */

static int send_all(const unsigned char *p, unsigned long n)
{
    while (n > 0) {
        int k = (int)write(sock, p, n);
        if (k <= 0) {
            if (k < 0 && errno == EINTR) continue;
            return -1;
        }
        p += k;
        n -= (unsigned long)k;
    }
    return 0;
}

static void send_frame(int type, const unsigned char *payload, unsigned int len)
{
    unsigned char buf[RKM_HDR + RKM_MAX_PAYLOAD];
    if (sock < 0) return;
    if (send_all(buf, rkm_pack(buf, type, payload, len)) < 0) {
        close(sock);
        sock = -1;
    }
}

static int hub_connect(void)
{
    struct sockaddr_in sa;
    struct hostent *he;
    int one = 1;

    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)hub_port);
    sa.sin_addr.s_addr = inet_addr(hub_host);
    if ((unsigned long)sa.sin_addr.s_addr == 0xFFFFFFFFUL) {
        he = gethostbyname(hub_host);
        if (!he) return -1;
        memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof sa.sin_addr);
    }
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    if (connect(sock, (struct sockaddr *)&sa, sizeof sa) < 0) {
        close(sock);
        sock = -1;
        return -1;
    }
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);
    return 0;
}

/* ---- keyboard mapping --------------------------------------------------- */

static KeySym keysym_for(int u)
{
    if (u >= HID_A && u <= HID_Z) return XK_a + (u - HID_A);
    if (u >= HID_1 && u < HID_0) return XK_1 + (u - HID_1);
    if (u >= HID_F1 && u <= HID_F12) return XK_F1 + (u - HID_F1);
    if (u >= HID_F13 && u <= HID_F24) return XK_F13 + (u - HID_F13);
    if (u >= HID_KP_1 && u <= HID_KP_9) return XK_KP_1 + (u - HID_KP_1);
    switch (u) {
    case HID_0: return XK_0;
    case HID_ENTER: return XK_Return;
    case HID_ESC: return XK_Escape;
    case HID_BACKSPACE: return XK_BackSpace;
    case HID_TAB: return XK_Tab;
    case HID_SPACE: return XK_space;
    case HID_MINUS: return XK_minus;
    case HID_EQUAL: return XK_equal;
    case HID_LBRACKET: return XK_bracketleft;
    case HID_RBRACKET: return XK_bracketright;
    case HID_BACKSLASH: return XK_backslash;
    case HID_SEMICOLON: return XK_semicolon;
    case HID_APOSTROPHE: return XK_apostrophe;
    case HID_GRAVE: return XK_grave;
    case HID_COMMA: return XK_comma;
    case HID_DOT: return XK_period;
    case HID_SLASH: return XK_slash;
    case HID_CAPSLOCK: return XK_Caps_Lock;
    case HID_PRINTSCREEN: return XK_Print;
    case HID_SCROLLLOCK: return XK_Scroll_Lock;
    case HID_PAUSE: return XK_Pause;
    case HID_INSERT: return XK_Insert;
    case HID_HOME: return XK_Home;
    case HID_PAGEUP: return XK_Prior;
    case HID_DELETE: return XK_Delete;
    case HID_END: return XK_End;
    case HID_PAGEDOWN: return XK_Next;
    case HID_RIGHT: return XK_Right;
    case HID_LEFT: return XK_Left;
    case HID_DOWN: return XK_Down;
    case HID_UP: return XK_Up;
    case HID_NUMLOCK: return XK_Num_Lock;
    case HID_KP_SLASH: return XK_KP_Divide;
    case HID_KP_STAR: return XK_KP_Multiply;
    case HID_KP_MINUS: return XK_KP_Subtract;
    case HID_KP_PLUS: return XK_KP_Add;
    case HID_KP_ENTER: return XK_KP_Enter;
    case HID_KP_0: return XK_KP_0;
    case HID_KP_DOT: return XK_KP_Decimal;
    case HID_KP_EQUAL: return XK_KP_Equal;
    case HID_NONUS_BACKSLASH: return XK_less;
    case HID_MENU: return XK_Menu;
    case HID_LCTRL: return XK_Control_L;
    case HID_LSHIFT: return XK_Shift_L;
    case HID_LALT: return XK_Alt_L;
    case HID_LGUI: return XK_Super_L;
    case HID_RCTRL: return XK_Control_R;
    case HID_RSHIFT: return XK_Shift_R;
    case HID_RALT: return XK_Alt_R;
    case HID_RGUI: return XK_Super_R;
    }
    return NoSymbol;
}

/* Prefer the key whose unshifted symbol matches, so "less" never resolves
 * to shift+comma; fall back to any level (keypad digits live on level 1). */
static void build_keymap(void)
{
    int min, max, per, u, kc, lvl;
    KeySym *map;

    XDisplayKeycodes(dpy, &min, &max);
    map = XGetKeyboardMapping(dpy, (KeyCode)min, max - min + 1, &per);
    memset(keycode_for, 0, sizeof keycode_for);
    if (!map) return;
    for (u = 0; u < 256; u++) {
        KeySym want = keysym_for(u), alt = NoSymbol;
        if (want == NoSymbol) continue;
        if (u == HID_LGUI) alt = XK_Meta_L;
        if (u == HID_RGUI) alt = XK_Meta_R;
        if (u == HID_RALT) alt = XK_Mode_switch;
        if (u == HID_LALT) alt = XK_Meta_L;
        for (lvl = 0; lvl < per && !keycode_for[u]; lvl++)
            for (kc = min; kc <= max; kc++)
                if (map[(kc - min) * per + lvl] == want) {
                    keycode_for[u] = (unsigned char)kc;
                    break;
                }
        for (kc = min; alt != NoSymbol && kc <= max && !keycode_for[u]; kc++)
            if (map[(kc - min) * per] == alt) keycode_for[u] = (unsigned char)kc;
    }
    XFree(map);
}

static void fake_key(int usage, int down)
{
    int kc = keycode_for[usage & 0xFF];
    if (!kc) return;
    if (!down && !key_down[kc]) return;
    key_down[kc] = (unsigned char)down;
    XTestFakeKeyEvent(dpy, (unsigned int)kc, down ? True : False, CurrentTime);
}

static void fake_button(unsigned int xbtn, int down)
{
    if (down) btn_down |= 1U << xbtn;
    else btn_down &= ~(1U << xbtn);
    XTestFakeButtonEvent(dpy, xbtn, down ? True : False, CurrentTime);
}

static void release_all(void)
{
    int i;
    for (i = 0; i < 256; i++)
        if (key_down[i]) {
            key_down[i] = 0;
            XTestFakeKeyEvent(dpy, (unsigned int)i, False, CurrentTime);
        }
    for (i = 1; i < 16; i++)
        if (btn_down & (1U << i)) XTestFakeButtonEvent(dpy, (unsigned int)i, False, CurrentTime);
    btn_down = 0;
    XFlush(dpy);
}

/* ---- text helpers ------------------------------------------------------- */

static unsigned char *latin1_to_utf8(const unsigned char *in, unsigned long n, unsigned long *outn)
{
    unsigned char *out = (unsigned char *)malloc(n * 2 + 1);
    unsigned long i, o = 0;
    if (!out) return NULL;
    for (i = 0; i < n; i++) {
        if (in[i] < 0x80) out[o++] = in[i];
        else {
            out[o++] = (unsigned char)(0xC0 | (in[i] >> 6));
            out[o++] = (unsigned char)(0x80 | (in[i] & 0x3F));
        }
    }
    *outn = o;
    return out;
}

static unsigned char *utf8_to_latin1(const unsigned char *in, unsigned long n, unsigned long *outn)
{
    unsigned char *out = (unsigned char *)malloc(n + 1);
    unsigned long i = 0, o = 0;
    if (!out) return NULL;
    while (i < n) {
        unsigned char c = in[i];
        if (c < 0x80) { out[o++] = c; i++; }
        else if ((c & 0xFC) == 0xC0 && i + 1 < n) {     /* U+0080..U+00FF */
            out[o++] = (unsigned char)(((c & 0x03) << 6) | (in[i + 1] & 0x3F));
            i += 2;
        } else {
            out[o++] = '?';
            i++;
            while (i < n && (in[i] & 0xC0) == 0x80) i++;
        }
    }
    *outn = o;
    return out;
}

static void remember(int idx, const unsigned char *data, unsigned long n)
{
    if (last_text[idx]) free(last_text[idx]);
    last_text[idx] = (unsigned char *)malloc(n + 1);
    last_len[idx] = 0;
    if (last_text[idx]) {
        memcpy(last_text[idx], data, n);
        last_len[idx] = n;
    }
}

/* ---- selections --------------------------------------------------------- */

static void serve_request(XSelectionRequestEvent *rq)
{
    XEvent ev;
    Atom prop = rq->property == None ? rq->target : rq->property;

    memset(&ev, 0, sizeof ev);
    ev.xselection.type = SelectionNotify;
    ev.xselection.display = rq->display;
    ev.xselection.requestor = rq->requestor;
    ev.xselection.selection = rq->selection;
    ev.xselection.target = rq->target;
    ev.xselection.time = rq->time;
    ev.xselection.property = None;

    if (clip_data) {
        if (rq->target == a_targets) {
            Atom list[4];
            int n = 0;
            list[n++] = a_targets;
            if (!latin1) list[n++] = a_utf8;
            list[n++] = XA_STRING;
            list[n++] = a_text;
            XChangeProperty(dpy, rq->requestor, prop, XA_ATOM, 32, PropModeReplace,
                            (unsigned char *)list, n);
            ev.xselection.property = prop;
        } else if (rq->target == a_utf8 && !latin1) {
            XChangeProperty(dpy, rq->requestor, prop, a_utf8, 8, PropModeReplace,
                            clip_data, (int)clip_len);
            ev.xselection.property = prop;
        } else if (rq->target == XA_STRING || rq->target == a_text) {
            if (latin1) {
                XChangeProperty(dpy, rq->requestor, prop, XA_STRING, 8, PropModeReplace,
                                clip_data, (int)clip_len);
            } else {
                unsigned long n = 0;
                unsigned char *l1 = utf8_to_latin1(clip_data, clip_len, &n);
                if (l1) {
                    XChangeProperty(dpy, rq->requestor, prop, XA_STRING, 8, PropModeReplace,
                                    l1, (int)n);
                    free(l1);
                }
            }
            ev.xselection.property = prop;
        }
    }
    XSendEvent(dpy, rq->requestor, False, 0L, &ev);
}

static void handle_xevent(XEvent *ev)
{
    switch (ev->type) {
    case SelectionRequest:
        serve_request(&ev->xselectionrequest);
        break;
    case MappingNotify:
        XRefreshKeyboardMapping(&ev->xmapping);
        build_keymap();
        break;
    default:
        break;
    }
}

/* Wait for one event of a given type on our window. */
static int wait_event(int type, XEvent *ev, int ms)
{
    int waited = 0;
    for (;;) {
        fd_set rf;
        struct timeval tv;
        if (XCheckTypedWindowEvent(dpy, win, type, ev)) return 1;
        if (waited >= ms) return 0;
        FD_ZERO(&rf);
        FD_SET(ConnectionNumber(dpy), &rf);
        tv.tv_sec = 0;
        tv.tv_usec = 20000;
        select(ConnectionNumber(dpy) + 1, &rf, NULL, NULL, &tv);
        waited += 20;
    }
}

static int append(unsigned char **buf, unsigned long *len, const unsigned char *d, unsigned long n)
{
    unsigned char *p;
    if (*len + n > MAX_CLIP_OUT) return -1;
    p = (unsigned char *)realloc(*buf, *len + n + 1);
    if (!p) return -1;
    memcpy(p + *len, d, n);
    *buf = p;
    *len += n;
    return 0;
}

/* Fetch a selection owned by another client.  Returns malloc'd bytes. */
static unsigned char *read_selection(Atom sel, Atom target, unsigned long *outlen)
{
    XEvent ev;
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL, *out = NULL;

    *outlen = 0;
    XDeleteProperty(dpy, win, a_prop);
    while (XCheckTypedWindowEvent(dpy, win, SelectionNotify, &ev)) { /* stale */ }
    XConvertSelection(dpy, sel, target, a_prop, win, CurrentTime);
    if (!wait_event(SelectionNotify, &ev, 1000)) return NULL;
    if (ev.xselection.property == None) return NULL;

    if (XGetWindowProperty(dpy, win, a_prop, 0L, 0x400000L, True, AnyPropertyType,
                           &type, &fmt, &n, &after, &data) != Success)
        return NULL;
    if (type == a_incr) {                       /* large transfer, in pieces */
        if (data) XFree(data);
        for (;;) {
            do {
                if (!wait_event(PropertyNotify, &ev, 2000)) { if (out) free(out); return NULL; }
            } while (ev.xproperty.atom != a_prop || ev.xproperty.state != PropertyNewValue);
            data = NULL;
            if (XGetWindowProperty(dpy, win, a_prop, 0L, 0x400000L, True, AnyPropertyType,
                                   &type, &fmt, &n, &after, &data) != Success)
                break;
            if (n == 0) { if (data) XFree(data); break; }
            if (fmt == 8) append(&out, outlen, data, n);
            XFree(data);
        }
    } else {
        if (fmt == 8 && n > 0) append(&out, outlen, data, n);
        if (data) XFree(data);
    }
    if (out) out[*outlen] = 0;
    return out;
}

static void send_clipboard(const unsigned char *data, unsigned long n)
{
    unsigned char hdr[5];
    unsigned long off;

    hdr[0] = RKM_CLIP_TEXT;
    RKM_PUT32(hdr + 1, n);
    send_frame(RKM_CLIP_BEGIN, hdr, 5);
    for (off = 0; off < n; off += RKM_CLIP_CHUNK)
        send_frame(RKM_CLIP_DATA, data + off,
                   (unsigned int)(n - off < RKM_CLIP_CHUNK ? n - off : RKM_CLIP_CHUNK));
    send_frame(RKM_CLIP_END, NULL, 0);
}

/* Called when the pointer leaves this machine: if another client took a
 * selection and its content is new, report it to the hub.  With report=0
 * the current content is only remembered (startup). */
static void check_selections(int report)
{
    Atom sels[2];
    int idx;

    sels[0] = a_clipboard;
    sels[1] = XA_PRIMARY;
    for (idx = 0; idx < 2; idx++) {
        Window owner;
        unsigned char *data;
        unsigned long n = 0;

        if ((idx == 0 && !use_clipboard) || (idx == 1 && !use_primary)) continue;
        owner = XGetSelectionOwner(dpy, sels[idx]);
        if (owner == None || owner == win) continue;

        if (latin1) {
            data = read_selection(sels[idx], XA_STRING, &n);
        } else {
            data = read_selection(sels[idx], a_utf8, &n);
            if (!data) {                        /* old client: Latin-1 only */
                unsigned long ln = 0;
                unsigned char *l1 = read_selection(sels[idx], XA_STRING, &ln);
                if (l1) {
                    data = latin1_to_utf8(l1, ln, &n);
                    free(l1);
                }
            }
        }
        if (!data) continue;
        if (n > 0 && !(last_text[idx] && last_len[idx] == n && !memcmp(last_text[idx], data, n))) {
            remember(idx, data, n);
            if (report) {
                send_clipboard(data, n);
                free(data);
                return;
            }
        }
        free(data);
    }
}

static void take_selections(void)
{
    if (use_clipboard) XSetSelectionOwner(dpy, a_clipboard, win, CurrentTime);
    if (use_primary) XSetSelectionOwner(dpy, XA_PRIMARY, win, CurrentTime);
}

/* ---- frames from the hub ------------------------------------------------ */

static void press_mods(unsigned int m, int down)
{
    int b;
    for (b = 0; b < 8; b++)
        if (m & (1U << b)) fake_key(HID_LCTRL + b, down);
}

static void on_frame(void *ctx, int type, const unsigned char *p, unsigned int len)
{
    static const unsigned int xbtn[] = { 0, 1, 3, 2, 8, 9 };
    (void)ctx;

    if (clip_only && type >= RKM_ENTER && type <= RKM_RESET && type != RKM_LEAVE) return;

    switch (type) {
    case RKM_WELCOME:
        if (len >= 2 && p[1] != RKM_OK) {
            fprintf(stderr, "rkm-x11: hub refused us (%s)\n",
                    p[1] == RKM_ERR_NAME ? "screen name not in hub config" : "version mismatch");
            exit(1);
        }
        break;
    case RKM_PING:
        send_frame(RKM_PONG, NULL, 0);
        break;
    case RKM_ENTER:
        if (len < 5) break;
        XTestFakeMotionEvent(dpy, DefaultScreen(dpy), (int)RKM_GET16(p), (int)RKM_GET16(p + 2), CurrentTime);
        press_mods(p[4], 1);
        break;
    case RKM_LEAVE:
    case RKM_RESET:
        if (!clip_only) release_all();
        if (type == RKM_LEAVE) check_selections(1);
        break;
    case RKM_MOVE:
        if (len < 4) break;
        XTestFakeMotionEvent(dpy, DefaultScreen(dpy), (int)RKM_GET16(p), (int)RKM_GET16(p + 2), CurrentTime);
        break;
    case RKM_MOVEREL:
        if (len < 4) break;
        XTestFakeRelativeMotionEvent(dpy, RKM_GETS16(p), RKM_GETS16(p + 2), CurrentTime);
        break;
    case RKM_BUTTON:
        if (len < 2 || p[0] < 1 || p[0] > 5) break;
        fake_button(xbtn[p[0]], p[1]);
        break;
    case RKM_WHEEL:
        if (len < 4) break;
        wheel_y += RKM_GETS16(p);
        wheel_x += RKM_GETS16(p + 2);
        while (wheel_y >= 120) { fake_button(4, 1); fake_button(4, 0); wheel_y -= 120; }
        while (wheel_y <= -120) { fake_button(5, 1); fake_button(5, 0); wheel_y += 120; }
        while (wheel_x >= 120) { fake_button(7, 1); fake_button(7, 0); wheel_x -= 120; }
        while (wheel_x <= -120) { fake_button(6, 1); fake_button(6, 0); wheel_x += 120; }
        break;
    case RKM_KEY:
        if (len < 2 || p[1] == RKM_KEY_REPEAT) break;   /* the X server repeats by itself */
        fake_key(p[0], p[1]);
        break;
    case RKM_CLIP_BEGIN:
        if (clip_in) free(clip_in);
        clip_in = NULL;
        if (len >= 5 && p[0] == RKM_CLIP_TEXT) {
            clip_in_total = RKM_GET32(p + 1);
            clip_in_len = 0;
            clip_in = (unsigned char *)malloc(clip_in_total + 1);
        }
        break;
    case RKM_CLIP_DATA:
        if (clip_in && clip_in_len + len <= clip_in_total) {
            memcpy(clip_in + clip_in_len, p, len);
            clip_in_len += len;
        }
        break;
    case RKM_CLIP_END:
        if (!clip_in) break;
        if (clip_data) free(clip_data);
        clip_data = clip_in;
        clip_len = clip_in_len;
        clip_in = NULL;
        remember(0, clip_data, clip_len);
        remember(1, clip_data, clip_len);
        take_selections();
        break;
    default:
        break;
    }
}

/* ---- main --------------------------------------------------------------- */

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

static void nap(int ms)
{
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000L;
    select(0, NULL, NULL, NULL, &tv);
}

/* A requestor window can vanish mid-transfer; that must not kill us. */
static int on_xerror(Display *d, XErrorEvent *e)
{
    (void)d;
    (void)e;
    return 0;
}

static void pump_x(void)
{
    XEvent ev;
    while (XPending(dpy)) {
        XNextEvent(dpy, &ev);
        handle_xevent(&ev);
    }
}

static void usage(void)
{
    fprintf(stderr,
        "usage: rkm-x11 [options] hub-host\n"
        "  -n name   screen name in the hub config (default: hostname)\n"
        "  -p port   hub port (default %d)\n"
        "  -s which  selection to share: clipboard, primary or both (default clipboard)\n"
        "  -l        clipboard text is ISO 8859-1 (default on IRIX)\n"
        "  -u        clipboard text is UTF-8 (default elsewhere)\n"
        "  -c        clipboard only, no mouse or keyboard\n", RKM_PORT);
    exit(2);
}

int main(int argc, char **argv)
{
    int i, evb, erb, maj, min;
    unsigned int maxkb;
    char *dot;

    gethostname(my_name, sizeof my_name - 1);
    dot = strchr(my_name, '.');
    if (dot) *dot = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            strncpy(my_name, argv[++i], RKM_NAME_MAX);
            my_name[RKM_NAME_MAX] = 0;
        } else if (!strcmp(argv[i], "-p") && i + 1 < argc) hub_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
            const char *v = argv[++i];
            use_clipboard = strcmp(v, "primary") != 0;
            use_primary = strcmp(v, "clipboard") != 0;
        } else if (!strcmp(argv[i], "-l")) latin1 = 1;
        else if (!strcmp(argv[i], "-u")) latin1 = 0;
        else if (!strcmp(argv[i], "-c")) clip_only = 1;
        else if (argv[i][0] == '-') usage();
        else hub_host = argv[i];
    }
    if (!hub_host) usage();

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "rkm-x11: cannot open display\n");
        return 1;
    }
    if (!clip_only && !XTestQueryExtension(dpy, &evb, &erb, &maj, &min)) {
        fprintf(stderr, "rkm-x11: this X server has no XTEST extension; use -c for clipboard only\n");
        return 1;
    }
    XSetErrorHandler(on_xerror);
    win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, 1, 1, 0, 0, 0);
    XSelectInput(dpy, win, PropertyChangeMask);
    a_clipboard = XInternAtom(dpy, "CLIPBOARD", False);
    a_targets = XInternAtom(dpy, "TARGETS", False);
    a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    a_text = XInternAtom(dpy, "TEXT", False);
    a_incr = XInternAtom(dpy, "INCR", False);
    a_prop = XInternAtom(dpy, "RKM_SELECTION", False);
    build_keymap();
    check_selections(0);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* a property must fit in one X request; leave room for the header */
    maxkb = (unsigned int)(XMaxRequestSize(dpy) / 256) - 1;

    while (!stop) {
        unsigned char hello[10 + RKM_NAME_MAX];
        unsigned int hl;

        if (hub_connect() < 0) {
            for (i = 0; i < 30 && !stop; i++) {  /* keep serving selections meanwhile */
                pump_x();
                nap(100);
            }
            continue;
        }
        fprintf(stderr, "rkm-x11: connected to %s as \"%s\"\n", hub_host, my_name);
        rkm_parser_init(&parser);
        hl = rkm_hello(hello, clip_only ? RKM_CAP_CLIP : RKM_CAP_INPUT | RKM_CAP_CLIP,
                       latin1 ? RKM_CS_LATIN1 : RKM_CS_UTF8, RKM_EOL_LF,
                       (unsigned int)DisplayWidth(dpy, DefaultScreen(dpy)),
                       (unsigned int)DisplayHeight(dpy, DefaultScreen(dpy)), maxkb, my_name);
        send_frame(RKM_HELLO, hello, hl);
        last_rx = time(NULL);

        while (!stop && sock >= 0) {
            fd_set rf;
            struct timeval tv;
            int xfd = ConnectionNumber(dpy), mx;

            pump_x();
            XFlush(dpy);
            FD_ZERO(&rf);
            FD_SET(sock, &rf);
            FD_SET(xfd, &rf);
            mx = sock > xfd ? sock : xfd;
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            if (select(mx + 1, &rf, NULL, NULL, &tv) < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (FD_ISSET(sock, &rf)) {
                unsigned char buf[2048];
                int n = (int)read(sock, buf, sizeof buf);
                if (n <= 0) break;
                last_rx = time(NULL);
                if (rkm_feed(&parser, buf, (unsigned long)n, on_frame, NULL) < 0) break;
            }
            if (time(NULL) - last_rx > HUB_SILENCE) break;
        }
        if (!clip_only) release_all();
        if (sock >= 0) close(sock);
        sock = -1;
        if (!stop) fprintf(stderr, "rkm-x11: lost the hub, reconnecting\n");
    }
    return 0;
}

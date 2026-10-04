/*
 * RetroKM agent for Windows 98 SE and Windows 2000 (works on later versions).
 *
 * Plain Win32 + Winsock 1.1, ANSI APIs only, single thread, C89.
 *
 *   Visual C++ 6:   cl /O1 rkm_win32.c ..\..\common\rkm_proto.c user32.lib shell32.lib wsock32.lib
 *   Open Watcom:    wcl386 -l=nt_win -bt=nt rkm_win32.c ..\..\common\rkm_proto.c
 *   MinGW:          gcc -Os -mwindows -o rkm-win32.exe rkm_win32.c ../../common/rkm_proto.c -lwsock32
 *
 *   rkm-win32.exe hub-host [name [port]]
 * or put an rkm.ini next to the exe:
 *   [retrokm]
 *   hub=192.168.1.10
 *   name=pc
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock.h>
#include <shellapi.h>
#include <stdlib.h>
#include <string.h>
#include "../../common/rkm_proto.h"

#ifndef MOUSEEVENTF_WHEEL
#define MOUSEEVENTF_WHEEL 0x0800
#endif
#ifndef MOUSEEVENTF_XDOWN
#define MOUSEEVENTF_XDOWN 0x0080
#define MOUSEEVENTF_XUP   0x0100
#endif
#ifndef VK_OEM_102
#define VK_OEM_102 0xE2
#endif

#define WM_SOCKET   (WM_USER + 1)
#define WM_TRAY     (WM_USER + 2)
#define IDM_QUIT    100
#define RETRY_MS    3000
#define SILENCE_MS  30000
#define MAX_CLIP    (1024UL * 1024UL)

enum { ST_IDLE, ST_CONNECTING, ST_UP };

static HINSTANCE inst;
static HWND hwnd, next_viewer;
static SOCKET sock = INVALID_SOCKET;
static int state = ST_IDLE;
static DWORD last_rx, retry_at, connect_at;
static rkm_parser parser;
static char hub_host[128], my_name[RKM_NAME_MAX + 1];
static int hub_port = RKM_PORT;

static unsigned char key_down[256];       /* by HID usage */
static int btn_down;
static int clip_dirty, clip_ready;
static HGLOBAL clip_in;
static unsigned long clip_in_len, clip_in_total;
static NOTIFYICONDATA nid;

/* ---- tray ---------------------------------------------------------------- */

static void tray_tip(const char *text)
{
    lstrcpyn(nid.szTip, text, sizeof nid.szTip);
    Shell_NotifyIcon(NIM_MODIFY, &nid);
}

/* ---- network ------------------------------------------------------------- */

static void release_all(void);

static void drop(void)
{
    if (sock != INVALID_SOCKET) closesocket(sock);
    sock = INVALID_SOCKET;
    if (state == ST_UP) release_all();
    state = ST_IDLE;
    retry_at = GetTickCount() + RETRY_MS;
    tray_tip("RetroKM: waiting for hub");
}

static int send_all(const unsigned char *p, unsigned int n)
{
    int tries = 0;
    while (n > 0 && sock != INVALID_SOCKET) {
        int k = send(sock, (const char *)p, (int)n, 0);
        if (k == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK && ++tries < 1000) {
                Sleep(5);
                continue;
            }
            drop();
            return -1;
        }
        p += k;
        n -= (unsigned int)k;
    }
    return 0;
}

static void send_frame(int type, const unsigned char *payload, unsigned int len)
{
    unsigned char buf[RKM_HDR + RKM_MAX_PAYLOAD];
    if (state != ST_UP) return;
    send_all(buf, rkm_pack(buf, type, payload, len));
}

static void start_connect(void)
{
    struct sockaddr_in sa;
    struct hostent *he;

    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)hub_port);
    sa.sin_addr.s_addr = inet_addr(hub_host);
    if (sa.sin_addr.s_addr == INADDR_NONE) {
        he = gethostbyname(hub_host);
        if (!he) { retry_at = GetTickCount() + RETRY_MS; return; }
        memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof sa.sin_addr);
    }
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) { retry_at = GetTickCount() + RETRY_MS; return; }
    WSAAsyncSelect(sock, hwnd, WM_SOCKET, FD_CONNECT | FD_READ | FD_CLOSE);
    if (connect(sock, (struct sockaddr *)&sa, sizeof sa) == SOCKET_ERROR &&
        WSAGetLastError() != WSAEWOULDBLOCK) {
        drop();
        return;
    }
    state = ST_CONNECTING;
    connect_at = GetTickCount();
}

static void send_hello(void)
{
    unsigned char p[10 + RKM_NAME_MAX];
    unsigned int n;
    BOOL one = TRUE;

    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    n = rkm_hello(p, RKM_CAP_INPUT | RKM_CAP_CLIP, RKM_CS_CP1252, RKM_EOL_CRLF,
                  (unsigned int)GetSystemMetrics(SM_CXSCREEN),
                  (unsigned int)GetSystemMetrics(SM_CYSCREEN),
                  (unsigned int)(MAX_CLIP / 1024), my_name);
    send_frame(RKM_HELLO, p, n);
}

/* ---- input injection ----------------------------------------------------- */

static int vk_for(int u, int *ext)
{
    *ext = 0;
    if (u >= HID_A && u <= HID_Z) return 'A' + (u - HID_A);
    if (u >= HID_1 && u < HID_0) return '1' + (u - HID_1);
    if (u >= HID_F1 && u <= HID_F12) return VK_F1 + (u - HID_F1);
    if (u >= HID_F13 && u <= HID_F24) return VK_F13 + (u - HID_F13);
    if (u >= HID_KP_1 && u <= HID_KP_9) return VK_NUMPAD1 + (u - HID_KP_1);
    switch (u) {
    case HID_0: return '0';
    case HID_ENTER: return VK_RETURN;
    case HID_ESC: return VK_ESCAPE;
    case HID_BACKSPACE: return VK_BACK;
    case HID_TAB: return VK_TAB;
    case HID_SPACE: return VK_SPACE;
    case HID_MINUS: return 0xBD;
    case HID_EQUAL: return 0xBB;
    case HID_LBRACKET: return 0xDB;
    case HID_RBRACKET: return 0xDD;
    case HID_BACKSLASH: return 0xDC;
    case HID_NONUS_HASH: return 0xDC;
    case HID_SEMICOLON: return 0xBA;
    case HID_APOSTROPHE: return 0xDE;
    case HID_GRAVE: return 0xC0;
    case HID_COMMA: return 0xBC;
    case HID_DOT: return 0xBE;
    case HID_SLASH: return 0xBF;
    case HID_CAPSLOCK: return VK_CAPITAL;
    case HID_PRINTSCREEN: *ext = 1; return VK_SNAPSHOT;
    case HID_SCROLLLOCK: return VK_SCROLL;
    case HID_PAUSE: return VK_PAUSE;
    case HID_INSERT: *ext = 1; return VK_INSERT;
    case HID_HOME: *ext = 1; return VK_HOME;
    case HID_PAGEUP: *ext = 1; return VK_PRIOR;
    case HID_DELETE: *ext = 1; return VK_DELETE;
    case HID_END: *ext = 1; return VK_END;
    case HID_PAGEDOWN: *ext = 1; return VK_NEXT;
    case HID_RIGHT: *ext = 1; return VK_RIGHT;
    case HID_LEFT: *ext = 1; return VK_LEFT;
    case HID_DOWN: *ext = 1; return VK_DOWN;
    case HID_UP: *ext = 1; return VK_UP;
    case HID_NUMLOCK: *ext = 1; return VK_NUMLOCK;
    case HID_KP_SLASH: *ext = 1; return VK_DIVIDE;
    case HID_KP_STAR: return VK_MULTIPLY;
    case HID_KP_MINUS: return VK_SUBTRACT;
    case HID_KP_PLUS: return VK_ADD;
    case HID_KP_ENTER: *ext = 1; return VK_RETURN;
    case HID_KP_0: return VK_NUMPAD0;
    case HID_KP_DOT: return VK_DECIMAL;
    case HID_NONUS_BACKSLASH: return VK_OEM_102;
    case HID_MENU: *ext = 1; return VK_APPS;
    case HID_LCTRL: return VK_CONTROL;
    case HID_RCTRL: *ext = 1; return VK_CONTROL;
    case HID_LSHIFT: return VK_SHIFT;
    case HID_RSHIFT: return VK_SHIFT;
    case HID_LALT: return VK_MENU;
    case HID_RALT: *ext = 1; return VK_MENU;
    case HID_LGUI: *ext = 1; return VK_LWIN;
    case HID_RGUI: *ext = 1; return VK_RWIN;
    }
    return 0;
}

static void fake_key(int usage, int st)
{
    int ext, vk = vk_for(usage, &ext);
    UINT scan;
    DWORD flags;

    if (!vk) return;
    if (st == RKM_KEY_UP && !key_down[usage]) return;
    key_down[usage] = (unsigned char)(st != RKM_KEY_UP);
    scan = MapVirtualKey((UINT)vk, 0);
    if (usage == HID_RSHIFT) scan = 0x36;       /* the scan code tells the shifts apart */
    flags = (ext ? KEYEVENTF_EXTENDEDKEY : 0) | (st == RKM_KEY_UP ? KEYEVENTF_KEYUP : 0);
    keybd_event((BYTE)vk, (BYTE)scan, flags, 0);
}

static void fake_button(int btn, int down)
{
    DWORD flags = 0, data = 0;

    switch (btn) {
    case RKM_BTN_LEFT: flags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
    case RKM_BTN_RIGHT: flags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
    case RKM_BTN_MIDDLE: flags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
    case RKM_BTN_BACK:
    case RKM_BTN_FWD:
        if (LOBYTE(LOWORD(GetVersion())) < 5) return;    /* extra buttons need Windows 2000 */
        flags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        data = (DWORD)(btn - RKM_BTN_BACK + 1);
        break;
    default:
        return;
    }
    if (down) btn_down |= 1 << btn;
    else btn_down &= ~(1 << btn);
    mouse_event(flags, 0, 0, data, 0);
}

static void move_abs(unsigned int x, unsigned int y)
{
    unsigned long w = (unsigned long)GetSystemMetrics(SM_CXSCREEN);
    unsigned long h = (unsigned long)GetSystemMetrics(SM_CYSCREEN);
    if (!w || !h) return;
    /* normalised 0..65535, aimed at the centre of the target pixel */
    mouse_event(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE,
                (DWORD)((x * 65536UL + 32768UL) / w), (DWORD)((y * 65536UL + 32768UL) / h), 0, 0);
}

static void release_all(void)
{
    int i;
    for (i = 0; i < 256; i++)
        if (key_down[i]) fake_key(i, RKM_KEY_UP);
    for (i = 1; i <= 5; i++)
        if (btn_down & (1 << i)) fake_button(i, 0);
}

/* ---- clipboard ----------------------------------------------------------- */

static void clip_report(void)
{
    HANDLE h;
    const char *p;
    unsigned char hdr[5];
    unsigned long n, off;

    if (!clip_dirty || state != ST_UP) return;
    clip_dirty = 0;
    if (!IsClipboardFormatAvailable(CF_TEXT) || !OpenClipboard(hwnd)) return;
    h = GetClipboardData(CF_TEXT);
    p = h ? (const char *)GlobalLock(h) : NULL;
    if (p) {
        n = (unsigned long)lstrlen(p);
        if (n > 0 && n <= MAX_CLIP) {
            hdr[0] = RKM_CLIP_TEXT;
            RKM_PUT32(hdr + 1, n);
            send_frame(RKM_CLIP_BEGIN, hdr, 5);
            for (off = 0; off < n; off += RKM_CLIP_CHUNK)
                send_frame(RKM_CLIP_DATA, (const unsigned char *)p + off,
                           (unsigned int)(n - off < RKM_CLIP_CHUNK ? n - off : RKM_CLIP_CHUNK));
            send_frame(RKM_CLIP_END, NULL, 0);
        }
        GlobalUnlock(h);
    }
    CloseClipboard();
}

static void clip_install(void)
{
    char *p;
    if (!clip_in) return;
    p = (char *)GlobalLock(clip_in);
    if (p) {
        p[clip_in_len] = 0;
        GlobalUnlock(clip_in);
    }
    if (p && OpenClipboard(hwnd)) {
        EmptyClipboard();                        /* makes us the owner; see WM_DRAWCLIPBOARD */
        if (SetClipboardData(CF_TEXT, clip_in)) clip_in = NULL;   /* the system owns it now */
        CloseClipboard();
    }
    if (clip_in) GlobalFree(clip_in);
    clip_in = NULL;
}

/* ---- frames from the hub ------------------------------------------------- */

static void on_frame(void *ctx, int type, const unsigned char *p, unsigned int len)
{
    int b;
    (void)ctx;

    switch (type) {
    case RKM_WELCOME:
        if (len >= 2 && p[1] != RKM_OK) {
            MessageBox(NULL, p[1] == RKM_ERR_NAME
                       ? "The hub does not know this screen name.\nAdd a [screen] section for it to the hub config."
                       : "The hub speaks a different protocol version.", "RetroKM", MB_ICONSTOP);
            PostQuitMessage(1);
        } else {
            tray_tip("RetroKM: connected");
        }
        break;
    case RKM_PING:
        send_frame(RKM_PONG, NULL, 0);
        break;
    case RKM_ENTER:
        if (len < 5) break;
        move_abs(RKM_GET16(p), RKM_GET16(p + 2));
        for (b = 0; b < 8; b++)
            if (p[4] & (1 << b)) fake_key(HID_LCTRL + b, RKM_KEY_DOWN);
        break;
    case RKM_LEAVE:
        release_all();
        clip_report();
        break;
    case RKM_RESET:
        release_all();
        break;
    case RKM_MOVE:
        if (len >= 4) move_abs(RKM_GET16(p), RKM_GET16(p + 2));
        break;
    case RKM_MOVEREL:
        if (len >= 4) mouse_event(MOUSEEVENTF_MOVE, (DWORD)RKM_GETS16(p), (DWORD)RKM_GETS16(p + 2), 0, 0);
        break;
    case RKM_BUTTON:
        if (len >= 2) fake_button(p[0], p[1]);
        break;
    case RKM_WHEEL:
        if (len >= 2 && RKM_GETS16(p)) mouse_event(MOUSEEVENTF_WHEEL, 0, 0, (DWORD)RKM_GETS16(p), 0);
        break;
    case RKM_KEY:
        if (len >= 2) fake_key(p[0], p[1]);
        break;
    case RKM_CLIP_BEGIN:
        if (clip_in) GlobalFree(clip_in);
        clip_in = NULL;
        if (len >= 5 && p[0] == RKM_CLIP_TEXT && RKM_GET32(p + 1) <= MAX_CLIP) {
            clip_in_total = RKM_GET32(p + 1);
            clip_in_len = 0;
            clip_in = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, clip_in_total + 1);
        }
        break;
    case RKM_CLIP_DATA:
        if (clip_in && clip_in_len + len <= clip_in_total) {
            char *d = (char *)GlobalLock(clip_in);
            if (d) {
                memcpy(d + clip_in_len, p, len);
                clip_in_len += len;
                GlobalUnlock(clip_in);
            }
        }
        break;
    case RKM_CLIP_END:
        clip_install();
        break;
    default:
        break;
    }
}

/* ---- window -------------------------------------------------------------- */

static LRESULT CALLBACK wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SOCKET:
        if ((SOCKET)wp != sock) break;
        if (WSAGETSELECTERROR(lp)) { drop(); break; }
        switch (WSAGETSELECTEVENT(lp)) {
        case FD_CONNECT:
            state = ST_UP;
            last_rx = GetTickCount();
            rkm_parser_init(&parser);
            send_hello();
            break;
        case FD_READ: {
            char buf[2048];
            int n = recv(sock, buf, sizeof buf, 0);
            if (n > 0) {
                last_rx = GetTickCount();
                if (rkm_feed(&parser, (unsigned char *)buf, (unsigned long)n, on_frame, NULL) < 0) drop();
            } else if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) {
                drop();
            }
            break;
        }
        case FD_CLOSE:
            drop();
            break;
        }
        break;

    case WM_TIMER: {
        DWORD now = GetTickCount();
        if (state == ST_IDLE && (long)(now - retry_at) >= 0) start_connect();
        else if (state == ST_CONNECTING && now - connect_at > 10000) drop();
        else if (state == ST_UP && now - last_rx > SILENCE_MS) drop();
        clip_report();                           /* a copy made while the pointer stays here */
        break;
    }

    case WM_DRAWCLIPBOARD:
        /* Skip the notification caused by joining the chain and by our own writes. */
        if (clip_ready && GetClipboardOwner() != hwnd) clip_dirty = 1;
        if (next_viewer) SendMessage(next_viewer, msg, wp, lp);
        break;
    case WM_CHANGECBCHAIN:
        if ((HWND)wp == next_viewer) next_viewer = (HWND)lp;
        else if (next_viewer) SendMessage(next_viewer, msg, wp, lp);
        break;

    case WM_DISPLAYCHANGE: {
        unsigned char p[4];
        RKM_PUT16(p, (unsigned int)LOWORD(lp));
        RKM_PUT16(p + 2, (unsigned int)HIWORD(lp));
        send_frame(RKM_SCREEN, p, 4);
        break;
    }

    case WM_TRAY:
        if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP) {
            HMENU m = CreatePopupMenu();
            POINT pt;
            AppendMenu(m, MF_STRING | MF_GRAYED, 0, nid.szTip);
            AppendMenu(m, MF_SEPARATOR, 0, NULL);
            AppendMenu(m, MF_STRING, IDM_QUIT, "Quit RetroKM");
            GetCursorPos(&pt);
            SetForegroundWindow(w);
            TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, w, NULL);
            PostMessage(w, WM_NULL, 0, 0);
            DestroyMenu(m);
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wp) == IDM_QUIT) DestroyWindow(w);
        break;

    case WM_ENDSESSION:
    case WM_DESTROY:
        release_all();
        ChangeClipboardChain(w, next_viewer);
        Shell_NotifyIcon(NIM_DELETE, &nid);
        if (msg == WM_DESTROY) PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(w, msg, wp, lp);
    }
    return 0;
}

/* Split "host name port" out of the command line, or fall back to rkm.ini. */
static void configure(const char *cmdline)
{
    char ini[MAX_PATH], args[256], *tok, *slash;
    DWORD n = sizeof my_name;

    GetComputerName(my_name, &n);
    CharLower(my_name);

    GetModuleFileName(NULL, ini, sizeof ini);
    slash = strrchr(ini, '\\');
    if (slash) lstrcpy(slash + 1, "rkm.ini");
    GetPrivateProfileString("retrokm", "hub", "", hub_host, sizeof hub_host, ini);
    GetPrivateProfileString("retrokm", "name", my_name, my_name, sizeof my_name, ini);
    hub_port = (int)GetPrivateProfileInt("retrokm", "port", RKM_PORT, ini);

    lstrcpyn(args, cmdline, sizeof args);
    tok = strtok(args, " \t");
    if (tok) { lstrcpyn(hub_host, tok, sizeof hub_host); tok = strtok(NULL, " \t"); }
    if (tok) { lstrcpyn(my_name, tok, sizeof my_name); tok = strtok(NULL, " \t"); }
    if (tok) hub_port = atoi(tok);
}

int WINAPI WinMain(HINSTANCE hinst, HINSTANCE prev, LPSTR cmdline, int show)
{
    WSADATA wsa;
    WNDCLASS wc;
    MSG msg;
    (void)prev;
    (void)show;

    inst = hinst;
    configure(cmdline);
    if (!hub_host[0]) {
        MessageBox(NULL, "Usage: rkm-win32 hub-host [name [port]]\n\n"
                         "or create rkm.ini next to the program:\n\n[retrokm]\nhub=192.168.1.10\nname=pc",
                   "RetroKM", MB_ICONINFORMATION);
        return 2;
    }
    if (WSAStartup(MAKEWORD(1, 1), &wsa) != 0) return 1;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = "RetroKM";
    RegisterClass(&wc);
    hwnd = CreateWindow("RetroKM", "RetroKM", WS_OVERLAPPED, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!hwnd) return 1;

    memset(&nid, 0, sizeof nid);
#ifdef NOTIFYICONDATA_V1_SIZE
    nid.cbSize = NOTIFYICONDATA_V1_SIZE;         /* the size Windows 98 expects */
#else
    nid.cbSize = sizeof nid;
#endif
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    lstrcpy(nid.szTip, "RetroKM: waiting for hub");
    Shell_NotifyIcon(NIM_ADD, &nid);

    next_viewer = SetClipboardViewer(hwnd);
    clip_ready = 1;
    SetTimer(hwnd, 1, 1000, NULL);
    start_connect();

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    if (sock != INVALID_SOCKET) closesocket(sock);
    WSACleanup();
    return (int)msg.wParam;
}

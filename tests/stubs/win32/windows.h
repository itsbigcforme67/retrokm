/* Syntax-check stand-in for the Win32 SDK headers.  NOT a real SDK: it only
 * lets `gcc -std=c89 -fsyntax-only` parse the agent on a host without one. */
typedef void *HANDLE, *HWND, *HINSTANCE, *HGLOBAL, *HMENU, *HICON;
typedef unsigned long DWORD; typedef unsigned int UINT; typedef unsigned char BYTE;
typedef unsigned long WPARAM; typedef long LPARAM, LRESULT; typedef int BOOL;
typedef char *LPSTR; typedef unsigned long SOCKET; typedef unsigned short u_short, WORD;
typedef struct { long x, y; } POINT;
typedef struct { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; } MSG;
typedef struct { UINT style; LRESULT (*lpfnWndProc)(HWND, UINT, WPARAM, LPARAM); HINSTANCE hInstance; const char *lpszClassName; } WNDCLASS;
typedef struct { int x; } WSADATA;
typedef struct { DWORD cbSize; HWND hWnd; UINT uID, uFlags, uCallbackMessage; HICON hIcon; char szTip[64]; } NOTIFYICONDATA;
struct in_addr { unsigned long s_addr; };
struct sockaddr { int x; };
struct sockaddr_in { short sin_family; u_short sin_port; struct in_addr sin_addr; };
struct hostent { char **h_addr_list; };
struct hostent *gethostbyname(const char *);
HANDLE GetClipboardData(UINT); void *GlobalLock(HGLOBAL); HGLOBAL GlobalAlloc(UINT, unsigned long);
HWND CreateWindow(const char *, const char *, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, void *);
HWND SetClipboardViewer(HWND); HWND GetClipboardOwner(void); HMENU CreatePopupMenu(void); HICON LoadIcon(HINSTANCE, const char *);
LRESULT DefWindowProc(HWND, UINT, WPARAM, LPARAM); DWORD GetTickCount(void); DWORD GetVersion(void);
SOCKET socket(int, int, int); unsigned long inet_addr(const char *); u_short htons(u_short);
#define CALLBACK
#define WINAPI
#define MAX_PATH 260
#define INVALID_SOCKET ((SOCKET)~0)
#define SOCKET_ERROR (-1)
#define INADDR_NONE 0xFFFFFFFFUL
#define LOWORD(x) ((WORD)(x))
#define HIWORD(x) ((WORD)((x) >> 16))
#define LOBYTE(x) ((BYTE)(x))
#define MAKEWORD(a, b) ((WORD)((a) | ((b) << 8)))
#define WSAGETSELECTERROR(l) HIWORD(l)
#define WSAGETSELECTEVENT(l) LOWORD(l)
#define TRUE 1
#define NULL ((void *)0)
#define WM_USER 0x400
enum { WM_TIMER = 1, WM_DRAWCLIPBOARD, WM_CHANGECBCHAIN, WM_DISPLAYCHANGE, WM_COMMAND,
       WM_ENDSESSION, WM_DESTROY, WM_NULL, WM_RBUTTONUP, WM_LBUTTONUP, WSAEWOULDBLOCK, AF_INET,
       SOCK_STREAM, FD_CONNECT, FD_READ, FD_CLOSE, IPPROTO_TCP, TCP_NODELAY, SM_CXSCREEN, SM_CYSCREEN,
       VK_F1, VK_F13, VK_NUMPAD0, VK_NUMPAD1, VK_RETURN, VK_ESCAPE, VK_BACK, VK_TAB, VK_SPACE,
       VK_CAPITAL, VK_SNAPSHOT, VK_SCROLL, VK_PAUSE, VK_INSERT, VK_HOME, VK_PRIOR, VK_DELETE, VK_END,
       VK_NEXT, VK_RIGHT, VK_LEFT, VK_DOWN, VK_UP, VK_NUMLOCK, VK_DIVIDE, VK_MULTIPLY, VK_SUBTRACT,
       VK_ADD, VK_DECIMAL, VK_APPS, VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN,
       KEYEVENTF_EXTENDEDKEY, KEYEVENTF_KEYUP, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP,
       MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP,
       MOUSEEVENTF_MOVE, MOUSEEVENTF_ABSOLUTE, CF_TEXT, GMEM_MOVEABLE, GMEM_DDESHARE, MB_ICONSTOP,
       MB_ICONINFORMATION, MF_STRING, MF_GRAYED, MF_SEPARATOR, TPM_RIGHTBUTTON, NIM_ADD, NIM_MODIFY,
       NIM_DELETE, NIF_ICON, NIF_MESSAGE, NIF_TIP, WS_OVERLAPPED };
#define IDI_APPLICATION ((const char *)32512)

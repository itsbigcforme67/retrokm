/* Syntax-check stand-in for the Mac Toolbox headers.  NOT real interfaces:
 * it only lets `gcc -std=c89 -fsyntax-only` parse the agent on a host
 * without a 68k cross compiler. */
#ifndef MACSTUB_H
#define MACSTUB_H
typedef char *Ptr; typedef Ptr *Handle; typedef unsigned char Boolean; typedef short OSErr;
typedef unsigned char Str255[256]; typedef const unsigned char *ConstStr255Param;
typedef struct { short v, h; } Point;
typedef struct { short top, left, bottom, right; } Rect;
typedef struct { short what; unsigned long message, when; Point where; unsigned short modifiers; } EventRecord;
typedef struct { Rect portRect; } GrafPort, *GrafPtr, *WindowPtr;
typedef struct { int x; } **MenuHandle;
typedef struct { unsigned long hi, lo; } ProcessSerialNumber;
typedef void *ParmBlkPtr;
typedef struct { short scrapCount; } *PScrapStuff;
extern struct { GrafPtr thePort; struct { Rect bounds; } screenBits; } qd;
PScrapStuff InfoScrap(void); Handle NewHandle(long); Ptr NewPtr(long);
MenuHandle NewMenu(short, const char *); MenuHandle GetMenuHandle(short);
WindowPtr NewWindow(void *, const Rect *, const char *, Boolean, short, WindowPtr, Boolean, long);
long NGetTrapAddress(unsigned short, int); long GetScriptManagerVariable(short);
unsigned long KeyTranslate(const void *, unsigned short, unsigned long *);
long MenuSelect(Point); long MenuKey(char); long GetScrap(Handle, unsigned long, long *);
enum { noErr, mouseDown, mouseUp, keyDown, keyUp, autoKey, updateEvt, osEvt = 15, everyEvent = -1,
       inMenuBar = 1, inSysWindow, inContent, inDrag, cmdKey = 0x100, shiftKey = 0x200, alphaLock = 0x400,
       optionKey = 0x800, controlKey = 0x1000, charCodeMask = 0xFF, suspendResumeMessage = 1,
       resumeFlag = 1, noGrowDocProc = 4, OSTrap = 0, ToolTrap = 1, gestaltLaunchControl = 3,
       smKCHRCache = 38, false = 0, true = 1 };
#define gestaltOSAttr 0x6F732020UL
#endif

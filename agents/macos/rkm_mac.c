/*
 * RetroKM agent for classic Mac OS: 68k System 6 (MultiFinder) / System 7,
 * and PowerPC Mac OS 8 / 9 (runs there as a 68k application).
 *
 * Network:   MacTCP driver API (.IPP).  Open Transport provides the same
 *            API for compatibility, so one binary covers every target.
 * Input:     injected at interrupt time from the TCP receive completion
 *            routine, the same way the ADB drivers do it: low-memory cursor
 *            globals for the pointer, PostEvent for clicks and keys.  That
 *            keeps the mouse alive while another application tracks a menu
 *            or otherwise hogs the processor.
 * Clipboard: Scrap Manager, from the main event loop.
 *
 * Build with Retro68 (see CMakeLists.txt).  The file is plain C89 and has
 * its own MacTCP definitions, so THINK C / CodeWarrior / MPW also work;
 * those compilers reach globals through A5, so define RKM_A5_GLOBALS there.
 *
 * Configuration: a text file named "RetroKM Config" next to the program:
 *     hub=192.168.1.10
 *     name=quadra
 *     port=24850          (optional)
 *     flip=smart          (optional: smart, always or never; see README)
 */
#include <Quickdraw.h>
#include <Windows.h>
#include <Menus.h>
#include <Events.h>
#include <Fonts.h>
#include <Dialogs.h>
#include <TextEdit.h>
#include <Scrap.h>
#include <Files.h>
#include <Devices.h>
#include <Memory.h>
#include <OSUtils.h>
#include <ToolUtils.h>
#include <Processes.h>
#include <Gestalt.h>
#include <Script.h>
#include "../../common/rkm_proto.h"

#if defined(__POWERPC__) || defined(__ppc__) || defined(powerc)
#error "Build this agent as a 68k application; it also runs on PowerPC Macs."
#endif

/* ---- MacTCP, the parts we use ------------------------------------------- */

#define kTCPCreate      30
#define kTCPActiveOpen  32
#define kTCPSend        34
#define kTCPRcv         37
#define kTCPAbort       39
#define kTCPRelease     42
#define kInProgress     1

struct RKiopb;
typedef void (*RKCompletion)(struct RKiopb *pb);     /* C calling convention */

typedef struct RKiopb {
    char            fill12[12];
    RKCompletion    ioCompletion;
    short           ioResult;
    char           *ioNamePtr;
    short           ioVRefNum;
    short           ioCRefNum;
    short           csCode;
    unsigned long   tcpStream;
    union {
        struct {
            Ptr             rcvBuff;
            unsigned long   rcvBuffLen;
            Ptr             notifyProc;
            Ptr             userDataPtr;
        } create;
        struct {
            unsigned char   ulpTimeoutValue, ulpTimeoutAction, validityFlags, commandTimeoutValue;
            unsigned long   remoteHost;
            unsigned short  remotePort;
            unsigned long   localHost;
            unsigned short  localPort;
            unsigned char   tosFlags, precedence, dontFrag, timeToLive, security, optionCnt;
            unsigned char   options[40];
            Ptr             userDataPtr;
        } open;
        struct {
            unsigned char   ulpTimeoutValue, ulpTimeoutAction, validityFlags, pushFlag;
            unsigned char   urgentFlag, filler;
            Ptr             wdsPtr;
            unsigned long   sendFree;
            unsigned short  sendLength;
            Ptr             userDataPtr;
        } send;
        struct {
            unsigned char   commandTimeoutValue, flagsAndFiller[3];
            Ptr             rcvBuff;
            unsigned short  rcvBuffLen;
            Ptr             rdsPtr;
            unsigned short  rdsLength;
            unsigned short  secondTimeStamp;
            Ptr             userDataPtr;
        } receive;
        char            pad[96];
    } csParam;
#ifdef RKM_A5_GLOBALS
    long            a5;                               /* our A5, for the completion routine */
#endif
} RKiopb;

typedef struct { unsigned short length; Ptr ptr; unsigned short end; } RKwds;

/* These layouts only hold with 68k (2-byte) alignment; refuse to build otherwise. */
#ifndef RKM_SYNTAX_CHECK
typedef char rk_check_open[(sizeof(((RKiopb *)0)->csParam.open) == 66) ? 1 : -1];
typedef char rk_check_wds[(sizeof(RKwds) == 8) ? 1 : -1];
#endif

/* ---- low-memory globals -------------------------------------------------- */

#define LM_Ticks       (*(volatile unsigned long *)0x016A)
#define LM_MBState     (*(volatile unsigned char *)0x0172)
#define LM_KeyMap      ((volatile unsigned char *)0x0174)
#define LM_MTemp       (*(volatile Point *)0x0828)
#define LM_RawMouse    (*(volatile Point *)0x082C)
#define LM_Mouse       (*(volatile Point *)0x0830)
#define LM_CrsrNew     (*(volatile unsigned char *)0x08CE)
#define LM_CrsrCouple  (*(volatile unsigned char *)0x08CF)
#define LM_MBarHeight  (*(volatile short *)0x0BAA)

#define TRAP_WaitNextEvent 0xA860
#define TRAP_Gestalt       0xA1AD
#define TRAP_Unimplemented 0xA89F

#define VK_COMMAND  0x37
#define VK_SHIFT    0x38
#define VK_CAPS     0x39
#define VK_OPTION   0x3A
#define VK_CONTROL  0x3B
#define VK_NONE     0xFF

/* ---- state --------------------------------------------------------------- */

#define STREAM_BUF  8192L
#define CLIP_MAX    32000L
#define RETRY_TICKS 180L
#define SILENCE_TICKS (30L * 60L)

enum { S_NOCONFIG, S_IDLE, S_OPENING, S_UP, S_DEAD, S_REFUSED };
enum { FLIP_NEVER, FLIP_SMART, FLIP_ALWAYS };
enum { FL_NONE, FL_OUT, FL_IN };

static unsigned long  gHubAddr;
static unsigned short gHubPort = RKM_PORT;
static char           gName[RKM_NAME_MAX + 1] = "mac";
static short          gFlipMode = FLIP_SMART;

static short          gDrv;
static unsigned long  gStream;
static Ptr            gStreamBuf;
static RKiopb         gOpenPB, gRcvPB, gPongPB;
static RKwds          gPongWds;
static unsigned char  gPongFrame[RKM_HDR];
static unsigned char  gRcvBuf[1024];
static rkm_parser     gParser;
static volatile short gState = S_NOCONFIG;
static volatile unsigned long gLastRx;
static unsigned long  gRetryAt;

static short          gScreenW, gScreenH;
static Point          gMouse;
static Boolean        gButtonDown, gFakeControl;
static unsigned char  gVK[256];                 /* HID usage -> Mac virtual key code */
static unsigned char  gHeld[16];                /* KeyMap bits we set */
static Ptr            gKCHR;
static unsigned long  gKeyTransState;

static Ptr            gClipBuf;                 /* clipboard arriving from the hub */
static volatile unsigned long gClipLen, gClipTotal;
static volatile Boolean gClipActive, gClipReady, gClipBusy;
static volatile Boolean gLeaveFlag, gMaybeCopied;
static short          gLastScrapCount;

static Boolean        gHasWNE, gHasProcMgr, gInFront = true, gQuit;
static short          gFlip = FL_NONE;
static unsigned long  gFlipDeadline;
static ProcessSerialNumber gPrevFront;
static WindowPtr      gWindow;
static short          gShownState = -1;

/* ---- interrupt-time injection --------------------------------------------- */

static void SetMouse(short x, short y)
{
    Point p;

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > gScreenW - 1) x = gScreenW - 1;
    if (y > gScreenH - 1) y = gScreenH - 1;
    p.h = x;
    p.v = y;
    gMouse = p;
    LM_RawMouse = p;
    LM_MTemp = p;
    LM_CrsrNew = LM_CrsrCouple;                 /* ask the cursor task to redraw */
}

static void SetKeyBit(unsigned char vk, Boolean down)
{
    unsigned char bit = (unsigned char)(1 << (vk & 7));
    if (down) {
        LM_KeyMap[vk >> 3] |= bit;
        gHeld[vk >> 3] |= bit;
    } else {
        LM_KeyMap[vk >> 3] &= (unsigned char)~bit;
        gHeld[vk >> 3] &= (unsigned char)~bit;
    }
}

#define KEYBIT(vk) (LM_KeyMap[(vk) >> 3] & (1 << ((vk) & 7)))

static void MouseButton(Boolean down)
{
    if (down == gButtonDown) return;
    gButtonDown = down;
    LM_Mouse = gMouse;                          /* the click lands where the pointer is now */
    LM_MBState = down ? 0x00 : 0x80;
    PostEvent(down ? mouseDown : mouseUp, 0L);
    if (down && gMouse.v < LM_MBarHeight) gMaybeCopied = true;   /* maybe Edit > Copy */
}

static void KeyEvent(unsigned char usage, unsigned char state)
{
    unsigned char vk = gVK[usage], ch = 0;
    unsigned short mods = 0, code;
    unsigned long r, msg;

    if (vk == VK_NONE) return;
    if (vk == VK_CAPS) {                        /* a latching key on the Mac */
        if (state == RKM_KEY_DOWN) SetKeyBit(vk, (Boolean)!KEYBIT(vk));
        return;
    }
    if (state != RKM_KEY_REPEAT) SetKeyBit(vk, (Boolean)(state == RKM_KEY_DOWN));
    if (usage >= HID_LCTRL) return;             /* modifiers post no events */

    if (KEYBIT(VK_COMMAND)) mods |= cmdKey;
    if (KEYBIT(VK_SHIFT)) mods |= shiftKey;
    if (KEYBIT(VK_CAPS)) mods |= alphaLock;
    if (KEYBIT(VK_OPTION)) mods |= optionKey;
    if (KEYBIT(VK_CONTROL)) mods |= controlKey;

    if (gKCHR) {
        code = (unsigned short)(vk | (mods & 0xFF00) | (state == RKM_KEY_UP ? 0x80 : 0));
        r = KeyTranslate(gKCHR, code, &gKeyTransState);
        ch = (unsigned char)(r & 0xFF);
        if (!ch) ch = (unsigned char)((r >> 16) & 0xFF);
        if (!ch && state != RKM_KEY_UP) return;  /* dead key: wait for the next one */
    }
    msg = ((unsigned long)vk << 8) | ch;
    PostEvent(state == RKM_KEY_UP ? keyUp : state == RKM_KEY_REPEAT ? autoKey : keyDown, msg);

    if (state == RKM_KEY_DOWN && (mods & cmdKey) && (usage == HID_A + 2 || usage == HID_A + 23))
        gMaybeCopied = true;                    /* Command-C or Command-X */
}

static void ReleaseAll(void)
{
    short i;

    if (gButtonDown) MouseButton(false);
    for (i = 0; i < 16; i++) {
        unsigned char keep = (i == (VK_CAPS >> 3)) ? (unsigned char)(1 << (VK_CAPS & 7)) : 0;
        LM_KeyMap[i] &= (unsigned char)~(gHeld[i] & ~keep);
        gHeld[i] &= keep;
    }
    gFakeControl = false;
}

static void SendPong(void)
{
    if (gPongPB.ioResult == kInProgress) return;
    gPongPB.ioCompletion = 0;
    gPongPB.ioCRefNum = gDrv;
    gPongPB.csCode = kTCPSend;
    gPongPB.tcpStream = gStream;
    gPongPB.csParam.send.ulpTimeoutValue = 20;
    gPongPB.csParam.send.ulpTimeoutAction = 1;
    gPongPB.csParam.send.validityFlags = 0xC0;
    gPongPB.csParam.send.pushFlag = 1;
    gPongPB.csParam.send.wdsPtr = (Ptr)&gPongWds;
    PBControlAsync((ParmBlkPtr)&gPongPB);
}

/* Runs at interrupt time.  No Memory Manager, no QuickDraw. */
static void OnFrame(void *ctx, int type, const unsigned char *p, unsigned int len)
{
    unsigned int i;
    (void)ctx;

    switch (type) {
    case RKM_WELCOME:
        if (len >= 2 && p[1] != RKM_OK) gState = S_REFUSED;
        break;
    case RKM_PING:
        SendPong();
        break;
    case RKM_ENTER:
        if (len < 5) break;
        SetMouse((short)RKM_GET16(p), (short)RKM_GET16(p + 2));
        for (i = 0; i < 8; i++)
            if (p[4] & (1 << i)) KeyEvent((unsigned char)(HID_LCTRL + i), RKM_KEY_DOWN);
        break;
    case RKM_LEAVE:
        ReleaseAll();
        gLeaveFlag = true;
        break;
    case RKM_RESET:
        ReleaseAll();
        break;
    case RKM_MOVE:
        if (len >= 4) SetMouse((short)RKM_GET16(p), (short)RKM_GET16(p + 2));
        break;
    case RKM_MOVEREL:
        if (len >= 4) SetMouse((short)(gMouse.h + RKM_GETS16(p)), (short)(gMouse.v + RKM_GETS16(p + 2)));
        break;
    case RKM_BUTTON:
        if (len < 2) break;
        if (p[0] == RKM_BTN_LEFT) {
            MouseButton((Boolean)p[1]);
        } else if (p[0] == RKM_BTN_RIGHT) {     /* one-button Mac: right click = control-click */
            if (p[1]) {
                if (!KEYBIT(VK_CONTROL)) { SetKeyBit(VK_CONTROL, true); gFakeControl = true; }
                MouseButton(true);
            } else {
                MouseButton(false);
                if (gFakeControl) { SetKeyBit(VK_CONTROL, false); gFakeControl = false; }
            }
        }
        break;
    case RKM_KEY:
        if (len >= 2) KeyEvent(p[0], p[1]);
        break;
    case RKM_CLIP_BEGIN:
        gClipActive = false;
        if (!gClipBusy && len >= 5 && p[0] == RKM_CLIP_TEXT && RKM_GET32(p + 1) <= (unsigned long)CLIP_MAX) {
            gClipReady = false;
            gClipTotal = RKM_GET32(p + 1);
            gClipLen = 0;
            gClipActive = true;
        }
        break;
    case RKM_CLIP_DATA:
        if (gClipActive && gClipLen + len <= gClipTotal) {
            for (i = 0; i < len; i++) gClipBuf[gClipLen + i] = (char)p[i];
            gClipLen += len;
        }
        break;
    case RKM_CLIP_END:
        if (gClipActive) {
            gClipActive = false;
            gClipReady = true;                  /* the event loop puts it on the scrap */
        }
        break;
    default:
        break;
    }
}

static void StartReceive(void);

static void ReceiveDone(RKiopb *pb)
{
#ifdef RKM_A5_GLOBALS
    long oldA5 = SetA5(pb->a5);
#else
    (void)pb;
#endif
    if (gRcvPB.ioResult == noErr && gState == S_UP) {
        gLastRx = LM_Ticks;
        if (rkm_feed(&gParser, gRcvBuf, (unsigned long)gRcvPB.csParam.receive.rcvBuffLen, OnFrame, 0) < 0)
            gState = S_DEAD;
        else
            StartReceive();
    } else if (gState == S_UP) {
        gState = S_DEAD;
    }
#ifdef RKM_A5_GLOBALS
    SetA5(oldA5);
#endif
}

static void ClearPB(RKiopb *pb)
{
    char *p = (char *)pb;
    unsigned short i;
    for (i = 0; i < sizeof(RKiopb); i++) p[i] = 0;
#ifdef RKM_A5_GLOBALS
    pb->a5 = (long)SetCurrentA5();
#endif
}

static void StartReceive(void)
{
    ClearPB(&gRcvPB);
    gRcvPB.ioCompletion = ReceiveDone;
    gRcvPB.ioCRefNum = gDrv;
    gRcvPB.csCode = kTCPRcv;
    gRcvPB.tcpStream = gStream;
    gRcvPB.csParam.receive.rcvBuff = (Ptr)gRcvBuf;
    gRcvPB.csParam.receive.rcvBuffLen = sizeof gRcvBuf;
    if (PBControlAsync((ParmBlkPtr)&gRcvPB) != noErr) gState = S_DEAD;
}

/* ---- TCP from the event loop ------------------------------------------------ */

static OSErr TcpCreate(void)
{
    RKiopb pb;
    OSErr err;

    ClearPB(&pb);
    pb.ioCRefNum = gDrv;
    pb.csCode = kTCPCreate;
    pb.csParam.create.rcvBuff = gStreamBuf;
    pb.csParam.create.rcvBuffLen = STREAM_BUF;
    err = PBControlSync((ParmBlkPtr)&pb);
    gStream = pb.tcpStream;
    return err;
}

static void TcpSimple(short code)               /* abort or release */
{
    RKiopb pb;
    ClearPB(&pb);
    pb.ioCRefNum = gDrv;
    pb.csCode = code;
    pb.tcpStream = gStream;
    PBControlSync((ParmBlkPtr)&pb);
}

static void TcpOpen(void)
{
    ClearPB(&gOpenPB);
    gOpenPB.ioCRefNum = gDrv;
    gOpenPB.csCode = kTCPActiveOpen;
    gOpenPB.tcpStream = gStream;
    gOpenPB.csParam.open.ulpTimeoutValue = 15;
    gOpenPB.csParam.open.ulpTimeoutAction = 1;
    gOpenPB.csParam.open.validityFlags = 0xC0;
    gOpenPB.csParam.open.remoteHost = gHubAddr;
    gOpenPB.csParam.open.remotePort = gHubPort;
    PBControlAsync((ParmBlkPtr)&gOpenPB);
}

static OSErr TcpSend(const unsigned char *data, unsigned short len)
{
    RKiopb pb;
    RKwds wds;

    wds.length = len;
    wds.ptr = (Ptr)data;
    wds.end = 0;
    ClearPB(&pb);
    pb.ioCRefNum = gDrv;
    pb.csCode = kTCPSend;
    pb.tcpStream = gStream;
    pb.csParam.send.ulpTimeoutValue = 20;
    pb.csParam.send.ulpTimeoutAction = 1;
    pb.csParam.send.validityFlags = 0xC0;
    pb.csParam.send.pushFlag = 1;
    pb.csParam.send.wdsPtr = (Ptr)&wds;
    return PBControlSync((ParmBlkPtr)&pb);
}

static void SendFrame(int type, const unsigned char *payload, unsigned int len)
{
    unsigned char buf[RKM_HDR + RKM_MAX_PAYLOAD];
    if (gState != S_UP) return;
    if (TcpSend(buf, (unsigned short)rkm_pack(buf, type, payload, len)) != noErr) gState = S_DEAD;
}

/* ---- clipboard ---------------------------------------------------------------- */

static short ScrapCount(void)
{
    return InfoScrap()->scrapCount;
}

static void ReportScrap(void)
{
    Handle h;
    long len, offset, off;
    unsigned char hdr[5];

    gMaybeCopied = false;
    if (ScrapCount() == gLastScrapCount) return;
    gLastScrapCount = ScrapCount();
    h = NewHandle(0L);
    if (!h) return;
    len = GetScrap(h, 'TEXT', &offset);
    if (len > 0 && len <= CLIP_MAX) {
        HLock(h);
        hdr[0] = RKM_CLIP_TEXT;
        RKM_PUT32(hdr + 1, (unsigned long)len);
        SendFrame(RKM_CLIP_BEGIN, hdr, 5);
        for (off = 0; off < len; off += RKM_CLIP_CHUNK)
            SendFrame(RKM_CLIP_DATA, (unsigned char *)*h + off,
                      (unsigned int)(len - off < RKM_CLIP_CHUNK ? len - off : RKM_CLIP_CHUNK));
        SendFrame(RKM_CLIP_END, 0, 0);
        HUnlock(h);
    }
    DisposeHandle(h);
}

static void InstallScrap(void)
{
    gClipBusy = true;
    if (gClipReady) {
        ZeroScrap();
        PutScrap((long)gClipLen, 'TEXT', gClipBuf);
        gLastScrapCount = ScrapCount();
        gClipReady = false;
    }
    gClipBusy = false;
}

/* Applications keep a private clipboard and only exchange it with the system
 * scrap when they are switched out or in.  So to pick up a copy made in the
 * front application, or to hand it new text, we briefly come to the front
 * and then give the front back ("flip").  Needs the System 7 Process Manager. */
static Boolean BeginFlip(short why)
{
    ProcessSerialNumber me;

    if (!gHasProcMgr || gInFront || gFlip != FL_NONE) return false;
    if (GetFrontProcess(&gPrevFront) != noErr || GetCurrentProcess(&me) != noErr) return false;
    if (SetFrontProcess(&me) != noErr) return false;
    gFlip = why;
    gFlipDeadline = LM_Ticks + 120;
    return true;
}

static void FinishFlip(void)
{
    short why = gFlip;

    gFlip = FL_NONE;
    if (why == FL_OUT) ReportScrap();
    else if (why == FL_IN) InstallScrap();
    if (why != FL_NONE) SetFrontProcess(&gPrevFront);
}

static void ClipboardIdle(void)
{
    if (gFlip != FL_NONE) {
        if (gInFront || LM_Ticks > gFlipDeadline) FinishFlip();
        return;
    }
    if (gClipReady) {
        if (gFlipMode == FLIP_NEVER || !BeginFlip(FL_IN)) InstallScrap();
        return;
    }
    if (gLeaveFlag) {
        Boolean want = gFlipMode == FLIP_ALWAYS || (gFlipMode == FLIP_SMART && gMaybeCopied);
        gLeaveFlag = false;
        if (!want || !BeginFlip(FL_OUT)) ReportScrap();
    }
}

/* ---- connection state machine ---------------------------------------------- */

static void SendHello(void)
{
    unsigned char p[10 + RKM_NAME_MAX];
    unsigned int n = rkm_hello(p, RKM_CAP_INPUT | RKM_CAP_CLIP, RKM_CS_MACROMAN, RKM_EOL_CR,
                               (unsigned int)gScreenW, (unsigned int)gScreenH,
                               (unsigned int)(CLIP_MAX / 1024), gName);
    SendFrame(RKM_HELLO, p, n);
}

static void NetworkIdle(void)
{
    switch (gState) {
    case S_IDLE:
        if (LM_Ticks >= gRetryAt) {
            TcpOpen();
            gState = S_OPENING;
        }
        break;
    case S_OPENING:
        if (gOpenPB.ioResult == kInProgress) break;
        if (gOpenPB.ioResult == noErr) {
            rkm_parser_init(&gParser);
            gLastRx = LM_Ticks;
            gState = S_UP;
            SendHello();
            if (gState == S_UP) StartReceive();
        } else {
            gState = S_IDLE;
            gRetryAt = LM_Ticks + RETRY_TICKS;
        }
        break;
    case S_UP:
        if (LM_Ticks - gLastRx > SILENCE_TICKS) gState = S_DEAD;
        break;
    case S_DEAD:
        ReleaseAll();
        TcpSimple(kTCPAbort);                   /* also ends the pending receive */
        gState = S_IDLE;
        gRetryAt = LM_Ticks + RETRY_TICKS;
        break;
    default:
        break;
    }
}

/* ---- configuration ------------------------------------------------------------ */

static Boolean StartsWith(const char *s, const char *key)
{
    while (*key)
        if (*s++ != *key++) return false;
    return true;
}

static unsigned long ParseNumber(const char **s)
{
    unsigned long v = 0;
    while (**s >= '0' && **s <= '9') v = v * 10 + (unsigned long)(*(*s)++ - '0');
    return v;
}

static void ParseLine(const char *s)
{
    short i;

    if (StartsWith(s, "hub=")) {
        unsigned long a = 0;
        s += 4;
        for (i = 0; i < 4; i++) {
            a = (a << 8) | (ParseNumber(&s) & 0xFF);
            if (*s == '.') s++;
        }
        gHubAddr = a;
    } else if (StartsWith(s, "port=")) {
        s += 5;
        gHubPort = (unsigned short)ParseNumber(&s);
    } else if (StartsWith(s, "name=")) {
        s += 5;
        for (i = 0; i < RKM_NAME_MAX && s[i] > ' '; i++) gName[i] = s[i];
        gName[i] = 0;
    } else if (StartsWith(s, "flip=")) {
        s += 5;
        gFlipMode = (short)(*s == 'n' ? FLIP_NEVER : *s == 'a' ? FLIP_ALWAYS : FLIP_SMART);
    }
}

static void ReadConfig(void)
{
    char buf[512], *line;
    short ref;
    long n = sizeof buf - 1, i;

    if (FSOpen("\pRetroKM Config", 0, &ref) != noErr) return;
    FSRead(ref, &n, buf);                       /* eofErr is fine: n is what we got */
    FSClose(ref);
    buf[n] = 0;
    line = buf;
    for (i = 0; i <= n; i++) {
        if (buf[i] == '\r' || buf[i] == '\n' || buf[i] == 0) {
            buf[i] = 0;
            ParseLine(line);
            line = buf + i + 1;
        }
    }
}

static void InitKeyTable(void)
{
    static const unsigned char letters[26] = {
        0x00, 0x0B, 0x08, 0x02, 0x0E, 0x03, 0x05, 0x04, 0x22, 0x26, 0x28, 0x25, 0x2E,
        0x2D, 0x1F, 0x23, 0x0C, 0x0F, 0x01, 0x11, 0x20, 0x09, 0x0D, 0x07, 0x10, 0x06 };
    static const unsigned char digits[10] = {     /* 1..9, 0 */
        0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1A, 0x1C, 0x19, 0x1D };
    static const unsigned char fkeys[12] = {
        0x7A, 0x78, 0x63, 0x76, 0x60, 0x61, 0x62, 0x64, 0x65, 0x6D, 0x67, 0x6F };
    static const unsigned char keypad[10] = {     /* 1..9, 0 */
        0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5B, 0x5C, 0x52 };
    static const unsigned char misc[][2] = {
        { HID_ENTER, 0x24 }, { HID_ESC, 0x35 }, { HID_BACKSPACE, 0x33 }, { HID_TAB, 0x30 },
        { HID_SPACE, 0x31 }, { HID_MINUS, 0x1B }, { HID_EQUAL, 0x18 }, { HID_LBRACKET, 0x21 },
        { HID_RBRACKET, 0x1E }, { HID_BACKSLASH, 0x2A }, { HID_SEMICOLON, 0x29 },
        { HID_APOSTROPHE, 0x27 }, { HID_GRAVE, 0x32 }, { HID_COMMA, 0x2B }, { HID_DOT, 0x2F },
        { HID_SLASH, 0x2C }, { HID_CAPSLOCK, VK_CAPS }, { HID_PRINTSCREEN, 0x69 },
        { HID_SCROLLLOCK, 0x6B }, { HID_PAUSE, 0x71 }, { HID_INSERT, 0x72 }, { HID_HOME, 0x73 },
        { HID_PAGEUP, 0x74 }, { HID_DELETE, 0x75 }, { HID_END, 0x77 }, { HID_PAGEDOWN, 0x79 },
        { HID_RIGHT, 0x7C }, { HID_LEFT, 0x7B }, { HID_DOWN, 0x7D }, { HID_UP, 0x7E },
        { HID_NUMLOCK, 0x47 }, { HID_KP_SLASH, 0x4B }, { HID_KP_STAR, 0x43 }, { HID_KP_MINUS, 0x4E },
        { HID_KP_PLUS, 0x45 }, { HID_KP_ENTER, 0x4C }, { HID_KP_DOT, 0x41 }, { HID_KP_EQUAL, 0x51 },
        { HID_NONUS_BACKSLASH, 0x0A },
        /* PC layout convention: Alt is Option, the Windows key is Command.
         * Swap them per screen in the hub config (remap = ...) if you prefer. */
        { HID_LCTRL, VK_CONTROL }, { HID_RCTRL, VK_CONTROL }, { HID_LSHIFT, VK_SHIFT },
        { HID_RSHIFT, VK_SHIFT }, { HID_LALT, VK_OPTION }, { HID_RALT, VK_OPTION },
        { HID_LGUI, VK_COMMAND }, { HID_RGUI, VK_COMMAND } };
    unsigned short i;

    for (i = 0; i < 256; i++) gVK[i] = VK_NONE;
    for (i = 0; i < 26; i++) gVK[HID_A + i] = letters[i];
    for (i = 0; i < 10; i++) gVK[HID_1 + i] = digits[i];
    for (i = 0; i < 12; i++) gVK[HID_F1 + i] = fkeys[i];
    for (i = 0; i < 10; i++) gVK[HID_KP_1 + i] = keypad[i];
    for (i = 0; i < sizeof misc / sizeof misc[0]; i++) gVK[misc[i][0]] = misc[i][1];
}

/* ---- user interface ---------------------------------------------------------- */

static void DrawStatus(void)
{
    static const char *text[] = {
        "\pNo \"RetroKM Config\" file with a hub= line.", "\pLooking for the hub\311",
        "\pConnecting to the hub\311", "\pConnected.", "\pConnection lost, retrying\311",
        "\pThe hub does not know this screen name." };
    Str255 name;
    short i;

    SetPort(gWindow);
    EraseRect(&gWindow->portRect);
    MoveTo(12, 22);
    DrawString((ConstStr255Param)text[gState]);
    for (i = 0; gName[i]; i++) name[i + 1] = (unsigned char)gName[i];
    name[0] = (unsigned char)i;
    MoveTo(12, 40);
    DrawString("\pScreen name: ");
    DrawString(name);
    gShownState = gState;
}

static void DoMenu(long choice)
{
    short menu = (short)(choice >> 16), item = (short)(choice & 0xFFFF);
    Str255 da;

    if (menu == 128 && item > 1) {
        GetMenuItemText(GetMenuHandle(128), item, da);
        OpenDeskAcc(da);
    } else if (menu == 129 && item == 1) {
        gQuit = true;
    }
    HiliteMenu(0);
}

static void HandleEvent(EventRecord *ev)
{
    WindowPtr w;

    switch (ev->what) {
    case mouseDown:
        switch (FindWindow(ev->where, &w)) {
        case inMenuBar: DoMenu(MenuSelect(ev->where)); break;
        case inSysWindow: SystemClick(ev, w); break;
        case inDrag: DragWindow(w, ev->where, &qd.screenBits.bounds); break;
        case inContent: SelectWindow(w); break;
        }
        break;
    case keyDown:
        if (ev->modifiers & cmdKey) DoMenu(MenuKey((char)(ev->message & charCodeMask)));
        break;
    case updateEvt:
        BeginUpdate((WindowPtr)ev->message);
        DrawStatus();
        EndUpdate((WindowPtr)ev->message);
        break;
    case osEvt:
        if (((ev->message >> 24) & 0xFF) == suspendResumeMessage)
            gInFront = (Boolean)((ev->message & resumeFlag) != 0);
        break;
    }
}

static Boolean TrapAvailable(short trap, short kind)
{
    return NGetTrapAddress(trap, kind) != NGetTrapAddress(TRAP_Unimplemented, ToolTrap);
}

int main(void)
{
    static const Rect bounds = { 60, 40, 110, 360 };
    MenuHandle m;
    EventRecord ev;
    long resp;
    Boolean got;

    MaxApplZone();
    MoreMasters();
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(0);
    InitCursor();
    FlushEvents(everyEvent, 0);

    m = NewMenu(128, "\p\024");
    AppendMenu(m, "\pRetroKM agent;(-");
    AppendResMenu(m, 'DRVR');
    InsertMenu(m, 0);
    m = NewMenu(129, "\pFile");
    AppendMenu(m, "\pQuit/Q");
    InsertMenu(m, 0);
    DrawMenuBar();
    gWindow = NewWindow(0, &bounds, "\pRetroKM", true, noGrowDocProc, (WindowPtr)-1L, false, 0);

    gHasWNE = TrapAvailable((short)TRAP_WaitNextEvent, ToolTrap);
    if (TrapAvailable((short)TRAP_Gestalt, OSTrap) && Gestalt(gestaltOSAttr, &resp) == noErr)
        gHasProcMgr = (Boolean)((resp & (1L << gestaltLaunchControl)) != 0);

    gScreenW = qd.screenBits.bounds.right;
    gScreenH = qd.screenBits.bounds.bottom;
    gMouse = LM_Mouse;
    gKCHR = (Ptr)GetScriptManagerVariable(smKCHRCache);
    gLastScrapCount = ScrapCount();
    InitKeyTable();
    ReadConfig();

    gPongWds.length = (unsigned short)rkm_pack(gPongFrame, RKM_PONG, 0, 0);
    gPongWds.ptr = (Ptr)gPongFrame;
    gPongWds.end = 0;

    gClipBuf = NewPtr(CLIP_MAX);
    gStreamBuf = NewPtr(STREAM_BUF);
    if (gHubAddr && gClipBuf && gStreamBuf &&
        OpenDriver("\p.IPP", &gDrv) == noErr && TcpCreate() == noErr)
        gState = S_IDLE;

    while (!gQuit) {
        if (gHasWNE) {
            got = WaitNextEvent(everyEvent, &ev, 6L, 0);
        } else {
            SystemTask();
            got = GetNextEvent(everyEvent, &ev);
        }
        if (got) HandleEvent(&ev);
        if (gState != S_NOCONFIG) {
            NetworkIdle();
            ClipboardIdle();
        }
        if (gShownState != gState) DrawStatus();
    }

    /* MacTCP keeps using the stream buffer until the stream is released. */
    if (gStream) {
        gState = S_IDLE;
        ReleaseAll();
        TcpSimple(kTCPAbort);
        TcpSimple(kTCPRelease);
    }
    return 0;
}

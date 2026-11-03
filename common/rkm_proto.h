/*
 * RetroKM wire protocol, version 1, revision 2 (docs/PROTOCOL.md).
 *
 * Strict C89, no libc dependencies, safe to call at interrupt time on
 * classic Mac OS.  See docs/PROTOCOL.md for the full description.
 *
 * Frame:  type:u8  length:u16 (big endian)  payload[length]
 */
#ifndef RKM_PROTO_H
#define RKM_PROTO_H

#define RKM_VERSION      1     /* HELLO version: changes only if old agents must break */
#define RKM_REVISION     2     /* new messages added; old ones mean the same */
#define RKM_PORT         24850
#define RKM_HDR          3
#define RKM_MAX_PAYLOAD  512
#define RKM_CLIP_CHUNK   480
#define RKM_NAME_MAX     31

/* ---- message types ---------------------------------------------------- */
/* session */
#define RKM_HELLO      0x01  /* A->H ver,caps,charset,eol,w16,h16,clipmax16,name.. */
#define RKM_WELCOME    0x02  /* H->A ver,status                                   */
#define RKM_PING       0x03  /* H->A                                              */
#define RKM_PONG       0x04  /* A->H                                              */
#define RKM_SCREEN     0x05  /* A->H w16,h16[,n,n*(x16,y16,w16,h16,mmw16,mmh16)]  */
                                     /*      desktop and its monitors (on any change)   */
/* input, hub -> agent */
#define RKM_ENTER      0x10  /* x16,y16,mods                                      */
#define RKM_LEAVE      0x11  /* (none)  agent releases everything it holds down   */
#define RKM_MOVE       0x12  /* x16,y16  absolute, in agent pixels                */
#define RKM_MOVEREL    0x13  /* dx16,dy16 signed                                  */
#define RKM_BUTTON     0x14  /* button,down                                       */
#define RKM_WHEEL      0x15  /* dy16,dx16 signed, 120 units = one notch           */
#define RKM_KEY        0x16  /* usage,state,mods                                  */
#define RKM_RESET      0x17  /* [flags]  release everything                       */
#define RKM_RESET_DISPLAYS 0x01 /* flag: and turn its displays off and on (hub reset) */
#define RKM_PAD        0x18  /* pad,buttons16,lt,rt,lx16,ly16,rx16,ry16  whole    */
                             /* game controller state (RKM_CAP_PAD agents)        */
#define RKM_RUMBLE     0x19  /* A->H pad,strong16,weak16  0..65535 each           */
/* clipboard, both directions */
#define RKM_CLIP_BEGIN 0x20  /* format,total32                                    */
#define RKM_CLIP_DATA  0x21  /* bytes (<= RKM_CLIP_CHUNK)                         */
#define RKM_CLIP_END   0x22  /* (none)                                            */

#define RKM_BUDDY      0x30  /* H->A cmd,label..  show/hide the desk pet's window */
#define RKM_BUDDY_EVENT 0x31 /* A->H event  someone clicked it / called it        */
#define RKM_SOUND      0x32  /* H->A id16,rate32,chirp16,gap16,lo16,hi16,level,   */
                             /*      lead16,n,channel[n]  play a chirp sequence   */
#define RKM_SOUND_EVENT 0x33 /* A->H id16,status,channels,rate32                  */
#define RKM_BUDDY_LOOK 0x34  /* H->A the pet's eyes as numbers (rkm_look)  rev 2  */
#define RKM_BUDDY_GAZE 0x35  /* H->A mode,gx8,gy8,hold,blink  where to look  rev 2  */
#define RKM_BUDDY_SAY  0x36  /* H->A secs,text..  a caption for a while      rev 2  */

/* HELLO caps */
#define RKM_CAP_INPUT  0x01  /* can inject mouse and keyboard                     */
#define RKM_CAP_CLIP   0x02  /* can read and write the clipboard                  */
#define RKM_CAP_REL    0x04  /* pointer is relative only (hardware injectors)     */
#define RKM_CAP_KEYS   0x08  /* a hardware keyboard: gets ENTER, LEAVE, KEY and   */
                             /* RESET only; the machine's agent (if any) keeps    */
                             /* the pointer and clipboard                         */
#define RKM_CAP_BUDDY  0x10  /* can show the desk buddy window (RKM_BUDDY)        */
#define RKM_CAP_SOUND  0x20  /* can play test chirps on its speakers (RKM_SOUND)  */
#define RKM_CAP_PAD    0x40  /* plays a game controller (a console's pad port):   */
                             /* gets ENTER, LEAVE, PAD and RESET only             */

/* HELLO charset: encoding the agent uses for clipboard text */
#define RKM_CS_UTF8     0
#define RKM_CS_LATIN1   1
#define RKM_CS_CP1252   2
#define RKM_CS_MACROMAN 3

/* HELLO eol: line ending the agent's OS expects */
#define RKM_EOL_LF   0
#define RKM_EOL_CRLF 1
#define RKM_EOL_CR   2

/* WELCOME status */
#define RKM_OK            0
#define RKM_ERR_NAME      1  /* screen name not in the hub config */
#define RKM_ERR_VERSION   2

/* BUTTON numbers */
#define RKM_BTN_LEFT   1
#define RKM_BTN_RIGHT  2
#define RKM_BTN_MIDDLE 3
#define RKM_BTN_BACK   4
#define RKM_BTN_FWD    5

/* KEY state */
#define RKM_KEY_UP     0
#define RKM_KEY_DOWN   1
#define RKM_KEY_REPEAT 2

/* KEY / ENTER modifier byte: identical to the USB HID boot report */
#define RKM_MOD_LCTRL  0x01
#define RKM_MOD_LSHIFT 0x02
#define RKM_MOD_LALT   0x04
#define RKM_MOD_LGUI   0x08
#define RKM_MOD_RCTRL  0x10
#define RKM_MOD_RSHIFT 0x20
#define RKM_MOD_RALT   0x40
#define RKM_MOD_RGUI   0x80
#define RKM_MOD_CTRL   (RKM_MOD_LCTRL | RKM_MOD_RCTRL)
#define RKM_MOD_SHIFT  (RKM_MOD_LSHIFT | RKM_MOD_RSHIFT)
#define RKM_MOD_ALT    (RKM_MOD_LALT | RKM_MOD_RALT)
#define RKM_MOD_GUI    (RKM_MOD_LGUI | RKM_MOD_RGUI)

/* RKM_PAD buttons: an Xbox-style pad.  Sticks are signed, -32768..32767,
 * right and UP positive; triggers 0..255.  ENTER, LEAVE and RESET mean
 * "back to neutral" for a pad agent; PAD frames carry the whole state. */
#define RKM_PAD_A      0x0001
#define RKM_PAD_B      0x0002
#define RKM_PAD_X      0x0004
#define RKM_PAD_Y      0x0008
#define RKM_PAD_LB     0x0010
#define RKM_PAD_RB     0x0020
#define RKM_PAD_BACK   0x0040   /* View / Select */
#define RKM_PAD_START  0x0080   /* Menu */
#define RKM_PAD_GUIDE  0x0100   /* the Xbox button */
#define RKM_PAD_LS     0x0200   /* stick clicks */
#define RKM_PAD_RS     0x0400
#define RKM_PAD_UP     0x0800
#define RKM_PAD_DOWN   0x1000
#define RKM_PAD_LEFT   0x2000
#define RKM_PAD_RIGHT  0x4000
#define RKM_PAD_SHARE  0x8000
#define RKM_PAD_LEN    13

/* CLIP_BEGIN format */
#define RKM_CLIP_TEXT  1

/* RKM_BUDDY cmd and RKM_BUDDY_EVENT event */
#define RKM_BUDDY_HIDE   0
#define RKM_BUDDY_SHOW   1
#define RKM_BUDDY_CLICK  1   /* its window was clicked                          */
#define RKM_BUDDY_SUMMON 2   /* called from the machine (tray icon, hotkey)     */

/* RKM_BUDDY_GAZE mode.  gx, gy are signed, -100..100 across and down;
 * hold is in tenths of a second (0: until the next GAZE). */
#define RKM_GAZE_FREE    0   /* look about on its own (the default)             */
#define RKM_GAZE_AT      1   /* look along gx, gy                               */
#define RKM_GAZE_POINTER 2   /* watch the pointer on this machine               */

/* RKM_BUDDY_LOOK: how the eyes are drawn.  The fixed part is
 * RKM_LOOK_LEN bytes, then name_len and the name; an agent that never
 * gets one draws the defaults (rkm_look_default). */
#define RKM_LOOK_LEN   20
#define RKM_LOOK_COLOR 0x01  /* flags: use the colours (else black and white)   */
#define RKM_LOOK_PLAIN 0x02  /* flags: plain background (else the X root weave) */
typedef struct rkm_look {
    unsigned char shape;       /* eye height : width, tenths (15 = 1.5 times as tall) */
    unsigned char gap;         /* space between the eyes, % of an eye's half-width */
    unsigned char pupil;       /* pupil half-width, % of the eye's half-width */
    unsigned char pupil_shape; /* pupil height : width, tenths */
    unsigned char rim;         /* rim thickness, % of the eye's half-width */
    unsigned char flags;       /* RKM_LOOK_* */
    unsigned char lid;         /* how much of the eye the upper lid covers, % */
    unsigned char blink;       /* seconds between blinks, about; 0 = never */
    unsigned char mouth;       /* mouth line width, % of the window; 0 = none */
    signed char smile;         /* -100 (frown) .. 100 (smile); 0 = straight */
    unsigned char paper[3];    /* background, RGB */
    unsigned char eye[3];      /* the whites */
    unsigned char ink[3];      /* rims, pupils, lids, mouth, the label strip */
    char name[RKM_NAME_MAX + 1];   /* window title and default label; "" = none */
} rkm_look;

/* RKM_SOUND channels (WAVE order) and RKM_SOUND_EVENT status */
#define RKM_CH_FL 0
#define RKM_CH_FR 1
#define RKM_CH_FC 2
#define RKM_CH_LFE 3
#define RKM_CH_RL 4
#define RKM_CH_RR 5
#define RKM_SOUND_PLAYING 0  /* started; channels = how many the card opened    */
#define RKM_SOUND_DONE    1
#define RKM_SOUND_FAILED  2

/* ---- byte helpers ------------------------------------------------------ */
#define RKM_GET16(p)  ((unsigned int)(((unsigned int)(p)[0] << 8) | (p)[1]))
#define RKM_GETS16(p) ((int)(short)RKM_GET16(p))
#define RKM_GET32(p)  (((unsigned long)(p)[0] << 24) | ((unsigned long)(p)[1] << 16) | \
                       ((unsigned long)(p)[2] << 8) | (unsigned long)(p)[3])
#define RKM_PUT16(p, v) ((p)[0] = (unsigned char)(((v) >> 8) & 0xFF), \
                         (p)[1] = (unsigned char)((v) & 0xFF))
#define RKM_PUT32(p, v) ((p)[0] = (unsigned char)(((v) >> 24) & 0xFF), \
                         (p)[1] = (unsigned char)(((v) >> 16) & 0xFF), \
                         (p)[2] = (unsigned char)(((v) >> 8) & 0xFF),  \
                         (p)[3] = (unsigned char)((v) & 0xFF))

/* ---- stream parser ----------------------------------------------------- */
typedef struct rkm_parser {
    unsigned char buf[RKM_HDR + RKM_MAX_PAYLOAD];
    unsigned int  have;
} rkm_parser;

typedef void (*rkm_frame_fn)(void *ctx, int type,
                             const unsigned char *payload, unsigned int len);

void rkm_parser_init(rkm_parser *ps);

/* Feed received bytes; fn is called once per complete frame.
 * Returns 0, or -1 if the peer sent an oversize frame (drop the link). */
int rkm_feed(rkm_parser *ps, const unsigned char *data, unsigned long n,
             rkm_frame_fn fn, void *ctx);

/* Build a frame in out (needs RKM_HDR + len bytes).  Returns total size. */
unsigned int rkm_pack(unsigned char *out, int type,
                      const unsigned char *payload, unsigned int len);

/* Build a HELLO payload in out (needs 10 + RKM_NAME_MAX bytes).
 * Returns payload length. */
unsigned int rkm_hello(unsigned char *out, int caps, int charset, int eol,
                       unsigned int w, unsigned int h, unsigned int clipmax_kb,
                       const char *name);

/* The look the buddy window had before revision 2: tall black and white
 * eyes on the X root weave. */
void rkm_look_default(rkm_look *l);
/* Read a BUDDY_LOOK payload; a short one gives the default look */
void rkm_look_parse(rkm_look *l, const unsigned char *p, unsigned int len);
/* Build one (needs RKM_LOOK_LEN + 1 + RKM_NAME_MAX bytes); returns its length */
unsigned int rkm_look_pack(unsigned char *out, const rkm_look *l);

/* ---- USB HID keyboard usages used on the wire (page 0x07) -------------- */
#define HID_A 0x04
#define HID_Z 0x1D
#define HID_1 0x1E
#define HID_0 0x27
#define HID_ENTER 0x28
#define HID_ESC 0x29
#define HID_BACKSPACE 0x2A
#define HID_TAB 0x2B
#define HID_SPACE 0x2C
#define HID_MINUS 0x2D
#define HID_EQUAL 0x2E
#define HID_LBRACKET 0x2F
#define HID_RBRACKET 0x30
#define HID_BACKSLASH 0x31
#define HID_NONUS_HASH 0x32
#define HID_SEMICOLON 0x33
#define HID_APOSTROPHE 0x34
#define HID_GRAVE 0x35
#define HID_COMMA 0x36
#define HID_DOT 0x37
#define HID_SLASH 0x38
#define HID_CAPSLOCK 0x39
#define HID_F1 0x3A
#define HID_F12 0x45
#define HID_PRINTSCREEN 0x46
#define HID_SCROLLLOCK 0x47
#define HID_PAUSE 0x48
#define HID_INSERT 0x49
#define HID_HOME 0x4A
#define HID_PAGEUP 0x4B
#define HID_DELETE 0x4C
#define HID_END 0x4D
#define HID_PAGEDOWN 0x4E
#define HID_RIGHT 0x4F
#define HID_LEFT 0x50
#define HID_DOWN 0x51
#define HID_UP 0x52
#define HID_NUMLOCK 0x53
#define HID_KP_SLASH 0x54
#define HID_KP_STAR 0x55
#define HID_KP_MINUS 0x56
#define HID_KP_PLUS 0x57
#define HID_KP_ENTER 0x58
#define HID_KP_1 0x59
#define HID_KP_9 0x61
#define HID_KP_0 0x62
#define HID_KP_DOT 0x63
#define HID_NONUS_BACKSLASH 0x64
#define HID_MENU 0x65
#define HID_KP_EQUAL 0x67
#define HID_F13 0x68
#define HID_F24 0x73
#define HID_LCTRL 0xE0
#define HID_LSHIFT 0xE1
#define HID_LALT 0xE2
#define HID_LGUI 0xE3
#define HID_RCTRL 0xE4
#define HID_RSHIFT 0xE5
#define HID_RALT 0xE6
#define HID_RGUI 0xE7

#endif /* RKM_PROTO_H */

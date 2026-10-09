/*
 * RetroKM wire protocol, version 1.
 *
 * Strict C89, no libc dependencies, safe to call at interrupt time on
 * classic Mac OS.  See docs/PROTOCOL.md for the full description.
 *
 * Frame:  type:u8  length:u16 (big endian)  payload[length]
 */
#ifndef RKM_PROTO_H
#define RKM_PROTO_H

#define RKM_VERSION      1
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
#define RKM_SCREEN     0x05  /* A->H w16,h16  (resolution changed)                */
/* input, hub -> agent */
#define RKM_ENTER      0x10  /* x16,y16,mods                                      */
#define RKM_LEAVE      0x11  /* (none)  agent releases everything it holds down   */
#define RKM_MOVE       0x12  /* x16,y16  absolute, in agent pixels                */
#define RKM_MOVEREL    0x13  /* dx16,dy16 signed                                  */
#define RKM_BUTTON     0x14  /* button,down                                       */
#define RKM_WHEEL      0x15  /* dy16,dx16 signed, 120 units = one notch           */
#define RKM_KEY        0x16  /* usage,state,mods                                  */
#define RKM_RESET      0x17  /* (none)  release everything                        */
/* clipboard, both directions */
#define RKM_CLIP_BEGIN 0x20  /* format,total32                                    */
#define RKM_CLIP_DATA  0x21  /* bytes (<= RKM_CLIP_CHUNK)                         */
#define RKM_CLIP_END   0x22  /* (none)                                            */

/* HELLO caps */
#define RKM_CAP_INPUT  0x01  /* can inject mouse and keyboard                     */
#define RKM_CAP_CLIP   0x02  /* can read and write the clipboard                  */
#define RKM_CAP_REL    0x04  /* pointer is relative only (hardware injectors)     */
#define RKM_CAP_KEYS   0x08  /* a hardware keyboard: gets ENTER, LEAVE, KEY and   */
                             /* RESET only; the machine's agent (if any) keeps    */
                             /* the pointer and clipboard                         */

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

/* CLIP_BEGIN format */
#define RKM_CLIP_TEXT  1

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

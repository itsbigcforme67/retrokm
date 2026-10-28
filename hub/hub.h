/* RetroKM hub: shared declarations. */
#ifndef HUB_H
#define HUB_H

#include <stddef.h>
#include "../common/rkm_proto.h"

#define MAX_SCREENS   16
#define MAX_MONITORS  16
#define MAX_CONNS     48
#define MAX_HOTKEYS   48
#define MAX_REMAP     16
#define MAX_DEVICES   16
#define VDEV_MAX_IO   64          /* inputs or outputs on one video device */
#define MAX_VDEVS     8           /* switchers, matrices ... */
#define MAX_WIRES     32          /* cables between them */
#define MAX_AREAS     8
#define MAX_ACC       12          /* accessories: the keyboard + mouse, Claude's, game pads */
#define MAX_SEATS     8           /* people (or scripts) at the desk, each with a focus */
#define MAX_PADS      4           /* game pads open at once */
#define MAX_PLAYERS   4           /* pad players (dongles) per screen */

/* Kinds of accessory.  A new kind (a mic, a headset): add it here and to
 * acc_kinds[] in config.c; the panel builds its toggles from the layout.
 * Each accessory is linked to machines; on a link it can be kept (that
 * machine gets it out of focus too), or kept everywhere (keep_all).
 * If its devices ignore input until told they have it (pad dongles want
 * RKM_ENTER), a machine that gets it out of focus must be told so when its
 * device joins and when "keep" is turned on, not only on focus changes -
 * see pads_take() in main.c. */
enum { ACC_KBM, ACC_PAD, ACC_KINDS };
typedef struct { const char *id, *label; } AccKind;
extern const AccKind acc_kinds[ACC_KINDS];

struct Screen;
struct Seat;

enum { CONN_FREE = 0, CONN_AGENT, CONN_CTL, CONN_PANEL };

typedef struct Conn {
    int kind;
    int fd;
    int dead;                    /* closed; reaped by the main loop */
    char peer[64];
    long long last_rx_ms;
    unsigned char *out;          /* pending output */
    size_t out_len, out_cap;
    /* agent */
    rkm_parser ps;
    struct Screen *screen;       /* NULL until HELLO */
    int caps, charset, eol;
    unsigned clipmax_kb;
    unsigned char *clip_in;      /* clipboard being received */
    size_t clip_in_len, clip_in_total;
    int clip_in_active;
    /* control */
    char line[256];
    int line_len;
    /* panel */
    unsigned layout_sent;        /* hash of the last layout it was sent */
    int trace;                   /* gets every mouse motion ("trace on"), for tuning */
} Conn;

typedef struct Screen {
    char name[32];
    char label[48];              /* shown on the touch panel */
    char art[16];                /* which picture the panel draws */
    int w, h;                    /* pixels; from HELLO, or config for local */
    int local;                   /* injected by the hub itself through uinput */
    int vdev, vin;               /* its video goes into this device's input; vin 0 = none */
    double speed;                /* per-screen pointer speed multiplier */
    int min_move_ms;             /* motion rate limit */
    Conn *in;                    /* connection with RKM_CAP_INPUT */
    Conn *kbd;                   /* hardware keyboard (RKM_CAP_KEYS): takes the keys */
    Conn *clip;                  /* connection with RKM_CAP_CLIP */
    Conn *buddy;                 /* connection with RKM_CAP_BUDDY */
    Conn *sound;                 /* connection with RKM_CAP_SOUND */
    Conn *pads[MAX_PLAYERS];     /* pad players (RKM_CAP_PAD): the Xbox dongle(s) */
    char about[160];             /* how it is attached, for the panel (built by the hub) */
    long long last_input_ms;     /* the hub last sent it input (0 = never) */
    long long last_key_ms;       /* ... a key */
    struct Seat *clip_seat;      /* whose clipboard it holds ... */
    unsigned clip_gen;           /* ... and which generation of it */
    struct Seat *clip_owner;     /* the seat that came last: its clipboard is that seat's */
    struct Seat *mover;          /* the seat whose pointer it is drawing */
    int move_pending;
    int rel_dx, rel_dy;          /* pending motion for relative-only agents */
    long long last_move_ms;
    unsigned char remap_from[MAX_REMAP], remap_to[MAX_REMAP];
    int nremap;
    int area[MAX_AREAS][4];      /* visible rectangles x,y,w,h; none = all of it */
    int area_mon[MAX_AREAS];     /* only real while this monitor is shared; -1 always */
    int narea;
    int cfg_area[MAX_AREAS][4];  /* the config file's areas, before the agent reported its own */
    int cfg_area_mon[MAX_AREAS];
    int cfg_narea;
    int no_agent;                /* video only (a console): never takes input */
    long color;                  /* keyboard lighting 0xRRGGBB, -1 = from its picture */
    long config_color;           /* what the config file said (panel picks are saved apart) */
    struct Screen *via;          /* a console whose video goes through this box (the RetroTINK) */
    struct Screen *plugged;      /* a box: the console wired into it now (set from the panel) */
} Screen;

typedef struct Monitor {
    char name[32];
    int col, row;                /* position in the physical grid */
    Screen *fixed;               /* always shows this screen, or ... */
    int vdev, vout;              /* ... whatever reaches this device's output */
    Screen *cur;                 /* resolved: what it shows right now */
    Screen *shared;              /* the monitor's other input shows this machine */
    int shared_on;               /* ... and is the one selected on the monitor */
    int portrait;                /* drawn tall on the panel (turned on its side) */
    int aspect_w, aspect_h;      /* its shape, as it stands unturned (16:9, 4:3 ...) */
    int inches10;                /* screen diagonal in tenths of an inch, 0 = not said */
    int shape_set;               /* shape, size or turn picked on the panel (saved in the state file) */
    int capture;                 /* a capture card, not a screen on the desk */
    char label[48];              /* shown on the touch panel */
    char ddc[64];                /* DRM connector (or /dev/i2c-N) reaching it, for DDC/CI */
    int ddc_shared, ddc_switcher;   /* its input codes for the shared machine / switcher */
} Monitor;

/* A game pad's state, as on the wire (RKM_PAD). */
typedef struct PadState {
    unsigned buttons;            /* RKM_PAD_* */
    unsigned char lt, rt;        /* 0..255 */
    short lx, ly, rx, ry;        /* right and up positive */
} PadState;

/* Something you hold, linked to the machines it may drive: the keyboard +
 * mouse, a game pad.  Pads are remembered (state file) while unplugged. */
typedef struct Accessory {
    char name[32];               /* stable id: "keyboard", "pad-045e0b22-1a2b3c" */
    char label[64];
    char about[96];              /* how it is attached */
    int kind;                    /* ACC_* */
    int present;
    long color;                  /* its lines on the panel; -1 = from its kind */
    unsigned char player[MAX_SCREENS];   /* 0 = not linked; pads: player 1..4 there */
    unsigned char keep[MAX_SCREENS];     /* that machine gets it out of focus too */
    int keep_all;                /* ... every machine it is linked to does */
    PadState st;                 /* pads: as last read */
    int scripted;                /* the control port's pad: not saved, not on the panel */
    int seat;                    /* the seat it belongs to: its input follows that seat's focus */
} Accessory;

/* Someone at the desk (docs/SEATS.md): a person, or a script.  Each has
 * its own focus, pointer, held keys and clipboard.  [0] is the first seat:
 * commands that name no seat are its, and it gets a machine at start. */
typedef struct Seat {
    char name[32];
    char label[48];
    long color;                  /* 0xRRGGBB: its ring on the panel */
    int from_config;
    struct Screen *active;       /* the machine it drives, or NULL */
    struct Monitor *mon;         /* the monitor its pointer is on */
    double px, py;               /* its pointer, in that machine's pixels */
    double rel_fx, rel_fy;       /* fractional remainder for relative agents */
    unsigned char mods;          /* modifiers it holds, HID bit order */
    int buttons;                 /* mouse buttons it holds (bit per button) */
    int locked;                  /* edge switching off */
    int offline;                 /* went offline: no focus, input dropped (devices bring it back) */
    struct Screen *was;          /* ... where it was */
    int touched;                 /* any input yet; until then the first seat takes the first machine */
    long long lost_ms;           /* its machine's agent dropped: when */
    unsigned char swallowed[256];
    unsigned char down[256];     /* keys it holds down on its machine */
    double last_x[MAX_SCREENS], last_y[MAX_SCREENS];   /* where its pointer was on each */
    unsigned char have_last[MAX_SCREENS];
    unsigned char *clip;         /* its clipboard: UTF-8, LF line endings */
    size_t clip_len;
    unsigned clip_gen;
    struct Screen *clip_src;
} Seat;

/* A video device (docs/SWITCHING.md): a switcher with numbered inputs and
 * outputs that the hub can drive.  The driver's state lives here too, so
 * there can be several of each kind. */
enum { VDRV_EXTRON_SIS = 1, VDRV_TEXT };

typedef struct VDevice {
    char name[32];
    int driver;                  /* VDRV_* */
    char path[128];              /* serial device, or tcp:HOST:PORT; empty = not connected */
    char model[48];              /* the devices/<model>.conf it was read from */
    int baud, poll;              /* poll: seconds between re-reads, 0 = never */
    int inputs, outputs;
    char tie_cmd, read_cmd;      /* Extron SIS: '!' video+audio ...; '%' read */
    /* the text driver: commands with {in} and {out}, escapes as in C */
    char tie_fmt[96], untie_fmt[96], read_fmt[96];
    char reply_fmt[96];          /* a line that says a tie: "input {in} -> output {out}" */
    /* driver state */
    int fd;
    int connecting;              /* TCP: connect() not finished yet */
    int tie[VDEV_MAX_IO + 1];    /* tie[output] = input, 0 unknown */
    unsigned char watched[VDEV_MAX_IO + 1], need[VDEV_MAX_IO + 1];
    int pending_out;
    long long pending_since, last_poll, last_open_try;
    char lbuf[128];
    int llen;
} VDevice;

/* A cable from one device's output to another's input */
typedef struct Wire { int from_dev, from_out, to_dev, to_in; } Wire;

typedef struct Hotkey {
    unsigned char mods;          /* RKM_MOD_CTRL etc, side-insensitive */
    unsigned char usage;
    char cmd[96];
} Hotkey;

typedef struct Config {
    char listen[64];
    int port, ctl_port;
    int panel_port;              /* touch panel, on the LAN; 0 = off */
    char state_path[256];        /* monitor positions moved from the panel */
    char devices[MAX_DEVICES][128];
    int ndevices;                /* 0 = auto-detect */
    char pad[128];               /* game pad: "auto" (default), "none" or a path */
    int grab;
    double speed, accel;
    double wiggle;               /* mouse back-and-forth (counts in 0.4 s) that is a wiggle */
    int follow_tie;              /* active screen follows the monitor on a tie */
    int lights;                  /* colour the keyboard after the active machine */
    int lights_brightness;       /* percent */
    Screen screens[MAX_SCREENS];
    int nscreens;
    Monitor monitors[MAX_MONITORS];
    int nmonitors;
    VDevice vdevs[MAX_VDEVS];    /* video devices: switchers ... */
    int nvdevs;
    Wire wires[MAX_WIRES];
    int nwires;
    Hotkey hotkeys[MAX_HOTKEYS];
    int nhotkeys;
    Accessory acc[MAX_ACC];      /* [0] is the keyboard + mouse */
    int nacc;
    Seat seats[MAX_SEATS];
    int nseats;
} Config;

extern Config cfg;
extern int verbose;

/* main.c */
void logmsg(const char *fmt, ...);
long long now_ms(void);
void hub_motion(int dx, int dy);
void hub_button(int btn, int down);
void hub_wheel(int dy, int dx);
void hub_key(int evcode, int usage, int state);
void hub_pad(Accessory *a);             /* a pad's state changed (a->st) */
void hub_video_changed(void);       /* a device's ties changed */
int hub_routing(void);                  /* some screen is taking input */
Screen *screen_by_name(const char *name);
int screen_is_desktop(const Screen *s); /* the hub's own desktop (uinput, or an agent on this machine) */
Monitor *monitor_by_name(const char *name);

/* config.c */
int config_load(const char *path);
int config_save_state(void);
Accessory *acc_by_name(const char *name);
Accessory *acc_add(const char *name, int kind, const char *label);   /* known, or new with default links */
int acc_kind(const char *id);           /* -1 if unknown */
Seat *seat_by_name(const char *name);
Seat *seat_add(const char *name, long color);   /* NULL if full */

/* keymap.c */
int evdev_to_hid(int code);
int hid_to_evdev(int usage);
int key_by_name(const char *name);      /* returns HID usage or -1 */
int parse_combo(const char *s, unsigned char *mods, unsigned char *usage);

/* evdev.c */
int input_init(void);
int input_fds(int *fds, int max);
void input_readable(int fd);
void input_tick(void);

/* pad.c: game pads (several), each one an Accessory */
int pad_fds(int *fds, int max);
void pad_readable(int fd);
void pad_tick(void);                    /* hotplug, and grab each pad unless the desktop reads it */
void pad_rumble(Accessory *a, unsigned strong, unsigned weak);
int hub_pad_on_desktop(const Accessory *a);   /* main.c: the hub's own desktop is a target */

/* uinput.c */
int uinput_open(const char *name);
void uinput_abs(int x, int y, int w, int h);
void uinput_rel(int dx, int dy);
void uinput_key(int evcode, int value);
void uinput_wheel(int dy, int dx);
void uinput_release_all(void);

/* extron.c: the Extron SIS driver, one VDevice each */
void extron_init(VDevice *d);
void extron_readable(VDevice *d);
void extron_tick(VDevice *d);
void extron_tie(VDevice *d, int input, int output);   /* input 0 = disconnect */

/* cmdtext.c: the "text" driver, any box that takes text commands over
 * serial or TCP (devices/) */
void cmdtext_init(VDevice *d);
void cmdtext_readable(VDevice *d);
void cmdtext_tick(VDevice *d);
int cmdtext_tie(VDevice *d, int input, int output);   /* -1: it cannot (untie, or no tie command) */
int cmdtext_match(const char *fmt, const char *line, int *in, int *out);

/* video.c: devices, wires and routes (docs/SWITCHING.md) */
int vdev_open(VDevice *d);                     /* serial path or tcp:HOST:PORT; -1 failed */
void vdev_close(VDevice *d);
void vdev_write(VDevice *d, const char *s);    /* closes it on a write error */
void vdev_read(VDevice *d, void (*line)(VDevice *d, char *s));   /* calls line() per line */
void vdev_keep_open(VDevice *d, void (*opened)(VDevice *d));     /* reopen; finish a connect */
VDevice *vdev_by_name(const char *name);
int video_init(void);
int video_fds(int *fds, int *devs, int max);   /* to poll, and whose they are */
void video_readable(int dev);
void video_tick(void);
int video_online(int dev);
int video_input_for(int dev, int out);         /* 0 = nothing or unknown */
int video_tie(int dev, int in, int out);        /* -1: the device cannot */
int video_any(void);                           /* any device configured */
int video_all_online(void);
/* What reaches dev's output: follows ties and wires back to an input no
 * wire feeds.  Returns 1 and that input, or 0. */
int video_source(int dev, int out, int *sdev, int *sin);
/* Put s's video on m: finds a path through the devices and ties it.
 * 0 = done; -1 with why in err. */
int video_route(const Screen *s, const Monitor *m, char *err, size_t n);
int video_unroute(const Monitor *m, char *err, size_t n);
const char *video_driver_name(int driver);
const char *video_port_name(int dev, int port, int out, char *buf, size_t n);   /* "mvx.in3" */

/* ddc.c */
int ddc_init(void);
int ddc_fd(void);                       /* readable when there is news; -1 unused */
void ddc_set_input(Monitor *m, int input);
int ddc_state(const Monitor *m, int *input);   /* 1 answering, 0 silent, -1 none */
void ddc_readable(void (*changed)(Monitor *m, int ok, int input));

/* lights.c */
void lights_set(unsigned long rgb);     /* 0xFFFFFFFF: the keyboard's own lighting */
void lights_tick(void);
unsigned long lights_color(const Screen *s);

/* text.c */
unsigned char *text_to_utf8(const unsigned char *in, size_t n, int charset, size_t *outn);
unsigned char *text_from_utf8(const unsigned char *in, size_t n, int charset, int eol, size_t *outn);

#endif

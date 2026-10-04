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
#define EXTRON_MAX_IO 64
#define MAX_AREAS     8

struct Screen;

enum { CONN_FREE = 0, CONN_AGENT, CONN_CTL };

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
} Conn;

typedef struct Screen {
    char name[32];
    int w, h;                    /* pixels; from HELLO, or config for local */
    int local;                   /* injected by the hub itself through uinput */
    int extron_input;            /* 0 = not routed through the switcher */
    double speed;                /* per-screen pointer speed multiplier */
    int min_move_ms;             /* motion rate limit */
    Conn *in;                    /* connection with RKM_CAP_INPUT */
    Conn *clip;                  /* connection with RKM_CAP_CLIP */
    unsigned clip_gen;           /* hub clipboard generation this screen has */
    double last_x, last_y;       /* where the pointer was when we left */
    int have_last;
    int move_pending;
    int rel_dx, rel_dy;          /* pending motion for relative-only agents */
    long long last_move_ms;
    unsigned char remap_from[MAX_REMAP], remap_to[MAX_REMAP];
    int nremap;
    int area[MAX_AREAS][4];      /* visible rectangles x,y,w,h; none = all of it */
    int narea;
} Screen;

typedef struct Monitor {
    char name[32];
    int col, row;                /* position in the physical grid */
    Screen *fixed;               /* always shows this screen, or ... */
    int extron_output;           /* ... whatever is tied to this output */
    Screen *cur;                 /* resolved: what it shows right now */
} Monitor;

typedef struct Hotkey {
    unsigned char mods;          /* RKM_MOD_CTRL etc, side-insensitive */
    unsigned char usage;
    char cmd[96];
} Hotkey;

typedef struct Config {
    char listen[64];
    int port, ctl_port;
    char devices[MAX_DEVICES][128];
    int ndevices;                /* 0 = auto-detect */
    int grab;
    double speed, accel;
    int follow_tie;              /* active screen follows the monitor on a tie */
    char extron_dev[128];
    int extron_baud, extron_poll;
    char extron_tie_cmd, extron_read_cmd;
    Screen screens[MAX_SCREENS];
    int nscreens;
    Monitor monitors[MAX_MONITORS];
    int nmonitors;
    Hotkey hotkeys[MAX_HOTKEYS];
    int nhotkeys;
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
void hub_extron_changed(void);
int hub_routing(void);                  /* some screen is taking input */
Screen *screen_by_name(const char *name);
Monitor *monitor_by_name(const char *name);

/* config.c */
int config_load(const char *path);

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

/* uinput.c */
int uinput_open(const char *name);
void uinput_abs(int x, int y, int w, int h);
void uinput_rel(int dx, int dy);
void uinput_key(int evcode, int value);
void uinput_wheel(int dy, int dx);
void uinput_release_all(void);

/* extron.c */
int extron_init(void);
int extron_fd(void);
void extron_readable(void);
void extron_tick(void);
int extron_input_for(int output);       /* 0 = unknown */
void extron_tie(int input, int output);
void extron_watch(int output);

/* text.c */
unsigned char *text_to_utf8(const unsigned char *in, size_t n, int charset, size_t *outn);
unsigned char *text_from_utf8(const unsigned char *in, size_t n, int charset, int eol, size_t *outn);

#endif

/* Linux evdev key codes <-> USB HID usages, plus key names for the config. */
#include <string.h>
#include <strings.h>
#include <linux/input-event-codes.h>
#include "hub.h"

static const struct { int ev; int hid; const char *name; } keys[] = {
    { KEY_A, 0x04, "a" }, { KEY_B, 0x05, "b" }, { KEY_C, 0x06, "c" }, { KEY_D, 0x07, "d" },
    { KEY_E, 0x08, "e" }, { KEY_F, 0x09, "f" }, { KEY_G, 0x0A, "g" }, { KEY_H, 0x0B, "h" },
    { KEY_I, 0x0C, "i" }, { KEY_J, 0x0D, "j" }, { KEY_K, 0x0E, "k" }, { KEY_L, 0x0F, "l" },
    { KEY_M, 0x10, "m" }, { KEY_N, 0x11, "n" }, { KEY_O, 0x12, "o" }, { KEY_P, 0x13, "p" },
    { KEY_Q, 0x14, "q" }, { KEY_R, 0x15, "r" }, { KEY_S, 0x16, "s" }, { KEY_T, 0x17, "t" },
    { KEY_U, 0x18, "u" }, { KEY_V, 0x19, "v" }, { KEY_W, 0x1A, "w" }, { KEY_X, 0x1B, "x" },
    { KEY_Y, 0x1C, "y" }, { KEY_Z, 0x1D, "z" },
    { KEY_1, 0x1E, "1" }, { KEY_2, 0x1F, "2" }, { KEY_3, 0x20, "3" }, { KEY_4, 0x21, "4" },
    { KEY_5, 0x22, "5" }, { KEY_6, 0x23, "6" }, { KEY_7, 0x24, "7" }, { KEY_8, 0x25, "8" },
    { KEY_9, 0x26, "9" }, { KEY_0, 0x27, "0" },
    { KEY_ENTER, 0x28, "enter" }, { KEY_ESC, 0x29, "esc" }, { KEY_BACKSPACE, 0x2A, "backspace" },
    { KEY_TAB, 0x2B, "tab" }, { KEY_SPACE, 0x2C, "space" }, { KEY_MINUS, 0x2D, "minus" },
    { KEY_EQUAL, 0x2E, "equal" }, { KEY_LEFTBRACE, 0x2F, "leftbrace" },
    { KEY_RIGHTBRACE, 0x30, "rightbrace" }, { KEY_BACKSLASH, 0x31, "backslash" },
    { KEY_SEMICOLON, 0x33, "semicolon" }, { KEY_APOSTROPHE, 0x34, "apostrophe" },
    { KEY_GRAVE, 0x35, "grave" }, { KEY_COMMA, 0x36, "comma" }, { KEY_DOT, 0x37, "dot" },
    { KEY_SLASH, 0x38, "slash" }, { KEY_CAPSLOCK, 0x39, "capslock" },
    { KEY_F1, 0x3A, "f1" }, { KEY_F2, 0x3B, "f2" }, { KEY_F3, 0x3C, "f3" }, { KEY_F4, 0x3D, "f4" },
    { KEY_F5, 0x3E, "f5" }, { KEY_F6, 0x3F, "f6" }, { KEY_F7, 0x40, "f7" }, { KEY_F8, 0x41, "f8" },
    { KEY_F9, 0x42, "f9" }, { KEY_F10, 0x43, "f10" }, { KEY_F11, 0x44, "f11" }, { KEY_F12, 0x45, "f12" },
    { KEY_SYSRQ, 0x46, "printscreen" }, { KEY_SCROLLLOCK, 0x47, "scrolllock" },
    { KEY_PAUSE, 0x48, "pause" }, { KEY_INSERT, 0x49, "insert" }, { KEY_HOME, 0x4A, "home" },
    { KEY_PAGEUP, 0x4B, "pageup" }, { KEY_DELETE, 0x4C, "delete" }, { KEY_END, 0x4D, "end" },
    { KEY_PAGEDOWN, 0x4E, "pagedown" }, { KEY_RIGHT, 0x4F, "right" }, { KEY_LEFT, 0x50, "left" },
    { KEY_DOWN, 0x51, "down" }, { KEY_UP, 0x52, "up" }, { KEY_NUMLOCK, 0x53, "numlock" },
    { KEY_KPSLASH, 0x54, "kpslash" }, { KEY_KPASTERISK, 0x55, "kpasterisk" },
    { KEY_KPMINUS, 0x56, "kpminus" }, { KEY_KPPLUS, 0x57, "kpplus" }, { KEY_KPENTER, 0x58, "kpenter" },
    { KEY_KP1, 0x59, "kp1" }, { KEY_KP2, 0x5A, "kp2" }, { KEY_KP3, 0x5B, "kp3" },
    { KEY_KP4, 0x5C, "kp4" }, { KEY_KP5, 0x5D, "kp5" }, { KEY_KP6, 0x5E, "kp6" },
    { KEY_KP7, 0x5F, "kp7" }, { KEY_KP8, 0x60, "kp8" }, { KEY_KP9, 0x61, "kp9" },
    { KEY_KP0, 0x62, "kp0" }, { KEY_KPDOT, 0x63, "kpdot" }, { KEY_102ND, 0x64, "102nd" },
    { KEY_COMPOSE, 0x65, "menu" }, { KEY_KPEQUAL, 0x67, "kpequal" },
    { KEY_F13, 0x68, "f13" }, { KEY_F14, 0x69, "f14" }, { KEY_F15, 0x6A, "f15" },
    { KEY_F16, 0x6B, "f16" }, { KEY_F17, 0x6C, "f17" }, { KEY_F18, 0x6D, "f18" },
    { KEY_F19, 0x6E, "f19" }, { KEY_F20, 0x6F, "f20" }, { KEY_F21, 0x70, "f21" },
    { KEY_F22, 0x71, "f22" }, { KEY_F23, 0x72, "f23" }, { KEY_F24, 0x73, "f24" },
    { KEY_LEFTCTRL, 0xE0, "leftctrl" }, { KEY_LEFTSHIFT, 0xE1, "leftshift" },
    { KEY_LEFTALT, 0xE2, "leftalt" }, { KEY_LEFTMETA, 0xE3, "leftmeta" },
    { KEY_RIGHTCTRL, 0xE4, "rightctrl" }, { KEY_RIGHTSHIFT, 0xE5, "rightshift" },
    { KEY_RIGHTALT, 0xE6, "rightalt" }, { KEY_RIGHTMETA, 0xE7, "rightmeta" },
};
#define NKEYS ((int)(sizeof keys / sizeof keys[0]))

int evdev_to_hid(int code)
{
    int i;
    for (i = 0; i < NKEYS; i++)
        if (keys[i].ev == code) return keys[i].hid;
    return 0;
}

int hid_to_evdev(int usage)
{
    int i;
    for (i = 0; i < NKEYS; i++)
        if (keys[i].hid == usage) return keys[i].ev;
    return 0;
}

int key_by_name(const char *name)
{
    int i;
    for (i = 0; i < NKEYS; i++)
        if (strcasecmp(keys[i].name, name) == 0) return keys[i].hid;
    return -1;
}

/* "ctrl+alt+f1" -> modifier mask (side-insensitive) + final key usage. */
int parse_combo(const char *s, unsigned char *mods, unsigned char *usage)
{
    char buf[64], *tok, *save = NULL;
    int k;

    strncpy(buf, s, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    *mods = 0;
    *usage = 0;
    for (tok = strtok_r(buf, "+ ", &save); tok; tok = strtok_r(NULL, "+ ", &save)) {
        if (!strcasecmp(tok, "ctrl")) *mods |= RKM_MOD_CTRL;
        else if (!strcasecmp(tok, "shift")) *mods |= RKM_MOD_SHIFT;
        else if (!strcasecmp(tok, "alt")) *mods |= RKM_MOD_ALT;
        else if (!strcasecmp(tok, "meta") || !strcasecmp(tok, "super")) *mods |= RKM_MOD_GUI;
        else if ((k = key_by_name(tok)) >= 0) *usage = (unsigned char)k;
        else return -1;
    }
    return *usage ? 0 : -1;
}

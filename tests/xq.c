/* Test helper: inspect or drive an X display.
 *   xq pointer            print "x y buttonmask"
 *   xq keys               print keysym names of keys currently held
 *   xq getclip [primary]  print CLIPBOARD (or PRIMARY) as UTF-8
 *   xq setclip TEXT       own CLIPBOARD with TEXT until someone else takes it */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

int main(int argc, char **argv)
{
    Display *d = XOpenDisplay(NULL);
    Window w, r, c;
    int rx, ry, wx, wy, i;
    unsigned int mask;
    Atom clip, utf8, prop, targets;

    if (!d || argc < 2) return 2;
    w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1, 1, 0, 0, 0);
    clip = XInternAtom(d, "CLIPBOARD", False);
    utf8 = XInternAtom(d, "UTF8_STRING", False);
    targets = XInternAtom(d, "TARGETS", False);
    prop = XInternAtom(d, "XQ", False);

    if (!strcmp(argv[1], "pointer")) {
        XQueryPointer(d, DefaultRootWindow(d), &r, &c, &rx, &ry, &wx, &wy, &mask);
        printf("%d %d %u\n", rx, ry, mask >> 8);
    } else if (!strcmp(argv[1], "keys")) {
        char km[32];
        XQueryKeymap(d, km);
        for (i = 0; i < 256; i++)
            if (km[i / 8] & (1 << (i % 8))) {
                int per;
                KeySym *ks = XGetKeyboardMapping(d, (KeyCode)i, 1, &per);
                printf("%s ", XKeysymToString(ks[0]));
            }
        printf("\n");
    } else if (!strcmp(argv[1], "getclip")) {
        XEvent ev;
        Atom type, sel = clip, tgt = (argc > 2 && !strcmp(argv[2], "latin1")) ? XA_STRING : utf8;
        int fmt;
        unsigned long n, after;
        unsigned char *data = NULL;
        XConvertSelection(d, sel, tgt, prop, w, CurrentTime);
        for (i = 0; i < 200; i++) {
            if (XCheckTypedWindowEvent(d, w, SelectionNotify, &ev)) break;
            usleep(10000);
        }
        if (i == 200 || ev.xselection.property == None) { printf("<none>\n"); return 1; }
        XGetWindowProperty(d, w, prop, 0, 1 << 22, True, AnyPropertyType, &type, &fmt, &n, &after, &data);
        fwrite(data, 1, n, stdout);
    } else if (!strcmp(argv[1], "setclip") && argc > 2) {
        XSetSelectionOwner(d, clip, w, CurrentTime);
        XFlush(d);
        for (;;) {
            XEvent ev, out;
            XNextEvent(d, &ev);
            if (ev.type == SelectionClear) return 0;
            if (ev.type != SelectionRequest) continue;
            memset(&out, 0, sizeof out);
            out.xselection.type = SelectionNotify;
            out.xselection.requestor = ev.xselectionrequest.requestor;
            out.xselection.selection = ev.xselectionrequest.selection;
            out.xselection.target = ev.xselectionrequest.target;
            out.xselection.time = ev.xselectionrequest.time;
            out.xselection.property = None;
            if (ev.xselectionrequest.target == utf8) {
                XChangeProperty(d, out.xselection.requestor, ev.xselectionrequest.property, utf8, 8,
                                PropModeReplace, (unsigned char *)argv[2], (int)strlen(argv[2]));
                out.xselection.property = ev.xselectionrequest.property;
            } else if (ev.xselectionrequest.target == targets) {
                Atom l[2];
                l[0] = targets; l[1] = utf8;
                XChangeProperty(d, out.xselection.requestor, ev.xselectionrequest.property, XA_ATOM, 32,
                                PropModeReplace, (unsigned char *)l, 2);
                out.xselection.property = ev.xselectionrequest.property;
            }
            XSendEvent(d, out.xselection.requestor, False, 0, &out);
        }
    }
    return 0;
}

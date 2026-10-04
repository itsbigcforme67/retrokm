/*
 * RetroKM input agent for modern Linux, for when the hub is a separate box.
 * Injects through /dev/uinput, so it works under Wayland, X11 and the
 * console alike.  Pair it with rkm_clip.py (or rkm-x11 -c) for the clipboard.
 *
 *   rkm-uinput [-n name] [-p port] [-g WIDTHxHEIGHT] hub-host
 */
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/input-event-codes.h>
#include "../../hub/hub.h"

static int sock = -1, width, height;

static void send_frame(int type, const unsigned char *payload, unsigned len)
{
    unsigned char buf[RKM_HDR + RKM_MAX_PAYLOAD];
    unsigned n = rkm_pack(buf, type, payload, len);
    if (sock >= 0 && send(sock, buf, n, MSG_NOSIGNAL) != (ssize_t)n) { close(sock); sock = -1; }
}

static void on_frame(void *ctx, int type, const unsigned char *p, unsigned len)
{
    static const int btn[] = { 0, BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA };
    int b;
    (void)ctx;

    switch (type) {
    case RKM_WELCOME:
        if (len >= 2 && p[1] != RKM_OK) {
            fprintf(stderr, "rkm-uinput: hub refused us (unknown screen name or version)\n");
            exit(1);
        }
        break;
    case RKM_PING: send_frame(RKM_PONG, NULL, 0); break;
    case RKM_ENTER:
        if (len < 5) break;
        uinput_abs((int)RKM_GET16(p), (int)RKM_GET16(p + 2), width, height);
        for (b = 0; b < 8; b++)
            if (p[4] & (1 << b)) uinput_key(hid_to_evdev(HID_LCTRL + b), 1);
        break;
    case RKM_LEAVE:
    case RKM_RESET: uinput_release_all(); break;
    case RKM_MOVE: if (len >= 4) uinput_abs((int)RKM_GET16(p), (int)RKM_GET16(p + 2), width, height); break;
    case RKM_MOVEREL: if (len >= 4) uinput_rel(RKM_GETS16(p), RKM_GETS16(p + 2)); break;
    case RKM_BUTTON: if (len >= 2 && p[0] >= 1 && p[0] <= 5) uinput_key(btn[p[0]], p[1] != 0); break;
    case RKM_WHEEL: if (len >= 4) uinput_wheel(RKM_GETS16(p), RKM_GETS16(p + 2)); break;
    case RKM_KEY: if (len >= 2) uinput_key(hid_to_evdev(p[0]), p[1]); break;
    }
}

static int hub_connect(const char *host, int port)
{
    struct sockaddr_in sa;
    struct hostent *he = gethostbyname(host);
    int one = 1;

    if (!he) return -1;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof sa.sin_addr);
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0 || connect(sock, (struct sockaddr *)&sa, sizeof sa) < 0) {
        if (sock >= 0) close(sock);
        sock = -1;
        return -1;
    }
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return 0;
}

int main(int argc, char **argv)
{
    const char *host = NULL;
    char name[RKM_NAME_MAX + 1] = "";
    int port = RKM_PORT, i;
    FILE *f;

    gethostname(name, sizeof name - 1);
    if ((f = fopen("/sys/class/graphics/fb0/virtual_size", "r"))) {
        if (fscanf(f, "%d,%d", &width, &height) != 2) width = height = 0;
        fclose(f);
    }
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) snprintf(name, sizeof name, "%s", argv[++i]);
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-g") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &width, &height);
        else if (argv[i][0] != '-') host = argv[i];
    }
    if (!host || width < 2 || height < 2) {
        fprintf(stderr, "usage: rkm-uinput [-n name] [-p port] -g WIDTHxHEIGHT hub-host\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    if (uinput_open("RetroKM") < 0) return 1;

    for (;;) {
        unsigned char hello[10 + RKM_NAME_MAX], buf[2048];
        rkm_parser ps;
        ssize_t n;

        if (hub_connect(host, port) < 0) { sleep(3); continue; }
        fprintf(stderr, "rkm-uinput: connected to %s as \"%s\" (%dx%d)\n", host, name, width, height);
        rkm_parser_init(&ps);
        send_frame(RKM_HELLO, hello, rkm_hello(hello, RKM_CAP_INPUT, RKM_CS_UTF8, RKM_EOL_LF,
                                               (unsigned)width, (unsigned)height, 0, name));
        while (sock >= 0 && (n = recv(sock, buf, sizeof buf, 0)) > 0)
            if (rkm_feed(&ps, buf, (unsigned long)n, on_frame, NULL) < 0) break;
        uinput_release_all();
        if (sock >= 0) close(sock);
        sock = -1;
        sleep(3);
    }
}

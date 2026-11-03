/* RetroKM wire protocol helpers.  Strict C89, no libc. */
#include "rkm_proto.h"

void rkm_parser_init(rkm_parser *ps)
{
    ps->have = 0;
}

int rkm_feed(rkm_parser *ps, const unsigned char *data, unsigned long n,
             rkm_frame_fn fn, void *ctx)
{
    while (n > 0) {
        unsigned int need;

        if (ps->have < RKM_HDR) {
            ps->buf[ps->have++] = *data++;
            n--;
            if (ps->have < RKM_HDR)
                continue;
        }
        need = RKM_GET16(ps->buf + 1);
        if (need > RKM_MAX_PAYLOAD)
            return -1;
        while (n > 0 && ps->have < RKM_HDR + need) {
            ps->buf[ps->have++] = *data++;
            n--;
        }
        if (ps->have == RKM_HDR + need) {
            ps->have = 0;
            fn(ctx, (int)ps->buf[0], ps->buf + RKM_HDR, need);
        }
    }
    return 0;
}

unsigned int rkm_pack(unsigned char *out, int type,
                      const unsigned char *payload, unsigned int len)
{
    unsigned int i;

    out[0] = (unsigned char)type;
    RKM_PUT16(out + 1, len);
    for (i = 0; i < len; i++)
        out[RKM_HDR + i] = payload[i];
    return RKM_HDR + len;
}

unsigned int rkm_hello(unsigned char *out, int caps, int charset, int eol,
                       unsigned int w, unsigned int h, unsigned int clipmax_kb,
                       const char *name)
{
    unsigned int n = 0;

    out[0] = RKM_VERSION;
    out[1] = (unsigned char)caps;
    out[2] = (unsigned char)charset;
    out[3] = (unsigned char)eol;
    RKM_PUT16(out + 4, w);
    RKM_PUT16(out + 6, h);
    RKM_PUT16(out + 8, clipmax_kb);
    while (name[n] != 0 && n < RKM_NAME_MAX) {
        out[10 + n] = (unsigned char)name[n];
        n++;
    }
    return 10 + n;
}

/* ---- the buddy window's look (RKM_BUDDY_LOOK) --------------------------- */

void rkm_look_default(rkm_look *l)
{
    int i;
    l->shape = 15;
    l->gap = 20;
    l->pupil = 24;
    l->pupil_shape = 12;
    l->rim = 16;
    l->flags = 0;
    l->lid = 0;
    l->blink = 0;
    l->mouth = 0;
    l->smile = 0;
    for (i = 0; i < 3; i++) {
        l->paper[i] = 0xFF;
        l->eye[i] = 0xFF;
        l->ink[i] = 0;
    }
    l->name[0] = 0;
}

void rkm_look_parse(rkm_look *l, const unsigned char *p, unsigned int len)
{
    unsigned int i, n;
    rkm_look_default(l);
    if (len < RKM_LOOK_LEN) return;
    if (p[0]) l->shape = p[0];
    l->gap = p[1];
    if (p[2]) l->pupil = p[2];
    if (p[3]) l->pupil_shape = p[3];
    l->rim = p[4];
    l->flags = p[5];
    l->lid = p[6] > 90 ? 90 : p[6];
    l->blink = p[7];
    l->mouth = p[8] > 90 ? 90 : p[8];
    l->smile = (signed char)p[9];
    for (i = 0; i < 3; i++) {
        l->paper[i] = p[10 + i];
        l->eye[i] = p[13 + i];
        l->ink[i] = p[16 + i];
    }
    n = p[19];
    if (n > RKM_NAME_MAX) n = RKM_NAME_MAX;
    if (n > len - RKM_LOOK_LEN) n = len - RKM_LOOK_LEN;
    for (i = 0; i < n; i++) l->name[i] = (char)p[RKM_LOOK_LEN + i];
    l->name[n] = 0;
}

unsigned int rkm_look_pack(unsigned char *out, const rkm_look *l)
{
    unsigned int i, n = 0;
    out[0] = l->shape;
    out[1] = l->gap;
    out[2] = l->pupil;
    out[3] = l->pupil_shape;
    out[4] = l->rim;
    out[5] = l->flags;
    out[6] = l->lid;
    out[7] = l->blink;
    out[8] = l->mouth;
    out[9] = (unsigned char)l->smile;
    for (i = 0; i < 3; i++) {
        out[10 + i] = l->paper[i];
        out[13 + i] = l->eye[i];
        out[16 + i] = l->ink[i];
    }
    while (l->name[n] != 0 && n < RKM_NAME_MAX) {
        out[RKM_LOOK_LEN + n] = (unsigned char)l->name[n];
        n++;
    }
    out[19] = (unsigned char)n;
    return RKM_LOOK_LEN + n;
}

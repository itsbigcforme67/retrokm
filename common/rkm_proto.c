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

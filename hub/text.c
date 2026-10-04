/* Clipboard text conversion.  The hub keeps text as UTF-8 with LF line
 * endings and converts at the edge, so 68k and Win9x agents stay trivial. */
#include <stdlib.h>
#include <string.h>
#include "hub.h"
#include "text_tables.h"

static const unsigned short *table_for(int charset)
{
    if (charset == RKM_CS_CP1252) return tbl_cp1252;
    if (charset == RKM_CS_MACROMAN) return tbl_macroman;
    return NULL;
}

static size_t put_utf8(unsigned char *o, unsigned long cp)
{
    if (cp < 0x80) { o[0] = (unsigned char)cp; return 1; }
    if (cp < 0x800) {
        o[0] = (unsigned char)(0xC0 | (cp >> 6));
        o[1] = (unsigned char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (unsigned char)(0xE0 | (cp >> 12));
        o[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (unsigned char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (unsigned char)(0xF0 | (cp >> 18));
    o[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (unsigned char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Decode one UTF-8 sequence; invalid input yields U+FFFD and skips a byte. */
static size_t get_utf8(const unsigned char *p, size_t n, unsigned long *cp)
{
    unsigned char c = p[0];
    size_t len, i;
    unsigned long v;

    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0) { len = 2; v = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { len = 3; v = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { len = 4; v = c & 0x07; }
    else { *cp = 0xFFFD; return 1; }
    if (len > n) { *cp = 0xFFFD; return 1; }
    for (i = 1; i < len; i++) {
        if ((p[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        v = (v << 6) | (p[i] & 0x3F);
    }
    *cp = v;
    return len;
}

unsigned char *text_to_utf8(const unsigned char *in, size_t n, int charset, size_t *outn)
{
    const unsigned short *tbl = table_for(charset);
    unsigned char *out = malloc(n * 3 + 1);
    size_t i, o = 0;

    if (!out) return NULL;
    for (i = 0; i < n; i++) {
        unsigned char c = in[i];
        if (c == '\r') {                       /* CRLF or lone CR -> LF */
            if (i + 1 < n && in[i + 1] == '\n') i++;
            out[o++] = '\n';
        } else if (c < 0x80 || charset == RKM_CS_UTF8) {
            out[o++] = c;
        } else if (tbl) {
            unsigned long cp = tbl[c - 0x80];
            o += put_utf8(out + o, cp ? cp : (unsigned long)c);
        } else {
            o += put_utf8(out + o, c);         /* Latin-1 */
        }
    }
    out[o] = 0;
    *outn = o;
    return out;
}

/* ASCII stand-ins for common characters a legacy charset cannot hold. */
static const char *fallback(unsigned long cp)
{
    switch (cp) {
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: return "'";
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: return "\"";
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2212: return "-";
    case 0x2026: return "...";
    case 0x2022: case 0x00B7: return "*";
    case 0x00A0: case 0x2002: case 0x2003: case 0x2009: case 0x202F: return " ";
    case 0x200B: case 0xFEFF: return "";
    case 0x2192: return "->";
    case 0x2190: return "<-";
    case 0x20AC: return "EUR";
    case 0x2122: return "(TM)";
    }
    return "?";
}

unsigned char *text_from_utf8(const unsigned char *in, size_t n, int charset, int eol, size_t *outn)
{
    const unsigned short *tbl = table_for(charset);
    unsigned char *out = malloc(n * 4 + 1);    /* worst case: fallbacks, CRLF */
    size_t i = 0, o = 0;

    if (!out) return NULL;
    while (i < n) {
        unsigned long cp;
        size_t len = get_utf8(in + i, n - i, &cp);

        if (cp == '\n') {
            if (eol == RKM_EOL_CRLF) { out[o++] = '\r'; out[o++] = '\n'; }
            else if (eol == RKM_EOL_CR) out[o++] = '\r';
            else out[o++] = '\n';
        } else if (charset == RKM_CS_UTF8) {
            memcpy(out + o, in + i, len);
            o += len;
        } else if (cp < 0x80) {
            out[o++] = (unsigned char)cp;
        } else {
            int found = -1, k;
            if (tbl) {
                for (k = 0; k < 128; k++)
                    if (tbl[k] == cp) { found = 0x80 + k; break; }
            } else if (cp >= 0xA0 && cp < 0x100) {
                found = (int)cp;
            }
            if (found >= 0) {
                out[o++] = (unsigned char)found;
            } else {
                const char *f = fallback(cp);
                size_t fl = strlen(f);
                memcpy(out + o, f, fl);
                o += fl;
            }
        }
        i += len;
    }
    out[o] = 0;
    *outn = o;
    return out;
}

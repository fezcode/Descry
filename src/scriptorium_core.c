#include "scriptorium_core.h"
#include "scriptorium_fonts.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------- numerals */

int scr_roman(unsigned n, char* out, size_t cap)
{
    if (!out || cap == 0) return 0;
    char buf[32];
    if (n == 0 || n > 3999) {
        snprintf(buf, sizeof buf, "%u", n);
    } else {
        static const struct { unsigned v; const char* s; } T[] = {
            { 1000, "M" }, { 900, "CM" }, { 500, "D" }, { 400, "CD" },
            { 100, "C" },  { 90, "XC" },  { 50, "L" },  { 40, "XL" },
            { 10, "X" },   { 9, "IX" },   { 5, "V" },   { 4, "IV" },
            { 1, "I" },
        };
        size_t k = 0;
        for (size_t i = 0; i < sizeof T / sizeof T[0]; ++i)
            while (n >= T[i].v) {
                for (const char* p = T[i].s; *p; ++p) buf[k++] = *p;
                n -= T[i].v;
            }
        buf[k] = 0;                       /* longest is 15: MMMDCCCLXXXVIII */
    }
    size_t len = strlen(buf);
    if (len > cap - 1) len = cap - 1;
    memcpy(out, buf, len);
    out[len] = 0;
    return (int)len;
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           ((unsigned char)c >= 0x80);
}

void scr_romanize(const char* in, char* out, size_t cap)
{
    if (!out || cap == 0) return;
    size_t n = 0;
    /* The argument is always evaluated (callers advance a pointer in it). */
    #define PUT(ch) do { char c_ = (ch); if (n < cap - 1) out[n++] = c_; } while (0)
    for (const char* p = in ? in : ""; *p; ) {
        if (!is_digit(*p)) { PUT(*p++); continue; }
        /* A number: digits, then any ",ddd" groups. */
        const char* q = p;
        unsigned long long v = 0;
        bool big = false;
        while (is_digit(*q)) {
            v = v * 10 + (unsigned)(*q++ - '0');
            if (v > 3999) big = true;
            if (big) v = 4000;
        }
        while (q[0] == ',' && is_digit(q[1]) && is_digit(q[2]) &&
               is_digit(q[3]) && !is_digit(q[4])) {
            v = v * 1000 + (unsigned)((q[1] - '0') * 100 + (q[2] - '0') * 10 +
                                      (q[3] - '0'));
            if (v > 3999) { big = true; v = 4000; }
            q += 4;
        }
        bool glued = (p > in && (is_alpha(p[-1]) || p[-1] == '_' ||
                                 (p[-1] == '.' && p - 1 > in && is_digit(p[-2])))) ||
                     is_alpha(*q) || *q == '_' ||
                     (*q == '.' && is_digit(q[1]));
        if (big || v == 0 || *p == '0' || glued) {
            while (p < q) PUT(*p++);
        } else {
            char r[32];
            scr_roman((unsigned)v, r, sizeof r);
            for (const char* c = r; *c; ++c) PUT(*c);
            p = q;
        }
    }
    #undef PUT
    out[n] = 0;
}

/* ------------------------------------------------------------- text */

static uint32_t utf8_next(const unsigned char** p, const unsigned char* end)
{
    const unsigned char* s = *p;
    uint32_t c = *s++;
    int extra = 0;
    if      (c >= 0xf0) { c &= 0x07; extra = 3; }
    else if (c >= 0xe0) { c &= 0x0f; extra = 2; }
    else if (c >= 0xc0) { c &= 0x1f; extra = 1; }
    else if (c >= 0x80) { *p = s; return 0xfffd; }
    while (extra-- > 0 && s < end && (*s & 0xc0) == 0x80)
        c = (c << 6) | (*s++ & 0x3f);
    *p = s;
    return c;
}

/* Latin letters with marks, folded to their base letter in the same case
 * (NULL = no glyph). */
static const char* fold_latin(uint32_t c, char one[3])
{
    char base = 0;
    bool lower = false;
    if (c >= 0xc0 && c <= 0xff && c != 0xd7 && c != 0xf7) {
        static const char L1[] = "AAAAAAACEEEEIIIIDNOOOOO*OUUUUY*s";
        uint32_t i = (c - 0xc0) & 0x1f;
        lower = c >= 0xe0;
        if (c == 0xdf) return "ss";
        if (c == 0xff) { base = 'Y'; }
        else if (L1[i] != '*' && L1[i] != 's') base = L1[i];
        else return NULL;                  /* thorn */
    } else if (c >= 0x100 && c <= 0x17f) {
        static const struct { uint16_t lo, hi; char to; } R[] = {
            { 0x100, 0x105, 'A' }, { 0x106, 0x10d, 'C' }, { 0x10e, 0x111, 'D' },
            { 0x112, 0x11b, 'E' }, { 0x11c, 0x123, 'G' }, { 0x124, 0x127, 'H' },
            { 0x128, 0x131, 'I' }, { 0x134, 0x135, 'J' }, { 0x136, 0x138, 'K' },
            { 0x139, 0x142, 'L' }, { 0x143, 0x14b, 'N' }, { 0x14c, 0x151, 'O' },
            { 0x154, 0x159, 'R' }, { 0x15a, 0x161, 'S' }, { 0x162, 0x167, 'T' },
            { 0x168, 0x173, 'U' }, { 0x174, 0x175, 'W' }, { 0x176, 0x178, 'Y' },
            { 0x179, 0x17e, 'Z' },
        };
        for (size_t i = 0; i < sizeof R / sizeof R[0]; ++i)
            if (c >= R[i].lo && c <= R[i].hi) { base = R[i].to; break; }
        if (!base) return NULL;
        /* Extended-A pairs are upper/lower on even/odd, except two runs
         * where the parity flips, and a few singletons. */
        if (c == 0x138 || c == 0x149 || c == 0x17f) lower = true;
        else if (c == 0x178) lower = false;
        else if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e))
            lower = (c & 1) == 0;
        else
            lower = (c & 1) == 1;
    } else {
        switch (c) {
            case 0xa0:   return " ";
            case 0xb7:   case 0x2022: return "\x01";
            case 0x2192: return "\x02";
            case 0x2190: return "\x03";
            case 0x2197: return "\x04";
            case 0x2665: case 0x2764: return "\x05";
            case 0x2013: case 0x2014: return "-";
            case 0x2018: case 0x2019: return "'";
            case 0x201c: case 0x201d: return "\"";
            case 0x2026: return "...";
            case 0x203a: return ">";
            case 0x2039: return "<";
            case 0xd7:   return "x";
        }
        return NULL;
    }
    one[0] = lower ? (char)(base + 32) : base;
    one[1] = 0;
    return one;
}

int scr_norm(const char* s, size_t len, char* out, int cap)
{
    if (!out || cap <= 0) return -1;
    const unsigned char* p = (const unsigned char*)s;
    const unsigned char* end = p + len;
    int n = 0;
    bool last_space = false, full = false;
    while (p < end) {
        uint32_t c = utf8_next(&p, end);
        const char* rep;
        char one[3] = { 0, 0, 0 };
        if (c >= 0x300 && c <= 0x36f) continue;          /* combining mark */
        if (c == 0xfe0f || c == 0x200d) return -1;        /* emoji joiners */
        if (c < 128) {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                if (last_space || n == 0) continue;
                rep = " ";
            } else {
                if (c < 32 || c == 127) continue;         /* control bytes */
                one[0] = (char)c;
                rep = one;
            }
        } else {
            rep = fold_latin(c, one);
            if (!rep) return -1;
        }
        /* Keep scanning after the buffer fills: a later character may still
         * make the whole text unchartable. */
        for (const char* q = rep; *q && !full; ++q) {
            if (n >= cap - 1) { full = true; break; }
            out[n++] = *q;
        }
        last_space = rep[0] == ' ';
    }
    while (n > 0 && out[n - 1] == ' ') --n;
    out[n] = 0;
    return n;
}

/* The nine-row charts, assembled once: capitals from SCR_CAPS; lower case
 * from the seven-row capitals of SCR_GLYPHS; everything else from its own
 * seven-row chart. Seven-row charts sit under two blank rows. */
static const char* g_chart[128][SCR_ROWS];

static void charts_build(void)
{
    static const char* const BLANK[8] = {
        "", "0", "00", "000", "0000", "00000", "000000", "0000000" };
    for (int c = 0; c < 128; ++c) {
        if (c >= 'A' && c <= 'Z') {
            for (int r = 0; r < SCR_ROWS; ++r) g_chart[c][r] = SCR_CAPS[c - 'A'][r];
            continue;
        }
        int src = (c >= 'a' && c <= 'z') ? c - 32 : c;
        if (!SCR_GLYPHS[src][0]) continue;
        size_t w = strlen(SCR_GLYPHS[src][0]);
        if (w > 7) w = 7;
        g_chart[c][0] = g_chart[c][1] = BLANK[w];
        for (int r = 0; r < SCR_SMALL_ROWS; ++r)
            g_chart[c][r + 2] = SCR_GLYPHS[src][r];
    }
}

const char* const* scr_glyph(unsigned char ch)
{
    if (!g_chart[' '][0]) charts_build();
    if (ch >= 128 || !g_chart[ch][0]) return NULL;
    return g_chart[ch];
}

int scr_glyph_cols(unsigned char ch)
{
    const char* const* g = scr_glyph(ch);
    return g ? (int)strlen(g[0]) : 0;
}

int scr_text_cols(const char* glyphs, int n)
{
    if (n <= 0) return 0;
    int cols = 0;
    for (int i = 0; i < n; ++i)
        cols += scr_glyph_cols((unsigned char)glyphs[i]) + 1;
    return cols - 1;
}

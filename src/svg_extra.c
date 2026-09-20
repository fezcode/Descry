/* See svg_extra.h. Two halves, kept in one file because they are two ends of
 * one job: a pure XML scan that harvests <text>, <image> and the badge clip
 * rect, and a compositor that paints them onto the buffer nanosvg produced.
 * Only the second half needs FreeType/HarfBuzz/nanosvg, so tests can exercise
 * the parser against a source string without a window or a font. */

#include "svg_extra.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <hb.h>
#include <hb-ft.h>

/* Declarations only: the nanosvg IMPLEMENTATION lives in icon_raster.c. */
#include "nanosvg.h"
#include "nanosvgrast.h"

/* ======================================================================= *
 * Small shared helpers
 * ======================================================================= */

/* strtof is locale-dependent: under a comma-decimal locale it stops at the
 * '.' in "0.1" and every transform in the document collapses. nanosvg carries
 * its own atof for exactly this reason; so do we. Advances *endp past what it
 * consumed when endp is non-NULL. */
static float sx_atof(const char* s, const char* e, const char** endp)
{
    const char* p = s;
    while (p < e && isspace((unsigned char)*p)) ++p;
    int sign = 1;
    if (p < e && (*p == '+' || *p == '-')) { if (*p == '-') sign = -1; ++p; }

    double ip = 0.0;
    int digits = 0;
    while (p < e && *p >= '0' && *p <= '9') { ip = ip * 10.0 + (*p - '0'); ++p; ++digits; }
    if (p < e && *p == '.') {
        ++p;
        double scale = 0.1;
        while (p < e && *p >= '0' && *p <= '9') {
            ip += (*p - '0') * scale; scale *= 0.1; ++p; ++digits;
        }
    }
    if (digits && p < e && (*p == 'e' || *p == 'E')) {
        const char* save = p;
        ++p;
        int esign = 1;
        if (p < e && (*p == '+' || *p == '-')) { if (*p == '-') esign = -1; ++p; }
        int ev = 0, edig = 0;
        while (p < e && *p >= '0' && *p <= '9') { ev = ev * 10 + (*p - '0'); ++p; ++edig; }
        if (edig) ip *= pow(10.0, esign * ev); else p = save;
    }
    if (endp) *endp = p;
    return digits ? (float)(sign * ip) : 0.0f;
}

/* A length with an optional CSS unit. `em_base` resolves em/ex/%; everything
 * else lands in user units. */
static float sx_length(const char* s, const char* e, float em_base)
{
    const char* p = NULL;
    float v = sx_atof(s, e, &p);
    if (!p) return v;
    while (p < e && isspace((unsigned char)*p)) ++p;
    size_t n = (size_t)(e - p);
    if (n >= 2 && strncmp(p, "pt", 2) == 0) return v * 96.0f / 72.0f;
    if (n >= 2 && strncmp(p, "pc", 2) == 0) return v * 16.0f;
    if (n >= 2 && strncmp(p, "mm", 2) == 0) return v * 96.0f / 25.4f;
    if (n >= 2 && strncmp(p, "cm", 2) == 0) return v * 96.0f / 2.54f;
    if (n >= 2 && strncmp(p, "in", 2) == 0) return v * 96.0f;
    if (n >= 3 && strncmp(p, "rem", 3) == 0) return v * em_base;
    if (n >= 2 && strncmp(p, "em", 2) == 0) return v * em_base;
    if (n >= 2 && strncmp(p, "ex", 2) == 0) return v * em_base * 0.5f;
    if (n >= 1 && *p == '%')                return v * em_base / 100.0f;
    return v;
}

/* out = a * b, both SVG 2x3 matrices laid out [a b c d e f]. */
static void mat_mul(float* out, const float* A, const float* B)
{
    float t[6];
    t[0] = A[0]*B[0] + A[2]*B[1];
    t[1] = A[1]*B[0] + A[3]*B[1];
    t[2] = A[0]*B[2] + A[2]*B[3];
    t[3] = A[1]*B[2] + A[3]*B[3];
    t[4] = A[0]*B[4] + A[2]*B[5] + A[4];
    t[5] = A[1]*B[4] + A[3]*B[5] + A[5];
    memcpy(out, t, sizeof t);
}

static void mat_apply(const float* m, float x, float y, float* ox, float* oy)
{
    *ox = m[0]*x + m[2]*y + m[4];
    *oy = m[1]*x + m[3]*y + m[5];
}

static float mat_scale_x(const float* m) { return sqrtf(m[0]*m[0] + m[1]*m[1]); }
static float mat_scale_y(const float* m) { return sqrtf(m[2]*m[2] + m[3]*m[3]); }
/* One number for "how big is a glyph here": the geometric mean of the two
 * axis scales, which is exact for the uniform scales transforms actually use
 * on text and sane for the rest. */
static float mat_scale_avg(const float* m)
{
    float s = sqrtf(mat_scale_x(m) * mat_scale_y(m));
    return s > 0.0f ? s : 1.0f;
}

/* Read up to 6 comma/space separated numbers out of a transform argument
 * list. Returns how many were found. */
static int read_args(const char* s, const char* e, float* out, int max)
{
    int n = 0;
    const char* p = s;
    while (p < e && n < max) {
        while (p < e && (isspace((unsigned char)*p) || *p == ',')) ++p;
        if (p >= e) break;
        if (!(*p == '+' || *p == '-' || *p == '.' || (*p >= '0' && *p <= '9'))) break;
        const char* q = NULL;
        out[n++] = sx_atof(p, e, &q);
        if (!q || q == p) break;
        p = q;
    }
    return n;
}

/* Parse an SVG transform list into `m` (pre-seeded with the parent matrix). */
static void parse_transform(const char* s, const char* e, float* m)
{
    const char* p = s;
    while (p < e) {
        while (p < e && (isspace((unsigned char)*p) || *p == ',')) ++p;
        const char* nb = p;
        while (p < e && (isalpha((unsigned char)*p))) ++p;
        const char* ne = p;
        while (p < e && isspace((unsigned char)*p)) ++p;
        if (p >= e || *p != '(') break;
        ++p;
        const char* ab = p;
        while (p < e && *p != ')') ++p;
        const char* ae = p;
        if (p < e) ++p;                               /* past ')' */

        float a[6] = {0,0,0,0,0,0};
        int n = read_args(ab, ae, a, 6);
        size_t nl = (size_t)(ne - nb);
        float t[6] = {1,0,0,1,0,0};

        if (nl == 9 && strncmp(nb, "translate", 9) == 0) {
            t[4] = n > 0 ? a[0] : 0.0f;
            t[5] = n > 1 ? a[1] : 0.0f;
        } else if (nl == 5 && strncmp(nb, "scale", 5) == 0) {
            t[0] = n > 0 ? a[0] : 1.0f;
            t[3] = n > 1 ? a[1] : t[0];
        } else if (nl == 6 && strncmp(nb, "matrix", 6) == 0 && n == 6) {
            memcpy(t, a, sizeof t);
        } else if (nl == 6 && strncmp(nb, "rotate", 6) == 0 && n > 0) {
            float r = a[0] * 3.14159265358979f / 180.0f;
            float cs = cosf(r), sn = sinf(r);
            float rot[6] = { cs, sn, -sn, cs, 0, 0 };
            if (n >= 3) {
                float pre[6]  = { 1,0,0,1,  a[1],  a[2] };
                float post[6] = { 1,0,0,1, -a[1], -a[2] };
                mat_mul(pre, pre, rot);
                mat_mul(pre, pre, post);
                memcpy(t, pre, sizeof t);
            } else {
                memcpy(t, rot, sizeof t);
            }
        } else if (nl == 5 && strncmp(nb, "skewX", 5) == 0 && n > 0) {
            t[2] = tanf(a[0] * 3.14159265358979f / 180.0f);
        } else if (nl == 5 && strncmp(nb, "skewY", 5) == 0 && n > 0) {
            t[1] = tanf(a[0] * 3.14159265358979f / 180.0f);
        } else {
            continue;                                  /* unknown: identity */
        }
        mat_mul(m, m, t);
    }
}

/* A deliberately short named-colour list: the ones that actually show up in
 * generated SVG. Anything else falls back to the inherited colour. */
static int named_color(const char* s, size_t n, unsigned char* r,
                       unsigned char* g, unsigned char* b)
{
    static const struct { const char* n; unsigned char r, g, bl; } tab[] = {
        {"white",   255,255,255}, {"black",     0,  0,  0},
        {"red",     255,  0,  0}, {"green",     0,128,  0},
        {"blue",      0,  0,255}, {"yellow",  255,255,  0},
        {"gray",    128,128,128}, {"grey",    128,128,128},
        {"silver",  192,192,192}, {"maroon",  128,  0,  0},
        {"navy",      0,  0,128}, {"olive",   128,128,  0},
        {"purple",  128,  0,128}, {"teal",      0,128,128},
        {"lime",      0,255,  0}, {"aqua",      0,255,255},
        {"cyan",      0,255,255}, {"magenta", 255,  0,255},
        {"fuchsia", 255,  0,255}, {"orange",  255,165,  0},
        {"darkgray",169,169,169}, {"darkgrey",169,169,169},
        {"lightgray",211,211,211},{"lightgrey",211,211,211},
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; ++i)
        if (strlen(tab[i].n) == n && strncmp(tab[i].n, s, n) == 0) {
            *r = tab[i].r; *g = tab[i].g; *b = tab[i].bl; return 1;
        }
    return 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 1 = parsed a colour, 0 = "none"/unrecognized (caller decides). */
static int parse_color(const char* s, const char* e, unsigned char* r,
                       unsigned char* g, unsigned char* b, int* none)
{
    *none = 0;
    while (s < e && isspace((unsigned char)*s)) ++s;
    while (e > s && isspace((unsigned char)e[-1])) --e;
    size_t n = (size_t)(e - s);
    if (n == 0) return 0;
    if (n == 4 && strncmp(s, "none", 4) == 0) { *none = 1; return 0; }
    if (*s == '#') {
        if (n == 4) {
            int h[3];
            for (int i = 0; i < 3; ++i) { h[i] = hexval(s[1+i]); if (h[i] < 0) return 0; }
            *r = (unsigned char)(h[0]*17); *g = (unsigned char)(h[1]*17);
            *b = (unsigned char)(h[2]*17);
            return 1;
        }
        if (n == 7) {
            int h[6];
            for (int i = 0; i < 6; ++i) { h[i] = hexval(s[1+i]); if (h[i] < 0) return 0; }
            *r = (unsigned char)(h[0]*16 + h[1]);
            *g = (unsigned char)(h[2]*16 + h[3]);
            *b = (unsigned char)(h[4]*16 + h[5]);
            return 1;
        }
        return 0;
    }
    if (n > 4 && strncmp(s, "rgb", 3) == 0) {
        const char* p = (const char*)memchr(s, '(', n);
        if (!p) return 0;
        float a[4];
        int got = read_args(p + 1, e, a, 4);
        if (got < 3) return 0;
        /* percentages are rare enough to ignore; clamp raw 0..255 */
        for (int i = 0; i < 3; ++i) { if (a[i] < 0) a[i] = 0; if (a[i] > 255) a[i] = 255; }
        *r = (unsigned char)(a[0] + 0.5f);
        *g = (unsigned char)(a[1] + 0.5f);
        *b = (unsigned char)(a[2] + 0.5f);
        return 1;
    }
    /* lowercase for the name table */
    char buf[24];
    if (n >= sizeof buf) return 0;
    for (size_t i = 0; i < n; ++i)
        buf[i] = (char)tolower((unsigned char)s[i]);
    return named_color(buf, n, r, g, b);
}

/* ======================================================================= *
 * XML scanning
 * ======================================================================= */

/* Element names are case-sensitive; a namespace prefix ("svg:text") is not
 * part of the name. */
static int tag_is(const char* b, const char* e, const char* name)
{
    const char* c = (const char*)memchr(b, ':', (size_t)(e - b));
    if (c) b = c + 1;
    size_t n = (size_t)(e - b), m = strlen(name);
    return n == m && strncmp(b, name, m) == 0;
}

/* Find attribute `name` in the attribute region [b,e). */
static int attr_get(const char* b, const char* e, const char* name,
                    const char** vb, const char** ve)
{
    size_t nl = strlen(name);
    const char* p = b;
    while (p < e) {
        while (p < e && isspace((unsigned char)*p)) ++p;
        if (p >= e) break;
        const char* ab = p;
        while (p < e && *p != '=' && !isspace((unsigned char)*p)) ++p;
        const char* ae = p;
        while (p < e && isspace((unsigned char)*p)) ++p;
        if (p >= e || *p != '=') continue;      /* valueless attr: skip it */
        ++p;
        while (p < e && isspace((unsigned char)*p)) ++p;
        char q = 0;
        if (p < e && (*p == '"' || *p == '\'')) { q = *p; ++p; }
        const char* s = p;
        while (p < e && (q ? *p != q : !isspace((unsigned char)*p))) ++p;
        const char* t = p;
        if (q && p < e) ++p;
        if ((size_t)(ae - ab) == nl && strncmp(ab, name, nl) == 0) {
            *vb = s; *ve = t; return 1;
        }
    }
    return 0;
}

/* One property out of a style="a:b;c:d" attribute. */
static int style_get(const char* b, const char* e, const char* name,
                     const char** vb, const char** ve)
{
    if (!b) return 0;
    size_t nl = strlen(name);
    const char* p = b;
    while (p < e) {
        while (p < e && (isspace((unsigned char)*p) || *p == ';')) ++p;
        const char* nb = p;
        while (p < e && *p != ':' && *p != ';') ++p;
        const char* ne = p;
        while (ne > nb && isspace((unsigned char)ne[-1])) --ne;
        if (p >= e || *p != ':') { while (p < e && *p != ';') ++p; continue; }
        ++p;
        const char* s = p;
        while (p < e && *p != ';') ++p;
        const char* t = p;
        while (s < t && isspace((unsigned char)*s)) ++s;
        while (t > s && isspace((unsigned char)t[-1])) --t;
        if ((size_t)(ne - nb) == nl && strncmp(nb, name, nl) == 0) {
            *vb = s; *ve = t; return 1;
        }
    }
    return 0;
}

/* A presentation value, style="" winning over the plain attribute (CSS
 * cascade order for the two places SVG lets you write the same thing). */
typedef struct {
    const char* ab; const char* ae;     /* attribute region */
    const char* sb; const char* se;     /* style="" region, or NULL */
} Props;

static int prop_get(const Props* pr, const char* name,
                    const char** vb, const char** ve)
{
    if (pr->sb && style_get(pr->sb, pr->se, name, vb, ve)) return 1;
    return attr_get(pr->ab, pr->ae, name, vb, ve);
}

/* ---------- text content ------------------------------------------------ */

static size_t utf8_put(char* out, uint32_t cp)
{
    if (cp < 0x80)    { out[0] = (char)cp; return 1; }
    if (cp < 0x800)   { out[0] = (char)(0xC0 | (cp >> 6));
                        out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12));
                        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* "&amp;" -> '&'. Returns 0 for an entity we don't know (caller emits the
 * raw '&'). `b` points just past the '&', `e` at the ';'. */
static uint32_t entity_cp(const char* b, const char* e)
{
    size_t n = (size_t)(e - b);
    if (n == 0) return 0;
    if (*b == '#') {
        uint32_t v = 0;
        if (n > 1 && (b[1] == 'x' || b[1] == 'X')) {
            for (const char* p = b + 2; p < e; ++p) {
                int h = hexval(*p);
                if (h < 0) return 0;
                v = v * 16 + (uint32_t)h;
            }
        } else {
            for (const char* p = b + 1; p < e; ++p) {
                if (*p < '0' || *p > '9') return 0;
                v = v * 10 + (uint32_t)(*p - '0');
            }
        }
        return (v && v <= 0x10FFFF) ? v : 0;
    }
    if (n == 3 && strncmp(b, "amp",  3) == 0) return '&';
    if (n == 2 && strncmp(b, "lt",   2) == 0) return '<';
    if (n == 2 && strncmp(b, "gt",   2) == 0) return '>';
    if (n == 4 && strncmp(b, "quot", 4) == 0) return '"';
    if (n == 4 && strncmp(b, "apos", 4) == 0) return '\'';
    if (n == 4 && strncmp(b, "nbsp", 4) == 0) return 0x00A0;
    return 0;
}

/* Character data of an element: nested tags dropped, entities decoded,
 * whitespace runs collapsed to one space and trimmed (xml:space="default").
 * Never longer than the input, so one allocation is enough. */
static char* text_content(const char* b, const char* e)
{
    char* out = malloc((size_t)(e - b) + 2);
    if (!out) return NULL;
    size_t n = 0;
    int in_tag = 0, pending_space = 0;
    for (const char* p = b; p < e; ++p) {
        char c = *p;
        if (in_tag) { if (c == '>') in_tag = 0; continue; }
        if (c == '<') { in_tag = 1; continue; }
        if (isspace((unsigned char)c)) { if (n) pending_space = 1; continue; }
        if (c == '&') {
            const char* semi = (const char*)memchr(p, ';', (size_t)(e - p));
            if (semi && semi - p <= 12) {
                uint32_t cp = entity_cp(p + 1, semi);
                if (cp) {
                    if (pending_space) { out[n++] = ' '; pending_space = 0; }
                    n += utf8_put(out + n, cp);
                    p = semi;
                    continue;
                }
            }
        }
        if (pending_space) { out[n++] = ' '; pending_space = 0; }
        out[n++] = c;
    }
    out[n] = 0;
    return out;
}

/* Attribute values can carry entities too (a percent-encoded data: URI often
 * arrives with &amp; in it). Same decode, minus the whitespace collapsing. */
static char* attr_value_dup(const char* b, const char* e)
{
    char* out = malloc((size_t)(e - b) + 2);
    if (!out) return NULL;
    size_t n = 0;
    for (const char* p = b; p < e; ++p) {
        if (*p == '&') {
            const char* semi = (const char*)memchr(p, ';', (size_t)(e - p));
            if (semi && semi - p <= 12) {
                uint32_t cp = entity_cp(p + 1, semi);
                if (cp) { n += utf8_put(out + n, cp); p = semi; continue; }
            }
        }
        out[n++] = *p;
    }
    out[n] = 0;
    return out;
}

/* ======================================================================= *
 * Parser
 * ======================================================================= */

#define SX_MAX_DEPTH 64

struct SvgExtra {
    SvgTextRun*  texts;   int n_text,  cap_text;
    SvgImageRef* images;  int n_image, cap_image;
    SvgClipRect  clip;
    char         clip_id[64];      /* id of the clipPath we harvested       */
    char         used_id[64];      /* first clip-path="url(#..)" referenced  */
    int          clip_has_rect;
    int          clip_used;
};

typedef struct {
    float         m[6];
    float         font_size;          /* user units */
    char          family[96];
    int           bold, italic;
    unsigned char r, g, b;            /* fill */
    int           fill_none;
    float         fill_opacity;       /* inherited (replaced, not multiplied) */
    float         chain_opacity;      /* product of ancestor opacity="" */
    int           anchor;
    int           filtered;           /* a <filter> we cannot reproduce */
} Ctx;

static void ctx_root(Ctx* c)
{
    memset(c, 0, sizeof *c);
    c->m[0] = c->m[3] = 1.0f;
    c->font_size     = 16.0f;         /* CSS "medium" */
    c->fill_opacity  = 1.0f;
    c->chain_opacity = 1.0f;
}

static int push_text(SvgExtra* e, const SvgTextRun* t)
{
    if (e->n_text == e->cap_text) {
        int nc = e->cap_text ? e->cap_text * 2 : 8;
        SvgTextRun* nt = realloc(e->texts, (size_t)nc * sizeof *nt);
        if (!nt) return 0;
        e->texts = nt; e->cap_text = nc;
    }
    e->texts[e->n_text++] = *t;
    return 1;
}

static int push_image(SvgExtra* e, const SvgImageRef* im)
{
    if (e->n_image == e->cap_image) {
        int nc = e->cap_image ? e->cap_image * 2 : 4;
        SvgImageRef* ni = realloc(e->images, (size_t)nc * sizeof *ni);
        if (!ni) return 0;
        e->images = ni; e->cap_image = nc;
    }
    e->images[e->n_image++] = *im;
    return 1;
}

/* Apply one element's presentation attributes on top of the inherited ctx. */
static void ctx_apply(Ctx* c, const Props* pr)
{
    const char *vb, *ve;

    if (attr_get(pr->ab, pr->ae, "transform", &vb, &ve))
        parse_transform(vb, ve, c->m);

    if (prop_get(pr, "fill", &vb, &ve)) {
        unsigned char r, g, b; int none = 0;
        if (parse_color(vb, ve, &r, &g, &b, &none)) {
            c->r = r; c->g = g; c->b = b; c->fill_none = 0;
        } else if (none) {
            c->fill_none = 1;
        }
    }
    if (prop_get(pr, "fill-opacity", &vb, &ve)) {
        float v = sx_length(vb, ve, 1.0f);
        c->fill_opacity = v < 0 ? 0 : (v > 1 ? 1 : v);
    }
    if (prop_get(pr, "opacity", &vb, &ve)) {
        float v = sx_length(vb, ve, 1.0f);
        c->chain_opacity *= (v < 0 ? 0 : (v > 1 ? 1 : v));
    }
    if (prop_get(pr, "font-size", &vb, &ve)) {
        float v = sx_length(vb, ve, c->font_size);
        if (v > 0) c->font_size = v;
    }
    if (prop_get(pr, "font-family", &vb, &ve)) {
        size_t n = (size_t)(ve - vb);
        if (n >= sizeof c->family) n = sizeof c->family - 1;
        memcpy(c->family, vb, n);
        c->family[n] = 0;
    }
    if (prop_get(pr, "font-weight", &vb, &ve)) {
        size_t n = (size_t)(ve - vb);
        if ((n == 4 && strncmp(vb, "bold", 4) == 0) ||
            (n == 6 && strncmp(vb, "bolder", 6) == 0)) {
            c->bold = 1;
        } else if (n && isdigit((unsigned char)*vb)) {
            c->bold = sx_atof(vb, ve, NULL) >= 550.0f;
        } else {
            c->bold = 0;                        /* normal / lighter */
        }
    }
    if (prop_get(pr, "font-style", &vb, &ve)) {
        size_t n = (size_t)(ve - vb);
        c->italic = (n == 6 && strncmp(vb, "italic",  6) == 0) ||
                    (n == 7 && strncmp(vb, "oblique", 7) == 0);
    }
    if (prop_get(pr, "text-anchor", &vb, &ve)) {
        size_t n = (size_t)(ve - vb);
        if      (n == 6 && strncmp(vb, "middle", 6) == 0) c->anchor = SVG_ANCHOR_MIDDLE;
        else if (n == 3 && strncmp(vb, "end",    3) == 0) c->anchor = SVG_ANCHOR_END;
        else                                              c->anchor = SVG_ANCHOR_START;
    }
    /* A filtered element is drawn through machinery we do not have (the drop
     * shadow shields.io puts under every label is a Gaussian blur). Painting
     * it unfiltered would be a hard black smear, so drop it: the unblurred
     * shadow copy underneath still gives the label its depth. */
    if (attr_get(pr->ab, pr->ae, "filter", &vb, &ve) && ve > vb)
        c->filtered = 1;
}

/* Is this subtree invisible or non-rendering? */
static int elem_hidden(const Props* pr)
{
    const char *vb, *ve;
    if (prop_get(pr, "display", &vb, &ve) &&
        (size_t)(ve - vb) == 4 && strncmp(vb, "none", 4) == 0) return 1;
    if (prop_get(pr, "visibility", &vb, &ve) &&
        (size_t)(ve - vb) == 6 && strncmp(vb, "hidden", 6) == 0) return 1;
    return 0;
}

/* Contents are not markup we ever draw; skip the subtree outright. */
static int elem_is_opaque(const char* nb, const char* ne)
{
    static const char* const names[] = {
        "style", "script", "title", "desc", "metadata", NULL
    };
    for (int i = 0; names[i]; ++i)
        if (tag_is(nb, ne, names[i])) return 1;
    return 0;
}

/* Definitions: nothing inside paints where it stands, but a <clipPath> in
 * here is still the clipPath the badge references, so we keep walking. */
static int elem_is_nodraw(const char* nb, const char* ne)
{
    static const char* const names[] = {
        "defs", "filter", "mask", "marker", "pattern", "symbol", NULL
    };
    for (int i = 0; names[i]; ++i)
        if (tag_is(nb, ne, names[i])) return 1;
    return 0;
}

/* Pointer to the '<' of the matching end tag for `name`, or `end`. */
static const char* find_end_tag(const char* p, const char* end, const char* name)
{
    size_t nl = strlen(name);
    while (p < end) {
        const char* lt = (const char*)memchr(p, '<', (size_t)(end - p));
        if (!lt) return end;
        if (end - lt >= (ptrdiff_t)(nl + 3) && lt[1] == '/') {
            const char* nb = lt + 2;
            const char* ne = nb;
            while (ne < end && *ne != '>' && !isspace((unsigned char)*ne)) ++ne;
            if (tag_is(nb, ne, name)) return lt;
        }
        p = lt + 1;
    }
    return end;
}

/* A url(#id) reference; writes the bare id. */
static void url_id(const char* b, const char* e, char* out, size_t cap)
{
    out[0] = 0;
    while (b < e && isspace((unsigned char)*b)) ++b;
    if ((size_t)(e - b) < 6 || strncmp(b, "url(", 4) != 0) return;
    b += 4;
    while (b < e && (isspace((unsigned char)*b) || *b == '"' || *b == '\'')) ++b;
    if (b < e && *b == '#') ++b;
    const char* q = b;
    while (q < e && *q != ')' && *q != '"' && *q != '\'' &&
           !isspace((unsigned char)*q)) ++q;
    size_t n = (size_t)(q - b);
    if (n >= cap) n = cap - 1;
    memcpy(out, b, n);
    out[n] = 0;
}

SvgExtra* svg_extra_parse(const char* src, size_t len)
{
    SvgExtra* e = calloc(1, sizeof *e);
    if (!e || !src) return e;

    Ctx stack[SX_MAX_DEPTH];
    ctx_root(&stack[0]);
    int depth = 0;
    int skip_from   = -1;    /* depth of a subtree we do not walk at all  */
    int nodraw_from = -1;    /* depth of a <defs>-like subtree            */
    int clip_from   = -1;    /* depth of the <clipPath> we are reading    */

    const char* p   = src;
    const char* end = src + len;

    while (p < end) {
        const char* lt = (const char*)memchr(p, '<', (size_t)(end - p));
        if (!lt) break;
        p = lt + 1;
        if (p >= end) break;

        if (*p == '!') {
            if (end - p >= 3 && strncmp(p, "!--", 3) == 0) {
                const char* q = p + 3;
                while (q + 3 <= end && strncmp(q, "-->", 3) != 0) ++q;
                p = (q + 3 <= end) ? q + 3 : end;
            } else {
                const char* q = (const char*)memchr(p, '>', (size_t)(end - p));
                p = q ? q + 1 : end;
            }
            continue;
        }
        if (*p == '?') {
            const char* q = p;
            while (q + 2 <= end && strncmp(q, "?>", 2) != 0) ++q;
            p = (q + 2 <= end) ? q + 2 : end;
            continue;
        }
        if (*p == '/') {                                   /* end tag */
            const char* q = (const char*)memchr(p, '>', (size_t)(end - p));
            p = q ? q + 1 : end;
            if (depth > 0) {
                if (skip_from   == depth) skip_from   = -1;
                if (nodraw_from == depth) nodraw_from = -1;
                if (clip_from   == depth) clip_from   = -1;
                --depth;
            }
            continue;
        }

        /* ---- start tag ---- */
        const char* nb = p;
        while (p < end && !isspace((unsigned char)*p) && *p != '>' && *p != '/') ++p;
        const char* ne = p;

        const char* ab = p;
        int inq = 0; char qc = 0;
        while (p < end) {
            char c = *p;
            if (inq)                            { if (c == qc) inq = 0; }
            else if (c == '"' || c == '\'')     { inq = 1; qc = c; }
            else if (c == '>')                  break;
            ++p;
        }
        const char* ae = p;
        int self_close = (ae > ab && ae[-1] == '/');
        if (self_close) --ae;
        if (p < end) ++p;                                  /* past '>' */

        if (skip_from >= 0) {                              /* <style>, <title>… */
            if (!self_close) ++depth;
            continue;
        }
        /* Past this point every branch that descends writes stack[depth], so
         * the depth cap has to be enforced before any of them, not inside
         * one of them. */
        if (!self_close && depth + 1 >= SX_MAX_DEPTH) {
            skip_from = depth + 1;
            ++depth;
            continue;
        }

        Props pr = { ab, ae, NULL, NULL };
        {
            const char *vb, *ve;
            if (attr_get(ab, ae, "style", &vb, &ve)) { pr.sb = vb; pr.se = ve; }
        }

        /* ---- inside a <clipPath>: harvest the first <rect> ---- */
        if (clip_from >= 0) {
            Ctx c = stack[depth];
            ctx_apply(&c, &pr);
            if (tag_is(nb, ne, "rect") && !e->clip_has_rect) {
                const char *vb, *ve;
                float x = 0, y = 0, w = 0, hgt = 0, rx = 0, ry = 0;
                if (attr_get(ab, ae, "x", &vb, &ve))      x   = sx_length(vb, ve, c.font_size);
                if (attr_get(ab, ae, "y", &vb, &ve))      y   = sx_length(vb, ve, c.font_size);
                if (attr_get(ab, ae, "width", &vb, &ve))  w   = sx_length(vb, ve, c.font_size);
                if (attr_get(ab, ae, "height", &vb, &ve)) hgt = sx_length(vb, ve, c.font_size);
                if (attr_get(ab, ae, "rx", &vb, &ve))     rx  = sx_length(vb, ve, c.font_size);
                if (attr_get(ab, ae, "ry", &vb, &ve))     ry  = sx_length(vb, ve, c.font_size);
                if (rx <= 0) rx = ry;
                if (ry <= 0) ry = rx;
                if (w > 0 && hgt > 0) {
                    float ox, oy;
                    mat_apply(c.m, x, y, &ox, &oy);
                    e->clip.x  = ox;
                    e->clip.y  = oy;
                    e->clip.w  = w   * mat_scale_x(c.m);
                    e->clip.h  = hgt * mat_scale_y(c.m);
                    e->clip.rx = rx  * mat_scale_x(c.m);
                    e->clip.ry = ry  * mat_scale_y(c.m);
                    e->clip_has_rect = 1;
                }
            }
            if (!self_close) stack[++depth] = c;
            continue;
        }

        if (tag_is(nb, ne, "clipPath")) {
            Ctx cp = stack[depth];
            ctx_apply(&cp, &pr);
            if (!e->clip_has_rect) {
                const char *vb, *ve;
                if (attr_get(ab, ae, "id", &vb, &ve)) {
                    size_t n = (size_t)(ve - vb);
                    if (n >= sizeof e->clip_id) n = sizeof e->clip_id - 1;
                    memcpy(e->clip_id, vb, n);
                    e->clip_id[n] = 0;
                }
                if (!self_close) clip_from = depth + 1;
            } else if (!self_close) {
                skip_from = depth + 1;
            }
            if (!self_close) stack[++depth] = cp;
            continue;
        }

        if (elem_is_opaque(nb, ne) || elem_hidden(&pr) ||
            depth + 1 >= SX_MAX_DEPTH) {
            if (!self_close) { skip_from = depth + 1; ++depth; }
            continue;
        }
        if (nodraw_from < 0 && elem_is_nodraw(nb, ne) && !self_close)
            nodraw_from = depth + 1;

        /* Note a clip-path reference so we know the badge clip is live. The
         * reference can precede the clipPath, so remember the id either way. */
        {
            const char *vb, *ve;
            if (prop_get(&pr, "clip-path", &vb, &ve)) {
                char id[64];
                url_id(vb, ve, id, sizeof id);
                if (id[0]) {
                    if (!e->used_id[0])
                        snprintf(e->used_id, sizeof e->used_id, "%s", id);
                    if (e->clip_id[0] && strcmp(id, e->clip_id) == 0)
                        e->clip_used = 1;
                }
            }
        }

        Ctx c = stack[depth];
        ctx_apply(&c, &pr);

        if (tag_is(nb, ne, "text")) {
            /* Consume the whole element: its character data (tspans included)
             * is the run, and we never descend into it. */
            const char* cb = p;
            const char* ce = self_close ? p : find_end_tag(p, end, "text");
            if (!self_close) {
                const char* gt = (const char*)memchr(ce, '>', (size_t)(end - ce));
                p = gt ? gt + 1 : end;
            }
            if (nodraw_from < 0 && !c.filtered && !c.fill_none && ce > cb) {
                const char *vb, *ve;
                float tx = 0, ty = 0, tlen = 0;
                if (attr_get(ab, ae, "x", &vb, &ve)) tx = sx_length(vb, ve, c.font_size);
                if (attr_get(ab, ae, "y", &vb, &ve)) ty = sx_length(vb, ve, c.font_size);
                if (attr_get(ab, ae, "textLength", &vb, &ve))
                    tlen = sx_length(vb, ve, c.font_size);

                char* str = text_content(cb, ce);
                if (str && str[0]) {
                    SvgTextRun t;
                    memset(&t, 0, sizeof t);
                    t.text = str;
                    mat_apply(c.m, tx, ty, &t.x, &t.y);
                    t.size    = c.font_size * mat_scale_avg(c.m);
                    t.length  = tlen > 0 ? tlen * mat_scale_x(c.m) : 0.0f;
                    t.anchor  = (SvgAnchor)c.anchor;
                    t.bold    = c.bold;
                    t.italic  = c.italic;
                    t.r = c.r; t.g = c.g; t.b = c.b;
                    t.opacity = c.fill_opacity * c.chain_opacity;
                    memcpy(t.family, c.family, sizeof t.family);
                    if (!push_text(e, &t)) free(str);
                } else {
                    free(str);
                }
            }
            continue;                                  /* never pushed */
        }

        if (tag_is(nb, ne, "image")) {
            const char *vb, *ve;
            float ix = 0, iy = 0, iw = 0, ih = 0;
            if (attr_get(ab, ae, "x", &vb, &ve))      ix = sx_length(vb, ve, c.font_size);
            if (attr_get(ab, ae, "y", &vb, &ve))      iy = sx_length(vb, ve, c.font_size);
            if (attr_get(ab, ae, "width", &vb, &ve))  iw = sx_length(vb, ve, c.font_size);
            if (attr_get(ab, ae, "height", &vb, &ve)) ih = sx_length(vb, ve, c.font_size);
            if (!attr_get(ab, ae, "href", &vb, &ve))
                attr_get(ab, ae, "xlink:href", &vb, &ve);
            if (nodraw_from < 0 && iw > 0 && ih > 0 && ve > vb) {
                SvgImageRef im;
                memset(&im, 0, sizeof im);
                mat_apply(c.m, ix, iy, &im.x, &im.y);
                im.w = iw * mat_scale_x(c.m);
                im.h = ih * mat_scale_y(c.m);
                im.href = attr_value_dup(vb, ve);
                if (!im.href || !push_image(e, &im)) free(im.href);
            }
            if (!self_close) { skip_from = depth + 1; ++depth; }
            continue;
        }

        if (!self_close) {
            stack[++depth] = c;
        }
    }

    if (e->clip_has_rect && !e->clip_used && e->clip_id[0] &&
        strcmp(e->clip_id, e->used_id) == 0)
        e->clip_used = 1;
    e->clip.present = e->clip_has_rect && e->clip_used;
    return e;
}

void svg_extra_free(SvgExtra* e)
{
    if (!e) return;
    for (int i = 0; i < e->n_text;  ++i) free(e->texts[i].text);
    for (int i = 0; i < e->n_image; ++i) free(e->images[i].href);
    free(e->texts);
    free(e->images);
    free(e);
}

int svg_extra_empty(const SvgExtra* e)
{
    return !e || (e->n_text == 0 && e->n_image == 0 && !e->clip.present);
}
int svg_extra_text_count (const SvgExtra* e) { return e ? e->n_text  : 0; }
int svg_extra_image_count(const SvgExtra* e) { return e ? e->n_image : 0; }

const SvgTextRun* svg_extra_text(const SvgExtra* e, int i)
{
    return (e && i >= 0 && i < e->n_text) ? &e->texts[i] : NULL;
}
const SvgImageRef* svg_extra_image(const SvgExtra* e, int i)
{
    return (e && i >= 0 && i < e->n_image) ? &e->images[i] : NULL;
}
const SvgClipRect* svg_extra_clip(const SvgExtra* e)
{
    return e ? &e->clip : NULL;
}

/* ======================================================================= *
 * Font resolution
 *
 * SVG names families the way CSS does ("Verdana,Geneva,DejaVu Sans,
 * sans-serif") and expects the host to own the mapping. We keep a short
 * per-platform table of the families that generated SVG actually asks for,
 * plus the three generics; anything unmatched lands on the platform sans, and
 * failing that on the font the app hands us.
 * ======================================================================= */

typedef struct { const char* name; const char* r, *b, *i, *bi; } FamilyEntry;

#if defined(_WIN32)
  #define FD "C:/Windows/Fonts/"
  static const FamilyEntry g_families[] = {
    {"verdana",        FD"verdana.ttf", FD"verdanab.ttf", FD"verdanai.ttf", FD"verdanaz.ttf"},
    {"dejavu sans",    FD"verdana.ttf", FD"verdanab.ttf", FD"verdanai.ttf", FD"verdanaz.ttf"},
    {"geneva",         FD"verdana.ttf", FD"verdanab.ttf", FD"verdanai.ttf", FD"verdanaz.ttf"},
    {"tahoma",         FD"tahoma.ttf",  FD"tahomabd.ttf", NULL,             NULL},
    {"arial",          FD"arial.ttf",   FD"arialbd.ttf",  FD"ariali.ttf",   FD"arialbi.ttf"},
    {"helvetica",      FD"arial.ttf",   FD"arialbd.ttf",  FD"ariali.ttf",   FD"arialbi.ttf"},
    {"helvetica neue", FD"arial.ttf",   FD"arialbd.ttf",  FD"ariali.ttf",   FD"arialbi.ttf"},
    {"segoe ui",       FD"segoeui.ttf", FD"segoeuib.ttf", FD"segoeuii.ttf", FD"segoeuiz.ttf"},
    {"calibri",        FD"calibri.ttf", FD"calibrib.ttf", FD"calibrii.ttf", FD"calibriz.ttf"},
    {"trebuchet ms",   FD"trebuc.ttf",  FD"trebucbd.ttf", FD"trebucit.ttf", FD"trebucbi.ttf"},
    {"georgia",        FD"georgia.ttf", FD"georgiab.ttf", FD"georgiai.ttf", FD"georgiaz.ttf"},
    {"times new roman",FD"times.ttf",   FD"timesbd.ttf",  FD"timesi.ttf",   FD"timesbi.ttf"},
    {"times",          FD"times.ttf",   FD"timesbd.ttf",  FD"timesi.ttf",   FD"timesbi.ttf"},
    {"serif",          FD"times.ttf",   FD"timesbd.ttf",  FD"timesi.ttf",   FD"timesbi.ttf"},
    {"courier new",    FD"cour.ttf",    FD"courbd.ttf",   FD"couri.ttf",    FD"courbi.ttf"},
    {"courier",        FD"cour.ttf",    FD"courbd.ttf",   FD"couri.ttf",    FD"courbi.ttf"},
    {"consolas",       FD"consola.ttf", FD"consolab.ttf", FD"consolai.ttf", FD"consolaz.ttf"},
    {"monospace",      FD"consola.ttf", FD"consolab.ttf", FD"consolai.ttf", FD"consolaz.ttf"},
    {"sans-serif",     FD"segoeui.ttf", FD"segoeuib.ttf", FD"segoeuii.ttf", FD"segoeuiz.ttf"},
    {"system-ui",      FD"segoeui.ttf", FD"segoeuib.ttf", FD"segoeuii.ttf", FD"segoeuiz.ttf"},
  };
  static const FamilyEntry g_generic_sans =
    {"sans-serif",     FD"segoeui.ttf", FD"segoeuib.ttf", FD"segoeuii.ttf", FD"segoeuiz.ttf"};
  static const char* const g_last_ditch[] = {
    FD"arial.ttf", FD"verdana.ttf", FD"tahoma.ttf", FD"consola.ttf", NULL };
#elif defined(__APPLE__)
  #define FS "/System/Library/Fonts/"
  #define FP FS"Supplemental/"
  static const FamilyEntry g_families[] = {
    {"verdana",        FP"Verdana.ttf",  FP"Verdana Bold.ttf",  FP"Verdana Italic.ttf",  FP"Verdana Bold Italic.ttf"},
    {"dejavu sans",    FP"Verdana.ttf",  FP"Verdana Bold.ttf",  FP"Verdana Italic.ttf",  FP"Verdana Bold Italic.ttf"},
    {"arial",          FP"Arial.ttf",    FP"Arial Bold.ttf",    FP"Arial Italic.ttf",    FP"Arial Bold Italic.ttf"},
    /* A .ttc is opened at face 0 (the regular cut), so the bold/italic slots
     * stay NULL and the synthesizer takes over rather than silently drawing
     * an upright, regular-weight glyph. */
    {"helvetica",      FS"Helvetica.ttc", NULL, NULL, NULL},
    {"helvetica neue", FS"HelveticaNeue.ttc", NULL, NULL, NULL},
    {"tahoma",         FP"Tahoma.ttf",   FP"Tahoma Bold.ttf",   NULL,                    NULL},
    {"trebuchet ms",   FP"Trebuchet MS.ttf", FP"Trebuchet MS Bold.ttf", FP"Trebuchet MS Italic.ttf", FP"Trebuchet MS Bold Italic.ttf"},
    {"georgia",        FP"Georgia.ttf",  FP"Georgia Bold.ttf",  FP"Georgia Italic.ttf",  FP"Georgia Bold Italic.ttf"},
    {"times new roman",FP"Times New Roman.ttf", FP"Times New Roman Bold.ttf", FP"Times New Roman Italic.ttf", FP"Times New Roman Bold Italic.ttf"},
    {"times",          FS"Times.ttc",    NULL, NULL, NULL},
    {"serif",          FP"Times New Roman.ttf", FP"Times New Roman Bold.ttf", FP"Times New Roman Italic.ttf", FP"Times New Roman Bold Italic.ttf"},
    {"courier new",    FP"Courier New.ttf", FP"Courier New Bold.ttf", FP"Courier New Italic.ttf", FP"Courier New Bold Italic.ttf"},
    {"menlo",          FS"Menlo.ttc",    NULL, NULL, NULL},
    {"monospace",      FS"Menlo.ttc",    NULL, NULL, NULL},
    {"sans-serif",     FS"Helvetica.ttc",NULL, NULL, NULL},
    {"system-ui",      FS"SFNS.ttf",     NULL, NULL, NULL},
  };
  static const FamilyEntry g_generic_sans =
    {"sans-serif",     FS"Helvetica.ttc",NULL, NULL, NULL};
  static const char* const g_last_ditch[] = {
    FS"Helvetica.ttc", FP"Arial.ttf", FS"Menlo.ttc", NULL };
#else
  #define FDV "/usr/share/fonts/truetype/dejavu/"
  #define FLB "/usr/share/fonts/truetype/liberation/"
  #define FNO "/usr/share/fonts/truetype/noto/"
  static const FamilyEntry g_families[] = {
    {"dejavu sans",    FDV"DejaVuSans.ttf", FDV"DejaVuSans-Bold.ttf", FDV"DejaVuSans-Oblique.ttf", FDV"DejaVuSans-BoldOblique.ttf"},
    {"verdana",        FDV"DejaVuSans.ttf", FDV"DejaVuSans-Bold.ttf", FDV"DejaVuSans-Oblique.ttf", FDV"DejaVuSans-BoldOblique.ttf"},
    {"arial",          FLB"LiberationSans-Regular.ttf", FLB"LiberationSans-Bold.ttf", FLB"LiberationSans-Italic.ttf", FLB"LiberationSans-BoldItalic.ttf"},
    {"helvetica",      FLB"LiberationSans-Regular.ttf", FLB"LiberationSans-Bold.ttf", FLB"LiberationSans-Italic.ttf", FLB"LiberationSans-BoldItalic.ttf"},
    {"liberation sans",FLB"LiberationSans-Regular.ttf", FLB"LiberationSans-Bold.ttf", FLB"LiberationSans-Italic.ttf", FLB"LiberationSans-BoldItalic.ttf"},
    {"noto sans",      FNO"NotoSans-Regular.ttf", FNO"NotoSans-Bold.ttf", FNO"NotoSans-Italic.ttf", FNO"NotoSans-BoldItalic.ttf"},
    {"times new roman",FLB"LiberationSerif-Regular.ttf", FLB"LiberationSerif-Bold.ttf", FLB"LiberationSerif-Italic.ttf", FLB"LiberationSerif-BoldItalic.ttf"},
    {"serif",          FDV"DejaVuSerif.ttf", FDV"DejaVuSerif-Bold.ttf", FDV"DejaVuSerif-Italic.ttf", FDV"DejaVuSerif-BoldItalic.ttf"},
    {"courier new",    FLB"LiberationMono-Regular.ttf", FLB"LiberationMono-Bold.ttf", FLB"LiberationMono-Italic.ttf", FLB"LiberationMono-BoldItalic.ttf"},
    {"monospace",      FDV"DejaVuSansMono.ttf", FDV"DejaVuSansMono-Bold.ttf", FDV"DejaVuSansMono-Oblique.ttf", FDV"DejaVuSansMono-BoldOblique.ttf"},
    {"sans-serif",     FDV"DejaVuSans.ttf", FDV"DejaVuSans-Bold.ttf", FDV"DejaVuSans-Oblique.ttf", FDV"DejaVuSans-BoldOblique.ttf"},
    {"system-ui",      FDV"DejaVuSans.ttf", FDV"DejaVuSans-Bold.ttf", FDV"DejaVuSans-Oblique.ttf", FDV"DejaVuSans-BoldOblique.ttf"},
  };
  static const FamilyEntry g_generic_sans =
    {"sans-serif",     FDV"DejaVuSans.ttf", FDV"DejaVuSans-Bold.ttf", FDV"DejaVuSans-Oblique.ttf", FDV"DejaVuSans-BoldOblique.ttf"};
  static const char* const g_last_ditch[] = {
    FDV"DejaVuSans.ttf", FLB"LiberationSans-Regular.ttf",
    FNO"NotoSans-Regular.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf", NULL };
#endif

static char g_fallback_font[512];

void svg_extra_set_fallback_font(const char* ttf_path)
{
    if (!ttf_path) { g_fallback_font[0] = 0; return; }
    snprintf(g_fallback_font, sizeof g_fallback_font, "%s", ttf_path);
}

static int sx_file_exists(const char* p)
{
    if (!p || !*p) return 0;
    FILE* f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* Pick the closest available variant, and say what still has to be faked. */
static const char* pick_variant(const FamilyEntry* fe, int bold, int italic,
                                int* synth_bold, int* synth_italic)
{
    const char* cands[4];
    int wb[4], wi[4], n = 0;
    if (bold && italic) {
        cands[n] = fe->bi; wb[n] = 0; wi[n] = 0; ++n;
        cands[n] = fe->b;  wb[n] = 0; wi[n] = 1; ++n;
        cands[n] = fe->i;  wb[n] = 1; wi[n] = 0; ++n;
    } else if (bold) {
        cands[n] = fe->b;  wb[n] = 0; wi[n] = 0; ++n;
    } else if (italic) {
        cands[n] = fe->i;  wb[n] = 0; wi[n] = 0; ++n;
    }
    cands[n] = fe->r; wb[n] = bold; wi[n] = italic; ++n;

    for (int k = 0; k < n; ++k) {
        if (!cands[k] || !sx_file_exists(cands[k])) continue;
        *synth_bold   = wb[k];
        *synth_italic = wi[k];
        return cands[k];
    }
    return NULL;
}

/* Walk the CSS family list left to right; first family we have wins. */
static const char* resolve_font(const char* family_list, int bold, int italic,
                                int* synth_bold, int* synth_italic)
{
    *synth_bold = bold;
    *synth_italic = italic;

    const char* p = family_list ? family_list : "";
    while (*p) {
        while (*p == ',' || isspace((unsigned char)*p)) ++p;
        const char* s = p;
        while (*p && *p != ',') ++p;
        const char* e = p;
        while (e > s && isspace((unsigned char)e[-1])) --e;
        if (e > s && (*s == '"' || *s == '\'') && e[-1] == *s) { ++s; --e; }
        size_t n = (size_t)(e - s);
        if (n && n < 64) {
            char name[64];
            for (size_t i = 0; i < n; ++i)
                name[i] = (char)tolower((unsigned char)s[i]);
            name[n] = 0;
            for (size_t i = 0; i < sizeof g_families / sizeof g_families[0]; ++i) {
                if (strcmp(g_families[i].name, name) != 0) continue;
                const char* path = pick_variant(&g_families[i], bold, italic,
                                                synth_bold, synth_italic);
                if (path) return path;
                break;
            }
        }
    }
    const char* path = pick_variant(&g_generic_sans, bold, italic,
                                    synth_bold, synth_italic);
    if (path) return path;
    for (int i = 0; g_last_ditch[i]; ++i)
        if (sx_file_exists(g_last_ditch[i])) {
            *synth_bold = bold; *synth_italic = italic;
            return g_last_ditch[i];
        }
    if (sx_file_exists(g_fallback_font)) {
        *synth_bold = bold; *synth_italic = italic;
        return g_fallback_font;
    }
    return NULL;
}

/* ======================================================================= *
 * Face cache (main thread only)
 * ======================================================================= */

typedef struct SxFace {
    char           path[512];
    int            px, bold, italic;
    FT_Face        ft;
    hb_font_t*     hb;
    struct SxFace* next;
} SxFace;

static FT_Library     g_ft;
static int            g_ft_failed;
static SxFace*        g_faces;
static NSVGrasterizer* g_img_rast;      /* for nested SVG logos, below */

static SxFace* face_get(const char* path, int px, int synth_bold, int synth_italic)
{
    if (!path || px < 1) return NULL;
    for (SxFace* f = g_faces; f; f = f->next)
        if (f->px == px && f->bold == synth_bold && f->italic == synth_italic &&
            strcmp(f->path, path) == 0)
            return f;

    if (!g_ft) {
        if (g_ft_failed) return NULL;
        if (FT_Init_FreeType(&g_ft) != 0) { g_ft_failed = 1; return NULL; }
    }

    SxFace* f = calloc(1, sizeof *f);
    if (!f) return NULL;
    if (FT_New_Face(g_ft, path, 0, &f->ft) != 0) { free(f); return NULL; }
    if (FT_Set_Pixel_Sizes(f->ft, 0, (FT_UInt)px) != 0) {
        FT_Done_Face(f->ft); free(f); return NULL;
    }
    if (synth_italic) {
        /* Same shear font.c uses for a faked italic. */
        FT_Matrix m = { 0x10000, (FT_Fixed)(0.21 * 0x10000), 0, 0x10000 };
        FT_Set_Transform(f->ft, &m, NULL);
    }
    snprintf(f->path, sizeof f->path, "%s", path);
    f->px     = px;
    f->bold   = synth_bold;
    f->italic = synth_italic;
    f->hb     = hb_ft_font_create_referenced(f->ft);
    f->next   = g_faces;
    g_faces   = f;
    return f;
}

void svg_extra_shutdown(void)
{
    SxFace* f = g_faces;
    while (f) {
        SxFace* n = f->next;
        if (f->hb) hb_font_destroy(f->hb);
        FT_Done_Face(f->ft);
        free(f);
        f = n;
    }
    g_faces = NULL;
    if (g_ft) { FT_Done_FreeType(g_ft); g_ft = NULL; }
    g_ft_failed = 0;
    if (g_img_rast) { nsvgDeleteRasterizer(g_img_rast); g_img_rast = NULL; }
}

/* ======================================================================= *
 * Compositing
 * ======================================================================= */

/* A coordinate we are willing to hand to lround. "1e999" parses to infinity
 * and a transform can turn that into a NaN, and every ordering test against a
 * NaN is false, so the range check has to be written positively. */
static int ok_f(float v)
{
    return v >= -1.0e7f && v <= 1.0e7f;
}

/* Source-over onto a straight-alpha RGBA pixel. */
static void blend_px(unsigned char* d, unsigned r, unsigned g, unsigned b, float sa)
{
    if (sa <= 0.0f) return;
    if (sa > 1.0f) sa = 1.0f;
    float da = d[3] / 255.0f;
    float oa = sa + da * (1.0f - sa);
    if (oa <= 0.0001f) { d[0] = d[1] = d[2] = d[3] = 0; return; }
    float inv = 1.0f / oa;
    const float src[3] = { (float)r, (float)g, (float)b };
    for (int i = 0; i < 3; ++i) {
        float v = (src[i] * sa + d[i] * da * (1.0f - sa)) * inv + 0.5f;
        d[i] = (unsigned char)(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v));
    }
    d[3] = (unsigned char)(oa * 255.0f + 0.5f);
}

static void blit_glyph(unsigned char* px, int w, int h, const FT_Bitmap* bm,
                       int x0, int y0, unsigned r, unsigned g, unsigned b,
                       float alpha)
{
    for (unsigned row = 0; row < bm->rows; ++row) {
        int y = y0 + (int)row;
        if (y < 0 || y >= h) continue;
        const unsigned char* s = bm->buffer + (int)row * bm->pitch;
        for (unsigned col = 0; col < bm->width; ++col) {
            int x = x0 + (int)col;
            if (x < 0 || x >= w) continue;
            unsigned cov;
            if (bm->pixel_mode == FT_PIXEL_MODE_MONO)
                cov = ((s[col >> 3] >> (7 - (col & 7))) & 1) ? 255u : 0u;
            else
                cov = s[col];
            if (!cov) continue;
            blend_px(px + ((size_t)y * w + x) * 4, r, g, b,
                     alpha * (cov / 255.0f));
        }
    }
}

static void draw_text_run(const SvgTextRun* t, unsigned char* px, int w, int h,
                          float scale)
{
    if (!t->text || !t->text[0]) return;
    float alpha = t->opacity;
    if (!(alpha > 0.004f)) return;
    if (!ok_f(t->x) || !ok_f(t->y) || !ok_f(t->length)) return;

    float size_px = t->size * scale;
    if (!(size_px >= 1.0f && size_px <= 2048.0f)) return;

    int synth_bold = 0, synth_italic = 0;
    const char* path = resolve_font(t->family, t->bold, t->italic,
                                    &synth_bold, &synth_italic);
    SxFace* f = face_get(path, (int)lroundf(size_px), synth_bold, synth_italic);
    if (!f || !f->hb) return;

    hb_buffer_t* buf = hb_buffer_create();
    if (!buf) return;
    hb_buffer_add_utf8(buf, t->text, -1, 0, -1);
    hb_buffer_guess_segment_properties(buf);
    hb_shape(f->hb, buf, NULL, 0);

    unsigned n = 0;
    hb_glyph_info_t*     gi = hb_buffer_get_glyph_infos(buf, &n);
    hb_glyph_position_t* gp = hb_buffer_get_glyph_positions(buf, &n);

    double natural = 0.0;
    for (unsigned i = 0; i < n; ++i) natural += gp[i].x_advance / 64.0;

    /* textLength is how a badge guarantees its label fits the coloured box.
     * SVG's default lengthAdjust is "spacing": stretch the gaps, leave the
     * glyph shapes alone -- which is exactly scaling the advances. */
    double want = (t->length > 0.0f) ? (double)t->length * scale : natural;
    double k    = (t->length > 0.0f && natural > 0.5) ? want / natural : 1.0;

    double pen = (double)t->x * scale;
    if      (t->anchor == SVG_ANCHOR_MIDDLE) pen -= want / 2.0;
    else if (t->anchor == SVG_ANCHOR_END)    pen -= want;
    double base_y = (double)t->y * scale;

    double adv = 0.0;
    for (unsigned i = 0; i < n; ++i) {
        int load = FT_LOAD_TARGET_LIGHT;
        if (!synth_bold) load |= FT_LOAD_RENDER;
        if (FT_Load_Glyph(f->ft, gi[i].codepoint, load) == 0) {
            if (synth_bold) {
                if (f->ft->glyph->format == FT_GLYPH_FORMAT_OUTLINE)
                    FT_Outline_Embolden(&f->ft->glyph->outline, 64);
                if (f->ft->glyph->format != FT_GLYPH_FORMAT_BITMAP)
                    FT_Render_Glyph(f->ft->glyph, FT_RENDER_MODE_LIGHT);
            }
            FT_GlyphSlot gs = f->ft->glyph;
            if (gs->bitmap.width && gs->bitmap.rows) {
                double gx = pen + adv * k + gp[i].x_offset / 64.0;
                double gy = base_y - gp[i].y_offset / 64.0;
                blit_glyph(px, w, h, &gs->bitmap,
                           (int)lround(gx) + gs->bitmap_left,
                           (int)lround(gy) - gs->bitmap_top,
                           t->r, t->g, t->b, alpha);
            }
        }
        adv += gp[i].x_advance / 64.0;
    }
    hb_buffer_destroy(buf);
}

/* ---------- data: URIs -------------------------------------------------- */

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

/* Always NUL-terminates so the result can be handed straight to nsvgParse. */
static char* b64_decode(const char* s, size_t n, size_t* out_n)
{
    char* out = malloc(n / 4 * 3 + 4);
    if (!out) return NULL;
    size_t o = 0;
    unsigned acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; ++i) {
        int v = b64_val(s[i]);
        if (v < 0) continue;                 /* whitespace, '=' padding */
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out[o++] = (char)((acc >> bits) & 0xFF); }
    }
    out[o] = 0;
    *out_n = o;
    return out;
}

static char* pct_decode(const char* s, size_t n, size_t* out_n)
{
    char* out = malloc(n + 2);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == '%' && i + 2 < n) {
            int hi = hexval(s[i+1]), lo = hexval(s[i+2]);
            if (hi >= 0 && lo >= 0) { out[o++] = (char)(hi * 16 + lo); i += 2; continue; }
        }
        out[o++] = s[i];
    }
    out[o] = 0;
    *out_n = o;
    return out;
}

/* Badge logos are nested SVG data URIs. Rasterize one into `px` at its
 * declared box, honouring the default preserveAspectRatio (xMidYMid meet). */
static void draw_image_ref(const SvgImageRef* im, unsigned char* px, int w, int h,
                           float scale)
{
    const char* href = im->href;
    if (!href || strncmp(href, "data:", 5) != 0) return;
    if (!ok_f(im->x) || !ok_f(im->y) || !ok_f(im->w) || !ok_f(im->h)) return;
    const char* comma = strchr(href, ',');
    if (!comma) return;
    size_t meta_n = (size_t)(comma - href);
    /* Only SVG payloads: a raster data URI would need the PNG/JPEG decoders,
     * which live on the other side of image.c. Named badge logos are SVG. */
    {
        char meta[128];
        size_t mn = meta_n < sizeof meta - 1 ? meta_n : sizeof meta - 1;
        memcpy(meta, href, mn);
        meta[mn] = 0;
        if (!strstr(meta, "image/svg+xml")) return;
    }
    int is_b64 = (meta_n > 7 && memcmp(comma - 7, ";base64", 7) == 0);

    const char* payload = comma + 1;
    size_t      pay_n   = strlen(payload);
    size_t      raw_n   = 0;
    char* raw = is_b64 ? b64_decode(payload, pay_n, &raw_n)
                       : pct_decode(payload, pay_n, &raw_n);
    if (!raw) return;
    if (raw_n == 0) { free(raw); return; }

    NSVGimage* img = nsvgParse(raw, "px", 96.0f);   /* mutates `raw` */
    free(raw);
    if (!img) return;

    int tw = (int)lround((double)im->w * scale);
    int th = (int)lround((double)im->h * scale);
    if (tw < 1 || th < 1 || tw > 4096 || th > 4096 ||
        img->width <= 0.0f || img->height <= 0.0f) { nsvgDelete(img); return; }

    float sx = tw / img->width, sy = th / img->height;
    float s  = sx < sy ? sx : sy;                    /* "meet" */
    float tx = (tw - img->width  * s) * 0.5f;
    float ty = (th - img->height * s) * 0.5f;

    if (!g_img_rast) g_img_rast = nsvgCreateRasterizer();
    unsigned char* tmp = g_img_rast ? calloc((size_t)tw * th * 4, 1) : NULL;
    if (!tmp) { nsvgDelete(img); return; }
    nsvgRasterize(g_img_rast, img, tx, ty, s, tmp, tw, th, tw * 4);
    nsvgDelete(img);

    int x0 = (int)lround((double)im->x * scale);
    int y0 = (int)lround((double)im->y * scale);
    for (int y = 0; y < th; ++y) {
        int dy = y0 + y;
        if (dy < 0 || dy >= h) continue;
        for (int x = 0; x < tw; ++x) {
            int dx = x0 + x;
            if (dx < 0 || dx >= w) continue;
            const unsigned char* s4 = tmp + ((size_t)y * tw + x) * 4;
            unsigned a = s4[3];
            if (!a) continue;
            /* nsvgRasterize un-premultiplies (and defringes) before it
             * returns, so this is already straight alpha. */
            blend_px(px + ((size_t)dy * w + dx) * 4,
                     s4[0], s4[1], s4[2], a / 255.0f);
        }
    }
    free(tmp);
}

/* ---------- rounded-corner clip ---------------------------------------- */

/* Only a clip rect that covers the whole canvas is applied, and only for its
 * corners: that is the badge case, and it means a clipPath aimed at one small
 * sub-element can never wipe out the rest of the picture. */
static void apply_clip(const SvgClipRect* c, unsigned char* px, int w, int h,
                       float scale)
{
    if (!ok_f(c->x) || !ok_f(c->y) || !ok_f(c->w) || !ok_f(c->h) ||
        !ok_f(c->rx) || !ok_f(c->ry)) return;
    float x0 = c->x * scale, y0 = c->y * scale;
    float cw = c->w * scale, ch = c->h * scale;
    if (!(x0 <= 0.75f && y0 <= 0.75f && cw >= w - 1.5f && ch >= h - 1.5f)) return;

    float r = c->rx > 0 ? c->rx : c->ry;
    if (c->ry > 0 && c->ry < r) r = c->ry;
    r *= scale;
    if (r < 0.35f) return;                       /* square corners: nothing to do */
    float maxr = (w < h ? w : h) * 0.5f;
    if (r > maxr) r = maxr;

    const float cx = w * 0.5f, cy = h * 0.5f;
    const float hx = w * 0.5f, hy = h * 0.5f;
    int span = (int)ceilf(r) + 1;
    if (span > w) span = w;
    if (span > h) span = h;

    for (int cyi = 0; cyi < 2; ++cyi) {
        for (int cxi = 0; cxi < 2; ++cxi) {
            int bx = cxi ? w - span : 0;
            int by = cyi ? h - span : 0;
            for (int y = by; y < by + span; ++y) {
                for (int x = bx; x < bx + span; ++x) {
                    /* Signed distance to a rounded rect, then a one-pixel
                     * ramp across the edge for anti-aliasing. */
                    float qx = fabsf(x + 0.5f - cx) - (hx - r);
                    float qy = fabsf(y + 0.5f - cy) - (hy - r);
                    float ax = qx > 0 ? qx : 0.0f;
                    float ay = qy > 0 ? qy : 0.0f;
                    float mq = qx > qy ? qx : qy;
                    float d  = sqrtf(ax*ax + ay*ay) + (mq < 0 ? mq : 0.0f) - r;
                    float cov = 0.5f - d;
                    if (cov >= 1.0f) continue;
                    if (cov < 0.0f) cov = 0.0f;
                    unsigned char* p = px + ((size_t)y * w + x) * 4;
                    p[3] = (unsigned char)(p[3] * cov + 0.5f);
                }
            }
        }
    }
}

void svg_extra_render(const SvgExtra* e, unsigned char* rgba, int w, int h,
                      float scale)
{
    if (!e || !rgba || w <= 0 || h <= 0 || scale <= 0.0f) return;

    if (e->clip.present) apply_clip(&e->clip, rgba, w, h, scale);
    for (int i = 0; i < e->n_image; ++i)
        draw_image_ref(&e->images[i], rgba, w, h, scale);
    for (int i = 0; i < e->n_text; ++i)
        draw_text_run(&e->texts[i], rgba, w, h, scale);
}

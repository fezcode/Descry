#include "markdown.h"

#include <md4c.h>

#include <stdlib.h>
#include <string.h>

typedef struct {
    MdDoc*        doc;
    LineKind      cur_kind;
    int           list_depth;
    int           quote_depth;
    size_t        line_start;
    int           in_text_block;
    int           skip_text;     /* in image span: skip alt text          */
    unsigned char style_mask;
    /* Pending task-list info from MD_BLOCK_LI; copied into the next emitted
     * line then cleared. */
    size_t        task_mark_off;
    int           task_pending;  /* 1 = next emitted line is a task line  */
    /* Table parsing state. Cells inside a row get tab-separated. */
    int           in_table;
    int           in_thead;
    int           in_row;
    /* Pending inline `[text](url)` link: start byte in doc->data and the
     * heap-allocated href. Captured on MD_SPAN_A enter, consumed on leave. */
    int           link_pending;
    size_t        link_start;
    char*         link_href;
} ParseCtx;

static void buffers_reserve(MdDoc* d, size_t extra)
{
    if (d->len + extra + 1 <= d->cap) return;
    size_t nc = d->cap ? d->cap * 2 : 1024;
    while (nc < d->len + extra + 1) nc *= 2;
    d->data  = realloc(d->data,  nc);
    d->style = realloc(d->style, nc);
    d->cap   = nc;
}

static void data_append_styled(MdDoc* d, const char* s, size_t n,
                               unsigned char style)
{
    buffers_reserve(d, n);
    memcpy(d->data + d->len, s, n);
    memset(d->style + d->len, style, n);
    d->len += n;
    d->data[d->len]  = 0;
    d->style[d->len] = 0;
}

static void lines_reserve(MdDoc* d, size_t extra)
{
    if (d->line_count + extra <= d->line_cap) return;
    size_t nc = d->line_cap ? d->line_cap * 2 : 64;
    while (nc < d->line_count + extra) nc *= 2;
    d->lines = realloc(d->lines, nc * sizeof(MdLine));
    d->line_cap = nc;
}

static void emit_line(ParseCtx* c, LineKind kind, int indent,
                      size_t start, size_t len)
{
    lines_reserve(c->doc, 1);
    size_t tmo = 0;
    if (c->task_pending &&
        (kind == LINE_LIST_TASK_OPEN || kind == LINE_LIST_TASK_DONE))
    {
        tmo = c->task_mark_off;
        c->task_pending = 0;
    }
    c->doc->lines[c->doc->line_count++] = (MdLine){
        .kind = kind, .indent = indent, .start = start, .len = len,
        .task_mark_off = tmo, .cached_h = -1,
    };
}

static void finalize_current_line(ParseCtx* c)
{
    if (!c->in_text_block) return;
    size_t start = c->line_start;
    size_t len   = c->doc->len - start;
    LineKind kind = c->cur_kind;
    int indent    = c->list_depth;
    if (c->quote_depth > 0 && c->list_depth == 0 && kind == LINE_NORMAL)
        kind = LINE_QUOTE;
    emit_line(c, kind, indent, start, len);
    c->line_start = c->doc->len;
}

static void emit_blank(ParseCtx* c)
{
    if (c->doc->line_count > 0 &&
        c->doc->lines[c->doc->line_count - 1].kind == LINE_BLANK) return;
    emit_line(c, LINE_BLANK, 0, c->doc->len, 0);
}

static int cb_enter_block(MD_BLOCKTYPE t, void* detail, void* ud)
{
    ParseCtx* c = ud;
    switch (t) {
        case MD_BLOCK_DOC: break;
        case MD_BLOCK_QUOTE: c->quote_depth++; break;
        case MD_BLOCK_UL:
        case MD_BLOCK_OL:    c->list_depth++; break;
        case MD_BLOCK_LI: {
            MD_BLOCK_LI_DETAIL* d = detail;
            if (d && d->is_task) {
                c->cur_kind = (d->task_mark == 'x' || d->task_mark == 'X')
                              ? LINE_LIST_TASK_DONE : LINE_LIST_TASK_OPEN;
                c->task_mark_off = d->task_mark_offset;
                c->task_pending  = 1;
            } else {
                c->cur_kind = LINE_LIST;
            }
            c->in_text_block = 1;
            c->line_start = c->doc->len;
            break;
        }
        case MD_BLOCK_H: {
            MD_BLOCK_H_DETAIL* d = detail;
            unsigned lvl = d->level < 1 ? 1 : (d->level > 6 ? 6 : d->level);
            c->cur_kind = (LineKind)(LINE_H1 + (lvl - 1));
            c->in_text_block = 1;
            c->line_start = c->doc->len;
            break;
        }
        case MD_BLOCK_CODE: {
            /* A ```mermaid fence renders as a diagram, not code text. Tag its
             * lines LINE_MERMAID (case-insensitive on the info string). */
            MD_BLOCK_CODE_DETAIL* d = detail;
            int is_mermaid = 0;
            if (d && d->lang.text && d->lang.size == 7) {
                static const char* M = "mermaid";
                is_mermaid = 1;
                for (int k = 0; k < 7; ++k) {
                    char ch = d->lang.text[k];
                    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
                    if (ch != M[k]) { is_mermaid = 0; break; }
                }
            }
            c->cur_kind = is_mermaid ? LINE_MERMAID : LINE_CODE;
            c->in_text_block = 1;
            c->line_start = c->doc->len;
            break;
        }
        case MD_BLOCK_P:
            if (!c->in_text_block) {
                c->cur_kind   = (c->quote_depth > 0) ? LINE_QUOTE : LINE_NORMAL;
                c->in_text_block = 1;
                c->line_start = c->doc->len;
            }
            break;
        case MD_BLOCK_TABLE:  c->in_table = 1; c->in_thead = 0; break;
        case MD_BLOCK_THEAD:  c->in_thead = 1; break;
        case MD_BLOCK_TBODY:  c->in_thead = 0; break;
        case MD_BLOCK_TR:
            c->in_row = 1;
            c->cur_kind = c->in_thead ? LINE_TABLE_HEAD : LINE_TABLE_ROW;
            c->in_text_block = 1;
            c->line_start = c->doc->len;
            break;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD:
            /* Cell content arrives via cb_text; nothing to do on enter. */
            break;
        default: break;
    }
    return 0;
}

static int cb_leave_block(MD_BLOCKTYPE t, void* detail, void* ud)
{
    (void)detail;
    ParseCtx* c = ud;
    switch (t) {
        case MD_BLOCK_QUOTE:
            c->quote_depth--;
            if (c->quote_depth == 0 && c->list_depth == 0) emit_blank(c);
            break;
        case MD_BLOCK_UL:
        case MD_BLOCK_OL:
            c->list_depth--;
            if (c->list_depth == 0 && c->quote_depth == 0) emit_blank(c);
            break;
        case MD_BLOCK_LI:
            finalize_current_line(c);
            c->in_text_block = 0;
            break;
        case MD_BLOCK_H:
            finalize_current_line(c);
            c->in_text_block = 0;
            emit_blank(c);
            break;
        case MD_BLOCK_CODE:
            finalize_current_line(c);
            c->in_text_block = 0;
            emit_blank(c);
            break;
        case MD_BLOCK_P:
            finalize_current_line(c);
            c->in_text_block = 0;
            if (c->quote_depth == 0 && c->list_depth == 0) emit_blank(c);
            break;
        case MD_BLOCK_TABLE:
            c->in_table = 0;
            emit_blank(c);
            break;
        case MD_BLOCK_THEAD:  c->in_thead = 0; break;
        case MD_BLOCK_TBODY:  break;
        case MD_BLOCK_TR:
            /* Strip trailing tab separator from the last cell. */
            if (c->doc->len > c->line_start &&
                c->doc->data[c->doc->len - 1] == '\t') {
                c->doc->len--;
                c->doc->data[c->doc->len]  = 0;
                c->doc->style[c->doc->len] = 0;
            }
            finalize_current_line(c);
            c->in_text_block = 0;
            c->in_row        = 0;
            break;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD:
            /* Append a tab so the renderer can split the row into cells. */
            data_append_styled(c->doc, "\t", 1, 0);
            break;
        default: break;
    }
    return 0;
}

/* ---- LaTeX-math prettifier --------------------------------------------
 * `$…$` / `$$…$$` spans (MD_FLAG_LATEXMATHSPANS) are rendered, not shown raw.
 * This is deliberately a *typographic* pass, not a TeX engine: it swaps common
 * `\command` tokens for their Unicode glyphs and lifts simple `^`/`_` operands
 * into Unicode super/subscripts. Anything it can't map degrades to plain text
 * (minus the backslash / braces). Glyphs are UTF-8 literals (this file, like
 * the rest of the tree, is UTF-8). */

static int math_superscript(char ch, char out[4])
{
    const char* r = NULL;
    switch (ch) {
        case '0': r = "⁰"; break; case '1': r = "¹"; break;
        case '2': r = "²"; break; case '3': r = "³"; break;
        case '4': r = "⁴"; break; case '5': r = "⁵"; break;
        case '6': r = "⁶"; break; case '7': r = "⁷"; break;
        case '8': r = "⁸"; break; case '9': r = "⁹"; break;
        case '+': r = "⁺"; break; case '-': r = "⁻"; break;
        case '=': r = "⁼"; break; case '(': r = "⁽"; break;
        case ')': r = "⁾"; break; case 'n': r = "ⁿ"; break;
        case 'i': r = "ⁱ"; break;
        default: return 0;
    }
    size_t l = strlen(r); memcpy(out, r, l); return (int)l;
}

static int math_subscript(char ch, char out[4])
{
    const char* r = NULL;
    switch (ch) {
        case '0': r = "₀"; break; case '1': r = "₁"; break;
        case '2': r = "₂"; break; case '3': r = "₃"; break;
        case '4': r = "₄"; break; case '5': r = "₅"; break;
        case '6': r = "₆"; break; case '7': r = "₇"; break;
        case '8': r = "₈"; break; case '9': r = "₉"; break;
        case '+': r = "₊"; break; case '-': r = "₋"; break;
        case '=': r = "₌"; break; case '(': r = "₍"; break;
        case ')': r = "₎"; break;
        case 'a': r = "ₐ"; break; case 'e': r = "ₑ"; break;
        case 'o': r = "ₒ"; break; case 'x': r = "ₓ"; break;
        case 'h': r = "ₕ"; break; case 'k': r = "ₖ"; break;
        case 'l': r = "ₗ"; break; case 'm': r = "ₘ"; break;
        case 'n': r = "ₙ"; break; case 'p': r = "ₚ"; break;
        case 's': r = "ₛ"; break; case 't': r = "ₜ"; break;
        case 'i': r = "ᵢ"; break; case 'j': r = "ⱼ"; break;
        default: return 0;
    }
    size_t l = strlen(r); memcpy(out, r, l); return (int)l;
}

/* Map a `\name` control word to a Unicode glyph. Returns "" for tokens we
 * recognize but intentionally drop (grouping/sizing), or NULL if unknown. */
static const char* math_symbol(const char* name, size_t len)
{
    static const struct { const char* k; const char* v; } T[] = {
        {"alpha","α"},{"beta","β"},{"gamma","γ"},{"delta","δ"},
        {"epsilon","ε"},{"varepsilon","ε"},{"zeta","ζ"},{"eta","η"},
        {"theta","θ"},{"iota","ι"},{"kappa","κ"},{"lambda","λ"},
        {"mu","μ"},{"nu","ν"},{"xi","ξ"},{"pi","π"},{"rho","ρ"},
        {"sigma","σ"},{"tau","τ"},{"phi","φ"},{"varphi","ϕ"},
        {"chi","χ"},{"psi","ψ"},{"omega","ω"},
        {"Gamma","Γ"},{"Delta","Δ"},{"Theta","Θ"},{"Lambda","Λ"},
        {"Xi","Ξ"},{"Pi","Π"},{"Sigma","Σ"},{"Phi","Φ"},
        {"Psi","Ψ"},{"Omega","Ω"},
        {"times","×"},{"div","÷"},{"cdot","·"},{"pm","±"},
        {"mp","∓"},{"ast","∗"},{"star","⋆"},{"circ","∘"},
        {"leq","≤"},{"le","≤"},{"geq","≥"},{"ge","≥"},
        {"neq","≠"},{"ne","≠"},{"approx","≈"},{"equiv","≡"},
        {"sim","∼"},{"simeq","≃"},{"cong","≅"},{"propto","∝"},
        {"infty","∞"},{"partial","∂"},{"nabla","∇"},
        {"forall","∀"},{"exists","∃"},{"neg","¬"},
        {"in","∈"},{"notin","∉"},{"ni","∋"},
        {"subset","⊂"},{"supset","⊃"},{"subseteq","⊆"},
        {"supseteq","⊇"},{"cup","∪"},{"cap","∩"},
        {"emptyset","∅"},{"varnothing","∅"},
        {"rightarrow","→"},{"to","→"},{"leftarrow","←"},
        {"gets","←"},{"Rightarrow","⇒"},{"Leftarrow","⇐"},
        {"leftrightarrow","↔"},{"Leftrightarrow","⇔"},{"mapsto","↦"},
        {"sum","∑"},{"prod","∏"},{"int","∫"},{"oint","∮"},
        {"sqrt","√"},{"angle","∠"},{"perp","⊥"},{"parallel","∥"},
        {"langle","⟨"},{"rangle","⟩"},{"lfloor","⌊"},{"rfloor","⌋"},
        {"lceil","⌈"},{"rceil","⌉"},
        {"cdots","⋯"},{"ldots","…"},{"dots","…"},{"vdots","⋮"},
        {"prime","′"},{"oplus","⊕"},{"otimes","⊗"},{"odot","⊙"},
        {"wedge","∧"},{"vee","∨"},{"land","∧"},{"lor","∨"},
        {"hbar","ℏ"},{"ell","ℓ"},{"Re","ℜ"},{"Im","ℑ"},
        {"aleph","ℵ"},{"degree","°"},{"deg","°"},
        /* recognized-but-dropped: sizing / grouping / fonts */
        {"left",""},{"right",""},{"big",""},{"Big",""},{"bigg",""},{"Bigg",""},
        {"displaystyle",""},{"limits",""},{"mathrm",""},{"mathbf",""},
        {"mathbb",""},{"mathcal",""},{"text",""},{"quad"," "},{"qquad","  "},
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; ++i)
        if (strlen(T[i].k) == len && memcmp(T[i].k, name, len) == 0)
            return T[i].v;
    return NULL;
}

/* Read a `{...}` group starting at s[i]=='{'. On success sets inner + inner_n
 * to the contents (braces stripped, nesting balanced) and returns the index
 * just past the closing brace. Returns i unchanged if s[i] isn't '{'. */
static size_t math_group(const char* s, size_t n, size_t i,
                         const char** inner, size_t* inner_n)
{
    if (i >= n || s[i] != '{') return i;
    size_t k = i + 1;
    int depth = 1;
    while (k < n && depth) {
        if (s[k] == '{') depth++;
        else if (s[k] == '}') { depth--; if (!depth) break; }
        k++;
    }
    *inner = s + i + 1;
    *inner_n = k - (i + 1);
    return (k < n) ? k + 1 : k;     /* past the '}' */
}

/* append_math recurses (into \frac groups and super/subscript operands); the
 * name is in scope within its own body, so no separate forward decl needed. */
static void append_math(MdDoc* d, const char* s, size_t n, unsigned char style)
{
    size_t i = 0;
    while (i < n) {
        char ch = s[i];
        if (ch == '\\') {
            size_t j = i + 1;
            while (j < n && ((s[j] >= 'a' && s[j] <= 'z') ||
                             (s[j] >= 'A' && s[j] <= 'Z'))) j++;
            size_t clen = j - (i + 1);
            if (clen == 0) {
                /* backslash + non-letter: \{ \} \_ \% … emit the char escaped;
                 * \, \; \: \space → thin space; other control symbols dropped. */
                if (j < n) {
                    char nx = s[j];
                    if (nx=='{'||nx=='}'||nx=='_'||nx=='%'||nx=='$'||
                        nx=='&'||nx=='#')
                        data_append_styled(d, &nx, 1, style);
                    else if (nx==','||nx==';'||nx==':'||nx==' ')
                        data_append_styled(d, " ", 1, style);
                    j++;
                }
                i = j;
                continue;
            }
            /* \frac{A}{B} → A⁄B (recurse so commands inside A/B expand too). */
            if (clen == 4 && memcmp(s + i + 1, "frac", 4) == 0) {
                size_t k = j;
                while (k < n && s[k] == ' ') k++;
                const char *na = NULL, *nb = NULL; size_t la = 0, lb = 0;
                size_t after_a = math_group(s, n, k, &na, &la);
                if (after_a != k) {
                    size_t m = after_a;
                    while (m < n && s[m] == ' ') m++;
                    size_t after_b = math_group(s, n, m, &nb, &lb);
                    if (after_b != m) {
                        append_math(d, na, la, style);
                        data_append_styled(d, "\xe2\x81\x84", 3, style); /* ⁄ */
                        append_math(d, nb, lb, style);
                        i = after_b;
                        continue;
                    }
                }
                /* malformed \frac: fall through to literal-name handling */
            }
            const char* sym = math_symbol(s + i + 1, clen);
            if (sym) {
                if (*sym) data_append_styled(d, sym, strlen(sym), style);
            } else {
                data_append_styled(d, s + i + 1, clen, style);  /* unknown name */
            }
            i = j;
            continue;
        }
        if (ch == '^' || ch == '_') {
            int sup = (ch == '^');
            size_t j = i + 1;
            const char* op = NULL; size_t opn = 0;
            char single = 0;
            if (j < n && s[j] == '{') {
                j = math_group(s, n, j, &op, &opn);
            } else if (j < n) {
                single = s[j]; op = &single; opn = 1; j++;
            }
            /* Try to lift the operand into Unicode super/subscripts. Works only
             * when every char maps (digits, +-=(), a few letters). */
            int ok = opn > 0;
            char buf[96]; size_t bn = 0;
            for (size_t t = 0; t < opn && ok; ++t) {
                char u[4];
                int ul = sup ? math_superscript(op[t], u)
                             : math_subscript(op[t], u);
                if (ul == 0 || bn + (size_t)ul > sizeof buf) { ok = 0; break; }
                memcpy(buf + bn, u, ul); bn += ul;
            }
            if (ok) {
                data_append_styled(d, buf, bn, style);
            } else {
                /* Fall back to a caret + the prettified operand (so e.g.
                 * `^{\infty}` → `^∞`, `^{i\pi}` → `^iπ`). */
                data_append_styled(d, sup ? "^" : "_", 1, style);
                if (opn) append_math(d, op, opn, style);
            }
            i = j;
            continue;
        }
        if (ch == '{' || ch == '}') { i++; continue; }   /* drop grouping */
        data_append_styled(d, &s[i], 1, style);
        i++;
    }
}

static int cb_enter_span(MD_SPANTYPE t, void* detail, void* ud)
{
    ParseCtx* c = ud;
    switch (t) {
        case MD_SPAN_STRONG: c->style_mask |= STYLE_BOLD;   break;
        case MD_SPAN_EM:     c->style_mask |= STYLE_ITALIC; break;
        case MD_SPAN_CODE:   c->style_mask |= STYLE_CODE;   break;
        case MD_SPAN_DEL:    c->style_mask |= STYLE_STRIKE; break;
        case MD_SPAN_LATEXMATH:
        case MD_SPAN_LATEXMATH_DISPLAY: c->style_mask |= STYLE_MATH; break;
        case MD_SPAN_A: {
            c->style_mask |= STYLE_LINK;
            MD_SPAN_A_DETAIL* d = detail;
            if (d && d->href.text && d->href.size > 0) {
                free(c->link_href);
                c->link_href = malloc(d->href.size + 1);
                memcpy(c->link_href, d->href.text, d->href.size);
                c->link_href[d->href.size] = 0;
                c->link_start   = c->doc->len;
                c->link_pending = 1;
            }
            break;
        }
        case MD_SPAN_IMG: {
            MD_SPAN_IMG_DETAIL* d = detail;
            /* Finalize whatever line is in flight (e.g. text before image). */
            if (c->in_text_block) finalize_current_line(c);
            /* Emit the image as its own LINE_IMAGE; bytes = src URL. */
            size_t s = c->doc->len;
            if (d->src.text && d->src.size > 0)
                data_append_styled(c->doc, d->src.text, d->src.size, 0);
            emit_line(c, LINE_IMAGE, c->list_depth, s, c->doc->len - s);
            /* Re-open a continuation line of the surrounding kind. */
            c->line_start    = c->doc->len;
            c->in_text_block = 1;
            c->skip_text     = 1;       /* skip the alt text inside the span */
            break;
        }
        default: break;
    }
    return 0;
}

static int cb_leave_span(MD_SPANTYPE t, void* detail, void* ud)
{
    (void)detail;
    ParseCtx* c = ud;
    switch (t) {
        case MD_SPAN_STRONG: c->style_mask &= ~STYLE_BOLD;   break;
        case MD_SPAN_EM:     c->style_mask &= ~STYLE_ITALIC; break;
        case MD_SPAN_CODE:   c->style_mask &= ~STYLE_CODE;   break;
        case MD_SPAN_DEL:    c->style_mask &= ~STYLE_STRIKE; break;
        case MD_SPAN_LATEXMATH:
        case MD_SPAN_LATEXMATH_DISPLAY: c->style_mask &= ~STYLE_MATH; break;
        case MD_SPAN_A:
            c->style_mask &= ~STYLE_LINK;
            if (c->link_pending) {
                if (c->doc->link_count >= c->doc->link_cap) {
                    c->doc->link_cap = c->doc->link_cap
                                       ? c->doc->link_cap * 2 : 8;
                    c->doc->links = realloc(c->doc->links,
                        c->doc->link_cap * sizeof(MdLink));
                }
                c->doc->links[c->doc->link_count++] = (MdLink){
                    .start = c->link_start,
                    .end   = c->doc->len,
                    .href  = c->link_href,
                };
                c->link_href    = NULL;
                c->link_pending = 0;
            }
            break;
        case MD_SPAN_IMG:    c->skip_text = 0;               break;
        default: break;
    }
    return 0;
}

static int cb_text(MD_TEXTTYPE t, const MD_CHAR* text, MD_SIZE size, void* ud)
{
    ParseCtx* c = ud;
    if (!c->in_text_block) return 0;
    if (c->skip_text)      return 0;

    /* LaTeX-math span content: prettify `\command`/`^`/`_` to Unicode instead
     * of emitting the raw TeX. (Math spans never carry soft/hard breaks.) */
    if (c->style_mask & STYLE_MATH) {
        append_math(c->doc, text, size, c->style_mask);
        return 0;
    }

    if (t == MD_TEXT_SOFTBR) {
        data_append_styled(c->doc, " ", 1, c->style_mask);
        return 0;
    }
    if (t == MD_TEXT_BR) {
        finalize_current_line(c);
        c->line_start = c->doc->len;
        return 0;
    }

    /* CODE blocks may contain embedded \n separating logical code lines. */
    const MD_CHAR* p   = text;
    const MD_CHAR* end = text + size;
    while (p < end) {
        const MD_CHAR* nl = memchr(p, '\n', (size_t)(end - p));
        size_t n = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (n) data_append_styled(c->doc, p, n, c->style_mask);
        if (nl) {
            finalize_current_line(c);
            c->line_start = c->doc->len;
            p = nl + 1;
        } else break;
    }
    return 0;
}

/* Post-parse pass: scan rendered text for Obsidian-style `[[wiki links]]`,
 * mark their bytes with STYLE_LINK, and record their positions so click
 * navigation can resolve them later. */
static void wikis_post(MdDoc* d)
{
    if (!d->data) return;
    for (size_t i = 0; i + 1 < d->len; ++i) {
        if (d->data[i] != '[' || d->data[i+1] != '[') continue;
        size_t end = i + 2;
        while (end + 1 < d->len &&
               !(d->data[end] == ']' && d->data[end+1] == ']')) {
            if (d->data[end] == '\n') break;     /* don't span line breaks */
            end++;
        }
        if (end + 1 >= d->len ||
            !(d->data[end] == ']' && d->data[end+1] == ']')) continue;

        for (size_t k = i; k < end + 2; ++k) d->style[k] |= STYLE_LINK;

        if (d->wiki_count >= d->wiki_cap) {
            d->wiki_cap = d->wiki_cap ? d->wiki_cap * 2 : 8;
            d->wikis = realloc(d->wikis, d->wiki_cap * sizeof(MdWiki));
        }
        d->wikis[d->wiki_count++] = (MdWiki){
            .start      = i,
            .end        = end + 2,
            .name_start = i + 2,
            .name_len   = end - (i + 2),
        };
        i = end + 1;
    }
}

int md_doc_parse(const char* src, size_t src_len, MdDoc* out)
{
    memset(out, 0, sizeof *out);
    ParseCtx ctx = { .doc = out };

    /* Strip UTF-8 BOM (EF BB BF) — md4c counts those bytes as line content
     * so e.g. "\xef\xbb\xbf# Heading" never matches the H1 production
     * (the '#' is no longer at column 0). Saved files routinely carry one. */
    if (src_len >= 3 &&
        (unsigned char)src[0] == 0xEF &&
        (unsigned char)src[1] == 0xBB &&
        (unsigned char)src[2] == 0xBF) {
        src     += 3;
        src_len -= 3;
    }

    MD_PARSER parser = {
        .abi_version = 0,
        .flags       = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS
                     | MD_FLAG_LATEXMATHSPANS,
        .enter_block = cb_enter_block,
        .leave_block = cb_leave_block,
        .enter_span  = cb_enter_span,
        .leave_span  = cb_leave_span,
        .text        = cb_text,
    };
    int rc = md_parse(src, (MD_SIZE)src_len, &parser, &ctx);

    while (out->line_count > 0 &&
           out->lines[out->line_count - 1].kind == LINE_BLANK) {
        out->line_count--;
    }
    wikis_post(out);
    /* If parsing bailed mid-link the orphaned href would leak. */
    free(ctx.link_href);
    return rc;
}

void md_doc_free(MdDoc* d)
{
    if (!d) return;
    free(d->data);
    free(d->style);
    free(d->lines);
    free(d->wikis);
    for (size_t i = 0; i < d->link_count; ++i) free(d->links[i].href);
    free(d->links);
    memset(d, 0, sizeof *d);
}

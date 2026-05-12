#include "regex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    N_CHAR, N_CLASS, N_ANY, N_BOL, N_EOL,
    N_QUANT, N_ALT, N_SEQ
};

typedef struct Node {
    int            type;
    unsigned char  ch;
    unsigned char  bits[32];      /* CLASS bitset (256 bits) */
    int            negated;       /* CLASS: defer negation until apply_ci */
    int            min, max;      /* QUANT: max < 0 means unbounded */
    int            greedy;        /* QUANT */
    struct Node*   child;         /* QUANT */
    struct Node*   left;          /* ALT */
    struct Node*   right;         /* ALT */
    struct Node**  seq;           /* SEQ children */
    int            seq_len;
} Node;

struct DsRegex {
    Node* root;
    int   case_insensitive;
};

static Node* parse_alt(const char** p, const char* end, char* err, size_t el);

static Node* node_new(int type)
{
    Node* n = (Node*)calloc(1, sizeof(Node));
    n->type = type;
    return n;
}

static void node_free(Node* n)
{
    if (!n) return;
    if (n->type == N_QUANT) node_free(n->child);
    if (n->type == N_ALT)   { node_free(n->left); node_free(n->right); }
    if (n->type == N_SEQ) {
        for (int i = 0; i < n->seq_len; ++i) node_free(n->seq[i]);
        free(n->seq);
    }
    free(n);
}

static void seq_push(Node* seq, Node* item)
{
    seq->seq = (Node**)realloc(seq->seq, sizeof(Node*) * (seq->seq_len + 1));
    seq->seq[seq->seq_len++] = item;
}

static void bits_set (unsigned char* b, unsigned char c) { b[c >> 3] |= (unsigned char)(1u << (c & 7)); }
static int  bits_test(const unsigned char* b, unsigned char c) { return (b[c >> 3] >> (c & 7)) & 1; }

/* Append the chars covered by an escape that names a predefined class
 * (\d, \D, \w, \W, \s, \S). The negative variants compute their inverse
 * relative to a fresh tmp bitset and OR the inverse into `bits`. */
static void class_add_predef(unsigned char* bits, char esc)
{
    unsigned char tmp[32] = {0};
    switch (esc) {
        case 'd':
            for (int i = '0'; i <= '9'; ++i) bits_set(bits, (unsigned char)i);
            return;
        case 'D':
            for (int i = '0'; i <= '9'; ++i) bits_set(tmp, (unsigned char)i);
            for (int i = 0; i < 32; ++i) bits[i] |= (unsigned char)~tmp[i];
            return;
        case 'w':
            for (int i = 'A'; i <= 'Z'; ++i) bits_set(bits, (unsigned char)i);
            for (int i = 'a'; i <= 'z'; ++i) bits_set(bits, (unsigned char)i);
            for (int i = '0'; i <= '9'; ++i) bits_set(bits, (unsigned char)i);
            bits_set(bits, '_');
            return;
        case 'W':
            for (int i = 'A'; i <= 'Z'; ++i) bits_set(tmp, (unsigned char)i);
            for (int i = 'a'; i <= 'z'; ++i) bits_set(tmp, (unsigned char)i);
            for (int i = '0'; i <= '9'; ++i) bits_set(tmp, (unsigned char)i);
            bits_set(tmp, '_');
            for (int i = 0; i < 32; ++i) bits[i] |= (unsigned char)~tmp[i];
            return;
        case 's':
            bits_set(bits, ' ');  bits_set(bits, '\t'); bits_set(bits, '\n');
            bits_set(bits, '\r'); bits_set(bits, '\f'); bits_set(bits, '\v');
            return;
        case 'S':
            bits_set(tmp, ' ');  bits_set(tmp, '\t'); bits_set(tmp, '\n');
            bits_set(tmp, '\r'); bits_set(tmp, '\f'); bits_set(tmp, '\v');
            for (int i = 0; i < 32; ++i) bits[i] |= (unsigned char)~tmp[i];
            return;
    }
}

/* Decode the escape after a backslash inside a `[...]`. Returns the byte
 * to add literally, or -1 if the escape was a predefined class (already
 * applied to bits). On EOF returns -2 (caller should signal "missing ]"). */
static int parse_class_escape(const char** p, const char* end,
                              unsigned char* bits)
{
    if (*p >= end) return -2;
    char esc = **p;
    if (esc == 'n') { (*p)++; return '\n'; }
    if (esc == 't') { (*p)++; return '\t'; }
    if (esc == 'r') { (*p)++; return '\r'; }
    if (esc == 'd' || esc == 'D' || esc == 'w' || esc == 'W' ||
        esc == 's' || esc == 'S') {
        class_add_predef(bits, esc);
        (*p)++;
        return -1;
    }
    int v = (unsigned char)esc;
    (*p)++;
    return v;
}

static Node* parse_atom(const char** p, const char* end, char* err, size_t el)
{
    if (*p >= end) return NULL;
    char c = **p;
    if (c == '(') {
        (*p)++;
        Node* sub = parse_alt(p, end, err, el);
        if (!sub) return NULL;
        if (*p >= end || **p != ')') {
            if (err) snprintf(err, el, "missing )");
            node_free(sub);
            return NULL;
        }
        (*p)++;
        return sub;
    }
    if (c == '[') {
        (*p)++;
        Node* n = node_new(N_CLASS);
        if (*p < end && **p == '^') { n->negated = 1; (*p)++; }
        while (*p < end && **p != ']') {
            int low;
            /* Read the low end of a possible range. -1 means the iteration
             * already consumed a predefined class (\d/\w/\s/...) and there
             * is no individual byte to range from. */
            if (**p == '\\') {
                (*p)++;
                int got = parse_class_escape(p, end, n->bits);
                if (got == -2) {
                    if (err) snprintf(err, el, "bad escape in class");
                    node_free(n);
                    return NULL;
                }
                if (got < 0) continue;
                low = got;
            } else {
                low = (unsigned char)**p;
                (*p)++;
            }
            /* Range: low '-' hi. The '-' is only a range op when followed by
             * something that isn't the closing ']' (otherwise it's literal). */
            if (*p + 1 < end && **p == '-' && (*p)[1] != ']') {
                (*p)++;     /* skip '-' */
                int hi;
                if (**p == '\\') {
                    (*p)++;
                    int got = parse_class_escape(p, end, n->bits);
                    if (got < 0) { bits_set(n->bits, (unsigned char)low); continue; }
                    hi = got;
                } else {
                    hi = (unsigned char)**p;
                    (*p)++;
                }
                int lo = low, hh = hi;
                if (hh < lo) { int t = lo; lo = hh; hh = t; }
                for (int i = lo; i <= hh; ++i)
                    bits_set(n->bits, (unsigned char)i);
            } else {
                bits_set(n->bits, (unsigned char)low);
            }
        }
        if (*p >= end) {
            if (err) snprintf(err, el, "missing ]");
            node_free(n);
            return NULL;
        }
        (*p)++;
        return n;
    }
    if (c == '.') { (*p)++; return node_new(N_ANY); }
    if (c == '^') { (*p)++; return node_new(N_BOL); }
    if (c == '$') { (*p)++; return node_new(N_EOL); }
    if (c == ')' || c == '|' || c == '*' || c == '+' || c == '?') return NULL;
    if (c == '\\' && *p + 1 < end) {
        (*p)++;
        char esc = **p;
        (*p)++;
        if (esc == 'd' || esc == 'D' || esc == 'w' || esc == 'W' ||
            esc == 's' || esc == 'S') {
            Node* n = node_new(N_CLASS);
            class_add_predef(n->bits, esc);
            return n;
        }
        if (esc == 'n') { Node* n = node_new(N_CHAR); n->ch = '\n'; return n; }
        if (esc == 't') { Node* n = node_new(N_CHAR); n->ch = '\t'; return n; }
        if (esc == 'r') { Node* n = node_new(N_CHAR); n->ch = '\r'; return n; }
        Node* n = node_new(N_CHAR);
        n->ch = (unsigned char)esc;
        return n;
    }
    Node* n = node_new(N_CHAR);
    n->ch = (unsigned char)c;
    (*p)++;
    return n;
}

static Node* parse_quant(const char** p, const char* end, char* err, size_t el)
{
    Node* a = parse_atom(p, end, err, el);
    if (!a) return NULL;
    if (*p < end) {
        char c = **p;
        if (c == '*' || c == '+' || c == '?') {
            (*p)++;
            Node* q = node_new(N_QUANT);
            q->child = a;
            if      (c == '*') { q->min = 0; q->max = -1; }
            else if (c == '+') { q->min = 1; q->max = -1; }
            else               { q->min = 0; q->max =  1; }
            q->greedy = 1;
            if (*p < end && **p == '?') { q->greedy = 0; (*p)++; }
            return q;
        }
    }
    return a;
}

static Node* parse_seq(const char** p, const char* end, char* err, size_t el)
{
    Node* seq = node_new(N_SEQ);
    while (*p < end && **p != '|' && **p != ')') {
        Node* item = parse_quant(p, end, err, el);
        if (!item) {
            if (seq->seq_len == 0) { node_free(seq); return NULL; }
            break;
        }
        seq_push(seq, item);
    }
    if (seq->seq_len == 1) {
        Node* only = seq->seq[0];
        free(seq->seq);
        free(seq);
        return only;
    }
    return seq;
}

static Node* parse_alt(const char** p, const char* end, char* err, size_t el)
{
    Node* left = parse_seq(p, end, err, el);
    if (!left) return NULL;
    if (*p < end && **p == '|') {
        (*p)++;
        Node* right = parse_alt(p, end, err, el);
        if (!right) { node_free(left); return NULL; }
        Node* a = node_new(N_ALT);
        a->left = left; a->right = right;
        return a;
    }
    return left;
}

/* Walk the tree post-parse: under case-insensitive, mirror letter bits in
 * every CLASS node (so [a-z] also accepts A-Z). Then apply deferred
 * negation. Mirror-then-negate is required for `[^a-z]` to correctly
 * exclude both 'A' and 'a' under ci. */
static void apply_ci(Node* n, int ci)
{
    if (!n) return;
    if (n->type == N_CLASS) {
        if (ci) {
            for (int i = 'A'; i <= 'Z'; ++i) {
                int lo = i + 32;
                int u = bits_test(n->bits, (unsigned char)i);
                int v = bits_test(n->bits, (unsigned char)lo);
                if (u || v) {
                    bits_set(n->bits, (unsigned char)i);
                    bits_set(n->bits, (unsigned char)lo);
                }
            }
        }
        if (n->negated) {
            for (int i = 0; i < 32; ++i)
                n->bits[i] = (unsigned char)~n->bits[i];
            n->negated = 0;
        }
    }
    if (n->type == N_QUANT) apply_ci(n->child, ci);
    if (n->type == N_ALT)   { apply_ci(n->left, ci); apply_ci(n->right, ci); }
    if (n->type == N_SEQ)   {
        for (int i = 0; i < n->seq_len; ++i) apply_ci(n->seq[i], ci);
    }
}

DsRegex* ds_regex_compile(const char* pattern, int ci, char* err, size_t el)
{
    if (err && el > 0) err[0] = 0;
    if (!pattern) return NULL;
    const char* p = pattern;
    const char* e = pattern + strlen(pattern);
    Node* root = parse_alt(&p, e, err, el);
    if (!root) {
        if (err && el > 0 && err[0] == 0) snprintf(err, el, "parse error");
        return NULL;
    }
    if (p < e) {
        if (err) snprintf(err, el, "unexpected '%c'", *p);
        node_free(root);
        return NULL;
    }
    apply_ci(root, ci);
    DsRegex* re = (DsRegex*)calloc(1, sizeof(DsRegex));
    re->root = root;
    re->case_insensitive = ci;
    return re;
}

void ds_regex_free(DsRegex* re)
{
    if (!re) return;
    node_free(re->root);
    free(re);
}

/* Continuation-passing matcher. Each node tries to match at `pos` and on
 * success calls `cont(end_pos, ud)`. Greedy quantifiers try the long
 * branch first; lazy ones try the short branch first. */

typedef int (*Cont)(size_t pos, void* ud);

typedef struct {
    const DsRegex* re;
    const char*    hay;
    size_t         hlen;
    size_t         best_end;
} Ctx;

static int do_match(Node* n, size_t pos, Cont cont, void* ud, Ctx* ctx);

static int cont_record(size_t pos, void* ud)
{
    Ctx* c = (Ctx*)ud;
    c->best_end = pos;
    return 1;
}

typedef struct {
    Node* const* items;
    int          idx;
    int          n;
    Cont         next;
    void*        next_ud;
    Ctx*         ctx;
} SeqCont;

static int do_seq_step(size_t pos, void* ud);

static int do_seq(SeqCont* s, size_t pos)
{
    if (s->idx >= s->n) return s->next(pos, s->next_ud);
    return do_match(s->items[s->idx], pos, do_seq_step, s, s->ctx);
}

static int do_seq_step(size_t pos, void* ud)
{
    SeqCont* s = (SeqCont*)ud;
    s->idx++;
    int r = do_seq(s, pos);
    if (!r) s->idx--;     /* unwind so re-entry from siblings is consistent */
    return r;
}

typedef struct {
    Node* atom;
    int   min, max;
    int   greedy;
    int   count;
    Cont  next;
    void* next_ud;
    Ctx*  ctx;
} QuantCont;

static int do_quant(QuantCont* q, size_t pos);

static int do_quant_step(size_t pos, void* ud)
{
    QuantCont* q = (QuantCont*)ud;
    q->count++;
    int r = do_quant(q, pos);
    if (!r) q->count--;
    return r;
}

static int do_quant(QuantCont* q, size_t pos)
{
    int try_more = (q->max < 0 || q->count < q->max);
    int try_stop = (q->count >= q->min);
    if (q->greedy) {
        if (try_more && do_match(q->atom, pos, do_quant_step, q, q->ctx))
            return 1;
        if (try_stop) return q->next(pos, q->next_ud);
    } else {
        if (try_stop && q->next(pos, q->next_ud)) return 1;
        if (try_more) return do_match(q->atom, pos, do_quant_step, q, q->ctx);
    }
    return 0;
}

static int ci_eq(unsigned char a, unsigned char b)
{
    if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + 32);
    if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + 32);
    return a == b;
}

static int do_match(Node* n, size_t pos, Cont cont, void* ud, Ctx* ctx)
{
    switch (n->type) {
        case N_CHAR: {
            if (pos >= ctx->hlen) return 0;
            unsigned char hc = (unsigned char)ctx->hay[pos];
            int eq = ctx->re->case_insensitive ? ci_eq(hc, n->ch) : (hc == n->ch);
            return eq ? cont(pos + 1, ud) : 0;
        }
        case N_ANY:
            if (pos >= ctx->hlen)            return 0;
            if (ctx->hay[pos] == '\n')       return 0;
            return cont(pos + 1, ud);
        case N_CLASS: {
            if (pos >= ctx->hlen) return 0;
            unsigned char hc = (unsigned char)ctx->hay[pos];
            return bits_test(n->bits, hc) ? cont(pos + 1, ud) : 0;
        }
        case N_BOL:
            return (pos == 0 || ctx->hay[pos - 1] == '\n') ? cont(pos, ud) : 0;
        case N_EOL:
            return (pos == ctx->hlen || ctx->hay[pos] == '\n') ? cont(pos, ud) : 0;
        case N_SEQ: {
            SeqCont s; s.items = n->seq; s.idx = 0; s.n = n->seq_len;
            s.next = cont; s.next_ud = ud; s.ctx = ctx;
            return do_seq(&s, pos);
        }
        case N_ALT:
            if (do_match(n->left, pos, cont, ud, ctx)) return 1;
            return do_match(n->right, pos, cont, ud, ctx);
        case N_QUANT: {
            QuantCont q; q.atom = n->child; q.min = n->min; q.max = n->max;
            q.greedy = n->greedy; q.count = 0;
            q.next = cont; q.next_ud = ud; q.ctx = ctx;
            return do_quant(&q, pos);
        }
    }
    return 0;
}

int ds_regex_find(const DsRegex* re,
                  const char* hay, size_t hlen, size_t start,
                  size_t* out_start, size_t* out_end)
{
    if (!re || !re->root) return 0;
    Ctx ctx; ctx.re = re; ctx.hay = hay; ctx.hlen = hlen; ctx.best_end = 0;
    for (size_t i = start; i <= hlen; ++i) {
        ctx.best_end = i;
        if (do_match(re->root, i, cont_record, &ctx, &ctx)) {
            if (out_start) *out_start = i;
            if (out_end)   *out_end   = ctx.best_end;
            return 1;
        }
    }
    return 0;
}

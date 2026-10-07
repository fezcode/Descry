#include "mermaid.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define MM_MAX_NODES 400
#define MM_MAX_EDGES 800

/* Spacing — deliberately generous so elements never crowd (per request). */
#define MM_NODE_GAP   46    /* between siblings within a layer (cross axis) */
#define MM_LAYER_GAP  90    /* between layers (main axis)                   */
#define MM_MARGIN     24    /* padding around the whole diagram             */
#define MM_PAD_X      18    /* node label horizontal padding (per side)     */
#define MM_PAD_Y      12    /* node label vertical padding (per side)       */

/* Sequence diagram limits + spacing. */
#define MM_SEQ_MAX_ITEMS 1000
#define MM_SEQ_DEPTH     16
#define SQ_BOX_GAP    50    /* min gap between neighbouring participant boxes */
#define SQ_BOX_MIN_W  90    /* min participant box width                      */
#define SQ_NOTE_PAD   10    /* note label padding (per side)                  */
#define SQ_FRAME_PAD  14    /* frame inset around its contents                */

typedef struct {
    MmDiagram*  d;
    MmMeasureFn measure;
    void*       mctx;
    int         text_h;
    /* sequence parse state */
    int         autonum, autonum_next, autonum_step;
    int         stack[MM_SEQ_DEPTH];   /* open frame item indices, -1 = box */
    int         sp;
} Builder;

/* ---- helpers ----------------------------------------------------------- */

static int ci_eq(const char* a, int alen, const char* b)
{
    int bl = (int)strlen(b);
    if (alen != bl) return 0;
    for (int i = 0; i < alen; ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}

static int is_id_ch(int c) { return isalnum((unsigned char)c) || c == '_'; }
static int is_link_ch(int c) { return c == '-' || c == '.' || c == '='; }

static int node_find(MmDiagram* d, const char* id, int idlen)
{
    for (int i = 0; i < d->node_count; ++i)
        if ((int)strlen(d->nodes[i].id) == idlen &&
            memcmp(d->nodes[i].id, id, (size_t)idlen) == 0)
            return i;
    return -1;
}

static int node_add(MmDiagram* d, const char* id, int idlen)
{
    int idx = node_find(d, id, idlen);
    if (idx >= 0) return idx;
    if (d->node_count >= MM_MAX_NODES) return -1;
    MmNode* nn = realloc(d->nodes, (size_t)(d->node_count + 1) * sizeof *nn);
    if (!nn) return -1;
    d->nodes = nn;
    MmNode* n = &d->nodes[d->node_count];
    memset(n, 0, sizeof *n);
    if (idlen >= (int)sizeof n->id) idlen = (int)sizeof n->id - 1;
    memcpy(n->id, id, (size_t)idlen);    n->id[idlen]    = 0;
    memcpy(n->label, id, (size_t)idlen); n->label[idlen] = 0;  /* default */
    n->shape = MM_SHAPE_RECT;
    n->layer = 0;
    return d->node_count++;
}

static void node_set_label(MmNode* n, const char* lab, int llen, MmShape shape)
{
    /* strip one layer of surrounding quotes */
    if (llen >= 2 && lab[0] == '"' && lab[llen - 1] == '"') { lab++; llen -= 2; }
    if (llen < 0) llen = 0;
    if (llen >= (int)sizeof n->label) llen = (int)sizeof n->label - 1;
    memcpy(n->label, lab, (size_t)llen);
    n->label[llen] = 0;
    n->shape = shape;
}

/* Parse a node reference (id + optional shape/label) at s[*pi]. Returns the
 * node index, or -1 if there's no id here. */
static int parse_node(MmDiagram* d, const char* s, int len, int* pi)
{
    int i = *pi;
    while (i < len && isspace((unsigned char)s[i])) i++;
    int id0 = i;
    while (i < len && is_id_ch(s[i])) i++;
    if (i == id0) { *pi = i; return -1; }
    int idx = node_add(d, s + id0, i - id0);

    if (i < len) {
        MmShape shape = MM_SHAPE_RECT;
        const char* close = NULL;
        int open_n = 0;
        char o = s[i];
        if (o == '[') {
            if (i + 1 < len && s[i + 1] == '[') { shape = MM_SHAPE_SUBROUT; close = "]]"; open_n = 2; }
            else if (i + 1 < len && s[i + 1] == '(') { shape = MM_SHAPE_STADIUM; close = ")]"; open_n = 2; }
            else { shape = MM_SHAPE_RECT; close = "]"; open_n = 1; }
        } else if (o == '(') {
            if (i + 1 < len && s[i + 1] == '(') { shape = MM_SHAPE_CIRCLE; close = "))"; open_n = 2; }
            else if (i + 1 < len && s[i + 1] == '[') { shape = MM_SHAPE_STADIUM; close = "])"; open_n = 2; }
            else { shape = MM_SHAPE_ROUND; close = ")"; open_n = 1; }
        } else if (o == '{') {
            if (i + 1 < len && s[i + 1] == '{') { shape = MM_SHAPE_HEX; close = "}}"; open_n = 2; }
            else { shape = MM_SHAPE_DIAMOND; close = "}"; open_n = 1; }
        }
        if (close) {
            int lab0 = i + open_n;
            int cl = (int)strlen(close);
            int j = lab0;
            while (j + cl <= len && memcmp(s + j, close, (size_t)cl) != 0) j++;
            if (j + cl <= len) {
                if (idx >= 0) node_set_label(&d->nodes[idx], s + lab0, j - lab0, shape);
                i = j + cl;
            }
        }
    }
    *pi = i;
    return idx;
}

/* Parse a link operator at s[*pi]. Returns 1 if a link was consumed (filling
 * label/dashed/arrow_to/arrow_from), 0 otherwise. */
static int parse_link(const char* s, int len, int* pi,
                      char* label, int labcap,
                      int* dashed, int* arrow_to, int* arrow_from)
{
    int i = *pi;
    while (i < len && isspace((unsigned char)s[i])) i++;
    int start = i;
    label[0] = 0; *dashed = 0; *arrow_to = 0; *arrow_from = 0;

    if (i < len && s[i] == '<') { *arrow_from = 1; i++; }
    if (!(i < len && (is_link_ch(s[i]) || s[i] == 'o' || s[i] == 'x'))) {
        *pi = start;
        return 0;
    }
    if (i < len && (s[i] == 'o' || s[i] == 'x')) i++;   /* leading o/x head */

    /* first punctuation run */
    while (i < len && is_link_ch(s[i])) { if (s[i] == '.') *dashed = 1; i++; }

    if (i < len && (s[i] == '>' || s[i] == 'o' || s[i] == 'x')) {
        *arrow_to = 1; i++;
    } else {
        /* maybe "-- text -->": only if another punct run follows on the line */
        int save = i, t0 = i;
        while (i < len && !is_link_ch(s[i]) && s[i] != '|') i++;
        if (i < len && is_link_ch(s[i])) {
            int tlen = i - t0;
            while (tlen > 0 && s[t0] == ' ')           { t0++; tlen--; }
            while (tlen > 0 && s[t0 + tlen - 1] == ' ') tlen--;
            if (tlen > 0) {
                if (tlen >= labcap) tlen = labcap - 1;
                memcpy(label, s + t0, (size_t)tlen); label[tlen] = 0;
            }
            while (i < len && is_link_ch(s[i])) { if (s[i] == '.') *dashed = 1; i++; }
            if (i < len && (s[i] == '>' || s[i] == 'o' || s[i] == 'x')) { *arrow_to = 1; i++; }
        } else {
            i = save;   /* open link (---); leave the rest to node parsing */
        }
    }

    /* trailing |label| */
    {
        int j = i;
        while (j < len && s[j] == ' ') j++;
        if (j < len && s[j] == '|') {
            j++;
            int l0 = j;
            while (j < len && s[j] != '|') j++;
            int llen = j - l0;
            if (j < len) j++;
            if (llen > 0) {
                if (llen >= labcap) llen = labcap - 1;
                memcpy(label, s + l0, (size_t)llen); label[llen] = 0;
            }
            i = j;
        }
    }

    *pi = i;
    return 1;
}

static void edge_add(MmDiagram* d, int from, int to, const char* label,
                     int dashed, int at, int af)
{
    if (from < 0 || to < 0) return;
    if (d->edge_count >= MM_MAX_EDGES) return;
    MmEdge* ne = realloc(d->edges, (size_t)(d->edge_count + 1) * sizeof *ne);
    if (!ne) return;
    d->edges = ne;
    MmEdge* e = &d->edges[d->edge_count++];
    memset(e, 0, sizeof *e);
    e->from = from; e->to = to; e->dashed = dashed;
    e->arrow_to = at; e->arrow_from = af;
    if (label && label[0]) {
        size_t ln = strlen(label);
        if (ln >= sizeof e->label) ln = sizeof e->label - 1;
        memcpy(e->label, label, ln);
        e->label[ln] = 0;
    }
}

/* Parse one statement: NODE (LINK NODE)*  — handles chains like A-->B-->C. */
static void parse_statement(MmDiagram* d, const char* s, int len)
{
    int i = 0;
    int prev = parse_node(d, s, len, &i);
    if (prev < 0 && d->node_count == 0) return;
    for (;;) {
        char label[160]; int dashed, at, af;
        int before = i;
        if (!parse_link(s, len, &i, label, (int)sizeof label, &dashed, &at, &af))
            break;
        int nxt = parse_node(d, s, len, &i);
        if (nxt < 0) { i = before; break; }
        edge_add(d, prev, nxt, label, dashed, at, af);
        prev = nxt;
    }
}

/* ---- layout ------------------------------------------------------------ */

/* DFS that marks back edges (edges pointing at a node still on the recursion
 * stack) so cyclic graphs get sane, terminating layering. */
static void mm_dfs(MmDiagram* d, int u, int* state, char* is_back)
{
    state[u] = 1;
    for (int e = 0; e < d->edge_count; ++e) {
        if (d->edges[e].from != u) continue;
        int v = d->edges[e].to;
        if (v < 0 || v >= d->node_count) continue;
        if (state[v] == 1)      { if (is_back) is_back[e] = 1; }
        else if (state[v] == 0) mm_dfs(d, v, state, is_back);
    }
    state[u] = 2;
}

static void mm_layout(Builder* b)
{
    MmDiagram* d = b->d;
    int n = d->node_count;
    if (n == 0) return;

    /* node sizes from label metrics */
    for (int i = 0; i < n; ++i) {
        MmNode* nd = &d->nodes[i];
        int tw = b->measure ? b->measure(b->mctx, nd->label, strlen(nd->label))
                            : (int)strlen(nd->label) * 8;
        int w = tw + 2 * MM_PAD_X;
        int h = b->text_h + 2 * MM_PAD_Y;
        if (w < 56) w = 56;
        switch (nd->shape) {
            case MM_SHAPE_DIAMOND: w += 30; h += 20; break;
            case MM_SHAPE_HEX:     w += 26; break;
            case MM_SHAPE_CIRCLE: { int s = w > h ? w : h; s += 12; w = h = s; } break;
            default: break;
        }
        nd->w = w; nd->h = h;
    }

    /* Break cycles first (mark back edges), then longest-path layering over
     * the remaining DAG so layers stay compact and the loop terminates. */
    int*  state   = calloc((size_t)n, sizeof(int));
    char* is_back = d->edge_count ? calloc((size_t)d->edge_count, 1) : NULL;
    if (state)
        for (int i = 0; i < n; ++i)
            if (state[i] == 0) mm_dfs(d, i, state, is_back);

    for (int i = 0; i < n; ++i) d->nodes[i].layer = 0;
    for (int pass = 0; pass < n; ++pass) {
        int changed = 0;
        for (int e = 0; e < d->edge_count; ++e) {
            if (is_back && is_back[e]) continue;
            int a = d->edges[e].from, c = d->edges[e].to;
            if (a < 0 || c < 0 || a >= n || c >= n) continue;
            if (d->nodes[c].layer <= d->nodes[a].layer) {
                d->nodes[c].layer = d->nodes[a].layer + 1;
                changed = 1;
            }
        }
        if (!changed) break;
    }
    free(state); free(is_back);

    int maxL = 0;
    for (int i = 0; i < n; ++i) if (d->nodes[i].layer > maxL) maxL = d->nodes[i].layer;
    int L = maxL + 1;

    int* main_size   = calloc((size_t)L, sizeof(int)); /* max main-axis size/layer */
    int* cross_total = calloc((size_t)L, sizeof(int)); /* total cross extent/layer */
    int* layer_cnt   = calloc((size_t)L, sizeof(int));
    int* main_pos    = calloc((size_t)L, sizeof(int));
    int* cross_cur   = calloc((size_t)L, sizeof(int));
    if (!main_size || !cross_total || !layer_cnt || !main_pos || !cross_cur) {
        free(main_size); free(cross_total); free(layer_cnt); free(main_pos); free(cross_cur);
        return;
    }

    int horiz = (d->dir == MM_DIR_LR || d->dir == MM_DIR_RL);

    for (int i = 0; i < n; ++i) {
        MmNode* nd = &d->nodes[i];
        int lyr = nd->layer;
        nd->order = layer_cnt[lyr]++;
        int msz = horiz ? nd->w : nd->h;
        int csz = horiz ? nd->h : nd->w;
        if (msz > main_size[lyr]) main_size[lyr] = msz;
        cross_total[lyr] += csz;
    }
    for (int l = 0; l < L; ++l)
        if (layer_cnt[l] > 1) cross_total[l] += (layer_cnt[l] - 1) * MM_NODE_GAP;

    int acc = 0;
    for (int l = 0; l < L; ++l) {
        main_pos[l] = acc;
        acc += main_size[l] + (l < L - 1 ? MM_LAYER_GAP : 0);
    }
    int total_main = acc;
    int max_cross = 0;
    for (int l = 0; l < L; ++l) if (cross_total[l] > max_cross) max_cross = cross_total[l];
    for (int l = 0; l < L; ++l) cross_cur[l] = (max_cross - cross_total[l]) / 2;

    for (int i = 0; i < n; ++i) {
        MmNode* nd = &d->nodes[i];
        int lyr = nd->layer;
        int msz = horiz ? nd->w : nd->h;
        int csz = horiz ? nd->h : nd->w;
        int main_off = (main_size[lyr] - msz) / 2;
        int main_c = main_pos[lyr] + main_off;
        int cross_c = cross_cur[lyr];
        cross_cur[lyr] += csz + MM_NODE_GAP;

        if (!horiz) {
            int yy = (d->dir == MM_DIR_BT) ? (total_main - (main_c + msz)) : main_c;
            nd->x = MM_MARGIN + cross_c;
            nd->y = MM_MARGIN + yy;
        } else {
            int xx = (d->dir == MM_DIR_RL) ? (total_main - (main_c + msz)) : main_c;
            nd->x = MM_MARGIN + xx;
            nd->y = MM_MARGIN + cross_c;
        }
    }

    if (!horiz) { d->width = MM_MARGIN * 2 + max_cross; d->height = MM_MARGIN * 2 + total_main; }
    else        { d->width = MM_MARGIN * 2 + total_main; d->height = MM_MARGIN * 2 + max_cross; }

    free(main_size); free(cross_total); free(layer_cnt); free(main_pos); free(cross_cur);
}

/* ---- sequence diagrams ------------------------------------------------- */

static int measure(Builder* b, const char* s)
{
    return b->measure ? b->measure(b->mctx, s, strlen(s)) : (int)strlen(s) * 8;
}

/* Copy a label: trim, drop one layer of quotes, turn <br>, <br/>, <br />
 * into spaces (labels are drawn on a single line). */
static void seq_copy_text(char* dst, int cap, const char* s, int n)
{
    while (n > 0 && isspace((unsigned char)*s))       { s++; n--; }
    while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') { s++; n -= 2; }
    int o = 0;
    for (int i = 0; i < n && o < cap - 1; ) {
        if (s[i] == '<' && i + 2 < n &&
            (s[i + 1] == 'b' || s[i + 1] == 'B') &&
            (s[i + 2] == 'r' || s[i + 2] == 'R')) {
            int j = i + 3;
            while (j < n && (s[j] == ' ' || s[j] == '/')) j++;
            if (j < n && s[j] == '>') { dst[o++] = ' '; i = j + 1; continue; }
        }
        dst[o++] = s[i++];
    }
    dst[o] = 0;
}

/* Participant by id (trimmed), created on first mention. */
static int seq_participant(MmDiagram* d, const char* s, int n)
{
    while (n > 0 && isspace((unsigned char)*s))       { s++; n--; }
    while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
    if (n <= 0) return -1;
    return node_add(d, s, n);
}

static MmSeqItem* seq_item_add(MmDiagram* d, MmSeqType type)
{
    if (d->item_count >= MM_SEQ_MAX_ITEMS) return NULL;
    MmSeqItem* ni = realloc(d->items, (size_t)(d->item_count + 1) * sizeof *ni);
    if (!ni) return NULL;
    d->items = ni;
    MmSeqItem* it = &d->items[d->item_count++];
    memset(it, 0, sizeof *it);
    it->type = type;
    it->from = it->to = -1;
    return it;
}

/* Message: FROM ARROW [+|-]TO : label */
static void seq_message(Builder* b, const char* s, int n)
{
    static const struct { const char* tok; int dashed, head, both; } AR[] = {
        { "<<-->>", 1, MM_HEAD_ARROW, 1 }, { "<<->>", 0, MM_HEAD_ARROW, 1 },
        { "-->>",   1, MM_HEAD_ARROW, 0 }, { "->>",   0, MM_HEAD_ARROW, 0 },
        { "--x",    1, MM_HEAD_CROSS, 0 }, { "-x",    0, MM_HEAD_CROSS, 0 },
        { "--)",    1, MM_HEAD_ASYNC, 0 }, { "-)",    0, MM_HEAD_ASYNC, 0 },
        { "-->",    1, MM_HEAD_NONE,  0 }, { "->",    0, MM_HEAD_NONE,  0 },
    };
    int colon = 0;
    while (colon < n && s[colon] != ':') colon++;
    for (int p = 1; p < colon; ++p) {
        for (size_t k = 0; k < sizeof AR / sizeof AR[0]; ++k) {
            int tl = (int)strlen(AR[k].tok);
            if (p + tl > colon || memcmp(s + p, AR[k].tok, (size_t)tl) != 0)
                continue;
            int q = p + tl;
            while (q < colon && (s[q] == '+' || s[q] == '-' || s[q] == ' ')) q++;
            int from = seq_participant(b->d, s, p);
            int to   = seq_participant(b->d, s + q, colon - q);
            if (from < 0 || to < 0) return;
            MmSeqItem* it = seq_item_add(b->d, MM_SEQ_MSG);
            if (!it) return;
            it->from = from; it->to = to;
            it->dashed = AR[k].dashed; it->head = AR[k].head; it->both = AR[k].both;
            if (colon < n)
                seq_copy_text(it->label, (int)sizeof it->label,
                              s + colon + 1, n - colon - 1);
            if (b->autonum) {
                it->number = b->autonum_next;
                b->autonum_next += b->autonum_step;
            }
            return;
        }
    }
}

static int is_frame_kw(const char* w, int n)
{
    return ci_eq(w, n, "loop") || ci_eq(w, n, "alt") || ci_eq(w, n, "opt") ||
           ci_eq(w, n, "par")  || ci_eq(w, n, "critical") ||
           ci_eq(w, n, "break") || ci_eq(w, n, "rect");
}

static void seq_line(Builder* b, const char* s, int n)
{
    MmDiagram* d = b->d;
    if (n > 0 && s[n - 1] == ';') n--;
    int t = 0;
    while (t < n && !isspace((unsigned char)s[t]) && s[t] != ':') t++;
    const char* rest = s + t;
    int rn = n - t;
    while (rn > 0 && isspace((unsigned char)*rest)) { rest++; rn--; }

    if (ci_eq(s, t, "create")) { seq_line(b, rest, rn); return; }

    if (ci_eq(s, t, "participant") || ci_eq(s, t, "actor")) {
        int u = 0;
        while (u < rn && !isspace((unsigned char)rest[u])) u++;
        int idx = seq_participant(d, rest, u);
        if (idx < 0) return;
        int v = u;
        while (v < rn && isspace((unsigned char)rest[v])) v++;
        int w = v;
        while (w < rn && !isspace((unsigned char)rest[w])) w++;
        MmNode* nd = &d->nodes[idx];
        if (ci_eq(rest + v, w - v, "as"))
            seq_copy_text(nd->label, (int)sizeof nd->label, rest + w, rn - w);
        nd->shape = ci_eq(s, t, "actor") ? MM_SHAPE_STADIUM : MM_SHAPE_RECT;
        return;
    }

    if (ci_eq(s, t, "autonumber")) {
        if (ci_eq(rest, rn, "off")) { b->autonum = 0; return; }
        int start = 1, step = 1;
        if (rn > 0) {
            char* end = NULL;
            long v1 = strtol(rest, &end, 10);
            if (end != rest) {
                start = (int)v1;
                long v2 = strtol(end, &end, 10);
                if (v2 > 0) step = (int)v2;
            }
        }
        b->autonum = 1; b->autonum_next = start; b->autonum_step = step;
        return;
    }

    if (ci_eq(s, t, "note")) {
        int place;
        int u = 0;
        while (u < rn && !isspace((unsigned char)rest[u])) u++;
        if      (ci_eq(rest, u, "over"))  place = MM_NOTE_OVER;
        else if (ci_eq(rest, u, "left"))  place = MM_NOTE_LEFT;
        else if (ci_eq(rest, u, "right")) place = MM_NOTE_RIGHT;
        else return;
        if (place != MM_NOTE_OVER) {           /* skip "of" */
            while (u < rn && isspace((unsigned char)rest[u])) u++;
            int v = u;
            while (v < rn && !isspace((unsigned char)rest[v])) v++;
            if (!ci_eq(rest + u, v - u, "of")) return;
            u = v;
        }
        int colon = u;
        while (colon < rn && rest[colon] != ':') colon++;
        int comma = u;
        while (comma < colon && rest[comma] != ',') comma++;
        int a = seq_participant(d, rest + u, comma - u);
        if (a < 0) return;
        int c = comma < colon
              ? seq_participant(d, rest + comma + 1, colon - comma - 1) : a;
        if (c < 0) c = a;
        MmSeqItem* it = seq_item_add(d, MM_SEQ_NOTE);
        if (!it) return;
        it->from = a; it->to = c; it->place = place;
        if (colon < rn)
            seq_copy_text(it->label, (int)sizeof it->label,
                          rest + colon + 1, rn - colon - 1);
        return;
    }

    if (is_frame_kw(s, t)) {
        MmSeqItem* it = seq_item_add(d, MM_SEQ_FRAME);
        if (!it) return;
        if (!ci_eq(s, t, "rect")) {           /* rect only tints; no tag */
            int kl = t < (int)sizeof it->kw - 1 ? t : (int)sizeof it->kw - 1;
            for (int k = 0; k < kl; ++k)
                it->kw[k] = (char)tolower((unsigned char)s[k]);
            it->kw[kl] = 0;
            seq_copy_text(it->label, (int)sizeof it->label, rest, rn);
        }
        if (b->sp < MM_SEQ_DEPTH) b->stack[b->sp++] = d->item_count - 1;
        return;
    }

    if (ci_eq(s, t, "box")) {                  /* participant group: ignored */
        if (b->sp < MM_SEQ_DEPTH) b->stack[b->sp++] = -1;
        return;
    }

    if (ci_eq(s, t, "else") || ci_eq(s, t, "and") || ci_eq(s, t, "option")) {
        int fr = b->sp > 0 ? b->stack[b->sp - 1] : -1;
        if (fr < 0) return;
        MmSeqItem* it = seq_item_add(d, MM_SEQ_DIVIDER);
        if (!it) return;
        it->from = fr;
        seq_copy_text(it->label, (int)sizeof it->label, rest, rn);
        return;
    }

    if (ci_eq(s, t, "end")) {
        if (b->sp <= 0) return;
        int fr = b->stack[--b->sp];
        if (fr < 0) return;
        MmSeqItem* it = seq_item_add(d, MM_SEQ_END);
        if (it) it->from = fr;
        return;
    }

    if (ci_eq(s, t, "title") || ci_eq(s, t, "activate") ||
        ci_eq(s, t, "deactivate") || ci_eq(s, t, "destroy") ||
        ci_eq(s, t, "link") || ci_eq(s, t, "links") ||
        ci_eq(s, t, "properties") || ci_eq(s, t, "details") ||
        ci_eq(s, t, "accTitle") || ci_eq(s, t, "accDescr"))
        return;

    seq_message(b, s, n);
}

/* Push participant c (and everything right of it) so its center sits at
 * least `need` px right of participant a's. */
static void seq_require(int* cx, int n, int a, int c, int need)
{
    if (a < 0 || c >= n || a >= c) return;
    int have = cx[c] - cx[a];
    if (have >= need) return;
    for (int k = c; k < n; ++k) cx[k] += need - have;
}

static void seq_extend(int* lo, int* hi, int x0, int x1)
{
    if (x0 < *lo) *lo = x0;
    if (x1 > *hi) *hi = x1;
}

static void seq_layout(Builder* b)
{
    MmDiagram* d = b->d;
    int n = d->node_count;
    if (n == 0) return;
    int th = b->text_h;
    int bh = th + 2 * MM_PAD_Y;

    int* cx = calloc((size_t)n, sizeof(int));
    if (!cx) return;
    for (int i = 0; i < n; ++i) {
        MmNode* nd = &d->nodes[i];
        nd->w = measure(b, nd->label) + 2 * MM_PAD_X;
        if (nd->w < SQ_BOX_MIN_W) nd->w = SQ_BOX_MIN_W;
        nd->h = bh;
        cx[i] = i == 0 ? nd->w / 2
                       : cx[i - 1] + (d->nodes[i - 1].w + nd->w) / 2 + SQ_BOX_GAP;
    }

    /* Widen columns so labels fit, narrowest spans first so a long message
     * across several columns only adds what the short ones didn't. */
    for (int span = 0; span < n; ++span) {
        for (int k = 0; k < d->item_count; ++k) {
            const MmSeqItem* it = &d->items[k];
            if (it->type != MM_SEQ_MSG && it->type != MM_SEQ_NOTE) continue;
            int a = it->from < it->to ? it->from : it->to;
            int c = it->from < it->to ? it->to : it->from;
            if (c - a != span) continue;
            if (it->type == MM_SEQ_MSG) {
                int lw = measure(b, it->label) + 24;
                if (span == 0) {
                    int loop = lw > MM_SEQ_SELF_W + 8 ? lw : MM_SEQ_SELF_W + 8;
                    if (a + 1 < n)
                        seq_require(cx, n, a, a + 1,
                                    loop + 16 + d->nodes[a + 1].w / 2);
                } else {
                    seq_require(cx, n, a, c, lw);
                }
            } else {
                int nw = measure(b, it->label) + 2 * SQ_NOTE_PAD + 20;
                if (it->place == MM_NOTE_RIGHT && a + 1 < n)
                    seq_require(cx, n, a, a + 1, nw + d->nodes[a + 1].w / 2);
                else if (it->place == MM_NOTE_LEFT && a > 0)
                    seq_require(cx, n, a - 1, a, nw + d->nodes[a - 1].w / 2);
                else if (it->place == MM_NOTE_OVER && c > a)
                    seq_require(cx, n, a, c, nw - 48);
            }
        }
    }

    /* Rows, top to bottom (y relative to the top of the boxes). Frames are
     * sized as they close: the box wraps everything inside, inner frames
     * included, then widens its parent. */
    int stack[MM_SEQ_DEPTH], lo[MM_SEQ_DEPTH], hi[MM_SEQ_DEPTH];
    int sp = 0;
    int y = bh + 24;
    int minx = 0, maxx = cx[n - 1] + d->nodes[n - 1].w / 2;
    for (int k = 0; k <= d->item_count; ++k) {
        int at_end = (k == d->item_count);
        MmSeqItem* it = at_end ? NULL : &d->items[k];

        if (at_end || it->type == MM_SEQ_END) {
            /* Close one frame — or, past the last row, every unclosed one. */
            while (sp > 0) {
                if (!at_end && stack[sp - 1] != it->from) break;  /* untracked */
                --sp;
                int fi = stack[sp];
                MmSeqItem* fr = &d->items[fi];
                int fl = lo[sp], fh = hi[sp];
                if (fl > fh) { fl = cx[0] - d->nodes[0].w / 2; fh = fl + 120; }
                fr->x = fl - SQ_FRAME_PAD;
                fr->w = fh - fl + 2 * SQ_FRAME_PAD;
                int need = measure(b, fr->kw) + measure(b, fr->label) + 50;
                for (int j = fi + 1; j < k; ++j)
                    if (d->items[j].type == MM_SEQ_DIVIDER && d->items[j].from == fi) {
                        int dw = measure(b, d->items[j].label) + 40;
                        if (dw > need) need = dw;
                    }
                if (fr->w < need) fr->w = need;
                y += 6;
                fr->h = y - fr->y;
                for (int j = fi + 1; j < k; ++j)
                    if (d->items[j].type == MM_SEQ_DIVIDER && d->items[j].from == fi) {
                        d->items[j].x = fr->x;
                        d->items[j].w = fr->w;
                    }
                if (!at_end) it->y = y;
                y += 16;
                if (sp > 0) seq_extend(&lo[sp - 1], &hi[sp - 1], fr->x, fr->x + fr->w);
                seq_extend(&minx, &maxx, fr->x, fr->x + fr->w);
                if (!at_end) break;
            }
            continue;
        }

        int x0 = 0, x1 = 0, has_x = 0;
        switch (it->type) {
        case MM_SEQ_MSG: {
            int lw = it->label[0] ? measure(b, it->label) : 0;
            int lh = it->label[0] ? th : 0;
            int a = cx[it->from], c = cx[it->to];
            int num = it->number ? 10 : 0;     /* autonumber badge radius */
            it->y = y + lh + 4;
            if (it->from == it->to) {
                x0 = a - num;
                x1 = a + (lw + 8 > MM_SEQ_SELF_W + 6 ? lw + 8 : MM_SEQ_SELF_W + 6);
                y = it->y + 18 + 22;
            } else {
                int m = (a + c) / 2;
                x0 = (a < c ? a : c) - num;
                x1 = (a < c ? c : a) + num;
                if (m - lw / 2 < x0) x0 = m - lw / 2;
                if (m + lw / 2 > x1) x1 = m + lw / 2;
                y = it->y + 22;
            }
            it->x = x0; it->w = x1 - x0;
            has_x = 1;
            break;
        }
        case MM_SEQ_NOTE: {
            int nw = measure(b, it->label) + 2 * SQ_NOTE_PAD;
            if (nw < 60) nw = 60;
            int a = cx[it->from], c = cx[it->to];
            if (a > c) { int tmp = a; a = c; c = tmp; }
            if      (it->place == MM_NOTE_RIGHT) it->x = a + 10;
            else if (it->place == MM_NOTE_LEFT)  it->x = a - 10 - nw;
            else if (c > a && c - a + 48 >= nw) { it->x = a - 24; nw = c - a + 48; }
            else                                 it->x = (a + c) / 2 - nw / 2;
            it->w = nw;
            it->y = y;
            it->h = th + 16;
            y += it->h + 16;
            x0 = it->x; x1 = it->x + it->w; has_x = 1;
            break;
        }
        case MM_SEQ_FRAME:
            it->y = y;
            y += th + 18;
            if (sp < MM_SEQ_DEPTH) {
                stack[sp] = k; lo[sp] = 1 << 30; hi[sp] = -(1 << 30); ++sp;
            }
            break;
        case MM_SEQ_DIVIDER:
            y += 4;
            it->y = y;
            y += th + 14;
            break;
        default:
            break;
        }
        if (has_x) {
            if (sp > 0) seq_extend(&lo[sp - 1], &hi[sp - 1], x0, x1);
            seq_extend(&minx, &maxx, x0, x1);
        }
    }
    d->life_bottom = y + 8;

    /* Shift everything so the leftmost element sits at MM_MARGIN. */
    int dx = MM_MARGIN - minx;
    for (int i = 0; i < n; ++i) {
        d->nodes[i].x = cx[i] - d->nodes[i].w / 2 + dx;
        d->nodes[i].y = MM_MARGIN;
    }
    for (int k = 0; k < d->item_count; ++k) {
        d->items[k].x += dx;
        d->items[k].y += MM_MARGIN;
    }
    d->life_bottom += MM_MARGIN;
    d->width  = maxx - minx + 2 * MM_MARGIN;
    d->height = d->life_bottom + bh + MM_MARGIN;
    free(cx);
}

/* ---- public ------------------------------------------------------------ */

MmDiagram* mermaid_build(const char* src, size_t len,
                         MmMeasureFn measure, void* mctx, int text_h)
{
    MmDiagram* d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->status = MM_EMPTY;
    Builder b = { d, measure, mctx, text_h, 0, 1, 1, {0}, 0 };

    int header_done = 0;
    size_t i = 0;
    while (i < len) {
        size_t ls = i;
        while (i < len && src[i] != '\n') i++;
        const char* line = src + ls;
        int llen = (int)(i - ls);
        if (i < len) i++;                 /* skip newline */

        while (llen > 0 && isspace((unsigned char)line[0]))         { line++; llen--; }
        while (llen > 0 && isspace((unsigned char)line[llen - 1]))  llen--;
        if (llen == 0) continue;
        if (llen >= 2 && line[0] == '%' && line[1] == '%') continue;   /* comment */

        int t = 0;
        while (t < llen && (is_id_ch(line[t]))) t++;

        if (!header_done) {
            header_done = 1;
            int tl = t < (int)sizeof d->type - 1 ? t : (int)sizeof d->type - 1;
            memcpy(d->type, line, (size_t)tl); d->type[tl] = 0;

            if (ci_eq(line, t, "graph") || ci_eq(line, t, "flowchart")) {
                d->status = MM_OK;
                int u = t; while (u < llen && isspace((unsigned char)line[u])) u++;
                int v = u; while (v < llen && isalpha((unsigned char)line[v])) v++;
                if      (ci_eq(line + u, v - u, "TD") || ci_eq(line + u, v - u, "TB")) d->dir = MM_DIR_TB;
                else if (ci_eq(line + u, v - u, "BT")) d->dir = MM_DIR_BT;
                else if (ci_eq(line + u, v - u, "LR")) d->dir = MM_DIR_LR;
                else if (ci_eq(line + u, v - u, "RL")) d->dir = MM_DIR_RL;
                else d->dir = MM_DIR_TB;
            } else if (ci_eq(line, t, "sequenceDiagram")) {
                d->status = MM_OK;
                d->kind   = MM_KIND_SEQUENCE;
            } else {
                d->status = MM_UNSUPPORTED;
            }
            continue;
        }
        if (d->status != MM_OK) continue;

        if (d->kind == MM_KIND_SEQUENCE) { seq_line(&b, line, llen); continue; }

        if (ci_eq(line, t, "subgraph") || ci_eq(line, t, "end") ||
            ci_eq(line, t, "direction") || ci_eq(line, t, "classDef") ||
            ci_eq(line, t, "class") || ci_eq(line, t, "style") ||
            ci_eq(line, t, "linkStyle") || ci_eq(line, t, "click"))
            continue;

        /* split on ';' into separate statements */
        int seg0 = 0;
        for (int k = 0; k <= llen; ++k) {
            if (k == llen || line[k] == ';') {
                if (k > seg0) parse_statement(d, line + seg0, k - seg0);
                seg0 = k + 1;
            }
        }
    }

    if (d->status == MM_OK && d->node_count == 0) d->status = MM_EMPTY;
    if (d->status == MM_OK) {
        if (d->kind == MM_KIND_SEQUENCE) seq_layout(&b);
        else                             mm_layout(&b);
    }
    return d;
}

void mermaid_free(MmDiagram* d)
{
    if (!d) return;
    free(d->nodes);
    free(d->edges);
    free(d->items);
    free(d);
}

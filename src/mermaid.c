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

typedef struct {
    MmDiagram*  d;
    MmMeasureFn measure;
    void*       mctx;
    int         text_h;
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

/* ---- public ------------------------------------------------------------ */

MmDiagram* mermaid_build(const char* src, size_t len,
                         MmMeasureFn measure, void* mctx, int text_h)
{
    MmDiagram* d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->status = MM_EMPTY;
    Builder b = { d, measure, mctx, text_h };

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
            } else {
                d->status = MM_UNSUPPORTED;
            }
            continue;
        }
        if (d->status != MM_OK) continue;

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
    if (d->status == MM_OK) mm_layout(&b);
    return d;
}

void mermaid_free(MmDiagram* d)
{
    if (!d) return;
    free(d->nodes);
    free(d->edges);
    free(d);
}

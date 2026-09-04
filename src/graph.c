#include "graph.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void graph_init(GraphModel* g) { memset(g, 0, sizeof *g); }

void graph_free(GraphModel* g)
{
    if (!g) return;
    free(g->nodes);
    free(g->edges);
    memset(g, 0, sizeof *g);
}

void graph_clear(GraphModel* g)
{
    g->node_count = 0;
    g->edge_count = 0;
}

static GraphNode* graph_new_node(GraphModel* g)
{
    if (g->node_count >= g->node_cap) {
        g->node_cap = g->node_cap ? g->node_cap * 2 : 64;
        g->nodes = realloc(g->nodes, g->node_cap * sizeof *g->nodes);
    }
    GraphNode* n = &g->nodes[g->node_count++];
    memset(n, 0, sizeof *n);
    n->vault_idx = -1;
    n->tag_idx   = -1;
    return n;
}

int graph_add_node(GraphModel* g, int vault_idx)
{
    GraphNode* n = graph_new_node(g);
    n->kind      = GRAPH_NOTE;
    n->vault_idx = vault_idx;
    return g->node_count - 1;
}

int graph_add_tag(GraphModel* g, int tag_idx)
{
    GraphNode* n = graph_new_node(g);
    n->kind    = GRAPH_TAG;
    n->tag_idx = tag_idx;
    return g->node_count - 1;
}

void graph_add_edge(GraphModel* g, int a, int b)
{
    if (a == b || a < 0 || b < 0 ||
        a >= g->node_count || b >= g->node_count) return;
    for (int i = 0; i < g->edge_count; ++i) {
        GraphEdge* e = &g->edges[i];
        if ((e->a == a && e->b == b) || (e->a == b && e->b == a)) return;
    }
    if (g->edge_count >= g->edge_cap) {
        g->edge_cap = g->edge_cap ? g->edge_cap * 2 : 128;
        g->edges = realloc(g->edges, g->edge_cap * sizeof *g->edges);
    }
    g->edges[g->edge_count++] = (GraphEdge){ a, b };
    g->nodes[a].degree++;
    g->nodes[b].degree++;
}

/* Iteration count scales down for big graphs so an on-demand open stays
 * snappy (layout is O(n^2) per step). */
static int layout_iters(int n)
{
    if (n <= 1)   return 0;
    if (n <= 120) return 420;
    if (n <= 400) return 240;
    if (n <= 900) return 130;
    return 70;
}

void graph_layout(GraphModel* g, float area_w, float area_h)
{
    int n = g->node_count;
    if (n <= 1) {
        if (n == 1) { g->nodes[0].x = 0; g->nodes[0].y = 0; }
        return;
    }

    const float area = area_w * area_h;
    const float k    = 0.62f * sqrtf(area / (float)n);   /* ideal edge length */
    const float R    = 0.42f * (area_w < area_h ? area_w : area_h);

    /* Deterministic seed: spread on a golden-angle spiral so no two nodes
     * start coincident (which would make repulsion explode). */
    const float golden = 2.39996323f;   /* ~137.5° in radians */
    for (int i = 0; i < n; ++i) {
        float t   = (float)(i + 1) / (float)n;
        float ang = golden * (float)i;
        float rad = R * sqrtf(t);
        g->nodes[i].x = rad * cosf(ang);
        g->nodes[i].y = rad * sinf(ang);
    }

    int   iters = layout_iters(n);
    float temp  = R;                       /* max move per step, cools to ~0 */
    float cool  = temp / (float)(iters + 1);

    for (int step = 0; step < iters; ++step) {
        for (int i = 0; i < n; ++i) { g->nodes[i].dx = 0; g->nodes[i].dy = 0; }

        /* Repulsion between every pair: f = k^2 / d. */
        for (int i = 0; i < n; ++i) {
            for (int j = i + 1; j < n; ++j) {
                float ddx = g->nodes[i].x - g->nodes[j].x;
                float ddy = g->nodes[i].y - g->nodes[j].y;
                float d2  = ddx * ddx + ddy * ddy;
                if (d2 < 0.01f) { ddx = 0.1f * (float)(i - j); ddy = 0.07f; d2 = ddx*ddx + ddy*ddy; }
                float d   = sqrtf(d2);
                float f   = (k * k) / d;
                float ux  = ddx / d, uy = ddy / d;
                g->nodes[i].dx += ux * f; g->nodes[i].dy += uy * f;
                g->nodes[j].dx -= ux * f; g->nodes[j].dy -= uy * f;
            }
        }

        /* Attraction along edges: f = d^2 / k. */
        for (int e = 0; e < g->edge_count; ++e) {
            GraphNode* a = &g->nodes[g->edges[e].a];
            GraphNode* b = &g->nodes[g->edges[e].b];
            float ddx = a->x - b->x, ddy = a->y - b->y;
            float d   = sqrtf(ddx * ddx + ddy * ddy);
            if (d < 0.01f) continue;
            float f   = (d * d) / k;
            float ux  = ddx / d, uy = ddy / d;
            a->dx -= ux * f; a->dy -= uy * f;
            b->dx += ux * f; b->dy += uy * f;
        }

        /* Gentle pull toward the origin so disconnected components don't drift
         * off to infinity. */
        for (int i = 0; i < n; ++i) {
            g->nodes[i].dx -= g->nodes[i].x * 0.012f;
            g->nodes[i].dy -= g->nodes[i].y * 0.012f;
        }

        /* Apply, capped by the current temperature. */
        for (int i = 0; i < n; ++i) {
            float dl = sqrtf(g->nodes[i].dx * g->nodes[i].dx +
                             g->nodes[i].dy * g->nodes[i].dy);
            if (dl > 0.0001f) {
                float lim = dl < temp ? dl : temp;
                g->nodes[i].x += (g->nodes[i].dx / dl) * lim;
                g->nodes[i].y += (g->nodes[i].dy / dl) * lim;
            }
        }
        temp -= cool;
        if (temp < 0) temp = 0;
    }

    /* Recenter on the centroid so the renderer's fit-to-view is symmetric. */
    float cx = 0, cy = 0;
    for (int i = 0; i < n; ++i) { cx += g->nodes[i].x; cy += g->nodes[i].y; }
    cx /= n; cy /= n;
    for (int i = 0; i < n; ++i) { g->nodes[i].x -= cx; g->nodes[i].y -= cy; }
}

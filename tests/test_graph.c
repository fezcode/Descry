/* Graph model tests: note nodes, `#tag` nodes, and the edge bookkeeping
 * that gives both their radius in the map. */

#include <assert.h>
#include <stdio.h>

#include "graph.h"

static void test_node_kinds(void)
{
    GraphModel g; graph_init(&g);
    int n0 = graph_add_node(&g, 7);
    int t0 = graph_add_tag(&g, 3);
    assert(n0 == 0 && t0 == 1 && g.node_count == 2);

    assert(g.nodes[n0].kind == GRAPH_NOTE);
    assert(g.nodes[n0].vault_idx == 7);
    assert(g.nodes[n0].tag_idx == -1);

    assert(g.nodes[t0].kind == GRAPH_TAG);
    assert(g.nodes[t0].tag_idx == 3);
    /* -1, not 0: a tag node must never be mistaken for vault item 0. */
    assert(g.nodes[t0].vault_idx == -1);
    graph_free(&g);
}

static void test_edges(void)
{
    GraphModel g; graph_init(&g);
    int a = graph_add_node(&g, 0);
    int b = graph_add_node(&g, 1);
    int t = graph_add_tag(&g, 0);

    graph_add_edge(&g, a, t);
    graph_add_edge(&g, b, t);
    graph_add_edge(&g, a, t);      /* duplicate: ignored */
    graph_add_edge(&g, t, a);      /* reversed duplicate: ignored */
    graph_add_edge(&g, a, a);      /* self-loop: ignored */
    graph_add_edge(&g, a, 99);     /* out of range: ignored */

    assert(g.edge_count == 2);
    assert(g.nodes[t].degree == 2);   /* the tag is the hub */
    assert(g.nodes[a].degree == 1);
    assert(g.nodes[b].degree == 1);
    graph_free(&g);
}

static void test_layout_is_finite_and_centred(void)
{
    GraphModel g; graph_init(&g);
    for (int i = 0; i < 12; ++i) graph_add_node(&g, i);
    for (int i = 0; i < 6; ++i) graph_add_tag(&g, i);
    for (int i = 0; i < 12; ++i) graph_add_edge(&g, i, 12 + (i % 6));
    graph_layout(&g, 1400.0f, 1000.0f);

    float cx = 0, cy = 0;
    for (int i = 0; i < g.node_count; ++i) {
        assert(g.nodes[i].x == g.nodes[i].x);   /* not NaN */
        assert(g.nodes[i].y == g.nodes[i].y);
        cx += g.nodes[i].x;
        cy += g.nodes[i].y;
    }
    cx /= g.node_count; cy /= g.node_count;
    assert(cx < 0.01f && cx > -0.01f);
    assert(cy < 0.01f && cy > -0.01f);
    graph_free(&g);
}

int main(void)
{
    test_node_kinds();
    test_edges();
    test_layout_is_finite_and_centred();
    printf("test_graph: all passed\n");
    return 0;
}

#ifndef DESCRY_GRAPH_H
#define DESCRY_GRAPH_H

/* Force-directed layout of the vault's relation graph. Pure C, no SDL — the
 * model holds node positions in an arbitrary world space centered on the
 * origin; the renderer fits that to a viewport. One node per note and one
 * per `#tag`; one edge per `[[wiki link]]` between two notes and one
 * per tag mention. */

typedef enum {
    GRAPH_NOTE = 0,
    GRAPH_TAG  = 1,
} GraphNodeKind;

typedef struct {
    float x, y;          /* world-space position (origin-centered)      */
    float dx, dy;        /* scratch displacement, per layout step       */
    int   kind;          /* GRAPH_NOTE / GRAPH_TAG                      */
    int   vault_idx;     /* GRAPH_NOTE: index into Vault.items, else -1 */
    int   tag_idx;       /* GRAPH_TAG: index into the tag store, else -1 */
    int   degree;        /* edge count; drives node radius              */
} GraphNode;

typedef struct { int a, b; } GraphEdge;

typedef struct {
    GraphNode* nodes;
    int        node_count, node_cap;
    GraphEdge* edges;
    int        edge_count, edge_cap;
} GraphModel;

void graph_init(GraphModel* g);
void graph_free(GraphModel* g);
void graph_clear(GraphModel* g);

/* Append a note node for the given vault item. Returns its node index. */
int  graph_add_node(GraphModel* g, int vault_idx);

/* Append a tag node for the given index into the caller's tag store.
 * Returns its node index. */
int  graph_add_tag(GraphModel* g, int tag_idx);

/* Add an undirected edge between node indices a and b. Self-loops and exact
 * duplicates are ignored. Bumps both endpoints' degree. */
void graph_add_edge(GraphModel* g, int a, int b);

/* Run the Fruchterman–Reingold layout. `area_w`/`area_h` set the nominal
 * canvas the spacing constant is derived from (the result is rescaled to fit
 * any viewport later). Deterministic: seed positions are index-derived. */
void graph_layout(GraphModel* g, float area_w, float area_h);

#endif

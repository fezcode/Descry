#ifndef DESCRY_MERMAID_H
#define DESCRY_MERMAID_H

#include <stddef.h>

/* Minimal Mermaid support: parse + lay out flowchart/graph diagrams into
 * positioned nodes + edges that the renderer can draw with SDL primitives.
 *
 * Supported today: `graph` / `flowchart` with directions TD/TB/BT/LR/RL,
 * node shapes [rect] (round) ([stadium]) {diamond} ((circle)) {{hexagon}}
 * [[subroutine]], and links --> --- -.-> ==> (with |label| or middle text).
 * Any other diagram type (sequenceDiagram, classDiagram, …) parses to
 * status == MM_UNSUPPORTED so the caller can render a graceful fallback card
 * instead of raw code. */

typedef enum {
    MM_SHAPE_RECT = 0,   /* [text]    */
    MM_SHAPE_ROUND,      /* (text)    */
    MM_SHAPE_STADIUM,    /* ([text])  */
    MM_SHAPE_SUBROUT,    /* [[text]]  */
    MM_SHAPE_DIAMOND,    /* {text}    */
    MM_SHAPE_CIRCLE,     /* ((text))  */
    MM_SHAPE_HEX,        /* {{text}}  */
} MmShape;

typedef enum { MM_DIR_TB = 0, MM_DIR_BT, MM_DIR_LR, MM_DIR_RL } MmDir;

typedef struct {
    char    id[80];
    char    label[256];
    MmShape shape;
    int     layer;        /* layering rank (set by layout)      */
    int     order;        /* position within layer              */
    int     x, y, w, h;   /* px, top-left, filled by layout     */
} MmNode;

typedef struct {
    int  from, to;        /* node indices                       */
    char label[160];
    int  dashed;          /* 1 = dotted link                    */
    int  arrow_to;        /* arrowhead at target                */
    int  arrow_from;      /* arrowhead at source (bidirectional)*/
} MmEdge;

typedef enum { MM_OK = 0, MM_EMPTY, MM_UNSUPPORTED } MmStatus;

/* Measure the pixel width of a UTF-8 run in the label font. */
typedef int (*MmMeasureFn)(void* ctx, const char* utf8, size_t len);

typedef struct {
    MmStatus status;
    char     type[40];        /* first token: "graph"/"flowchart"/… */
    MmDir    dir;
    MmNode*  nodes; int node_count;
    MmEdge*  edges; int edge_count;
    int      width, height;   /* total laid-out bounds in px        */
} MmDiagram;

/* Parse + lay out `src` (len bytes). `measure` returns label widths in the
 * caller's font; `text_h` is that font's line height in px. Returns a heap
 * diagram (free with mermaid_free), never NULL except on OOM. */
MmDiagram* mermaid_build(const char* src, size_t len,
                         MmMeasureFn measure, void* mctx, int text_h);
void       mermaid_free(MmDiagram* d);

#endif

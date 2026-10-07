#ifndef DESCRY_MERMAID_H
#define DESCRY_MERMAID_H

#include <stddef.h>

/* Minimal Mermaid support: parse + lay out flowchart/graph diagrams into
 * positioned nodes + edges that the renderer can draw with SDL primitives.
 *
 * Supported today: `graph` / `flowchart` with directions TD/TB/BT/LR/RL,
 * node shapes [rect] (round) ([stadium]) {diamond} ((circle)) {{hexagon}}
 * [[subroutine]], and links --> --- -.-> ==> (with |label| or middle text).
 *
 * Also `sequenceDiagram`: participant/actor (with `as` aliases), messages
 * ->> -->> -> --> -x --x -) --) <<->> <<-->>, notes (left of / right of /
 * over A[,B]), autonumber, and loop/alt/opt/par/critical/break/rect frames
 * with else/and/option dividers. Participants land in `nodes`, everything
 * else in `items`.
 *
 * Any other diagram type (classDiagram, gantt, …) parses to
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

typedef enum { MM_KIND_FLOW = 0, MM_KIND_SEQUENCE } MmKind;

#define MM_SEQ_SELF_W 36   /* how far a self-message loop sticks out, px */

/* Sequence diagram rows, in source order. */
typedef enum {
    MM_SEQ_MSG = 0,      /* from -> to (from == to: self message)        */
    MM_SEQ_NOTE,         /* note box over / beside from[..to]            */
    MM_SEQ_FRAME,        /* loop/alt/... box opening                     */
    MM_SEQ_DIVIDER,      /* else/and/option line; `from` = frame item    */
    MM_SEQ_END,          /* frame close; `from` = frame item             */
} MmSeqType;

typedef enum { MM_HEAD_ARROW = 0, MM_HEAD_NONE, MM_HEAD_CROSS, MM_HEAD_ASYNC } MmHead;
typedef enum { MM_NOTE_OVER = 0, MM_NOTE_LEFT, MM_NOTE_RIGHT } MmNotePlace;

typedef struct {
    MmSeqType type;
    int  from, to;        /* participant indices (msg/note); frame item index
                           * for divider/end                                */
    char label[160];
    char kw[16];          /* frame keyword shown in its tag ("" for rect)  */
    int  dashed;          /* msg: dotted line                              */
    int  head;            /* msg: MmHead at `to`                           */
    int  both;            /* msg: head at `from` too                       */
    int  place;           /* note: MmNotePlace                             */
    int  number;          /* msg: autonumber value, 0 = none               */
    int  x, y, w, h;      /* layout px: msg → line y (x/w = horizontal
                           * extent incl. label); note/frame → box;
                           * divider → line y, x/w of its frame            */
} MmSeqItem;

/* Measure the pixel width of a UTF-8 run in the label font. */
typedef int (*MmMeasureFn)(void* ctx, const char* utf8, size_t len);

typedef struct {
    MmStatus status;
    MmKind   kind;
    char     type[40];        /* first token: "graph"/"flowchart"/… */
    MmDir    dir;
    MmNode*  nodes; int node_count;
    MmEdge*  edges; int edge_count;
    MmSeqItem* items; int item_count;   /* sequence diagrams only  */
    int      life_bottom;     /* sequence: y of the bottom participant
                               * boxes (lifelines run down to it)  */
    int      width, height;   /* total laid-out bounds in px        */
} MmDiagram;

/* Parse + lay out `src` (len bytes). `measure` returns label widths in the
 * caller's font; `text_h` is that font's line height in px. Returns a heap
 * diagram (free with mermaid_free), never NULL except on OOM. */
MmDiagram* mermaid_build(const char* src, size_t len,
                         MmMeasureFn measure, void* mctx, int text_h);
void       mermaid_free(MmDiagram* d);

#endif

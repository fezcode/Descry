/* Mermaid parser tests: flowcharts still lay out, sequence diagrams parse
 * participants / messages / notes / frames, and unknown types fall back. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mermaid.h"

static MmDiagram* build(const char* src)
{
    return mermaid_build(src, strlen(src), NULL, NULL, 16);
}

static void test_flowchart(void)
{
    MmDiagram* d = build("graph TD\n  A[Start] --> B{Ok?}\n  B -->|yes| C\n");
    assert(d->status == MM_OK && d->kind == MM_KIND_FLOW);
    assert(d->node_count == 3 && d->edge_count == 2);
    assert(strcmp(d->edges[1].label, "yes") == 0);
    assert(d->width > 0 && d->height > 0);
    mermaid_free(d);
}

static void test_sequence_basic(void)
{
    MmDiagram* d = build("sequenceDiagram\n"
                         "    Alice->>Bob: Hello Bob\n"
                         "    Bob-->>Alice: Hi Alice\n");
    assert(d->status == MM_OK && d->kind == MM_KIND_SEQUENCE);
    assert(d->node_count == 2);
    assert(strcmp(d->nodes[0].id, "Alice") == 0);
    assert(strcmp(d->nodes[1].id, "Bob") == 0);
    assert(d->item_count == 2);
    assert(d->items[0].type == MM_SEQ_MSG);
    assert(d->items[0].from == 0 && d->items[0].to == 1);
    assert(!d->items[0].dashed && d->items[0].head == MM_HEAD_ARROW);
    assert(strcmp(d->items[0].label, "Hello Bob") == 0);
    assert(d->items[1].dashed && d->items[1].from == 1);
    /* Rows go downward; Bob's box sits right of Alice's. */
    assert(d->items[1].y > d->items[0].y);
    assert(d->nodes[1].x > d->nodes[0].x + d->nodes[0].w);
    assert(d->life_bottom > d->items[1].y && d->height > d->life_bottom);
    mermaid_free(d);
}

static void test_sequence_features(void)
{
    MmDiagram* d = build("sequenceDiagram\n"
                         "  autonumber\n"
                         "  actor U as The User\n"
                         "  participant S as \"Server\"\n"
                         "  U->>+S: Login<br/>now\n"
                         "  S--xU: denied\n"
                         "  S-)S: retry\n"
                         "  Note over U,S: shared\n"
                         "  Note right of S: aside\n"
                         "  alt ok\n"
                         "    S->U: yes\n"
                         "  else bad\n"
                         "    S-->U: no\n"
                         "  end\n"
                         "  activate S\n"
                         "  title ignored\n");
    assert(d->status == MM_OK && d->node_count == 2);
    assert(strcmp(d->nodes[0].label, "The User") == 0);
    assert(d->nodes[0].shape == MM_SHAPE_STADIUM);
    assert(strcmp(d->nodes[1].label, "Server") == 0);

    const MmSeqItem* it = d->items;
    assert(d->item_count == 10);
    assert(it[0].type == MM_SEQ_MSG && it[0].to == 1 && it[0].number == 1);
    assert(strcmp(it[0].label, "Login now") == 0);
    assert(it[1].head == MM_HEAD_CROSS && it[1].dashed && it[1].number == 2);
    assert(it[2].head == MM_HEAD_ASYNC && it[2].from == 1 && it[2].to == 1);
    assert(it[3].type == MM_SEQ_NOTE && it[3].place == MM_NOTE_OVER);
    assert(it[3].from == 0 && it[3].to == 1);
    assert(it[4].type == MM_SEQ_NOTE && it[4].place == MM_NOTE_RIGHT);
    assert(it[5].type == MM_SEQ_FRAME && strcmp(it[5].kw, "alt") == 0);
    assert(strcmp(it[5].label, "ok") == 0);
    assert(it[6].head == MM_HEAD_NONE && !it[6].dashed);
    assert(it[7].type == MM_SEQ_DIVIDER && it[7].from == 5);
    assert(strcmp(it[7].label, "bad") == 0);
    assert(it[8].head == MM_HEAD_NONE && it[8].dashed);
    assert(it[9].type == MM_SEQ_END && it[9].from == 5);
    /* The frame wraps its rows, and the divider spans the frame. */
    assert(it[5].y < it[6].y && it[5].y + it[5].h > it[8].y);
    assert(it[7].x == it[5].x && it[7].w == it[5].w);
    /* Nothing laid out left of the margin. */
    for (int k = 0; k < d->item_count; ++k)
        if (it[k].type == MM_SEQ_NOTE || it[k].type == MM_SEQ_FRAME)
            assert(it[k].x >= 0 && it[k].x + it[k].w <= d->width);
    mermaid_free(d);
}

static void test_unclosed_frame(void)
{
    MmDiagram* d = build("sequenceDiagram\n  loop forever\n  A->>B: tick\n");
    assert(d->status == MM_OK && d->item_count == 2);
    assert(d->items[0].type == MM_SEQ_FRAME && d->items[0].h > 0);
    mermaid_free(d);
}

static void test_unsupported(void)
{
    MmDiagram* d = build("classDiagram\n  Animal <|-- Duck\n");
    assert(d->status == MM_UNSUPPORTED);
    assert(strcmp(d->type, "classDiagram") == 0);
    mermaid_free(d);

    d = build("sequenceDiagram\n");
    assert(d->status == MM_EMPTY);
    mermaid_free(d);
}

int main(void)
{
    test_flowchart();
    test_sequence_basic();
    test_sequence_features();
    test_unclosed_frame();
    test_unsupported();
    printf("test_mermaid: all passed\n");
    return 0;
}

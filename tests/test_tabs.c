#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "tabs.h"
#include "buffer.h"

static void test_find_append(void) {
    TabList t; tablist_init(&t);
    assert(t.count == 0 && t.active == -1);
    assert(tablist_find(&t, "a.md") == -1);
    int i = tablist_append(&t, "a.md");
    int j = tablist_append(&t, "b.md");
    assert(i == 0 && j == 1 && t.count == 2);
    assert(tablist_find(&t, "a.md") == 0);
    assert(tablist_find(&t, "b.md") == 1);
    assert(tablist_find(&t, "c.md") == -1);
    assert(tablist_find(&t, NULL) == -1);
    tablist_free(&t);
}

static void test_store_load_roundtrip(void) {
    TabList t; tablist_init(&t);
    tablist_append(&t, NULL);            /* slot 0, empty husk */

    Buffer live; buffer_init(&live);
    buffer_set_text(&live, "hello world", 11);
    live.dirty = true;
    /* park live -> slot 0 */
    tab_store(&t.items[0], "a.md", &live, 42, 7, true, false);
    /* live is now an empty husk */
    assert(live.len == 0);
    assert(strcmp(t.items[0].path, "a.md") == 0);
    assert(t.items[0].buf.len == 11);
    assert(t.items[0].buf.dirty == true);
    assert(t.items[0].scroll_y == 42 && t.items[0].scroll_x == 7);
    assert(t.items[0].edit_mode == true);

    /* load slot 0 -> fresh live */
    char* path = NULL; Buffer live2; buffer_init(&live2);
    int sy=0,sx=0; bool em=false,vi=true;
    tab_load(&t.items[0], &path, &live2, &sy, &sx, &em, &vi);
    assert(strcmp(path, "a.md") == 0);
    assert(live2.len == 11 && memcmp(live2.data, "hello world", 11) == 0);
    assert(sy == 42 && sx == 7 && em == true && vi == false);
    /* slot buffer is now an empty husk */
    assert(t.items[0].buf.len == 0);

    free(path);
    buffer_free(&live);
    buffer_free(&live2);
    tablist_free(&t);
}

static void test_remove_neighbor(void) {
    TabList t; tablist_init(&t);
    tablist_append(&t, "a.md");
    tablist_append(&t, "b.md");
    tablist_append(&t, "c.md");
    /* remove middle -> right neighbor (was index 2 "c", now shifts to 1) */
    int next = tablist_remove(&t, 1);
    assert(t.count == 2);
    assert(strcmp(t.items[0].path, "a.md") == 0);
    assert(strcmp(t.items[1].path, "c.md") == 0);
    assert(next == 1);                    /* "c.md" */
    /* remove last -> left neighbor */
    next = tablist_remove(&t, 1);
    assert(t.count == 1 && next == 0);
    /* remove only -> -1 */
    next = tablist_remove(&t, 0);
    assert(t.count == 0 && next == -1);
    tablist_free(&t);
}

static void test_parse_state_line(void) {
    char p[512]; int act = -1;
    assert(tabs_parse_state_line("@tab=notes/a.md", p, sizeof p, &act) == 1);
    assert(strcmp(p, "notes/a.md") == 0);
    assert(tabs_parse_state_line("@active=3", p, sizeof p, &act) == 2);
    assert(act == 3);
    assert(tabs_parse_state_line("@sidebar_w=240", p, sizeof p, &act) == 0);
    assert(tabs_parse_state_line("some/collapsed/dir", p, sizeof p, &act) == 0);
}

int main(void) {
    test_find_append();
    test_store_load_roundtrip();
    test_remove_neighbor();
    test_parse_state_line();
    printf("all tab tests passed\n");
    return 0;
}

#include "tabs.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void tablist_init(TabList* t) {
    t->items = NULL; t->count = 0; t->cap = 0; t->active = -1;
}

void tablist_free(TabList* t) {
    for (int i = 0; i < t->count; ++i) {
        free(t->items[i].path);
        buffer_free(&t->items[i].buf);
    }
    free(t->items);
    tablist_init(t);
}

int tablist_find(const TabList* t, const char* path) {
    if (!path) return -1;
    for (int i = 0; i < t->count; ++i)
        if (t->items[i].path && strcmp(t->items[i].path, path) == 0) return i;
    return -1;
}

int tablist_append(TabList* t, const char* path) {
    if (t->count == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 8;
        t->items = realloc(t->items, (size_t)t->cap * sizeof *t->items);
    }
    Tab* nt = &t->items[t->count];
    memset(nt, 0, sizeof *nt);
    nt->path = path ? strdup(path) : NULL;
    buffer_init(&nt->buf);            /* fresh empty buffer (owns its alloc) */
    nt->scroll_y = nt->scroll_x = 0;
    nt->edit_mode = false;
    nt->viewing_image = false;
    return t->count++;
}

void tab_buf_move(Buffer* dst, Buffer* src) {
    buffer_free(dst);                 /* release dst's current contents */
    *dst = *src;                      /* transfer ownership */
    buffer_init(src);                 /* src becomes a fresh empty buffer */
}

void tab_store(Tab* slot, const char* path, Buffer* buf,
               int scroll_y, int scroll_x, bool edit_mode, bool viewing_image) {
    free(slot->path);
    slot->path = path ? strdup(path) : NULL;
    tab_buf_move(&slot->buf, buf);
    slot->scroll_y = scroll_y; slot->scroll_x = scroll_x;
    slot->edit_mode = edit_mode; slot->viewing_image = viewing_image;
}

void tab_load(Tab* slot, char** out_path, Buffer* out_buf,
              int* scroll_y, int* scroll_x, bool* edit_mode, bool* viewing_image) {
    free(*out_path);
    *out_path = slot->path ? strdup(slot->path) : NULL;
    tab_buf_move(out_buf, &slot->buf);
    *scroll_y = slot->scroll_y; *scroll_x = slot->scroll_x;
    *edit_mode = slot->edit_mode; *viewing_image = slot->viewing_image;
}

int tablist_remove(TabList* t, int i) {
    if (i < 0 || i >= t->count) return t->active;
    free(t->items[i].path);
    buffer_free(&t->items[i].buf);
    for (int j = i; j < t->count - 1; ++j) t->items[j] = t->items[j + 1];
    t->count--;
    if (t->count == 0) return -1;
    /* right neighbor now sits at index i (post-shift); clamp to last. */
    return (i < t->count) ? i : t->count - 1;
}

int tabs_parse_state_line(const char* line, char* out_path, int cap, int* out_active) {
    if (strncmp(line, "@tab=", 5) == 0) {
        snprintf(out_path, (size_t)cap, "%s", line + 5);
        return 1;
    }
    if (strncmp(line, "@active=", 8) == 0) {
        *out_active = atoi(line + 8);
        return 2;
    }
    return 0;
}

int tabs_strip_clamp(int scroll, int content_w, int strip_w) {
    int max = content_w - strip_w;
    if (max < 0) max = 0;
    if (scroll > max) scroll = max;
    if (scroll < 0) scroll = 0;
    return scroll;
}

int tabs_strip_follow(int scroll, int content_w, int strip_w,
                      int chip_x, int chip_w) {
    if (chip_x < scroll)                          scroll = chip_x;
    else if (chip_x + chip_w > scroll + strip_w)  scroll = chip_x + chip_w - strip_w;
    if (chip_x < scroll)                          scroll = chip_x;   /* wider than the strip */
    return tabs_strip_clamp(scroll, content_w, strip_w);
}

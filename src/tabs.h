#ifndef DESCRY_TABS_H
#define DESCRY_TABS_H

#include <stdbool.h>
#include "buffer.h"

/* One open file. While a tab is the active one, its `buf` is an empty husk;
 * the live App fields hold the real state until the tab is parked. */
typedef struct {
    char*  path;            /* owned; NULL = unsaved scratch */
    Buffer buf;             /* parked text+cursor+undo+dirty (moved in/out) */
    int    scroll_y, scroll_x;
    bool   edit_mode;
    bool   viewing_image;
    /* File identity when this tab was last read/written, for the external-
     * change watch. Carried across park/restore so a file edited while the
     * tab sat in the background is still caught when you switch back. */
    long long disk_mtime, disk_size;
} Tab;

typedef struct {
    Tab* items;
    int  count;
    int  cap;
    int  active;            /* index of active tab, or -1 if none */
} TabList;

void tablist_init(TabList* t);
void tablist_free(TabList* t);

/* Index of the tab whose path equals `path` (exact strcmp; NULL never matches),
 * or -1. */
int  tablist_find(const TabList* t, const char* path);

/* Append a new tab for `path` (may be NULL). Its buffer is buffer_init'd and
 * scroll/mode default to 0/false. Returns the new index. */
int  tablist_append(TabList* t, const char* path);

/* Leak-safe shallow move of a Buffer: frees dst's current contents, transfers
 * src's ownership to dst, and re-inits src to a fresh empty buffer. */
void tab_buf_move(Buffer* dst, Buffer* src);

/* Store live state INTO `slot` (parks it). Moves *buf into slot->buf and
 * replaces slot->path with strdup(path). */
void tab_store(Tab* slot, const char* path, Buffer* buf,
               int scroll_y, int scroll_x, bool edit_mode, bool viewing_image);

/* Load `slot` OUT into the live destinations. Frees *out_path, sets it to
 * strdup(slot->path), and moves slot->buf into *out_buf. */
void tab_load(Tab* slot, char** out_path, Buffer* out_buf,
              int* scroll_y, int* scroll_x, bool* edit_mode, bool* viewing_image);

/* Remove tab i (frees its path + buffer, shifts the array). Returns the index
 * that should become active next: the right neighbor, else the left, else -1.
 * The returned index is already adjusted for the post-removal array. */
int  tablist_remove(TabList* t, int i);

/* Parse one line of a vault section in the app-data state file (.state).
 *   "@tab=PATH"  -> copies PATH into out_path (cap bytes), returns 1
 *   "@active=N"  -> sets *out_active = N, returns 2
 *   anything else -> returns 0
 */
int  tabs_parse_state_line(const char* line, char* out_path, int cap, int* out_active);

/* Tab-strip overflow. `content_w` is the total width of every chip laid end
 * to end, `strip_w` the visible strip, `scroll` how far the chips are shifted
 * left — all in pixels, chip positions measured from the strip's left edge
 * before scrolling ("content space"). */

/* Clamp `scroll` so the strip never shows empty space past either end. */
int  tabs_strip_clamp(int scroll, int content_w, int strip_w);

/* Smallest move of `scroll` that brings the chip at [chip_x, chip_x + chip_w)
 * fully into view (its left edge wins when the chip is wider than the strip).
 * The result is clamped. */
int  tabs_strip_follow(int scroll, int content_w, int strip_w,
                       int chip_x, int chip_w);

#endif

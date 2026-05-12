#ifndef DOWNSEE_BUFFER_H
#define DOWNSEE_BUFFER_H

#include <stdbool.h>
#include <stddef.h>

/* v0.9: contiguous text buffer with UTF-8-aware navigation and an undo log
 * with simple coalescing (consecutive typing or backspace at the same
 * position is merged into one undo unit). */

typedef enum {
    OP_NONE,
    OP_INSERT,
    OP_DELETE_BACK,    /* deleted bytes are BEFORE the cursor at op time */
    OP_DELETE_FWD,     /* deleted bytes are AT/AFTER the cursor          */
} OpKind;

typedef struct {
    OpKind kind;
    size_t pos;
    char*  text;       /* malloc'd; n bytes, no NUL */
    size_t n;
} BufferOp;

typedef struct {
    char*  data;
    size_t len;
    size_t cap;

    size_t cursor;
    long   sel_anchor;
    int    pref_col;
    bool   dirty;

    /* undo log */
    BufferOp* ops;
    size_t    op_count;
    size_t    op_cap;
    size_t    op_head;        /* number of ops currently applied to data    */
    size_t    saved_head;     /* op_head at last save; dirty = (head!=saved) */
    OpKind    coalesce;       /* OP_NONE breaks coalescing                  */
} Buffer;

void buffer_init(Buffer* b);
void buffer_free(Buffer* b);

void buffer_set_text(Buffer* b, const char* s, size_t n);
int  buffer_save    (Buffer* b, const char* path);

void buffer_insert         (Buffer* b, const char* utf8, size_t n);
void buffer_delete_back    (Buffer* b);
void buffer_delete_forward (Buffer* b);
void buffer_delete_selection(Buffer* b);

void buffer_undo(Buffer* b);
void buffer_redo(Buffer* b);

/* Call between logical user actions (cursor motion, mode switch, paste) so
 * the next edit starts a fresh undo group. */
void buffer_undo_break(Buffer* b);

bool buffer_has_selection (const Buffer* b);
void buffer_get_selection (const Buffer* b, size_t* lo, size_t* hi);
void buffer_clear_selection(Buffer* b);
void buffer_select_all    (Buffer* b);

void buffer_move_left  (Buffer* b, bool select);
void buffer_move_right (Buffer* b, bool select);
void buffer_move_up    (Buffer* b, bool select);
void buffer_move_down  (Buffer* b, bool select);
void buffer_move_line_start(Buffer* b, bool select);
void buffer_move_line_end  (Buffer* b, bool select);
void buffer_move_doc_start (Buffer* b, bool select);
void buffer_move_doc_end   (Buffer* b, bool select);

void buffer_set_cursor(Buffer* b, size_t pos, bool select);

size_t buffer_line_count(const Buffer* b);
size_t buffer_line_start(const Buffer* b, size_t line);
size_t buffer_line_end  (const Buffer* b, size_t line);
void   buffer_cursor_pos(const Buffer* b, size_t* line, size_t* col);

#endif

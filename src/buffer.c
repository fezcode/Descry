#include "buffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

static size_t utf8_prev(const char* s, size_t pos)
{
    if (pos == 0) return 0;
    pos--;
    while (pos > 0 && is_cont((unsigned char)s[pos])) pos--;
    return pos;
}

static size_t utf8_next(const char* s, size_t len, size_t pos)
{
    if (pos >= len) return len;
    pos++;
    while (pos < len && is_cont((unsigned char)s[pos])) pos++;
    return pos;
}

static size_t line_start_of(const char* s, size_t pos)
{
    while (pos > 0 && s[pos - 1] != '\n') pos--;
    return pos;
}

static size_t line_end_of(const char* s, size_t len, size_t pos)
{
    while (pos < len && s[pos] != '\n') pos++;
    return pos;
}

static void reserve(Buffer* b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap) return;
    size_t nc = b->cap ? b->cap * 2 : 1024;
    while (nc < b->len + extra + 1) nc *= 2;
    b->data = realloc(b->data, nc);
    b->cap  = nc;
}

/* ---------- line-start index ------------------------------------------ */
static void lis_reserve(Buffer* b, size_t extra)
{
    if (b->line_starts_count + extra <= b->line_starts_cap) return;
    size_t nc = b->line_starts_cap ? b->line_starts_cap * 2 : 64;
    while (nc < b->line_starts_count + extra) nc *= 2;
    b->line_starts = realloc(b->line_starts, nc * sizeof(size_t));
    b->line_starts_cap = nc;
}

/* Keep line_rows[] sized to at least `need` and parallel to line_starts. */
static void lir_reserve(Buffer* b, size_t need)
{
    if (need <= b->line_rows_cap) return;
    size_t nc = b->line_rows_cap ? b->line_rows_cap * 2 : 64;
    while (nc < need) nc *= 2;
    b->line_rows = realloc(b->line_rows, nc * sizeof(int));
    b->line_rows_cap = nc;
}

/* Mark line_rows[from .. count) as unknown. memset of 0xff yields -1 (int). */
static void lir_invalidate_from(Buffer* b, size_t from)
{
    if (from >= b->line_starts_count) return;
    memset(&b->line_rows[from], 0xff,
           (b->line_starts_count - from) * sizeof(int));
}

/* Smallest index i with line_starts[i] > pos, or count if none. */
static size_t lis_first_after(const Buffer* b, size_t pos)
{
    size_t lo = 0, hi = b->line_starts_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (b->line_starts[mid] > pos) hi = mid;
        else                            lo = mid + 1;
    }
    return lo;
}

static void lis_rebuild(Buffer* b)
{
    b->line_starts_count = 0;
    lis_reserve(b, 1);
    b->line_starts[b->line_starts_count++] = 0;
    for (size_t i = 0; i < b->len; ++i) {
        if (b->data[i] == '\n') {
            lis_reserve(b, 1);
            b->line_starts[b->line_starts_count++] = i + 1;
        }
    }
    /* Reset row cache — entire doc unknown. */
    lir_reserve(b, b->line_starts_count);
    memset(b->line_rows, 0xff, b->line_starts_count * sizeof(int));
}

/* Patch the index after `n` bytes were inserted at `pos`. The index still
 * carries the pre-insert offsets; this routine relocates and inserts. */
static void lis_after_insert(Buffer* b, size_t pos, const char* s, size_t n)
{
    if (n == 0) return;
    size_t nl = 0;
    for (size_t i = 0; i < n; ++i) if (s[i] == '\n') nl++;
    size_t after = lis_first_after(b, pos);
    if (nl > 0) {
        lis_reserve(b, nl);
        memmove(&b->line_starts[after + nl],
                &b->line_starts[after],
                (b->line_starts_count - after) * sizeof(size_t));
        b->line_starts_count += nl;
        /* Mirror the shift in the row cache so indices stay parallel. */
        lir_reserve(b, b->line_starts_count);
        memmove(&b->line_rows[after + nl],
                &b->line_rows[after],
                (b->line_starts_count - nl - after) * sizeof(int));
    } else {
        lir_reserve(b, b->line_starts_count);
    }
    for (size_t i = after + nl; i < b->line_starts_count; ++i)
        b->line_starts[i] += n;
    size_t k = after;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == '\n') b->line_starts[k++] = pos + i + 1;
    }
    /* Touched line + everything below: invalidate (handles fence cascades). */
    size_t touched = (after > 0) ? after - 1 : 0;
    lir_invalidate_from(b, touched);
}

/* Patch after deleting [pos, pos+n). Lines whose start is in (pos, pos+n]
 * disappear (their birth-newline was deleted); later starts shift -n. */
static void lis_after_delete(Buffer* b, size_t pos, size_t n)
{
    if (n == 0) return;
    size_t first_dead = lis_first_after(b, pos);
    size_t first_kept = lis_first_after(b, pos + n);
    if (first_kept > first_dead) {
        size_t dead_count = first_kept - first_dead;
        memmove(&b->line_starts[first_dead],
                &b->line_starts[first_kept],
                (b->line_starts_count - first_kept) * sizeof(size_t));
        memmove(&b->line_rows[first_dead],
                &b->line_rows[first_kept],
                (b->line_starts_count - first_kept) * sizeof(int));
        b->line_starts_count -= dead_count;
    }
    for (size_t i = first_dead; i < b->line_starts_count; ++i)
        b->line_starts[i] -= n;
    size_t touched = (first_dead > 0) ? first_dead - 1 : 0;
    lir_invalidate_from(b, touched);
}

void buffer_invalidate_row_cache(Buffer* b)
{
    if (!b || !b->line_rows) return;
    memset(b->line_rows, 0xff, b->line_starts_count * sizeof(int));
}

/* ---------- raw data mutations (no op tracking) ------------------------ */
static void apply_insert(Buffer* b, size_t pos, const char* s, size_t n)
{
    reserve(b, n);
    memmove(b->data + pos + n, b->data + pos, b->len - pos + 1);
    memcpy(b->data + pos, s, n);
    b->len += n;
    lis_after_insert(b, pos, s, n);
}

static void apply_delete(Buffer* b, size_t pos, size_t n)
{
    memmove(b->data + pos, b->data + pos + n, b->len - pos - n + 1);
    b->len -= n;
    lis_after_delete(b, pos, n);
}

/* ---------- op log management ----------------------------------------- */
static void ops_reserve(Buffer* b, size_t extra)
{
    if (b->op_count + extra <= b->op_cap) return;
    size_t nc = b->op_cap ? b->op_cap * 2 : 64;
    while (nc < b->op_count + extra) nc *= 2;
    b->ops = realloc(b->ops, nc * sizeof(BufferOp));
    b->op_cap = nc;
}

/* Drop redo branch ops past op_head. Call before pushing a new op. */
static void truncate_redo(Buffer* b)
{
    for (size_t i = b->op_head; i < b->op_count; ++i)
        free(b->ops[i].text);
    b->op_count = b->op_head;
    /* If the saved snapshot is now in the redo branch (impossible to reach),
     * mark as "permanently dirty" by sentinelling saved_head. */
    if (b->saved_head > b->op_head) b->saved_head = (size_t)-1;
}

static void push_op(Buffer* b, OpKind kind, size_t pos,
                    const char* s, size_t n)
{
    truncate_redo(b);
    ops_reserve(b, 1);
    BufferOp* op = &b->ops[b->op_count++];
    op->kind = kind;
    op->pos  = pos;
    op->n    = n;
    op->text = malloc(n);
    memcpy(op->text, s, n);
    b->op_head = b->op_count;
}

static void update_dirty(Buffer* b)
{
    b->dirty = (b->op_head != b->saved_head);
}

void buffer_undo_break(Buffer* b) { b->coalesce = OP_NONE; }

/* ---------- init / free / load / save --------------------------------- */
void buffer_init(Buffer* b)
{
    memset(b, 0, sizeof *b);
    b->sel_anchor = -1;
    reserve(b, 0);
    b->data[0] = 0;
    lis_rebuild(b);
}

void buffer_free(Buffer* b)
{
    if (!b) return;
    free(b->data);
    for (size_t i = 0; i < b->op_count; ++i) free(b->ops[i].text);
    free(b->ops);
    free(b->line_starts);
    free(b->line_rows);
    memset(b, 0, sizeof *b);
    b->sel_anchor = -1;
}

void buffer_set_text(Buffer* b, const char* s, size_t n)
{
    if (n + 1 > b->cap) {
        b->cap = n + 1;
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data, s, n);
    b->data[n]    = 0;
    b->len        = n;
    b->cursor     = 0;
    b->sel_anchor = -1;
    b->pref_col   = 0;
    b->dirty      = false;
    /* discard any prior history */
    for (size_t i = 0; i < b->op_count; ++i) free(b->ops[i].text);
    b->op_count = b->op_head = b->saved_head = 0;
    b->coalesce = OP_NONE;
    lis_rebuild(b);
}

int buffer_save(Buffer* b, const char* path)
{
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(b->data, 1, b->len, f);
    fclose(f);
    if (w != b->len) return -1;
    b->saved_head = b->op_head;
    b->dirty = false;
    /* Break coalescing so the next keystroke pushes a fresh op instead
     * of extending the pre-save op in place. Without this, op_head
     * never moves past saved_head and dirty stays false forever. */
    b->coalesce = OP_NONE;
    return 0;
}

/* ---------- selection ------------------------------------------------- */
bool buffer_has_selection(const Buffer* b)
{
    return b->sel_anchor >= 0 && (size_t)b->sel_anchor != b->cursor;
}

void buffer_get_selection(const Buffer* b, size_t* lo, size_t* hi)
{
    size_t a = (size_t)b->sel_anchor;
    if (a < b->cursor) { *lo = a;         *hi = b->cursor; }
    else               { *lo = b->cursor; *hi = a;         }
}

void buffer_clear_selection(Buffer* b) { b->sel_anchor = -1; }

void buffer_select_all(Buffer* b)
{
    b->sel_anchor = 0;
    b->cursor     = b->len;
    b->pref_col   = 0;
    b->coalesce   = OP_NONE;
}

void buffer_delete_selection(Buffer* b)
{
    if (!buffer_has_selection(b)) return;
    size_t lo, hi;
    buffer_get_selection(b, &lo, &hi);
    size_t n = hi - lo;
    push_op(b, OP_DELETE_FWD, lo, b->data + lo, n);
    apply_delete(b, lo, n);
    b->cursor     = lo;
    b->sel_anchor = -1;
    b->coalesce   = OP_NONE;
    update_dirty(b);
}

/* ---------- edit operations (with coalescing) ------------------------- */
void buffer_insert(Buffer* b, const char* s, size_t n)
{
    if (n == 0) return;
    if (buffer_has_selection(b)) buffer_delete_selection(b);

    /* Coalesce only if previous op was a contiguous insert. */
    if (b->coalesce == OP_INSERT && b->op_count > 0) {
        BufferOp* last = &b->ops[b->op_count - 1];
        if (last->kind == OP_INSERT && last->pos + last->n == b->cursor) {
            last->text = realloc(last->text, last->n + n);
            memcpy(last->text + last->n, s, n);
            last->n += n;
            apply_insert(b, b->cursor, s, n);
            b->cursor += n;
            update_dirty(b);
            return;
        }
    }
    push_op(b, OP_INSERT, b->cursor, s, n);
    b->coalesce = OP_INSERT;
    apply_insert(b, b->cursor, s, n);
    b->cursor += n;
    b->sel_anchor = -1;
    size_t ls = line_start_of(b->data, b->cursor);
    b->pref_col = (int)(b->cursor - ls);
    update_dirty(b);
}

void buffer_delete_back(Buffer* b)
{
    if (buffer_has_selection(b)) { buffer_delete_selection(b); return; }
    if (b->cursor == 0) return;
    size_t prev = utf8_prev(b->data, b->cursor);
    size_t n    = b->cursor - prev;

    if (b->coalesce == OP_DELETE_BACK && b->op_count > 0) {
        BufferOp* last = &b->ops[b->op_count - 1];
        if (last->kind == OP_DELETE_BACK && last->pos == b->cursor) {
            char* nt = malloc(n + last->n);
            memcpy(nt, b->data + prev, n);
            memcpy(nt + n, last->text, last->n);
            free(last->text);
            last->text = nt;
            last->n   += n;
            last->pos  = prev;
            apply_delete(b, prev, n);
            b->cursor = prev;
            update_dirty(b);
            return;
        }
    }
    push_op(b, OP_DELETE_BACK, prev, b->data + prev, n);
    b->coalesce = OP_DELETE_BACK;
    apply_delete(b, prev, n);
    b->cursor = prev;
    size_t ls = line_start_of(b->data, b->cursor);
    b->pref_col = (int)(b->cursor - ls);
    update_dirty(b);
}

void buffer_delete_forward(Buffer* b)
{
    if (buffer_has_selection(b)) { buffer_delete_selection(b); return; }
    if (b->cursor >= b->len) return;
    size_t nxt = utf8_next(b->data, b->len, b->cursor);
    size_t n   = nxt - b->cursor;

    if (b->coalesce == OP_DELETE_FWD && b->op_count > 0) {
        BufferOp* last = &b->ops[b->op_count - 1];
        if (last->kind == OP_DELETE_FWD && last->pos == b->cursor) {
            last->text = realloc(last->text, last->n + n);
            memcpy(last->text + last->n, b->data + b->cursor, n);
            last->n += n;
            apply_delete(b, b->cursor, n);
            update_dirty(b);
            return;
        }
    }
    push_op(b, OP_DELETE_FWD, b->cursor, b->data + b->cursor, n);
    b->coalesce = OP_DELETE_FWD;
    apply_delete(b, b->cursor, n);
    update_dirty(b);
}

/* ---------- undo / redo ----------------------------------------------- */
void buffer_undo(Buffer* b)
{
    if (b->op_head == 0) return;
    b->op_head--;
    BufferOp* op = &b->ops[b->op_head];
    if (op->kind == OP_INSERT) {
        apply_delete(b, op->pos, op->n);
        b->cursor = op->pos;
    } else {
        apply_insert(b, op->pos, op->text, op->n);
        if (op->kind == OP_DELETE_BACK) b->cursor = op->pos + op->n;
        else                            b->cursor = op->pos;
    }
    b->sel_anchor = -1;
    b->coalesce   = OP_NONE;
    update_dirty(b);
}

void buffer_redo(Buffer* b)
{
    if (b->op_head >= b->op_count) return;
    BufferOp* op = &b->ops[b->op_head];
    if (op->kind == OP_INSERT) {
        apply_insert(b, op->pos, op->text, op->n);
        b->cursor = op->pos + op->n;
    } else {
        apply_delete(b, op->pos, op->n);
        b->cursor = op->pos;
    }
    b->op_head++;
    b->sel_anchor = -1;
    b->coalesce   = OP_NONE;
    update_dirty(b);
}

/* ---------- cursor motion (each one breaks coalescing) ---------------- */
static void start_or_clear_selection(Buffer* b, bool select)
{
    if (select) { if (b->sel_anchor < 0) b->sel_anchor = (long)b->cursor; }
    else        { b->sel_anchor = -1; }
}

void buffer_move_left(Buffer* b, bool select)
{
    start_or_clear_selection(b, select);
    if (b->cursor > 0) b->cursor = utf8_prev(b->data, b->cursor);
    b->pref_col = (int)(b->cursor - line_start_of(b->data, b->cursor));
    b->coalesce = OP_NONE;
}

void buffer_move_right(Buffer* b, bool select)
{
    start_or_clear_selection(b, select);
    if (b->cursor < b->len) b->cursor = utf8_next(b->data, b->len, b->cursor);
    b->pref_col = (int)(b->cursor - line_start_of(b->data, b->cursor));
    b->coalesce = OP_NONE;
}

void buffer_move_up(Buffer* b, bool select)
{
    start_or_clear_selection(b, select);
    size_t ls = line_start_of(b->data, b->cursor);
    if (ls == 0) { b->coalesce = OP_NONE; return; }
    size_t prev_le = ls - 1;
    size_t prev_ls = line_start_of(b->data, prev_le);
    size_t prev_len = prev_le - prev_ls;
    size_t target = (size_t)b->pref_col < prev_len
        ? (size_t)b->pref_col : prev_len;
    b->cursor = prev_ls + target;
    b->coalesce = OP_NONE;
}

void buffer_move_down(Buffer* b, bool select)
{
    start_or_clear_selection(b, select);
    size_t le = line_end_of(b->data, b->len, b->cursor);
    if (le >= b->len) { b->coalesce = OP_NONE; return; }
    size_t nxt_ls = le + 1;
    size_t nxt_le = line_end_of(b->data, b->len, nxt_ls);
    size_t nxt_len = nxt_le - nxt_ls;
    size_t target = (size_t)b->pref_col < nxt_len
        ? (size_t)b->pref_col : nxt_len;
    b->cursor = nxt_ls + target;
    b->coalesce = OP_NONE;
}

void buffer_move_line_start(Buffer* b, bool select)
{
    start_or_clear_selection(b, select);
    b->cursor = line_start_of(b->data, b->cursor);
    b->pref_col = 0;
    b->coalesce = OP_NONE;
}

void buffer_move_line_end(Buffer* b, bool select)
{
    start_or_clear_selection(b, select);
    b->cursor = line_end_of(b->data, b->len, b->cursor);
    b->pref_col = (int)(b->cursor - line_start_of(b->data, b->cursor));
    b->coalesce = OP_NONE;
}

void buffer_move_doc_start(Buffer* b, bool select)
{
    start_or_clear_selection(b, select);
    b->cursor = 0;
    b->pref_col = 0;
    b->coalesce = OP_NONE;
}

void buffer_move_doc_end(Buffer* b, bool select)
{
    start_or_clear_selection(b, select);
    b->cursor = b->len;
    b->pref_col = (int)(b->cursor - line_start_of(b->data, b->cursor));
    b->coalesce = OP_NONE;
}

void buffer_set_cursor(Buffer* b, size_t pos, bool select)
{
    if (pos > b->len) pos = b->len;
    while (pos > 0 && is_cont((unsigned char)b->data[pos])) pos--;
    start_or_clear_selection(b, select);
    b->cursor = pos;
    b->pref_col = (int)(b->cursor - line_start_of(b->data, b->cursor));
    b->coalesce = OP_NONE;
}

/* ---------- line queries (O(1) / O(log L) via line_starts index) ------- */
size_t buffer_line_count(const Buffer* b)
{
    return b->line_starts_count;
}

size_t buffer_line_start(const Buffer* b, size_t line)
{
    if (line >= b->line_starts_count) return b->len;
    return b->line_starts[line];
}

size_t buffer_line_end(const Buffer* b, size_t line)
{
    if (line >= b->line_starts_count) return b->len;
    if (line + 1 < b->line_starts_count) {
        /* Next line's start sits one past the '\n' that ends this line. */
        return b->line_starts[line + 1] - 1;
    }
    return b->len;
}

void buffer_cursor_pos(const Buffer* b, size_t* line, size_t* col)
{
    size_t after = lis_first_after(b, b->cursor);   /* never 0: line_starts[0]==0 */
    size_t ln    = after - 1;
    *line = ln;
    *col  = b->cursor - b->line_starts[ln];
}

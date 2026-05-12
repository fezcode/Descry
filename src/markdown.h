#ifndef DOWNSEE_MARKDOWN_H
#define DOWNSEE_MARKDOWN_H

#include <stddef.h>

/* v0.6: parse markdown -> per-line metadata + per-byte inline style flags.
 *
 * data[i]  — the i-th UTF-8 byte of the rendered text
 * style[i] — bitmask of inline styles active on byte i
 *
 * style[] is parallel to data[]; multi-byte UTF-8 sequences carry the same
 * style on every byte (we set them in the same cb_text call). */

typedef enum {
    LINE_NORMAL,
    LINE_H1, LINE_H2, LINE_H3, LINE_H4, LINE_H5, LINE_H6,
    LINE_CODE,
    LINE_QUOTE,
    LINE_LIST,
    LINE_LIST_TASK_OPEN,    /* `- [ ] ...` */
    LINE_LIST_TASK_DONE,    /* `- [x] ...` */
    LINE_BLANK,
    LINE_IMAGE,    /* data[start..start+len) holds the src/path string */
    /* Table rows. Cells inside data[start..start+len) are tab-separated.
     * align[i] is 'l' / 'c' / 'r' for columns 0..7. The renderer detects
     * consecutive TABLE_HEAD/ROW lines as a single table and computes
     * column widths from the max content width seen. */
    LINE_TABLE_HEAD,
    LINE_TABLE_ROW,
} LineKind;

typedef enum {
    STYLE_BOLD   = 1 << 0,
    STYLE_ITALIC = 1 << 1,
    STYLE_CODE   = 1 << 2,
    STYLE_STRIKE = 1 << 3,
    STYLE_LINK   = 1 << 4,
} StyleFlags;

typedef struct {
    LineKind kind;
    int      indent;
    size_t   start;
    size_t   len;
    /* For LINE_LIST_TASK_OPEN/DONE: byte offset in the SOURCE (the buffer
     * that was passed to md_doc_parse) of the char between the brackets,
     * i.e. the ' ' or 'x' that the click handler must toggle. 0 otherwise. */
    size_t   task_mark_off;
} MdLine;

/* `[[note-name]]` Obsidian-style wiki link found in the text. The full byte
 * range (including the brackets) gets STYLE_LINK applied; this struct lets
 * a click handler resolve a byte position back to the link's target name. */
typedef struct {
    size_t start;       /* byte offset of the first '['                  */
    size_t end;         /* byte offset just past the second ']'          */
    size_t name_start;  /* start of the inner name (after `[[`)          */
    size_t name_len;    /* length of inner name                          */
} MdWiki;

/* `[text](url)` inline link. `start`..`end` are the rendered-text byte
 * range that received STYLE_LINK; `href` is a heap-allocated copy of the
 * URL. A Ctrl+click hit-test resolves a clicked byte to this struct to
 * open the URL externally. */
typedef struct {
    size_t start;
    size_t end;
    char*  href;
} MdLink;

typedef struct {
    char*          data;
    unsigned char* style;        /* parallel to data, one byte per byte */
    size_t         len, cap;
    MdLine*        lines;
    size_t         line_count, line_cap;
    MdWiki*        wikis;
    size_t         wiki_count, wiki_cap;
    MdLink*        links;
    size_t         link_count, link_cap;
} MdDoc;

int  md_doc_parse(const char* src, size_t src_len, MdDoc* out);
void md_doc_free (MdDoc* d);

#endif

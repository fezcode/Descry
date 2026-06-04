#ifndef DESCRY_APP_H
#define DESCRY_APP_H

#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>

#include "buffer.h"
#include "image.h"
#include "markdown.h"
#include "tabs.h"
#include "vault.h"

typedef struct Font    Font;
typedef struct LuaHost LuaHost;

typedef struct {
    SDL_Window*   window;
    SDL_Renderer* renderer;
    int           win_w;
    int           win_h;

    /* IDE chrome font (regular only) — title bar, menus, sidebar, status
     * bar, settings overlay, every modal/picker/popup. Independent of the
     * preview body font so the user can pin chrome to a clean UI sans-serif
     * while reading prose in something more bookish. */
    Font* font_ide;

    /* Body font + 3 inline-style variants from the same TTF. Used by the
     * preview pane only — markdown body text, lists, blockquotes, tables. */
    Font* font_body;
    Font* font_body_bold;
    Font* font_body_italic;
    Font* font_body_bold_italic;

    /* Heading sizes (single weight each). */
    Font* font_h1;
    Font* font_h2;
    Font* font_h3;

    /* Monospace face for code blocks AND the editor pane, plus three
     * variants used to render inline `**bold**`, `*italic*`, `` `code` ``
     * inside body lines while keeping monospace cells. */
    Font* font_code;
    Font* font_code_bold;
    Font* font_code_italic;
    Font* font_code_bold_italic;

    LuaHost*    lua;
    ImageCache* imgcache;

    char*    note_path;
    Buffer   buf;          /* source-of-truth text */
    MdDoc    doc;          /* parsed view of buf, for preview rendering */
    bool     edit_mode;    /* false = preview, true = edit              */
    bool     viewing_image;/* current note is a raw image file (.png/.jpg/..)
                            * — buffer holds a synthetic `![](file)` so
                            * preview renders the image; edit/save blocked */

    int      scroll_y;
    int      scroll_x;          /* horizontal pan offset, only used when
                                 * edit mode wrap is off (cfg_edit_wrap) */
    int      doc_height_px;
    int      doc_width_px;      /* widest visible line in px, for hscroll */

    /* Tracks the wrap settings the per-line row cache (Buffer.line_rows) was
     * computed against. When either changes between frames, the cache is
     * bulk-invalidated so we don't reuse stale heights. */
    int      row_cache_wrap_w;
    bool     row_cache_wrap_on;

    /* Split live-preview: when on, the doc area shows the editor (left) and a
     * live rendered preview of the SAME file (right). render_pane is set
     * transiently by app_render so doc_x_left/right confine each pass to one
     * half without any change to the editor/preview render bodies. */
    bool     split_preview;
    int      preview_scroll_y;     /* right pane's own vertical scroll */
    int      preview_doc_height_px;/* right pane's rendered content height */
    unsigned long preview_parsed_seq;/* buf.seq the preview doc was parsed at */
    float    split_ratio;          /* editor's fraction of the doc width (~0.5) */
    int      render_pane;          /* PANE_FULL / PANE_LEFT / PANE_RIGHT */

    /* Open tabs. The ACTIVE tab's live state lives in the a->buf / note_path /
     * scroll / edit_mode / viewing_image fields; tabs.items[tabs.active] is a
     * stale husk until parked via tabs_park_active(). */
    TabList  tabs;

    /* Vault sidebar */
    Vault    vault;
    bool     sidebar_open;
    int      sidebar_w;
    int      sidebar_hover;
    int      sidebar_scroll_y;

    /* Editor cursor blink state */
    uint32_t blink_anchor;       /* SDL_GetTicks at last cursor activity */

    /* Mouse drag-select state in edit mode */
    bool     mouse_selecting;

    /* Sidebar resize handle (drag the divider to resize) */
    bool     resizing_sidebar;
    bool     resizing_split;         /* dragging the split-preview divider */
    SDL_Cursor* cursor_resize;
    bool     cursor_is_resize;
    /* Window-edge resize cursors (used while hovering the borderless
     * window's resize zones). cur_kind is the last-set cursor index so we
     * only call SDL_SetCursor when the cursor actually changes. */
    SDL_Cursor* cur_arrow;
    SDL_Cursor* cur_ns;
    SDL_Cursor* cur_we;
    SDL_Cursor* cur_nwse;
    SDL_Cursor* cur_nesw;
    int         cur_kind;     /* 0=arrow 1=ns 2=we 3=nwse 4=nesw */

    /* Preview-mode text selection + drag */
    long     preview_sel_start;  /* -1 = no selection                       */
    size_t   preview_sel_end;
    bool     preview_selecting;

    /* Per-row layout map populated by render_line during preview render.
     * Used to map mouse (x,y) back to a doc byte position. */
    struct PreviewRow {
        int    y;
        int    lh;
        int    x_start;
        Font*  font;
        size_t byte_start;
        size_t byte_end;     /* exclusive */
    }*       preview_rows;
    size_t   preview_row_count;
    size_t   preview_row_cap;

    /* Find / Replace overlay state */
    int      search_mode;        /* 0 = off, 1 = find, 2 = find+replace    */
    char     search_query[256];
    char     search_replace[256];
    size_t   search_qlen;
    size_t   search_rlen;
    /* Caret positions inside the query/replace fields so the user can
     * edit the middle of the string instead of only the tail. */
    size_t   search_qcursor;
    size_t   search_rcursor;
    bool     search_focus_replace;
    bool     search_case_insensitive;   /* Alt+I */
    bool     search_whole_word;         /* Alt+W */
    bool     search_regex;              /* Alt+R */
    size_t*  search_matches;
    size_t*  search_match_lens;         /* parallel to search_matches */
    size_t   search_count;
    size_t   search_cap;
    int      search_current;
    char     search_re_err[64];         /* compile error (empty = OK) */

    /* Quick switcher (Ctrl+P) — fuzzy/substring file picker */
    bool     switcher_active;
    char     switcher_query[128];
    size_t   switcher_qlen;
    int*     switcher_matches;   /* indices into vault.items                */
    int      switcher_count;
    int      switcher_cap;
    int      switcher_selected;

    /* Command palette (Ctrl+Shift+P) — fuzzy-search every built-in action
     * plus every plugin-registered Lua action. cmdp_entries is rebuilt on
     * each open so newly-loaded plugins surface automatically. */
    bool     cmdp_active;
    char     cmdp_query[128];
    size_t   cmdp_qlen;
    int*     cmdp_matches;       /* indices into cmdp_entries               */
    int      cmdp_count;         /* number of filtered matches              */
    int      cmdp_cap;
    int      cmdp_selected;     /* keyboard / authoritative selection      */
    int      cmdp_hover;        /* row under mouse, doesn't scroll list    */
    int      cmdp_scroll;       /* px scroll offset within the row band    */
    /* Each entry: label is what's displayed, name is the canonical action
     * id (sent to lua_host_invoke_action for plugins), fn is non-NULL for
     * built-in C actions, category is shown as a chip on the right. */
    struct CmdEntry {
        char label[80];
        char name[64];
        char category[24];
        char shortcut[24];
        void (*fn)(void*);        /* App* — typed as void* to keep app.h C-clean */
        bool is_plugin;
    }*       cmdp_entries;
    int      cmdp_entry_count;
    int      cmdp_entry_cap;

    /* Recent vault directories — most-recent first, capped at 5. Loaded
     * from settings.lua at boot, written back on every vault change.
     * Surfaced in the File menu and quickly switchable. */
    char     recent_dirs[5][512];
    int      recent_dirs_count;

    /* Plugins overlay — flat list, one row per loaded plugin, expandable
     * to show its registered actions. */
    bool     plugins_active;
    int      plugins_selected;
    int      plugins_scroll;
    /* Snapshot built from lua_host_each_plugin on open. */
    struct PluginRow {
        char name[64];
        char path[400];
        char status[40];     /* "loaded" / "error" / etc.       */
        char error[200];
        char actions[16][48];
        int  action_count;
    }*       plugins_rows;
    int      plugins_count;
    int      plugins_cap;

    /* Wiki-link auto-complete (triggered by typing `[[` in edit mode). The
     * filter text is the slice of the buffer between wc_anchor and the
     * cursor; popup is anchored at (wc_x, wc_y) under the trigger position. */
    bool     wc_active;
    size_t   wc_anchor;          /* buffer pos right after the `[[`         */
    int*     wc_matches;         /* indices into vault.items                */
    int      wc_count;
    int      wc_cap;
    int      wc_selected;
    int      wc_x, wc_y;         /* screen-space top-left of popup          */

    /* Click hits populated each preview render. `byte_start` semantics
     * depend on `kind`: HIT_WIKI = position inside a `[[...]]` for the
     * wiki-link resolver; HIT_TASK = byte position of the ' '/'x' inside
     * a `[ ]` / `[x]` task-list checkbox to toggle. */
    struct ClickHit {
        SDL_Rect rect;
        size_t   byte_start;
        int      kind;        /* HIT_WIKI / HIT_TASK */
    }* hits;
    size_t   hit_count;
    size_t   hit_cap;

    /* Tooltip shown when hovering a wiki/external link in preview. Text
     * resolves on each motion: where the link points or "no match" for
     * unresolved wiki targets. anchor_x/y are the cursor position; the
     * renderer places the pill just below-right of the cursor. */
    bool     tip_active;
    char     tip_text[256];
    int      tip_anchor_x;
    int      tip_anchor_y;
    bool     tip_broken;          /* true when wiki target doesn't resolve */

    /* Sidebar visible-item map: indices of items currently shown after
     * applying folder collapse. Built per render; queried on click. */
    int*     sidebar_visible;
    int      sidebar_visible_count;
    int      sidebar_visible_cap;

    /* Sidebar right-click context menu. */
    bool     ctx_menu_active;
    int      ctx_menu_x;
    int      ctx_menu_y;
    int      ctx_menu_target;        /* vault item index, or -1 for empty */
    int      ctx_menu_hover;         /* index of currently hovered row    */
    /* Preview-mode right-click context: byte offset in doc.data the user
     * clicked at, so the "Go to source" action can hunt that text down in
     * the source buffer and jump the cursor there. (size_t)-1 means none. */
    size_t   ctx_menu_preview_doc_off;

    /* Live font / size config (mutable by the settings page). */
    char     cfg_font_path[260];        /* preview body */
    char     cfg_font_path_ide[260];    /* chrome / sidebar / overlays */
    char     cfg_font_path_mono[260];   /* editor + inline code */
    int      cfg_font_size;
    int      cfg_font_size_h1;
    int      cfg_font_size_h2;
    int      cfg_font_size_h3;
    int      cfg_line_spacing;       /* extra px added between every line */

    /* Line ending policy applied at save time:
     *   0 = preserve (auto-detect majority in buffer at save time)
     *   1 = always LF
     *   2 = always CRLF                                            */
    int      cfg_line_endings;

    /* Close-window animation kind:
     *   0 = off (window vanishes instantly)
     *   1 = fade (~250ms opacity ramp)                            */
    int      cfg_close_anim;

    /* Edit-mode soft wrap. true = long lines wrap visually at the
     * viewport width (file content unchanged). false = lines extend
     * past the viewport and a horizontal scrollbar appears. Toggleable
     * via View > Toggle Word Wrap. */
    bool     cfg_edit_wrap;

    /* Custom in-app confirmation modal (replaces SDL_ShowMessageBox).
     * Synchronous: confirm_discard pumps SDL events itself until the
     * user hits a button. */
    bool     confirm_active;
    int      confirm_choice;         /* -1 pending, 0 = discard, 1 = cancel */
    int      confirm_hover;          /* 0 = discard btn, 1 = cancel btn */

    /* 3-row line-ending picker (Edit > Convert line endings…). Same
     * synchronous-pump pattern as confirm_action. choice: -2 pending,
     *   -1 = cancel, 0 = LF, 1 = CRLF. hover: 0/1/2 (LF / CRLF / Cancel). */
    bool     eol_pick_active;
    int      eol_pick_choice;
    int      eol_pick_hover;
    char     confirm_title[64];
    char     confirm_msg[400];     /* multi-line — split on '\n' at render */
    /* Optional custom button labels. Empty falls back to "Discard"/"Cancel".
     * Set by confirm_action() for "Open link?", "Quit?" style prompts. */
    char     confirm_btn0_label[32];
    char     confirm_btn1_label[32];

    /* In-app text-input modal (replaces native Save As / Rename dialog).
     * Synchronous like confirm_discard: app_text_modal pumps events until
     * the user hits OK or Cancel. tinput_text holds the live input. */
    bool     tinput_active;
    /* When true the modal acts as an in-app folder picker. The input
     * field becomes a path bar (Enter navigates), the OK button reads
     * "Use this folder", and the caller gets back tinput_dir. The
     * `tinput_path_err` flag toggles a red border + error label when
     * the typed path doesn't resolve. `tinput_err_text` holds the
     * specific path that failed so the error label can quote it. */
    bool     tinput_pick_dir;
    bool     tinput_path_err;
    char     tinput_err_text[260];
    /* Inline right-click context menu inside the modal. Holds the row
     * the menu was opened on plus its screen position. Click target
     * becomes the rename target. */
    bool     tinput_ctx_active;
    int      tinput_ctx_row;
    int      tinput_ctx_x, tinput_ctx_y;
    /* Rename popup: a small modal overlay on top of the picker with its
     * own input field and OK/Cancel buttons. Independent state so the
     * picker's path bar isn't touched while the popup is open. */
    bool     tinput_renpop_active;
    char     tinput_renpop_old[260];   /* original basename               */
    char     tinput_renpop_text[260];  /* live edit buffer                */
    int      tinput_renpop_len;
    int      tinput_renpop_cursor;
    int      tinput_renpop_sel_anchor; /* -1 = no selection               */
    int      tinput_renpop_choice;     /* -1 pending, 0 = OK, 1 = Cancel  */
    int      tinput_renpop_hover;      /* 0 = OK, 1 = Cancel, -1 = none   */
    bool     tinput_renpop_err;
    char     tinput_renpop_err_text[260];
    int      tinput_choice;          /* -1 pending, 0 = OK, 1 = Cancel  */
    int      tinput_hover;           /* 0 = OK, 1 = Cancel, -1 = none   */
    char     tinput_title[64];
    char     tinput_text[260];
    int      tinput_len;
    int      tinput_cursor;          /* caret position within tinput_text */
    int      tinput_sel_anchor;      /* -1 = no selection                */
    /* Dir-listing for the modal so Save/Rename/New show actual files
     * in the target directory. tinput_files entries are heap-allocated
     * basenames; freed when the modal closes. */
    char     tinput_dir[512];
    char**   tinput_files;
    bool*    tinput_files_isdir;     /* parallel to tinput_files */
    int      tinput_files_count;
    int      tinput_files_cap;
    int      tinput_files_hover;     /* row index, -1 = none */
    int      tinput_files_scroll;    /* px scroll offset */
    /* "Selected" row (single-click = select, double-click = navigate).
     * The selection drives both the visible highlight and the OK button:
     * "Use this folder" with a folder selected commits that folder,
     * otherwise it commits the current dir. -1 = no selection. */
    int      tinput_files_selected;

    /* Outline panel resize handle drag state. */
    bool     resizing_outline;

    /* Scrollbar drag state — captures which thumb is being dragged
     * and the y-offset within the thumb when the drag started. */
    enum { SB_NONE = 0, SB_DOC, SB_KEYBIND, SB_OUTLINE_PANEL,
           SB_VSEARCH, SB_OUTLINE_LIST, SB_BACKLINKS, SB_TAGS, SB_PICKER,
           SB_CMDP, SB_TINPUT, SB_DOC_H }
                                                              sb_drag;
    int      sb_drag_offset;

    /* Settings overlay state. */
    bool     settings_active;
    int      settings_selected;
    int      settings_hover;         /* row under cursor, -1 if none */
    int      settings_font_idx;      /* preview body — index into g_font_choices */
    int      settings_font_idx_ide;  /* IDE chrome    — index into g_font_choices */
    int      settings_font_idx_mono; /* editor / mono — index into g_font_choices */
    int      settings_theme_idx;     /* index into g_themes */

    /* Keybindings overlay (F1) state — also serves as the help surface;
     * the standalone Help overlay was merged into it. */
    bool     keybind_active;
    int      keybind_selected;       /* row in the action list           */
    int      keybind_hover;          /* mouse hover row                  */
    bool     keybind_capturing;      /* true = next key combo gets bound */
    int      keybind_scroll;         /* px scroll offset                 */

    /* Color picker overlay state. */
    bool     picker_active;
    int      picker_selected;        /* color slot index */
    int      picker_hover;           /* mouse hover slot, -1 if none */
    int      picker_channel;         /* 0=R, 1=G, 2=B, 3=A */
    int      picker_scroll;

    /* Chrome bar hover tracking — which top-bar button is the mouse over
     * right now? -1 if none. Indices match enum ChromeButton in main.c. */
    int      chrome_hover;

    /* Breadcrumb segment hit-rects, populated each frame by render_chrome
     * so the click/motion handlers don't re-derive them. -1 in crumb_hover
     * means the cursor isn't on any segment. */
    SDL_Rect crumb_rect_vault;
    SDL_Rect crumb_rect_title;
    int      crumb_hover;            /* -1 none, 0 vault, 1 title */
    float    crumb_hover_t[2];       /* eased hover state per segment */

    /* Tab strip hit-rects, rebuilt every frame by render_chrome (parallel to
     * tabs.items, capped at 64). close_rect is the close-button sub-rect.
     * tab_strip_x0/x1 are the strip's horizontal bounds so the mouse handlers
     * can gate clicks to the strip region. */
    struct TabHit { SDL_Rect rect; SDL_Rect close_rect; } tab_hits[64];
    int      tab_strip_count;        /* tabs laid out this frame */
    int      tab_hover;              /* hovered tab index, -1 none */
    int      tab_scroll_x;           /* horizontal scroll of the strip, px */
    int      tab_strip_x0, tab_strip_x1;

    /* Custom window-decoration state. Title bar window controls hover
     * tracking + animations. -1 in tb_btn_hover means no button hovered. */
    int      tb_btn_hover;           /* -1 none, 0 min, 1 max, 2 close */
    float    tb_btn_hover_t[3];      /* eased hover state per button */
    /* Top menu bar (File/Edit/View/Help). */
    int      menu_hover;             /* -1 none, 0..3 = menu index */
    float    menu_hover_t[4];        /* eased hover state per menu */
    int      menu_open;              /* -1 closed, 0..3 = which dropdown */
    SDL_Rect menu_rects[4];          /* hit rects per menu (set by render) */

    /* Per-button animation level [0..1] for the chrome bar; eased toward
     * 1 when the button is hovered, 0 otherwise. Drives bg fade and the
     * animated underline. Indexed by ChromeButton enum value (0..CB_COUNT-1). */
    float    chrome_hover_t[8];   /* one per CB_* enum value */
    float    chrome_press_t[8];

    /* Animation tick for time-stepped UI animations (chrome hover/press,
     * context menu fade-in, etc.). Updated once per frame. */
    uint32_t anim_last_ms;
    /* Set by render code when an animation is mid-progress so the main
     * loop wakes at ~60fps instead of the idle 50ms timeout. */
    bool     wants_anim_frame;
    /* Live resize indicator: set to (now + ~900ms) on every SIZE_CHANGED.
     * While SDL_GetTicks() < this, render_resize_badge draws a centered
     * "WxH" pill so the user has feedback as they drag the edge. */
    uint32_t resize_show_until;

    /* Context-menu open animation [0..1], drives fade-in + slight rise. */
    float    ctx_menu_open_t;
    /* Per-row hover animation level for context menu (max 16 rows). */
    float    ctx_menu_row_t[16];
    /* Which kind of context menu is open: 0 = sidebar item, 1 = editor. */
    int      ctx_menu_kind;

    /* Recent-vaults submenu — opens to the right of the File menu when
     * the user hovers/clicks the "Recent vaults" row. Always lists the
     * entries from app->recent_dirs; doesn't need its own kind enum. */
    bool     ctx_submenu_active;
    int      ctx_submenu_x;
    int      ctx_submenu_y;
    int      ctx_submenu_hover;        /* hovered submenu row, -1 if none  */
    float    ctx_submenu_open_t;
    float    ctx_submenu_row_t[16];

    /* Frontmatter (YAML-ish `---` block at top of buffer). Populated by
     * reparse_preview; consumed by render_preview (properties pill at top),
     * update_window_title, and the tag panel. */
    bool     fm_present;
    size_t   fm_body_start;        /* byte offset in buf.data of body */
    char     fm_title[160];
    char     fm_tags_csv[400];     /* space-separated tag names */

    /* Hit-test rects for the frontmatter tag chips, populated during
     * render_frontmatter_pill so the click handler can pivot a chip click
     * to a vault search for that tag. Capped at 16 chips per note. */
    struct FmChip {
        SDL_Rect rect;             /* in window coords (post-scroll) */
        char     tag[64];
    }        fm_chip_hits[16];
    int      fm_chip_count;

    /* MRU list of recently opened files (max 10). Persisted to
     * data/.recent. Updated each time load_note succeeds. The switcher
     * (Ctrl+P) shows these first when its query is empty. */
    char*    recent_paths[10];
    int      recent_count;

    /* Backlinks panel (Ctrl+Shift+B): files that wiki-link to this note. */
    bool     backlinks_active;
    int      backlinks_selected;
    int      backlinks_hover;
    int      backlinks_scroll;
    struct BacklinkHit {
        int    vault_idx;
        int    line_no;       /* 1-based */
        char   preview[160];
    }*       backlinks_hits;
    int      backlinks_count;
    int      backlinks_cap;

    /* Tag panel (Ctrl+Shift+G): every #tag across the vault, by count. */
    bool     tags_active;
    int      tags_selected;
    int      tags_hover;
    int      tags_scroll;
    struct TagEntry {
        char   name[64];      /* without leading '#' */
        int    count;
    }*       tags_entries;
    int      tags_count;
    int      tags_cap;

    /* Template picker (shown by New File when templates/ has *.md files). */
    bool     tpl_active;
    int      tpl_selected;
    int      tpl_hover;
    int      tpl_scroll;
    struct TemplateEntry {
        char   name[80];      /* basename sans .md */
        char   path[600];
    }*       tpl_entries;
    int      tpl_count;
    int      tpl_cap;

    /* Outline (TOC) overlay (Ctrl+Shift+O) and / or right-side pin panel
     * (Ctrl+Alt+O). When pinned, the panel takes a fixed right strip and
     * the document area shrinks. The overlay flavor still works on top of
     * the pinned panel. */
    bool     outline_active;
    bool     outline_pinned;
    int      outline_panel_w;     /* width of the pinned right panel */
    int      outline_selected;
    int      outline_hover;
    int      outline_scroll;
    struct OutlineEntry {
        int    line_no;       /* 0-based */
        int    level;         /* 1..6 */
        char   text[160];
    }*       outline_entries;
    int      outline_count;
    int      outline_cap;

    /* Vault-wide search overlay (Ctrl+Shift+F). */
    bool     vsearch_active;
    char     vsearch_query[256];
    size_t   vsearch_qlen;
    bool     vsearch_regex;
    bool     vsearch_ci;
    char     vsearch_re_err[64];
    /* `vault_search_hits` array: each row is either a file header
     * (vault_idx >= 0, line_no == 0) or a hit row (line_no > 0). */
    struct VSearchHit {
        int    vault_idx;            /* index into vault.items */
        int    line_no;              /* 1-based; 0 = file header row */
        int    match_col_in_line;    /* byte offset of match within preview */
        int    match_len;
        char   preview[160];         /* ASCII line slice for display */
    }* vsearch_hits;
    int      vsearch_count;
    int      vsearch_cap;
    int      vsearch_selected;       /* index into vsearch_hits */
    int      vsearch_hover;
    int      vsearch_scroll;
    int      vsearch_files_with_hits;
    int      vsearch_total_hits;

    /* Sidebar drag-and-drop. */
    bool     dnd_active;
    int      dnd_source_idx;         /* vault item being dragged           */
    int      dnd_drop_target;        /* vault index of folder under cursor */
    int      dnd_x, dnd_y;           /* current cursor coords for ghost    */
    int      dnd_press_x, dnd_press_y;   /* mouse-down coords; threshold   */
    bool     dnd_armed;              /* mouse pressed; not yet exceeded threshold */

    /* Transient status-bar notification from descry.notify(). */
    char*    notification_msg;
    uint32_t notification_until;     /* SDL_GetTicks deadline */

    SDL_Color bg;
    SDL_Color fg;
    SDL_Color fg_heading;
    SDL_Color fg_quote;
    SDL_Color fg_link;
    SDL_Color bg_code;
    SDL_Color fg_muted;
    SDL_Color bg_sidebar;
    SDL_Color bg_sidebar_hover;
    SDL_Color bg_sidebar_active;
    SDL_Color bg_status;
    SDL_Color fg_status;
    SDL_Color bg_selection;
    SDL_Color fg_cursor;

    bool running;
} App;

#endif

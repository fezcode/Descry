# Tabs & Split Live-Preview — Design

**Date:** 2026-06-04
**Project:** Descry (markdown editor, C + SDL2 + Lua)
**Status:** Approved direction, pending spec review

## Problem

Descry can only hold one open file at a time. The whole `App` struct (`src/app.h`)
models a single current document (`note_path`, `buf`, `doc`, `scroll_y`, …).
Opening a second file replaces the first, losing unsaved edits, cursor, and scroll.

Users want:
1. **Tabs** — keep several files open at once, switch freely, edits persist per file.
2. **Split live preview** — a *toggleable* right pane that renders a live preview of
   the file being edited on the left (Typora/Obsidian "edit beside preview").

These are two independent features that compose: tabs decide which document is
active; the split shows that one active document as source-left / preview-right.

## Decisions (locked during brainstorming)

- **Live buffers, not reload-on-switch.** Each open tab keeps its full in-memory
  state (text, cursor, undo, dirty, scroll). Switching away and back is lossless.
- **Right pane = live preview of the *same* file**, not an independent second
  editor group. There is never more than one *active* document. This is what makes
  the implementation tractable (see Architecture).

## Goals / Non-goals

**Goals**
- Multiple files open simultaneously with a visible tab strip.
- Lossless tab switching (unsaved edits, cursor, scroll, undo all preserved).
- Open/switch/close via mouse + keyboard; reopen the same tab set on next launch.
- A toggleable split that shows live preview of the active file beside its source.

**Non-goals (YAGNI for this work)**
- Independent documents per pane / arbitrary split grids.
- "Ephemeral" / preview tabs (single-click-to-peek like VS Code).
- More than two panes; vertical (top/bottom) splits.
- Synced scrolling between editor and preview (v1 ships independent scroll;
  may revisit later).
- Drag-to-reorder tabs (can be a later add; not in this spec).

## Architecture

### Why this stays cheap

Two facts from the current code make this low-risk:

1. **`Buffer` is a self-contained, movable value** (`src/buffer.h`). One struct owns
   its text, cursor, selection, dirty flag, line index, row cache, and undo log.
   Parking a buffer = shallow-move the struct into a slot and zero the original;
   no deep copy.
2. **`doc` / frontmatter / outline are *derived* from `buf`** by `reparse_preview`
   (`src/main.c:838`). A tab only needs to store *source* state; the derived view is
   rebuilt on switch.

Therefore the active document continues to live in the existing `a->buf`,
`a->note_path`, `a->scroll_y`, … fields exactly as today. The **~290 `a->buf` /
`a->doc` references across `main.c` do not change.** Rendering always sees one
active document.

For the split, both render paths already confine themselves to the doc area via
two helpers — `doc_x_left()` (`src/main.c:569`) and `doc_x_right()`
(`src/main.c:585`). Making those two functions *pane-aware* lets `render_editor`
(`:4767`) and `render_preview` (`:4202`) each draw into a half-width sub-rect with
**no change to their internals**.

### Data model (`App` additions, `src/app.h`)

```c
typedef struct {
    char*  path;             /* owned; NULL = unsaved scratch buffer        */
    Buffer buf;              /* parked text + cursor + undo + dirty         */
    int    scroll_y, scroll_x;
    int    preview_scroll_y; /* this tab's right-pane scroll (Phase 2)      */
    bool   edit_mode;
    bool   viewing_image;
} Tab;

/* The ACTIVE tab's state lives in the existing live a->buf / a->note_path /
 * a->scroll_y / a->edit_mode / a->viewing_image fields — NOT duplicated here.
 * tabs[active_tab] is a stale placeholder while that tab is active; it is
 * refreshed (parked) the instant we switch away. */
Tab*  tabs;
int   tab_count, tab_cap;
int   active_tab;            /* index into tabs; -1 only before first open   */

bool  split_preview;         /* global view toggle (like sidebar_open)       */
int   preview_scroll_y;      /* live right-pane scroll for the active tab    */
float split_ratio;           /* fraction of doc width given to editor; 0.5   */
int   focus_pane;            /* 0 = editor (left), 1 = preview (right)       */
bool  resizing_split;        /* dragging the split divider                   */
int   render_pane;           /* transient, set during app_render only:       */
                             /*   0 = FULL, 1 = LEFT, 2 = RIGHT — read by    */
                             /*   doc_x_left/right to confine a pane (Ph.2)  */
```

### Core operations

```
park_active():
    t = &tabs[active_tab]
    free(t->path); t->path = a->note_path own-transfer (or strdup)
    move a->buf -> t->buf            (memcpy + buffer_init(&a->buf) to detach)
    t->scroll_y/x, t->preview_scroll_y, t->edit_mode, t->viewing_image = live

activate(i):
    park_active()                    (if a tab was active)
    t = &tabs[i]
    a->note_path = t->path           (own-transfer back)
    move t->buf -> a->buf
    a->scroll_y/x, a->preview_scroll_y, a->edit_mode, a->viewing_image = t->*
    active_tab = i
    reparse_preview(a); update_window_title(a)
    a->vault.selected = vault_index_of(&a->vault, a->note_path)
```

`load_note` (`:960`) is refactored so the file-reading part is reusable: opening a
path becomes "find existing tab → activate it, else make a new tab and load into
the live buffer." `recent_push`/`recent_save` stay where they are.

## Phase 1 — Tabs

### Behaviors
- **Open a file** (sidebar click, `Ctrl+P` switcher, wiki-link, File ▸ Open):
  if a tab with that path exists, `activate` it; otherwise `park_active`, append a
  new tab, and load the file into the live buffer. (Reuse, never duplicate.)
- **Switch:** click a tab; `Ctrl+Tab` / `Ctrl+Shift+Tab` cycle next/prev;
  `Ctrl+1`..`Ctrl+9` jump to the Nth tab (`Ctrl+9` = last).
- **Close:** the `×` hit-rect, middle-click on a tab, or `Ctrl+W`.
  - If the tab is dirty, route through the existing `confirm_discard` modal
    (`app.h` `confirm_*`) before closing.
  - Closing the active tab activates the right neighbor, else the left.
  - Closing the last tab → welcome/empty state (existing "Welcome to Descry" path).
- **New scratch** (optional, low cost): File ▸ New already exists; it becomes a new
  tab instead of replacing.

### Tab strip (render + input)
- A horizontal strip occupying the top of the **doc area** (right of the sidebar),
  reusing the breadcrumb row's vertical band. Height ≈ one chrome row.
- Each tab: file icon (optional), elided filename (reuse `font_draw_elided` from the
  sidebar fix), a `●` when dirty, and a `×` hit-rect on hover/active.
- Active tab is visually distinct (underline/fill, matching existing chrome idioms).
- Overflow: the strip scrolls horizontally (wheel / drag); the active tab is kept
  in view. No hard cap on tab count.
- Hit-testing mirrors the existing chrome hover/click pattern (per-frame rects like
  `crumb_rect_*`, `menu_rects[]`).
- The breadcrumb's *filename* segment is now redundant; either drop it or keep just
  the `vault ▸ subdir` path context. (Defaulting to: keep path context, drop the
  trailing filename since the active tab shows it.)

### Persistence
- Extend the existing `.descry.state` sidecar (loaded at `src/main.c:3420`, written
  by `save_collapse_state` `:3454`) with new `@`-directives:
  - `@tab=<vault-relative-or-absolute path>` — one per open tab, in order.
  - `@active=<index>`.
- On launch, after the vault loads, reopen each `@tab` path (skipping any that no
  longer exist) and activate `@active` (clamped). If none restore, fall back to the
  current first-file / welcome behavior.
- Dirty unsaved state is **not** persisted across runs (out of scope; only the set
  of open file paths is).

## Phase 2 — Split live preview

### Toggle
- `View ▸ Toggle Preview Pane` menu item + keybinding (**proposed `Ctrl+\`**).
- `split_preview` is a global view setting, persisted in `settings.lua` alongside
  `cfg_edit_wrap` / `sidebar_open`.

### Rendering
- Make `doc_x_left()` / `doc_x_right()` consult `a->focus_pane`-independent render
  state: introduce `a->render_pane` (FULL / LEFT / RIGHT). When split is on,
  `app_render` (`:11239`) does:
  ```
  divider_x = doc_x_left + split_ratio * (doc_x_right - doc_x_left)
  render_pane = LEFT;  render_editor(a)          // confined to [left .. divider]
  render_pane = RIGHT; render_preview(a, true)   // confined to [divider .. right]
  render_pane = FULL;  draw 1px divider
  ```
  Because both functions read their extent only through the two helpers, their
  bodies are untouched.
- **Scroll separation:** editor uses `a->scroll_y`; preview uses
  `a->preview_scroll_y`. `render_preview` reads its scroll through a small accessor
  so the same code serves both the full-pane (uses `scroll_y`) and split-right
  (uses `preview_scroll_y`) cases. (Implementation detail for the plan: cleanest is
  a `current_scroll_y(App*)` accessor keyed on `render_pane`.)
- When split is **off**, behavior is exactly today's: `edit_mode` flips a single
  full-width editor/preview.

### Input routing
- Compute `divider_x` once per frame. On mouse events in the doc area, set
  `focus_pane` from `button.x` vs `divider_x`.
- Keyboard text editing always targets the editor (left). Wheel scroll, link
  clicks, and preview text selection act on whichever pane the cursor is over,
  mapped into that pane's sub-rect (the preview hit map `preview_rows` is already
  rect-relative; it just needs the right `xL`).
- Dragging the divider sets `split_ratio` (reuse the sidebar-divider drag pattern:
  `resizing_sidebar` → `resizing_split`, with a resize cursor over the divider).

### Interaction with `edit_mode`
- While `split_preview` is on, the view is always editor+preview; the plain
  edit/preview toggle is a no-op (or repurposed to move keyboard focus). When the
  split is turned off, the edit/preview toggle resumes normal behavior.

## Edge cases
- **Image tabs** (`viewing_image`): allowed as tabs; in split mode the right pane
  previews the synthetic image embed (same as full preview today). Editing stays
  blocked.
- **Rename / delete of an open file** (sidebar context menu): update or close the
  matching tab; if the active file is deleted, mark its buffer unsaved/scratch
  rather than crashing on save.
- **Save** (`save_note` `:11516`) operates on the active tab's `note_path` —
  unchanged.
- **Closing with multiple dirty tabs** (e.g. app quit): walk dirty tabs through the
  existing confirm flow; cancel aborts the quit.
- **Switcher / vault search "open"**: all funnel through the same open-path entry
  point so they reuse-or-create tabs consistently.

## Keybindings (to collision-check against the live keymap during planning)
Existing (from `app.h`): `Ctrl+B` sidebar, `Ctrl+P` switcher, `Ctrl+Shift+P`
palette, `Ctrl+Shift+B` backlinks, `Ctrl+Shift+G` tags, `Ctrl+Shift+O` /
`Ctrl+Alt+O` outline, `Ctrl+Shift+F` vault search, `F1` help.
Proposed new: `Ctrl+Tab` / `Ctrl+Shift+Tab`, `Ctrl+1..9`, `Ctrl+W`, `Ctrl+\`.
All must be verified free (and added to the keybindings/help overlay) in the plan.

## Testing strategy
The project has no automated test harness (CMake builds only the `descry` target +
vendored lua/md4c); features are verified by running the app. Plan accordingly:
- **Build gate:** `cmake --build build` clean (warnings-as-noise baseline only).
- **Manual checklist (Phase 1):** open 3 files; edit two without saving; switch
  among all three and confirm text/cursor/scroll/undo persist; dirty `●` correct;
  close a dirty tab → confirm modal; close active tab → neighbor activates; close
  last → welcome; quit + relaunch → same tabs reopen, missing file skipped.
- **Manual checklist (Phase 2):** toggle split; type on left → right updates live;
  scroll each pane independently; drag divider; click a wiki-link in the right pane;
  select text in the right pane; toggle split off → single pane resumes; switch tabs
  while split is on → preview follows active file.
- Where practical, factor `park`/`activate` and the `.descry.state` tab parsing as
  pure-ish functions so they *could* be exercised headlessly later.

## Open defaults (vetoable)
- No ephemeral tabs.
- Independent (non-synced) preview scroll in v1.
- `Ctrl+W` / `Ctrl+\` keybindings (pending collision check).
- Split state is global, not per-tab.
- Breadcrumb keeps `vault ▸ subdir` path context, drops the trailing filename.

## Build sequence
1. **Phase 1 (tabs)** — data model + park/activate, refactor `load_note` open-path,
   tab strip render + input, keybindings, `.descry.state` persistence. Ship & test.
2. **Phase 2 (split preview)** — pane-aware `doc_x_left/right` + `render_pane`,
   preview scroll separation, divider drag, input routing, toggle + persistence.

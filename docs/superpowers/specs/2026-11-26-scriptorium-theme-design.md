# Scriptorium theme — design

Date: 2026-11-26
Status: awaiting approval

## Intent

Add a second "material" theme to Descry, after Knit: **Scriptorium**, an
illuminated-manuscript look taken from a reference mockup the user supplied
(a parchment IDE with cross-stitched borders, red rubric headers, a pixel
uncial hand, Roman numerals, an illuminated drop cap, and small marginal
drawings: candle, chapel, snail).

The user chose the fullest of three scopes: palette + manuscript chrome +
drop cap + marginal ornaments.

Reference image: `2026-11-26-scriptorium-reference.png`, next to this file.
It is the source of truth for the look — letterforms, stitch density,
ornaments, the drop-cap tile — wherever this document is only prose.
Parchment ground, sepia ink, cinnabar red, ochre gold, lapis blue and
verdigris green accents. Every border is a row of small cross stitches. All
chrome lettering is a pixel hand in capitals and small capitals.

## Goals

- Selecting the theme "Scriptorium" in Settings (or `theme = "Scriptorium"`
  in settings.lua) turns the whole window into the manuscript look.
- It reads as the reference picture at a glance: parchment, stitched borders,
  rubric headers, pixel hand in the chrome, Roman numerals in the status bar,
  a drop cap, the ornaments.
- Note text stays as readable as in any other light theme.
- Every other theme, Knit included, looks and behaves exactly as before.

## Non-goals

- No editor line-number gutter. The picture has Roman line numbers, but
  Descry has no line numbers at all; adding a gutter is a separate feature.
- No refactor of Knit into a shared "material theme" interface. The two
  themes differ at nearly every draw call; a shared layer would be guesswork
  with two examples. Scriptorium branches sit beside the Knit branches.
- No syntax colouring changes, no new settings, no new keybindings.
- No changes to the macOS native menu.

## Anti-goals

- Note body text drawn in the pixel hand. It would be capitals only and
  tiring to read; the pixel hand is for chrome.
- Ornaments or the drop cap ever covering, clipping or shifting text in a
  way that breaks selection, click targets, or find highlights.
- Hit-testing geometry that differs between themes in ways that are not
  mirrored in the existing layout helpers (sidebar rows, tabs, menus must
  stay clickable where they are drawn).

## Constraints

- C, SDL2 renderer, no new dependencies. Same build (CMake + `build.ps1`).
- Must be correct at DPI scales 1.0, 1.5 and 2.0 (`a->dpi_scale`,
  `DESCRY_HIDPI_SCALE=2`): pixel art snaps to whole device pixels.
- Text the pixel hand cannot chart (CJK, emoji, Cyrillic...) falls back to
  the IDE font in the same ink colour, as Knit does with `klabel`.
- The theme is identified by name (`"Scriptorium"`), like Knit.

## Approaches

1. **Palette only** — one `g_themes` entry. Rejected by the user: it reads as
   a sepia light theme, not as the picture.
2. **Palette + manuscript chrome** — stitched borders, rubric headers, pixel
   hand, Roman numerals. Was my recommendation.
3. **Everything in the picture** — (2) plus drop cap and marginal ornaments.
   **Chosen by the user.**

How it is built (my call, the user handed over the how): a new module
`src/scriptorium.c` beside `src/knit.c`, with the same shape — an opaque
`Scriptorium*` on `App`, textures built lazily at device resolution and
cached, flushed when the scale changes — and `scriptorium_on(a)` branches in
`main.c` at the same sites Knit branches. The Knit commit (`af53094`) is the
recipe; its diff of `main.c` lists every hook site.

Alternatives dropped: drawing everything immediate-mode with
`SDL_RenderFillRect` (simplest, but thousands of tiny rects per frame for the
stitched borders and text); shipping PNG assets (the repo generates all its
art in code; assets would need packaging in `forge.toml`, the macOS bundle
and the AppImage).

## The look

### Palette

`g_themes` entry `"Scriptorium"`, placed after Knit. Starting values, to be
tuned by eye against the reference:

| slot | colour | role |
|---|---|---|
| bg | `#E8DCC6` | parchment page |
| fg | `#4A3A2A` | sepia ink |
| fg_heading | `#B22D3A` | cinnabar (rubric) |
| fg_quote | `#6B7A4A` | verdigris |
| fg_link | `#3F5A8C` | lapis |
| bg_code | `#DFD1B6` | darker vellum |
| fg_muted | `#8C7A62` | faded ink |
| bg_sidebar | `#E2D5BC` | |
| bg_sidebar_hover | `#D9CAAC` | |
| bg_sidebar_active | `#D2C09C` | |
| bg_status | `#E2D5BC` | |
| fg_status | `#6E5C46` | |
| bg_selection | `#B08A3C` at alpha 110 | gold wash |
| fg_cursor | `#B22D3A` | |

Module inks (the equivalent of Knit's yarns): INK, FADED, CINNABAR, GOLD
(`#B08A3C`), LAPIS, VERDIGRIS, ROSE (`#E9C9C0`, the pale band behind rubric
headers), VELLUM (`#DFD1B6`), PARCHMENT.

Unlike Knit there is no second palette: document, chrome and overlays all
use the one parchment palette, so no `theme_capture` / `theme_set` swapping.

### Cross-stitch borders

A stitch is a small "x" on a square cell (about 4 logical px, snapped to
whole device pixels). Borders are runs of stitches:

- **Window frame**: one row of gold stitches round the window (cinnabar
  while resizing), replacing the 1px border.
- **Panel seams**: one column of gold stitches between sidebar and page,
  page and outline panel, and between the two split panes; one row under the
  chrome and above the status bar.
- **Boxes**: a two-stitch-deep frame. Gold for neutral cards (overlays,
  menus, tooltips, command palette, settings); cinnabar for the confirm
  modal and the resize badge.

### The hand (pixel font)

New glyph charts in `src/scriptorium_fonts.h`, same string-row format as
`knit_fonts.h`. One face, two heights on a shared baseline:

- capitals: 5 wide x 7 tall
- small capitals: 4 wide x 5 tall — lower-case input maps here, so mixed
  case survives as caps + small caps, as in the picture

Uncial flavour on A, D, E, G, H, M, T, U (rounded forms); plain forms
elsewhere. ASCII printable set plus the same folded Latin accents and mapped
marks Knit handles (`fold_latin`). Drawn as crisp square pixels, not
stitches.

Sizes: SMALL (1 logical px per font pixel... scaled so cap height is about
the IDE font's cap height) for menus, tabs, sidebar rows, status bar; BIG
(1.5x to 2x) for titles. Exact pixel sizes are the builder's, picked so rows
keep their current heights.

**Rubricated initial**: a label can be drawn with its first letter in
cinnabar and the rest in ink (menu labels, sidebar folder names).

**Rubric header**: a section header as in the picture's EXPLORER / OUTLINE
blocks — a rose band edged with cinnabar stitches, the letters drawn as
hollow cinnabar outlines (rose showing through), in a wider outline cut of
the capitals. Contrast comes from the cinnabar outline, not from the fill.

### Roman numerals

Pure function, 1..3999 to Roman; 0 and anything above 3999 stay Arabic.
Used only where numbers are chrome: status bar (line, total lines, column,
word count), sidebar folder counts, outline count, resize badge.
Not applied to note content.

### Where each thing goes

- **Title bar**: parchment. Menu labels in the hand with rubricated
  initials; hovered/open menu gets a vellum swatch. Note title centred in
  the BIG hand, ink. A small stitched gold pediment ornament to the left of
  the window buttons when there is room (at least 160px free).
- **Tool row / tabs**: vault crumb in the hand, faded; `/` in gold. Tabs in
  the hand: active tab is a parchment swatch with a cinnabar underline,
  inactive faded; dirty marker is a gold dot; close is a cinnabar `x`.
  EDIT / PREVIEW button: a stitched box, cinnabar frame for EDIT, gold for
  PREVIEW, caption in the hand.
- **Sidebar**: vault name as a rubric header. Rows in the hand; folders with
  a rubricated initial and a cinnabar caret; selected row has a vellum
  swatch with a cinnabar bar at its left edge; hover is a lighter swatch.
  Folder counts in Roman numerals, faded. Same row geometry as today.
  **Candle** ornament pinned to the bottom of the sidebar when at least its
  height + 24px is free below the last row. The flame is drawn only while
  the active note has unsaved changes, with the caption "SAVE THY WORK
  BEFORE DARK" in cinnabar; saved, the candle is unlit and has no caption.
- **Outline panel** (when pinned): "OUTLINE" rubric header with the count in
  Roman numerals; rows keep the IDE font (headings are note content).
  **Chapel** ornament at the bottom when room, same rule as the candle.
- **Page (preview and edit)**: parchment, note text in the user's normal
  fonts with the palette above. A ruled margin: a thin cinnabar line inside
  a thin gold line down the left and right of the text column, like a ruled
  manuscript page. `---` rules draw as a row of gold stitches.
- **Drop cap** (preview only): see below.
- **Snail** ornament: a colophon after the last line of the note in
  preview, right-aligned in the text column, scrolling with the note and
  included in the document height. Not drawn in edit mode.
- **Status bar**: parchment with a stitched row above. Mode in cinnabar,
  the rest in the hand, numbers in Roman numerals.
- **Overlays and modals**: existing `overlay_card` gets the two-stitch gold
  frame (cinnabar for confirm). Their inner text stays in the IDE font.
- **Split divider**: a stitch column instead of the hairline.
- **Scrollbars, graph view, mermaid, tables, code blocks**: palette only.

### Drop cap

In preview, the first `LINE_NORMAL` paragraph of the note whose first
character is a letter the hand can chart gets an illuminated initial:

- a square tile two text rows tall: gold stitched frame, cinnabar ground
  with a small rose diaper pattern, the letter in parchment in the BIG hand
  scaled to fit
- the letter is not drawn again in the text; the first two visual rows of
  that paragraph start to the right of the tile; later rows return to the
  normal left edge
- the paragraph is at least two rows tall in layout even if its text is
  shorter
- selection, click-to-position and find highlights keep working: the two
  shifted rows record their own `x_start` and byte range in `preview_rows`
  (the first row's range starts after the initial). The initial itself is
  not highlightable but is still part of the text when a selection that
  spans it is copied
- skipped when the paragraph starts with a link, inline code, math, tag,
  bold/italic run, or a non-chartable character; skipped for headings,
  lists, quotes, tables
- only the first qualifying paragraph of the note, and only if it is the
  first non-blank, non-heading block (a note that opens with a list or code
  block gets no drop cap)

### Ornaments

Pixel-art charts in `scriptorium.c` (rows of characters mapped to inks, as
Knit's `HEN`), drawn in cinnabar and gold line work: candle (lit / unlit),
chapel, snail, pediment. Each about 40-70 logical px. They only ever sit in
free space and are never interactive.

## Code shape

- `src/scriptorium.h` / `src/scriptorium.c` — the material layer: inks,
  stitch runs and frames, the hand (measure / draw / elide / can-chart),
  rubric header, drop-cap tile, ornaments, Roman numerals, scale handling
  and texture caches. Knows nothing about `App`.
- `src/scriptorium_fonts.h` — glyph charts.
- `src/app.h` — `struct Scriptorium* scriptorium;`
- `src/main.c` — the palette entry, `scriptorium_on()`, an `slabel` helper
  (the `klabel` twin with IDE-font fallback), and branches at the Knit hook
  sites: `overlay_card`, `render_titlebar`, `render_chrome`,
  `sidebar_items_top`, `render_sidebar` (a `render_sidebar_scriptorium`),
  `render_outline_panel`, `render_window_border`, `render_resize_badge`,
  `render_status`, `app_render`; plus `render_line` / `render_preview` for
  the drop cap, the stitched `---` and the snail.
- `CMakeLists.txt` — add `src/scriptorium.c`.
- `tests/test_scriptorium.c` — Roman numerals (bounds, subtractive forms),
  text normalisation / can-chart, every printable ASCII character has a
  glyph in both heights, measure is consistent with elision.
- `README.md` — a Scriptorium section with two screenshots in
  `docs/screenshots/`, like Knit's.
- Version: 0.94.0 in `src/version.h`, `forge.toml`, `build_installer.ps1`.

## Verification

Visual, with the existing hands-off harness (sandboxed APPDATA,
`theme = "Scriptorium"` in settings.lua, PrintWindow screenshots): preview,
edit, split, sidebar open/closed, outline pinned, command palette, confirm
modal, menus open, at scale 1 and 2. Compare with the reference picture.
Regression shots of Knit and one plain theme before and after.

## Calls I made (not the user's words)

- Name "Scriptorium".
- No line-number gutter.
- Note text keeps its normal font; the pixel hand is chrome only.
- Candle flame = unsaved changes.
- Snail as an end-of-note colophon rather than a side margin drawing.
- Chapel in the outline panel, pediment in the title bar.
- Drop cap two rows tall, first paragraph only, preview only.
- Roman numerals in chrome only, capped at 3999.
- Version 0.94.0.

## Left to the builder

Exact pixel sizes and stitch cell size; the glyph and ornament charts
themselves; texture caching strategy; final colour tuning by eye; how the
scriptorium branches are arranged around the Knit ones.

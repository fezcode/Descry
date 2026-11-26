# Scriptorium Theme Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add "Scriptorium", an illuminated-manuscript material theme (parchment, cross-stitched borders, pixel hand, Roman numerals, drop cap, ornaments), selectable like any other theme.

**Architecture:** A pure-C core (`scriptorium_core.c`: Roman numerals, text normalisation, glyph charts) that unit-tests without SDL, and an SDL material layer (`scriptorium.c`) that rasterises stitches, text, the rubric header, the drop-cap tile and ornaments into cached textures. `main.c` gets `scriptorium_on(a)` branches beside the existing `knit_on(a)` ones.

**Tech Stack:** C11, SDL2 renderer, CMake/Ninja via `build.ps1`, assert-based ctest executables.

**Spec:** `docs/superpowers/specs/2026-11-26-scriptorium-theme-design.md` (reference picture: `docs/superpowers/specs/2026-11-26-scriptorium-reference.png`). The Knit commit `af53094` (`git show af53094 -- src/main.c`) lists every hook site this plan mirrors.

## Global Constraints

- No new dependencies; no image assets — all art is generated in code.
- Theme name is exactly `"Scriptorium"`; it is detected by name, like Knit.
- Every other theme, Knit included, must render exactly as before: every new branch is guarded by `scriptorium_on(a)`.
- Pixel art snaps to whole device pixels at `a->dpi_scale` 1.0, 1.5 and 2.0.
- Text the hand cannot chart falls back to `a->font_ide` in the same ink.
- Note body text is never drawn in the pixel hand.
- No line-number gutter. Roman numerals only in chrome, only for 1..3999.
- Shell is PowerShell; chain with `;`. Build: `.\build.ps1`; tests: `.\build.ps1 -Tests`. Kill a running `descry.exe` before building.
- Commits: only if the user has asked for them; never on `main` (branch `scriptorium-theme`); no `Co-Authored-By` trailer.
- Visual checks use the hands-off harness: sandboxed `APPDATA`/`LOCALAPPDATA`, `settings.lua` with `theme = "Scriptorium"`, `PrintWindow` screenshots, `DESCRY_HIDPI_SCALE` for scale.

## Review Focus

1. Non-ASCII names (Türkçe, Кириллица, 日本語, emoji) in vault, file, tab and title labels: drawn in the IDE-font fallback, measured with the same function that draws them so hit rects match. Pinned by `test_norm` (Task 1) and the unicode vault shot (Task 4).
2. Live theme switching in Settings between a plain theme, Knit and Scriptorium: no stale preview row heights (drop cap and snail change heights), no leftover margins. Pinned in Task 5 Step 7.
3. Drop-cap edge paragraphs: one-letter paragraph, one short word, a paragraph shorter than two rows, a find match and a selection that include the first word. Pinned in Task 5 Step 6.
4. Small windows and a narrow sidebar: ornaments disappear instead of overlapping rows; stitched frames never cover card content. Pinned in Task 4 Step 5.
5. Scale 1.5 and 2.0, and a scale change mid-session: no seams or blurred stitches; caches rebuilt. Pinned in Task 2 Step 6.

---

### Task 1: Pure core — Roman numerals, normalisation, glyph charts

**Files:**
- Create: `src/scriptorium_core.h`, `src/scriptorium_core.c`, `src/scriptorium_fonts.h`
- Create: `tests/test_scriptorium.c`
- Modify: `CMakeLists.txt` (test section: executable, the `foreach` list, `add_test(NAME scriptorium ...)`)

**Interfaces — Produces:**

```c
#define SCR_ROWS 7                      /* every glyph chart is 7 rows */
/* n in 1..3999 -> Roman; anything else -> decimal. Returns strlen(out). */
int  scr_roman(unsigned n, char* out, size_t cap);
/* Copy `in`, replacing each number (a digit run, with optional ",ddd"
 * thousands groups) by scr_roman of its value. */
void scr_romanize(const char* in, char* out, size_t cap);
/* UTF-8 -> glyph bytes (case kept, accents folded, whitespace collapsed,
 * same mark mapping as knit_norm). Returns length, -1 if unchartable. */
int  scr_norm(const char* s, size_t len, char* out, int cap);
/* Glyph chart rows ("0"/"1" strings) or NULL. Lower-case letters are the
 * 4-wide x 5-tall small capitals with two blank rows on top. */
const char* const* scr_glyph(unsigned char ch);
int  scr_glyph_cols(unsigned char ch);
int  scr_text_cols(const char* glyphs, int n);   /* 1-column letter gap */
```

- [ ] **Step 1: Write `tests/test_scriptorium.c`** (assert style of `tests/test_reveal.c`):

```c
static void test_roman(void) {
    char b[32];
    scr_roman(1, b, sizeof b);    assert(!strcmp(b, "I"));
    scr_roman(4, b, sizeof b);    assert(!strcmp(b, "IV"));
    scr_roman(18, b, sizeof b);   assert(!strcmp(b, "XVIII"));
    scr_roman(47, b, sizeof b);   assert(!strcmp(b, "XLVII"));
    scr_roman(1994, b, sizeof b); assert(!strcmp(b, "MCMXCIV"));
    scr_roman(2026, b, sizeof b); assert(!strcmp(b, "MMXXVI"));
    scr_roman(3999, b, sizeof b); assert(!strcmp(b, "MMMCMXCIX"));
    scr_roman(0, b, sizeof b);    assert(!strcmp(b, "0"));
    scr_roman(4000, b, sizeof b); assert(!strcmp(b, "4000"));
    assert(scr_roman(3888, b, 4) == 3 && strlen(b) == 3);   /* truncates, terminated */
}
static void test_romanize(void) {
    char b[96];
    scr_romanize("Ln 18 / 240  Col 47", b, sizeof b);
    assert(!strcmp(b, "Ln XVIII / CCXL  Col XLVII"));
    scr_romanize("1,234 words", b, sizeof b);  assert(!strcmp(b, "MCCXXXIV words"));
    scr_romanize("12,345 words", b, sizeof b); assert(!strcmp(b, "12345 words"));
    scr_romanize("0 words", b, sizeof b);      assert(!strcmp(b, "0 words"));
    scr_romanize("1920 x 1080", b, sizeof b);  assert(!strcmp(b, "MCMXX x MLXXX"));
    scr_romanize("v2, then 3", b, sizeof b);   assert(!strcmp(b, "vII, then III"));
}
static void test_norm(void) {
    char g[64];
    assert(scr_norm("Notes", 5, g, sizeof g) == 5 && !strcmp(g, "Notes"));
    assert(scr_norm("  a \t b ", 8, g, sizeof g) == 3 && !strcmp(g, "a b"));
    assert(scr_norm("\xc3\x87ay", 4, g, sizeof g) == 3 && !strcmp(g, "Cay"));      /* Çay */
    assert(scr_norm("\xc3\xbc", 2, g, sizeof g) == 1 && !strcmp(g, "u"));          /* ü */
    assert(scr_norm("\xe6\x97\xa5", 3, g, sizeof g) == -1);                        /* 日 */
    assert(scr_norm("\xd0\x9a", 2, g, sizeof g) == -1);                            /* К */
    assert(scr_norm("\xf0\x9f\x93\x9d", 4, g, sizeof g) == -1);                    /* 📝 */
    assert(scr_norm("", 0, g, sizeof g) == 0);
}
static void test_glyphs(void) {
    for (int c = 32; c < 127; ++c) {
        const char* const* rows = scr_glyph((unsigned char)c);
        assert(rows);
        size_t w = strlen(rows[0]);
        assert(w >= 1 && w <= 7);
        for (int r = 0; r < SCR_ROWS; ++r) assert(strlen(rows[r]) == w);
    }
    for (int c = 'a'; c <= 'z'; ++c) {                 /* small caps: 5 tall */
        const char* const* rows = scr_glyph((unsigned char)c);
        assert(!strchr(rows[0], '1') && !strchr(rows[1], '1'));
    }
    assert(scr_text_cols("AB", 2) == scr_glyph_cols('A') + 1 + scr_glyph_cols('B'));
    assert(scr_text_cols("", 0) == 0);
}
```

- [ ] **Step 2: Register the test in `CMakeLists.txt`** — `add_executable(test_scriptorium tests/test_scriptorium.c src/scriptorium_core.c)`, add to the `foreach` list, `add_test(NAME scriptorium COMMAND test_scriptorium)`. Run `.\build.ps1 -Tests`; expected: build fails (no source yet).
- [ ] **Step 3: Write `src/scriptorium_fonts.h`** — `static const char* const SCR_GLYPHS[128][SCR_ROWS]`, same format as `knit_fonts.h`. Capitals 5x7 (M, W may be 5), uncial forms for A D E G H M T U as in the reference picture; lower case 4x5 small capitals under two blank rows; digits, punctuation; control bytes 1..5 for the same marks Knit maps (middle dot, arrows, heart).
- [ ] **Step 4: Implement `scriptorium_core.c`.** `scr_norm` follows `knit_norm`/`fold_latin` in `src/knit.c:641-733` but keeps case and folds accented lower-case to lower-case.
- [ ] **Step 5: Run `.\build.ps1 -Tests`** — expected: `scriptorium ... Passed`, all other tests still pass.

### Task 2: Material layer, palette, lifecycle, frames

**Files:**
- Create: `src/scriptorium.h`, `src/scriptorium.c`
- Modify: `src/app.h` (add `struct Scriptorium* scriptorium;` after `knit`), `CMakeLists.txt` (add both `.c` files to `descry`), `src/main.c`

**Interfaces — Consumes:** Task 1 core. **Produces:**

```c
typedef enum { SCR_INK, SCR_FADED, SCR_CINNABAR, SCR_GOLD, SCR_LAPIS,
               SCR_VERDIGRIS, SCR_ROSE, SCR_VELLUM, SCR_PARCHMENT,
               SCR_INK_COUNT } ScrInk;
typedef enum { SCR_FONT_SMALL, SCR_FONT_BIG, SCR_FONT_COUNT } ScrFont;
typedef enum { SCR_ORN_CANDLE, SCR_ORN_CANDLE_LIT, SCR_ORN_CHAPEL,
               SCR_ORN_SNAIL, SCR_ORN_PEDIMENT, SCR_ORN_COUNT } ScrOrnament;
typedef struct Scriptorium Scriptorium;

Scriptorium* scr_create(SDL_Renderer* r);
void      scr_destroy(Scriptorium* s);
void      scr_set_scale(Scriptorium* s, float scale);   /* flushes caches */
SDL_Color scr_ink(ScrInk i);
int       scr_cell(const Scriptorium* s);               /* stitch cell, logical px */
void      scr_stitch_row(Scriptorium* s, int x, int y, int w, ScrInk i);
void      scr_stitch_col(Scriptorium* s, int x, int y, int h, ScrInk i);
/* A stitched frame `depth` stitches deep, just inside r. */
void      scr_frame(Scriptorium* s, SDL_Rect r, int depth, ScrInk i);
bool      scr_text_ok(const char* str, size_t len);
int       scr_text_w(const Scriptorium* s, const char* str, size_t len, ScrFont f);
int       scr_text_h(const Scriptorium* s, ScrFont f);
/* Top-left at (x,y); first letter in `initial` (pass `ink` for none);
 * elided with "..." to max_w (<=0: no limit). Returns width, -1 if the
 * text can't be charted. */
int       scr_text(Scriptorium* s, const char* str, size_t len, ScrFont f,
                   int x, int y, ScrInk ink, ScrInk initial, int max_w);
int       scr_rubric_h(const Scriptorium* s);
/* Rose band across r.x..r.x+r.w at r.y, cinnabar stitches above and below,
 * text as hollow cinnabar outline capitals. False if text can't be charted. */
bool      scr_rubric(Scriptorium* s, SDL_Rect r, const char* text);
void      scr_dropcap(Scriptorium* s, SDL_Rect tile, char letter);
int       scr_orn_w(const Scriptorium* s, ScrOrnament o);
int       scr_orn_h(const Scriptorium* s, ScrOrnament o);
void      scr_ornament(Scriptorium* s, ScrOrnament o, int x, int y);
```

Inks: INK `#4A3A2A`, FADED `#8C7A62`, CINNABAR `#B22D3A`, GOLD `#B08A3C`, LAPIS `#3F5A8C`, VERDIGRIS `#6B7A4A`, ROSE `#E9C9C0`, VELLUM `#DFD1B6`, PARCHMENT `#E8DCC6`.

- [ ] **Step 1: Write `scriptorium.h` / `scriptorium.c`.** Structure after `knit.c`: canvas → texture upload, lazy per-ink caches, LRU text cache, clip push/pop, everything flushed in `scr_set_scale`. A stitch is an "x" in a square cell of `round(4 * scale)` device px; one font pixel is `round(1.5 * scale)` device px for SMALL and `round(2.5 * scale)` for BIG (tune so SMALL cap height ≈ IDE cap height). Rubric letters: the capital charts scaled 2x, outline only. Ornament charts as rows of characters mapped to inks (as `HEN` in `knit.c:475`), drawn from the reference picture; `SCR_ORN_CANDLE_LIT` is the candle plus flame, same footprint.
- [ ] **Step 2: Palette + switch in `main.c`.** Add the `"Scriptorium"` `g_themes` entry after Knit with the spec's palette table; `static bool scriptorium_on(const App* a)` next to `knit_on` (forward-declared where `knit_on` is); `scr_create` / `scr_destroy` beside `knit_create` / `knit_destroy`; `scr_set_scale(a->scriptorium, a->dpi_scale)` in `app_render` when on.
- [ ] **Step 3: `slabel` helpers** beside `klabel`:

```c
static int slabel_w(const App* a, const char* s, ScrFont f);
/* vertically centred on cy; IDE-font fallback in the same ink */
static int slabel(App* a, const char* s, ScrFont f, int x, int cy,
                  ScrInk ink, ScrInk initial, int max_w);
```

- [ ] **Step 4: Frames.** `render_window_border`: `scr_frame(win rect, 1, resizing ? SCR_CINNABAR : SCR_GOLD)`. `overlay_card`: after the existing fill, `scr_frame(box, 2, SCR_GOLD)` when `box.w > 60 && box.h > 40`; `render_confirm_modal` passes cinnabar (add a file-static `g_card_ink` set around its `overlay_card` call, or frame again after it). `app_render`: stitched seams — a column at the sidebar's right edge, the outline panel's left edge and the split divider (replacing the hairline), a row under the chrome and above the status bar.
- [ ] **Step 5: Build and screenshot** with `theme = "Scriptorium"`: parchment palette, gold stitched window frame and seams, command palette (Ctrl+P or the bound key) in a gold two-stitch frame. Expected: no stitch covers card text; other chrome still in the IDE font.
- [ ] **Step 6: Scale check** — shots at `DESCRY_HIDPI_SCALE=1.5` and `2`: stitches crisp, evenly spaced, no gaps at corners. Shot of `theme = "Knit"` and `theme = "Light"`: identical to before the change.

### Task 3: Chrome — title bar, tabs, mode button, status bar, resize badge

**Files:** Modify `src/main.c` (`render_titlebar`, `render_chrome`, `render_status`, `render_resize_badge`)

**Interfaces — Consumes:** `slabel`, `slabel_w`, `scr_*`, `scr_romanize`.

- [ ] **Step 1: Title bar.** Menu labels via `slabel(..., SCR_INK, SCR_CINNABAR, 0)`, widths via `slabel_w` (so `a->menu_rects` match); hovered/open menu gets a VELLUM fill. Title in `SCR_FONT_BIG`, INK, centred with the existing min/max clamps. Pediment ornament right-aligned before `titlebar_content_right(a)` when at least 160px lie between the title's right edge and that limit.
- [ ] **Step 2: Tool row.** Vault crumb FADED (INK on hover), `/` in GOLD. Tabs: `slabel` names, cap 200px as Knit; active = PARCHMENT fill + 2px CINNABAR underline, inactive FADED; dirty = a GOLD middle dot; close = CINNABAR `x`. The strip's edge fade uses `a->bg_status`. Mode button: VELLUM fill, `scr_frame(…, 1, active ? SCR_CINNABAR : SCR_GOLD)`, caption `EDIT` / `PREVIEW` centred.
- [ ] **Step 3: Status bar.** Mode in CINNABAR; the detail string, position, EOL and word count passed through `scr_romanize` before measuring and drawing with `slabel` (FADED for the right cluster). Notification text in CINNABAR.
- [ ] **Step 4: Resize badge.** VELLUM box, `scr_frame(…, 2, SCR_CINNABAR)`, label through `scr_romanize`, `SCR_FONT_BIG`, INK.
- [ ] **Step 5: Build and screenshot.** Expected: `File Edit …` with red initials; status bar reads like `PREVIEW … Ln XVIII / CCXL Col XLVII LF MCCXXXIV words`. Click check: open the second menu and switch tabs with posted keys (Ctrl+1 / Ctrl+9) — drawn positions and behaviour agree.

### Task 4: Sidebar, outline panel, candle and chapel

**Files:** Modify `src/main.c` (`sidebar_items_top`, new `render_sidebar_scriptorium`, `render_sidebar`, `render_outline_panel`)

- [ ] **Step 1: `sidebar_items_top`** when on: `chrome_bar_h(a) + 8 + scr_rubric_h(s) + 8`.
- [ ] **Step 2: `render_sidebar_scriptorium(App* a)`** — same geometry and loop as `render_sidebar_knit` (`main.c`, search the name). Header: `scr_rubric` with the vault name (IDE font in CINNABAR if it can't be charted). Rows: selected = `bg_sidebar_active` fill + 3px CINNABAR bar at x=0; hover = `bg_sidebar_hover`; folders INK with CINNABAR initial and a CINNABAR caret (the existing `draw_icon_caret`), files INK, no file icon; folder count through `scr_roman`, FADED; dirty marker a GOLD dot. Tree guides in FADED at alpha 70.
- [ ] **Step 3: Candle.** After the rows: if `sidebar_w >= 150` and the space between the last row's bottom and the status bar is at least `scr_orn_h + 24`, draw `a->buf.dirty ? SCR_ORN_CANDLE_LIT : SCR_ORN_CANDLE` at the bottom-left (12px inset); when lit and `sidebar_w` leaves room, the caption `SAVE THY WORK` / `BEFORE DARK` in two CINNABAR lines to its right.
- [ ] **Step 4: Outline panel.** Header replaced by `scr_rubric` reading `OUTLINE` with the count in Roman numerals drawn FADED at the band's right; rows unchanged (IDE font) and `rows_top` unchanged; chapel at the bottom-right under the same free-space rule.
- [ ] **Step 5: Build and screenshot** — a vault with folders; a unicode vault (folder `Türkçe`, files `日本語.md`, `Кириллица.md`); a 600x400 window; sidebar dragged to its minimum width. Expected: fallback names legible and elided inside the sidebar; ornaments absent when space is short; candle flame appears after typing a character in edit mode and disappears after Ctrl+S.

### Task 5: The page — ruled margins, stitched rules, snail, drop cap

**Files:** Modify `src/main.c` (`render_line`, `render_preview`, `app_render`, `theme_apply`)

- [ ] **Step 1: Ruled margins.** In `app_render` after each pane's document pass: at `doc_x_left(a) + MARGIN_X - 14` and `doc_x_right(a) - MARGIN_X + 12`, a 1px GOLD line with a 1px CINNABAR line 3px inside it, from the chrome to the status bar, alpha 150.
- [ ] **Step 2: `LINE_HR`** when on: `scr_stitch_row` in GOLD across the text column instead of the rule.
- [ ] **Step 3: Snail.** In `render_preview`, after the line loop, when on and `a->doc.line_count > 0`: add `scr_orn_h + 28` to `y`; when `draw`, draw the snail right-aligned to `doc_x_right(a) - MARGIN_X`, 16px below the last line.
- [ ] **Step 4: Drop cap.** In `render_preview`, find the drop-cap line index once per pass: the first line that is not `LINE_BLANK` and not `LINE_H1..H6`; it qualifies only if it is `LINE_NORMAL`, `indent == 0`, its first byte is an ASCII letter and the style bytes of its whole first word are 0. Pass it to `render_line` through a new `App` field `int scr_dropcap_line` (-1 = none). In `render_line` for that line: tile side `2 * lh`, drawn by `scr_dropcap` at `(x_start, y)` with the upper-cased letter; the word loop starts at byte 1; rows whose top is above `y0 + 2 * lh` use `x_start + tile + 8` as their left edge (recorded as that `PreviewRow`'s `x_start`), later rows use the normal `x_start`; the line's height is at least `2 * lh + 4`.
- [ ] **Step 5: Invalidate heights on theme change.** In `theme_apply`, set every `a->doc.lines[i].cached_h = -1` (and the other tabs' docs if they cache separately — follow how `row_cache_wrap_w` invalidation does it).
- [ ] **Step 6: Drop-cap edge checks** — a test note per case, screenshot each in preview: paragraph `A`; paragraph `Hi there`; a long paragraph (wraps under the tile from row 3); a note opening with `# Title` then a paragraph (cap on the paragraph); a note opening with a list (no cap); a paragraph opening with `**Bold**` (no cap); a paragraph opening with `Çay` (no cap). With a long paragraph: Ctrl+F for its first word highlights the remainder of the word in the right place; select-all + copy yields the full text including the initial.
- [ ] **Step 7: Live switching** — in Settings cycle Light → Knit → Scriptorium → Light with a note open in preview: no overlapping text, no leftover gap where the drop cap or snail was, scroll range correct at each stop.

### Task 6: README, screenshots, version, regression

**Files:** Modify `README.md`, `src/version.h`, `forge.toml` (5 spots), `build_installer.ps1`; Create `docs/screenshots/scriptorium-preview.png`, `docs/screenshots/scriptorium-split.png`

- [ ] **Step 1:** Version `0.93.0` → `0.94.0` in all three files.
- [ ] **Step 2:** README: a Scriptorium section after Knit's, same shape, with the two screenshots (taken with the harness at scale 1, sidebar and outline open, a note with a drop cap).
- [ ] **Step 3:** `.\build.ps1 -Tests` — all tests pass; then `.\build.ps1` — clean build, no new warnings from the new files.
- [ ] **Step 4:** Final side-by-side of the preview screenshot and the reference picture; tune palette and sizes by eye. Knit and Light regression shots match the ones from Task 2 Step 6.

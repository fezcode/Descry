# Descry Feature Test

A manual visual regression test. Scroll through this file and confirm each
section renders as the description claims. Mismatches are bugs (or known
gaps — I've labeled those `(NOT YET)`).

## 1. Heading sizes

You should see six headings below, decreasing in size from H1 down to H3,
then H4–H6 the same size as H3 (in v0.6, all sub-H3 levels share one size).

# H1 — biggest, the most prominent
## H2 — clearly smaller than H1
### H3 — noticeably smaller again
#### H4 — same size as H3 in v0.6
##### H5 — same as H4
###### H6 — same as H5

## 2. Inline styles (the v0.6 feature)

Plain baseline text — no styling, used for visual comparison.

**Bold text should render in a heavier weight.**

*Italic text should slant to the right by ~12 degrees.*

***Bold-italic should be both heavy and slanted.***

`Inline code should be monospace and muted in color.`

A [link to nowhere](https://example.com) should appear in the accent color
(blue-ish, distinct from body text).

Mid-word styling: pre**BOLD**post — "pre" and "post" stay plain, only the
middle 4 characters get bold weight. Same with pre*ITAL*post and pre`CODE`post.

A line combining **bold** with *italic* with `code` and a [link](#) — all
four styles should be visually distinct in the same line.

## 3. Lists

### Unordered

- First item, plain text
- Second item with **bold** in the middle
- Third item with `inline code`
- A nested level:
  - Sub-item one
  - Sub-item two
    - Even deeper sub-item

Each level should indent further than its parent. The bullet "•" should
appear at the start of every item.

### Ordered

1. First numbered item
2. Second numbered item
3. Third item with *italic emphasis*

(In v0.6 ordered lists also render with "•" — proper "1." numbering is a
later pass.)

## 4. Code blocks

A fenced block — should sit on a darker background, monospace, full width:

```
function example(x) {
    return x * 2;
}
```

A wider one to confirm the background extends across:

```
The quick brown fox jumps over the lazy dog. 0123456789 !@#$%^&*()
A second line that's also wide enough to test the background tint.
```

## 5. Block quotes

> A single-line quote with **bold** inside. There should be a vertical
> accent bar (3 px) on the left edge of every visual row.

> A longer quote that gets joined into one paragraph by md4c — multiple `>`
> lines collapse with soft breaks (rendered as spaces). When this wraps to
> several visual rows, the accent bar should run down the left of each row.

## 6. Word wrap

This paragraph is intentionally very long so the greedy whitespace word-wrap
algorithm gets exercised at the right edge of the window. Resize the window
narrower to trigger wrap at different points and confirm no text is clipped
or lost. Inline **bold** and *italic* runs should not break the wrap — the
word with the style stays atomic, never split across lines.

A reallylongunbrokenwordthatdoesnotcontainwhitespaceandshouldoverflowtheline rather than disappear — verify it overhangs the right margin instead of vanishing.

## 7. Unicode coverage (single-font limitation)

Descry currently uses one font for the whole document. Glyphs the font
doesn't have render as the FreeType "tofu box" (a hollow rectangle).
**This is the expected behavior in v0.8 — font fallback is a planned pass.**

ASCII baseline: The quick brown fox jumps over the lazy dog.

Accented Latin (Consolas has these): é è à ñ ü ç ø

Glyphs Consolas DOES have: → ← ↑ ↓ ° µ ± ÷ × ¼ ½ ¾

Glyphs Consolas does NOT have (expected to tofu in v0.8):
★ ✓ ✗ ⌘ ⏎ ☐ ☑ ⚡ — these would render with `font_path =
"C:/Windows/Fonts/seguisym.ttf"` but then prose would look wrong.

Emoji (no monochrome font has color emoji): 🦊 🟢 🚧

CJK (Consolas has none): 你好 こんにちは 안녕하세요

Workaround until font fallback lands: in `init.lua`, set `font_path` to a
broader-coverage TTF like Noto Sans or DejaVu Sans Mono.

## 8. Known gaps (NOT YET rendered visually)

These are parsed correctly by md4c, the style flags are set, but the
visual treatment is not yet wired up. Don't report as bugs:

- **Strikethrough**: ~~this should look struck through but doesn't yet~~
- **Task list checkboxes**:
  - [ ] No checkbox glyph yet — the literal `[ ]` text will appear
  - [x] Same here — `[x]` will appear as text
- **Horizontal rule** below this line should be a horizontal line — it isn't:

---

- **Tables** below should render as a grid — they'll appear as plain text:

| Column A | Column B |
| -------- | -------- |
| cell 1   | cell 2   |
| cell 3   | cell 4   |

- **Clickable links**: the link in section 2 has the right color but
  hovering / clicking does nothing yet.
- **Image embeds**: `![alt](path.png)` — not yet decoded or drawn.

## 9. Scroll padding

The next section just exists to make the document tall enough to test
scrolling (mouse wheel, ↑↓, j/k, PgUp/PgDn, Space, Home, End).

Line 01 of scroll padding.
Line 02 of scroll padding.
Line 03 of scroll padding.
Line 04 of scroll padding.
Line 05 of scroll padding.
Line 06 of scroll padding.
Line 07 of scroll padding.
Line 08 of scroll padding.
Line 09 of scroll padding.
Line 10 of scroll padding.
Line 11 of scroll padding.
Line 12 of scroll padding.
Line 13 of scroll padding.
Line 14 of scroll padding.
Line 15 of scroll padding.
Line 16 of scroll padding.
Line 17 of scroll padding.
Line 18 of scroll padding.
Line 19 of scroll padding.
Line 20 — the end. Press **Home** to jump back to the top, or **Ctrl+Q** to quit.

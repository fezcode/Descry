#ifndef DESCRY_INPUTFIELD_H
#define DESCRY_INPUTFIELD_H

/* Horizontal scroll for a single-line text input.
 *
 * The text is drawn at `x - scroll` and clipped to a window `view_w` wide,
 * so `scroll` is how far the text is shifted left, in pixels. Positions are
 * measured from the start of the string ("content space"): `caret_x` is the
 * pixel width of everything before the caret, `text_w` the width of the
 * whole string.
 *
 * The caret sits one past the last glyph, so the scrollable content is
 * `text_w + caret_w` wide — clamping against `text_w` alone would leave the
 * caret hanging outside the field at the end of a long line. */

/* Clamp `scroll` so the field never shows empty space past either end. */
int input_scroll_clamp(int scroll, int content_w, int view_w);

/* Smallest move of `scroll` that brings the caret at
 * [caret_x, caret_x + caret_w) fully into view. The result is clamped. */
int input_scroll_follow(int scroll, int caret_x, int caret_w,
                        int text_w, int view_w);

#endif

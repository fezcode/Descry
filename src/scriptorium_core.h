#ifndef DESCRY_SCRIPTORIUM_CORE_H
#define DESCRY_SCRIPTORIUM_CORE_H

#include <stddef.h>

/* The Scriptorium theme's pure half: Roman numerals, the text normaliser
 * and the glyph charts of its pixel hand. No SDL, so it unit-tests alone
 * (tests/test_scriptorium.c); scriptorium.c draws with it. */

#define SCR_ROWS 9                      /* every glyph chart is 9 rows tall */
#define SCR_SMALL_ROWS 7                /* ... the small capitals fill 7 */

/* n in 1..3999 -> Roman numerals; anything else -> decimal. Truncates to
 * cap - 1 characters. Returns strlen(out). */
int  scr_roman(unsigned n, char* out, size_t cap);

/* Copy `in`, turning each stand-alone number in 1..3999 (a digit run, with
 * optional ",ddd" thousands groups) into Roman numerals. Numbers glued to
 * letters, decimals, zero-led and out-of-range numbers stay as written. */
void scr_romanize(const char* in, char* out, size_t cap);

/* UTF-8 -> glyph bytes: case kept, Latin accents folded to their base
 * letter, whitespace collapsed and trimmed, a few typographic marks mapped.
 * Returns the length (truncated to cap - 1), or -1 if some character has no
 * glyph (CJK, Cyrillic, emoji, ...). */
int  scr_norm(const char* s, size_t len, char* out, int cap);

/* Glyph chart: SCR_ROWS strings of '0' / '1', all the same width, or NULL.
 * Capitals fill all nine rows; lower-case letters are small capitals, and
 * figures and punctuation sit with them, seven rows under two blank ones.
 * Bytes 1..5 are the middle dot, right / left / up-right arrows, heart. */
const char* const* scr_glyph(unsigned char ch);
int  scr_glyph_cols(unsigned char ch);
/* Width in font pixels of n glyph bytes, one blank column between glyphs. */
int  scr_text_cols(const char* glyphs, int n);

#endif

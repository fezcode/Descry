#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "scriptorium_core.h"

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
    /* truncates, stays terminated */
    assert(scr_roman(3888, b, 4) == 3 && strlen(b) == 3);
}

static void test_romanize(void) {
    char b[96];
    scr_romanize("Ln 18 / 240  Col 47", b, sizeof b);
    assert(!strcmp(b, "Ln XVIII / CCXL  Col XLVII"));
    scr_romanize("1,234 words", b, sizeof b);  assert(!strcmp(b, "MCCXXXIV words"));
    /* out of range: left exactly as written */
    scr_romanize("12,345 words", b, sizeof b); assert(!strcmp(b, "12,345 words"));
    scr_romanize("0 words", b, sizeof b);      assert(!strcmp(b, "0 words"));
    scr_romanize("1920 x 1080", b, sizeof b);  assert(!strcmp(b, "MCMXX x MLXXX"));
    /* numbers glued to letters or decimals are names, not counts */
    scr_romanize("v2, then 3", b, sizeof b);   assert(!strcmp(b, "v2, then III"));
    scr_romanize("notes2024.md", b, sizeof b); assert(!strcmp(b, "notes2024.md"));
    scr_romanize("1.5x", b, sizeof b);         assert(!strcmp(b, "1.5x"));
    scr_romanize("007", b, sizeof b);          assert(!strcmp(b, "007"));
    scr_romanize("99999999999999999999 7", b, sizeof b);
    assert(!strcmp(b, "99999999999999999999 VII"));
    /* tight buffer: terminated, never overrun */
    char t[6];
    scr_romanize("3888 words", t, sizeof t);
    assert(strlen(t) == 5 && !strcmp(t, "MMMDC"));
}

static void test_norm(void) {
    char g[64];
    assert(scr_norm("Notes", 5, g, sizeof g) == 5 && !strcmp(g, "Notes"));
    assert(scr_norm("  a \t b ", 8, g, sizeof g) == 3 && !strcmp(g, "a b"));
    assert(scr_norm("\xc3\x87" "ay", 4, g, sizeof g) == 3 && !strcmp(g, "Cay"));  /* Çay */
    assert(scr_norm("\xc3\xbc", 2, g, sizeof g) == 1 && !strcmp(g, "u"));         /* ü */
    assert(scr_norm("\xc4\xb0\xc4\xb1", 4, g, sizeof g) == 2 && !strcmp(g, "Ii")); /* İı */
    assert(scr_norm("\xc5\x9f\xc4\x9e", 4, g, sizeof g) == 2 && !strcmp(g, "sG")); /* şĞ */
    assert(scr_norm("\xe6\x97\xa5", 3, g, sizeof g) == -1);                       /* 日 */
    assert(scr_norm("\xd0\x9a", 2, g, sizeof g) == -1);                           /* К */
    assert(scr_norm("\xf0\x9f\x93\x9d", 4, g, sizeof g) == -1);                   /* 📝 */
    assert(scr_norm("", 0, g, sizeof g) == 0);
    /* tight buffer */
    char t[4];
    assert(scr_norm("abcdef", 6, t, sizeof t) == 3 && !strcmp(t, "abc"));
}

static void test_glyphs(void) {
    for (int c = 32; c < 127; ++c) {
        const char* const* rows = scr_glyph((unsigned char)c);
        assert(rows);
        size_t w = strlen(rows[0]);
        assert(w >= 1 && w <= 7);
        for (int r = 0; r < SCR_ROWS; ++r) {
            assert(strlen(rows[r]) == w);
            assert(strspn(rows[r], "01") == w);
        }
        assert(scr_glyph_cols((unsigned char)c) == (int)w);
    }
    for (int c = 1; c <= 5; ++c) assert(scr_glyph((unsigned char)c));
    assert(!scr_glyph(200));
    for (int c = 'a'; c <= 'z'; ++c) {                 /* small caps: 5 tall */
        const char* const* rows = scr_glyph((unsigned char)c);
        assert(!strchr(rows[0], '1') && !strchr(rows[1], '1'));
    }
    assert(scr_text_cols("AB", 2) == scr_glyph_cols('A') + 1 + scr_glyph_cols('B'));
    assert(scr_text_cols("", 0) == 0);
}

int main(void) {
    test_roman();
    test_romanize();
    test_norm();
    test_glyphs();
    printf("test_scriptorium: ok\n");
    return 0;
}

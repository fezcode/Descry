/* svg_extra.c parser tests.
 *
 * The renderer half needs FreeType and a font, so what is nailed down here is
 * the harvest: given SVG source, do we come out with the right text runs, at
 * the right place, in the right colour, with the transform chain folded in --
 * and do we correctly ignore the parts that must not be painted.
 *
 * The shields.io badge in badge_svg() is a byte-for-byte copy of what
 * img.shields.io serves for `![License](.../badge/license-MIT-green)`, which
 * is the exact document that used to render as two blank colour bars. */

#include "svg_extra.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int near(float a, float b) { return fabsf(a - b) < 0.01f; }

static const char* badge_svg(void)
{
    return
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"78\" height=\"20\" "
      "role=\"img\" aria-label=\"license: MIT\">"
    "<title>license: MIT</title>"
    "<filter id=\"blur\"><feGaussianBlur stdDeviation=\"16\"/></filter>"
    "<linearGradient id=\"s\" x2=\"0\" y2=\"100%\">"
      "<stop offset=\"0\" stop-color=\"#bbb\" stop-opacity=\".1\"/>"
      "<stop offset=\"1\" stop-opacity=\".1\"/></linearGradient>"
    "<clipPath id=\"r\"><rect width=\"78\" height=\"20\" rx=\"3\"/></clipPath>"
    "<g clip-path=\"url(#r)\">"
      "<rect width=\"47\" height=\"20\" fill=\"#555\"/>"
      "<rect x=\"47\" width=\"31\" height=\"20\" fill=\"#67ac09\"/>"
      "<rect width=\"78\" height=\"20\" fill=\"url(#s)\"/></g>"
    "<g fill=\"#fff\" text-anchor=\"middle\" "
       "font-family=\"Verdana,Geneva,DejaVu Sans,sans-serif\" "
       "text-rendering=\"geometricPrecision\" font-size=\"110\">"
      "<g transform=\"scale(.1)\">"
        "<g aria-hidden=\"true\" fill=\"#010101\">"
          "<text x=\"245\" y=\"150\" fill-opacity=\".8\" filter=\"url(#blur)\" "
                "textLength=\"370\">license</text>"
          "<text x=\"245\" y=\"150\" fill-opacity=\".3\" "
                "textLength=\"370\">license</text></g>"
        "<text x=\"245\" y=\"140\" textLength=\"370\">license</text></g>"
      "<g transform=\"scale(.1)\">"
        "<g aria-hidden=\"true\" fill=\"#010101\">"
          "<text x=\"615\" y=\"150\" fill-opacity=\".8\" filter=\"url(#blur)\" "
                "textLength=\"210\">MIT</text>"
          "<text x=\"615\" y=\"150\" fill-opacity=\".3\" "
                "textLength=\"210\">MIT</text></g>"
        "<text x=\"615\" y=\"140\" textLength=\"210\">MIT</text></g></g></svg>";
}

/* The label text, its position, its size and its colour all survive the
 * <g transform="scale(.1)"> that shields.io wraps every label in. */
static void test_badge_text(void)
{
    const char* s = badge_svg();
    SvgExtra* e = svg_extra_parse(s, strlen(s));
    assert(e);
    assert(!svg_extra_empty(e));

    /* Six <text> elements, but two of them carry filter="url(#blur)" — the
     * drop shadow we cannot reproduce — so four runs survive. */
    assert(svg_extra_text_count(e) == 4);

    const SvgTextRun* shadow = svg_extra_text(e, 0);
    assert(strcmp(shadow->text, "license") == 0);
    assert(near(shadow->x, 24.5f) && near(shadow->y, 15.0f));
    assert(near(shadow->size, 11.0f));
    assert(near(shadow->length, 37.0f));
    assert(shadow->anchor == SVG_ANCHOR_MIDDLE);
    assert(shadow->r == 0x01 && shadow->g == 0x01 && shadow->b == 0x01);
    assert(near(shadow->opacity, 0.3f));
    assert(strcmp(shadow->family, "Verdana,Geneva,DejaVu Sans,sans-serif") == 0);
    assert(!shadow->bold && !shadow->italic);

    const SvgTextRun* label = svg_extra_text(e, 1);
    assert(strcmp(label->text, "license") == 0);
    assert(near(label->y, 14.0f));              /* one unit above its shadow */
    assert(label->r == 255 && label->g == 255 && label->b == 255);
    assert(near(label->opacity, 1.0f));

    const SvgTextRun* value = svg_extra_text(e, 3);
    assert(strcmp(value->text, "MIT") == 0);
    assert(near(value->x, 61.5f) && near(value->y, 14.0f));
    assert(near(value->length, 21.0f));

    svg_extra_free(e);
}

/* <title> is document metadata, not a label: "license: MIT" must not end up
 * on the badge (it did, the first time this parser walked character data). */
static void test_title_is_not_drawn(void)
{
    const char* s = badge_svg();
    SvgExtra* e = svg_extra_parse(s, strlen(s));
    for (int i = 0; i < svg_extra_text_count(e); ++i)
        assert(strcmp(svg_extra_text(e, i)->text, "license: MIT") != 0);
    svg_extra_free(e);
}

/* The rounded corners: a clipPath holding one rect, referenced by a group. */
static void test_badge_clip(void)
{
    const char* s = badge_svg();
    SvgExtra* e = svg_extra_parse(s, strlen(s));
    const SvgClipRect* c = svg_extra_clip(e);
    assert(c->present);
    assert(near(c->x, 0.0f) && near(c->y, 0.0f));
    assert(near(c->w, 78.0f) && near(c->h, 20.0f));
    assert(near(c->rx, 3.0f) && near(c->ry, 3.0f));
    svg_extra_free(e);
}

/* A clipPath nobody references must not mask anything. */
static void test_unused_clip_is_inert(void)
{
    const char* s =
        "<svg width='10' height='10'>"
        "<clipPath id='r'><rect width='10' height='10' rx='3'/></clipPath>"
        "<rect width='10' height='10' fill='#000'/></svg>";
    SvgExtra* e = svg_extra_parse(s, strlen(s));
    assert(!svg_extra_clip(e)->present);
    assert(svg_extra_empty(e));
    svg_extra_free(e);
}

/* A clipPath declared inside <defs> — where most authoring tools put it — is
 * still found, even though nothing inside <defs> is painted where it stands. */
static void test_clip_inside_defs(void)
{
    const char* s =
        "<svg width='40' height='20'>"
        "<defs><clipPath id='c'><rect width='40' height='20' rx='4'/></clipPath>"
        "<text x='1' y='1'>not drawn</text></defs>"
        "<g clip-path='url(#c)'><rect width='40' height='20'/></g></svg>";
    SvgExtra* e = svg_extra_parse(s, strlen(s));
    const SvgClipRect* c = svg_extra_clip(e);
    assert(c->present && near(c->rx, 4.0f));
    assert(svg_extra_text_count(e) == 0);       /* the <defs> text stays put */
    svg_extra_free(e);
}

/* The logo: an <image> whose href is a nested SVG data URI. */
static void test_image_href(void)
{
    const char* s =
        "<svg width='85' height='20'>"
        "<image x='5' y='3' width='14' height='14' "
               "href='data:image/svg+xml;base64,PHN2Zy8+'/></svg>";
    SvgExtra* e = svg_extra_parse(s, strlen(s));
    assert(svg_extra_image_count(e) == 1);
    const SvgImageRef* im = svg_extra_image(e, 0);
    assert(near(im->x, 5.0f) && near(im->y, 3.0f));
    assert(near(im->w, 14.0f) && near(im->h, 14.0f));
    assert(strcmp(im->href, "data:image/svg+xml;base64,PHN2Zy8+") == 0);
    svg_extra_free(e);
}

/* Presentation attributes inherit down the tree, style="" beats the plain
 * attribute, and nested transforms compose. */
static void test_inheritance_and_transform(void)
{
    const char* s =
        "<svg width='100' height='100'>"
        "<g fill='#ff0000' font-size='10' font-family='Arial' "
           "transform='translate(10,20)'>"
          "<g transform='scale(2)' font-weight='bold'>"
            "<text x='5' y='5' style='fill:#00ff00' font-style='italic'>hi</text>"
          "</g>"
        "</g></svg>";
    SvgExtra* e = svg_extra_parse(s, strlen(s));
    assert(svg_extra_text_count(e) == 1);
    const SvgTextRun* t = svg_extra_text(e, 0);
    assert(near(t->x, 20.0f) && near(t->y, 30.0f));   /* translate then scale */
    assert(near(t->size, 20.0f));                     /* 10 * 2               */
    assert(t->r == 0 && t->g == 255 && t->b == 0);    /* style wins over fill */
    assert(t->bold && t->italic);
    assert(strcmp(t->family, "Arial") == 0);
    svg_extra_free(e);
}

/* Character data: entities decoded, tspans flattened, whitespace collapsed. */
static void test_text_content(void)
{
    const char* s =
        "<svg width='100' height='20'>"
        "<text x='0' y='10'>  a &amp;&#32;b\n  <tspan>&#x263A;</tspan> c </text>"
        "</svg>";
    SvgExtra* e = svg_extra_parse(s, strlen(s));
    assert(svg_extra_text_count(e) == 1);
    assert(strcmp(svg_extra_text(e, 0)->text, "a & b \xe2\x98\xba c") == 0);
    svg_extra_free(e);
}

/* fill="none" and display:none produce nothing; an empty <text/> is not a run. */
static void test_invisible_text(void)
{
    const char* s =
        "<svg width='100' height='20'>"
        "<text x='0' y='10' fill='none'>invisible</text>"
        "<g display='none'><text x='0' y='10'>hidden</text></g>"
        "<text x='0' y='10' visibility='hidden'>gone</text>"
        "<text x='0' y='10'/>"
        "<text x='0' y='10'>   </text>"
        "</svg>";
    SvgExtra* e = svg_extra_parse(s, strlen(s));
    assert(svg_extra_text_count(e) == 0);
    svg_extra_free(e);
}

/* text-anchor, textLength and font-size units all reach the run. */
static void test_anchor_and_units(void)
{
    const char* s =
        "<svg width='100' height='20'>"
        "<text x='90' y='10' text-anchor='end' font-size='12pt'>right</text>"
        "<text x='0' y='10' textLength='50' font-weight='700'>wide</text>"
        "</svg>";
    SvgExtra* e = svg_extra_parse(s, strlen(s));
    assert(svg_extra_text_count(e) == 2);
    const SvgTextRun* a = svg_extra_text(e, 0);
    assert(a->anchor == SVG_ANCHOR_END);
    assert(near(a->size, 16.0f));                     /* 12pt at 96dpi */
    const SvgTextRun* b = svg_extra_text(e, 1);
    assert(b->anchor == SVG_ANCHOR_START);
    assert(near(b->length, 50.0f));
    assert(b->bold);
    svg_extra_free(e);
}

/* Comments, XML prologs and self-closing shapes must not derail the scan, and
 * a document with nothing we handle must report itself empty. */
static void test_scanner_robustness(void)
{
    const char* s =
        "<?xml version='1.0'?><!DOCTYPE svg><!-- <text x='0' y='0'>no</text> -->"
        "<svg width='10' height='10'><path d='M0 0 L1 1'/>"
        "<rect width='2' height='2'/></svg>";
    SvgExtra* e = svg_extra_parse(s, strlen(s));
    assert(svg_extra_text_count(e) == 0);
    assert(svg_extra_image_count(e) == 0);
    assert(svg_extra_empty(e));
    svg_extra_free(e);

    /* Truncated mid-tag: must terminate, not read past the buffer. */
    const char* t = "<svg width='10'><g fill='#fff'><text x='1' y='2'>oh";
    SvgExtra* e2 = svg_extra_parse(t, strlen(t));
    assert(e2);
    svg_extra_free(e2);

    SvgExtra* e3 = svg_extra_parse("", 0);
    assert(e3 && svg_extra_empty(e3));
    svg_extra_free(e3);
}

int main(void)
{
    test_badge_text();
    test_title_is_not_drawn();
    test_badge_clip();
    test_unused_clip_is_inert();
    test_clip_inside_defs();
    test_image_href();
    test_inheritance_and_transform();
    test_text_content();
    test_invisible_text();
    test_anchor_and_units();
    test_scanner_robustness();
    printf("test_svg_extra: all passed\n");
    return 0;
}

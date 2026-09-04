/* Parser tests for the `#tag` relation spans (markdown.c tags_post).
 *
 * Tags are the loose end of the two relation sigils: `[[` is rare enough in
 * prose that an unpaired one is harmless, but `#` is not — it opens every
 * heading and shows up in `C#`, `issue #12` and colour literals — so the
 * rules that keep it from lighting up ordinary text are what's worth
 * pinning down here. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "markdown.h"

/* Parse `src` and return the doc; caller md_doc_free()s it. */
static MdDoc parse(const char* src)
{
    MdDoc d;
    int rc = md_doc_parse(src, strlen(src), &d);
    assert(rc == 0);
    return d;
}

/* The rendered-text slice a tag covers, as a NUL-terminated string. */
static void tag_name(const MdDoc* d, size_t i, char* out, size_t cap)
{
    size_t n = d->tags[i].name_len;
    if (n >= cap) n = cap - 1;
    memcpy(out, d->data + d->tags[i].name_start, n);
    out[n] = 0;
}

static void test_basic(void)
{
    MdDoc d = parse("Filed under #graph-theory here.\n");
    assert(d.tag_count == 1);
    char nm[80]; tag_name(&d, 0, nm, sizeof nm);
    assert(strcmp(nm, "graph-theory") == 0);
    /* Every byte of the span, the `#` included, is a link AND a tag. */
    for (size_t b = d.tags[0].start; b < d.tags[0].end; ++b) {
        assert(d.style[b] & STYLE_LINK);
        assert(d.style[b] & STYLE_TAG);
    }
    /* The text on either side is neither. */
    assert(!(d.style[0] & STYLE_TAG));
    assert(!(d.style[d.len - 1] & STYLE_TAG));
    md_doc_free(&d);
}

static void test_two_on_one_line(void)
{
    MdDoc d = parse("Two: #alpha and #beta.\n");
    assert(d.tag_count == 2);
    char a[40], b[40];
    tag_name(&d, 0, a, sizeof a);
    tag_name(&d, 1, b, sizeof b);
    assert(strcmp(a, "alpha") == 0);
    assert(strcmp(b, "beta") == 0);
    md_doc_free(&d);
}

/* Nested paths are one tag, not a tag plus a stray slash. */
static void test_nested_path(void)
{
    MdDoc d = parse("Filed under #math/algebra today.\n");
    assert(d.tag_count == 1);
    char nm[40]; tag_name(&d, 0, nm, sizeof nm);
    assert(strcmp(nm, "math/algebra") == 0);
    md_doc_free(&d);
}

/* A `#` needs whitespace or an opening bracket in front of it. */
static void test_boundary_rule(void)
{
    MdDoc d = parse("I write C# and Java.\n");
    assert(d.tag_count == 0);
    md_doc_free(&d);

    d = parse("See (#bracketed) and [#square].\n");
    assert(d.tag_count == 2);
    md_doc_free(&d);
}

/* A `#` with nothing a name can hold after it is just a `#`. */
static void test_empty_name(void)
{
    MdDoc d = parse("A bare # on its own.\n");
    assert(d.tag_count == 0);
    md_doc_free(&d);

    d = parse("Punctuation #! stops it.\n");
    assert(d.tag_count == 0);
    md_doc_free(&d);
}

/* Heading markers are consumed by the block parser, so `## Title` arrives
 * here as the text "Title" and can never read as a tag. */
static void test_headings_are_not_tags(void)
{
    MdDoc d = parse("# Title\n\n## Subtitle\n");
    assert(d.tag_count == 0);
    md_doc_free(&d);
}

/* Paragraph boundaries leave no separator byte in `data`, so a buffer-wide
 * scan could run the last word of one paragraph into the next. */
static void test_paragraphs_stay_separate(void)
{
    MdDoc d = parse("Ends with a #tag\n\nplain follows.\n");
    assert(d.tag_count == 1);
    char nm[40]; tag_name(&d, 0, nm, sizeof nm);
    assert(strcmp(nm, "tag") == 0);
    md_doc_free(&d);
}

static void test_name_length_cap(void)
{
    char src[256];
    int o = snprintf(src, sizeof src, "x #");
    for (int i = 0; i < MD_TAG_MAX_NAME + 1; ++i) src[o++] = 'a';
    o += snprintf(src + o, sizeof src - o, " y\n");
    (void)o;
    MdDoc d = parse(src);
    assert(d.tag_count == 0);
    md_doc_free(&d);

    o = snprintf(src, sizeof src, "x #");
    for (int i = 0; i < MD_TAG_MAX_NAME; ++i) src[o++] = 'a';
    o += snprintf(src + o, sizeof src - o, " y\n");
    d = parse(src);
    assert(d.tag_count == 1);
    assert(d.tags[0].name_len == MD_TAG_MAX_NAME);
    md_doc_free(&d);
}

static void test_wiki_links_still_parse(void)
{
    MdDoc d = parse("Mixed: [[wikis]] plus #markdown.\n");
    assert(d.wiki_count == 1);
    assert(d.tag_count == 1);
    md_doc_free(&d);
}

int main(void)
{
    test_basic();
    test_two_on_one_line();
    test_nested_path();
    test_boundary_rule();
    test_empty_name();
    test_headings_are_not_tags();
    test_paragraphs_stay_separate();
    test_name_length_cap();
    test_wiki_links_still_parse();
    printf("test_markdown: all passed\n");
    return 0;
}

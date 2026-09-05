#include <assert.h>
#include <stdio.h>
#include "inputfield.h"

/* All widths in pixels. A 200 px window is the stand-in for the modal's
 * input field; CARET is the 2 px caret the modals draw. */
#define VIEW  200
#define CARET 2

static void test_clamp(void)
{
    /* Content narrower than the window: nothing to scroll. */
    assert(input_scroll_clamp(0,   100, VIEW) == 0);
    assert(input_scroll_clamp(50,  100, VIEW) == 0);
    assert(input_scroll_clamp(-30, 100, VIEW) == 0);
    /* Content wider: scroll stops at the content's right edge. */
    assert(input_scroll_clamp(0,   500, VIEW) == 0);
    assert(input_scroll_clamp(150, 500, VIEW) == 150);
    assert(input_scroll_clamp(400, 500, VIEW) == 300);
    assert(input_scroll_clamp(-10, 500, VIEW) == 0);
}

static void test_short_text_never_scrolls(void)
{
    /* Text that fits stays pinned left wherever the caret is. */
    assert(input_scroll_follow(0, 0,   CARET, 120, VIEW) == 0);
    assert(input_scroll_follow(0, 120, CARET, 120, VIEW) == 0);
    /* Even from a stale non-zero scroll. */
    assert(input_scroll_follow(90, 60, CARET, 120, VIEW) == 0);
}

static void test_caret_at_end_of_long_text(void)
{
    /* The whole caret must land inside the window, not half past its
     * right edge — hence text_w + CARET, not text_w. */
    assert(input_scroll_follow(0, 500, CARET, 500, VIEW) == 302);
}

static void test_caret_back_to_start(void)
{
    /* Home on a long path scrolls all the way back. */
    assert(input_scroll_follow(302, 0, CARET, 500, VIEW) == 0);
}

static void test_caret_already_visible(void)
{
    /* Caret inside the window leaves the view alone (no jitter per frame). */
    assert(input_scroll_follow(100, 150, CARET, 500, VIEW) == 100);
    assert(input_scroll_follow(100, 100, CARET, 500, VIEW) == 100);
    assert(input_scroll_follow(100, 298, CARET, 500, VIEW) == 100);
    /* One px further and it has to move. */
    assert(input_scroll_follow(100, 299, CARET, 500, VIEW) == 101);
}

static void test_caret_left_of_window(void)
{
    /* Walking the caret left past the window pulls the view with it. */
    assert(input_scroll_follow(200, 150, CARET, 500, VIEW) == 150);
}

static void test_text_shrinks(void)
{
    /* Deleting most of a long path must not leave the view parked out in
     * empty space beyond the (now short) text. */
    assert(input_scroll_follow(300, 40, CARET, 40, VIEW) == 0);
}

int main(void)
{
    test_clamp();
    test_short_text_never_scrolls();
    test_caret_at_end_of_long_text();
    test_caret_back_to_start();
    test_caret_already_visible();
    test_caret_left_of_window();
    test_text_shrinks();
    printf("all input field tests passed\n");
    return 0;
}

#include "inputfield.h"

int input_scroll_clamp(int scroll, int content_w, int view_w)
{
    int max = content_w - view_w;
    if (max < 0)      max = 0;
    if (scroll > max) scroll = max;
    if (scroll < 0)   scroll = 0;
    return scroll;
}

int input_scroll_follow(int scroll, int caret_x, int caret_w,
                        int text_w, int view_w)
{
    if (caret_x < scroll)
        scroll = caret_x;
    else if (caret_x + caret_w > scroll + view_w)
        scroll = caret_x + caret_w - view_w;
    return input_scroll_clamp(scroll, text_w + caret_w, view_w);
}

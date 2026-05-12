# Pretty edit mode test

Plain text on this line for baseline comparison.

This line has **bold** and *italic* and `inline code` mid-sentence.

Mid-word styling: pre**BOLD**post and pre*italic*post should keep
the prefix and suffix plain while the middle is styled.

Bold and italic combined: ***triple-star*** and **bold with *italic*
inside** demonstrate nesting.

Multiple inline `code` `runs` `on` `one` `line`.

A long mixed line: edit-mode now shows **bold** runs with a heavier
weight, *italic* runs slanted ~12 degrees, `code` muted, all in the
same monospace cell width so cursor positioning still feels sane.

## Heading lines stay clean

H2 headings ignore inline styles to keep their own weight.

### H3 also clean

Fenced code blocks now suppress the inline-marker scan:

```c
int main(void) {
    /* `**this**` should NOT render bold inside a fence. */
    char* s = "**still plain inside the fence**";
    printf("%s\n", s);
    return 0;
}
```

After the closing fence, **bold** and *italic* and `code` resume
working as inline markers on body lines.

Wiki-link auto-complete: place the cursor on the next line, press
`Ctrl+E` if not already in edit mode, then type `[[` to open the
note picker. Tab/Enter inserts; Esc cancels.

Try it here: 

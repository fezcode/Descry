# Wiki-link visual test

Downsee now recognizes Obsidian-style **wiki links**. The bytes
between (and including) the brackets get the link color so they're
visually distinct from regular text.

A simple link: [[sample]] should be highlighted.

Mid-paragraph: see [[test]] for the feature regression file, or
[[symbols]] for the font fallback test.

Two on the same line: [[sample]] and [[test]].

Wiki links don't span line breaks: [[wont
match]] should render as plain text (no highlight).

A code block (links here should NOT be highlighted because parser
doesn't recurse into code spans... wait, actually they will — the
post-pass scans the whole `data` buffer):

```
[[this also gets highlighted in v0.11 — known limitation]]
```

Click navigation: not yet wired up. Visual only.

$ErrorActionPreference = "Stop"

$out = "D:\Workhammer\Descry\data\giant.md"
$targetBytes = 1MB

$header = @'
# The Complete Markdown Kitchen Sink

> A reference document exercising every markdown feature supported by CommonMark, GFM, and most extended flavors.

---

## Table of Contents

1. [Headings](#headings)
2. [Paragraphs and Line Breaks](#paragraphs-and-line-breaks)
3. [Emphasis](#emphasis)
4. [Lists](#lists)
5. [Links](#links)
6. [Images](#images)
7. [Code](#code)
8. [Blockquotes](#blockquotes)
9. [Horizontal Rules](#horizontal-rules)
10. [Tables](#tables)
11. [Task Lists](#task-lists)
12. [Footnotes](#footnotes)
13. [Definition Lists](#definition-lists)
14. [HTML](#html)
15. [Math](#math)
16. [Diagrams](#diagrams)
17. [Admonitions](#admonitions)
18. [Strikethrough and Highlight](#strikethrough-and-highlight)
19. [Subscript and Superscript](#subscript-and-superscript)
20. [Emoji](#emoji)
21. [Front Matter](#front-matter)
22. [Wiki Links](#wiki-links)
23. [Tags](#tags)

---

## Headings

# Heading level 1
## Heading level 2
### Heading level 3
#### Heading level 4
##### Heading level 5
###### Heading level 6

Alternate H1
============

Alternate H2
------------

---

## Paragraphs and Line Breaks

This is a regular paragraph. It can span multiple sentences and is just plain text. Markdown collapses single newlines into a single space when rendering.

This is the second paragraph, separated from the first by a blank line.

This line ends with two spaces
so this line appears on a new line in rendered output.

Use a backslash\
at the end of a line for a hard break too.

---

## Emphasis

*Italic with asterisks*, _italic with underscores_.
**Bold with double asterisks**, __bold with double underscores__.
***Bold and italic***, ___bold and italic___, **_mixed_**, *__mixed__*.

You can ~~strike through text~~ in GFM.

==Highlighted text== is supported in some flavors.

---

## Lists

### Unordered

- Apples
- Bananas
  - Cavendish
  - Plantain
    - Green
    - Ripe
- Cherries

* Star bullets work too
+ Plus bullets work too

### Ordered

1. First
2. Second
3. Third
   1. Nested item
   2. Another nested item
      1. Deeply nested
4. Fourth

### Mixed

1. Step one
   - Sub item
   - Another sub item
2. Step two
   1. Numbered sub item
   2. Another numbered sub item

### Loose vs tight

Tight list:
- one
- two
- three

Loose list:

- one

- two

- three

---

## Links

[Inline link](https://example.com)
[Inline link with title](https://example.com "Example title")
<https://autolink.example.com>
<user@example.com>

[Reference link][ref-id]
[Collapsed reference][]
[Shortcut reference]

[ref-id]: https://example.com/reference "Reference title"
[Collapsed reference]: https://example.com/collapsed
[Shortcut reference]: https://example.com/shortcut

[Relative link](./other-file.md)
[Anchor link](#headings)

---

## Images

![Alt text](https://via.placeholder.com/150 "Image title")
![Reference image][img-ref]

[img-ref]: https://via.placeholder.com/200

Linked image:
[![Alt](https://via.placeholder.com/100)](https://example.com)

---

## Code

Inline `code` with backticks. Use `` ` `` to embed a backtick.

Indented code block (four spaces):

    function hello() {
        return "world";
    }

Fenced code block (no language):

```
plain text in a fenced block
```

Fenced code block with language:

```python
def fibonacci(n):
    if n < 2:
        return n
    return fibonacci(n - 1) + fibonacci(n - 2)

print([fibonacci(i) for i in range(10)])
```

```javascript
const greet = (name) => `Hello, ${name}!`;
console.log(greet("world"));
```

```c
#include <stdio.h>

int main(void) {
    printf("Hello, world!\n");
    return 0;
}
```

```rust
fn main() {
    let v: Vec<i32> = (0..10).collect();
    println!("{:?}", v);
}
```

```sql
SELECT id, name FROM users WHERE active = true ORDER BY name LIMIT 10;
```

```bash
#!/usr/bin/env bash
for i in {1..5}; do
    echo "iteration $i"
done
```

```json
{
    "name": "Descry",
    "version": "0.71.0",
    "features": ["markdown", "lua", "plugins"]
}
```

```yaml
name: Descry
version: 0.71.0
features:
  - markdown
  - lua
  - plugins
```

```diff
- old line removed
+ new line added
  unchanged line
```

---

## Blockquotes

> Single line blockquote.

> Multi-line blockquote
> spanning two lines.

> Nested blockquotes:
>
> > Inner quote
> >
> > > Even deeper

> ### Blockquote with heading
>
> And a paragraph.
>
> - And a list item
> - And another

---

## Horizontal Rules

---

***

___

- - -

* * *

---

## Tables

| Column A | Column B | Column C |
|----------|----------|----------|
| a1       | b1       | c1       |
| a2       | b2       | c2       |
| a3       | b3       | c3       |

Aligned columns:

| Left | Center | Right |
|:-----|:------:|------:|
| L    | C      | R     |
| 1    | 2      | 3     |
| foo  | bar    | baz   |

Inline formatting in tables:

| Feature | Example                       |
|---------|-------------------------------|
| Bold    | **bold text**                 |
| Italic  | *italic text*                 |
| Code    | `inline code`                 |
| Link    | [example](https://example.com)|

---

## Task Lists

- [x] Completed task
- [ ] Pending task
- [x] Another done one
  - [ ] Nested incomplete
  - [x] Nested complete
- [ ] Final pending task

---

## Footnotes

This sentence has a footnote.[^1] And this one has another.[^longnote]

[^1]: Short footnote text.
[^longnote]: A longer footnote with multiple paragraphs.

    The second paragraph of the footnote.

---

## Definition Lists

Term 1
:   Definition 1

Term 2
:   Definition 2a
:   Definition 2b

Markdown
:   A lightweight markup language with plain-text formatting syntax.

CommonMark
:   A strongly defined, highly compatible specification of Markdown.

---

## HTML

<p>Raw HTML <strong>paragraph</strong>.</p>

<details>
<summary>Click to expand</summary>

Hidden content with **markdown** inside.

</details>

<kbd>Ctrl</kbd>+<kbd>S</kbd> to save.

<mark>Highlighted via HTML</mark>.

<sub>subscript</sub> and <sup>superscript</sup>.

<div align="center">Centered text via HTML.</div>

---

## Math

Inline math: $E = mc^2$ and $a^2 + b^2 = c^2$.

Block math:

$$
\int_{-\infty}^{\infty} e^{-x^2} \, dx = \sqrt{\pi}
$$

$$
\begin{aligned}
f(x) &= ax^2 + bx + c \\
f'(x) &= 2ax + b
\end{aligned}
$$

---

## Diagrams

```mermaid
graph TD
    A[Start] --> B{Decision}
    B -->|Yes| C[Path A]
    B -->|No| D[Path B]
    C --> E[End]
    D --> E
```

```mermaid
sequenceDiagram
    Alice->>Bob: Hello Bob
    Bob-->>Alice: Hi Alice
```

---

## Admonitions

> [!NOTE]
> Useful information that users should know.

> [!TIP]
> Helpful advice for doing things better.

> [!IMPORTANT]
> Key information users need to know.

> [!WARNING]
> Urgent info that needs immediate attention.

> [!CAUTION]
> Negative potential consequences of an action.

---

## Strikethrough and Highlight

This text is ~~struck through~~.
This text is ==highlighted== (extension).

---

## Subscript and Superscript

H~2~O and E = mc^2^ (extension syntax).

---

## Emoji

:smile: :rocket: :sparkles: :tada: :white_check_mark: :x: :warning: :memo:

---

## Front Matter

```
---
title: Example Document
date: 2026-05-16
tags: [markdown, example]
author: Descry
---
```

---

## Wiki Links

[[Internal Page]]
[[Another Page|Display Text]]
[[Folder/Subpage]]

---

## Tags

#markdown #reference #example #descry #kitchen-sink

---

## Long Form Content Sample

Lorem ipsum dolor sit amet, consectetur adipiscing elit. Sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat. Duis aute irure dolor in reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla pariatur. Excepteur sint occaecat cupidatat non proident, sunt in culpa qui officia deserunt mollit anim id est laborum.

Sed ut perspiciatis unde omnis iste natus error sit voluptatem accusantium doloremque laudantium, totam rem aperiam, eaque ipsa quae ab illo inventore veritatis et quasi architecto beatae vitae dicta sunt explicabo. Nemo enim ipsam voluptatem quia voluptas sit aspernatur aut odit aut fugit, sed quia consequuntur magni dolores eos qui ratione voluptatem sequi nesciunt.

At vero eos et accusamus et iusto odio dignissimos ducimus qui blanditiis praesentium voluptatum deleniti atque corrupti quos dolores et quas molestias excepturi sint occaecati cupiditate non provident, similique sunt in culpa qui officia deserunt mollitia animi, id est laborum et dolorum fuga.

---

'@

# Write the header once
[System.IO.File]::WriteAllText($out, "")

$fs = [System.IO.File]::Open($out, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
$bw = New-Object System.IO.StreamWriter($fs, [System.Text.UTF8Encoding]::new($false))
$bw.NewLine = "`n"

# Top of file: front matter
$frontMatter = @'
---
title: Giant Markdown Reference
date: 2026-05-16
tags: [markdown, reference, kitchen-sink, stress-test]
author: Descry
description: A 100 MB markdown file exercising every markdown feature, used to stress-test the editor.
---

'@
$bw.Write($frontMatter)
$bw.Write($header)

$chunkNum = 0
$reportEvery = 50

while ($fs.Position -lt $targetBytes) {
    $chunkNum++

    $bw.Write("`n## Section ${chunkNum}`n`n")
    $bw.Write("This is iteration **${chunkNum}** of the kitchen-sink content. Generated for stress-testing the Descry markdown editor with very large documents.`n`n")
    $bw.Write("> Iteration ${chunkNum} block quote with *emphasis*, **strong**, ~~strikethrough~~, and ``inline code``.`n`n")

    # Re-emit the kitchen sink (without re-emitting the TOC)
    $bw.Write($header)

    # Add a few unique-ish lines per iteration so the file isn't pure repetition
    $bw.Write("`n### Unique lines for iteration ${chunkNum}`n`n")
    for ($i = 0; $i -lt 10; $i++) {
        $bw.Write("- Item ${i} in section ${chunkNum} - lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua.`n")
    }
    $bw.Write("`n")

    if ($chunkNum % $reportEvery -eq 0) {
        $mb = [math]::Round($fs.Position / 1MB, 2)
        Write-Host "Iteration $chunkNum -- $mb MB written"
        $bw.Flush()
    }
}

$bw.Flush()
$bw.Close()
$fs.Close()

$finalMb = [math]::Round((Get-Item $out).Length / 1MB, 2)
Write-Host "Done. $out is $finalMb MB ($chunkNum iterations)."

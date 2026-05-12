# Images in Downsee

Downsee renders inline images in preview mode for PNG, JPG/JPEG, GIF,
WEBP and BMP — both as embeds inside a note and as standalone files
you click in the sidebar. This note demonstrates the syntax.

## Basic embed

The standard Markdown image syntax:

```
![alt text](images/osi-approved.png)
```

renders as:

![OSI Approved seal](images/osi-approved.png)

The alt text is parsed but currently isn't shown; it's there for
accessibility tooling and future fallback rendering.

## Path resolution

Images can be referenced three ways:

1. **Relative to this note** — `images/foo.png` resolves under the
   directory holding the note.
2. **Absolute path** — `C:/Users/.../foo.png` or `/home/.../foo.png`
   are used as-is.
3. **From the vault root** — anything not absolute uses the open note's
   directory as the base, so `images/foo.png` works from any note in
   `data/`.

External `http://` and `https://` URLs are **not** fetched — only
local files.

## Standalone image files

`.png`, `.jpg`, `.jpeg`, `.gif`, `.webp`, `.bmp` files in the vault
appear directly in the left sidebar alongside `.md` notes. Click one
to open it in preview mode — Downsee synthesizes a tiny markdown
buffer `# filename` + `![](filename)` and renders the image. Save and
edit are blocked in this mode so a stray keystroke can't corrupt the
binary.

Try clicking `data/images/osi-approved.png` in the sidebar.

## Sizing

Images are scaled to fit the document width while preserving aspect
ratio. If you want smaller images, resize them on disk — there's no
explicit `=400x` size syntax yet.

## Notes inside lists work

- Plain text bullet
- ![Inline list image](images/osi-approved.png)
- Another bullet after the image

## Ctrl+click on a link still works

Ctrl+click [this link](https://example.com) to open it externally
(with a confirm dialog), independent of the image rendering above.

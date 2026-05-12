# Downsee

A markdown editor in **C** that's *almost* an Obsidian clone — small, fast, and with `inline code` that renders properly.

## What works in v0.6

- SDL2 window, vsync'd render loop
- FreeType + HarfBuzz with **bold**, *italic*, and ***bold italic*** synthesis
- Per-character styling driven by md4c spans (this `inline code` is a different font)
- Lua-driven config (window size, fonts, theme — all from `init.lua`)
- md4c-parsed markdown (headings, lists, quotes, code blocks, soft wrap)
- Mouse-wheel and keyboard scrolling

## A code block

```
int main(void) {
    return 0;
}
```

## Roadmap

1. Real text buffer + cursor + editing
2. Theme palette beyond fg/bg
3. Vault sidebar
4. Image rendering (libpng / libjpeg, sandboxed decode)
5. Plugin host with restricted Lua API

> Press **ESC** or **Ctrl+Q** to quit. Mouse wheel, ↑↓, PgUp/PgDn, Space, and Home/End all scroll.

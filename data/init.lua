-- Descry config. This file returns a single table; values not present
-- here fall back to the defaults compiled into the binary.

return {
    window_w  = 1100,
    window_h  = 720,

    -- Vault: the folder shown in the sidebar. Top-level *.md files appear
    -- as a flat list (recursive tree comes in a later pass).
    vault_path     = "data",
    sidebar_open   = true,
    sidebar_width  = 240,

    -- Where plugins (*.lua) are loaded from on startup.
    plugin_path    = "data/plugins",

    -- Start in EDIT instead of PREVIEW. (Toggle any time with Ctrl+E.)
    start_in_edit_mode = false,

    -- Preview body font (prose, lists, quotes, table cells, headings).
    -- Must be a TrueType file.
    font_path      = "C:/Windows/Fonts/consola.ttf",
    -- IDE chrome font: title bar, menus, sidebar, status bar, overlays
    -- and modals. Defaults to font_path if unset.
    font_path_ide  = "C:/Windows/Fonts/consola.ttf",
    -- Editor + code-block monospace face. Defaults to font_path if unset.
    font_path_mono = "C:/Windows/Fonts/consola.ttf",

    font_size     = 16,
    font_size_h1  = 28,
    font_size_h2  = 22,
    font_size_h3  = 18,

    -- Fallback fonts, tried in order when the primary font lacks a glyph.
    -- Loaded at the same size as the primary, always regular weight.
    font_fallback = {
        "C:/Windows/Fonts/seguisym.ttf",   -- ★ ✓ ✗ ⌘ ⏎ ☐ ☑ etc.
        "C:/Windows/Fonts/msgothic.ttc",   -- CJK (Japanese has Hiragana/Kanji)
    },

    -- Pick a theme by name. The theme defines all 14 colors. To override
    -- one specific color, add a color_* key after this line — it'll win
    -- over the theme's value.
    -- Available: "Editorial Dark" (default), "Default Dark", "Light",
    -- "Solarized Dark", "Nord", "Gruvbox Dark", "Rose Pine Moon".
    theme = "Editorial Dark",

    -- Keybindings. Modifier order: ctrl, shift, alt (e.g. "ctrl+shift+z").
    -- Action names: quit, save, open_file, toggle_sidebar, toggle_edit,
    -- undo, redo, select_all, copy, cut, paste. Plugins can register more.
    -- Anything NOT listed here uses the default; this table is overrides.
    keybindings = {
        -- ["ctrl+s"]       = "save",         -- default; uncomment to override
        -- ["ctrl+shift+t"] = "my_action",    -- map to a plugin-registered action
    },
}

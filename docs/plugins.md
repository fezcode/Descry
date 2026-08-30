# Plugins

Descry can load Lua scripts at startup and expose them as named **actions**
that the user can run from the command palette, bind to a key, or invoke
from the menus.

A plugin can register named actions, show toasts, dialogs, **yes/no prompts
and text prompts**, **read and edit the open document**, **list, refresh and
open vault notes**, **declare typed settings** that get a real settings UI,
**invoke other actions**, read the **theme** and the **clipboard**, and
**subscribe to lifecycle events** (`open` / `save` / `text_change` /
`mode_change` / `vault_change` / `config_change`). The surface is intentionally
small but no longer a dead end — a plugin can do real work on the text the
user is looking at.

What's still missing (custom UI panels, timers, inter-plugin calls) is listed
honestly under [What's NOT exposed](#whats-not-exposed-yet) at the bottom.

---

## Where plugins live

By default Descry scans `data/plugins/` next to the exe and loads every
`*.lua` file it finds (top level only — no recursion).

Override the directory with `plugin_path` in your `init.lua` or
`settings.lua`:

```lua
return {
    plugin_path = "C:/Users/me/descry-plugins",
    -- ... other config keys
}
```

Hidden files (anything starting with `.`) are skipped. Files that fail to
parse don't break the boot — they're flagged in the **Plugins overlay**
(Ctrl+Alt+P) with the Lua error message.

---

## Lifecycle

1. App starts, reads `init.lua` then overlays `settings.lua`.
2. The `descry` global is created (`notify`, `dialog`,
   `register_action`).
3. Every `*.lua` under the plugin dir is `dofile`'d, in directory-listing
   order. Each file's top-level body runs once. Anything you call at the
   top level (`descry.notify(...)`) fires *during load*, before the
   editor window finishes drawing — these get queued and surface once the
   UI is up.
4. Actions registered via `descry.register_action(name, fn)` get added
   to the action registry.
5. The user can later invoke any action via the command palette
   (Ctrl+Shift+P), bind it to a key in `Settings → Keybindings…`, or
   reload the plugin set via the Plugins overlay (Ctrl+Alt+P → Reload).

Reloading is destructive: every plugin re-runs from scratch and the
actions table is wiped first, so removed plugins disappear cleanly.
There is no per-plugin teardown hook — keep state in module-locals if
you want it to reset on reload, in `_G` if you want it to persist (until
the app exits).

---

## The `descry` global

A handful of top-level functions plus four sub-tables (`descry.buffer`,
`descry.vault`, `descry.decorations`, `descry.clipboard`) and one field,
`descry.version` (the app version string, e.g. `"0.83.0"`).

### `descry.register_action(name, fn)`

Register a named action. `name` must be a string (lowercase, no spaces
recommended — the keybindings UI shows the raw name). `fn` is a Lua
function called with no arguments when the action is invoked.

```lua
descry.register_action("uppercase_clipboard", function()
    -- ... your code here
end)
```

Calling `register_action` with a name that already exists overwrites
the previous binding. Action names that collide with built-ins (like
`save`, `quit`, `find`) are still accepted but the built-in always wins
when the user binds a key — your action will show in the command palette
but won't fire from the keystroke. This is deliberate; tell the user to
pick a different name if it matters.

### `descry.notify(message [, ms])`

Push a one-line toast onto the status bar. Use this for transient
"happened" feedback ("indexed 142 files", "selection cleaned"). Long
messages get clipped at the right edge — keep it under ~80 chars. The
optional `ms` overrides how long the toast stays (default ~3.5 s).

```lua
descry.notify("hello from a plugin")
descry.notify("this one lingers", 8000)
```

`message` must be a string. The text also gets logged to stderr so
plugin authors running Descry from a terminal can see what fired.

### `descry.log(...)`

`print`-style logging to the log file / stderr only — no toast. Any number
of values, tostring'd and tab-joined. Use it for diagnostics you don't want
the user to see.

### `descry.invoke(name)`

Run any action by name — a built-in (`"save"`, `"toggle_edit"`,
`"vault_search"`, …: the same ids the keybindings overlay shows) or another
plugin's action. Returns `true` if something ran.

```lua
descry.register_action("save_and_preview", function()
    descry.invoke("save")
    descry.set_edit_mode(false)
end)
```

### `descry.dialog(message)` or `descry.dialog(title, message)`

Pop a modal dialog with an OK button. Blocks the user until they
dismiss. Use this for output that demands attention (errors, results
worth reading) or when you want to confirm a step.

```lua
descry.dialog("Done")
descry.dialog("Reindex", "Scanned 142 files in 0.3s.")
```

There is no Cancel button and no return value — `dialog` is purely a
notification. For input use the two prompts below.

### `descry.confirm(title, message [, yes_label, no_label])`

Modal yes/no using the app's own confirm dialog. Returns `true` when the
user picks the affirmative button (or presses `Y`), `false` on `Esc` / the
safe button. Labels default to "Yes" / "No".

```lua
if descry.confirm("Reindex", "Rebuild the whole index? Takes a while.",
                  "Rebuild", "Cancel") then
    rebuild()
end
```

### `descry.prompt(title [, default [, description]])`

Modal single-line text input (the compact prompt the Plugins overlay uses
for config values). Returns the string on OK / Enter, or `nil` on cancel.
`default` pre-fills the field (selected, so typing replaces it);
`description` is a muted help line under the title.

```lua
local name = descry.prompt("New note title", "Untitled", "Used as the H1.")
if name then descry.buffer.insert("# " .. name .. "\n") end
```

### `descry.edit_mode()` / `descry.set_edit_mode([on])`

`edit_mode()` returns `true` while the editor pane is shown, `false` in
preview. `set_edit_mode` switches (see Navigation below).

### `descry.theme([slot])`

The current theme's colors as `{r, g, b}` tables (0–255), so decorations
can match the palette. With a slot name returns one color; with no
argument returns a table of every slot. Slots: `bg`, `fg`, `heading`,
`quote`, `link`, `code_bg`, `muted`, `sidebar_bg`, `sidebar_hover`,
`sidebar_active`, `status_bg`, `status_fg`, `selection`, `cursor`.

```lua
local link = descry.theme("link")           -- e.g. {122, 162, 247}
descry.decorations.add(0, 10, { underline = link })
```

### `descry.clipboard.get()` / `descry.clipboard.set(text)`

Read the system clipboard (string or `nil`) / replace it.

---

## `descry.buffer` — the open document

Read and edit the active note. All text is UTF-8; positions are **byte
offsets** into that UTF-8. Edits go through the real editor, so they land on
the undo stack (Ctrl+Z reverts a plugin edit) and the preview re-renders
immediately. The synthetic buffer behind an image preview is read-only — edit
calls on it are silently ignored.

| Call | Returns | Notes |
|------|---------|-------|
| `descry.buffer.text()` | string | the whole document |
| `descry.buffer.set_text(s)` | — | replace the whole document (undoable) |
| `descry.buffer.selection()` | string or `nil` | selected text, or nil if none |
| `descry.buffer.replace_selection(s)` | — | replace the selection (or insert at caret) |
| `descry.buffer.insert(s)` | — | insert `s` at the caret |
| `descry.buffer.cursor()` | integer | caret byte offset |
| `descry.buffer.set_cursor(pos)` | — | move the caret to byte `pos` (clamped) |
| `descry.buffer.length()` | integer | document length in bytes |
| `descry.buffer.line_count()` | integer | number of lines (1 + newlines) |
| `descry.buffer.path()` | string or `nil` | the note's file path, or nil if unsaved |
| `descry.buffer.selection_range()` | `lo, hi` or `nil` | byte range `[lo, hi)` of the selection |
| `descry.buffer.set_selection(lo, hi)` | — | select bytes `[lo, hi)`, caret at `hi` (clamped) |

```lua
-- Uppercase the current selection.
descry.register_action("upper_selection", function()
    local sel = descry.buffer.selection()
    if sel then descry.buffer.replace_selection(sel:upper()) end
end)
```

## `descry.vault` — the note collection

| Call | Returns | Notes |
|------|---------|-------|
| `descry.vault.list()` | array of strings | every note's path (`.md` / `.markdown` / `.txt`; folders and images excluded) |
| `descry.vault.dir()` | string or `nil` | the vault root folder |
| `descry.vault.refresh()` | — | rescan the folder so the sidebar picks up files you wrote with `io` (fires `vault_change`) |

```lua
descry.register_action("note_count", function()
    descry.notify(#descry.vault.list() .. " notes in the vault")
end)

-- Create a note on disk, then show + open it.
descry.register_action("scratch_note", function()
    local p = descry.vault.dir() .. "/scratch.md"
    local f = io.open(p, "wb"); if f then f:write("# scratch\n"); f:close() end
    descry.vault.refresh()
    descry.open(p)
end)
```

## Navigation: `descry.open` / `descry.save`

- **`descry.open(path)`** — open `path` in a tab (reusing an existing tab if it
  is already open). Because tabs keep each file's unsaved edits, this never
  loses work.
- **`descry.save()`** — save the active document to its file (a no-op for an
  unsaved scratch buffer with no path yet).
- **`descry.set_edit_mode([on])`** — switch to edit mode (no arg / `true`) or
  preview (`false`). Handy after a whole-buffer rewrite whose effect only shows
  in the source — e.g. line wrapping, which markdown preview reflows away.

## `descry.decorations` — styled text ranges

Push styled byte ranges and the renderer paints them in **both** the editor and
the preview. Ranges are buffer byte offsets — the same coordinate space as
`descry.buffer.*` (so a range computed from `descry.buffer.text()` "just
works"). The render path never calls back into Lua: you recompute the
decoration set in an event handler, and the engine applies it every frame.

| Call | Effect |
|------|--------|
| `descry.decorations.clear()` | drop all decorations |
| `descry.decorations.add(start, end, style)` | style bytes `[start, end)` |

`style` is a table; every field is optional:

```lua
{ fg = {r, g, b},          -- text color (0–255 each)
  bg = {r, g, b},          -- background highlight
  underline = {r, g, b} }  -- colored underline
```

Recompute on edits/opens so the styling tracks the text:

```lua
-- Highlight every line that contains "TODO".
local function refresh()
    descry.decorations.clear()
    local t, off = descry.buffer.text(), 0
    for line in (t .. "\n"):gmatch("(.-)\n") do
        if line:find("TODO", 1, true) then
            descry.decorations.add(off, off + #line, { bg = {80, 60, 0} })
        end
        off = off + #line + 1
    end
end
descry.on("open", refresh)
descry.on("text_change", refresh)
```

The bundled **`rainbow.lua`** is a fuller example: one `fg` decoration per line,
cycling the hue. Notes / limits: decorations are assumed non-overlapping (on
overlap, the one with the greatest `start ≤ offset` wins); ranges over a
`$…$` math span color the whole rendered span (math glyphs share one source
offset); recomputing thousands of decorations on every keystroke is fine for
normal notes but can lag on very large documents.

## Events: `descry.on(event, fn)`

Register a handler that fires on an editor lifecycle event. Handlers run with
no arguments — query `descry.buffer.*` for context. Register as many as you
like (per event, in order). Like actions, handlers are wiped and re-registered
on a plugin reload.

| Event | Fires when |
|-------|-----------|
| `"open"` | a note is loaded from disk into the active document |
| `"save"` | the active document is written to its file |
| `"text_change"` | the document's text changed this frame (typing, paste, undo, a plugin edit) |
| `"mode_change"` | the view switched between edit and preview (`descry.edit_mode()` tells which) |
| `"vault_change"` | the vault root changed or was rescanned (new/renamed/deleted files, `descry.vault.refresh()`) |
| `"config_change"` | a config value was changed from the Plugins overlay / settings modal / `descry.config_set` |

```lua
-- Stamp an updated-time into YAML frontmatter on every save.
descry.on("save", function()
    descry.notify("saved " .. (descry.buffer.path() or "scratch"))
end)
```

A handler that errors is logged to stderr (`[lua] event 'NAME': …`) and skipped;
the others still run, and the editor keeps going. Beware editing the buffer
from inside `text_change` — your edit triggers another `text_change`. It is
frame-throttled (not a hard loop), but guard against re-entrancy if your
handler writes to the buffer.

---

## Settings: `descry.config`

Plugins keep their settings in the app's `settings.lua` (under the
`plugins` table) and get a settings UI for free: every declared key shows in
the **Plugins overlay** (Ctrl+Alt+P) and in the per-plugin **settings**
modal (the `settings` chip on the plugin's row), with a type-aware editor —
a toggle for booleans, `‹ ›` cycling for choices, a validated prompt for
numbers and strings, `R` to reset, `Reset all` per plugin.

```lua
-- descry.config(key [, default [, opts]]) -> value
local width = descry.config("format.max_column", 80, {
    type = "number", min = 20, max = 400,
    desc = "Wrap paragraphs at this column",
})
local on = descry.config("rainbow.enabled", false,
    { type = "bool", desc = "Color every line of the note" })
local mode = descry.config("mytool.mode", "fast",
    { type = "choice", choices = { "fast", "careful" }, desc = "Trade-off" })
```

- **Keys** are `"<plugin>.<name>"` by convention; that prefix (or the plugin
  that declared the key at load time) is what groups keys per plugin in the
  UI.
- **Types**: `"string"` (default), `"number"`, `"bool"`, `"choice"`. Typed
  keys come back as Lua numbers / booleans; untyped keys stay strings (use
  `tonumber`). `min` / `max` bound numbers; `choices` lists the allowed
  values.
- **Declare at load time** (call `descry.config` at the top level of the
  file) so the overlay lists the key before the action ever runs. Re-reading
  the key later returns the live value, so read it inside the action rather
  than caching it.
- **`descry.config_set(key, value)`** writes a value (booleans and numbers
  are accepted), persists `settings.lua` and fires `config_change` so other
  handlers (and the plugin's own `refresh`) can react.

The bundled `format.lua` and `rainbow.lua` are complete examples.

---

## Discovery in the UI

Once plugins are loaded:

| What                          | How                                         |
|-------------------------------|---------------------------------------------|
| List of loaded plugins        | **Plugins overlay** — Ctrl+Alt+P            |
| Run an action by name         | **Command palette** — Ctrl+Shift+P, then type |
| Bind an action to a key       | F1 → scroll to your action → Enter to capture a keystroke |
| See load errors               | **Plugins overlay** shows the file in red with the Lua error |
| Reload after editing          | Plugins overlay → **Reload** button         |

Actions registered by plugins appear in the command palette with a
`Plugin` chip on the right so they're easy to distinguish from
built-ins.

---

## A complete example

```lua
-- data/plugins/word_count.lua
--
-- A real word counter: it reads the live document and keeps a status-bar
-- tally in sync as you type. (This file ships in data/plugins/.)

local function counts()
    local t = descry.buffer.text()
    local _, words = t:gsub("%S+", "")   -- runs of non-space = words
    return words, #t
end

descry.register_action("word_count", function()
    local words, chars = counts()
    descry.dialog("Word count",
        string.format("%d words\n%d characters", words, chars))
end)

descry.on("text_change", function()
    descry.notify("words: " .. (select(1, counts())))
end)

descry.notify("[word_count] loaded")
```

Drop that file in `data/plugins/`, restart Descry (or hit Reload in the
Plugins overlay). The `word_count` action shows up in the command palette;
bind it to e.g. `ctrl+shift+w` from the keybindings overlay (F1) and now
Ctrl+Shift+W pops the dialog — while the status bar already shows a live
count from the `text_change` handler.

---

## What's NOT exposed (yet)

Honest list of remaining holes — none are fundamental, just unwritten C
glue. If you need one, the codebase is small enough to add it in
`src/lua_host.c` against `DESCRY_LIB[]` / the `LuaAppBridge` vtable:

- **Custom UI**: no way to draw a panel, add a sidebar item, or render
  inside the preview pane. `dialog` / `notify` / `confirm` / `prompt` and
  decorations are the entire output surface.
- **Async / timers**: no `set_timeout`, no background work.
- **Direct keybind from Lua**: plugins register actions; users bind
  keys. There's no way for a plugin to claim a default keystroke.
- **Vault writes**: no create/rename/delete helper — use Lua's `io`, then
  `descry.vault.refresh()` and `descry.open`.

Buffer access, vault listing, file open/save, lifecycle events, typed
settings, prompts, inter-plugin `invoke`, theme and clipboard access —
all previously on this list — are now implemented (see the sections
above).

The host design is intentionally minimal until real use cases push for
more. If you write a plugin that wants any of the above, the right
answer is to add the C-side hook and document it here, not to work
around it in Lua.

---

## Config keys plugins commonly read

Use `descry.config` (see **Settings** above) for anything user-tunable —
it is persisted, editable from the UI, and typed. Values live in
`settings.lua` under the `plugins` table, keyed by the full
`"plugin.key"` string:

```lua
return {
    -- ... other keys
    plugins = {
        ["format.max_column"] = "100",
        ["rainbow.enabled"]   = "true",
    },
}
```

Plugins share the same Lua state as the config loader, but the config
table is stashed in the registry rather than exposed as a global, so
read app-level keys through the `descry.*` accessors rather than poking
at `init.lua` directly.

---

## Debugging a plugin

- Run Descry from a terminal — every `notify`, `dialog`, and load
  error mirrors to stderr with a `[notify]`, `[dialog]`, or `[plugin]`
  prefix.
- Lua `print` works and goes to the same stderr.
- Syntax errors show in the Plugins overlay (Ctrl+Alt+P) with the full
  Lua message — the file gets loaded but its actions don't register.
- Runtime errors inside an action surface as
  `[lua] action 'NAME': MESSAGE` on stderr. The action returns
  immediately; the rest of the app keeps running.

---

## Where it lives in the code

For anyone hacking on the plugin system itself:

- `src/lua_host.h` / `src/lua_host.c` — the host (state, plugin
  registry, the `descry.*` library, reload).
- `src/main.c` near `plugins_action_reload` and the Plugins overlay
  rendering — the UI side.
- `data/plugins/hello.lua` — the bundled example.

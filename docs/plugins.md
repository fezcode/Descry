# Plugins

Descry can load Lua scripts at startup and expose them as named **actions**
that the user can run from the command palette, bind to a key, or invoke
from the menus.

A plugin can register named actions, show toast notifications and modal
dialogs, **read and edit the open document**, **list and open vault notes**,
and **subscribe to lifecycle events** (`open` / `save` / `text_change`). The
surface is intentionally small but no longer a dead end — a plugin can do real
work on the text the user is looking at.

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

A handful of top-level functions plus two sub-tables (`descry.buffer`,
`descry.vault`).

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

### `descry.notify(message)`

Push a one-line toast onto the status bar. Use this for transient
"happened" feedback ("indexed 142 files", "selection cleaned"). Long
messages get clipped at the right edge — keep it under ~80 chars.

```lua
descry.notify("hello from a plugin")
```

`message` must be a string. The text also gets logged to stderr so
plugin authors running Descry from a terminal can see what fired.

### `descry.dialog(message)` or `descry.dialog(title, message)`

Pop a modal dialog with an OK button. Blocks the user until they
dismiss. Use this for output that demands attention (errors, results
worth reading) or when you want to confirm a step.

```lua
descry.dialog("Done")
descry.dialog("Reindex", "Scanned 142 files in 0.3s.")
```

There is no Cancel button and no return value — `dialog` is purely a
notification, not a prompt. If you need user input, you don't have it
yet; either fall back to `notify` for now or open an issue.

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
| `descry.buffer.path()` | string or `nil` | the note's file path, or nil if unsaved |

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
| `descry.vault.list()` | array of strings | every markdown note's path (folders/images excluded) |

```lua
descry.register_action("note_count", function()
    descry.notify(#descry.vault.list() .. " notes in the vault")
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
  inside the preview pane. `dialog`/`notify` is the entire output
  surface.
- **Inter-plugin calls**: no `descry.invoke("other_action")`.
- **Async / timers**: no `set_timeout`, no background work.
- **Direct keybind from Lua**: plugins register actions; users bind
  keys. There's no way for a plugin to claim a default keystroke.
- **Vault writes**: `descry.vault.list()` is read-only — there's no
  create/rename/delete helper (use Lua's `io` plus `descry.open`).

Buffer access, vault listing, file open/save, and lifecycle events —
all previously on this list — are now implemented (see the sections
above).

The host design is intentionally minimal until real use cases push for
more. If you write a plugin that wants any of the above, the right
answer is to add the C-side hook and document it here, not to work
around it in Lua.

---

## Config keys plugins commonly read

Plugins share the same Lua state as the config loader, so anything in
`init.lua` / `settings.lua` is reachable via standard Lua globals
**only inside the file's top-level body during load** — once the
config is stashed in the registry, the table is no longer the file's
return value.

If you need to read a config key from inside an action callback, expose
it as a top-level `local` at load time:

```lua
local my_cfg = (descry_cfg or {}).my_plugin or {}

descry.register_action("greet", function()
    local who = my_cfg.greet_target or "world"
    descry.notify("hello, " .. who)
end)
```

…and in `init.lua`:

```lua
return {
    -- ... other keys
    my_plugin = {
        greet_target = "descry user",
    },
}
```

(Note: `descry_cfg` isn't currently exposed as a global. This pattern
will work once that hook lands; for now use Lua's built-in `require`
for any plugin-private config.)

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

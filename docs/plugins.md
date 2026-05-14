# Plugins

Downsee can load Lua scripts at startup and expose them as named **actions**
that the user can run from the command palette, bind to a key, or invoke
from the menus.

This is a small, deliberately-narrow API today. A plugin can register
named actions, show toast notifications, and pop up modal dialogs.
There is no read/write access to the open document, no buffer
manipulation, no event subscription. If you need any of that, open
an issue or send a PR — most of it would be a few dozen lines of C
glue against `lua_host.c`.

---

## Where plugins live

By default Downsee scans `data/plugins/` next to the exe and loads every
`*.lua` file it finds (top level only — no recursion).

Override the directory with `plugin_path` in your `init.lua` or
`settings.lua`:

```lua
return {
    plugin_path = "C:/Users/me/downsee-plugins",
    -- ... other config keys
}
```

Hidden files (anything starting with `.`) are skipped. Files that fail to
parse don't break the boot — they're flagged in the **Plugins overlay**
(Ctrl+Alt+P) with the Lua error message.

---

## Lifecycle

1. App starts, reads `init.lua` then overlays `settings.lua`.
2. The `downsee` global is created (`notify`, `dialog`,
   `register_action`).
3. Every `*.lua` under the plugin dir is `dofile`'d, in directory-listing
   order. Each file's top-level body runs once. Anything you call at the
   top level (`downsee.notify(...)`) fires *during load*, before the
   editor window finishes drawing — these get queued and surface once the
   UI is up.
4. Actions registered via `downsee.register_action(name, fn)` get added
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

## The `downsee` global

Three functions. That's the whole API.

### `downsee.register_action(name, fn)`

Register a named action. `name` must be a string (lowercase, no spaces
recommended — the keybindings UI shows the raw name). `fn` is a Lua
function called with no arguments when the action is invoked.

```lua
downsee.register_action("uppercase_clipboard", function()
    -- ... your code here
end)
```

Calling `register_action` with a name that already exists overwrites
the previous binding. Action names that collide with built-ins (like
`save`, `quit`, `find`) are still accepted but the built-in always wins
when the user binds a key — your action will show in the command palette
but won't fire from the keystroke. This is deliberate; tell the user to
pick a different name if it matters.

### `downsee.notify(message)`

Push a one-line toast onto the status bar. Use this for transient
"happened" feedback ("indexed 142 files", "selection cleaned"). Long
messages get clipped at the right edge — keep it under ~80 chars.

```lua
downsee.notify("hello from a plugin")
```

`message` must be a string. The text also gets logged to stderr so
plugin authors running Downsee from a terminal can see what fired.

### `downsee.dialog(message)` or `downsee.dialog(title, message)`

Pop a modal dialog with an OK button. Blocks the user until they
dismiss. Use this for output that demands attention (errors, results
worth reading) or when you want to confirm a step.

```lua
downsee.dialog("Done")
downsee.dialog("Reindex", "Scanned 142 files in 0.3s.")
```

There is no Cancel button and no return value — `dialog` is purely a
notification, not a prompt. If you need user input, you don't have it
yet; either fall back to `notify` for now or open an issue.

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
-- Two actions: one shows a toast with a stub word count, the other
-- pops a dialog. Realistic plugins look like this — small, focused,
-- one file each.

local function fake_word_count()
    -- The plugin API doesn't expose the buffer yet; this is a
    -- placeholder. Replace with a real count once that hook lands.
    return 0
end

downsee.register_action("word_count_toast", function()
    downsee.notify("words: " .. fake_word_count())
end)

downsee.register_action("word_count_dialog", function()
    downsee.dialog("Word count",
        "This document has " .. fake_word_count() .. " words.")
end)

downsee.notify("[word_count] loaded")
```

Drop that file in `data/plugins/`, restart Downsee (or hit Reload in
the Plugins overlay). Both actions show up in the command palette.
Bind `word_count_toast` to e.g. `ctrl+shift+w` from the keybindings
overlay (F1) and now Ctrl+Shift+W fires it.

---

## What's NOT exposed (yet)

Honest list of holes — none of these are fundamental, just unwritten C
glue. If you need one, the codebase is small enough to add it in
`src/lua_host.c` against `DOWNSEE_LIB[]`:

- **Buffer access**: no read/write to the current document, no cursor
  position, no selection, no insert/delete.
- **File I/O scoped to vault**: plugins can use Lua's standard
  `io.open` if you want, but there's no helper for "open this note" or
  "list vault files".
- **Event subscription**: no `on_save`, `on_open`, `on_text_change`.
  Actions only fire when the user explicitly invokes them.
- **Custom UI**: no way to draw a panel, add a sidebar item, or render
  inside the preview pane. `dialog`/`notify` is the entire output
  surface.
- **Inter-plugin calls**: no `downsee.invoke("other_action")`.
- **Async / timers**: no `set_timeout`, no background work.
- **Direct keybind from Lua**: plugins register actions; users bind
  keys. There's no way for a plugin to claim a default keystroke.

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
local my_cfg = (downsee_cfg or {}).my_plugin or {}

downsee.register_action("greet", function()
    local who = my_cfg.greet_target or "world"
    downsee.notify("hello, " .. who)
end)
```

…and in `init.lua`:

```lua
return {
    -- ... other keys
    my_plugin = {
        greet_target = "downsee user",
    },
}
```

(Note: `downsee_cfg` isn't currently exposed as a global. This pattern
will work once that hook lands; for now use Lua's built-in `require`
for any plugin-private config.)

---

## Debugging a plugin

- Run Downsee from a terminal — every `notify`, `dialog`, and load
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
  registry, the `downsee.*` library, reload).
- `src/main.c` near `plugins_action_reload` and the Plugins overlay
  rendering — the UI side.
- `data/plugins/hello.lua` — the bundled example.

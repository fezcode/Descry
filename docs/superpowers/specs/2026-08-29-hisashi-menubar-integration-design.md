# Hisashi menubar integration (hoswl) — Design

**Date:** 2026-08-29
**Status:** Approved (brainstormed 2026-08-29); implementation in progress

## Problem

On Windows, Descry draws its own File / Edit / View / Help strip in the title
bar. Hisashi (the Fezcode top bar) is gaining a macOS-style global menubar
driven by a tiny named-pipe protocol called **hoswl** (Hisashi OS Window
Layer). Descry is the first app to integrate: when Hisashi is running and
the user has opted in, Descry's menus should appear in Hisashi's bar and the
in-app strip should get out of the way — the same thing v0.81.0 already does
on macOS with the native `NSMenu` mirror.

The counterpart spec (protocol, host service, widget) is
`Hisashi/docs/superpowers/specs/2026-08-29-os-window-layer-design.md`.

## Decisions (locked during brainstorming)

- Transport: named pipe `\\.\pipe\hoswl`, one JSON object per line.
- Descry's own strip auto-hides while connected **and** menubar export is
  on; it returns the instant the pipe drops. No third setting.
- Two settings rows (Windows only): **Hisashi integration** (connect to the
  layer at all; default Off) and **Hisashi menubar** (publish menus / the
  protocol's `enable` flag; default On).
- Client code is the shared single header `src/hoswl.h`, vendored verbatim
  from `Hisashi/sdk/hoswl/hoswl.h`. Descry must not need anything the
  header doesn't give every other C app.

## Goals / Non-goals

Goals: zero new link libraries; zero threads; the main loop never blocks on
the pipe; the same `MENU_TABLES` drive both the in-app strip and Hisashi
(no duplicated menu definitions); toggles show a checkmark in Hisashi;
Recent vaults is a real submenu.

Non-goals: publishing Lua plugin actions as menu items (they stay in the
command palette); Undo/Redo enabled-state patches (parity with the in-app
strip, which never greys them); macOS/Linux behaviour changes (all new code
is `#ifdef _WIN32`).

## Architecture

Mirror the macOS design one-for-one so the two platforms read the same:

| macOS (v0.81.0) | Windows (this spec) |
|---|---|
| `native_menu_install()` | `hoswl_menu_publish(App*)` |
| `native_menu_refresh_recents()` | (folded into `hoswl_menu_publish`, called from the same `recent_dirs_push` site) |
| `native_menu_flush()` in `main()` | `hoswl_menu_flush(App*)` in `main()` |
| `cfg_native_menubar` / `SET_NATIVE_MENU` | `cfg_hoswl` + `cfg_hoswl_menus` / `SET_HOSWL` + `SET_HOSWL_MENUS` |
| `menu_strip_visible()` → `!cfg_native_menubar` | `menu_strip_visible()` → `!hoswl_menus_live(a)` |

### Data model (`src/app.h`)

```c
bool cfg_hoswl;         /* Windows: connect to the Hisashi OS Window Layer */
bool cfg_hoswl_menus;   /* Windows: publish our menus to Hisashi's menubar */
```

Both exist on every platform so settings plumbing stays uniform; only the
Windows build shows rows for them or acts on them. The `hoswl_t` client
state is a `static` in `main.c` next to the other menu statics
(`g_hoswl`), plus `static uint32_t g_hoswl_fingerprint` for change
detection.

### Menu ids

Ids are positional so `MenuItem` needs no new field and the dispatch path is
the one macOS already uses:

- `m<menu>.<row>` — e.g. `m0.4` is File › Save. Resolved with
  `MENU_TABLES[m][r].fn`.
- `recent.<i>` — Recent vaults submenu entry → `submenu_invoke_row(a, i)`.

### Publishing (`hoswl_menu_publish`)

Builds the menu text DSL from `MENU_LABELS` / `MENU_TABLES` /
`menu_count_static()` and hands it to `hoswl_set_menus()`. Per row:
`m%d.%d|<label>|<shortcut>|<flags>`. Flags come from
`menu_row_checked(a, m, r)`, which maps the row's `fn` pointer to the
matching state:

| `fn` | state |
|---|---|
| `action_toggle_edit` | `a->edit_mode` |
| `action_toggle_sidebar` | `a->sidebar_open` |
| `action_toggle_wrap` | `a->cfg_edit_wrap` |
| `action_toggle_split` | `a->split_preview` |
| `action_outline_pin` | `a->outline_pinned` |
| `action_toggle_spellcheck` | `a->spellcheck_on` |

Rows that aren't in the map are plain (no flags). When
`app_recent_dirs_count() > 0`, a `recent|Recent vaults|>` submenu header is
appended to File with one `recent.<i>|<dir>` child per entry (label =
`vault_basename`, full path not sent).

A **fingerprint** (FNV-1a over the six booleans, `recent_dirs_count` and
each recent path) is computed every frame in `hoswl_menu_flush`; when it
differs from the last published value the whole menu is republished. Menus
are ~2 KB, so a full resend beats bookkeeping `set` patches.

### Per-frame flush (`hoswl_menu_flush`, called in `main()` after `fs_watch_poll`)

```c
#ifdef _WIN32
static void hoswl_menu_flush(App* a)
{
    if (!a->cfg_hoswl) { if (hoswl_connected(&g_hoswl)) hoswl_shutdown(&g_hoswl); return; }
    if (!g_hoswl.inited) hoswl_init(&g_hoswl, "com.fezcode.descry", "Descry", DESCRY_VERSION);
    bool was = hoswl_connected(&g_hoswl);
    const char* id;
    while ((id = hoswl_poll(&g_hoswl)) != NULL) hoswl_menu_dispatch(a, id);
    if (!was && hoswl_connected(&g_hoswl)) hoswl_set_enabled(&g_hoswl, a->cfg_hoswl_menus);
    uint32_t fp = hoswl_menu_fingerprint(a);
    if (fp != g_hoswl_fingerprint) { g_hoswl_fingerprint = fp; hoswl_menu_publish(a); }
}
#endif
```

`hoswl_menu_dispatch` parses `m%d.%d` / `recent.%d`, validates the range,
closes any open in-app dropdown, and calls the action — deferred to the main
loop exactly like the macOS pick queue, so actions that open modal pumps
are safe.

### Strip visibility

```c
static bool hoswl_menus_live(const App* a)   /* Windows; false elsewhere */
{ return a->cfg_hoswl && a->cfg_hoswl_menus && hoswl_connected(&g_hoswl); }

static bool menu_strip_visible(const App* a)
{
#if defined(__APPLE__)
    return !a->cfg_native_menubar;
#elif defined(_WIN32)
    return !hoswl_menus_live(a);
#else
    return true;
#endif
}
```

`render_titlebar`, `titlebar_menu_at` and `window_hit_test_cb` already
consult `menu_strip_visible` (v0.81.0), so hiding needs no further edits.
When the strip disappears while a dropdown is open, `hoswl_menu_flush`
closes it (`ctx_menu_close`, `menu_open = -1`).

### Settings

- Enum: `SET_HOSWL`, `SET_HOSWL_MENUS` under `#if defined(_WIN32)` right
  after `SET_EDIT_WRAP` (same slot the macOS row occupies on that platform).
- Labels: "Hisashi integration", "Hisashi menubar".
- `settings_value_str`: "On"/"Off". `settings_adjust`: toggle; turning
  integration off triggers the disconnect on the next flush; toggling menubar
  sends `enable` immediately when connected.
- `settings_persist`: `hoswl = true/false`, `hoswl_menus = true/false`.
- `app_init`: `cfg_hoswl = lua_host_cfg_number(lua, "hoswl", 0) != 0`,
  `cfg_hoswl_menus = lua_host_cfg_number(lua, "hoswl_menus", 1) != 0`.
- `SettingsSnapshot`: two more ints, diffed in the Save/Discard summary,
  restored on Discard — mirroring `native_menubar`.

### Shutdown

`app_shutdown`: `hoswl_shutdown(&g_hoswl)` first thing (sends `bye`), before
`settings_persist`.

## Edge cases

- Hisashi not running: `hoswl_poll` retries a connect every 2 s
  (`CreateFileW` fails instantly with `ERROR_FILE_NOT_FOUND`; no blocking).
  Strip stays visible.
- Hisashi quits mid-session: `PeekNamedPipe` reports `ERROR_BROKEN_PIPE`;
  the header marks disconnected; next frame the strip is back.
- Two Descry processes: each registers with its own pid; Hisashi shows the
  focused one's menus.
- Modal pumps (`confirm_action`, `info_modal`, …) block `main()`; clicks
  sent meanwhile sit in the pipe buffer and are dispatched after the modal
  returns. Acceptable for v1.
- Descry window not foreground: nothing to do — Hisashi decides what to
  show.

## Testing strategy

- `tests/test_hoswl.c`: DSL → JSON compilation (slug ids, escaping of `"`,
  `\`, newlines, non-ASCII passthrough), flags, submenu nesting, separator;
  `click` line scanning (`hoswl__parse_click`), oversize/garbage lines
  ignored; fingerprint stability. Pure C, no pipe.
- CMake: `option(DESCRY_TESTS "Build unit tests" OFF)` → `descry_tests`
  targets for `test_tabs` and `test_hoswl`, `enable_testing()` +
  `add_test`. `build.ps1 -Tests` passes the option and runs `ctest`.
- Manual: Hisashi with the Menubar widget on two monitors; toggle both rows
  live; kill/restart Hisashi; pick every menu row from the bar including a
  Recent vault; confirm modals open in front of Descry.

## Build sequence

1. Vendor `src/hoswl.h`; add `tests/test_hoswl.c` + CMake test option.
2. `app.h` fields; settings rows; persistence; snapshot.
3. `hoswl_menu_*` functions + `menu_strip_visible` branch + `main()` hook +
   shutdown.
4. `docs/plugins.md` no change (nothing exposed to Lua); README "What works"
   bullet; version 0.81.0 → **0.82.0** in `src/main.c`, `build_installer.ps1`,
   `forge.toml` (release only after the user has tested).

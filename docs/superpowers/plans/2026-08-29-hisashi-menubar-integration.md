# Hisashi Menubar Integration (hoswl) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** On Windows, publish Descry's File/Edit/View/Help menus to Hisashi's menubar over the hoswl named pipe and hide the in-app strip while connected.

**Architecture:** Vendor the single-header client `src/hoswl.h` from `Hisashi/sdk/hoswl/hoswl.h`. A small `hoswl_menu_*` block in `main.c` mirrors the existing macOS `native_menu_*` block: it compiles `MENU_TABLES` into the header's menu-text DSL, republishes when a fingerprint of toggle states + recent vaults changes, and dispatches `click` ids (`m<menu>.<row>`, `recent.<i>`) through the same function pointers the in-app dropdown uses. Two Windows-only settings rows control it.

**Tech Stack:** C11, SDL2, Win32 named pipes (kernel32 only — no new link libs), CMake/Ninja under MSYS2 MinGW-w64.

**Spec:** `docs/superpowers/specs/2026-08-29-hisashi-menubar-integration-design.md`

## Global Constraints

- All new code is `#ifdef _WIN32` (macOS/Linux builds unchanged); the `cfg_*` fields exist on every platform.
- No threads; the main loop never blocks on the pipe; one `hoswl_menu_flush(&app)` per frame.
- Settings keys `hoswl` (default false) and `hoswl_menus` (default true) must be written by `settings_persist` or they are lost on save.
- `src/hoswl.h` is a verbatim copy of `D:\Workhammer\Hisashi\sdk\hoswl\hoswl.h` — never edit it here; fix upstream and re-copy.
- Version 0.81.0 → **0.82.0** in `src/main.c:111`, `build_installer.ps1:3`, `forge.toml` (lines 3, 22, 23, 34, 179) in the final task; no installer build/release (user tests first).
- Commits never carry a `Co-Authored-By` trailer; message style `v0.82.0: <summary>` for the release commit.

---

### Task 1: Vendor `hoswl.h`, add the unit test and an opt-in CMake test target

**Files:**
- Create: `src/hoswl.h` (copy), `tests/test_hoswl.c` (copy of `Hisashi/sdk/hoswl/test_hoswl.c` with `#include "hoswl.h"` resolved via `-Isrc`)
- Modify: `CMakeLists.txt` (end of file), `build.ps1` (new `-Tests` switch)

- [ ] **Step 1: Copy** `Copy-Item D:\Workhammer\Hisashi\sdk\hoswl\hoswl.h src\hoswl.h; Copy-Item D:\Workhammer\Hisashi\sdk\hoswl\test_hoswl.c tests\test_hoswl.c`.

- [ ] **Step 2: CMake option** (append to `CMakeLists.txt`):

```cmake
option(DESCRY_TESTS "Build unit tests" OFF)
if(DESCRY_TESTS)
  enable_testing()
  add_executable(test_tabs tests/test_tabs.c src/tabs.c src/buffer.c)
  target_include_directories(test_tabs PRIVATE src)
  target_link_libraries(test_tabs PRIVATE PkgConfig::SDL2)
  add_test(NAME tabs COMMAND test_tabs)
  add_executable(test_hoswl tests/test_hoswl.c)
  target_include_directories(test_hoswl PRIVATE src)
  add_test(NAME hoswl COMMAND test_hoswl)
endif()
```

If `test_tabs.c` needs more sources to link (check the includes at its top), add them to that `add_executable`.

- [ ] **Step 3: `build.ps1 -Tests`** — add `[switch]$Tests` to `param(...)`; when set, pass `-DDESCRY_TESTS=ON` to the `cmake -G Ninja -B build` configure call (force a reconfigure when the flag differs from `build/CMakeCache.txt`'s `DESCRY_TESTS` value), and after the build run `ctest --test-dir build --output-on-failure` and fail the script on non-zero exit.

- [ ] **Step 4: Run** `.\build.ps1 -Tests` → `100% tests passed, 0 tests failed out of 2`.

- [ ] **Step 5: Commit** `git add src/hoswl.h tests/test_hoswl.c CMakeLists.txt build.ps1 && git commit -m "Vendor hoswl.h (Hisashi OS Window Layer client) + opt-in unit-test target"`.

---

### Task 2: Settings plumbing — `cfg_hoswl`, `cfg_hoswl_menus`, rows, persistence, snapshot

**Files:**
- Modify: `src/app.h` (after `cfg_native_menubar`, ~line 377), `src/main.c`: `app_init` cfg reads (~4036), `SettingsRow` enum (~10154), `SETTINGS_LABELS` (~10191), `settings_value_str` (~10253), `settings_adjust` (~10357), `settings_persist` (~10645), `SettingsSnapshot` struct/capture/diff/restore (~10763, 10786, 10866, 10902)

- [ ] **Step 1: Fields** (`app.h`, right after `cfg_native_menubar`):

```c
    /* Windows only: connect to the Hisashi OS Window Layer (hoswl) and, when
     * `cfg_hoswl_menus` is also on, publish File/Edit/View/Help to Hisashi's
     * menubar and drop the in-app strip while connected. Fields exist on every
     * platform so the settings plumbing stays uniform. */
    bool     cfg_hoswl;
    bool     cfg_hoswl_menus;
```

- [ ] **Step 2: Read at startup** (`app_init`, next to `native_menubar`):

```c
    a->cfg_hoswl       = lua_host_cfg_number(a->lua, "hoswl", 0) != 0;
    a->cfg_hoswl_menus = lua_host_cfg_number(a->lua, "hoswl_menus", 1) != 0;
```

- [ ] **Step 3: Rows.** In the enum, after the `#if defined(__APPLE__) SET_NATIVE_MENU #endif` block:

```c
#if defined(_WIN32)
    SET_HOSWL,          /* on/off: connect to the Hisashi OS Window Layer */
    SET_HOSWL_MENUS,    /* on/off: publish our menus to Hisashi's menubar */
#endif
```

Labels at the same position: `"Hisashi integration"`, `"Hisashi menubar"`. `settings_value_str`: both `"On"/"Off"`. `settings_adjust`:

```c
#if defined(_WIN32)
        case SET_HOSWL: {
            a->cfg_hoswl = !a->cfg_hoswl;      /* connect/disconnect happens in hoswl_menu_flush */
            break;
        }
        case SET_HOSWL_MENUS: {
            a->cfg_hoswl_menus = !a->cfg_hoswl_menus;
            hoswl_menu_set_enabled(a);         /* sends "enable" immediately when connected */
            a->menu_open  = -1;
            a->menu_hover = -1;
            break;
        }
#endif
```

(`hoswl_menu_set_enabled` is defined in Task 3; forward-declare it near line 128 beside `native_menu_*`.)

- [ ] **Step 4: Persist** (in `settings_persist`, next to the `native_menubar` line):

```c
    fprintf(f, "    hoswl = %s,\n",       a->cfg_hoswl       ? "true" : "false");
    fprintf(f, "    hoswl_menus = %s,\n", a->cfg_hoswl_menus ? "true" : "false");
```

- [ ] **Step 5: Snapshot** — add `int hoswl, hoswl_menus;` to `SettingsSnapshot`; capture both; in the diff summary add lines `"Hisashi integration: %s -> %s"` / `"Hisashi menubar: %s -> %s"` (same helper the `native_menubar` diff uses); restore both on Discard.

- [ ] **Step 6: Build** `.\build.ps1` → links (the forward-declared `hoswl_menu_set_enabled` needs a stub until Task 3 — add the real function in the same commit by doing Task 3 first, or temporarily define an empty `static void hoswl_menu_set_enabled(App* a) { (void)a; }`). Commit `git commit -m "Settings: Hisashi integration / Hisashi menubar rows (Windows)"`.

---

### Task 3: `hoswl_menu_*` block, strip visibility, main-loop hook, shutdown

**Files:**
- Modify: `src/main.c`: includes (~line 7: `#include "hoswl.h"` under `#ifdef _WIN32` with `#define HOSWL_IMPLEMENTATION` before it), forward decls (~128), `menu_strip_visible` (~6831), new block after the `#endif /* __APPLE__ */` at ~9012, `recent_dirs_push` (~10587), `app_shutdown` (~4559), `main()` (~19737)

- [ ] **Step 1: Include**

```c
#ifdef _WIN32
  #define HOSWL_IMPLEMENTATION
  #include "hoswl.h"
#endif
```

- [ ] **Step 2: Forward decls** (beside `native_menu_*`):

```c
#if defined(_WIN32)
static void hoswl_menu_flush(App* a);
static void hoswl_menu_set_enabled(App* a);
static void hoswl_menu_publish(App* a);
static bool hoswl_menus_live(const App* a);
#endif
```

- [ ] **Step 3: Strip visibility**

```c
static bool menu_strip_visible(const App* a)
{
#if defined(__APPLE__)
    return !a->cfg_native_menubar;
#elif defined(_WIN32)
    return !hoswl_menus_live(a);
#else
    (void)a;
    return true;
#endif
}
```

- [ ] **Step 4: The block** (after the macOS block):

```c
#if defined(_WIN32)
/* ------------------- Hisashi OS Window Layer (hoswl) --------------------
 * Windows twin of the macOS native menu bar above: the same MENU_TABLES are
 * published to Hisashi's menubar over a named pipe (see src/hoswl.h and
 * Hisashi/docs/hoswl-protocol.md). Ids are positional — "m<menu>.<row>" and
 * "recent.<i>" — so dispatch reuses the exact function pointers the in-app
 * dropdown calls. Clicks arrive on hoswl_poll() in the main loop, never from
 * inside a callback, so actions that pump their own modal loop are safe. */

static hoswl_t  g_hoswl;
static uint32_t g_hoswl_fingerprint;

static bool hoswl_menus_live(const App* a)
{
    return a->cfg_hoswl && a->cfg_hoswl_menus && hoswl_connected(&g_hoswl);
}

/* Which rows carry a checkmark in Hisashi, keyed by the row's action. */
static int hoswl_row_check(const App* a, int m, int r)
{
    void (*fn)(App*) = MENU_TABLES[m][r].fn;
    if (fn == action_toggle_edit)       return a->edit_mode      ? 1 : 0;
    if (fn == action_toggle_sidebar)    return a->sidebar_open   ? 1 : 0;
    if (fn == action_toggle_wrap)       return a->cfg_edit_wrap  ? 1 : 0;
    if (fn == action_toggle_split)      return a->split_preview  ? 1 : 0;
    if (fn == action_outline_pin)       return a->outline_pinned ? 1 : 0;
    if (fn == action_toggle_spellcheck) return a->spellcheck_on  ? 1 : 0;
    return -1;
}

static uint32_t hoswl_menu_fingerprint(const App* a)
{
    uint32_t h = 2166136261u;
    #define HOSWL_FNV(b) (h = (h ^ (uint32_t)(b)) * 16777619u)
    for (int m = 0; m < 4; ++m)
        for (int r = 0; r < menu_count_static(m); ++r) HOSWL_FNV(hoswl_row_check(a, m, r) + 2);
    int n = app_recent_dirs_count();
    HOSWL_FNV(n);
    for (int i = 0; i < n; ++i)
        for (const char* p = app_recent_dir_at(i); p && *p; ++p) HOSWL_FNV((unsigned char)*p);
    #undef HOSWL_FNV
    return h;
}

/* Strip a label's "…" is fine (UTF-8 passes through); just guard '|' and
 * newlines, which are the DSL's separators. */
static void hoswl_dsl_label(char* out, size_t cap, const char* s)
{
    size_t o = 0;
    for (; *s && o + 1 < cap; ++s) out[o++] = (*s == '|' || *s == '\n' || *s == '\r') ? ' ' : *s;
    out[o] = 0;
}

static void hoswl_menu_publish(App* a)
{
    static char text[HOSWL_MENU_MAX];
    size_t o = 0;
    #define HOSWL_PUT(...) do { int w_ = snprintf(text + o, sizeof text - o, __VA_ARGS__); \
                                if (w_ < 0 || (size_t)w_ >= sizeof text - o) goto too_big; o += (size_t)w_; } while (0)
    for (int m = 0; m < 4; ++m) {
        HOSWL_PUT("%s\n", MENU_LABELS[m]);
        int n = menu_count_static(m);
        for (int r = 0; r < n; ++r) {
            char label[128];
            hoswl_dsl_label(label, sizeof label, MENU_TABLES[m][r].label);
            const char* key = MENU_TABLES[m][r].shortcut ? MENU_TABLES[m][r].shortcut : "";
            int chk = hoswl_row_check(a, m, r);
            HOSWL_PUT(" m%d.%d|%s|%s|%s\n", m, r, label, key, chk < 0 ? "" : (chk ? "x" : "c"));
        }
        if (m == 0 && app_recent_dirs_count() > 0) {
            HOSWL_PUT(" -\n recent|Recent vaults|>\n");
            for (int i = 0; i < app_recent_dirs_count(); ++i) {
                char label[300];
                hoswl_dsl_label(label, sizeof label, app_recent_dir_at(i));
                HOSWL_PUT("  recent.%d|%s\n", i, label);
            }
        }
    }
    #undef HOSWL_PUT
    if (hoswl_set_menus(&g_hoswl, text) != 0)
        fprintf(stderr, "hoswl: menu rejected: %s\n", g_hoswl.last_error);
    return;
too_big:
    fprintf(stderr, "hoswl: menu text too large, not published\n");
}

static void hoswl_menu_set_enabled(App* a)
{
    if (g_hoswl.inited) hoswl_set_enabled(&g_hoswl, a->cfg_hoswl_menus ? 1 : 0);
}

static void hoswl_menu_dispatch(App* a, const char* id)
{
    int m = 0, r = 0;
    if (a->menu_open >= 0 || a->ctx_menu_active) ctx_menu_close(a);
    if (sscanf(id, "recent.%d", &r) == 1) { submenu_invoke_row(a, r); return; }
    if (sscanf(id, "m%d.%d", &m, &r) != 2) return;
    if (m < 0 || m >= 4 || r < 0 || r >= menu_count_static(m)) return;
    void (*fn)(App*) = MENU_TABLES[m][r].fn;
    if (fn) fn(a);
}

static void hoswl_menu_flush(App* a)
{
    if (!a->cfg_hoswl) {
        if (g_hoswl.inited && hoswl_connected(&g_hoswl)) hoswl_shutdown(&g_hoswl);
        g_hoswl_fingerprint = 0;
        return;
    }
    if (!g_hoswl.inited) {
        hoswl_init(&g_hoswl, "com.fezcode.descry", "Descry", DESCRY_VERSION);
        hoswl_set_enabled(&g_hoswl, a->cfg_hoswl_menus ? 1 : 0);
    }
    bool was = hoswl_connected(&g_hoswl) != 0;
    const char* id;
    while ((id = hoswl_poll(&g_hoswl)) != NULL) hoswl_menu_dispatch(a, id);
    uint32_t fp = hoswl_menu_fingerprint(a);
    if (fp != g_hoswl_fingerprint || (!was && hoswl_connected(&g_hoswl))) {
        g_hoswl_fingerprint = fp;
        hoswl_menu_publish(a);
    }
    /* The strip just disappeared under an open dropdown: close it. */
    if (hoswl_menus_live(a) && a->menu_open >= 0) { ctx_menu_close(a); a->menu_open = -1; a->menu_hover = -1; }
}
#endif  /* _WIN32 */
```

Note: `hoswl_shutdown` must reset `inited` to 0 so the next flush re-inits after the setting is turned back on — check the header; if it doesn't, set `g_hoswl.inited = 0` after calling it. Also `hoswl_init` on a struct that was previously used must fully reset it.

- [ ] **Step 5: Hooks**
- `main()`: after `fs_watch_poll(&app);` add
  ```c
  #if defined(_WIN32)
          hoswl_menu_flush(&app);    /* Hisashi menubar: connect, publish, dispatch clicks */
  #endif
  ```
- `app_shutdown`: first statement `#if defined(_WIN32) if (g_hoswl.inited) hoswl_shutdown(&g_hoswl); #endif`.
- `recent_dirs_push` (where `native_menu_refresh_recents(a)` is called): nothing extra — the fingerprint catches it next frame.

- [ ] **Step 6: Build + manual test**
1. `.\build.ps1 -Run`; Settings → set "Hisashi integration" On. With Hisashi running and the Menubar widget placed, focus Descry → the bar shows **Descry File Edit View Help** and the in-app strip is gone.
2. View → Toggle Word Wrap from the bar → checkmark flips, editor rewraps.
3. File → Recent vaults → pick one → vault opens.
4. Settings → "Hisashi menubar" Off → strip returns instantly, bar shows only **Descry**.
5. Quit Hisashi → strip returns within a frame; relaunch Hisashi → menus back within 2 s.
6. `descry.log` shows no `hoswl:` errors.

- [ ] **Step 7: Commit** `git commit -m "Hisashi menubar: publish File/Edit/View/Help over hoswl, hide the strip while connected"`.

---

### Task 4: README + version 0.82.0

- [ ] README "What works": bullet `- **Hisashi menubar** (Windows) — opt-in in Settings; File/Edit/View/Help move into [Hisashi](https://github.com/fezcode/hisashi)'s macOS-style menubar and the in-app strip hides while connected.`
- [ ] Version: `src/main.c:111` → `"0.82.0"`, `build_installer.ps1:3`, `forge.toml` five occurrences.
- [ ] `.\build.ps1 -Tests` green; commit `git commit -m "v0.82.0: Hisashi menubar integration (hoswl)"`. Stop for user testing before any installer/release.

---

## Self-review

- Spec coverage: vendoring + tests (T1), settings + persistence + snapshot (T2), publish/fingerprint/dispatch/strip/main-loop/shutdown (T3), README + version (T4). Modal-pump gap and Lua actions are documented non-goals.
- Type consistency: `hoswl_menu_set_enabled(App*)` is referenced from `settings_adjust` (T2) and defined in T3 — build T2+T3 together or stub as noted.

#ifndef DESCRY_LUA_HOST_H
#define DESCRY_LUA_HOST_H

#include <stddef.h>

typedef struct LuaHost LuaHost;

LuaHost* lua_host_create(void);
void     lua_host_destroy(LuaHost* h);

/* Run a Lua file and leave its returned table on the registry under the key
 * "descry.config" so config_get_* below can read from it. */
int lua_host_load_config(LuaHost* h, const char* path);

/* Run a Lua file and merge its returned table over the existing config —
 * keys present in the overlay overwrite the base; absent keys keep the
 * base value. Used for `settings.lua` overlay persistence. */
int lua_host_overlay_config(LuaHost* h, const char* path);

/* Read a string from the loaded config table (top-level key). Returned string
 * is owned by Lua; copy if you need to keep it past the next config reload. */
const char* lua_host_cfg_string(LuaHost* h, const char* key, const char* fallback);
double      lua_host_cfg_number(LuaHost* h, const char* key, double fallback);

/* Read 4-element [r,g,b,a] (each 0-255) from a top-level array key. Defaults
 * fill in any missing components. Always writes 4 bytes to out. */
void lua_host_cfg_color(LuaHost* h, const char* key,
                        unsigned char def_r, unsigned char def_g,
                        unsigned char def_b, unsigned char def_a,
                        unsigned char out[4]);

/* Top-level array helpers (Lua arrays are 1-indexed in the index argument). */
int         lua_host_cfg_array_length(LuaHost* h, const char* key);
const char* lua_host_cfg_array_string(LuaHost* h, const char* key, int i);

/* Look up a string under a top-level table-of-strings (e.g. keybindings). */
const char* lua_host_cfg_table_string(LuaHost* h, const char* key,
                                      const char* subkey);

/* Iterate every (string-key, string-value) pair in a top-level table.
 * Non-string entries are skipped. The strings handed to `cb` are owned by
 * Lua — callbacks must copy if they need to keep them. Returns 0 if the
 * table doesn't exist; otherwise the number of pairs visited. */
typedef void (*LuaTableEachCb)(const char* key, const char* value, void* ud);
int lua_host_each_in_table(LuaHost* h, const char* key,
                           LuaTableEachCb cb, void* ud);

/* Plugin host: register the `descry` global with notify/register_action,
 * load every *.lua under the given directory, and invoke a registered Lua
 * action by name. Returns count of plugins loaded, or 0 if dir missing. */
void lua_host_setup_api    (LuaHost* h);
int  lua_host_load_plugins (LuaHost* h, const char* dir);
int  lua_host_invoke_action(LuaHost* h, const char* action_name);

/* Visit every name in the plugin-registered actions table. Names are owned
 * by Lua — copy if you need to keep them past the callback. Returns the
 * number of actions visited. */
typedef void (*LuaActionEachCb)(const char* name, void* ud);
int lua_host_each_action(LuaHost* h, LuaActionEachCb cb, void* ud);

/* Per-plugin info delivered to the each-plugin callback. Pointers stay
 * valid until the next lua_host_load_plugins() call. */
typedef struct {
    const char*        name;
    const char*        path;
    const char* const* actions;
    int                action_count;
    int                load_failed;
    const char*        error;            /* empty when not load_failed */
    int                disabled;         /* 1 = user-disabled, not run */
} LuaPluginView;
typedef void (*LuaPluginEachCb)(const LuaPluginView* p, void* ud);
int lua_host_each_plugin(LuaHost* h, LuaPluginEachCb cb, void* ud);

/* Install a gate consulted while loading each plugin: cb(name, ud) returns 0 to
 * skip (disable) that plugin. It's still listed by lua_host_each_plugin with
 * disabled=1 so the UI can offer to re-enable it. Set before load/reload. */
void lua_host_set_plugin_filter(LuaHost* h,
                                int (*cb)(const char* name, void* ud), void* ud);

/* Re-scan + reload the plugin directory at runtime. Returns the count of
 * plugins that loaded without error. Old plugin records are freed first;
 * the Lua actions registry is reset before reload so removed plugins
 * disappear cleanly. */
int  lua_host_reload_plugins(LuaHost* h, const char* dir);

/* Name of the plugin whose file is currently being executed by
 * lua_host_load_plugins, or NULL outside a load. Lets the app attribute
 * config keys declared at load time to their plugin. */
const char* lua_host_current_plugin(LuaHost* h);

/* Register a callback invoked whenever a plugin calls descry.notify(s[, ms]).
 * Lets the app surface the notification in its UI. Single global slot.
 * `ms` is the requested duration, 0 = the app's default. */
typedef void (*LuaNotifyCallback)(void* userdata, const char* msg, int ms);
void lua_host_on_notify(LuaNotifyCallback cb, void* userdata);

/* Register a callback invoked when a plugin calls descry.dialog(title, msg).
 * Hosts open a modal dialog with an OK button. */
typedef void (*LuaDialogCallback)(void* userdata,
                                  const char* title, const char* msg);
void lua_host_on_dialog(LuaDialogCallback cb, void* userdata);

/* ---- Document / vault bridge ------------------------------------------
 * The plugin API exposes `descry.buffer.*` and `descry.vault.*`, but the
 * host knows nothing about the editor's App state. The app fills in this
 * vtable once at startup; the host's C functions call back through it.
 *
 * All text is UTF-8; positions are byte offsets into that UTF-8. Pointers
 * returned via `*_len` out-params are malloc'd by the app callback and freed
 * by the host after copying into Lua. Any field may be NULL — the host
 * guards every call, so a partially-filled bridge degrades gracefully. */
typedef struct {
    void* ud;                                                /* App* */
    char*  (*buf_text)       (void* ud, size_t* out_len);    /* whole doc; malloc'd */
    void   (*buf_set_text)   (void* ud, const char* s, size_t n);
    char*  (*buf_selection)  (void* ud, size_t* out_len);    /* NULL if no selection */
    void   (*buf_replace_sel)(void* ud, const char* s, size_t n);
    void   (*buf_insert)     (void* ud, const char* s, size_t n);
    size_t (*buf_cursor)     (void* ud);
    void   (*buf_set_cursor) (void* ud, size_t pos);
    size_t (*buf_len)        (void* ud);
    const char* (*note_path) (void* ud);                     /* NULL if unsaved */
    int    (*vault_count)    (void* ud);                     /* note files only */
    const char* (*vault_path)(void* ud, int i);
    void   (*open_path)      (void* ud, const char* path);
    void   (*save)           (void* ud);
    /* Text decorations. Colors are packed 0xRRGGBB, or -1 when that channel is
     * unset. Ranges are buffer byte offsets (same space as the buffer API). */
    void   (*decor_clear)    (void* ud);
    void   (*decor_add)      (void* ud, size_t start, size_t end,
                              long fg, long bg, long ul);
    /* Read a plugin config value (always a string) by key, with a default.
     * Reading a key also registers it so the Plugins overlay can list it. */
    const char* (*config_get)(void* ud, const char* key, const char* def);
    /* Switch view: on != 0 → edit mode, 0 → preview. No-op for image views. */
    void   (*set_edit_mode)  (void* ud, int on);

    /* ---- v0.83 additions (every field optional, host guards NULL) ---- */
    /* Write a config value (persisted by the app). */
    void   (*config_set)     (void* ud, const char* key, const char* val);
    /* Declare a config key's schema for the settings UI. `type` is one of
     * "string" / "number" / "bool" / "choice"; `choices` is '|'-separated
     * (choice only); min/max apply per has_range bits (1 = min, 2 = max). */
    void   (*config_declare) (void* ud, const char* key, const char* def,
                              const char* type, const char* desc,
                              const char* choices,
                              double min, double max, int has_range);
    /* Config type as declared: 0 string, 1 number, 2 bool, 3 choice. */
    int    (*config_type)    (void* ud, const char* key);
    /* Run a built-in OR plugin action by name. 0 on success. */
    int    (*invoke)         (void* ud, const char* action);
    /* Modal yes/no. Returns 1 for yes. Labels may be NULL. */
    int    (*confirm)        (void* ud, const char* title, const char* msg,
                              const char* yes, const char* no);
    /* Modal single-line text prompt. Returns 1 on OK (text in `out`). */
    int    (*prompt)         (void* ud, const char* title, const char* desc,
                              const char* def, char* out, size_t cap);
    const char* (*vault_dir) (void* ud);                     /* NULL if none */
    void   (*vault_refresh)  (void* ud);                     /* rescan sidebar */
    int    (*buf_sel_range)  (void* ud, size_t* lo, size_t* hi); /* 1 if any */
    void   (*buf_set_sel)    (void* ud, size_t lo, size_t hi);
    int    (*get_edit_mode)  (void* ud);                     /* 1 = edit */
    char*  (*clipboard_get)  (void* ud);                     /* malloc'd or NULL */
    void   (*clipboard_set)  (void* ud, const char* s);
    /* Theme color by slot name ("bg", "fg", "link", ...). 1 if found. */
    int    (*theme_color)    (void* ud, const char* name, unsigned char rgb[3]);
    /* Slot name for index i (NULL past the end) — lets Lua enumerate. */
    const char* (*theme_slot)(void* ud, int i);
    const char* version;                                     /* "0.83.0" */
} LuaAppBridge;

/* Install the document/vault bridge. Copies the struct; the app keeps no
 * ownership obligation. Call once after lua_host_setup_api(). */
void lua_host_set_bridge(LuaHost* h, const LuaAppBridge* bridge);

/* Invoke every handler a plugin registered with descry.on(event, fn), in
 * registration order. Safe (a no-op) when no handler is registered. Handlers
 * are called with no arguments — they query the descry.buffer and descry.vault
 * tables for context. Known events: "open", "save", "text_change",
 * "mode_change", "vault_change". A handler that errors is logged and
 * skipped; the rest still run. */
void lua_host_fire_event(LuaHost* h, const char* event);

/* Does any plugin have a handler for `event`? */
int  lua_host_has_event(LuaHost* h, const char* event);

#endif

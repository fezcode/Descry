#ifndef DESCRY_LUA_HOST_H
#define DESCRY_LUA_HOST_H

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
} LuaPluginView;
typedef void (*LuaPluginEachCb)(const LuaPluginView* p, void* ud);
int lua_host_each_plugin(LuaHost* h, LuaPluginEachCb cb, void* ud);

/* Re-scan + reload the plugin directory at runtime. Returns the count of
 * plugins that loaded without error. Old plugin records are freed first;
 * the Lua actions registry is reset before reload so removed plugins
 * disappear cleanly. */
int  lua_host_reload_plugins(LuaHost* h, const char* dir);

/* Register a callback invoked whenever a plugin calls descry.notify(s).
 * Lets the app surface the notification in its UI. Single global slot. */
typedef void (*LuaNotifyCallback)(void* userdata, const char* msg);
void lua_host_on_notify(LuaNotifyCallback cb, void* userdata);

/* Register a callback invoked when a plugin calls descry.dialog(title, msg).
 * Hosts open a modal dialog with an OK button. */
typedef void (*LuaDialogCallback)(void* userdata,
                                  const char* title, const char* msg);
void lua_host_on_dialog(LuaDialogCallback cb, void* userdata);

#endif

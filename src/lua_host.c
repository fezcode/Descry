#include "lua_host.h"

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CFG_KEY     "descry.config"
#define ACTIONS_KEY "descry.actions"
#define EVENTS_KEY  "descry.events"

/* Per-plugin record. Built as plugins are loaded so the host can later
 * report "this file registered these actions" without re-walking Lua. */
typedef struct {
    char*  name;          /* basename (e.g. "hello") */
    char*  path;          /* full file path          */
    char** actions;       /* strdup'd action names   */
    int    action_count;
    int    action_cap;
    int    load_failed;   /* 1 if luaL_dofile errored; actions[] is empty */
    char   error[256];    /* lua error message (best-effort)             */
    int    disabled;      /* 1 if skipped (user-disabled); not run        */
} PluginInfo;

struct LuaHost {
    lua_State*  L;
    PluginInfo* plugins;
    int         plugin_count;
    int         plugin_cap;
    int         current_plugin;   /* -1 outside a load, else plugins[i]  */
    LuaAppBridge bridge;          /* document/vault accessors (app-filled) */
    /* Optional gate: returns 0 to skip (disable) a plugin by name. */
    int        (*enabled_cb)(const char* name, void* ud);
    void*       enabled_ud;
};

/* The host pointer is stashed in the Lua extra-space so callbacks can find
 * it without globals. */
static void host_set_self(lua_State* L, LuaHost* h)
{
    *(LuaHost**)lua_getextraspace(L) = h;
}
static LuaHost* host_self(lua_State* L)
{
    return *(LuaHost**)lua_getextraspace(L);
}

LuaHost* lua_host_create(void)
{
    LuaHost* h = calloc(1, sizeof *h);
    h->L = luaL_newstate();
    h->current_plugin = -1;
    host_set_self(h->L, h);
    luaL_openlibs(h->L);
    return h;
}

static void plugins_free_all(LuaHost* h)
{
    for (int i = 0; i < h->plugin_count; ++i) {
        free(h->plugins[i].name);
        free(h->plugins[i].path);
        for (int j = 0; j < h->plugins[i].action_count; ++j)
            free(h->plugins[i].actions[j]);
        free(h->plugins[i].actions);
    }
    free(h->plugins);
    h->plugins = NULL;
    h->plugin_count = h->plugin_cap = 0;
}

void lua_host_destroy(LuaHost* h)
{
    if (!h) return;
    plugins_free_all(h);
    if (h->L) lua_close(h->L);
    free(h);
}

int lua_host_load_config(LuaHost* h, const char* path)
{
    if (luaL_dofile(h->L, path) != LUA_OK) {
        fprintf(stderr, "lua: %s\n", lua_tostring(h->L, -1));
        lua_pop(h->L, 1);
        return -1;
    }
    /* The config file should `return { ... }`. Stash that table. */
    if (!lua_istable(h->L, -1)) {
        fprintf(stderr, "lua: %s did not return a table\n", path);
        lua_pop(h->L, 1);
        return -1;
    }
    lua_setfield(h->L, LUA_REGISTRYINDEX, CFG_KEY);
    return 0;
}

int lua_host_each_in_table(LuaHost* h, const char* key,
                           LuaTableEachCb cb, void* ud)
{
    lua_getfield(h->L, LUA_REGISTRYINDEX, CFG_KEY);
    if (!lua_istable(h->L, -1)) { lua_pop(h->L, 1); return 0; }
    lua_getfield(h->L, -1, key);
    if (!lua_istable(h->L, -1)) { lua_pop(h->L, 2); return 0; }
    /* [cfg, table] */
    int n = 0;
    lua_pushnil(h->L);                          /* [cfg, table, nil] */
    while (lua_next(h->L, -2) != 0) {
        /* [cfg, table, key, value] */
        if (lua_type(h->L, -2) == LUA_TSTRING &&
            lua_type(h->L, -1) == LUA_TSTRING)
        {
            const char* k = lua_tostring(h->L, -2);
            const char* v = lua_tostring(h->L, -1);
            if (cb) cb(k, v, ud);
            n++;
        }
        lua_pop(h->L, 1);                       /* pop value, keep key */
    }
    lua_pop(h->L, 2);                           /* pop table + cfg */
    return n;
}

int lua_host_overlay_config(LuaHost* h, const char* path)
{
    /* Try to run the file. Missing file is a quiet no-op (NOT an error). */
    FILE* probe = fopen(path, "rb");
    if (!probe) return 0;
    fclose(probe);
    if (luaL_dofile(h->L, path) != LUA_OK) {
        fprintf(stderr, "lua: %s\n", lua_tostring(h->L, -1));
        lua_pop(h->L, 1);
        return -1;
    }
    if (!lua_istable(h->L, -1)) {
        fprintf(stderr, "lua: %s did not return a table\n", path);
        lua_pop(h->L, 1);
        return -1;
    }
    /* Stack: [overlay] */
    lua_getfield(h->L, LUA_REGISTRYINDEX, CFG_KEY);
    if (!lua_istable(h->L, -1)) {
        /* No base — promote overlay to be the base. */
        lua_pop(h->L, 1);                       /* pop the non-table */
        lua_setfield(h->L, LUA_REGISTRYINDEX, CFG_KEY);
        return 0;
    }
    /* Stack: [overlay, base]. Iterate overlay and write into base. */
    lua_pushnil(h->L);                          /* [overlay, base, nil] */
    while (lua_next(h->L, -3) != 0) {
        /* [overlay, base, key, value] */
        lua_pushvalue(h->L, -2);                /* [..., key]   */
        lua_pushvalue(h->L, -2);                /* [..., value] */
        lua_settable(h->L, -5);                 /* base[key] = value */
        lua_pop(h->L, 1);                       /* pop value, keep key */
    }
    lua_pop(h->L, 2);                           /* pop overlay + base */
    return 0;
}

static int push_cfg(lua_State* L, const char* key)
{
    lua_getfield(L, LUA_REGISTRYINDEX, CFG_KEY);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return 0; }
    lua_getfield(L, -1, key);
    lua_remove(L, -2); /* drop the cfg table; keep just the value */
    return 1;
}

const char* lua_host_cfg_string(LuaHost* h, const char* key, const char* fallback)
{
    if (!push_cfg(h->L, key)) return fallback;
    const char* s = lua_isstring(h->L, -1) ? lua_tostring(h->L, -1) : fallback;
    /* leave on stack — caller treats result as transient.  Lua keeps the
     * string alive as long as the registry holds the cfg table. */
    lua_pop(h->L, 1);
    return s;
}

double lua_host_cfg_number(LuaHost* h, const char* key, double fallback)
{
    if (!push_cfg(h->L, key)) return fallback;
    /* Accept Lua booleans too — `start_in_edit_mode = true` is more natural
     * to write than `= 1`, and the call sites use `!= 0` semantics anyway. */
    double v;
    if      (lua_isnumber (h->L, -1)) v = lua_tonumber(h->L, -1);
    else if (lua_isboolean(h->L, -1)) v = lua_toboolean(h->L, -1) ? 1.0 : 0.0;
    else                              v = fallback;
    lua_pop(h->L, 1);
    return v;
}

void lua_host_cfg_color(LuaHost* h, const char* key,
                        unsigned char dr, unsigned char dg,
                        unsigned char db, unsigned char da,
                        unsigned char out[4])
{
    out[0] = dr; out[1] = dg; out[2] = db; out[3] = da;
    if (!push_cfg(h->L, key)) return;
    if (!lua_istable(h->L, -1)) { lua_pop(h->L, 1); return; }

    for (int i = 0; i < 4; ++i) {
        lua_rawgeti(h->L, -1, i + 1);
        if (lua_isnumber(h->L, -1)) {
            int v = (int)lua_tointeger(h->L, -1);
            if (v < 0)   v = 0;
            if (v > 255) v = 255;
            out[i] = (unsigned char)v;
        }
        lua_pop(h->L, 1);
    }
    lua_pop(h->L, 1);
}

int lua_host_cfg_array_length(LuaHost* h, const char* key)
{
    if (!push_cfg(h->L, key)) return 0;
    if (!lua_istable(h->L, -1)) { lua_pop(h->L, 1); return 0; }
    int n = (int)lua_rawlen(h->L, -1);
    lua_pop(h->L, 1);
    return n;
}

const char* lua_host_cfg_array_string(LuaHost* h, const char* key, int i)
{
    if (!push_cfg(h->L, key)) return NULL;
    if (!lua_istable(h->L, -1)) { lua_pop(h->L, 1); return NULL; }
    lua_rawgeti(h->L, -1, i);
    const char* s = lua_isstring(h->L, -1) ? lua_tostring(h->L, -1) : NULL;
    lua_pop(h->L, 2);
    return s;
}

const char* lua_host_cfg_table_string(LuaHost* h, const char* key,
                                      const char* subkey)
{
    if (!push_cfg(h->L, key)) return NULL;
    if (!lua_istable(h->L, -1)) { lua_pop(h->L, 1); return NULL; }
    lua_getfield(h->L, -1, subkey);
    const char* s = lua_isstring(h->L, -1) ? lua_tostring(h->L, -1) : NULL;
    lua_pop(h->L, 2);
    return s;
}

/* ---------- plugin host ---------------------------------------------- */

static LuaNotifyCallback g_notify_cb = NULL;
static void*             g_notify_ud = NULL;

void lua_host_on_notify(LuaNotifyCallback cb, void* ud)
{
    g_notify_cb = cb;
    g_notify_ud = ud;
}

/* descry.notify(msg [, ms]) — toast; optional duration in milliseconds. */
static int l_notify(lua_State* L)
{
    const char* s  = luaL_checkstring(L, 1);
    int         ms = (int)luaL_optinteger(L, 2, 0);
    if (ms < 0) ms = 0;
    fprintf(stderr, "[notify] %s\n", s);
    if (g_notify_cb) g_notify_cb(g_notify_ud, s, ms);
    return 0;
}

/* descry.log(...) — stderr / log file only, no toast. Accepts any number of
 * values, tostring'd and tab-joined like print. */
static int l_log(lua_State* L)
{
    int n = lua_gettop(L);
    fputs("[plugin] ", stderr);
    for (int i = 1; i <= n; ++i) {
        size_t len;
        const char* s = luaL_tolstring(L, i, &len);
        if (i > 1) fputc('\t', stderr);
        fwrite(s, 1, len, stderr);
        lua_pop(L, 1);
    }
    fputc('\n', stderr);
    return 0;
}

const char* lua_host_current_plugin(LuaHost* h)
{
    if (!h || h->current_plugin < 0 || h->current_plugin >= h->plugin_count)
        return NULL;
    return h->plugins[h->current_plugin].name;
}

static LuaDialogCallback g_dialog_cb = NULL;
static void*             g_dialog_ud = NULL;

void lua_host_on_dialog(LuaDialogCallback cb, void* ud)
{
    g_dialog_cb = cb;
    g_dialog_ud = ud;
}

static int l_dialog(lua_State* L)
{
    /* Accept either dialog("msg") or dialog("title", "msg"). */
    const char* title = NULL;
    const char* msg   = NULL;
    if (lua_gettop(L) >= 2) {
        title = luaL_checkstring(L, 1);
        msg   = luaL_checkstring(L, 2);
    } else {
        msg = luaL_checkstring(L, 1);
    }
    fprintf(stderr, "[dialog] %s: %s\n", title ? title : "", msg ? msg : "");
    if (g_dialog_cb) g_dialog_cb(g_dialog_ud, title ? title : "Plugin", msg);
    return 0;
}

static int l_register_action(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    lua_getfield(L, LUA_REGISTRYINDEX, ACTIONS_KEY);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, ACTIONS_KEY);
    }
    lua_pushvalue(L, 2);             /* fn  */
    lua_setfield(L, -2, name);       /* actions[name] = fn */
    lua_pop(L, 1);

    /* Tag the action with the currently-loading plugin (if any) so the
     * plugins overlay can list "this file registered these actions". */
    LuaHost* h = host_self(L);
    if (h && h->current_plugin >= 0 && h->current_plugin < h->plugin_count) {
        PluginInfo* p = &h->plugins[h->current_plugin];
        if (p->action_count >= p->action_cap) {
            p->action_cap = p->action_cap ? p->action_cap * 2 : 4;
            p->actions = realloc(p->actions, p->action_cap * sizeof(char*));
        }
        p->actions[p->action_count++] = strdup(name);
    }
    return 0;
}

/* ---- document / vault bridge (descry.buffer.*, descry.vault.*) -------- */

void lua_host_set_bridge(LuaHost* h, const LuaAppBridge* b)
{
    if (!h || !b) return;
    h->bridge = *b;
}

void lua_host_set_plugin_filter(LuaHost* h,
                                int (*cb)(const char* name, void* ud), void* ud)
{
    if (!h) return;
    h->enabled_cb = cb;
    h->enabled_ud = ud;
}

static int l_buf_text(lua_State* L)
{
    LuaHost* h = host_self(L);
    if (h && h->bridge.buf_text) {
        size_t n = 0;
        char* s = h->bridge.buf_text(h->bridge.ud, &n);
        if (s) { lua_pushlstring(L, s, n); free(s); return 1; }
    }
    lua_pushliteral(L, "");
    return 1;
}

static int l_buf_set_text(lua_State* L)
{
    size_t n = 0;
    const char* s = luaL_checklstring(L, 1, &n);
    LuaHost* h = host_self(L);
    if (h && h->bridge.buf_set_text) h->bridge.buf_set_text(h->bridge.ud, s, n);
    return 0;
}

static int l_buf_selection(lua_State* L)
{
    LuaHost* h = host_self(L);
    if (h && h->bridge.buf_selection) {
        size_t n = 0;
        char* s = h->bridge.buf_selection(h->bridge.ud, &n);
        if (s) { lua_pushlstring(L, s, n); free(s); return 1; }
    }
    lua_pushnil(L);
    return 1;
}

static int l_buf_replace_sel(lua_State* L)
{
    size_t n = 0;
    const char* s = luaL_checklstring(L, 1, &n);
    LuaHost* h = host_self(L);
    if (h && h->bridge.buf_replace_sel) h->bridge.buf_replace_sel(h->bridge.ud, s, n);
    return 0;
}

static int l_buf_insert(lua_State* L)
{
    size_t n = 0;
    const char* s = luaL_checklstring(L, 1, &n);
    LuaHost* h = host_self(L);
    if (h && h->bridge.buf_insert) h->bridge.buf_insert(h->bridge.ud, s, n);
    return 0;
}

static int l_buf_cursor(lua_State* L)
{
    LuaHost* h = host_self(L);
    size_t pos = (h && h->bridge.buf_cursor) ? h->bridge.buf_cursor(h->bridge.ud) : 0;
    lua_pushinteger(L, (lua_Integer)pos);
    return 1;
}

static int l_buf_set_cursor(lua_State* L)
{
    lua_Integer pos = luaL_checkinteger(L, 1);
    if (pos < 0) pos = 0;
    LuaHost* h = host_self(L);
    if (h && h->bridge.buf_set_cursor) h->bridge.buf_set_cursor(h->bridge.ud, (size_t)pos);
    return 0;
}

static int l_buf_len(lua_State* L)
{
    LuaHost* h = host_self(L);
    size_t n = (h && h->bridge.buf_len) ? h->bridge.buf_len(h->bridge.ud) : 0;
    lua_pushinteger(L, (lua_Integer)n);
    return 1;
}

static int l_buf_path(lua_State* L)
{
    LuaHost* h = host_self(L);
    const char* p = (h && h->bridge.note_path) ? h->bridge.note_path(h->bridge.ud) : NULL;
    if (p) lua_pushstring(L, p); else lua_pushnil(L);
    return 1;
}

static int l_vault_list(lua_State* L)
{
    LuaHost* h = host_self(L);
    lua_newtable(L);
    if (h && h->bridge.vault_count && h->bridge.vault_path) {
        int n = h->bridge.vault_count(h->bridge.ud);
        for (int i = 0; i < n; ++i) {
            const char* p = h->bridge.vault_path(h->bridge.ud, i);
            if (!p) continue;
            lua_pushstring(L, p);
            lua_rawseti(L, -2, i + 1);
        }
    }
    return 1;
}

static int l_open(lua_State* L)
{
    const char* p = luaL_checkstring(L, 1);
    LuaHost* h = host_self(L);
    if (h && h->bridge.open_path) h->bridge.open_path(h->bridge.ud, p);
    return 0;
}

static int l_save(lua_State* L)
{
    LuaHost* h = host_self(L);
    if (h && h->bridge.save) h->bridge.save(h->bridge.ud);
    return 0;
}

/* descry.set_edit_mode([on]) — no/`nil` arg → edit; boolean sets explicitly. */
static int l_set_edit_mode(lua_State* L)
{
    int on = 1;
    if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) on = lua_toboolean(L, 1);
    LuaHost* h = host_self(L);
    if (h && h->bridge.set_edit_mode) h->bridge.set_edit_mode(h->bridge.ud, on);
    return 0;
}

/* Push a config value converted per its declared type: numbers come back
 * as Lua numbers, bools as booleans, everything else as the raw string. */
static void push_config_typed(lua_State* L, LuaHost* h, const char* key,
                              const char* v)
{
    int type = (h && h->bridge.config_type)
               ? h->bridge.config_type(h->bridge.ud, key) : 0;
    if (!v) v = "";
    if (type == 1) {
        lua_pushstring(L, v);
        if (lua_isnumber(L, -1)) {
            lua_Number n = lua_tonumber(L, -1);
            lua_pop(L, 1);
            lua_pushnumber(L, n);
        }
    } else if (type == 2) {
        int on = (strcmp(v, "true") == 0 || strcmp(v, "1") == 0 ||
                  strcmp(v, "on") == 0   || strcmp(v, "yes") == 0);
        lua_pushboolean(L, on);
    } else {
        lua_pushstring(L, v);
    }
}

/* Stringify a Lua value for the config store: booleans -> "true"/"false",
 * numbers -> shortest form, else tostring. */
static const char* config_value_string(lua_State* L, int idx, char* buf,
                                       size_t cap)
{
    switch (lua_type(L, idx)) {
        case LUA_TBOOLEAN:
            return lua_toboolean(L, idx) ? "true" : "false";
        case LUA_TNUMBER:
            if (lua_isinteger(L, idx))
                snprintf(buf, cap, "%lld", (long long)lua_tointeger(L, idx));
            else
                snprintf(buf, cap, "%.10g", lua_tonumber(L, idx));
            return buf;
        case LUA_TNIL:
            return "";
        default:
            return lua_tostring(L, idx) ? lua_tostring(L, idx) : "";
    }
}

/* descry.config(key [, default [, opts]]) -> value.
 * Reading a key registers it for the Plugins overlay's config list. `opts`
 * declares the key's schema for the settings UI:
 *   { type = "string"|"number"|"bool"|"choice", desc = "...",
 *     choices = {"a","b"}, min = 0, max = 100 }
 * Typed keys return Lua numbers / booleans; untyped keys return strings. */
static int l_config(lua_State* L)
{
    const char* key = luaL_checkstring(L, 1);
    char defbuf[64];
    const char* def = (lua_gettop(L) >= 2)
                      ? config_value_string(L, 2, defbuf, sizeof defbuf) : "";
    LuaHost* h = host_self(L);
    if (lua_gettop(L) >= 3 && lua_istable(L, 3) && h && h->bridge.config_declare) {
        const char* type = "string";
        const char* desc = "";
        char choices[256] = {0};
        double mn = 0, mx = 0; int has_range = 0;
        lua_getfield(L, 3, "type");
        if (lua_isstring(L, -1)) type = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "desc");
        if (lua_isstring(L, -1)) desc = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "choices");
        if (lua_istable(L, -1)) {
            int n = (int)lua_rawlen(L, -1);
            size_t used = 0;
            for (int i = 1; i <= n; ++i) {
                lua_rawgeti(L, -1, i);
                const char* c = lua_tostring(L, -1);
                if (c && used < sizeof choices - 1) {
                    int w = snprintf(choices + used, sizeof choices - used,
                                     "%s%s", used ? "|" : "", c);
                    if (w > 0) used += (size_t)w;
                    if (used > sizeof choices - 1) used = sizeof choices - 1;
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        lua_getfield(L, 3, "min");
        if (lua_isnumber(L, -1)) { mn = lua_tonumber(L, -1); has_range |= 1; }
        lua_pop(L, 1);
        lua_getfield(L, 3, "max");
        if (lua_isnumber(L, -1)) { mx = lua_tonumber(L, -1); has_range |= 2; }
        lua_pop(L, 1);
        h->bridge.config_declare(h->bridge.ud, key, def, type, desc,
                                 choices, mn, mx, has_range);
    }
    const char* v = (h && h->bridge.config_get)
                    ? h->bridge.config_get(h->bridge.ud, key, def) : def;
    push_config_typed(L, h, key, v);
    return 1;
}

/* descry.config_set(key, value) — write + persist a config value. */
static int l_config_set(lua_State* L)
{
    const char* key = luaL_checkstring(L, 1);
    char buf[64];
    const char* v = config_value_string(L, 2, buf, sizeof buf);
    LuaHost* h = host_self(L);
    if (h && h->bridge.config_set) h->bridge.config_set(h->bridge.ud, key, v);
    return 0;
}

/* descry.invoke(action) -> bool. Runs a built-in or plugin action by name. */
static int l_invoke(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);
    LuaHost* h = host_self(L);
    int rc = (h && h->bridge.invoke) ? h->bridge.invoke(h->bridge.ud, name) : -1;
    lua_pushboolean(L, rc == 0);
    return 1;
}

/* descry.confirm(title, msg [, yes_label, no_label]) -> bool */
static int l_confirm(lua_State* L)
{
    const char* title = luaL_checkstring(L, 1);
    const char* msg   = luaL_optstring(L, 2, "");
    const char* yes   = luaL_optstring(L, 3, NULL);
    const char* no    = luaL_optstring(L, 4, NULL);
    LuaHost* h = host_self(L);
    int ok = (h && h->bridge.confirm)
             ? h->bridge.confirm(h->bridge.ud, title, msg, yes, no) : 0;
    lua_pushboolean(L, ok);
    return 1;
}

/* descry.prompt(title [, default [, description]]) -> string | nil */
static int l_prompt(lua_State* L)
{
    const char* title = luaL_checkstring(L, 1);
    const char* def   = luaL_optstring(L, 2, "");
    const char* desc  = luaL_optstring(L, 3, "");
    LuaHost* h = host_self(L);
    char out[1024];
    int ok = (h && h->bridge.prompt)
             ? h->bridge.prompt(h->bridge.ud, title, desc, def, out, sizeof out)
             : 0;
    if (ok) lua_pushstring(L, out); else lua_pushnil(L);
    return 1;
}

/* descry.edit_mode() -> bool (true while the editor pane is shown). */
static int l_get_edit_mode(lua_State* L)
{
    LuaHost* h = host_self(L);
    int on = (h && h->bridge.get_edit_mode)
             ? h->bridge.get_edit_mode(h->bridge.ud) : 0;
    lua_pushboolean(L, on);
    return 1;
}

/* descry.theme([slot]) -> {r,g,b} for one slot, or a table of every slot. */
static int l_theme(lua_State* L)
{
    LuaHost* h = host_self(L);
    if (!h || !h->bridge.theme_color) { lua_pushnil(L); return 1; }
    unsigned char rgb[3];
    if (lua_gettop(L) >= 1 && lua_isstring(L, 1)) {
        if (!h->bridge.theme_color(h->bridge.ud, lua_tostring(L, 1), rgb)) {
            lua_pushnil(L);
            return 1;
        }
        lua_createtable(L, 3, 0);
        for (int i = 0; i < 3; ++i) {
            lua_pushinteger(L, rgb[i]);
            lua_rawseti(L, -2, i + 1);
        }
        return 1;
    }
    lua_newtable(L);
    if (h->bridge.theme_slot) {
        for (int i = 0; ; ++i) {
            const char* slot = h->bridge.theme_slot(h->bridge.ud, i);
            if (!slot) break;
            if (!h->bridge.theme_color(h->bridge.ud, slot, rgb)) continue;
            lua_createtable(L, 3, 0);
            for (int k = 0; k < 3; ++k) {
                lua_pushinteger(L, rgb[k]);
                lua_rawseti(L, -2, k + 1);
            }
            lua_setfield(L, -2, slot);
        }
    }
    return 1;
}

static int l_vault_dir(lua_State* L)
{
    LuaHost* h = host_self(L);
    const char* d = (h && h->bridge.vault_dir) ? h->bridge.vault_dir(h->bridge.ud) : NULL;
    if (d && *d) lua_pushstring(L, d); else lua_pushnil(L);
    return 1;
}

static int l_vault_refresh(lua_State* L)
{
    LuaHost* h = host_self(L);
    if (h && h->bridge.vault_refresh) h->bridge.vault_refresh(h->bridge.ud);
    return 0;
}

/* descry.buffer.selection_range() -> lo, hi  (byte offsets) or nil */
static int l_buf_sel_range(lua_State* L)
{
    LuaHost* h = host_self(L);
    size_t lo = 0, hi = 0;
    if (h && h->bridge.buf_sel_range &&
        h->bridge.buf_sel_range(h->bridge.ud, &lo, &hi)) {
        lua_pushinteger(L, (lua_Integer)lo);
        lua_pushinteger(L, (lua_Integer)hi);
        return 2;
    }
    lua_pushnil(L);
    return 1;
}

/* descry.buffer.set_selection(lo, hi) — select bytes [lo, hi); caret at hi. */
static int l_buf_set_sel(lua_State* L)
{
    lua_Integer lo = luaL_checkinteger(L, 1);
    lua_Integer hi = luaL_checkinteger(L, 2);
    if (lo < 0) lo = 0;
    if (hi < lo) hi = lo;
    LuaHost* h = host_self(L);
    if (h && h->bridge.buf_set_sel)
        h->bridge.buf_set_sel(h->bridge.ud, (size_t)lo, (size_t)hi);
    return 0;
}

/* descry.buffer.line_count() -> number of lines (1 + newlines). */
static int l_buf_line_count(lua_State* L)
{
    LuaHost* h = host_self(L);
    lua_Integer lines = 1;
    if (h && h->bridge.buf_text) {
        size_t n = 0;
        char* s = h->bridge.buf_text(h->bridge.ud, &n);
        if (s) {
            for (size_t i = 0; i < n; ++i) if (s[i] == '\n') lines++;
            free(s);
        }
    }
    lua_pushinteger(L, lines);
    return 1;
}

static int l_clip_get(lua_State* L)
{
    LuaHost* h = host_self(L);
    char* s = (h && h->bridge.clipboard_get) ? h->bridge.clipboard_get(h->bridge.ud) : NULL;
    if (s) { lua_pushstring(L, s); free(s); } else lua_pushnil(L);
    return 1;
}

static int l_clip_set(lua_State* L)
{
    const char* s = luaL_checkstring(L, 1);
    LuaHost* h = host_self(L);
    if (h && h->bridge.clipboard_set) h->bridge.clipboard_set(h->bridge.ud, s);
    return 0;
}

/* ---- text decorations: descry.decorations.clear/add ------------------- */

static int l_decor_clear(lua_State* L)
{
    LuaHost* h = host_self(L);
    if (h && h->bridge.decor_clear) h->bridge.decor_clear(h->bridge.ud);
    return 0;
}

/* Read style[key] = {r,g,b} into a packed 0xRRGGBB, or -1 if absent. */
static long decor_color_field(lua_State* L, int style_idx, const char* key)
{
    if (!lua_istable(L, style_idx)) return -1;
    lua_getfield(L, style_idx, key);          /* style[key] */
    long rgb = -1;
    if (lua_istable(L, -1)) {
        int comp[3] = { 0, 0, 0 };
        for (int i = 0; i < 3; ++i) {
            lua_rawgeti(L, -1, i + 1);
            int v = (int)luaL_optinteger(L, -1, 0);
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            comp[i] = v;
            lua_pop(L, 1);
        }
        rgb = ((long)comp[0] << 16) | ((long)comp[1] << 8) | (long)comp[2];
    }
    lua_pop(L, 1);                             /* style[key] */
    return rgb;
}

/* descry.decorations.add(start, end, { fg={r,g,b}, bg=…, underline=… }) */
static int l_decor_add(lua_State* L)
{
    lua_Integer start = luaL_checkinteger(L, 1);
    lua_Integer end   = luaL_checkinteger(L, 2);
    long fg = decor_color_field(L, 3, "fg");
    long bg = decor_color_field(L, 3, "bg");
    long ul = decor_color_field(L, 3, "underline");
    if (start < 0) start = 0;
    if (end <= start) return 0;
    LuaHost* h = host_self(L);
    if (h && h->bridge.decor_add)
        h->bridge.decor_add(h->bridge.ud, (size_t)start, (size_t)end, fg, bg, ul);
    return 0;
}

/* ---- events: descry.on(event, fn) ------------------------------------- */

static int l_on(lua_State* L)
{
    const char* ev = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_getfield(L, LUA_REGISTRYINDEX, EVENTS_KEY);   /* [events] */
    if (!lua_istable(L, -1)) {                        /* defensive: recreate */
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, EVENTS_KEY);
    }
    lua_getfield(L, -1, ev);                          /* [events, events[ev]] */
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, ev);                      /* events[ev] = {} */
    }
    lua_Integer len = (lua_Integer)lua_rawlen(L, -1);
    lua_pushvalue(L, 2);                              /* the fn */
    lua_rawseti(L, -2, len + 1);                      /* events[ev][#+1] = fn */
    lua_pop(L, 2);                                    /* events[ev], events */
    return 0;
}

int lua_host_has_event(LuaHost* h, const char* event)
{
    if (!h || !event) return 0;
    lua_State* L = h->L;
    lua_getfield(L, LUA_REGISTRYINDEX, EVENTS_KEY);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return 0; }
    lua_getfield(L, -1, event);
    int n = lua_istable(L, -1) ? (int)lua_rawlen(L, -1) : 0;
    lua_pop(L, 2);
    return n > 0;
}

void lua_host_fire_event(LuaHost* h, const char* event)
{
    if (!h || !event) return;
    lua_State* L = h->L;
    lua_getfield(L, LUA_REGISTRYINDEX, EVENTS_KEY);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    lua_getfield(L, -1, event);
    if (!lua_istable(L, -1)) { lua_pop(L, 2); return; }
    int n = (int)lua_rawlen(L, -1);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, -1, i);                        /* fn */
        if (lua_isfunction(L, -1)) {
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                fprintf(stderr, "[lua] event '%s': %s\n",
                        event, lua_tostring(L, -1));
                lua_pop(L, 1);                         /* error message */
            }
        } else {
            lua_pop(L, 1);                             /* non-function entry */
        }
    }
    lua_pop(L, 2);                                    /* event array + events */
}

static const luaL_Reg DESCRY_LIB[] = {
    { "notify",          l_notify },
    { "log",             l_log },
    { "dialog",          l_dialog },
    { "confirm",         l_confirm },
    { "prompt",          l_prompt },
    { "register_action", l_register_action },
    { "invoke",          l_invoke },
    { "on",              l_on },
    { "open",            l_open },
    { "save",            l_save },
    { "config",          l_config },
    { "config_set",      l_config_set },
    { "set_edit_mode",   l_set_edit_mode },
    { "edit_mode",       l_get_edit_mode },
    { "theme",           l_theme },
    { NULL, NULL },
};

static const luaL_Reg DESCRY_CLIP_LIB[] = {
    { "get", l_clip_get },
    { "set", l_clip_set },
    { NULL, NULL },
};

static const luaL_Reg DESCRY_DECOR_LIB[] = {
    { "clear", l_decor_clear },
    { "add",   l_decor_add },
    { NULL, NULL },
};

static const luaL_Reg DESCRY_BUFFER_LIB[] = {
    { "text",              l_buf_text },
    { "set_text",          l_buf_set_text },
    { "selection",         l_buf_selection },
    { "replace_selection", l_buf_replace_sel },
    { "insert",            l_buf_insert },
    { "cursor",            l_buf_cursor },
    { "set_cursor",        l_buf_set_cursor },
    { "length",            l_buf_len },
    { "path",              l_buf_path },
    { "selection_range",   l_buf_sel_range },
    { "set_selection",     l_buf_set_sel },
    { "line_count",        l_buf_line_count },
    { NULL, NULL },
};

static const luaL_Reg DESCRY_VAULT_LIB[] = {
    { "list",    l_vault_list },
    { "dir",     l_vault_dir },
    { "refresh", l_vault_refresh },
    { NULL, NULL },
};

void lua_host_setup_api(LuaHost* h)
{
    luaL_newlib(h->L, DESCRY_LIB);                /* [descry] */
    luaL_newlib(h->L, DESCRY_BUFFER_LIB);         /* [descry, buffer] */
    lua_setfield(h->L, -2, "buffer");             /* descry.buffer = … */
    luaL_newlib(h->L, DESCRY_VAULT_LIB);          /* [descry, vault] */
    lua_setfield(h->L, -2, "vault");              /* descry.vault = … */
    luaL_newlib(h->L, DESCRY_DECOR_LIB);          /* [descry, decorations] */
    lua_setfield(h->L, -2, "decorations");        /* descry.decorations = … */
    luaL_newlib(h->L, DESCRY_CLIP_LIB);           /* [descry, clipboard] */
    lua_setfield(h->L, -2, "clipboard");          /* descry.clipboard = … */
    lua_pushstring(h->L, h->bridge.version ? h->bridge.version : "dev");
    lua_setfield(h->L, -2, "version");            /* descry.version */
    lua_setglobal(h->L, "descry");
    /* pre-create the actions + events registries so plugins don't need to */
    lua_newtable(h->L);
    lua_setfield(h->L, LUA_REGISTRYINDEX, ACTIONS_KEY);
    lua_newtable(h->L);
    lua_setfield(h->L, LUA_REGISTRYINDEX, EVENTS_KEY);
}

int lua_host_load_plugins(LuaHost* h, const char* dir)
{
    /* Clear any previous run so a reload doesn't double-count. */
    plugins_free_all(h);

    DIR* d = opendir(dir);
    if (!d) return 0;
    int loaded = 0;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        size_t n = strlen(ent->d_name);
        if (n < 4 || strcmp(ent->d_name + n - 4, ".lua") != 0) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, ent->d_name);

        /* Reserve a PluginInfo slot and mark it current so register_action
         * calls inside this file get attributed correctly. */
        if (h->plugin_count >= h->plugin_cap) {
            h->plugin_cap = h->plugin_cap ? h->plugin_cap * 2 : 8;
            h->plugins = realloc(h->plugins,
                                 h->plugin_cap * sizeof(PluginInfo));
        }
        PluginInfo* p = &h->plugins[h->plugin_count];
        memset(p, 0, sizeof *p);
        /* Strip ".lua" for the display name. */
        char namebuf[256];
        snprintf(namebuf, sizeof namebuf, "%.*s", (int)(n - 4), ent->d_name);
        p->name = strdup(namebuf);
        p->path = strdup(path);
        h->current_plugin = h->plugin_count;
        h->plugin_count++;

        if (h->enabled_cb && !h->enabled_cb(namebuf, h->enabled_ud)) {
            /* Disabled by the host — listed in the overlay but not run, so its
             * actions/events never register. */
            p->disabled = 1;
        } else if (luaL_dofile(h->L, path) != LUA_OK) {
            const char* msg = lua_tostring(h->L, -1);
            fprintf(stderr, "[plugin] %s: %s\n", path, msg ? msg : "?");
            p->load_failed = 1;
            if (msg) snprintf(p->error, sizeof p->error, "%s", msg);
            lua_pop(h->L, 1);
        } else {
            loaded++;
        }
        h->current_plugin = -1;
    }
    closedir(d);
    return loaded;
}

int lua_host_invoke_action(LuaHost* h, const char* name)
{
    lua_State* L = h->L;
    lua_getfield(L, LUA_REGISTRYINDEX, ACTIONS_KEY);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return -1; }
    lua_getfield(L, -1, name);
    lua_remove(L, -2);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return -1; }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        fprintf(stderr, "[lua] action '%s': %s\n", name, lua_tostring(L, -1));
        lua_pop(L, 1);
        return -1;
    }
    return 0;
}

int lua_host_each_plugin(LuaHost* h, LuaPluginEachCb cb, void* ud)
{
    if (!h || !cb) return 0;
    for (int i = 0; i < h->plugin_count; ++i) {
        PluginInfo* p = &h->plugins[i];
        LuaPluginView v = {
            .name         = p->name,
            .path         = p->path,
            .actions      = (const char* const*)p->actions,
            .action_count = p->action_count,
            .load_failed  = p->load_failed,
            .error        = p->error,
            .disabled     = p->disabled,
        };
        cb(&v, ud);
    }
    return h->plugin_count;
}

int lua_host_reload_plugins(LuaHost* h, const char* dir)
{
    if (!h) return 0;
    /* Reset the actions + events tables so removed plugins' actions and
     * event handlers disappear; survivors re-register on their re-run. */
    lua_newtable(h->L);
    lua_setfield(h->L, LUA_REGISTRYINDEX, ACTIONS_KEY);
    lua_newtable(h->L);
    lua_setfield(h->L, LUA_REGISTRYINDEX, EVENTS_KEY);
    return lua_host_load_plugins(h, dir);
}

int lua_host_each_action(LuaHost* h, LuaActionEachCb cb, void* ud)
{
    if (!h || !cb) return 0;
    lua_State* L = h->L;
    lua_getfield(L, LUA_REGISTRYINDEX, ACTIONS_KEY);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return 0; }
    int n = 0;
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            cb(lua_tostring(L, -2), ud);
            n++;
        }
        lua_pop(L, 1);   /* value; leave key for next iteration */
    }
    lua_pop(L, 1);       /* the actions table */
    return n;
}

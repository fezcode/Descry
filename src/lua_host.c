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
} PluginInfo;

struct LuaHost {
    lua_State*  L;
    PluginInfo* plugins;
    int         plugin_count;
    int         plugin_cap;
    int         current_plugin;   /* -1 outside a load, else plugins[i]  */
    LuaAppBridge bridge;          /* document/vault accessors (app-filled) */
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

static int l_notify(lua_State* L)
{
    const char* s = luaL_checkstring(L, 1);
    fprintf(stderr, "[notify] %s\n", s);
    if (g_notify_cb) g_notify_cb(g_notify_ud, s);
    return 0;
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
    { "dialog",          l_dialog },
    { "register_action", l_register_action },
    { "on",              l_on },
    { "open",            l_open },
    { "save",            l_save },
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
    { NULL, NULL },
};

static const luaL_Reg DESCRY_VAULT_LIB[] = {
    { "list", l_vault_list },
    { NULL, NULL },
};

void lua_host_setup_api(LuaHost* h)
{
    luaL_newlib(h->L, DESCRY_LIB);                /* [descry] */
    luaL_newlib(h->L, DESCRY_BUFFER_LIB);         /* [descry, buffer] */
    lua_setfield(h->L, -2, "buffer");             /* descry.buffer = … */
    luaL_newlib(h->L, DESCRY_VAULT_LIB);          /* [descry, vault] */
    lua_setfield(h->L, -2, "vault");              /* descry.vault = … */
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

        if (luaL_dofile(h->L, path) != LUA_OK) {
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

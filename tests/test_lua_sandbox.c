/* The plugin host hands every .lua file in plugins/ a Lua state, so what that
 * state can reach is a security boundary — and, since descry.exe was once
 * quarantined as Trojan:Win32/Bearfoos.A!ml for looking like an interpreter
 * that shells out, a shipping constraint too. lua_host_create() trims and
 * confines the stdlib; this pins down exactly what a plugin can and cannot do.
 *
 * The contract (docs/plugins.md):
 *   - no process spawning, no native code, no taking the app down;
 *   - the file API (io.open, io.lines, os.remove, os.rename, require of Lua
 *     modules) works, but only inside the vault and the plugin folder;
 *   - Lua is only ever loaded from source text — never precompiled bytecode,
 *     which Lua does not verify;
 *   - no debug library, no string.dump. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lua_host.h"

#include <lua.h>
#include <lauxlib.h>

#if defined(_WIN32)
  #include <direct.h>
  #define MKDIR(p) _mkdir(p)
#else
  #include <sys/stat.h>
  #include <unistd.h>
  #define MKDIR(p) mkdir(p, 0700)
#endif

static char root[512];       /* scratch folder for the whole test     */
static char plugdir[600];    /* root/plugins: the sandbox             */
static char outside[600];    /* root/outside.txt: must stay unreachable */

/* Real precompiled bytecode, produced by a separate, unrestricted Lua
 * state -- what a hostile plugin author would ship. */
static int dump_writer(lua_State* L, const void* p, size_t sz, void* ud)
{
    (void)L;
    return fwrite(p, 1, sz, (FILE*)ud) == sz ? 0 : 1;
}

static void write_bytecode(const char* path, const char* src)
{
    lua_State* L = luaL_newstate();
    assert(L && luaL_loadstring(L, src) == LUA_OK);
    FILE* f = fopen(path, "wb");
    assert(f);
    assert(lua_dump(L, dump_writer, f, 0) == 0);
    fclose(f);
    lua_close(L);
}

static int g_bc_plugin_failed = -1;
static void note_plugin(const LuaPluginView* p, void* ud)
{
    (void)ud;
    if (strcmp(p->name, "bcplug") == 0) g_bc_plugin_failed = p->load_failed;
}

static void write_file(const char* path, const char* text)
{
    FILE* f = fopen(path, "wb");
    assert(f && "could not write a test file");
    fputs(text, f);
    fclose(f);
}

static int g_probe_failed = -1;
static void note_probe(const LuaPluginView* p, void* ud)
{
    (void)ud;
    if (strcmp(p->name, "zz_probe") == 0) {
        g_probe_failed = p->load_failed;
        if (p->load_failed) fprintf(stderr, "probe plugin error: %s\n", p->error);
    }
}

/* The probe runs as a real plugin -- that is what the sandbox confines --
 * and leaves its answers in a global. A config file (the user's own, so not
 * confined) then returns that table, which lua_host_cfg_number can read. */
static void run_probe(LuaHost* h, const char* body)
{
    char path[700], src[8192];
    snprintf(path, sizeof path, "%s/zz_probe.lua", plugdir);
    snprintf(src, sizeof src,
             "local PLUG = [[%s]]\nlocal OUT = [[%s]]\n"
             "PROBE_RESULTS = (function()\n%s\nend)()\n", plugdir, outside, body);
    write_file(path, src);
    lua_host_load_plugins(h, plugdir);
    lua_host_each_plugin(h, note_probe, NULL);
    assert(g_probe_failed == 0 && "probe plugin failed to run");
    remove(path);

    snprintf(path, sizeof path, "%s/collect.lua", root);
    write_file(path, "return PROBE_RESULTS\n");
    int rc = lua_host_load_config(h, path);
    assert(rc == 0 && "could not collect the probe results");
    remove(path);
}

int main(void)
{
    const char* tmp = getenv("TMPDIR");
    if (!tmp) tmp = getenv("TEMP");
    if (!tmp) tmp = ".";
    snprintf(root, sizeof root, "%s/descry_sandbox_%d", tmp, (int)rand());
    MKDIR(root);
    snprintf(plugdir, sizeof plugdir, "%s/plugins", root);
    MKDIR(plugdir);
    snprintf(outside, sizeof outside, "%s/outside.txt", root);
    write_file(outside, "secret\n");

    /* A Lua module inside the box, for require. */
    {
        char p[700];
        snprintf(p, sizeof p, "%s/helper.lua", plugdir);
        write_file(p, "return { answer = 42 }\n");
        /* Bytecode: as a plugin file, and as a chunk to load by hand. */
        snprintf(p, sizeof p, "%s/bcplug.lua", plugdir);
        write_bytecode(p, "BC_PLUGIN_RAN = true");
        snprintf(p, sizeof p, "%s/chunk.luac", plugdir);
        write_bytecode(p, "return 99");
        /* A module just outside the box, for require to (not) find. */
        snprintf(p, sizeof p, "%s/outside_mod.lua", root);
        write_file(p, "return {}\n");
    }

#if !defined(_WIN32)
    /* A dangling link inside the box pointing outside it: writing through it
     * would create the target out there. (Windows needs privileges to make
     * symlinks, so this case is POSIX-only here.) */
    char dangle_target[700];
    snprintf(dangle_target, sizeof dangle_target, "%s/dangle_target.txt", root);
    {
        char link[700];
        snprintf(link, sizeof link, "%s/dangle", plugdir);
        assert(symlink(dangle_target, link) == 0);
    }
#endif

    remove("4242");             /* the numeric-path probe's would-be file */
    LuaHost* h = lua_host_create();
    assert(h);
    lua_host_setup_api(h);      /* the descry.* table, for descry.open */
    /* Loading plugins from plugdir makes it the sandbox root (there is no
     * vault in this test). */
    lua_host_load_plugins(h, plugdir);
    lua_host_each_plugin(h, note_plugin, NULL);
    assert(g_bc_plugin_failed == 1 && "a bytecode plugin file was executed");

    run_probe(h,
        "local function has(x) return x ~= nil and 1 or 0 end\n"
        "local inside = PLUG .. '/note.txt'\n"
        "local f = io.open(inside, 'wb'); local wrote = f and 1 or 0\n"
        "if f then f:write('ok'); f:close() end\n"
        "local g = io.open(inside, 'rb'); local readback = g and g:read('a') or ''\n"
        "if g then g:close() end\n"
        "local renamed = os.rename(inside, PLUG .. '/note2.txt') and 1 or 0\n"
        "local removed = os.remove(PLUG .. '/note2.txt') and 1 or 0\n"
        "package.path = PLUG .. '/?.lua'\n"
        "local okreq, mod = pcall(require, 'helper')\n"
        "package.path = PLUG .. '/../?.lua'\n"
        "local okesc = pcall(require, 'outside_mod')\n"
        "local bc = string.dump and 1 or 0\n"
        "local bcf = io.open(PLUG .. '/chunk.luac', 'rb')\n"
        "local bcs = bcf and bcf:read('a') or ''\n"
        "if bcf then bcf:close() end\n"
        "assert(#bcs > 4 and bcs:byte(1) == 27, 'bytecode fixture missing')\n"
        "local okbin = (load(bcs) or load(bcs, 'x', 'b') or load(bcs, 'x', 'bt')\n"
        "              or loadfile(PLUG .. '/chunk.luac')\n"
        "              or (pcall(dofile, PLUG .. '/chunk.luac') and true)) and 1 or 0\n"
        "return {\n"
        "  execute = has(os.execute), exit = has(os.exit), popen = has(io.popen),\n"
        "  loadlib = has(package.loadlib), debug = has(debug), dump = bc,\n"
        "  tmpname = has(os.tmpname), cpath = (package.cpath == '') and 1 or 0,\n"
        "  time = has(os.time), date = has(os.date), getenv = has(os.getenv),\n"
        "  wrote = wrote, readback = (readback == 'ok') and 1 or 0,\n"
        "  renamed = renamed, removed = removed,\n"
        "  require_in = (okreq and mod and mod.answer == 42) and 1 or 0,\n"
        "  require_out = okesc and 1 or 0,\n"
        "  read_out = io.open(OUT, 'rb') and 1 or 0,\n"
        "  write_out = io.open(PLUG .. '/../evil.txt', 'wb') and 1 or 0,\n"
        "  lines_out = pcall(io.lines, OUT) and (io.lines(OUT) ~= nil) and 1 or 0,\n"
        "  remove_out = os.remove(OUT) and 1 or 0,\n"
        "  rename_out = os.rename(OUT, PLUG .. '/stolen.txt') and 1 or 0,\n"
        "  rename_in_out = os.rename(PLUG .. '/helper.lua', OUT .. '.x') and 1 or 0,\n"
        "  loadfile_out = loadfile(OUT) and 1 or 0,\n"
        "  dofile_out = pcall(dofile, OUT) and 1 or 0,\n"
        "  bytecode = okbin,\n"
        "  open_out = pcall(descry.open, OUT) and 1 or 0,\n"
        "  number_path = io.open(4242, 'wb') and 1 or 0,\n"
        "  dangling = io.open(PLUG .. '/dangle', 'wb') and 1 or 0,\n"
        "  bc_plugin_ran = BC_PLUGIN_RAN and 1 or 0,\n"
        "  text_load = load('return 7')() == 7 and 1 or 0,\n"
        "}\n");

    /* Removed: process spawning, native code, killing the app, introspection
     * and bytecode forging. */
    assert(lua_host_cfg_number(h, "execute", -1) == 0);
    assert(lua_host_cfg_number(h, "exit",    -1) == 0);
    assert(lua_host_cfg_number(h, "popen",   -1) == 0);
    assert(lua_host_cfg_number(h, "loadlib", -1) == 0);
    assert(lua_host_cfg_number(h, "debug",   -1) == 0);
    assert(lua_host_cfg_number(h, "dump",    -1) == 0);
    assert(lua_host_cfg_number(h, "tmpname", -1) == 0);
    assert(lua_host_cfg_number(h, "cpath",   -1) == 1);
    assert(lua_host_cfg_number(h, "bytecode", -1) == 0);
    assert(lua_host_cfg_number(h, "bc_plugin_ran", -1) == 0);

    /* Kept: the documented surface, working inside the box. */
    assert(lua_host_cfg_number(h, "time",       -1) == 1);
    assert(lua_host_cfg_number(h, "date",       -1) == 1);
    assert(lua_host_cfg_number(h, "getenv",     -1) == 1);
    assert(lua_host_cfg_number(h, "wrote",      -1) == 1);
    assert(lua_host_cfg_number(h, "readback",   -1) == 1);
    assert(lua_host_cfg_number(h, "renamed",    -1) == 1);
    assert(lua_host_cfg_number(h, "removed",    -1) == 1);
    assert(lua_host_cfg_number(h, "require_in", -1) == 1);
    assert(lua_host_cfg_number(h, "text_load",  -1) == 1);

    /* Confined: nothing outside the box, by any route. */
    assert(lua_host_cfg_number(h, "require_out",   -1) == 0);
    assert(lua_host_cfg_number(h, "read_out",      -1) == 0);
    assert(lua_host_cfg_number(h, "write_out",     -1) == 0);
    assert(lua_host_cfg_number(h, "lines_out",     -1) == 0);
    assert(lua_host_cfg_number(h, "remove_out",    -1) == 0);
    assert(lua_host_cfg_number(h, "rename_out",    -1) == 0);
    assert(lua_host_cfg_number(h, "rename_in_out", -1) == 0);
    assert(lua_host_cfg_number(h, "loadfile_out",  -1) == 0);
    assert(lua_host_cfg_number(h, "dofile_out",    -1) == 0);
    assert(lua_host_cfg_number(h, "open_out",      -1) == 0);
    assert(lua_host_cfg_number(h, "number_path",   -1) == 0);
#if !defined(_WIN32)
    assert(lua_host_cfg_number(h, "dangling",      -1) == 0);
#else
    /* No link on Windows: "dangle" is just a new file inside the box. */
    assert(lua_host_cfg_number(h, "dangling",      -1) == 1);
#endif
    {
        FILE* f = fopen("4242", "rb");
        assert(!f && "a numeric path slipped past the sandbox");
    }
#if !defined(_WIN32)
    {
        FILE* f = fopen(dangle_target, "rb");
        assert(!f && "a write through a dangling link escaped the box");
    }
#endif

    /* The outside file is untouched. */
    {
        FILE* f = fopen(outside, "rb");
        assert(f && "outside file was removed or renamed");
        fclose(f);
        char evil[700];
        snprintf(evil, sizeof evil, "%s/evil.txt", root);
        f = fopen(evil, "rb");
        assert(!f && "a plugin wrote outside the box");
    }

    lua_host_destroy(h);
    char p[700];
    snprintf(p, sizeof p, "%s/helper.lua", plugdir); remove(p);
    snprintf(p, sizeof p, "%s/bcplug.lua", plugdir); remove(p);
    snprintf(p, sizeof p, "%s/chunk.luac", plugdir); remove(p);
    snprintf(p, sizeof p, "%s/dangle", plugdir);     remove(p);
    snprintf(p, sizeof p, "%s/outside_mod.lua", root); remove(p);
    remove(outside);
#if defined(_WIN32)
    _rmdir(plugdir); _rmdir(root);
#else
    rmdir(plugdir); rmdir(root);
#endif
    printf("lua_sandbox: ok\n");
    return 0;
}

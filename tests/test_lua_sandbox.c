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

#if defined(_WIN32)
  #include <direct.h>
  #define MKDIR(p) _mkdir(p)
#else
  #include <sys/stat.h>
  #define MKDIR(p) mkdir(p, 0700)
#endif

static char root[512];       /* scratch folder for the whole test     */
static char plugdir[600];    /* root/plugins: the sandbox             */
static char outside[600];    /* root/outside.txt: must stay unreachable */

static void write_file(const char* path, const char* text)
{
    FILE* f = fopen(path, "wb");
    assert(f && "could not write a test file");
    fputs(text, f);
    fclose(f);
}

/* lua_host's only public way to run Lua outside a plugin is load_config,
 * which evaluates a file and keeps the table it returns. So each probe is a
 * config file whose values are the answers. */
static void run_probe(LuaHost* h, const char* body)
{
    char path[700], src[8192];
    snprintf(path, sizeof path, "%s/probe.lua", root);
    snprintf(src, sizeof src,
             "local PLUG = [[%s]]\nlocal OUT = [[%s]]\n%s", plugdir, outside, body);
    write_file(path, src);
    int rc = lua_host_load_config(h, path);
    assert(rc == 0 && "probe script failed to run");
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
    }

    LuaHost* h = lua_host_create();
    assert(h);
    /* Loading plugins from plugdir makes it the sandbox root (there is no
     * vault in this test). */
    lua_host_load_plugins(h, plugdir);

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
        "local okesc = pcall(require, 'probe')\n"
        "local bc = string.dump and 1 or 0\n"
        "local okbin = load('\\27Lua', 'x', 'b') and 1 or 0\n"
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
    snprintf(p, sizeof p, "%s/probe.lua", root);     remove(p);
    remove(outside);
#if defined(_WIN32)
    _rmdir(plugdir); _rmdir(root);
#else
    rmdir(plugdir); rmdir(root);
#endif
    printf("lua_sandbox: ok\n");
    return 0;
}

/* The plugin host hands every .lua file in plugins/ a full Lua state, so what
 * that state can reach is a security boundary — and, since descry.exe was being
 * quarantined as Trojan:Win32/Bearfoos.A!ml for looking like an interpreter that
 * shells out, a shipping constraint too. lua_host_create() trims the stdlib;
 * this pins down exactly what it trims and what it must leave alone, because
 * docs/plugins.md promises plugins the file API. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lua_host.h"

/* lua_host's only public way to run Lua is load_config, which evaluates a file
 * and keeps the table it returns. So the probe is a config file whose values
 * are the answers. */
static const char* PROBE =
    "return {\n"
    "  execute = (os.execute      ~= nil) and 1 or 0,\n"
    "  exit    = (os.exit         ~= nil) and 1 or 0,\n"
    "  popen   = (io.popen        ~= nil) and 1 or 0,\n"
    "  loadlib = (package.loadlib ~= nil) and 1 or 0,\n"
    "  open    = (io.open         ~= nil) and 1 or 0,\n"
    "  lines   = (io.lines        ~= nil) and 1 or 0,\n"
    "  time    = (os.time         ~= nil) and 1 or 0,\n"
    "  date    = (os.date         ~= nil) and 1 or 0,\n"
    "  getenv  = (os.getenv       ~= nil) and 1 or 0,\n"
    "  require = (require         ~= nil) and 1 or 0,\n"
    "}\n";

static char probe_path[512];

static void write_probe(void) {
    const char* tmp = getenv("TMPDIR");
    if (!tmp) tmp = getenv("TEMP");
    if (!tmp) tmp = ".";
    snprintf(probe_path, sizeof probe_path, "%s/descry_sandbox_probe.lua", tmp);
    FILE* f = fopen(probe_path, "wb");
    assert(f && "could not write the probe script");
    fputs(PROBE, f);
    fclose(f);
}

int main(void) {
    write_probe();

    LuaHost* h = lua_host_create();
    assert(h);
    assert(lua_host_load_config(h, probe_path) == 0);

    /* Removed: everything that spawns a process, loads native code, or takes
     * the app down without giving the buffers a chance to save. */
    assert(lua_host_cfg_number(h, "execute", -1) == 0);
    assert(lua_host_cfg_number(h, "exit",    -1) == 0);
    assert(lua_host_cfg_number(h, "popen",   -1) == 0);
    assert(lua_host_cfg_number(h, "loadlib", -1) == 0);

    /* Kept: the surface docs/plugins.md tells plugin authors to use. Trimming
     * these would break published plugins, so they are part of the contract. */
    assert(lua_host_cfg_number(h, "open",    -1) == 1);
    assert(lua_host_cfg_number(h, "lines",   -1) == 1);
    assert(lua_host_cfg_number(h, "time",    -1) == 1);
    assert(lua_host_cfg_number(h, "date",    -1) == 1);
    assert(lua_host_cfg_number(h, "getenv",  -1) == 1);
    assert(lua_host_cfg_number(h, "require", -1) == 1);

    lua_host_destroy(h);
    remove(probe_path);
    printf("lua_sandbox: ok\n");
    return 0;
}

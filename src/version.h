#ifndef DESCRY_VERSION_H
#define DESCRY_VERSION_H

/* The one place the version is written down. main.c uses it for the title bar,
 * the About box and the Lua `descry.version` field; CMakeLists.txt parses this
 * file at configure time to fill in the Windows VERSIONINFO resource, and lists
 * it in CMAKE_CONFIGURE_DEPENDS so a bump here regenerates that resource on the
 * next build. Keeping the define in its own header is what makes that cheap --
 * a configure dependency on main.c would re-run CMake on every edit to it. */
#define DESCRY_VERSION "0.90.1"

#endif /* DESCRY_VERSION_H */

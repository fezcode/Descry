/* Reveal a path in the OS file manager. See reveal.h. */

#include "reveal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
  #include <windows.h>
#endif

size_t shell_quote_single(char* out, size_t cap, const char* s)
{
    size_t n = 0;
#define PUT(c) do { if (n + 1 < cap) out[n] = (c); n++; } while (0)
    PUT('\'');
    for (const char* p = s ? s : ""; *p; ++p) {
        if (*p == '\'') { PUT('\''); PUT('\\'); PUT('\''); PUT('\''); }
        else            PUT(*p);
    }
    PUT('\'');
#undef PUT
    if (cap) out[n < cap ? n : cap - 1] = 0;
    return n;
}

/* pre + quoted(path) + post into out; 0 (and an empty out) on overflow. */
static int join_quoted(char* out, size_t cap, const char* pre,
                       const char* path, const char* post)
{
    char q[2048];
    if (shell_quote_single(q, sizeof q, path) >= sizeof q) {
        if (cap) out[0] = 0;
        return 0;
    }
    int n = snprintf(out, cap, "%s%s%s", pre, q, post);
    if (n < 0 || (size_t)n >= cap) {
        if (cap) out[0] = 0;
        return 0;
    }
    return 1;
}

int reveal_command_macos(char* out, size_t cap, const char* path, bool is_dir)
{
    return join_quoted(out, cap, is_dir ? "open " : "open -R ",
                       path, " >/dev/null 2>&1 &");
}

int reveal_command_linux(char* out, size_t cap, const char* path, bool is_dir)
{
    char dir[1024];
    if (is_dir) {
        snprintf(dir, sizeof dir, "%s", path);
    } else {
        const char* slash = strrchr(path, '/');
        if (!slash)              snprintf(dir, sizeof dir, ".");
        else if (slash == path)  snprintf(dir, sizeof dir, "/");
        else                     snprintf(dir, sizeof dir, "%.*s",
                                          (int)(slash - path), path);
    }
    return join_quoted(out, cap, "xdg-open ", dir, " >/dev/null 2>&1 &");
}

int reveal_windows_target(char* out, size_t cap, const char* path, bool is_dir)
{
    char t[1024];
    snprintf(t, sizeof t, "%s", path);
    for (char* p = t; *p; ++p) if (*p == '/') *p = '\\';
    int n = is_dir ? snprintf(out, cap, "%s", t)
                   : snprintf(out, cap, "/select,\"%s\"", t);
    if (n < 0 || (size_t)n >= cap) {
        if (cap) out[0] = 0;
        return 0;
    }
    return 1;
}

void reveal_in_file_manager(const char* path, bool is_dir)
{
    if (!path || !path[0]) return;
    char cmd[2400];
#if defined(_WIN32)
    /* explorer.exe resolves a relative path against ITS working directory,
     * so absolutize against ours first. */
    char abs[1024];
    if (!_fullpath(abs, path, sizeof abs)) snprintf(abs, sizeof abs, "%s", path);
    if (!reveal_windows_target(cmd, sizeof cmd, abs, is_dir)) return;
    if (is_dir) ShellExecuteA(NULL, "explore", cmd, NULL, NULL, SW_SHOWNORMAL);
    else        ShellExecuteA(NULL, "open", "explorer.exe", cmd, NULL, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    if (!reveal_command_macos(cmd, sizeof cmd, path, is_dir)) return;
    int rc = system(cmd); (void)rc;
#else
    if (!reveal_command_linux(cmd, sizeof cmd, path, is_dir)) return;
    int rc = system(cmd); (void)rc;
#endif
}

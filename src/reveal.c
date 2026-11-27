/* Reveal a path in the OS file manager. See reveal.h. */

#include "reveal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <pthread.h>
  #include <spawn.h>
  #include <stdint.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  extern char** environ;
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
    (void)cmd;
    const char* argv[] = { "open", is_dir ? "--" : "-R", is_dir ? path : "--",
                           is_dir ? NULL : path, NULL };
    spawn_detached(argv);
#else
    char dir[1024];
    if (is_dir) snprintf(dir, sizeof dir, "%s", path);
    else {
        const char* slash = strrchr(path, '/');
        if (!slash)             snprintf(dir, sizeof dir, ".");
        else if (slash == path) snprintf(dir, sizeof dir, "/");
        else snprintf(dir, sizeof dir, "%.*s", (int)(slash - path), path);
    }
    (void)cmd;
    const char* argv[] = { "xdg-open", dir, NULL };
    spawn_detached(argv);
#endif
}

/* ---------------------------------------------------------------------- *
 * Links and process launching
 * ---------------------------------------------------------------------- */

bool url_is_openable(const char* url)
{
    if (!url || !*url) return false;
    static const char* const schemes[] = { "http://", "https://", "mailto:" };
    bool ok = false;
    for (size_t i = 0; i < sizeof schemes / sizeof schemes[0]; ++i) {
        size_t n = strlen(schemes[i]);
        size_t k = 0;
        while (k < n && url[k]) {
            char c = url[k];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != schemes[i][k]) break;
            ++k;
        }
        if (k == n) { ok = true; break; }
    }
    if (!ok) return false;
    size_t len = 0;
    for (const unsigned char* p = (const unsigned char*)url; *p; ++p, ++len) {
        /* Control characters, spaces, quotes and backslashes have no business
         * in a link the OS is about to act on. */
        if (*p < 0x21 || *p == 0x7f || *p == '"' || *p == '\\') return false;
        if (len > 4096) return false;
    }
    /* After the scheme there must be something to open. */
    const char* rest = strchr(url, ':') + 1;
    while (*rest == '/') ++rest;
    return *rest != 0;
}

#if !defined(_WIN32)
static void* reap_child(void* arg)
{
    pid_t pid = (pid_t)(intptr_t)arg;
    int st;
    while (waitpid(pid, &st, 0) < 0) { /* EINTR */ }
    return NULL;
}

/* argv[0] is looked up on PATH and run directly: no shell ever sees the
 * arguments, so nothing in them can be interpreted as a command. */
void spawn_detached(const char* const argv[])
{
    if (!argv || !argv[0]) return;
    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) return;
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", 1 /* O_WRONLY */, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", 1 /* O_WRONLY */, 0);
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &fa, NULL, (char* const*)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return;
    pthread_t t;
    if (pthread_create(&t, NULL, reap_child, (void*)(intptr_t)pid) == 0)
        pthread_detach(t);
}
#endif

#if defined(_WIN32)
static wchar_t* utf8_to_wide(const char* s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t* w = malloc((size_t)n * sizeof *w);
    if (w && MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) { free(w); w = NULL; }
    return w;
}
#endif

void open_url(const char* url)
{
    if (!url_is_openable(url)) return;
#if defined(_WIN32)
    wchar_t* w = utf8_to_wide(url);
    if (w) { ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL); free(w); }
#elif defined(__APPLE__)
    const char* argv[] = { "open", url, NULL };
    spawn_detached(argv);
#else
    const char* argv[] = { "xdg-open", url, NULL };
    spawn_detached(argv);
#endif
}

void open_folder(const char* path)
{
    if (!path || !*path) return;
#if defined(_WIN32)
    wchar_t* w = utf8_to_wide(path);
    if (w) { ShellExecuteW(NULL, L"explore", w, NULL, NULL, SW_SHOWNORMAL); free(w); }
#elif defined(__APPLE__)
    const char* argv[] = { "open", "--", path, NULL };
    spawn_detached(argv);
#else
    const char* argv[] = { "xdg-open", path, NULL };
    spawn_detached(argv);
#endif
}

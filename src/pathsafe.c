/* Path containment checks. See pathsafe.h. */

#include "pathsafe.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
  #include <windows.h>
  #include <wchar.h>
#endif

#define PS_MAX 4096

#if defined(_WIN32)
int path_canon_existing(const char* path, char* out, size_t cap)
{
    if (!path || !*path || !out || cap == 0) return 0;
    wchar_t wpath[PS_MAX];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, PS_MAX)) return 0;
    HANDLE f = CreateFileW(wpath, 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    wchar_t wfinal[PS_MAX];
    DWORD n = GetFinalPathNameByHandleW(f, wfinal, PS_MAX, VOLUME_NAME_DOS);
    CloseHandle(f);
    if (n == 0 || n >= PS_MAX) return 0;
    const wchar_t* w = wfinal;
    if (wcsncmp(w, L"\\\\?\\UNC\\", 8) == 0) {
        /* \\?\UNC\host\share -> \\host\share */
        wfinal[6] = L'\\';
        w = wfinal + 6;
    } else if (wcsncmp(w, L"\\\\?\\", 4) == 0) {
        w += 4;
    }
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL)) return 0;
    for (char* p = out; *p; ++p) {
        if (*p == '\\') *p = '/';
        else if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    }
    return 1;
}
#else
int path_canon_existing(const char* path, char* out, size_t cap)
{
    if (!path || !*path || !out || cap == 0) return 0;
    char buf[PATH_MAX];
    if (!realpath(path, buf)) return 0;
    size_t n = strlen(buf);
    if (n >= cap) return 0;
    memcpy(out, buf, n + 1);
    return 1;
}
#endif

int path_resolves_under(const char* path, const char* root)
{
    char c[PS_MAX], r[PS_MAX];
    if (!path_canon_existing(path, c, sizeof c)) return 0;
    if (!path_canon_existing(root, r, sizeof r)) return 0;
    size_t rl = strlen(r);
    while (rl > 1 && r[rl - 1] == '/') r[--rl] = 0;
    if (strncmp(c, r, rl) != 0) return 0;
    return c[rl] == 0 || c[rl] == '/' || r[rl - 1] == '/';
}

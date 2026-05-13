#include "vault.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
  #include <windows.h>
  #include <commdlg.h>
  #include <shlobj.h>
  #include <SDL_syswm.h>
#endif

void vault_init(Vault* v)
{
    memset(v, 0, sizeof *v);
    v->selected = -1;
}

void vault_free(Vault* v)
{
    if (!v) return;
    for (size_t i = 0; i < v->count; ++i) {
        free(v->items[i].name);
        free(v->items[i].path);
    }
    free(v->items);
    free(v->dir);
    memset(v, 0, sizeof *v);
}

const char* vault_basename(const char* path)
{
    if (!path) return "";
    const char* slash = strrchr(path, '/');
    const char* bs    = strrchr(path, '\\');
    const char* last  = slash > bs ? slash : bs;
    return last ? last + 1 : path;
}

static int has_md_ext(const char* name)
{
    size_t n = strlen(name);
    return n > 3 &&
        (name[n-3] == '.') &&
        (name[n-2] == 'm' || name[n-2] == 'M') &&
        (name[n-1] == 'd' || name[n-1] == 'D');
}

static int ext_eq(const char* name, const char* ext)
{
    size_t nn = strlen(name);
    size_t en = strlen(ext);
    if (nn <= en) return 0;
    for (size_t i = 0; i < en; ++i) {
        char a = name[nn - en + i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

static int has_image_ext(const char* name)
{
    return ext_eq(name, ".png")  || ext_eq(name, ".jpg") ||
           ext_eq(name, ".jpeg") || ext_eq(name, ".gif") ||
           ext_eq(name, ".webp") || ext_eq(name, ".bmp");
}

static void items_reserve(Vault* v, size_t extra)
{
    if (v->count + extra <= v->cap) return;
    size_t nc = v->cap ? v->cap * 2 : 16;
    while (nc < v->count + extra) nc *= 2;
    v->items = realloc(v->items, nc * sizeof(VaultItem));
    v->cap   = nc;
}

static int item_cmp_path(const void* a, const void* b)
{
    const VaultItem* ia = a;
    const VaultItem* ib = b;
    return strcmp(ia->path, ib->path);
}

static void scan_recursive(Vault* v, const char* dir, int depth)
{
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if ((st.st_mode & S_IFMT) == S_IFDIR) {
            items_reserve(v, 1);
            v->items[v->count].name      = strdup(ent->d_name);
            v->items[v->count].path      = strdup(path);
            v->items[v->count].depth     = depth;
            v->items[v->count].is_dir    = 1;
            v->items[v->count].is_image  = 0;
            v->items[v->count].collapsed = 0;
            v->count++;
            if (depth < 8) scan_recursive(v, path, depth + 1);
        } else if ((st.st_mode & S_IFMT) == S_IFREG &&
                   (has_md_ext(ent->d_name) || has_image_ext(ent->d_name))) {
            items_reserve(v, 1);
            v->items[v->count].name      = strdup(ent->d_name);
            v->items[v->count].path      = strdup(path);
            v->items[v->count].depth     = depth;
            v->items[v->count].is_dir    = 0;
            v->items[v->count].is_image  = has_image_ext(ent->d_name);
            v->items[v->count].collapsed = 0;
            v->count++;
        }
    }
    closedir(d);
}

int vault_scan(Vault* v, const char* dir)
{
    for (size_t i = 0; i < v->count; ++i) {
        free(v->items[i].name);
        free(v->items[i].path);
    }
    v->count = 0;
    v->selected = -1;
    free(v->dir);
    v->dir = strdup(dir);

    scan_recursive(v, dir, 0);
    qsort(v->items, v->count, sizeof(VaultItem), item_cmp_path);
    return (int)v->count;
}

int vault_index_of(const Vault* v, const char* path)
{
    if (!v || !path) return -1;
    for (size_t i = 0; i < v->count; ++i)
        if (strcmp(v->items[i].path, path) == 0) return (int)i;
    return -1;
}

#ifdef _WIN32
static HWND wm_hwnd(SDL_Window* parent)
{
    if (!parent) return NULL;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(parent, &info)) return info.info.win.window;
    return NULL;
}
#endif

#if !defined(_WIN32)
/* Read one line from a popen'd command; trim trailing newline. */
static char* run_popen_line(const char* cmd)
{
    FILE* fp = popen(cmd, "r");
    if (!fp) return NULL;
    char buf[2048];
    if (!fgets(buf, sizeof buf, fp)) { pclose(fp); return NULL; }
    pclose(fp);
    size_t n = strlen(buf);
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
    return n > 0 ? strdup(buf) : NULL;
}
#endif

char* vault_open_dialog(SDL_Window* parent)
{
#if defined(_WIN32)
    OPENFILENAMEA ofn;
    char filename[MAX_PATH] = {0};
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner   = wm_hwnd(parent);
    ofn.lpstrFilter = "Markdown (*.md)\0*.md\0All files (*.*)\0*.*\0";
    ofn.lpstrFile   = filename;
    ofn.nMaxFile    = sizeof filename;
    ofn.lpstrTitle  = "Open note";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) return strdup(filename);
    return NULL;
#elif defined(__APPLE__)
    (void)parent;
    return run_popen_line(
        "osascript -e 'POSIX path of (choose file with prompt \"Open note\")' 2>/dev/null");
#elif defined(__linux__) || defined(__unix__)
    (void)parent;
    /* Try zenity first, then kdialog as fallback. */
    char* p = run_popen_line(
        "zenity --file-selection --title=\"Open note\" "
        "--file-filter='Markdown | *.md' 2>/dev/null");
    if (!p) p = run_popen_line(
        "kdialog --getopenfilename '' '*.md|Markdown' 2>/dev/null");
    return p;
#else
    (void)parent;
    return NULL;
#endif
}

char* vault_pick_dir(SDL_Window* parent, const char* title)
{
#if defined(_WIN32)
    BROWSEINFOA bi = { 0 };
    bi.hwndOwner   = wm_hwnd(parent);
    bi.lpszTitle   = title ? title : "Choose folder";
    bi.ulFlags     = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE
                   | BIF_EDITBOX;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return NULL;
    char path[MAX_PATH] = {0};
    BOOL ok = SHGetPathFromIDListA(pidl, path);
    CoTaskMemFree(pidl);
    if (!ok) return NULL;
    /* Forward-slash form is what the rest of the app uses internally. */
    for (char* p = path; *p; ++p) if (*p == '\\') *p = '/';
    return strdup(path);
#elif defined(__APPLE__)
    (void)parent;
    char cmd[512];
    snprintf(cmd, sizeof cmd,
        "osascript -e 'POSIX path of (choose folder with prompt \"%s\")' 2>/dev/null",
        title ? title : "Choose folder");
    return run_popen_line(cmd);
#elif defined(__linux__) || defined(__unix__)
    (void)parent;
    char cmd[512];
    snprintf(cmd, sizeof cmd,
        "zenity --file-selection --directory --title=\"%s\" 2>/dev/null",
        title ? title : "Choose folder");
    char* p = run_popen_line(cmd);
    if (!p) p = run_popen_line(
        "kdialog --getexistingdirectory '' 2>/dev/null");
    return p;
#else
    (void)parent; (void)title;
    return NULL;
#endif
}

char* vault_save_dialog(SDL_Window* parent, const char* default_name)
{
#if defined(_WIN32)
    OPENFILENAMEA ofn;
    char filename[MAX_PATH] = {0};
    char initial_dir[MAX_PATH] = {0};
    /* Split a passed `dir/filename.md` into (initial dir, filename only).
     * Windows' GetSaveFileNameA stays on the working dir if you stuff slashes
     * into lpstrFile and ignores them; the user has to navigate manually.
     * Splitting via lpstrInitialDir + lpstrFile preselects both correctly. */
    if (default_name && *default_name) {
        const char* slash = NULL;
        for (const char* p = default_name; *p; ++p) {
            if (*p == '/' || *p == '\\') slash = p;
        }
        if (slash && (size_t)(slash - default_name) < sizeof initial_dir) {
            size_t dn = (size_t)(slash - default_name);
            memcpy(initial_dir, default_name, dn);
            initial_dir[dn] = 0;
            /* Win32 file dialogs prefer backslashes for lpstrInitialDir. */
            for (char* q = initial_dir; *q; ++q)
                if (*q == '/') *q = '\\';
            snprintf(filename, sizeof filename, "%s", slash + 1);
        } else {
            snprintf(filename, sizeof filename, "%s", default_name);
        }
    }

    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize    = sizeof ofn;
    ofn.hwndOwner      = wm_hwnd(parent);
    ofn.lpstrFilter    = "Markdown (*.md)\0*.md\0All files (*.*)\0*.*\0";
    ofn.lpstrFile      = filename;
    ofn.nMaxFile       = sizeof filename;
    ofn.lpstrTitle     = "Save note";
    ofn.lpstrDefExt    = "md";
    ofn.lpstrInitialDir = initial_dir[0] ? initial_dir : NULL;
    ofn.Flags          = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetSaveFileNameA(&ofn)) return strdup(filename);
    return NULL;
#elif defined(__APPLE__)
    (void)parent;
    char cmd[1024];
    snprintf(cmd, sizeof cmd,
        "osascript -e 'POSIX path of (choose file name with prompt "
        "\"Save note\" default name \"%s\")' 2>/dev/null",
        default_name ? default_name : "untitled.md");
    return run_popen_line(cmd);
#elif defined(__linux__) || defined(__unix__)
    (void)parent;
    char cmd[1024];
    snprintf(cmd, sizeof cmd,
        "zenity --file-selection --save --confirm-overwrite "
        "--title=\"Save note\" --file-filter='Markdown | *.md' "
        "--filename=%s 2>/dev/null",
        default_name ? default_name : "untitled.md");
    char* p = run_popen_line(cmd);
    if (!p) {
        snprintf(cmd, sizeof cmd,
            "kdialog --getsavefilename '%s' '*.md|Markdown' 2>/dev/null",
            default_name ? default_name : "untitled.md");
        p = run_popen_line(cmd);
    }
    return p;
#else
    (void)parent; (void)default_name;
    return NULL;
#endif
}

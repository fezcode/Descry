#ifndef DOWNSEE_VAULT_H
#define DOWNSEE_VAULT_H

#include <SDL.h>
#include <stddef.h>

typedef struct {
    char* name;        /* basename, for display                     */
    char* path;        /* full path, used to open / collapse-prefix */
    int   depth;       /* 0 = top of vault; increases per subdir    */
    int   is_dir;      /* 1 = folder, 0 = file                      */
    int   is_image;    /* 1 = image file (.png/.jpg/.gif/.webp/...) */
    int   collapsed;   /* only meaningful when is_dir; 1 = hide kids */
} VaultItem;

typedef struct {
    VaultItem* items;
    size_t     count;
    size_t     cap;
    int        selected;   /* index of currently-open file, -1 if none */
    char*      dir;        /* root of the vault                        */
} Vault;

void   vault_init  (Vault* v);
void   vault_free  (Vault* v);

/* Scan `dir` for *.md files (top-level only for v0.7), populate v->items
 * sorted by filename. Returns the number of items found, or -1 on error. */
int    vault_scan  (Vault* v, const char* dir);

int    vault_index_of(const Vault* v, const char* path);

/* Open a native file picker (modal) and return a heap-allocated path on
 * success, or NULL if the user cancelled / on platforms without an impl.
 * Caller must free(). Currently Windows-only via comdlg32. */
char*  vault_open_dialog(SDL_Window* parent);

/* Generic native file-open dialog. `title` is the window caption,
 * `filter_label` is what's shown in the dropdown (e.g. "Fonts"),
 * `filter_pattern` is the wildcard list (e.g. "*.ttf;*.otf").
 * Returns malloc'd path on success, NULL on cancel. */
char*  vault_pick_file(SDL_Window* parent, const char* title,
                       const char* filter_label, const char* filter_pattern);

/* Open a native folder picker. Returns malloc'd absolute path on success,
 * NULL on cancel. Used to switch the vault root at runtime. */
char*  vault_pick_dir(SDL_Window* parent, const char* title);

/* Native Save-As dialog. Pre-fills filename with `default_name` (just the
 * basename, not a full path). Returns malloc'd path on success, else NULL. */
char*  vault_save_dialog(SDL_Window* parent, const char* default_name);

/* Last component of a path (after final '/' or '\\'). Returned pointer is
 * inside the input string; do not free. */
const char* vault_basename(const char* path);

#endif

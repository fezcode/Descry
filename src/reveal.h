#ifndef DESCRY_REVEAL_H
#define DESCRY_REVEAL_H

#include <stdbool.h>
#include <stddef.h>

/* "Show in <file manager>" — one implementation for every place a path can
 * be revealed from (sidebar row, tab chip), so no caller has to remember the
 * per-platform verb. Files are selected inside their folder, folders are
 * opened. Fire-and-forget: failures are silent. */
void reveal_in_file_manager(const char* path, bool is_dir);

/* Menu wording for the platform's file manager. A macro so it can sit in
 * static label tables. */
#if defined(_WIN32)
#  define REVEAL_MENU_LABEL "Show in Explorer"
#elif defined(__APPLE__)
#  define REVEAL_MENU_LABEL "Show in Finder"
#else
#  define REVEAL_MENU_LABEL "Show in file manager"
#endif

/* The pure string builders behind reveal_in_file_manager, built on every
 * platform so the tests can pin each one down. Each returns 1 and fills
 * `out`, or 0 with out[0] = 0 when the result would not fit. */

/* POSIX single-quoting: `it's` -> `'it'\''s'`. Returns the length the quoted
 * form needs, like snprintf, so a caller can detect truncation. */
size_t shell_quote_single(char* out, size_t cap, const char* s);

/* macOS: `open -R` selects a file in Finder, plain `open` opens a folder. */
int reveal_command_macos(char* out, size_t cap, const char* path, bool is_dir);

/* Linux: xdg-open the folder itself, or a file's parent — there is no
 * portable "select this file". */
int reveal_command_linux(char* out, size_t cap, const char* path, bool is_dir);

/* Windows: the explorer.exe `/select,"C:\..."` argument for a file, or the
 * backslashed folder path for ShellExecute's `explore` verb. */
int reveal_windows_target(char* out, size_t cap, const char* path, bool is_dir);

#endif

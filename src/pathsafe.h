#ifndef DESCRY_PATHSAFE_H
#define DESCRY_PATHSAFE_H

#include <stddef.h>

/* Path containment checks shared by the vault scanner and the plugin
 * sandbox. Both need the same answer to "where does this path really
 * point?", with symlinks and (on Windows) junctions followed. */

/* Resolve an existing file or folder to its final path: every link
 * followed, absolute, forward slashes. On Windows the result is lowercased
 * (the file system is case-insensitive) and a network path comes out as
 * //host/share/... Returns 1 on success, 0 if it does not exist or does not
 * fit in `cap`. */
int path_canon_existing(const char* path, char* out, size_t cap);

/* 1 if `path` itself is a symlink (or, on Windows, any reparse point such
 * as a junction), whether or not its target exists. */
int path_is_link(const char* path);

/* 1 if the existing `path` resolves to `root` itself or somewhere beneath
 * it (both resolved first), else 0. */
int path_resolves_under(const char* path, const char* root);

#endif

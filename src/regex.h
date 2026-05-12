#ifndef DOWNSEE_REGEX_H
#define DOWNSEE_REGEX_H

#include <stddef.h>

/* Minimal POSIX-flavored regex used by the Find overlay. Supports:
 *   .  ^  $  *  +  ?  *?  +?  ??  |  ()
 *   [abc]  [^abc]  [a-z]
 *   \d \D \w \W \s \S  \n \t \r  and \<other> = literal <other>
 * No backreferences, no lookaround, no { } repetition. Anchors are
 * line-relative (^ matches start-of-line, $ matches end-of-line). */

typedef struct DsRegex DsRegex;

/* Compile `pattern`. Returns NULL on syntax error and writes a short
 * diagnostic into err[errlen] (if non-NULL).
 * `case_insensitive` mirrors letter bits in classes and uses ASCII
 * fold-equality for literal chars. */
DsRegex* ds_regex_compile(const char* pattern, int case_insensitive,
                          char* err, size_t errlen);
void     ds_regex_free   (DsRegex* re);

/* Find the first match in haystack[start..hlen). On success returns 1
 * and writes the byte range [out_start, out_end). Returns 0 if no match.
 * Empty matches (e.g. `a*` against "b") report length 0; the caller is
 * expected to advance past zero-length matches to avoid infinite loops. */
int ds_regex_find(const DsRegex* re,
                  const char* haystack, size_t hlen, size_t start,
                  size_t* out_start, size_t* out_end);

#endif

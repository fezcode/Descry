#ifndef DESCRY_SPELL_H
#define DESCRY_SPELL_H

#include <stdbool.h>
#include <stddef.h>

/* A spell-check dictionary: a chained hash set of lowercased words. The editor
 * underlines any word not in the set. The feature is opt-in (config
 * `spellcheck = true`) and inert until a dictionary is loaded, so it never
 * fires false positives on users who haven't asked for it. */

typedef struct SpellNode {
    struct SpellNode* next;
    char*             word;   /* lowercased, NUL-terminated */
} SpellNode;

typedef struct {
    SpellNode** buckets;
    size_t      nbuckets;
    size_t      count;
} Spell;

void spell_init(Spell* s);
void spell_free(Spell* s);

/* Add one word (lowercased internally). No-op for empty/over-long tokens.
 * Returns true if a word was stored (or already present). */
bool spell_add(Spell* s, const char* w, size_t n);

/* True if the word is in the dictionary (case-insensitive). */
bool spell_known(const Spell* s, const char* w, size_t n);

/* Load a newline-separated word list. Returns the count loaded, or -1 if the
 * file can't be opened. Lines with non-letters (e.g. comments) are still
 * stored verbatim-lowercased; callers point this at real word lists. */
int  spell_load_file(Spell* s, const char* path);

/* True once at least one word is loaded. */
bool spell_ready(const Spell* s);

#endif

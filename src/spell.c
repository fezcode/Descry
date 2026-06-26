#include "spell.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPELL_BUCKETS 65536u      /* power of two; mask-indexed */
#define SPELL_MAXWORD 64

static unsigned long spell_hash(const char* s, size_t n)
{
    unsigned long h = 5381;
    for (size_t i = 0; i < n; ++i)
        h = ((h << 5) + h) ^ (unsigned char)s[i];   /* djb2-xor */
    return h;
}

/* Lowercase ASCII into out (cap incl. NUL). Returns length, or 0 if the token
 * is empty or doesn't fit. Bytes >= 0x80 are passed through unchanged so
 * accented words still hash/compare consistently (we just don't case-fold
 * them). */
static size_t spell_norm(const char* w, size_t n, char* out, size_t cap)
{
    if (n == 0 || n >= cap) return 0;
    size_t j = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)w[i];
        out[j++] = (c < 0x80) ? (char)tolower(c) : (char)c;
    }
    out[j] = 0;
    return j;
}

void spell_init(Spell* s)
{
    s->nbuckets = SPELL_BUCKETS;
    s->buckets  = calloc(s->nbuckets, sizeof *s->buckets);
    s->count    = 0;
}

void spell_free(Spell* s)
{
    if (!s || !s->buckets) { if (s) memset(s, 0, sizeof *s); return; }
    for (size_t i = 0; i < s->nbuckets; ++i) {
        SpellNode* n = s->buckets[i];
        while (n) { SpellNode* nx = n->next; free(n->word); free(n); n = nx; }
    }
    free(s->buckets);
    memset(s, 0, sizeof *s);
}

bool spell_known(const Spell* s, const char* w, size_t n)
{
    if (!s || !s->buckets || s->count == 0) return false;
    char buf[SPELL_MAXWORD];
    size_t m = spell_norm(w, n, buf, sizeof buf);
    if (m == 0) return false;
    size_t b = spell_hash(buf, m) & (s->nbuckets - 1);
    for (SpellNode* it = s->buckets[b]; it; it = it->next)
        if (strcmp(it->word, buf) == 0) return true;
    return false;
}

bool spell_add(Spell* s, const char* w, size_t n)
{
    if (!s || !s->buckets) return false;
    char buf[SPELL_MAXWORD];
    size_t m = spell_norm(w, n, buf, sizeof buf);
    if (m == 0) return false;
    size_t b = spell_hash(buf, m) & (s->nbuckets - 1);
    for (SpellNode* it = s->buckets[b]; it; it = it->next)
        if (strcmp(it->word, buf) == 0) return true;   /* already present */
    SpellNode* node = malloc(sizeof *node);
    if (!node) return false;
    node->word = malloc(m + 1);
    if (!node->word) { free(node); return false; }
    memcpy(node->word, buf, m + 1);
    node->next = s->buckets[b];
    s->buckets[b] = node;
    s->count++;
    return true;
}

int spell_load_file(Spell* s, const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    char line[256];
    int loaded = 0;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r' ||
                         line[n-1] == ' '  || line[n-1] == '\t')) line[--n] = 0;
        if (n == 0) continue;
        if (spell_add(s, line, n)) loaded++;
    }
    fclose(f);
    return loaded;
}

bool spell_ready(const Spell* s) { return s && s->count > 0; }

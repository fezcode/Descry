#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "reveal.h"

static void test_quote(void) {
    char q[64];
    assert(shell_quote_single(q, sizeof q, "plain") == 7);
    assert(strcmp(q, "'plain'") == 0);
    assert(strcmp((shell_quote_single(q, sizeof q, ""), q), "''") == 0);
    assert(strcmp((shell_quote_single(q, sizeof q, NULL), q), "''") == 0);
    /* an embedded quote closes, escapes, reopens */
    shell_quote_single(q, sizeof q, "it's");
    assert(strcmp(q, "'it'\\''s'") == 0);
    /* shell metacharacters are inert inside single quotes: passed verbatim */
    shell_quote_single(q, sizeof q, "$HOME `x` \"y\" ; rm");
    assert(strcmp(q, "'$HOME `x` \"y\" ; rm'") == 0);
    /* truncation: reports the full length, terminates what it wrote */
    size_t need = shell_quote_single(q, 4, "abcdef");
    assert(need == 8);
    assert(strlen(q) == 3);
}

static void test_macos(void) {
    char c[256];
    assert(reveal_command_macos(c, sizeof c, "/Users/a/My Notes/it's.md", false));
    assert(strcmp(c, "open -R '/Users/a/My Notes/it'\\''s.md' >/dev/null 2>&1 &") == 0);
    assert(reveal_command_macos(c, sizeof c, "/Users/a/Notes", true));
    assert(strcmp(c, "open '/Users/a/Notes' >/dev/null 2>&1 &") == 0);
    /* doesn't fit -> refused, not truncated into a different command */
    assert(!reveal_command_macos(c, 16, "/Users/a/Notes/x.md", false));
    assert(c[0] == 0);
}

static void test_linux(void) {
    char c[256];
    assert(reveal_command_linux(c, sizeof c, "/home/a/notes/x.md", false));
    assert(strcmp(c, "xdg-open '/home/a/notes' >/dev/null 2>&1 &") == 0);
    assert(reveal_command_linux(c, sizeof c, "/home/a/notes", true));
    assert(strcmp(c, "xdg-open '/home/a/notes' >/dev/null 2>&1 &") == 0);
    assert(reveal_command_linux(c, sizeof c, "x.md", false));
    assert(strcmp(c, "xdg-open '.' >/dev/null 2>&1 &") == 0);
    assert(reveal_command_linux(c, sizeof c, "/x.md", false));
    assert(strcmp(c, "xdg-open '/' >/dev/null 2>&1 &") == 0);
}

static void test_windows(void) {
    char c[256];
    assert(reveal_windows_target(c, sizeof c, "C:/Vault/notes/a.md", false));
    assert(strcmp(c, "/select,\"C:\\Vault\\notes\\a.md\"") == 0);
    assert(reveal_windows_target(c, sizeof c, "C:/Vault/notes", true));
    assert(strcmp(c, "C:\\Vault\\notes") == 0);
    assert(!reveal_windows_target(c, 8, "C:/Vault/notes", true));
    assert(c[0] == 0);
}

static void test_label(void) {
    assert(strlen(REVEAL_MENU_LABEL) > 0);
#if defined(_WIN32)
    assert(strcmp(REVEAL_MENU_LABEL, "Show in Explorer") == 0);
#elif defined(__APPLE__)
    assert(strcmp(REVEAL_MENU_LABEL, "Show in Finder") == 0);
#endif
}

static void test_url_allowlist(void) {
    /* Web and mail links open. */
    assert(url_is_openable("https://example.com/a?b=c#d"));
    assert(url_is_openable("HTTP://Example.com"));
    assert(url_is_openable("mailto:someone@example.com"));
    /* Anything that can launch a program, reach a share, or poke a protocol
     * handler does not. */
    assert(!url_is_openable("file:///C:/Windows/System32/calc.exe"));
    assert(!url_is_openable("file://attacker/share/evil.exe"));
    assert(!url_is_openable("\\\\attacker\\share\\x"));
    assert(!url_is_openable("ms-settings:defaultapps"));
    assert(!url_is_openable("search-ms:query=x"));
    assert(!url_is_openable("javascript:alert(1)"));
    assert(!url_is_openable("ssh://host"));
    assert(!url_is_openable("httpx://example.com"));
    assert(!url_is_openable("https://"));
    assert(!url_is_openable(""));
    assert(!url_is_openable(NULL));
    /* Shell metacharacters that need a quote or space are refused outright;
     * the rest are harmless because no shell is ever involved. */
    assert(!url_is_openable("https://e.com/'$(id)'"  "\x20"));
    assert(!url_is_openable("https://e.com/a b"));
    assert(!url_is_openable("https://e.com/\"x"));
    assert(!url_is_openable("https://e.com/\\x"));
    assert(!url_is_openable("https://e.com/\nx"));
}

int main(void) {
    test_quote();
    test_macos();
    test_linux();
    test_windows();
    test_label();
    test_url_allowlist();
    printf("all reveal tests passed\n");
    return 0;
}

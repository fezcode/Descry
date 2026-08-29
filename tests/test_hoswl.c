/* Unit tests for hoswl.h — pure parts only (no Hisashi needed).
 * Build + run: sdk/hoswl/test.ps1  (or: gcc -std=c99 -Wall -Wextra -Werror -I. test_hoswl.c && ./a.out) */
#define HOSWL_IMPLEMENTATION
#define HOSWL_PIPE "\\\\.\\pipe\\hoswl-test-nobody-listens"   /* never a live Hisashi: offline tests stay deterministic */
#include "hoswl.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_escape(void)
{
    char out[64];
    hoswl_json_escape(out, sizeof out, "a\"b\\c\nd\t\xc3\xa9");
    assert(strcmp(out, "a\\\"b\\\\c\\nd\\t\xc3\xa9") == 0);
    hoswl_json_escape(out, sizeof out, "\x01");
    assert(strcmp(out, "\\u0001") == 0);
    hoswl_json_escape(out, 4, "abcdef");          /* truncates, stays terminated */
    assert(strcmp(out, "abc") == 0);
}

static void test_compile_basic(void)
{
    char out[2048], err[128];
    int rc = hoswl_compile_menu_text(
        "File\n"
        " file.new|New|Ctrl+N\n"
        " -\n"
        " file.quit|Quit\n"
        "View\n"
        " view.wrap|Word Wrap||x\n"
        " view.spell|Spell||c\n"
        " view.old|Old||d\n", out, sizeof out, err, sizeof err);
    assert(rc == 0);
    assert(strcmp(out,
        "[{\"id\":\"file\",\"label\":\"File\",\"items\":["
        "{\"id\":\"file.new\",\"label\":\"New\",\"key\":\"Ctrl+N\"},"
        "{\"sep\":true},"
        "{\"id\":\"file.quit\",\"label\":\"Quit\"}]},"
        "{\"id\":\"view\",\"label\":\"View\",\"items\":["
        "{\"id\":\"view.wrap\",\"label\":\"Word Wrap\",\"check\":true},"
        "{\"id\":\"view.spell\",\"label\":\"Spell\",\"check\":false},"
        "{\"id\":\"view.old\",\"label\":\"Old\",\"enabled\":false}]}]") == 0);
}

static void test_compile_submenu_and_explicit_menu_id(void)
{
    char out[2048], err[128];
    int rc = hoswl_compile_menu_text(
        "file|File\r\n"
        " recent|Recent vaults|>\n"
        "  recent.0|C:\\vault\n"
        "  recent.1|D:\\notes\n"
        " file.quit|Quit\n", out, sizeof out, err, sizeof err);
    assert(rc == 0);
    assert(strcmp(out,
        "[{\"id\":\"file\",\"label\":\"File\",\"items\":["
        "{\"id\":\"recent\",\"label\":\"Recent vaults\",\"items\":["
        "{\"id\":\"recent.0\",\"label\":\"C:\\\\vault\"},"
        "{\"id\":\"recent.1\",\"label\":\"D:\\\\notes\"}]},"
        "{\"id\":\"file.quit\",\"label\":\"Quit\"}]}]") == 0);
}

static void test_compile_slug_and_empty(void)
{
    char out[512], err[128];
    assert(hoswl_compile_menu_text("Open Recent Files\n x|X\n", out, sizeof out, err, sizeof err) == 0);
    assert(strncmp(out, "[{\"id\":\"open-recent-files\"", 26) == 0);
    assert(hoswl_compile_menu_text("", out, sizeof out, err, sizeof err) == 0);
    assert(strcmp(out, "[]") == 0);
    assert(hoswl_compile_menu_text("Help\n", out, sizeof out, err, sizeof err) == 0);
    assert(strcmp(out, "[{\"id\":\"help\",\"label\":\"Help\",\"items\":[]}]") == 0);
}

static void test_compile_errors(void)
{
    char out[256], err[128];
    assert(hoswl_compile_menu_text(" orphan|Item\n", out, sizeof out, err, sizeof err) == -1);
    assert(strstr(err, "before any menu") != NULL);
    assert(hoswl_compile_menu_text("File\n |NoId\n", out, sizeof out, err, sizeof err) == -1);
    assert(hoswl_compile_menu_text("File\n   too.deep|X\n", out, sizeof out, err, sizeof err) == -1);
    assert(hoswl_compile_menu_text("File\n file.a|A\n", out, 8, err, sizeof err) == -1);
    assert(strstr(err, "too small") != NULL);
}

static void test_parse_click(void)
{
    char id[HOSWL_ID_MAX];
    assert(hoswl_parse_click("{\"t\":\"click\",\"id\":\"file.save\"}", id, sizeof id) == 1);
    assert(strcmp(id, "file.save") == 0);
    assert(hoswl_parse_click("{\"id\":\"x\",\"t\":\"click\"}", id, sizeof id) == 1);
    assert(strcmp(id, "x") == 0);
    assert(hoswl_parse_click("{ \"t\" : \"click\", \"id\" : \"spaced\" }", id, sizeof id) == 1);
    assert(strcmp(id, "spaced") == 0);
    assert(hoswl_parse_click("{\"t\":\"welcome\",\"v\":1,\"host\":\"Hisashi\"}", id, sizeof id) == 0);
    assert(hoswl_parse_click("garbage", id, sizeof id) == 0);
    assert(hoswl_parse_click("{\"t\":\"click\",\"id\":\"a\\\"b\"}", id, sizeof id) == 1);
    assert(strcmp(id, "a\"b") == 0);
    assert(hoswl_parse_click("{\"t\":\"click\",\"id\":\"\\u00e9\\ud83d\\ude00\"}", id, sizeof id) == 1);
    assert(strcmp(id, "\xc3\xa9\xf0\x9f\x98\x80") == 0);
}

static void test_init_and_offline_poll(void)
{
    hoswl_t h;
    assert(hoswl_init(&h, "com.test.app", "Test", "1.0") == 0);
    assert(h.enabled == 1);
    assert(hoswl_set_menus(&h, "File\n file.new|New\n") == 0);
    assert(strstr(h.menu_line, "\"t\":\"menu\"") != NULL);
    assert(h.menu_line[h.menu_len - 1] == '\n');
    assert(hoswl_set_menus(&h, " bad\n") == -1);
    assert(h.menu_len == 0);
    assert(hoswl_set_menus_json(&h, "[{\"id\":\"a\",\"label\":\"A\",\"items\":[]}]") == 0);
    assert(strstr(h.menu_line, "\"menus\":[{\"id\":\"a\"") != NULL);
    /* Not connected: state calls succeed quietly, poll never blocks. */
    assert(hoswl_set_enabled(&h, 0) == 0 && h.enabled == 0);
    assert(hoswl_set_item(&h, "a", 0, -1) == 0);
    assert(hoswl_poll(&h) == NULL);
    assert(hoswl_connected(&h) == 0);
    hoswl_shutdown(&h);
    assert(h.inited == 0);
    assert(hoswl_init(NULL, "x", "y", NULL) == -1);
    assert(hoswl_init(&h, "", "y", NULL) == -1);
}

int main(void)
{
    test_escape();
    test_compile_basic();
    test_compile_submenu_and_explicit_menu_id();
    test_compile_slug_and_empty();
    test_compile_errors();
    test_parse_click();
    test_init_and_offline_poll();
    puts("all hoswl tests passed");
    return 0;
}

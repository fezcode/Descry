#ifndef DESCRY_MACOS_MENU_H
#define DESCRY_MACOS_MENU_H

#if defined(__APPLE__)

/* Native macOS menu bar.
 *
 * Descry draws its own File/Edit/View/Help strip in the title bar on every
 * platform. On macOS that strip duplicates something the OS already gives
 * every app, so the same tables are mirrored into an NSMenu and the in-app
 * copy can be switched off (Settings -> "macOS menu bar").
 *
 * Picks are NOT dispatched inline. AppKit invokes the action while it is
 * still tracking the menu, and several of our actions open modals that pump
 * SDL events themselves — re-entering the event pump from inside Cocoa's own
 * is a good way to deadlock. The pick is queued instead, and the main loop
 * collects it with macos_menu_take_pick() once the pump has returned. */

/* Modifier bits for MacMenuItem.mods. */
#define MAC_MOD_CMD    (1 << 0)
#define MAC_MOD_SHIFT  (1 << 1)
#define MAC_MOD_ALT    (1 << 2)
#define MAC_MOD_CTRL   (1 << 3)

/* One row of a menu; a NULL label terminates the array.
 *
 * `key` is the row's key equivalent (a lowercase character, 0 for none) and
 * `mods` its modifier bits. Descry's accelerator is Cmd on this platform, so
 * these agree with what the app itself listens for — but AppKit consumes a
 * matching keystroke before SDL ever sees it, which means only rows whose
 * action is right to run unconditionally may carry one. The caller decides;
 * everything to do with the clipboard, undo or find deliberately arrives
 * here with key == 0 so those keystrokes still reach the focused text
 * field. */
typedef struct {
    const char* label;
    char        key;
    int         mods;
} MacMenuItem;

/* Build (or rebuild) the menu bar. `titles` is a NULL-terminated array of
 * top-level names; `items[i]` is that menu's NULL-terminated row array.
 * SDL's own application menu (Quit, Hide, Services) is left alone, as are
 * the Window/View menus it appends — ours are inserted between them.
 * Must be called after the window exists, since that is when SDL builds
 * the menu bar we are extending. */
void macos_menu_install(const char* const* titles,
                        const MacMenuItem* const* items);

/* Replace the contents of the "Recent vaults" submenu hanging off the first
 * menu. Passing 0 entries hides the submenu row entirely. */
void macos_menu_set_recents(const char* const* dirs, int count);

/* A pick whose row is >= MAC_ROW_RECENT is a recent-vault entry; subtract
 * MAC_ROW_RECENT for its index. Regular rows are always well below this. */
#define MAC_ROW_RECENT 1000

/* Pop a queued pick. Returns 1 and fills *menu / *row when one was waiting,
 * 0 otherwise. */
int macos_menu_take_pick(int* menu, int* row);

#endif  /* __APPLE__ */
#endif  /* DESCRY_MACOS_MENU_H */

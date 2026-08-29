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

/* One row of a menu; a NULL label terminates the array.
 *
 * There is deliberately no key-equivalent field. Descry's bindings are
 * Ctrl-based on every platform, so a native ⌘ shortcut would disagree with
 * what the app actually listens for, and a native ⌃ shortcut would fire the
 * menu action *and* the app's own keybinding for the same keystroke —
 * harmless for Save, but every toggle would flip twice and appear dead. */
typedef struct {
    const char* label;
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

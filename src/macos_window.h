#ifndef DESCRY_MACOS_WINDOW_H
#define DESCRY_MACOS_WINDOW_H

#if defined(__APPLE__)

/* Cocoa-side cosmetics for the borderless SDL window (macos_window.m).
 *
 * SDL creates the window with NSWindowStyleMaskBorderless because Descry
 * draws its own title bar. That mask also drops the rounded corners every
 * other macOS window has, so they are put back here: the content view's
 * backing layer is clipped to the radius and the window is made non-opaque
 * so the desktop shows through the trimmed corners. */

/* `nswindow` is SDL_SysWMinfo.info.cocoa.window. A radius of 0 restores the
 * square, opaque window (used in fullscreen). Safe to call repeatedly. */
void macos_window_set_corner_radius(void* nswindow, float radius);

/* Title-bar double-click. SDL implements hit-test dragging itself on macOS
 * and drops the mouse-down of any click on a DRAGGABLE area, so the app
 * never sees a double-click there and the standard "double-click the title
 * bar to zoom" doesn't happen. This installs an NSEvent monitor that catches
 * double-clicks in `nswindow` and hands them to `fn` with the point in SDL
 * window coordinates (points, top-left origin) and the action the user
 * picked in System Settings > Desktop & Dock. `fn` returns nonzero when the
 * point was title bar and it acted, which swallows the click. */
enum { MACOS_DBL_ZOOM = 0, MACOS_DBL_MINIMIZE = 1, MACOS_DBL_NONE = 2 };
typedef int (*macos_titlebar_dbl_fn)(void* ud, int x, int y, int action);
void macos_window_install_titlebar_dblclick(void* nswindow,
                                            macos_titlebar_dbl_fn fn, void* ud);

#endif  /* __APPLE__ */
#endif  /* DESCRY_MACOS_WINDOW_H */

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

#endif  /* __APPLE__ */
#endif  /* DESCRY_MACOS_WINDOW_H */

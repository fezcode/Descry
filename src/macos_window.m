/* Rounded corners for the borderless window — the Cocoa half of
 * macos_window.h. Built with ARC (see CMakeLists) like macos_menu.m. */

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include "macos_window.h"

void macos_window_set_corner_radius(void* nswindow, float radius)
{
    @autoreleasepool {
        NSWindow* win = (__bridge NSWindow*)nswindow;
        if (!win) return;
        NSView* content = [win contentView];
        if (!content) return;

        BOOL rounded = radius > 0.0f;

        /* The content view needs a backing layer to clip with. SDL's Metal
         * view is a layer-backed subview, so the mask trims it too. */
        [content setWantsLayer:YES];
        CALayer* layer = [content layer];
        if (!layer) return;
        [layer setCornerRadius:rounded ? (CGFloat)radius : 0.0];
        [layer setMasksToBounds:rounded];

        /* Outside the radius the window has to show the desktop, not its
         * own (square) backing colour. */
        [win setOpaque:!rounded];
        [win setBackgroundColor:rounded ? [NSColor clearColor]
                                        : [NSColor windowBackgroundColor]];

        /* AppKit derives a non-opaque window's shadow from its content and
         * caches the shape, so ask again once SDL has presented a frame —
         * at this point the content is still fully transparent. The block
         * runs from the main run loop, which SDL's event pump drives. */
        [win setHasShadow:YES];
        [win invalidateShadow];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{ [win invalidateShadow]; });
    }
}

/* What a title-bar double-click should do, per System Settings > Desktop &
 * Dock ("Double-click a window's title bar to ..."). AppleActionOnDoubleClick
 * is "Maximize" / "Minimize" / "None"; older systems only have the boolean
 * AppleMiniaturizeOnDoubleClick. Unset means zoom, the macOS default. */
static int titlebar_dbl_action(void)
{
    NSUserDefaults* d = [NSUserDefaults standardUserDefaults];
    NSString* act = [d stringForKey:@"AppleActionOnDoubleClick"];
    if (act) {
        if ([act caseInsensitiveCompare:@"Minimize"] == NSOrderedSame) return MACOS_DBL_MINIMIZE;
        if ([act caseInsensitiveCompare:@"None"] == NSOrderedSame)     return MACOS_DBL_NONE;
        return MACOS_DBL_ZOOM;
    }
    if ([d boolForKey:@"AppleMiniaturizeOnDoubleClick"]) return MACOS_DBL_MINIMIZE;
    return MACOS_DBL_ZOOM;
}

static id g_dbl_monitor = nil;

void macos_window_install_titlebar_dblclick(void* nswindow,
                                            macos_titlebar_dbl_fn fn, void* ud)
{
    @autoreleasepool {
        NSWindow* win = (__bridge NSWindow*)nswindow;
        if (!win || !fn || g_dbl_monitor) return;
        __weak NSWindow* weak_win = win;
        g_dbl_monitor = [NSEvent
            addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown
            handler:^NSEvent* (NSEvent* ev) {
                NSWindow* w = weak_win;
                if (!w || [ev window] != w || [ev clickCount] != 2) return ev;
                NSView* content = [w contentView];
                if (!content) return ev;
                NSPoint p = [content convertPoint:[ev locationInWindow] fromView:nil];
                int x = (int)p.x;
                int y = [content isFlipped]
                        ? (int)p.y
                        : (int)(NSHeight([content bounds]) - p.y);
                return fn(ud, x, y, titlebar_dbl_action()) ? nil : ev;
            }];
    }
}

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

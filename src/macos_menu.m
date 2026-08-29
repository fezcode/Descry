/* Native macOS menu bar — the Cocoa half of macos_menu.h.
 *
 * Built with ARC (see CMakeLists), so the static globals below keep the
 * menu objects alive for the life of the app without explicit retains. */

#import <Cocoa/Cocoa.h>

#include "macos_menu.h"

/* The pick AppKit last reported, waiting for the main loop to collect it.
 * -1 in g_pick_menu means "nothing queued". Only ever touched on the main
 * thread: AppKit menu tracking and the SDL loop are both there. */
static int g_pick_menu = -1;
static int g_pick_row  = -1;

/* Menu items carry their (menu, row) in the NSMenuItem tag. Recent-vault
 * entries use the row space at MAC_ROW_RECENT and up, which is why the
 * multiplier is comfortably larger than any real row count. */
#define TAG_STRIDE 10000

@interface DescryMenuTarget : NSObject
- (void)fire:(id)sender;
@end

@implementation DescryMenuTarget
- (void)fire:(id)sender
{
    NSInteger tag = [(NSMenuItem *)sender tag];
    g_pick_menu = (int)(tag / TAG_STRIDE);
    g_pick_row  = (int)(tag % TAG_STRIDE);
}
@end

static DescryMenuTarget  *g_target      = nil;
static NSMenu            *g_recent_menu = nil;
static NSMenuItem        *g_recent_item = nil;
/* The top-level items we added, so a rebuild removes exactly ours and
 * leaves SDL's application / Window / View menus untouched. */
static NSMutableArray<NSMenuItem *> *g_ours = nil;

int macos_menu_take_pick(int *menu, int *row)
{
    if (g_pick_menu < 0) return 0;
    if (menu) *menu = g_pick_menu;
    if (row)  *row  = g_pick_row;
    g_pick_menu = -1;
    g_pick_row  = -1;
    return 1;
}

void macos_menu_install(const char *const *titles,
                        const MacMenuItem *const *items)
{
    if (!titles || !items) return;

    @autoreleasepool {
        NSMenu *bar = [NSApp mainMenu];
        if (!bar) {
            /* No window yet, so SDL has not registered the app. Nothing to
             * extend; the caller is expected to try again after setup. */
            return;
        }
        if (!g_target) g_target = [[DescryMenuTarget alloc] init];
        if (!g_ours)   g_ours   = [[NSMutableArray alloc] init];

        for (NSMenuItem *old in g_ours) {
            NSInteger at = [bar indexOfItem:old];
            if (at >= 0) [bar removeItemAtIndex:at];
        }
        [g_ours removeAllObjects];
        g_recent_menu = nil;
        g_recent_item = nil;

        /* Insert straight after SDL's application menu so ours read left to
         * right in the usual order and Window/View stay on the right. */
        NSInteger at = ([bar numberOfItems] > 0) ? 1 : 0;

        for (int m = 0; titles[m]; ++m) {
            NSString *title = [NSString stringWithUTF8String:titles[m]];
            if (!title) continue;

            NSMenu *sub = [[NSMenu alloc] initWithTitle:title];
            [sub setAutoenablesItems:NO];

            for (int r = 0; items[m] && items[m][r].label; ++r) {
                NSString *label =
                    [NSString stringWithUTF8String:items[m][r].label];
                if (!label) continue;
                NSMenuItem *mi = [[NSMenuItem alloc] initWithTitle:label
                                                           action:@selector(fire:)
                                                    keyEquivalent:@""];
                [mi setTarget:g_target];
                [mi setTag:(NSInteger)m * TAG_STRIDE + r];
                [mi setEnabled:YES];
                [sub addItem:mi];
            }

            /* The first menu (File) carries the live recent-vaults list. */
            if (m == 0) {
                [sub addItem:[NSMenuItem separatorItem]];
                g_recent_menu = [[NSMenu alloc] initWithTitle:@"Recent vaults"];
                [g_recent_menu setAutoenablesItems:NO];
                g_recent_item = [[NSMenuItem alloc] initWithTitle:@"Recent vaults"
                                                          action:NULL
                                                   keyEquivalent:@""];
                [g_recent_item setSubmenu:g_recent_menu];
                [g_recent_item setHidden:YES];   /* until entries arrive */
                [sub addItem:g_recent_item];
            }

            NSMenuItem *top = [[NSMenuItem alloc] initWithTitle:title
                                                         action:NULL
                                                  keyEquivalent:@""];
            [top setSubmenu:sub];
            [bar insertItem:top atIndex:at++];
            [g_ours addObject:top];
        }
    }
}

void macos_menu_set_recents(const char *const *dirs, int count)
{
    @autoreleasepool {
        if (!g_recent_menu || !g_recent_item) return;
        [g_recent_menu removeAllItems];
        for (int i = 0; i < count && dirs; ++i) {
            if (!dirs[i]) continue;
            NSString *label = [NSString stringWithUTF8String:dirs[i]];
            if (!label) continue;
            NSMenuItem *mi = [[NSMenuItem alloc] initWithTitle:label
                                                       action:@selector(fire:)
                                                keyEquivalent:@""];
            [mi setTarget:g_target];
            [mi setTag:(NSInteger)MAC_ROW_RECENT + i];
            [mi setEnabled:YES];
            [g_recent_menu addItem:mi];
        }
        [g_recent_item setHidden:([g_recent_menu numberOfItems] == 0)];
    }
}

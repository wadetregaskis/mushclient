// wx_osx_statusitem.mm — NSStatusItem implementation of the menu-bar item.
//
// Compiled only on Apple (added to the mushgui target under if(APPLE) in
// CMakeLists.txt, beside wx_osx_text.mm, which is the pattern-setter for
// AppKit shims in this port).  See wx_osx_statusitem.h for why this exists at
// all; this file only knows AppKit.
//
// MRR, not ARC — the same as wx_osx_text.mm and as wxOSX itself.

#import <AppKit/AppKit.h>
#include <wx/bitmap.h>
#include <cstring>
#include "wx_osx_statusitem.h"

// The one item.  Retained for the process's lifetime once shown; released (and
// removed from the bar) by wxOSXStatusItemHide.
static NSStatusItem* g_statusItem = nil;

// ── Menu delegate ─────────────────────────────────────────────────────────
// AppKit calls menuNeedsUpdate: every time the menu is about to be displayed,
// which is exactly when the world list must be re-read.  Building the menu
// once at install time would show the worlds that existed at startup for ever.
@interface McStatusMenuDelegate : NSObject <NSMenuDelegate>
- (void) mcItemChosen: (id) sender;
@end

@implementation McStatusMenuDelegate

- (void) menuNeedsUpdate: (NSMenu*) menu
{
    [menu removeAllItems];

    const int n = mc_statusitem_menu_count();
    for (int i = 0; i < n; ++i)
    {
        const int command = mc_statusitem_menu_command(i);
        if (command == 0)                       // 0 is the separator marker
        {
            [menu addItem: [NSMenuItem separatorItem]];
            continue;
        }

        const char* label = mc_statusitem_menu_label(i);
        NSString* title = [NSString stringWithUTF8String: (label ? label : "")];
        if (!title)
            title = @"";

        NSMenuItem* item =
            [[NSMenuItem alloc] initWithTitle: title
                                       action: @selector(mcItemChosen:)
                                keyEquivalent: @""];
        [item setTarget: self];
        [item setTag: command];
        // A disabled item still needs autoenabling off, or AppKit re-enables it
        // from the target's validateMenuItem: (which we do not implement, so it
        // would default to YES and "(no worlds open)" would look clickable).
        [item setEnabled: mc_statusitem_menu_enabled(i) ? YES : NO];
        [menu addItem: item];
        [item release];
    }
}

- (void) mcItemChosen: (id) sender
{
    if ([sender isKindOfClass: [NSMenuItem class]])
        mc_statusitem_menu_invoke((int) [(NSMenuItem*) sender tag]);
}

@end

static McStatusMenuDelegate* g_menuDelegate = nil;

// ── Image ─────────────────────────────────────────────────────────────────

// Last-resort artwork, drawn rather than loaded: a rounded-rect outline with a
// dot in it, at the menu bar's height.  Nothing in the bundle is required for
// this, so a missing/unreadable icon file can never leave an item that occupies
// menu-bar space and shows nothing — which is worse than no item at all,
// because it is invisible AND unclickable-looking.
//
// This one IS a template image: it is a monochrome glyph by construction, so it
// should follow the menu bar's light/dark appearance the way system items do.
static NSImage* McPlaceholderImage(CGFloat side)
{
    NSImage* img = [[NSImage alloc] initWithSize: NSMakeSize(side, side)];
    [img lockFocus];
    [[NSColor blackColor] set];
    NSBezierPath* outline =
        [NSBezierPath bezierPathWithRoundedRect: NSMakeRect(1.5, 1.5, side - 3.0, side - 3.0)
                                        xRadius: 3.0
                                        yRadius: 3.0];
    [outline setLineWidth: 1.5];
    [outline stroke];
    NSBezierPath* dot =
        [NSBezierPath bezierPathWithOvalInRect:
            NSMakeRect(side * 0.5 - 1.5, side * 0.5 - 1.5, 3.0, 3.0)];
    [dot fill];
    [img unlockFocus];
    [img setTemplate: YES];
    return [img autorelease];
}

// The user's chosen icon (a .ico/.png out of Contents/Resources/dialog_art),
// scaled to the menu bar's height.  NOT a template image: these are colour
// artwork — the whole point of the "Tray Icon" preference is to pick one — and
// -setTemplate: renders by alpha only, which turns a document icon into an
// undifferentiated blob.  Colour status items are ordinary on macOS.
static NSImage* McImageFromBitmap(const wxBitmap& bmp, CGFloat side)
{
    if (!bmp.IsOk())
        return nil;
    CGImageRef cg = bmp.CreateCGImage();
    if (!cg)
        return nil;
    NSImage* img = [[NSImage alloc] initWithCGImage: cg size: NSMakeSize(side, side)];
    CGImageRelease(cg);
    return [img autorelease];
}

// ── Public entry points ───────────────────────────────────────────────────

bool wxOSXStatusItemShow(const wxBitmap& icon, const char* tooltip)
{
    NSStatusBar* bar = [NSStatusBar systemStatusBar];
    if (!bar)
        return false;

    if (!g_statusItem)
    {
        g_statusItem = [[bar statusItemWithLength: NSSquareStatusItemLength] retain];
        if (!g_statusItem)
            return false;
    }

    // Menu-bar thickness minus a little breathing room, the size AppKit's own
    // items use.  Fall back to 18 if the bar reports nothing sensible.
    CGFloat side = [bar thickness] - 4.0;
    if (side < 8.0 || side > 64.0)
        side = 18.0;

    NSImage* img = McImageFromBitmap(icon, side);
    if (!img)
        img = McPlaceholderImage(side);
    [img setSize: NSMakeSize(side, side)];

    NSStatusBarButton* button = [g_statusItem button];
    if (button)
    {
        [button setImage: img];
        [button setImageScaling: NSImageScaleProportionallyDown];
        if (tooltip)
            [button setToolTip: [NSString stringWithUTF8String: tooltip]];
    }

    if (![g_statusItem menu])
    {
        if (!g_menuDelegate)
            g_menuDelegate = [[McStatusMenuDelegate alloc] init];
        NSMenu* menu = [[NSMenu alloc] initWithTitle: @"MUSHclient"];
        // We decide each item's enabled state in the delegate; letting AppKit
        // autoenable would ask a validateMenuItem: we do not implement.
        [menu setAutoenablesItems: NO];
        [menu setDelegate: g_menuDelegate];
        [g_statusItem setMenu: menu];
        [menu release];
    }

    return true;
}

void wxOSXStatusItemHide()
{
    if (!g_statusItem)
        return;
    [[NSStatusBar systemStatusBar] removeStatusItem: g_statusItem];
    [g_statusItem release];
    g_statusItem = nil;
}

void wxOSXActivateSelf()
{
    // ignoringOtherApps: the user just clicked a menu-bar item belonging to
    // this application, so the request is explicit and unambiguous — the same
    // standing SetForegroundWindow has when it is called from a tray click.
    [NSApp activateIgnoringOtherApps: YES];
}

bool wxOSXStatusItemVisible()
{
    return g_statusItem != nil;
}

int wxOSXStatusItemImageWidth()
{
    if (!g_statusItem) return 0;
    NSImage* img = [[g_statusItem button] image];
    return img ? (int) [img size].width : 0;
}

int wxOSXStatusItemImageHeight()
{
    if (!g_statusItem) return 0;
    NSImage* img = [[g_statusItem button] image];
    return img ? (int) [img size].height : 0;
}

int wxOSXStatusItemRebuildMenu()
{
    if (!g_statusItem) return -1;
    NSMenu* menu = [g_statusItem menu];
    if (!menu) return -1;
    // Go through the delegate rather than rebuilding here, so the harness
    // exercises the same path a real click takes.
    id<NSMenuDelegate> d = [menu delegate];
    if (!d || ![d respondsToSelector: @selector(menuNeedsUpdate:)])
        return -1;
    [d menuNeedsUpdate: menu];
    return (int) [menu numberOfItems];
}

bool wxOSXStatusItemMenuItemAt(int index, char* title, int titleLen,
                               int* tag, int* enabled, int* separator)
{
    if (titleLen > 0 && title)
        title[0] = '\0';
    if (!g_statusItem) return false;
    NSMenu* menu = [g_statusItem menu];
    if (!menu) return false;
    if (index < 0 || index >= (int) [menu numberOfItems]) return false;

    NSMenuItem* item = [menu itemAtIndex: index];
    if (title && titleLen > 0)
    {
        const char* utf8 = [[item title] UTF8String];
        if (!utf8) utf8 = "";
        std::strncpy(title, utf8, (size_t) titleLen - 1);
        title[titleLen - 1] = '\0';
    }
    if (tag)       *tag       = (int) [item tag];
    if (enabled)   *enabled   = [item isEnabled] ? 1 : 0;
    if (separator) *separator = [item isSeparatorItem] ? 1 : 0;
    return true;
}

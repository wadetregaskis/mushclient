// wx_osx_text.mm — macOS implementation of wxOSXDisableSmartSubstitutions().
//
// Compiled only on Apple (added to the mushgui target under if(APPLE) in
// CMakeLists.txt; OBJCXX is enabled there too).  See wx_osx_text.h for why.

#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>
#include <wx/textctrl.h>
#include <wx/bitmap.h>
#include "wx_osx_text.h"

void wxOSXDisableSmartSubstitutions(wxTextCtrl* ctrl)
{
    if (!ctrl)
        return;

    // wxWindow::GetHandle() returns the native peer's main NSView.  For a
    // multiline wxTextCtrl that is the enclosing NSScrollView, whose
    // documentView is the NSTextView we want; for a control that already *is*
    // an NSTextView, use it directly.  Single-line controls (NSTextField) have
    // no NSTextView document view → nothing to do.
    id peer = (id) ctrl->GetHandle();
    if (!peer)
        return;

    NSTextView* tv = nil;
    if ([peer isKindOfClass:[NSTextView class]])
        tv = (NSTextView*) peer;
    else if ([peer respondsToSelector:@selector(documentView)])
    {
        id doc = [peer documentView];
        if ([doc isKindOfClass:[NSTextView class]])
            tv = (NSTextView*) doc;
    }
    if (!tv)
        return;

    // Turn off every automatic substitution that can mangle source text.
    [tv setAutomaticQuoteSubstitutionEnabled:NO];
    [tv setAutomaticDashSubstitutionEnabled:NO];
    [tv setAutomaticTextReplacementEnabled:NO];
    [tv setAutomaticSpellingCorrectionEnabled:NO];
    [tv setSmartInsertDeleteEnabled:NO];

    // Also drop the matching "check while typing" types so nothing re-enables
    // them implicitly (e.g. when the view is re-shown).
    tv.enabledTextCheckingTypes &= ~(NSTextCheckingTypeQuote |
                                     NSTextCheckingTypeDash |
                                     NSTextCheckingTypeReplacement);
}

void wxOSXSetDockIcon(const wxBitmap& bmp)
{
    if (!bmp.IsOk())
        return;
    CGImageRef cg = bmp.CreateCGImage();
    if (!cg)
        return;
    // size:NSZeroSize → NSImage adopts the CGImage's pixel size; the Dock scales
    // it up as needed (MUSHclient's icons are small/low-res, faithfully so).
    NSImage* img = [[NSImage alloc] initWithCGImage:cg size:NSZeroSize];
    CGImageRelease(cg);
    if (img)
    {
        [NSApp setApplicationIconImage:img];
        [img release];   // this .mm is compiled MRR (no ARC), as is wxOSX
    }
}

int wxOSXFontHasGlyph(const char* facename, unsigned int codepoint)
{
    if (!facename || !*facename)
        return 0;
    CFStringRef name = CFStringCreateWithCString(NULL, facename, kCFStringEncodingUTF8);
    if (!name)
        return 0;
    CTFontRef font = CTFontCreateWithName(name, 12.0, NULL);
    CFRelease(name);
    if (!font)
        return 0;

    // Encode the code point as UTF-16 (a surrogate pair beyond the BMP).
    UniChar  chars[2];
    CGGlyph  glyphs[2] = { 0, 0 };
    CFIndex  count;
    if (codepoint > 0xFFFF) {
        unsigned int c = codepoint - 0x10000;
        chars[0] = (UniChar)(0xD800 + (c >> 10));
        chars[1] = (UniChar)(0xDC00 + (c & 0x3FF));
        count = 2;
    } else {
        chars[0] = (UniChar) codepoint;
        count = 1;
    }
    bool ok = CTFontGetGlyphsForCharacters(font, chars, glyphs, count) && glyphs[0] != 0;
    CFRelease(font);
    return ok ? 1 : 0;
}

// utils.sendtofront(name): raise a running app to the front.  Windows matches a
// top-level window title; macOS has no global window-title registry for other
// processes, so match on the application instead — case-insensitive substring of
// the localized name or the bundle identifier.  Activating an app brings its
// windows forward, which is what scripts use this for (surfacing an external
// editor after world.shellexecute opens a script file).
bool wxOSXActivateApp(const char* name)
{
    if (!name || !*name)
        return false;

    NSString* needle = [[NSString stringWithUTF8String:name] lowercaseString];
    if (!needle)
        return false;

    for (NSRunningApplication* app in [[NSWorkspace sharedWorkspace] runningApplications])
    {
        NSString* appName   = [[app localizedName] lowercaseString];
        NSString* bundleId  = [[app bundleIdentifier] lowercaseString];
        if ((appName  && [appName  rangeOfString:needle].location != NSNotFound) ||
            (bundleId && [bundleId rangeOfString:needle].location != NSNotFound))
        {
            [app activateWithOptions:NSApplicationActivateAllWindows];
            return true;
        }
    }
    return false;
}

// Terminate the application the same way the Dock icon's "Quit" does, by going
// through -[NSApplication terminate:].  This reliably exits the process — unlike
// closing wx windows and trusting ExitOnFrameDelete, which on some macOS versions
// leaves the app running with no windows.  Cocoa calls applicationShouldTerminate:
// first, which wx forwards to CMUSHWxApp::OSXOnShouldTerminate() — that is where
// the port runs the save-modified / persist-plugin-layout flow; -[NSApp terminate:]
// itself does NOT run the MFC document close/save handling.  Both the menu/Cmd-Q
// Quit and the Dock's Quit funnel through here.
void wxOSXTerminateApp()
{
    [NSApp terminate:nil];
}

// ── Native window tabbing (Global Preferences ▸ "Window tabs") ─────────────
// See wx_osx_text.h.  wx makes each wxMDIChildFrame a real NSWindow on macOS,
// so the system's own tabbing is the natural fit — and the only one available,
// since there is no in-parent client area to dock a Win32 tab strip into.
//
// KNOWN LIMIT, and the reason the port cannot honour all three settings: AppKit
// draws window tabs along the TOP, always.  There is no supported way to put
// them at the bottom, so "Bottom" behaves as "Top" rather than silently doing
// nothing.  Windows honours both.
void wxOSXSetWindowTabbing(wxWindow* win, bool bPreferTabs, const char* identifier)
{
    if (!win) return;
    NSView* view = (NSView*) win->GetHandle();
    if (!view) return;
    NSWindow* window = [view window];
    if (!window) return;

    if (identifier && *identifier)
        [window setTabbingIdentifier: [NSString stringWithUTF8String: identifier]];

    // Automatic (not Disallowed) when off: the preference means "do not ask for
    // tabs", and taking away the user's ability to make them by hand would be a
    // bigger change than the setting describes.
    [window setTabbingMode: bPreferTabs ? NSWindowTabbingModePreferred
                                        : NSWindowTabbingModeAutomatic];
}

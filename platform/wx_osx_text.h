// wx_osx_text.h — disable macOS automatic text substitutions on a wxTextCtrl.
//
// On macOS, NSTextView (the native peer behind a multiline wxTextCtrl) applies
// "smart" substitutions by default: straight quotes → curly quotes, "--" → em
// dash, and user text replacements.  In the script / Immediate / notepad edits
// this silently corrupts Lua source so it no longer compiles.  This helper
// reaches the NSTextView and turns those substitutions off.
//
// No-op on every non-Apple platform (and harmless on single-line controls,
// whose peer is not an NSTextView).
#pragma once

class wxTextCtrl;
class wxBitmap;

#ifdef __APPLE__
void wxOSXDisableSmartSubstitutions(wxTextCtrl* ctrl);   // defined in wx_osx_text.mm
// Set the application's Dock icon at runtime ([NSApp setApplicationIconImage:]).
// macOS has no system tray, so this is where MUSHclient's tray/taskbar-icon
// choice lives on the port.  Ignored if the bitmap is invalid.
void wxOSXSetDockIcon(const wxBitmap& bmp);
// 1 if the named font has a glyph for the Unicode code point, else 0 (CoreText).
int wxOSXFontHasGlyph(const char* facename, unsigned int codepoint);
// utils.sendtofront(name): activate (raise to front) a running application whose
// name matches `name` (case-insensitive substring of the localized app name or
// its bundle identifier).  This is the closest macOS analogue of the Windows
// "bring window with this title to front" behaviour, used to surface an external
// script editor.  Returns true if a matching app was found and activated.
bool wxOSXActivateApp(const char* name);
#else
inline void wxOSXDisableSmartSubstitutions(wxTextCtrl*) {}
inline void wxOSXSetDockIcon(const wxBitmap&) {}
inline int  wxOSXFontHasGlyph(const char*, unsigned int) { return 0; }
inline bool wxOSXActivateApp(const char*) { return false; }
#endif

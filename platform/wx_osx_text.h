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
#else
inline void wxOSXDisableSmartSubstitutions(wxTextCtrl*) {}
inline void wxOSXSetDockIcon(const wxBitmap&) {}
#endif

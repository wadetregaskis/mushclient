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

#ifdef __APPLE__
void wxOSXDisableSmartSubstitutions(wxTextCtrl* ctrl);   // defined in wx_osx_text.mm
#else
inline void wxOSXDisableSmartSubstitutions(wxTextCtrl*) {}
#endif

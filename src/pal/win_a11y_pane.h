// pal/win_a11y_pane.h -- the text pane on Windows: a real read-only RichEdit holding what
// the output window paints, so that a screen reader's caret, review and braille work on
// it as on any other edit control.  It sits behind the output view, the same size, so
// nothing on the screen changes.  See docs/ACCESSIBILITY.md.

#ifdef _WIN32
#pragma once

#include <memory>

class CMUSHclientDoc;

class CWinA11yPane;

CWinA11yPane * WinA11yPane_Create (HWND hOutputView, HWND hCommandBox, CMUSHclientDoc * pDoc);
void WinA11yPane_Destroy (CWinA11yPane * pPane);

void WinA11yPane_Append (CWinA11yPane * pPane, const char * utf8Line);
void WinA11yPane_Flush (CWinA11yPane * pPane);     // lines go in a batch at a time
void WinA11yPane_Reset (CWinA11yPane * pPane, const char * utf8Text);
bool WinA11yPane_Focused (const CWinA11yPane * pPane);
void WinA11yPane_Focus (CWinA11yPane * pPane, bool bPane);

// The message loop's first look at a key: true if it was dealt with.  For the pane itself
// and for the command box.  Keys must reach the pane past the frame's accelerators.
bool WinA11y_PreTranslate (MSG * pMsg);
bool WinA11y_CommandBoxKey (CMUSHclientDoc * pDoc, const MSG * pMsg);

#endif // _WIN32

// doc_dblclick_shared.cpp — double-clicking a word in the output window.
//
// ONE BODY, EVERY BUILD.  Same shape as doc_hyperlink_shared.cpp: the decision
// lives here, and the caller is told only what remains for its own frontend.
//
// The two world options are "Double-click Sends Word" (m_bDoubleClickSends) and
// "Double-click Pastes Word" (m_bDoubleClickInserts), both on World Properties ▸
// Commands ▸ "Keyboard preferences…", both defaulting OFF.  On Windows the whole
// decision lived inline in CMUSHView::OnLButtonDblClk — a view the macOS port
// never constructs — so both options were inert there.  Hoisting it means the
// empty-word test and the sends-beats-inserts precedence exist exactly once.
//
// WHAT DELIBERATELY STAYS WITH THE CALLER, and why it is not an oversight:
//
//   * THE COMMAND-HISTORY ADD.  It must NOT be routed through the doc, because
//     the two frontends' history calls are not equivalent.  Windows uses
//     CSendView::AddToCommandHistory (sendvw.cpp:2227), which applies a no-echo
//     gate and resets the Up-arrow browse position; the PAL adapter's
//     AddCommandToHistory (pal/win_worldview.cpp) reproduces neither, and
//     Windows really does construct that adapter (doc_construct.cpp:214).  A
//     shared body calling it would, ON WINDOWS, start recording double-clicked
//     words during a password prompt and stop resetting the browse position —
//     an observable Windows behaviour change, i.e. a defect.
//
//   * THE COMMAND-BAR SPLICE.  Windows does ReplaceSel (insert at the caret,
//     keeping what is already typed); the port does the same through the PAL
//     command-selection seam.  Both are frontend widgets.
//
// The port additionally suppresses the INSERT arm while a password prompt is
// showing.  Windows has no masked input bar and so has no equivalent hazard;
// see the note at the port's call site.

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"

// Decide what a double-clicked output word should do.  strWord is already the
// word the frontend resolved under the cursor; an empty one means the click did
// not land on a word, which is not an error — Windows reaches the same state by
// computing an empty strCurrentLine and testing IsEmpty() before doing anything.
eDoubleClickOutcome CMUSHclientDoc::OutputWordDoubleClicked (const CString & strWord)
  {

  if (strWord.IsEmpty ())
    return eDblClickNothing;

  // Precedence is SENDS over INSERTS, matching the if / else-if Windows used.
  // Ticking both is not a configuration error; sending simply wins.
  if (m_bDoubleClickSends)
    {
    // CheckConnected() returns true when NOT connected, having already offered
    // to reconnect.  Windows returns immediately in that case, which also skips
    // its selection tidy-up and auto-copy; the caller must do nothing further.
    if (CheckConnected ())
      return eDblClickAbort;

    return eDblClickSend;
    }
  else if (m_bDoubleClickInserts)
    return eDblClickInsert;

  return eDblClickNothing;

  }   // end of CMUSHclientDoc::OutputWordDoubleClicked

// The send itself, once the caller has recorded the word in its own history.
//
// SendMsg, NOT Execute() and NOT EvaluateCommand(): Execute would apply command
// stacking (a word containing the stack character would split into two sends),
// the script prefix, the recursion guard and a per-sub-command plugin callback,
// none of which Windows does here — and it has no way to carry the explicit
// logging flag.  This is deliberately the narrow path.
//
// m_iCurrentActionSource is also left alone on purpose.  OnLButtonDblClk never
// set it, so GetInfo(239) reads eUnknownActionSource inside OnPluginSend on
// Windows, and must here too.
void CMUSHclientDoc::SendDoubleClickedWord (const CString & strWord)
  {

  SendMsg (strWord, m_display_my_input, false, LoggingInput ());

  }   // end of CMUSHclientDoc::SendDoubleClickedWord

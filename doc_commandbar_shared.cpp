// doc_commandbar_shared.cpp — command-bar editing commands.
//
// ONE BODY, EVERY BUILD (Windows, macOS/Linux, headless tests).
//
// These were CSendView members: OnRepeatLastWord, OnInputGlobalchange, the
// arrow-key gate inside PreTranslateMessage, and the script-prefix test at the
// top of OnChar.  None of them is view work — each reads world options and the
// command history and then splices text into the input bar, and pal::IWorldView
// already exposes every one of those operations (currentCommand /
// commandSelection / replaceCommandSelection / commandList).
//
// Why they moved: the wx port never instantiates CSendView (CChildFrame's
// splitter is a stub, so OnCreateClient bails), so all four bodies were dead
// code there and the port grew replacements with materially different
// behaviour:
//
//   * Ctrl+Backspace was hardwired to "delete the word before the caret", so
//     Windows' DEFAULT action for that key — "Recall Last Word", i.e. append
//     the last word of the previously-sent command — did not exist at all, and
//     the world option that chooses between the two
//     (m_bCtrlBackspaceDeletesLastWord / "ctrl_backspace_deletes_last_word")
//     had no reader anywhere outside sendvw.cpp.  Both arms were unreachable.
//
//   * Game ▸ Global Change ignored the input bar's SELECTION and rewrote the
//     whole control, so a change scoped to the selected text hit the rest of
//     the line as well; it also never reported "No replacements made".
//
//   * plain Up/Down browsed the history from ANY caret position.  Windows only
//     browses when the caret is collapsed at the very start or the very end of
//     the text, so the arrows still navigate a multi-line composition — and the
//     command bar is multi-line on both platforms (ES_MULTILINE on Windows,
//     wxTE_MULTILINE here, with Shift+Return inserting a break).
//
//   * "spell check on send" ran on script-prefix commands, so typing a line of
//     Lua with the check turned on popped the suggestion dialog on the
//     identifiers — and accepting a suggestion rewrote the code before it ran.
//
// Windows behaviour is unchanged: same algorithms, same order, same options.

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"

// ── Ctrl+Backspace: "Recall Last Word" / "delete last word" ─────────────────
//
// Was CSendView::OnRepeatLastWord (ID_REPEAT_LAST_WORD, Edit ▸ Recall Last
// Word / Ctrl+Backspace).  The world option m_bCtrlBackspaceDeletesLastWord
// picks the arm; it defaults to FALSE (doc_construct.cpp), so the SHIPPED
// behaviour of Ctrl+Backspace is recall, not delete.
void CMUSHclientDoc::RepeatLastWord ()
{
long lStart = 0, lEnd = 0;
  GetCommandSelection (lStart, lEnd);

// if wanted, delete the last word in the command window
if (m_bCtrlBackspaceDeletesLastWord)
  {
  // if something selected, just delete it
  if (lStart != lEnd)
    {
    ReplaceCommandSelectionRange (lStart, lEnd, "");
    return;
    }

  // get current text
  CString strCurrent = GetCommandText ().c_str ();

  // don't bother if nothing typed
  if (strCurrent.IsEmpty ())
    return;

  const int nStartChar = (int) lStart;
  CString strFirst = strCurrent.Left (nStartChar);  // up to the cursor
  strFirst.TrimRight ();  // skip trailing spaces
  int iPos = strFirst.ReverseFind (' ');    // find the preceding space
  if (iPos < 0)  // if no space, delete back to the start of the line
    iPos = 0;

  // Upstream selected the whole control and re-inserted
  // Left(iPos) + Mid(nStartChar), then put the caret at iPos.  Deleting
  // [iPos, nStartChar) is the same edit — same resulting text, same caret,
  // since ReplaceSel/wxTextCtrl::Replace both leave the caret where the
  // replacement ended — but as ONE undoable edit that does not rewrite (and
  // so does not scroll) the whole control.
  ReplaceCommandSelectionRange (iPos, nStartChar, "");
  return;
  }

// otherwise, recall the last word from the previously-sent line

// can't, if no previous command
std::vector<std::string> history = GetCommandListEntries (1);   // newest first
  if (history.empty ())
    return;

CString strLine = history [0].c_str ();

  strLine.TrimRight ();

int iPos = strLine.ReverseFind (' ');

CString strWord = strLine.Mid (iPos + 1);

  // replace the selection (a bare caret is an empty selection), allowing undo
  ReplaceCommandSelectionRange (lStart, lEnd, strWord);

}   // end of CMUSHclientDoc::RepeatLastWord

// ── Game ▸ Global Change: find/replace inside the command bar ───────────────
//
// Was the second half of CSendView::OnInputGlobalchange; the caller still owns
// CGlobalChangeDlg and the from/to it remembers between invocations, because
// that is frontend work.  strFrom/strTo arrive exactly as the user typed them
// (the backslash translation below is part of the canonical body).
//
// lSelStart/lSelEnd are the bar's selection as it was BEFORE the dialog ran —
// upstream reads it at the top of OnInputGlobalchange, before DoModal, and the
// caller must do the same rather than re-reading it here (a modal dialog is
// entitled to disturb the selection of the control behind it).
//
// Returns false when nothing matched — the canonical arm that reports
// "No replacements made for ..." and leaves the bar untouched.
bool CMUSHclientDoc::GlobalChangeCommand (const CString strChangeFrom,
                                          const CString strChangeTo,
                                          const long lSelStart,
                                          const long lSelEnd)
{
CString strCurrent = GetCommandText ().c_str ();
const int iLength = strCurrent.GetLength ();

int nStartChar = (int) lSelStart;
int nEndChar   = (int) lSelEnd;

  // if they are replacing a selection - just get that bit
  if (nEndChar > nStartChar)
    strCurrent = strCurrent.Mid (nStartChar, nEndChar - nStartChar);

  // fix up special characters

  // version 3.65 - better handling of backslash
  // without this something like \\n becomes treated as \ \n

  strCurrent.Replace ("\r", "");  // get rid of carriage-returns

CString strFrom = strChangeFrom;
CString strTo   = strChangeTo;

  // from
  strFrom.Replace ("\\\\", "\x01"); // temporarily make \\ something else
  strFrom.Replace ("\\n", "\n");
  strFrom.Replace ("\\t", "\t");
  strFrom.Replace ("\x01", "\\");   // put backslashes back

  //to
  strTo.Replace ("\\\\", "\x01");   // temporarily make \\ something else
  strTo.Replace ("\\n", "\n");
  strTo.Replace ("\\t", "\t");
  strTo.Replace ("\x01", "\\");     // put backslashes back

// ::Replace, not the doc's own scripting method of the same name
CString strNew = ::Replace (strCurrent, strFrom, strTo);  // replace all

  if (strNew == strCurrent)
    return false;   // caller reports "No replacements made for ..."

  // The '\n' → ENDLINE conversion upstream did here belongs to the frontend's
  // edit control, not to this decision: CEdit wants CRLF, wxTextCtrl wants LF.
  // The Windows adapter (pal/win_worldview.cpp) does it on the way in.

  // replace the selection — or the whole line if nothing was selected, which
  // is what upstream's SetSel (0, -1) selected.  One undoable edit either way.
  if (nEndChar > nStartChar)
    ReplaceCommandSelectionRange (nStartChar, nEndChar, strNew);
  else
    ReplaceCommandSelectionRange (0, iLength, strNew);

  return true;

}   // end of CMUSHclientDoc::GlobalChangeCommand

// ── the Up/Down arrow gate ──────────────────────────────────────────────────
//
// Was the shared test in both arms of CSendView::PreTranslateMessage: "only
// traverse commands if we are at the *start* or *end* of the command".  The
// command bar is multi-line, so without this the arrows cannot move the caret
// between the lines of a composed command — pressing Up would throw the whole
// composition away and replace it with a history entry.
bool CMUSHclientDoc::CommandCaretAtEdge ()
{
long lStart = 0, lEnd = 0;
  GetCommandSelection (lStart, lEnd);

const long lLength = (long) GetCommandText ().size ();

  return (lStart == 0 && lEnd == 0) ||
         (lStart == lLength && lEnd == lLength);

}   // end of CMUSHclientDoc::CommandCaretAtEdge

// ── "is this line a script command?" ────────────────────────────────────────
//
// Was the test at the top of CSendView::OnChar's spell-check arm: don't spell
// check script commands.  Scripting has to be enabled AND a prefix configured
// AND the line has to start with it.
bool CMUSHclientDoc::IsScriptPrefixCommand (const CString & strText) const
{
  return m_bEnableScripts &&                            // providing scripting active
         !m_strScriptPrefix.IsEmpty () &&               // and we *have* a script prefix
         strText.Left (m_strScriptPrefix.GetLength ()) == m_strScriptPrefix;

}   // end of CMUSHclientDoc::IsScriptPrefixCommand

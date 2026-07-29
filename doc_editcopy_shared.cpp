// doc_editcopy_shared.cpp — which selection Edit ▸ Copy acts on.
//
// ONE BODY, EVERY BUILD (Windows, macOS/Linux, headless tests).
//
// This was two separate hand-written tests in mushview.cpp that had to agree
// with each other:
//
//   CMUSHView::OnUpdateNeedSelectionForCopy  — clamp the selection columns to
//       their lines' lengths, enable if the range is non-degenerate, otherwise
//       defer to the command window's CEditView::OnUpdateNeedSel.
//   CMUSHView::OnEditCopy                    — if GetSelection() reports no
//       output selection, copy the command window's selection instead.
//
// One function now answers both questions, so "the item is enabled" and "this
// is what Copy will copy" cannot drift apart.
//
// Why it moved: on the wx port Copy was routed by *keyboard focus* — the Edit
// menu handler asked wxWindow::FindFocus() whether the output pane had focus and
// only then copied its selection.  Nothing ever calls SetFocus() on COutputPane
// (see the "output pane never focused" note), so that test was always false:
// selecting text in the output window and pressing Copy consulted the command
// bar, found no selection there, and did nothing.  Focus is the wrong question
// anyway — Windows never asks it either.  CMUSHView::OnEditCopy actually moves
// the focus *back* to the command bar (OnKeysActivatecommandview) before
// copying, which only makes sense because the decision is selection-based.
//
// Windows behaviour is unchanged: same clamp, same comparison, same
// output-beats-command precedence.  The clamp now lives in the two IWorldView
// adapters' outputSelection() — the Windows one is a transcription of the very
// loop this file replaced (pal/win_worldview.cpp), so the arithmetic is the
// same code, just moved.

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"

eCopyTarget CMUSHclientDoc::EditCopyTarget (void)
  {
  if (m_pWorldView == NULL)
    return eCopyNothing;      // no view attached (headless, or mid-teardown)

// The output window's selection wins when there is one.
//
// outputSelection() is 1-based with all-zeros meaning "nothing selected", and
// the adapter has already clamped both columns to their lines' lengths.  The
// non-degeneracy comparison is repeated here rather than trusting the zeros,
// because an adapter that returned raw endpoints instead would otherwise report
// a caret (start == end) as a selection.

  pal::TextRange sel = m_pWorldView->outputSelection ();

  if (sel.startLine != 0 &&
      (sel.endLine > sel.startLine ||
        (sel.endLine == sel.startLine && sel.endCol > sel.startCol)))
    return eCopyOutputSelection;

// Otherwise the command bar, if IT has a selection.  commandSelection() is the
// raw half-open [start, end) character range (pal/pal_types.h), so an empty
// selection — a plain caret — has end == start.

  pal::Range cmd = m_pWorldView->commandSelection ();

  if (cmd.end > cmd.start)
    return eCopyCommandSelection;

  return eCopyNothing;
  }

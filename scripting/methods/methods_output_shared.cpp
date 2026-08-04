// scripting/methods/methods_output_shared.cpp — the parts of the
// world.Set* output family that are pure engine state.
//
// Moved VERBATIM out of scripting/methods/methods_output.cpp, which is
// mushgui-only because most of the family really does need a view.  These three
// do not: each writes a document member and, at most, pushes it out through an
// Emit* seam that exists in every build.  Their headless stubs in
// platform/doc_gui_method_stubs.cpp answered with a contract Windows has never
// had — SetUnseenLines did nothing at all, SetMainTitle never wrote
// m_strMainWindowTitle (which world.GetInfo(89) reads back), and
// SetBackgroundColour dropped the member write AND returned 0 instead of the
// previous colour, which is its documented return value.
//
// A build-graph move only; the code is unchanged, and Windows compiles the
// whole family into one binary either way.  The file is in MUSHclient.vcxproj.

#include "stdafx.h"
#include "../../MUSHclient.h"
#include "../../doc.h"
#include "../errors.h"

#include <string>

long CMUSHclientDoc::SetBackgroundColour(long Colour)
{
  long oldColour = m_iBackgroundColour;

  m_iBackgroundColour = Colour;

  EmitBackgroundColour((unsigned long)Colour);

  UpdateAllViews (NULL);

	return oldColour;
}   // end of CMUSHclientDoc::SetBackgroundColour

void CMUSHclientDoc::SetMainTitle(LPCTSTR Title)
{
	m_strMainWindowTitle = Title;
  EmitMainTitle(std::string(static_cast<const char*>(m_strMainWindowTitle)));
} // end of CMUSHclientDoc::SetMainTitle

void CMUSHclientDoc::SetUnseenLines(long Counter)
{
	m_new_lines = Counter;
} // end of CMUSHclientDoc::SetUnseenLines

// ── DeleteLines ─────────────────────────────────────────────────────────────
// world.DeleteLines(n).  Engine bookkeeping throughout — the only view-shaped
// part was already #ifdef'd (a CMUSHView walk on Windows, a DeleteOutputLines
// seam call off it).  Its headless stub in doc_gui_method_stubs.cpp diverged
// from it in three ways the 2026-08 audit caught, all of which a test could
// have been written to enshrine: it dropped the m_bInSendToScript refusal that
// keeps a trigger from deleting the paragraph ProcessPreviousLine is standing
// in, it never decremented m_total_lines, and it left a stale jump-index chunk
// when the post-delete count landed on an exact multiple of JUMP_SIZE.
void CMUSHclientDoc::DeleteLines(long Count)
{

POSITION pos;

/*

  I can't delete lines when in send-to-script, please don't try to make me. ;)

  The problem is that in ProcessPreviousLine we have established the start and end line
  of the paragraph we are processing, if that is deleted by a trigger in the middle, all
  hell breaks loose.

  */

  if (m_bInSendToScript)
    return;   // can't do it

  if (Count <= 0)
     return;        // nothing to do

  // if we have the empty line at the end of the buffer, delete that too
  if (m_pCurrentLine && m_pCurrentLine->len == 0)
    Count++;

#ifndef _WIN32
  // Snapshot how many lines the output pane currently has so we can tell it
  // how many to remove after we've trimmed m_LineList.
  long guiEmittedBefore = m_gui_lines_emitted;
#endif

// delete all lines in this set
// (The original loop kept a POSITION across RemoveTail and read it afterwards
// via GetPrev — MFC tolerates that stale read by accident (freed nodes keep
// pPrev until reused), but it is use-after-free and crashes on the portable
// list. Iterating by emptiness deletes exactly the same tail lines.)
  for ( ; Count > 0 && !m_LineList.IsEmpty (); Count--)
   {
   // if this particular line was added to the line positions array, then make it null
    if (m_LineList.GetCount () % JUMP_SIZE == 1)
          m_LinePositions.Set (m_LineList.GetCount () / JUMP_SIZE, NULL);

    delete m_LineList.GetTail (); // delete contents of tail iten -- version 3.85
    m_LineList.RemoveTail ();   // get rid of the line
    m_total_lines--;            // don't count as received
   }

  // try to allow world.tells to span omitted lines
  if (!m_LineList.IsEmpty ())
    {
    m_pCurrentLine = m_LineList.GetTail ();
    if (((m_pCurrentLine->flags & COMMENT) == 0) ||
        m_pCurrentLine->hard_return)
        m_pCurrentLine = NULL;
    }
  else
    m_pCurrentLine = NULL;

  if (!m_pCurrentLine)
    {
    // restart with a blank line at the end of the list
    m_pCurrentLine = new CLine (++m_total_lines, 
                                m_nWrapColumn,
                                m_iFlags,
                                m_iForeColour,
                                m_iBackColour,
                                m_bUTF_8);
    pos = m_LineList.AddTail (m_pCurrentLine);

    if (m_LineList.GetCount () % JUMP_SIZE == 1)
      m_LinePositions.Set (m_LineList.GetCount () / JUMP_SIZE, pos);
    }

// notify view that we have "added" stuff (deleted, really)
// this is so that scroll bar positions are recalculated, and the
// view now scrolls to the bottom, making the previous lines now end at the end of the window.

#ifndef _WIN32
  {
    // Clamp m_gui_lines_emitted to the current list size minus the
    // in-progress tail line (which UpdateAllViews never emits until done).
    long newListSize = (long)m_LineList.GetCount();
    long newEmitted  = std::min(guiEmittedBefore, std::max(0L, newListSize - 1));
    m_gui_lines_emitted = newEmitted;

    // Remove the corresponding number of lines from the output pane's buffer.
    long paneRemove = guiEmittedBefore - newEmitted;
    if (paneRemove > 0)
      DeleteOutputLines(paneRemove);
  }
#else
  for(pos = GetFirstViewPosition(); pos != NULL; )
    {
  	CView* pView = GetNextView(pos);

  	if (pView->IsKindOf(RUNTIME_CLASS(CMUSHView)))
  	  {
  		CMUSHView* pmyView = (CMUSHView*)pView;

  		pmyView->addedstuff ();

  		}	  // end of being an output view
  	}   // end of doing each view
#endif

}  // end of DeleteLines

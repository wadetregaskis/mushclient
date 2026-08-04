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

// wx_a11y_pane.cpp -- the text pane on macOS and Linux: a read-only wxTextCtrl, which is an
// NSTextView or a GtkTextView, so VoiceOver and Orca read it as they read any other.  It is a
// strip under the output window, and exists only while output is being read or the player
// asked for it.  UNCOMPILED where it was written.  docs/ACCESSIBILITY.md describes it.

#include "wx_world_child.h"
#include "wx_a11y.h"
#include "../a11y.h"

#include <wx/sizer.h>
#include <wx/textctrl.h>

namespace {
const int kMaxPaneLines = 5000;
const int kTrimPaneAt = 5500;
}

// Translates a key to the engine's terms, for the command box and the pane alike.
bool CWorldChildFrame::A11yKeyDown (wxKeyEvent & evt)
  {
  if (!m_doc)
    return false;
  A11yKey key;
  int ch = 0;
  const int code = evt.GetKeyCode ();
  switch (code)
    {
    case WXK_UP:       key = A11yKey::Up;       break;
    case WXK_DOWN:     key = A11yKey::Down;     break;
    case WXK_LEFT:     key = A11yKey::Left;     break;
    case WXK_RIGHT:    key = A11yKey::Right;    break;
    case WXK_HOME:     key = A11yKey::Home;     break;
    case WXK_END:      key = A11yKey::End;      break;
    case WXK_PAGEUP:   key = A11yKey::PageUp;   break;
    case WXK_PAGEDOWN: key = A11yKey::PageDown; break;
    case WXK_RETURN:   key = A11yKey::Return;   break;
    case WXK_F6:       key = A11yKey::F6;       break;
    case WXK_F12:      key = A11yKey::F12;      break;
    default:
      if (code < 32 || code > 126)
        return false;
      key = A11yKey::Character;
      ch = code >= 'a' && code <= 'z' ? code - 32 : code;
      break;
    }
  return A11y_Key (*m_doc, key, ch, evt.RawControlDown (), evt.AltDown (), evt.ShiftDown ());
  }

void CWorldChildFrame::accessiblePaneShow (bool show)
  {
  if (!show)
    {
    if (m_a11yPane)
      {
      m_a11yPane->Destroy ();
      m_a11yPane = nullptr;
      m_a11yPaneLines = 0;
      Layout ();
      }
    return;
    }
  if (m_a11yPane)
    return;

  m_a11yPane = new wxTextCtrl (this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize (-1, 80),
                               wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
  McSetAccessibleName (m_a11yPane, "Output");
  if (m_input)
    McSetAccessibleName (m_input, "Command");

  m_a11yPane->Bind (wxEVT_KEY_DOWN, [this] (wxKeyEvent & evt)
    {
    if (A11yKeyDown (evt))
      return;
    const int code = evt.GetKeyCode ();
    if (code == WXK_TAB || code == WXK_RETURN || code == WXK_ESCAPE)
      {
      accessibleFocusPane (false);
      return;
      }
    evt.Skip ();
    });
  // Typing belongs in the command box, as it does from the output window.
  m_a11yPane->Bind (wxEVT_CHAR, [this] (wxKeyEvent & evt)
    {
    const wxChar ch = evt.GetUnicodeKey ();
    wxTextCtrl * input = ActiveInput ();
    if (ch >= 32 && ch != 127 && input && !evt.RawControlDown () && !evt.CmdDown ())
      {
      input->AppendText (wxString (ch));
      input->SetFocus ();
      input->SetInsertionPointEnd ();
      return;
      }
    evt.Skip ();
    });

  // Under the output window, above the input bar: the sizer keeps them in that order.
  if (wxSizer * sizer = GetSizer ())
    {
    sizer->Insert (1, m_a11yPane, 0, wxEXPAND);
    Layout ();
    }
  }

void CWorldChildFrame::accessibleLine (const char * utf8)
  {
  if (!m_a11yPane)
    return;

  // Whatever the player is reading stays where it is.
  long from = 0, to = 0;
  m_a11yPane->GetSelection (&from, &to);
  const long caret = m_a11yPane->GetInsertionPoint ();

  m_a11yPane->AppendText ((m_a11yPane->IsEmpty () ? wxString () : wxString ("\n")) + wxString::FromUTF8 (utf8));
  m_a11yPaneLines++;

  long removed = 0;
  if (m_a11yPaneLines > kTrimPaneAt)
    {
    removed = m_a11yPane->XYToPosition (0, m_a11yPaneLines - kMaxPaneLines);
    if (removed > 0)
      m_a11yPane->Remove (0, removed);
    m_a11yPaneLines = kMaxPaneLines;
    }

  if (from != to)
    m_a11yPane->SetSelection (wxMax (0L, from - removed), wxMax (0L, to - removed));
  else
    m_a11yPane->SetInsertionPoint (wxMax (0L, caret - removed));
  }

void CWorldChildFrame::accessibleReset (const char * utf8Text)
  {
  if (!m_a11yPane)
    return;
  wxString text = wxString::FromUTF8 (utf8Text);
  if (text.EndsWith ("\n"))
    text.RemoveLast ();
  m_a11yPane->ChangeValue (text);
  m_a11yPaneLines = m_a11yPane->GetNumberOfLines ();
  }

bool CWorldChildFrame::accessiblePaneFocused () const
  {
  return m_a11yPane && m_a11yPane->HasFocus ();
  }

void CWorldChildFrame::accessibleFocusPane (bool pane)
  {
  if (!pane)
    {
    if (wxTextCtrl * input = ActiveInput ())
      input->SetFocus ();
    return;
    }
  if (!m_a11yPane)
    return;
  // On arriving, at the newest line.
  m_a11yPane->SetInsertionPoint (m_a11yPane->XYToPosition (0, wxMax (0, m_a11yPane->GetNumberOfLines () - 1)));
  m_a11yPane->SetFocus ();
  }

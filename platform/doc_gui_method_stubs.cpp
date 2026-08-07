// platform/doc_gui_method_stubs.cpp
//
// Inert headless definitions of the five GUI method families
// (miniwindow / notepad / chat / output-view / accelerator) that
// scripting/lua_methods.cpp binds but whose real TUs
// (scripting/methods/methods_{miniwindows,notepad,chat,output,accelerators}.cpp)
// are mushapp-only.  Each body returns the canonical no-GUI result —
// eNoSuchWindow / eNoChatConnections / empty safearray / VT_NULL / silent eOK —
// per the Campaign C flip inventory; the pre-flip portable twins'
// behaviours are the spec.  Delete family-by-family as the real TUs are
// adopted into mushcore (Phase 2 of the campaign endgame).
//
// Compiled into mushcore ONLY (never mushapp — the real TUs own these
// symbols there).

#ifndef _WIN32

// B-3 atomic flip: real headers (portable stub deleted; mushcore is FULL_BUILD).
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/errors.h"   // eOK / eNoSuchWindow / eNoChatConnections

#include <cstring>   // strlen
#include <cstdlib>   // free

// mxp/mxputils.cpp (mushcore) — returns true on FAILURE ("can't find colour")
bool SetColour (const CString & strName, COLORREF & iColour);

// ── VARIANT helpers ──────────────────────────────────────────────────────

// VT_EMPTY — what every real Window*/Accelerator List method answers when
// there is nothing to list: they either stamp VT_EMPTY explicitly
// (WindowFontList/WindowImageList/WindowHotspotList when the window doesn't
// exist) or never CreateOneDim ("cannot create empty array dimension":
// WindowList/AcceleratorList) so COleSafeArray::Detach hands back VT_EMPTY.
// pushVariant turns it into Lua nil, matching Windows.
static VARIANT EmptySafeArray (void)
  {
  VARIANT v;
  VariantInit (&v);
  v.vt = VT_EMPTY;
  return v;
  }

// VT_NULL — pushVariant's default arm turns it into nil.
static VARIANT NullVariant (void)
  {
  VARIANT v;
  VariantInit (&v);
  v.vt = VT_NULL;
  return v;
  }

// VT_EMPTY — also nil through pushVariant; the exact result the Windows
// list builders produce when they skip CreateOneDim on a zero count.
static VARIANT EmptyVariant (void)
  {
  VARIANT v;
  VariantInit (&v);
  return v;
  }


// ══════════════════════════════════════════════════════════════════════════
// Miniwindow family: NO LONGER STUBBED.  miniwindow.cpp, miniwindow_compose.cpp,
// surface_bridge.cpp and scripting/methods/methods_miniwindows.cpp are compiled
// into mushcore now (stage 1).
//
// The intuition this retires is that miniwindows need the GUI.  They do not —
// they need a SURFACE.  Those TUs include no wx headers at all; ~190 of their
// ~205 drawing sites go through pal::ISurface, and every one is guarded
// `if (!srf) return ...`.  Headless nothing installs an adapter, so the surface
// stays null and the guarded arms run: the window EXISTS, and its geometry,
// z-order, hotspots, fonts, info and popup menu are all real.  Only the pixels
// wait for stage 2 (a headless pal::CpuRaster bridge + DIB store).
//
// The 47 stubs deleted here answered eNoSuchWindow to everything, so no
// miniwindow could be created and the whole family was one error code deep.
// The one behaviour to preserve carefully is the eOK-on-null-surface arm: it is
// what makes stage 1 safe without stage 2, and turning it into an error would be
// a real behaviour change on BOTH shipping platforms if an adapter ever failed
// to install.

// ══════════════════════════════════════════════════════════════════════════
// Notepad family: NO LONGER STUBBED.  TextDocument.cpp and
// scripting/methods/methods_notepad.cpp are compiled into mushcore now, so the
// tests exercise the real bodies.
//
// The 14 stubs deleted here did not merely return inert values — they forwarded
// to a parallel g_wxhook_*_notepad table that carried no traffic in EITHER
// build.  Nine of those hooks had exactly one caller (this file, headless) and
// exactly one installer (platform/wx_mfc_impl.cpp, GUI), and the two adapter
// sets never co-link — so every forward was permanently nullptr where it was
// called and permanently uncalled where it was installed.  Meanwhile the twin
// had drifted: SendToNotepad appended where Windows creates, CloseNotepad
// discarded QuerySave, GetNotepadList discarded All (the parameter's whole
// purpose is world-scoping), and NotepadFont/NotepadColour/NotepadReadOnly
// never wrote the state they are named for.
//
// methods_notepad.cpp needed no new seam: its non-Windows arms already go
// through CTextDocument's std::function m_fn_* members (TextDocument.h:66-98),
// which a headless test fills with lambdas over a std::string.

// ══════════════════════════════════════════════════════════════════════════
// Chat family: NO LONGER STUBBED.  chatsock.cpp, chatlistensock.cpp and
// scripting/methods/methods_chat.cpp are compiled into mushcore now.
//
// The 25 stubs deleted here all returned eNoChatConnections, and that was wrong
// for 18 of them even as a no-connections answer: the real bodies return
// eChatIDNotFound, eBadParameter, eChatPersonNotFound, eClipboardEmpty, eOK, or
// a plain COUNT.  ChatDisconnectAll returns the number it closed, so the stub
// put an error code (30044) in a count slot and a script doing
// `if world.ChatDisconnectAll() > 0` behaved backwards.  ChatNameChange
// SUCCEEDS with zero connections on Windows — it renames m_strOurChatName and
// broadcasts — where the stub mutated nothing.
//
// mushcore already linked half this engine: GetChatSocket and
// SendChatMessageToAll are the real bodies in doc.cpp, walking m_ChatList and
// calling into sockets these stubs had hollowed out.
//
// The chat DIALOGS are NOT part of this and stay stubbed in
// platform/doc_flip_gui_stubs.cpp: they are irreducible shells that the dialog
// bridge renders in the product.

// ══════════════════════════════════════════════════════════════════════════
// Output/view family (25) — GUI ops inert; line-list ops keep twin semantics
// ══════════════════════════════════════════════════════════════════════════

void CMUSHclientDoc::ActivateClient ()
  {
  // GUI-only headless — silent no-op
  }   // end of CMUSHclientDoc::ActivateClient

long CMUSHclientDoc::AddFont (LPCTSTR PathName)
  {
  // The real body's FIRST act is a parameter check, ahead of anything GUI:
  //   if (strlen (PathName) <= 0) return eBadParameter;
  // (methods_output.cpp).  Registering the font itself needs a GUI, but
  // answering eOK to an empty path is a contract Windows has never had, and a
  // test asserting it would be pinning the stub.
  if (!PathName || strlen (PathName) == 0)
    return eBadParameter;
  return eOK;   // nothing to register headless
  }   // end of CMUSHclientDoc::AddFont

// Bookmark is no longer stubbed: the real body moved to doc_shared.cpp (its
// only view dependency was the CMUSHView walk, now the IWorldView seam), so
// mushcore links the canonical one.

// DeleteLines is no longer stubbed: the real body moved to
// scripting/methods/methods_output_shared.cpp, so mushcore links the
// canonical one (m_bInSendToScript refusal, m_total_lines, jump index).

void CMUSHclientDoc::DeleteOutput ()
  {
  // The real body (methods_output.cpp) is exactly this one call, and
  // ClearOutput lives in doc.cpp, which mushcore links.  The old stub
  // reimplemented the teardown by hand and skipped three things
  // ClearOutput does: m_sRecentLines.clear (), the m_DisplayFindInfo
  // reset, and EmitClearOutput () -- so world.DeleteOutput left stale
  // trigger context, a live find cursor into freed lines, and a view
  // still showing the text it had just deleted.
  ClearOutput ();
  }   // end of CMUSHclientDoc::DeleteOutput

void CMUSHclientDoc::FlashIcon ()
  {
  // GUI-only headless — silent no-op
  }   // end of CMUSHclientDoc::FlashIcon

long CMUSHclientDoc::GetFrame ()
  {
  return 0;   // HWND-ish null — L_GetFrame pushes it as a null lightuserdata
  }   // end of CMUSHclientDoc::GetFrame

BSTR CMUSHclientDoc::GetRecentLines (long Count)
  {
  // Join the last Count entries of m_sRecentLines (the old portable twin).
  std::string result;
  if (Count > 0)
    {
    long total = (long) m_sRecentLines.size ();
    long start = total - Count;
    if (start < 0) start = 0;
    for (long i = start; i < total; ++i)
      {
      // Guard on the ACCUMULATOR, not the index — methods_output.cpp:116-117 is
      // `if (!s.empty ()) s += '\n';`.  The two differ in exactly one case: when
      // the oldest line in the window is blank.  Blank lines DO reach here —
      // ProcessPreviousLine.cpp:549 pushes every completed line unconditionally —
      // so with {"", "hp: 100"} the index guard returned "\nhp: 100" where the
      // product returns "hp: 100".  Multi-line trigger matching reads this.
      if (!result.empty ()) result += '\n';
      result += m_sRecentLines [(size_t) i];
      }
    }
  return CString (result.c_str ()).AllocSysString ();
  }   // end of CMUSHclientDoc::GetRecentLines

void CMUSHclientDoc::MoveMainWindow (long Left, long Top, long Width, long Height)
  {
  // GUI-only headless — silent no-op
  }   // end of CMUSHclientDoc::MoveMainWindow

void CMUSHclientDoc::MoveWorldWindowX (long Left, long Top, long Width, long Height, short Which)
  {
  // GUI-only headless — silent no-op
  }   // end of CMUSHclientDoc::MoveWorldWindowX

// MoveWorldWindow and GetNotepadWindowPosition were the only two dispatch-map
// entries with no headless body, which nothing noticed because the dispatch
// table itself (platform/mc_dispatch.cpp) had never been linked into a test
// binary -- the archive member was only pulled in once something referenced
// MC_DispatchInvoke, and until tests/test_mc_dispatch.cpp nothing did.  Adding
// them is what lets that whole substrate be tested at all.
void CMUSHclientDoc::MoveWorldWindow (long Left, long Top, long Width, long Height)
  {
  // GUI-only headless — silent no-op
  }   // end of CMUSHclientDoc::MoveWorldWindow

// GetNotepadWindowPosition is the real body now (methods_notepad.cpp:368),
// adopted with the rest of the notepad family.

void CMUSHclientDoc::Pause (BOOL Flag)
  {
  // Scroll-lock / release the output pane through the IWorldView seam
  // (the old portable twin's behaviour; m_gui_paused feeds GetInfo(114)).
  EmitPaused (Flag != 0);
  m_gui_paused = Flag != 0;
  }   // end of CMUSHclientDoc::Pause

void CMUSHclientDoc::Repaint ()
  {
  // NOT a no-op.  The real body (methods_output.cpp:509-523) opens with
  // UpdateAllViews (NULL) and then walks the views to force an immediate
  // redraw.  Only the walk is GUI-bound: CMUSHclientDoc::UpdateAllViews is the
  // REAL override in doc_display_shared.cpp:175, which mushcore links, and it
  // is what drains the display buffer into pal::IWorldView.  Dropping it meant
  // world.Repaint () did nothing at all headless.
  UpdateAllViews (NULL);
  }   // end of CMUSHclientDoc::Repaint

void CMUSHclientDoc::ResetStatusTime ()
  {
  // GUI-only headless — silent no-op
  }   // end of CMUSHclientDoc::ResetStatusTime

long CMUSHclientDoc::SetBackgroundImage (LPCTSTR FileName, short Mode)
  {
  return eOK;
  }   // end of CMUSHclientDoc::SetBackgroundImage

long CMUSHclientDoc::SetCursor (long Cursor)
  {
  // Same range gate the real port arm applies before it touches a widget
  // (methods_output.cpp: `if (Cursor < -1 || Cursor > 12) return eBadParameter`).
  if (Cursor < -1 || Cursor > 12)
    return eBadParameter;
  return eOK;   // no pane to set a cursor on headless
  }   // end of CMUSHclientDoc::SetCursor

long CMUSHclientDoc::SetForegroundImage (LPCTSTR FileName, short Mode)
  {
  return eOK;
  }   // end of CMUSHclientDoc::SetForegroundImage

void CMUSHclientDoc::SetOutputFont (LPCTSTR FontName, short PointSize)
  {
  // Drive the IWorldView seam as the old portable twin did.
  EmitOutputFont (std::string (FontName ? FontName : ""), PointSize, false, false);
  }   // end of CMUSHclientDoc::SetOutputFont

long CMUSHclientDoc::SetScroll (long Position, BOOL Visible)
  {
  // MUST match the real body in scripting/methods/methods_output.cpp, which is
  // what mushgui links -- this file is test-link-only.  The two drifted once
  // already: this twin dropped Visible on the floor, so GetInfo(120) reported a
  // hard-coded "true" no matter what a script set, and the headless suite could
  // not see the product's behaviour at all.
  //
  // Visible is RECORDED even though the port's pane always shows its scrollbar:
  // m_bScrollBarWanted has no other writer besides doc_construct.cpp:136, and
  // GetInfo(120) reports it.  -2 means "no change" to the position.
  m_bScrollBarWanted = Visible;

  if (Position != -2)
    EmitScroll (Position);
  return eOK;
  }   // end of CMUSHclientDoc::SetScroll

long CMUSHclientDoc::SetToolBarPosition (short Which, BOOL Float, short Side, long Top, long Left)
  {
  // The real body's switch on Which ends in `default: return eBadParameter`
  // (methods_output.cpp) before any toolbar is touched.  There are no toolbars
  // headless, but the validation is not a GUI concern.
  if (Which < 1 || Which > 4)
    return eBadParameter;
  return eOK;
  }   // end of CMUSHclientDoc::SetToolBarPosition

void CMUSHclientDoc::SetWorldWindowStatus (short Parameter)
  {
  // GUI-only headless — silent no-op
  }   // end of CMUSHclientDoc::SetWorldWindowStatus

long CMUSHclientDoc::TextRectangle (long Left, long Top, long Right, long Bottom, long BorderOffset, long BorderColour, long BorderWidth, long OutsideFillColour, long OutsideFillStyle)
  {
  // The real body (methods_output.cpp:520-580) opens with ValidateBrushStyle
  // (miniwindow.cpp, mushgui-only) and has a CMUSHView walk near the end, but
  // everything else in it is portable: the nine assignments are pure document
  // state that world.GetInfo and the wrap-column plumbing both read back, and
  // the last two statements are NOT view-bound at all.
  //
  // The brush validation is NOT replicated here: duplicating it would be the
  // twin drift this file is supposed to be shrinking, and a headless caller
  // has no brush.  So an invalid OutsideFillStyle is accepted here and refused
  // on the GUI build — recorded rather than hidden.
  m_TextRectangle = CRect (Left, Top, Right, Bottom);
  m_TextRectangleBorderOffset      = BorderOffset;
  m_TextRectangleBorderColour      = BorderColour;
  m_TextRectangleBorderWidth       = BorderWidth;
  m_TextRectangleOutsideFillColour = OutsideFillColour;
  m_TextRectangleOutsideFillStyle  = OutsideFillStyle;
  EmitTextRectChanged ();

  // methods_output.cpp:577-578.  SendWindowSizes is doc_shared.cpp:4570 — a
  // mushcore body, not a view call — so dropping it was pure drift: reserving a
  // text rectangle shrinks the usable width, and the MUD has to be told through
  // NAWS.  The headless engine used to skip that renegotiation silently, which
  // also meant no test could ever see a NAWS regression through this path.
  SendWindowSizes (m_nWrapColumn);
  Redraw ();
  return eOK;
  }   // end of CMUSHclientDoc::TextRectangle

BOOL CMUSHclientDoc::Transparency (long Key, short Amount)
  {
  return FALSE;   // no layered windows headless
  }   // end of CMUSHclientDoc::Transparency

// ══════════════════════════════════════════════════════════════════════════
// Accelerator family: NO LONGER STUBBED.  scripting/methods/methods_accelerators.cpp
// is in mushcore now, so the tests exercise the real bodies — which is the point:
// the off-Windows arm of AcceleratorTo used to be unreachable behind a view-list
// gate, and a stub here would have hidden that forever.

#endif  // !_WIN32

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
// Output/view family: NO LONGER STUBBED.  scripting/methods/methods_output.cpp
// (and MakeWindowTransparent.cpp, which Transparency calls) are compiled into
// mushcore now.  That empties this file of CMUSHclientDoc methods entirely.
//
// The two blockers on record for this family both dissolved rather than being
// solved: ValidateBrushStyle arrived with miniwindow.cpp in the previous stage,
// and CMUSHView::AutoWrapWindowWidth is address-only — TextRectangle reaches it
// inside a GetFirstViewPosition walk that yields nothing headless, so it needs a
// definition, not an implementation.
//
// A third "blocker" was a misreading: GetFrame and Transparency dereference
// App.m_pMainWnd unguarded, and that was recorded as a permanent headless
// contract.  It is not.  platform/headless_app.cpp points m_pMainWnd at the real
// CMainFrame that windows_stubs.cpp already owns, whose m_hWnd is null — so both
// answer exactly what their stubs answered (0 and FALSE) with no dereference and
// no invented contract.
//
// Worth knowing about Transparency specifically: it returns FALSE on the PRODUCT
// too, because MakeWindowTransparent opens with LoadLibrary("user32") and the
// shim returns nullptr.  world.Transparency has never done anything on macOS.
// The stub agreed with the product by coincidence; now there is one body, so the
// gap is visible and fixable in one place.  Same shape as world.AddFont
// (LoadLibrary("gdi32")) — see docs/STUB-ADOPTION-2026-08.md §4.

// ══════════════════════════════════════════════════════════════════════════
// Accelerator family: NO LONGER STUBBED.  scripting/methods/methods_accelerators.cpp
// is in mushcore now, so the tests exercise the real bodies — which is the point:
// the off-Windows arm of AcceleratorTo used to be unreachable behind a view-list
// gate, and a stub here would have hidden that forever.

#endif  // !_WIN32

// platform/doc_headless_stubs.cpp — the Stage-3 strong-symbol safety net for
// the Campaign-B header flip (docs/PHASE2-PLAN.md, Stage 3 items 1 & 3).
//
// Build: compiled into mushcore ONLY (MUSHCORE_SOURCES), never into mushapp/
// mushgui.  Like platform/windows_stubs.cpp it is compiled against the REAL
// headers (doc.h / mainfrm.h / MUSHclient.h), like windows_stubs.cpp — so the
// out-of-line definitions here bind the doc.h-declared methods.  During Stages
// 3-6 the retired headless stub's inline bodies still satisfied the TUs that
// included it; these STRONG out-of-line defs coexisted with, and won over,
// those WEAK inline defs (the proven windows_stubs.cpp pattern).  When Stage 4-7
// flip those TUs onto the real headers the weak inlines vanish and these strong
// defs become the sole providers — so mushcore keeps linking across the flip.
//
// ─── Per-symbol duplicate audit (Stage 3 requirement) ─────────────────────────
// Every method in the plan's "12-method post-flip undefined set" was checked
// against the mushcore-compiled TUs (windows_stubs.cpp / mushapp_stubs.cpp is
// mushapp-only / doc_portable.cpp / doc_gui_method_stubs.cpp).  Symbols that
// ALREADY carry a strong def in a mushcore TU are NOT redefined here (a
// strong-vs-strong collision is a link error) — they are listed below and are
// relocated/superseded when their current provider is deleted in Stage 5/7:
//
//   Redraw                  → strong in platform/windows_stubs.cpp:195
//   GetChildFrame           → strong in doc_portable.cpp:537
//   GetWorldWindowPositionX → strong in doc_portable.cpp:616
//   FindAllEntryPoints      → strong in doc_portable.cpp:748
//
// The remainder (below) were satisfied in mushcore pre-flip only by the stub's weak
// inlines (or, for the free ShowHelp/ShowFunctionslist, not at all — they are
// net-new), so a strong def here is safe and is the actual safety net.

#ifndef _WIN32

// Same real-header include order as windows_stubs.cpp, so CMUSHclientDoc /
// CMainFrame are the full doc.h / mainfrm.h shapes (mainfrm.h → mainfrm_stub.h
// off-Windows) and the free-function decls come from stdafx.h.
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "MUSHview.h"
#include "sendvw.h"
#include "mainfrm.h"
#include "paneline.h"
#include "platform/wx_mfc_hooks.h"   // g_wxhook_activate_notepad

// ════════════════════════════════════════════════════════════════════════════
// Item 1 — CMUSHclientDoc methods whose real bodies live in TUs mushcore never
// compiles (scripting/methods/methods_notepad.cpp, timers.cpp #ifdef _WIN32,
// scripting/scripting.cpp, the view no-op).  All are doc.h-declared.
// ════════════════════════════════════════════════════════════════════════════

// ActivateNotepad is the REAL body now (methods_notepad.cpp:210), adopted with
// the rest of the notepad family.

// SortTimers — timers.cpp:431 is #ifdef _WIN32 (rebuilds a sorted display list);
// the headless timer map is iterated directly, so this is a no-op.  Body copied
// from the mushapp no-op at platform/mushapp_stubs.cpp:358-361 (mushapp-only).
void CMUSHclientDoc::SortTimers ()
{
}

// ShowErrorLines / WriteErrorLines — scripting/scripting.cpp:450 / :490 show the
// script file around an error point.  Headless there is no script editor / error
// window, so both are no-ops (matching the retired headless-stub inlines).
void CMUSHclientDoc::ShowErrorLines (const int /*iLine*/)
{
}

void CMUSHclientDoc::WriteErrorLines (const int /*iLine*/, FILE * /*f*/)
{
}

// DrawMappingStatusLine — the real body (Mapping.cpp:258) pushes the speedwalk
// preview to Frame.SetStatusMessageNow().  Headless there is no status bar, so
// this is a no-op (matching the retired headless-stub inline).  NB: the AddToMap
// adoption in Unit B calls this; pre-flip it resolved to the stub's weak inline,
// against a flipped TU it resolves here — both are the same no-op.
void CMUSHclientDoc::DrawMappingStatusLine (void)
{
}

// ════════════════════════════════════════════════════════════════════════════
// Item 1 — free functions ::ShowHelp / ::ShowFunctionslist (declared
// stdafx.h:799/802).  Real bodies live in scripting/functionlist.cpp, which is
// mushapp-only (it also defines CMainFrame::OnGameFunctionslist).  Net-new in
// mushcore: ShowHelp is exactly the port arm at functionlist.cpp:633; the
// headless ShowFunctionslist has no function-list window to populate.
// ════════════════════════════════════════════════════════════════════════════

void ShowFunctionslist (CString & /*strSelection*/, int /*nStartChar*/,
                        int /*nEndChar*/, const bool /*bLua*/)
{
}

void ShowHelp (const CString /*strPrefix*/, const CString /*strTopic*/)
{
}

// ════════════════════════════════════════════════════════════════════════════
// Item 3 — CMainFrame methods the doc-side call surface reaches.  mushcore
// converges on real mainfrm.h (→ mainfrm_stub.h off-Windows).  windows_stubs.cpp
// already strong-defines SetStatusMessage / SetStatusMessageNow (:71/:75); the
// two remaining ~15-set members with inline bodies in mainfrm_stub.h —
// IsFullScreen (:172) and PostMessage (:185) — CANNOT be redefined out-of-line
// (that is an in-class-inline redefinition), so they stay on their weak inlines.
// The 8 below are declared-only in mainfrm_stub.h and undefined in mushcore
// today; their real bodies are in mainfrm.cpp (mushapp-only).  Headless no-ops:
// ════════════════════════════════════════════════════════════════════════════

void CMainFrame::SetStatusNormal ()
{
}

void CMainFrame::ReturnStatusRect (CRect & /*rect*/)
{
}

void CMainFrame::ReturnToolbarRect (CRect & /*rect*/)
{
}

void CMainFrame::FixUpTitleBar ()
{
}

void CMainFrame::OnUpdateBtnWorlds (int /*iWorld*/, CCmdUI * /*pCmdUI*/)
{
}

void CMainFrame::CancelSound ()
{
}

// The real doc.cpp PlaySoundFile routes through Frame.PlaySoundFile (the MCI
// path on Windows).  Off-Windows this Frame stub is the sound seam: route it to
// the wx sound hook (installed by mushgui / the test) so trigger + MSP + world.-
// Sound audio still plays — the g_wxhook path the retired doc_portable.cpp
// PlaySoundFile used (plan Stage 7.2: Frame-level no-op + g_wxhook_sound path).
bool CMainFrame::PlaySoundFile (const CString & strSound)
{
    if (g_wxhook_play_sound)
        return g_wxhook_play_sound (static_cast<const char*> (strSound), false);
    return false;   // headless: no audio backend
}

void CMainFrame::CheckTimerFallback ()
{
}

#endif // _WIN32

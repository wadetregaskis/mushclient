// doc_display_shared.cpp
//
// The push-side display drain for the non-Windows builds: the off-Windows
// CMUSHclientDoc::UpdateAllViews override walks m_LineList from the
// m_gui_lines_emitted cursor and pushes each completed line as an SOutputLine
// through the IWorldView seam (plus the in-progress line as a pending prompt
// row).  Windows keeps the MFC CDocument::UpdateAllViews + CMUSHView pull —
// the deliberate Option A divergence recorded in docs/PAL.md — so this TU is
// NOT in MUSHclient.vcxproj.
//
// Relocated verbatim from platform/mushapp_stubs.cpp (campaign-b display
// step 4) and shared with mushcore since the display flip (step 7): both
// non-Windows builds emit through this one drain — the twin per-'\n' push
// and the no-op UpdateAllViews are gone.

#ifndef _WIN32

  #include "stdafx.h"
  #include "MUSHclient.h"
  #include "doc.h"
  #include "OtherTypes.h"   // CLine, CStyle, HILITE, UNDERLINE

#include <string>
#include <utility>

// ── CMUSHclientDoc::UpdateAllViews ────────────────────────────────────────────
// Emits completed lines to the wxWidgets output pane via the IWorldView seam.
// Walk m_LineList from m_gui_lines_emitted, stop before m_pCurrentLine
// (the line still being built).  Each CStyle run → SOutputRun.
//
// Uses BeginOutputBatch / EndOutputBatch (BeginBatch / EndBatch on the pane) to
// accumulate all new lines and trigger a single scrollbar update + repaint
// rather than one per line.

// ── CMUSHclientDoc::RebuildOutputView ────────────────────────────────────────
// Re-resolve the whole scrollback through the current palette.
//
// Rewind-and-re-drain, reusing exactly the mechanism ClearOutput already uses
// (doc.cpp: EmitClearOutput() then m_gui_lines_emitted = 0), so there is only
// one way to reset the emission cursor.  Every line then comes back through
// GetStyleRGB against the live m_normalcolour / m_boldcolour.
//
// The caller owns preserving anything the view cares about across this -- the
// wx frame snapshots the scroll position -- because what is worth keeping is a
// frontend question, not a doc one.
//
// NO PRODUCTION CALLER: a palette change used to come through here, and now
// recolours in place instead (COutputPane::RecolourFromPalette, driven by
// CWorldChildFrame's palette poll) -- runs carry the style inputs their colours
// were resolved from, so nothing has to be re-emitted.  Retained, and exercised
// only by tests/test_pal_worldview.cpp, because it is the sole exerciser of the
// EmitClearOutput + m_gui_lines_emitted rewind that ClearOutput also relies on.
// Whether it should go at all is a separate call (ledger K-750).
void CMUSHclientDoc::RebuildOutputView()
{
    if (!HaveOutputSink()) return;

    EmitClearOutput();
    m_gui_lines_emitted = 0;
    UpdateAllViews(nullptr);
}

// ── BuildLineFromCLine ─────────────────────────────────────────
// One CLine -> one SOutputLine.  THE conversion: BuildOutputLine (the pull
// path) and both of UpdateAllViews' emit loops (the completed lines and the
// in-progress row) all come through here.  They used to be three hand-written
// copies of the same run loop -- while the comment claimed there was one -- and
// they had already drifted: neither drain copy set userInput, so an echoed
// command reached the view as ordinary output on the push path and as input on
// the pull one.  That is the twin drift this port keeps paying for elsewhere.
//
// A file-static rather than a member, so no doc.h change is needed; everything
// it reads off the document is public.  `out` is fully overwritten, flags
// included, so no caller has to clear it first.
static void BuildLineFromCLine (const CMUSHclientDoc & doc,
                                const CLine * pLine,
                                SOutputLine & out)
{
    out.clear();
    out.pending = false;
    // When the line arrived, for the output-buffer fade (the view has no CLine).
    // The in-progress row needs it as much as a completed line does: it is the
    // one a prompt sits in, so leaving it unstamped made the NEWEST line on
    // screen look ancient to the fade.
    out.arrived = pLine->m_theTime.GetTime ();
    // A note line: a send-link on it is executed, not sent.
    out.comment = (pLine->flags & COMMENT) != 0;
    // An echoed command: the line-preamble (timestamp) family picks its text
    // and colours by kind — note / input / output.
    out.userInput = (pLine->flags & USER_INPUT) != 0;
    // The engine's BOOKMARK flag (world.Bookmark / Display ▸ Bookmark
    // Selection).  Carrying it here makes the CLine the single source of
    // truth, so a view that re-reads a line -- the pull path's on-demand
    // fetch, or a RebuildOutputView re-emit -- gets the bookmark back without
    // having to save and restore it itself.
    out.bookmark = (pLine->flags & BOOKMARK) != 0;

    // Horizontal rule: emit a single sentinel run rather than text.
    if (pLine->flags & HORIZ_RULE) {
        SOutputRun hrRun;
        hrRun.horiz_rule = true;
        out.push_back(std::move(hrRun));
        return;
    }

    POSITION spos = pLine->styleList.GetHeadPosition();
    int charOff = 0;
    while (spos) {
        CStyle* pStyle = pLine->styleList.GetNext(spos);
        if (pStyle->iLength > 0) {
            SOutputRun run;
            doc.GetStyleRGB(pStyle, run.fg, run.bg);
            // ...and the inputs it resolved them from, so a palette
            // change can recolour this run without a re-emit.
            run.style_flags = (uint16_t)(pStyle->iFlags & STYLE_BITS);
            run.fore_idx    = (int32_t) pStyle->iForeColour;
            run.back_idx    = (int32_t) pStyle->iBackColour;
            // The three "Show" checkboxes (World Properties > Appearance >
            // Output) decide whether the ANSI attribute is rendered at all.
            // Windows honours them when it builds the 16 style fonts
            // (doc.cpp ChangeFont, which takes bShowBold/Italic/Underline);
            // these run flags are the equivalent choke point for a PAL view
            // adapter, and were ungated — so on the port the checkboxes did
            // nothing.  UpdateAllViews re-emits, so this is live.
            run.bold          = doc.m_bShowBold      && ((pStyle->iFlags & HILITE)    != 0);
            run.underline     = doc.m_bShowUnderline && ((pStyle->iFlags & UNDERLINE) != 0);
            run.italic        = doc.m_bShowItalic    && ((pStyle->iFlags & BLINK)     != 0);  // MUSHclient maps BLINK → italic
            run.strikethrough = (pStyle->iFlags & STRIKEOUT) != 0;
            run.text = std::string(pLine->text + charOff, pStyle->iLength);
            run.utf8 = doc.m_bUTF_8;                      // non-UTF-8 worlds: tag the
            if (!doc.m_bUTF_8)                            // bytes' charset so the pane
                run.charset = (int) doc.m_font_charset;   // decodes them (else blank)
            // MXP hyperlink / send-link: copy action and hint strings.
            if (pStyle->pAction) {
                // One shared_ptr allocation, and ONLY for a link run: the
                // action/hint/type used to be plain members on every run.
                // Windows branches on these when the link is clicked (a
                // prompt-link fills the command bar; a send-link on a note
                // line executes instead of sending), so carry them out to
                // the view with the action itself.
                run.setLink(static_cast<const char*>(pStyle->pAction->m_strAction),
                            static_cast<const char*>(pStyle->pAction->m_strHint),
                            pStyle->iFlags & ACTIONTYPE);
            }
            out.push_back(std::move(run));
        }
        charOff += pStyle->iLength;
    }
}

// ── EmitPendingClear ───────────────────────────────────────────
// Tell the view the in-progress row has GONE.
//
// The view holds the pending row until something replaces it, and the only
// thing that does is a COMPLETED line (COutputPane::AppendLine drops
// m_pendingLine when a non-pending one arrives).  So when the engine EMPTIES
// the current line instead — a '\r' under carriage_return_clears_line
// (doc_shared.cpp) sets len back to 0, and a gag deletes the CLine outright —
// nothing said so, and the partial text kept painting as a dim ghost row until
// some later completed line happened to land.  In the gag case that means text
// the user asked to hide stayed on screen.
//
// An empty pending SOutputLine IS the clear: AppendLine's pending branch stores
// whatever it is given, and the paint's `!m_pendingLine.empty()` guard then
// draws nothing.  Callers gate on the OLD fingerprint, so an idle drain (no row
// before, none now) still emits nothing at all.
static void EmitPendingClear (CMUSHclientDoc & doc)
{
    SOutputLine gone;
    gone.pending = true;
    doc.EmitOutputLine (std::move (gone));
}

// ── BuildOutputLine ─────────────────────────────────────────────
// The PULL path: a view fetching line k at paint time (COutputPane::LineAt →
// m_fn_fetch_line → here), so a line is rebuilt from its CLine on demand.
//
// Returns false when `index` is out of range.  `index` is a 0-based COMMITTED
// line index, the same space m_gui_lines_emitted counts in.
bool CMUSHclientDoc::BuildOutputLine (long index, SOutputLine & out) const
{
  out.clear();
  out.pending = false;
  out.arrived = 0;
  if (index < 0 || index >= m_LineList.GetCount ())
    return false;

  CLine * pLine = m_LineList.GetAt (
      const_cast<CMUSHclientDoc *>(this)->GetLinePosition (index));
  if (!pLine || pLine == m_pCurrentLine)
    return false;

  BuildLineFromCLine (*this, pLine, out);
  return true;
}

// ── HasUnflushedOutput ────────────────────────────────────────────────────
// Would UpdateAllViews actually push anything?  See the header for why this is
// for the periodic timer flush ONLY.
//
// Without it the world timer drained (and therefore repainted) 10 times a
// second forever: the drain's fast-exit path still ended in EmitRepaint, so an
// idle, connected world with a prompt on screen burned CPU continuously with
// nothing on screen changing.  That is a battery cost at today's default 5,000
// lines, quite apart from being the multiplier on every O(buffer) path the
// unbounded-scrollback work is trying to remove.
bool CMUSHclientDoc::HasUnflushedOutput () const
{
    if (!HaveOutputSink ())
        return false;

    // A completed line the view has not seen yet.
    //
    // m_gui_lines_emitted counts COMPLETED lines, and the in-progress line is
    // always the list's tail (StartNewLine appends it) but is never emitted as
    // a completed one -- so the steady-state difference is 1, not 0.  Comparing
    // against the raw count instead makes this arm permanently true and the
    // idle skip never engages.  != rather than > so a rewind (RebuildOutputView
    // sets the cursor back to 0) also reports work.
    const long committed = m_LineList.GetCount () - (m_pCurrentLine ? 1 : 0);
    if (committed != m_gui_lines_emitted)
        return true;

    // ...or the in-progress row appearing, growing, restyling or vanishing.
    const long fingerprint =
        (m_pCurrentLine && m_pCurrentLine->len > 0)
        ? (long) m_pCurrentLine->len * 31 +
          (long) m_pCurrentLine->styleList.GetCount () + 1
        : 0;

    return fingerprint != m_iPendingFingerprint;
}

void CMUSHclientDoc::UpdateAllViews(void* /*hint*/)
{
    if (!HaveOutputSink()) return;

    // Resume at the first line the GUI has not seen yet.
    //
    // That line is always in the TAIL of the list — everything before it has
    // been emitted — so walk BACKWARDS from the tail by the number of
    // un-emitted lines, rather than forwards from the head by the number of
    // emitted ones.  Same landing point, but the cost is proportional to the
    // new lines in this packet instead of to the whole scrollback.
    //
    // This matters because of how often the drain runs: once per received
    // packet (doc_shared.cpp OnBytesReceived), once per 100 ms world timer
    // tick, and once per Note / WindowCreate / WindowShow / … scripting call.
    // Skipping forwards meant a plugin doing a dozen Window* calls per prompt
    // re-walked the entire buffer a dozen times per prompt.
    //
    // Deliberately NOT a cached POSITION: RemoveChunk deletes head nodes and
    // ClearOutput deletes all of them, so a stored cursor would be a dangling
    // pointer waiting for one more mutation site to be added without updating
    // it.  Counting from the tail is derived fresh every call and cannot go
    // stale.
    const long total  = m_LineList.GetCount();
    const long toEmit = total - m_gui_lines_emitted;

    POSITION pos = nullptr;
    if (toEmit > 0) {
        pos = m_LineList.GetTailPosition();
        for (long i = 1; i < toEmit && pos; ++i)
            m_LineList.GetPrev(pos);
    }

    // Fast-exit: nothing pending — but still repaint so that operations that
    // change the visual state without emitting a new line (e.g. RemoveChunk
    // after a buffer-size change, or a miniwindow deletion) still refresh the
    // canvas.
    if (!pos) {
        // Nothing at or after the cursor means there is no in-progress row
        // either (m_pCurrentLine is the tail when it exists), so the pending
        // fingerprint must not be left stale — HasUnflushedOutput would then
        // report a phantom change on every tick and defeat the idle skip.
        //
        // This is where a GAGGED prompt lands: ProcessPreviousLine deleted the
        // CLine the view is still showing, so the row has no owner left and the
        // view has to be told (EmitPendingClear).
        if (m_iPendingFingerprint != 0)
            EmitPendingClear (*this);
        m_iPendingFingerprint = 0;
        EmitRepaint();
        return;
    }

    bool batchOpen     = false;
    bool emittedPending = false;

    // Emit each completed (non-current) line.
    while (pos) {
        CLine* pLine = m_LineList.GetNext(pos);
        if (pLine == m_pCurrentLine) break;  // in-progress — stop here

        // Open a paint batch on the first line we actually emit.
        if (!batchOpen) {
            BeginOutputBatch();
            batchOpen = true;
        }

        SOutputLine outline;
        BuildLineFromCLine (*this, pLine, outline);
        EmitOutputLine(std::move(outline));
        ++m_gui_lines_emitted;
    }

    // NOTE: the batch stays OPEN across the pending-row emit below and is
    // closed after it.  It used to close here, so a packet that both completed
    // a line and left a prompt behind — the ordinary shape of MUD output — paid
    // TWO scrollbar updates and two invalidations per packet: one from
    // EndOutputBatch and another from the pending AppendLine landing outside
    // any batch.  Every packet, all day.

    // ── Emit the in-progress line as a pending update ──────────────────────
    // m_pCurrentLine is the line still being assembled (no trailing '\n' yet).
    // MUDs often send a prompt without a newline (e.g. "> " or "Press enter
    // to continue") — we emit it as a pending SOutputLine so the output pane
    // can display it as a dim trailing row even before the '\n' arrives.
    // Re-emitting on every UpdateAllViews call is cheap: AppendLine just
    // replaces m_pendingLine and schedules a single Refresh().
    if (m_pCurrentLine && m_pCurrentLine->len > 0) {
        SOutputLine pending;
        BuildLineFromCLine (*this, m_pCurrentLine, pending);
        if (!pending.empty()) {
            pending.pending = true;
            EmitOutputLine(std::move(pending));
            emittedPending = true;
        }
        // Remember what the pending row looked like, so the world timer can
        // tell "the prompt grew" from "nothing happened" (HasUnflushedOutput).
        m_iPendingFingerprint =
            (long) m_pCurrentLine->len * 31 +
            (long) m_pCurrentLine->styleList.GetCount () + 1;   // never 0
    }
    else {
        // The row is gone rather than merely absent: it was on screen last
        // drain (the fingerprint says so) and the current line is now empty, so
        // emit the clear.  Counting it as an emit keeps the repaint arithmetic
        // below honest — AppendLine has already invalidated for it.
        if (m_iPendingFingerprint != 0) {
            EmitPendingClear (*this);
            emittedPending = true;
        }
        m_iPendingFingerprint = 0;   // no pending row on screen
    }

    // Close the batch: ONE scrollbar update and ONE invalidation for everything
    // this call emitted, completed lines and the prompt row together.
    if (batchOpen) EndOutputBatch();

    // ...and repaint for the callers that changed visual state without emitting
    // anything at all — world.WindowShow / WindowPosition / WindowCreate and
    // friends call UpdateAllViews purely to get the canvas recomposited.
    //
    // Skipped when something WAS emitted, because both paths above already
    // invalidated: EndOutputBatch does UpdateScrollbars + Refresh, and an
    // unbatched pending row does the same inside AppendLine.  Calling
    // EmitRepaint anyway just added a third invalidation per packet.
    if (!batchOpen && !emittedPending)
        EmitRepaint();
}

#endif // !_WIN32

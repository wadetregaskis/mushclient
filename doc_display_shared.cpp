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
void CMUSHclientDoc::RebuildOutputView()
{
    if (!HaveOutputSink()) return;

    EmitClearOutput();
    m_gui_lines_emitted = 0;
    UpdateAllViews(nullptr);
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

        // Horizontal rule: emit a single sentinel run rather than text.
        if (pLine->flags & HORIZ_RULE) {
            SOutputLine outline;
            SOutputRun hrRun;
            hrRun.horiz_rule = true;
            outline.push_back(std::move(hrRun));
            EmitOutputLine(std::move(outline));
            ++m_gui_lines_emitted;
            continue;
        }

        SOutputLine outline;
        // A note line: a send-link on it is executed, not sent.
        outline.comment = (pLine->flags & COMMENT) != 0;
        // The engine's BOOKMARK flag (world.Bookmark / Display ▸ Bookmark
        // Selection).  Carrying it here makes the CLine the single source of
        // truth: RebuildOutputView re-emits the buffer, so bookmarks survive a
        // palette change without the view having to save and restore them.
        outline.bookmark = (pLine->flags & BOOKMARK) != 0;
        POSITION spos = pLine->styleList.GetHeadPosition();
        int charOff = 0;
        while (spos) {
            CStyle* pStyle = pLine->styleList.GetNext(spos);
            if (pStyle->iLength > 0) {
                SOutputRun run;
                GetStyleRGB(pStyle, run.fg, run.bg);
                // The three "Show" checkboxes (World Properties > Appearance >
                // Output) decide whether the ANSI attribute is rendered at all.
                // Windows honours them when it builds the 16 style fonts
                // (doc.cpp ChangeFont, which takes bShowBold/Italic/Underline);
                // these run flags are the equivalent choke point for a PAL view
                // adapter, and were ungated — so on the port the checkboxes did
                // nothing.  UpdateAllViews re-emits, so this is live.
                run.bold          = m_bShowBold      && ((pStyle->iFlags & HILITE)    != 0);
                run.underline     = m_bShowUnderline && ((pStyle->iFlags & UNDERLINE) != 0);
                run.italic        = m_bShowItalic    && ((pStyle->iFlags & BLINK)     != 0);  // MUSHclient maps BLINK → italic
                run.strikethrough = (pStyle->iFlags & STRIKEOUT) != 0;
                run.text = std::string(pLine->text + charOff, pStyle->iLength);
                run.utf8 = m_bUTF_8;                      // non-UTF-8 worlds: tag the
                if (!m_bUTF_8)                            // bytes' charset so the pane
                    run.charset = (int) m_font_charset;   // decodes them (else blank)
                // MXP hyperlink / send-link: copy action and hint strings.
                if (pStyle->pAction) {
                    run.action = static_cast<const char*>(pStyle->pAction->m_strAction);
                    run.hint   = static_cast<const char*>(pStyle->pAction->m_strHint);
                    // Windows branches on these when the link is clicked (a
                    // prompt-link fills the command bar; a send-link on a note
                    // line executes instead of sending), so carry them out to
                    // the view with the action itself.
                    run.action_type = pStyle->iFlags & ACTIONTYPE;
                }
                outline.push_back(std::move(run));
            }
            charOff += pStyle->iLength;
        }

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
        pending.comment = (m_pCurrentLine->flags & COMMENT) != 0;
        POSITION spos = m_pCurrentLine->styleList.GetHeadPosition();
        int charOff = 0;
        while (spos) {
            CStyle* pStyle = m_pCurrentLine->styleList.GetNext(spos);
            if (pStyle->iLength > 0) {
                SOutputRun run;
                GetStyleRGB(pStyle, run.fg, run.bg);
                // Same "Show" gates as the finished-line emitter above.
                run.bold          = m_bShowBold      && ((pStyle->iFlags & HILITE)    != 0);
                run.underline     = m_bShowUnderline && ((pStyle->iFlags & UNDERLINE) != 0);
                run.italic        = m_bShowItalic    && ((pStyle->iFlags & BLINK)     != 0);
                run.strikethrough = (pStyle->iFlags & STRIKEOUT) != 0;
                run.text = std::string(m_pCurrentLine->text + charOff, pStyle->iLength);
                run.utf8 = m_bUTF_8;
                if (!m_bUTF_8) run.charset = (int) m_font_charset;
                if (pStyle->pAction) {
                    run.action = static_cast<const char*>(pStyle->pAction->m_strAction);
                    run.hint   = static_cast<const char*>(pStyle->pAction->m_strHint);
                    // Windows branches on these when the link is clicked (a
                    // prompt-link fills the command bar; a send-link on a note
                    // line executes instead of sending), so carry them out to
                    // the view with the action itself.
                    run.action_type = pStyle->iFlags & ACTIONTYPE;
                }
                pending.push_back(std::move(run));
            }
            charOff += pStyle->iLength;
        }
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
    else
        m_iPendingFingerprint = 0;   // no pending row on screen

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

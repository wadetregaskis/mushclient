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
void CMUSHclientDoc::UpdateAllViews(void* /*hint*/)
{
    if (!HaveOutputSink()) return;

    // Skip lines already forwarded to the GUI.
    long toSkip = m_gui_lines_emitted;
    POSITION pos = m_LineList.GetHeadPosition();
    while (pos && toSkip > 0) {
        m_LineList.GetNext(pos);
        --toSkip;
    }

    // Fast-exit: nothing pending — but still repaint so that operations that
    // change the visual state without emitting a new line (e.g. RemoveChunk
    // after a buffer-size change, or a miniwindow deletion) still refresh the
    // canvas.
    if (!pos) {
        EmitRepaint();
        return;
    }

    bool batchOpen = false;

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
        POSITION spos = pLine->styleList.GetHeadPosition();
        int charOff = 0;
        while (spos) {
            CStyle* pStyle = pLine->styleList.GetNext(spos);
            if (pStyle->iLength > 0) {
                SOutputRun run;
                GetStyleRGB(pStyle, run.fg, run.bg);
                run.bold          = (pStyle->iFlags & HILITE)    != 0;
                run.underline     = (pStyle->iFlags & UNDERLINE) != 0;
                run.italic        = (pStyle->iFlags & BLINK)     != 0;  // MUSHclient maps BLINK → italic
                run.strikethrough = (pStyle->iFlags & STRIKEOUT) != 0;
                run.text = std::string(pLine->text + charOff, pStyle->iLength);
                run.utf8 = m_bUTF_8;                      // non-UTF-8 worlds: tag the
                if (!m_bUTF_8)                            // bytes' charset so the pane
                    run.charset = (int) m_font_charset;   // decodes them (else blank)
                // MXP hyperlink / send-link: copy action and hint strings.
                if (pStyle->pAction) {
                    run.action = static_cast<const char*>(pStyle->pAction->m_strAction);
                    run.hint   = static_cast<const char*>(pStyle->pAction->m_strHint);
                }
                outline.push_back(std::move(run));
            }
            charOff += pStyle->iLength;
        }

        EmitOutputLine(std::move(outline));
        ++m_gui_lines_emitted;
    }

    // Close the batch (triggers a single scrollbar update + repaint).
    if (batchOpen) EndOutputBatch();

    // ── Emit the in-progress line as a pending update ──────────────────────
    // m_pCurrentLine is the line still being assembled (no trailing '\n' yet).
    // MUDs often send a prompt without a newline (e.g. "> " or "Press enter
    // to continue") — we emit it as a pending SOutputLine so the output pane
    // can display it as a dim trailing row even before the '\n' arrives.
    // Re-emitting on every UpdateAllViews call is cheap: AppendLine just
    // replaces m_pendingLine and schedules a single Refresh().
    if (m_pCurrentLine && m_pCurrentLine->len > 0) {
        SOutputLine pending;
        POSITION spos = m_pCurrentLine->styleList.GetHeadPosition();
        int charOff = 0;
        while (spos) {
            CStyle* pStyle = m_pCurrentLine->styleList.GetNext(spos);
            if (pStyle->iLength > 0) {
                SOutputRun run;
                GetStyleRGB(pStyle, run.fg, run.bg);
                run.bold          = (pStyle->iFlags & HILITE)    != 0;
                run.underline     = (pStyle->iFlags & UNDERLINE) != 0;
                run.italic        = (pStyle->iFlags & BLINK)     != 0;
                run.strikethrough = (pStyle->iFlags & STRIKEOUT) != 0;
                run.text = std::string(m_pCurrentLine->text + charOff, pStyle->iLength);
                run.utf8 = m_bUTF_8;
                if (!m_bUTF_8) run.charset = (int) m_font_charset;
                if (pStyle->pAction) {
                    run.action = static_cast<const char*>(pStyle->pAction->m_strAction);
                    run.hint   = static_cast<const char*>(pStyle->pAction->m_strHint);
                }
                pending.push_back(std::move(run));
            }
            charOff += pStyle->iLength;
        }
        if (!pending.empty()) {
            pending.pending = true;
            EmitOutputLine(std::move(pending));
        }
    }

    // Always trigger a repaint so miniwindow operations (WindowCreate,
    // WindowShow, WindowPosition, …) that call UpdateAllViews without emitting
    // any new text lines still refresh the canvas.
    EmitRepaint();
}

#endif // !_WIN32

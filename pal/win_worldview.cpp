// pal/win_worldview.cpp — the native Windows pal::IWorldView adapter (WV-1).
//
// Every method body below is the exact `#ifdef _WIN32` branch of its engine
// call site, relocated verbatim (view loops and all) with the doc reached
// through m_pDoc.  Source sites, for review:
//
//   setTitle            methods_output.cpp  SetTitle (FixupTitle view loop)
//   setStatus           methods_utilities.cpp SetStatus (m_strStatusMessage +
//                       ShowStatusLine — the product path; the seam route is
//                       dormant in the product on every platform)
//   setPaused           methods_output.cpp  Pause (m_freeze view loop)
//   clearOutput         doc.cpp             ClearOutput (scroll-home + addedstuff)
//   setScroll           methods_output.cpp  SetScroll (scroll half; the seam
//                       carries no Visible param — visibility is unchanged)
//   deleteLines         methods_output.cpp  DeleteLines (addedstuff view loop)
//   setOutputSelection  methods_utilities.cpp SetSelection
//   setOutputFont       methods_output.cpp  SetOutputFont (ChangeFont)
//   setInputFont        methods_commands.cpp SetInputFont (ChangeInputFont)
//   outputSelection     methods_info.cpp    GetSelection{Start,End}{Line,Column}
//   scrollPos           methods_info.cpp    GetInfo 296
//   currentCommand      methods_commands.cpp GetCommand
//   setCommand          methods_commands.cpp SetCommand
//   selectCommand       methods_commands.cpp SelectCommand
//   commandSelection    methods_info.cpp    GetInfo 236/237
//   setCommandSelection methods_commands.cpp SetCommandSelection
//   pushCommand         methods_commands.cpp PushCommand
//   commandList         methods_commands.cpp GetCommandList
//   addToCommandHistory methods_sending.cpp AddToCommandHistory
//   deleteCommandHistory methods_commands.cpp DeleteCommandHistory
//   setMainTitle        methods_output.cpp  SetMainTitle (Frame.FixUpTitleBar)
//   moveWorldWindow     methods_output.cpp  MoveWorldWindowX (Which == 1)
//   setWindowStatus     methods_output.cpp  SetWorldWindowStatus
//   activateClient      methods_output.cpp  ActivateClient
//   isFullscreen        methods_info.cpp    GetInfo 125 (Frame.IsFullScreen)
//   saveWorld           methods_utilities.cpp Save ("" name: !DoSave(path))
//   outputRect          methods_output.cpp  GetWorldWindowPositionX (Which 1)
//   mainRect            methods_info.cpp    GetMainWindowPosition
//   infobar*            methods_infobar.cpp ShowInfoBar/InfoClear/Info/
//                       InfoColour/InfoBackground/InfoFont
//   flash               methods_output.cpp  FlashIcon (Frame.FlashWindow)
//
// DISPLAY FAMILY (dormant until its flip, which comes LAST): Windows paints
// by pull — DisplayMsg fills the doc's line list and UpdateAllViews
// invalidates CMUSHView, which re-reads the list in OnDraw.  appendLine /
// requestRepaint therefore reduce to the invalidation the old flow
// performed; note() is engine-side lines on Windows; beginBatch/endBatch
// have no Windows counterpart (paint coalescing already batches).
//
// CANNOT be compiled here (no MSVC) — by-reasoning parity, reviewed as a
// diff against the site forks named above.

#ifdef _WIN32

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "MUSHview.h"
#include "sendvw.h"
#include "mainfrm.h"
#include "winplace.h"   // CWindowPlacement — outputRect/mainRect GetWindowPlacement

#include "pal/win_worldview.h"

namespace pal {

class CWinWorldView : public IWorldView {
public:
    explicit CWinWorldView(CMUSHclientDoc* pDoc) : m_pDoc(pDoc) {}

    // ── display (dormant until the display-family flip — see header) ──────
    void appendLine(SOutputLine&& /*line*/) override
    {
        // Windows pulls line content from the doc's list at paint time; the
        // push carries no extra information.  The old flow's effect was the
        // UpdateAllViews invalidation.
        m_pDoc->UpdateAllViews(NULL);
    }

    void note(const char* /*text*/) override
    {
        // Notes become ordinary lines engine-side on Windows (Note() appends
        // to the line list before any view involvement) — nothing to do.
    }

    void setTitle(const char* /*title*/) override
    {
        // now update all views  (SetTitle — reads m_pDoc->m_strWindowTitle,
        // already stored by the engine site before the emit)
        for (POSITION pos = m_pDoc->GetFirstViewPosition(); pos != NULL; )
        {
            CView* pView = m_pDoc->GetNextView(pos);

            if (pView->IsKindOf(RUNTIME_CLASS(CMUSHView)))
            {
                CMUSHView* pmyView = (CMUSHView*)pView;
                pmyView->FixupTitle();
            }
        }
    }

    void setStatus(const char* text) override
    {
        m_pDoc->m_strStatusMessage = text;
        m_pDoc->ShowStatusLine(true);    // show it now
    }

    void setBackgroundColour(Colour /*c*/) override
    {
        // the engine site (SetBackgroundColour) stores the colour and calls
        // UpdateAllViews itself outside the fork — nothing view-side to do
    }

    void setMaxLines(int /*n*/) override
    {
        // the buffer trim is engine-side; Windows views recompute scroll
        // sizes from the list on the next paint — nothing to do
    }

    void requestRepaint(Rect /*area*/) override
    {
        // Repaint (methods_output.cpp)
        m_pDoc->UpdateAllViews(NULL);

        for (POSITION pos = m_pDoc->GetFirstViewPosition(); pos != NULL; )
        {
            CView* pView = m_pDoc->GetNextView(pos);

            if (pView->IsKindOf(RUNTIME_CLASS(CMUSHView)))
            {
                CMUSHView* pmyView = (CMUSHView*)pView;
                pmyView->UpdateWindow();
            }
        }
    }

    void clearOutput() override
    {
        // ClearOutput (doc.cpp): scroll home + resync scrollbar state
        for (POSITION pos = m_pDoc->GetFirstViewPosition(); pos != NULL; )
        {
            CView* pView = m_pDoc->GetNextView(pos);

            if (pView->IsKindOf(RUNTIME_CLASS(CMUSHView)))
            {
                CMUSHView* pmyView = (CMUSHView*)pView;

                pmyView->ScrollToPosition(CPoint(0, 0), false);
                pmyView->addedstuff();
            }
        }
    }

    void setPaused(bool paused) override
    {
        // Pause (methods_output.cpp)
        for (POSITION pos = m_pDoc->GetFirstViewPosition(); pos != NULL; )
        {
            CView* pView = m_pDoc->GetNextView(pos);

            if (pView->IsKindOf(RUNTIME_CLASS(CMUSHView)))
            {
                CMUSHView* pmyView = (CMUSHView*)pView;
                pmyView->m_freeze = paused;
            }
        }
    }

    void flash() override
    {
        // FlashIcon (methods_output.cpp)
        Frame.FlashWindow(TRUE);
    }

    // ── pulls ──────────────────────────────────────────────────────────────
    std::string currentCommand() const override
    {
        // GetCommand (methods_commands.cpp)
        CString strCommand;

        for (POSITION pos = m_pDoc->GetFirstViewPosition(); pos != NULL; )
        {
            CView* pView = m_pDoc->GetNextView(pos);

            if (pView->IsKindOf(RUNTIME_CLASS(CSendView)))
            {
                CSendView* pmyView = (CSendView*)pView;

                // find what the command is
                pmyView->GetEditCtrl().GetWindowText(strCommand);
            }
        }

        return std::string((const char*)strCommand);
    }

    TextRange outputSelection() const override
    {
        // GetSelection{Start,End}{Line,Column} (methods_info.cpp), fused:
        // 1-based line/col; all zeros = no selection.  Start/EndColumn are
        // startcol + 1 / endcol + 1 after the per-line length clamp.
        TextRange r{0, 0, 0, 0};

        for (POSITION pos = m_pDoc->GetFirstViewPosition(); pos != NULL; )
        {
            CView* pView = m_pDoc->GetNextView(pos);

            if (pView->IsKindOf(RUNTIME_CLASS(CMUSHView)) &&
                m_pDoc->m_LineList.GetCount() > 0)
            {
                CMUSHView* pmyView = (CMUSHView*)pView;
                long startcol, endcol;

                CLine* pStartLine = m_pDoc->m_LineList.GetAt(
                    m_pDoc->GetLinePosition(pmyView->m_selstart_line));
                CLine* pEndLine   = m_pDoc->m_LineList.GetAt(
                    m_pDoc->GetLinePosition(pmyView->m_selend_line));

                startcol = MIN(pmyView->m_selstart_col, pStartLine->len);
                endcol   = MIN(pmyView->m_selend_col, pEndLine->len);

                // all zeros if no selection
                if (!(pmyView->m_selend_line > pmyView->m_selstart_line ||
                      (pmyView->m_selend_line == pmyView->m_selstart_line &&
                       endcol > startcol)))
                    return r;

                r.startLine = pmyView->m_selstart_line + 1;
                r.startCol  = startcol + 1;
                r.endLine   = pmyView->m_selend_line + 1;
                r.endCol    = endcol + 1;
                return r;
            }
        }

        return r;
    }

    int scrollPos() const override
    {
        // GetInfo 296 (methods_info.cpp)
        CMUSHView* pmyView = m_pDoc->GetFirstOutputWindow();
        if (pmyView)
        {
            CPoint pt = pmyView->GetScrollPosition();
            return pt.y;
        }
        return 0;
    }

    Size outputAreaChars() const override
    {
        // Dormant on every platform (the engine reads m_gui_cols/rows,
        // pushed by NAWS).  Faithful equivalent: the SendWindowSizes view
        // math — wrap column x visible rows.
        Size s{(int)m_pDoc->m_nWrapColumn, 0};
        CMUSHView* pmyView = m_pDoc->GetFirstOutputWindow();
        if (pmyView && m_pDoc->m_FontHeight)
        {
            RECT r;
            pmyView->GetTextRect(&r);
            s.h = (r.bottom - r.top - m_pDoc->m_iPixelOffset)
                  / m_pDoc->m_FontHeight;
        }
        return s;
    }

    // ── the command input bar ──────────────────────────────────────────────
    bool setCommand(const char* text) override
    {
        // SetCommand (methods_commands.cpp): fill ONLY if empty
        for (POSITION pos = m_pDoc->GetFirstViewPosition(); pos != NULL; )
        {
            CView* pView = m_pDoc->GetNextView(pos);

            if (pView->IsKindOf(RUNTIME_CLASS(CSendView)))
            {
                CSendView* pmyView = (CSendView*)pView;

                CString strCurrent;

                pmyView->GetEditCtrl().GetWindowText(strCurrent);
                if (strCurrent.IsEmpty())
                {
                    pmyView->GetEditCtrl().ReplaceSel(text, TRUE);
                    pmyView->NotifyPluginCommandChanged();
                    return true;
                }   // end of command being empty
                else
                    return false;
            }
        }

        return true;
    }

    void selectCommand() override
    {
        // SelectCommand (methods_commands.cpp)
        for (POSITION pos = m_pDoc->GetFirstViewPosition(); pos != NULL; )
        {
            CView* pView = m_pDoc->GetNextView(pos);

            if (pView->IsKindOf(RUNTIME_CLASS(CSendView)))
            {
                CSendView* pmyView = (CSendView*)pView;

                pmyView->GetEditCtrl().SetSel(0, -1);   // select all
            }
        }
    }

    Range commandSelection() const override
    {
        // GetInfo 236/237 (methods_info.cpp): CEdit::GetSel semantics —
        // start is nStartChar + 1; end is nEndChar, forced to 0 when the
        // selection is empty (nEndChar <= nStartChar).
        Range r{0, 0};

        for (POSITION pos = m_pDoc->GetFirstViewPosition(); pos != NULL; )
        {
            CView* pView = m_pDoc->GetNextView(pos);

            if (pView->IsKindOf(RUNTIME_CLASS(CSendView)))
            {
                CSendView* pmyView = (CSendView*)pView;
                int nStartChar;
                int nEndChar;

                // find where cursor is.  The seam contract is the raw
                // CEdit::GetSel offsets (pal/iworldview.h: "endpoints, in
                // characters" — the wx adapter returns them untouched).  The
                // GetInfo 236/237 engine sites own the 1-based / zero-if-empty
                // transform, so applying it here as well would run it twice.
                pmyView->GetEditCtrl().GetSel(nStartChar, nEndChar);
                r.start = nStartChar;
                r.end   = nEndChar;
                break;
            }
        }

        return r;
    }

    void setCommandSelection(long first, long last) override
    {
        // SetCommandSelection (methods_commands.cpp)
        for (POSITION pos = m_pDoc->GetFirstViewPosition(); pos != NULL; )
        {
            CView* pView = m_pDoc->GetNextView(pos);

            if (pView->IsKindOf(RUNTIME_CLASS(CSendView)))
            {
                CSendView* pmyView = (CSendView*)pView;
                pmyView->GetEditCtrl().SetSel(first - 1, last, FALSE);
            }
        }
    }

    std::string pushCommand() override
    {
        // PushCommand (methods_commands.cpp)
        CString strCommand;

        for (POSITION pos = m_pDoc->GetFirstViewPosition(); pos != NULL; )
        {
            CView* pView = m_pDoc->GetNextView(pos);

            if (pView->IsKindOf(RUNTIME_CLASS(CSendView)))
            {
                CSendView* pmyView = (CSendView*)pView;

                // find what the command is
                pmyView->GetEditCtrl().GetWindowText(strCommand);

                // save the echo flag
                bool bSaveNoEcho = m_pDoc->m_bNoEcho;
                m_pDoc->m_bNoEcho = false;  // make sure we save it

                // remember it
                pmyView->AddToCommandHistory(strCommand);

                // restore the command
                m_pDoc->m_bNoEcho = bSaveNoEcho;

                pmyView->GetEditCtrl().SetSel(0, -1);          // select all
                pmyView->GetEditCtrl().ReplaceSel("", TRUE);   // blank it out
                pmyView->NotifyPluginCommandChanged();
            }
        }

        return std::string((const char*)strCommand);
    }

    std::vector<std::string> commandList(long count) const override
    {
        // GetCommandList (methods_commands.cpp): newest first; count <= 0
        // means all
        std::vector<std::string> result;

        for (POSITION vpos = m_pDoc->GetFirstViewPosition(); vpos != NULL; )
        {
            CView* pView = m_pDoc->GetNextView(vpos);

            if (pView->IsKindOf(RUNTIME_CLASS(CSendView)))
            {
                CSendView* pmyView = (CSendView*)pView;

                if (!pmyView->m_msgList.IsEmpty())
                {
                    long n = count;
                    if (n <= 0)
                        n = pmyView->m_msgList.GetCount();     // zero means all
                    else
                        n = MIN(n, pmyView->m_msgList.GetCount());

                    long iCount = 0;
                    for (POSITION pos = pmyView->m_msgList.GetTailPosition();
                         pos && iCount < n; iCount++)
                    {
                        CString strLine = pmyView->m_msgList.GetPrev(pos);
                        result.push_back(std::string((const char*)strLine));
                    }
                }
            }
        }

        return result;
    }

    void addToCommandHistory(const char* text) override
    {
        // AddToCommandHistory (methods_sending.cpp): consecutive duplicates
        // suppressed; oldest evicted at m_nHistoryLines
        for (POSITION pos = m_pDoc->GetFirstViewPosition(); pos != NULL; )
        {
            CView* pView = m_pDoc->GetNextView(pos);

            if (pView->IsKindOf(RUNTIME_CLASS(CSendView)))
            {
                CSendView* pmyView = (CSendView*)pView;

                if (strlen(text) > 0 &&
                    strcmp(text, pmyView->m_last_command) != 0)
                {
                    if (pmyView->m_inputcount >= m_pDoc->m_nHistoryLines)
                    {
                        pmyView->m_msgList.RemoveHead();   // keep max of "m_nHistoryLines" previous commands
                        pmyView->m_HistoryFindInfo.m_nCurrentLine--;     // adjust for a "find again"
                        if (pmyView->m_HistoryFindInfo.m_nCurrentLine < 0)
                            pmyView->m_HistoryFindInfo.m_nCurrentLine = 0;
                    }   // end of buffer full
                    else
                        pmyView->m_inputcount++;
                    pmyView->m_msgList.AddTail(text);
                    pmyView->m_last_command = text;
                } // end of different from last one

                break;
            }	  // end of being a CSendView
        }   // end of loop through views
    }

    void deleteCommandHistory() override
    {
        // DeleteCommandHistory (methods_commands.cpp)
        for (POSITION pos = m_pDoc->GetFirstViewPosition(); pos != NULL; )
        {
            CView* pView = m_pDoc->GetNextView(pos);

            if (pView->IsKindOf(RUNTIME_CLASS(CSendView)))
            {
                CSendView* pmyView = (CSendView*)pView;

                // OK, do it ...
                pmyView->m_msgList.RemoveAll();
                pmyView->m_HistoryPosition = NULL;
                pmyView->m_inputcount = 0;
                pmyView->m_HistoryFindInfo.m_pFindPosition = NULL;
                pmyView->m_HistoryFindInfo.m_nCurrentLine = 0;
                pmyView->m_HistoryFindInfo.m_bAgain = FALSE;
                pmyView->m_strPartialCommand.Empty();
                pmyView->m_last_command.Empty();
            }
        }
    }

    void setInputFont(const char* face, int pointSize,
                      bool bold, bool italic) override
    {
        // SetInputFont (methods_commands.cpp) — the seam carries bold as a
        // flag; the original passed a full weight (bold begins at 600)
        m_pDoc->ChangeInputFont(pointSize, face,
                                bold ? FW_BOLD : FW_NORMAL,
                                DEFAULT_CHARSET, italic);
    }

    // ── output-pane management ─────────────────────────────────────────────
    void beginBatch() override
    {
        // no Windows counterpart: paint coalescing already batches the
        // per-line invalidations the display flip will introduce
    }

    void endBatch() override
    {
    }

    void deleteLines(long /*n*/) override
    {
        // DeleteLines (methods_output.cpp): the engine already trimmed the
        // list; resync the views' scrollbar state
        for (POSITION pos = m_pDoc->GetFirstViewPosition(); pos != NULL; )
        {
            CView* pView = m_pDoc->GetNextView(pos);

            if (pView->IsKindOf(RUNTIME_CLASS(CMUSHView)))
            {
                CMUSHView* pmyView = (CMUSHView*)pView;

                pmyView->addedstuff();
            }
        }
    }

    void setScroll(long line) override
    {
        // SetScroll (methods_output.cpp), scroll half only — the seam does
        // not carry the Visible flag, so scrollbar visibility is unchanged.
        // -1 = bottom; engine filters -2 before emitting.
        CPoint pt(0, 0);
        int lastline = m_pDoc->GetLastLine();

        for (POSITION pos = m_pDoc->GetFirstViewPosition(); pos != NULL; )
        {
            CView* pView = m_pDoc->GetNextView(pos);

            if (pView->IsKindOf(RUNTIME_CLASS(CMUSHView)))
            {
                CMUSHView* pmyView = (CMUSHView*)pView;

                CPoint cur_pt = pmyView->GetScrollPosition();
                bool will_scroll = (line != cur_pt.y);
                int highest = (lastline * m_pDoc->m_FontHeight)
                              - pmyView->GetOutputWindowHeight();
                will_scroll = will_scroll &&
                              ((line != -1) || (cur_pt.y != highest));

                if (will_scroll)
                {
                    // -1 goes to the end
                    if (line == -1)
                        pt.y = highest;
                    else
                        pt.y = line;

                    if (pt.y < 0)
                        pt.y = 0;
                    if (pt.y > highest)
                        pt.y = highest;

                    pmyView->ScrollToPosition(pt, false);
                    pmyView->Invalidate();
                }
            }
        }
    }

    void setOutputSelection(int startLine, int endLine,
                            int startCol, int endCol) override
    {
        // SetSelection (methods_utilities.cpp)
        CMUSHView* pmyView = m_pDoc->GetFirstOutputWindow();

        // return if unable
        if (!pmyView)
            return;

        pmyView->m_selstart_line = startLine - 1;
        pmyView->m_selend_line   = endLine - 1;
        pmyView->m_selstart_col  = startCol - 1;
        pmyView->m_selend_col    = endCol - 1;

        pmyView->m_pin_line      = pmyView->m_selstart_line;
        pmyView->m_pin_col       = pmyView->m_selstart_col;

        pmyView->SelectionChanged();

        CRgn newrgn;

        if (!pmyView->get_selection(newrgn) && m_pDoc->m_bAutoFreeze)
            pmyView->m_freeze = true;   // freeze output so they can copy or print it

        newrgn.DeleteObject();

        // Invalidate new stuff
        m_pDoc->UpdateAllViews(NULL);
    }

    void setOutputFont(const char* face, int pointSize,
                       bool /*bold*/, bool /*italic*/) override
    {
        // SetOutputFont (methods_output.cpp) — the original always passed
        // FW_NORMAL; bold/italic display styling comes from the m_bShow*
        // options, preserved here exactly
        m_pDoc->ChangeFont(pointSize, face, FW_NORMAL,
                           m_pDoc->m_font_charset,
                           m_pDoc->m_bShowBold, m_pDoc->m_bShowItalic,
                           m_pDoc->m_bShowUnderline, m_pDoc->m_iLineSpacing);
    }

    void textRectChanged() override
    {
        // Windows performed no view-side action when the reserved text
        // rectangle changed (the engine site already calls this helper
        // unconditionally today; it was a no-op with a null view)
    }

    Rect outputRect() const override
    {
        // GetWorldWindowPositionX, Which == 1 (methods_output.cpp)
        Rect out{0, 0, 0, 0};
        CWindowPlacement wp;

        for (POSITION pos = m_pDoc->GetFirstViewPosition(); pos != NULL; )
        {
            CView* pView = m_pDoc->GetNextView(pos);

            if (pView->IsKindOf(RUNTIME_CLASS(CSendView)))
            {
                CSendView* pmyView = (CSendView*)pView;

                pmyView->GetParentFrame()->GetWindowPlacement(&wp);
                out.x = wp.rcNormalPosition.left;
                out.y = wp.rcNormalPosition.top;
                out.w = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
                out.h = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
                break;
            }
        }

        return out;
    }

    // ── world/main window management ───────────────────────────────────────
    void setMainTitle(const char* /*title*/) override
    {
        // SetMainTitle (methods_output.cpp — reads m_strMainWindowTitle,
        // already stored by the engine site)
        Frame.FixUpTitleBar();
    }

    void moveWorldWindow(long left, long top,
                         long width, long height) override
    {
        // MoveWorldWindowX, Which == 1 (methods_output.cpp)
        for (POSITION pos = m_pDoc->GetFirstViewPosition(); pos != NULL; )
        {
            CView* pView = m_pDoc->GetNextView(pos);

            if (pView->IsKindOf(RUNTIME_CLASS(CSendView)))
            {
                CSendView* pmyView = (CSendView*)pView;

                pmyView->GetParentFrame()->MoveWindow(left, top, width, height);

                break;
            }
        }
    }

    void setWindowStatus(int status) override
    {
        // SetWorldWindowStatus (methods_output.cpp)
        CFrameWnd* pParent = NULL;

        for (POSITION pos = m_pDoc->GetFirstViewPosition(); pos != NULL; )
        {
            CView* pView = m_pDoc->GetNextView(pos);

            if (pView->IsKindOf(RUNTIME_CLASS(CSendView)))
            {
                CSendView* pmyView = (CSendView*)pView;

                pParent = pmyView->GetParentFrame();

                break;
            }
        }

        if (pParent)
        {
            switch (status)
            {
                case 1: pParent->ShowWindow(SW_SHOWMAXIMIZED); break;
                case 2: pParent->ShowWindow(SW_MINIMIZE); break;
                case 3: pParent->ShowWindow(SW_RESTORE); break;
                case 4: pParent->ShowWindow(SW_SHOWNORMAL); break;
            } // end of switch
        } // have parent
    }

    void activateClient() override
    {
        // ActivateClient (methods_output.cpp)
        if (Frame.IsIconic())
            Frame.ShowWindow(SW_RESTORE);

        Frame.SetForegroundWindow();
        Frame.SetFocus();
    }

    bool isFullscreen() const override
    {
        // GetInfo 125 (methods_info.cpp)
        return Frame.IsFullScreen() != 0;
    }

    bool saveWorld() override
    {
        // Save with no name (methods_utilities.cpp): !DoSave == success.
        // GetPathName() is CDocument's public accessor for m_strPathName,
        // which the original reached directly as a member of the doc.
        return !m_pDoc->DoSave(m_pDoc->GetPathName(), TRUE);
    }

    Rect mainRect() const override
    {
        // GetMainWindowPosition (methods_info.cpp)
        Rect out{0, 0, 0, 0};
        CWindowPlacement wp;

        Frame.GetWindowPlacement(&wp);
        out.x = wp.rcNormalPosition.left;
        out.y = wp.rcNormalPosition.top;
        out.w = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
        out.h = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;

        return out;
    }

    // ── the info bar ───────────────────────────────────────────────────────
    void infobarShow(bool visible) override
    {
        // ShowInfoBar (methods_infobar.cpp)
        if (Frame.m_wndInfoBar.m_hWnd)
            Frame.ShowControlBar(&Frame.m_wndInfoBar, visible, FALSE);
    }

    void infobarClear() override
    {
        // InfoClear (methods_infobar.cpp)
        if (!Frame.m_wndInfoBar.m_hWnd)
            return;

        CRichEditCtrl* pRichEdit =
            (CRichEditCtrl*)Frame.m_wndInfoBar.GetDlgItem(IDC_INFOTEXT);

        // back to default colour, font etc.
        pRichEdit->SetDefaultCharFormat(Frame.m_defaultInfoBarFormat);

        // select all
        pRichEdit->SetSel(0, -1);

        // delete everything
        pRichEdit->ReplaceSel("");
    }

    void infobarText(const char* text) override
    {
        // Info (methods_infobar.cpp)
        if (!Frame.m_wndInfoBar.m_hWnd)
            return;

        CRichEditCtrl* pRichEdit =
            (CRichEditCtrl*)Frame.m_wndInfoBar.GetDlgItem(IDC_INFOTEXT);

        pRichEdit->ReplaceSel(text);
    }

    void infobarColour(Colour c) override
    {
        // InfoColour (methods_infobar.cpp) — the seam carries the resolved
        // COLORREF (the engine resolves the colour name before emitting)
        if (!Frame.m_wndInfoBar.m_hWnd)
            return;

        CHARFORMAT cf;

        CRichEditCtrl* pRichEdit =
            (CRichEditCtrl*)Frame.m_wndInfoBar.GetDlgItem(IDC_INFOTEXT);

        memset(&cf, 0, sizeof(cf));
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_COLOR;
        cf.crTextColor = c;
        pRichEdit->SetSelectionCharFormat(cf);
    }

    void infobarBackground(Colour c) override
    {
        // InfoBackground (methods_infobar.cpp) — resolved COLORREF as above
        if (!Frame.m_wndInfoBar.m_hWnd)
            return;

        CRichEditCtrl* pRichEdit =
            (CRichEditCtrl*)Frame.m_wndInfoBar.GetDlgItem(IDC_INFOTEXT);

        pRichEdit->SetBackgroundColor(FALSE, c);
    }

    void infobarFont(const char* face, int pointSize, int style) override
    {
        // InfoFont (methods_infobar.cpp)
        if (!Frame.m_wndInfoBar.m_hWnd)
            return;

        CHARFORMAT cf;

        CRichEditCtrl* pRichEdit =
            (CRichEditCtrl*)Frame.m_wndInfoBar.GetDlgItem(IDC_INFOTEXT);

        memset(&cf, 0, sizeof(cf));
        cf.cbSize = sizeof(cf);

        // font name provided?
        if (strlen(face) > 0)
        {
            cf.dwMask = CFM_FACE;
            strncpy(cf.szFaceName, face, sizeof(cf.szFaceName) - 1);

            // do this so fonts like webdings, marlett and so on work properly
            // without it, such fonts would take (say) "g" and substitute a "g" from
            // the default font, not show the "g" equivalent in the selected font
            cf.dwMask |= CFM_CHARSET;
            cf.bCharSet = SYMBOL_CHARSET;
        }

        // size provided?
        if (pointSize > 0)
        {
            cf.dwMask |= CFM_SIZE;
            cf.yHeight = pointSize * 20; // points
        }

        // style
        cf.dwMask |= CFM_BOLD | CFM_ITALIC | CFM_STRIKEOUT | CFM_UNDERLINE;

        if (style & 1)   // bold
            cf.dwEffects |= CFE_BOLD;

        if (style & 2)   // italic
            cf.dwEffects |= CFE_ITALIC;

        if (style & 4)   // underline
            cf.dwEffects |= CFE_UNDERLINE;

        if (style & 8)   // strikeout
            cf.dwEffects |= CFE_STRIKEOUT;

        pRichEdit->SetSelectionCharFormat(cf);
    }

    Size infobarSize() const override
    {
        // GetInfo 257/258 pixel size; (0,0) when hidden — the Windows cases
        // measured the control bar's window rect
        Size s{0, 0};
        if (Frame.m_wndInfoBar.m_hWnd && Frame.m_wndInfoBar.IsWindowVisible())
        {
            CRect r;
            Frame.m_wndInfoBar.GetWindowRect(&r);
            s.w = r.Width();
            s.h = r.Height();
        }
        return s;
    }

private:
    CMUSHclientDoc* m_pDoc;
};


// ── factory ──────────────────────────────────────────────────────────────────

IWorldView* CreateWinWorldView(CMUSHclientDoc* pDoc)
{
    return new CWinWorldView(pDoc);
}

} // namespace pal

#endif // _WIN32

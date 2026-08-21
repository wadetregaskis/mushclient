#ifndef MUSHCLIENT_TESTS_NOTEPAD_FIXTURE_H
#define MUSHCLIENT_TESTS_NOTEPAD_FIXTURE_H

// tests/notepad_fixture.h — a real CTextDocument, backed by a std::string.
//
// The notepad family was adopted into mushcore (docs/STUB-ADOPTION-2026-08.md):
// scripting/methods/methods_notepad.cpp and TextDocument.cpp are compiled
// headless now, so the tests drive the shipping bodies instead of a twin.
//
// Those bodies reach the GUI through exactly one seam — CTextDocument's twelve
// std::function m_fn_* members (TextDocument.h:66-98), which CWxNotepadFrame
// fills in the product (platform/wx_notepad.cpp).  Nothing here needs a window,
// a surface or a DC: filling the same twelve with lambdas over a std::string
// gives the real methods a real notepad to act on.
//
// This mirrors what the product does, in the same order, so that a divergence
// between the two shows up as a test failure rather than as a fixture that
// quietly models something else:
//   1. CTextDocument::CreateInstance()      (the ctor is protected)
//   2. set the identity fields FindNotepad matches on — m_pRelatedWorld,
//      m_iUniqueDocumentNumber, m_strTitle
//   3. wire the callbacks
//   4. App.m_pNormalDocTemplate->AddDocument()
//
// Step 2's identity fields are the load-bearing ones and the reason this is a
// fixture rather than three lines inline: FindNotepad matches on world AND
// docnum (doc.cpp:2988-2992), so a fixture that skips either one is findable
// when the real thing would not be, and every "not found" arm silently stops
// being tested.

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "TextDocument.h"

#include "platform/wx_mfc_hooks.h"   // g_wxhook_create_text_window (NotepadCapture)

#include <algorithm>
#include <string>
#include <vector>

struct FakeNotepad {
    CTextDocument* doc = nullptr;

    // The notepad's contents and selection, as the wx text control would hold
    // them.  Public so a test can seed or inspect them directly.
    std::string text;
    int selStart = 0, selEnd = 0;

    // Observation counters — several of the adopted methods are only observable
    // through the fact that they asked the window to do something.
    int  applyFontCalls    = 0;
    int  applyColourCalls  = 0;
    int  activateCalls     = 0;
    int  closeCalls        = 0;
    int  moveCalls         = 0;
    int  saveCalls         = 0;
    bool readOnly          = false;
    int  lastMove[4]       = {0, 0, 0, 0};
    std::string lastSavePath;
    bool saveSucceeds      = true;

    // The rect m_fn_get_rect reports, as "left top right bottom".  Deliberately
    // NOT square and NOT origin-anchored: GetNotepadWindowPosition reformats
    // l/t/r/b into l/t/WIDTH/HEIGHT, and a fixture where right == width would
    // make that conversion an identity and hide a mistake in it.
    int rect[4] = {100, 50, 400, 250};   // → left 100 top 50 width 300 height 200

    FakeNotepad(CMUSHclientDoc* world, const char* title, __int64 docnum)
    {
        doc = CTextDocument::CreateInstance();

        // Identity — what FindNotepad matches on.
        doc->m_pRelatedWorld         = world;
        doc->m_iUniqueDocumentNumber = docnum;
        doc->m_strTitle              = title;
        doc->SetTitle(title);

        doc->m_fn_get_text        = [this]() { return text; };
        doc->m_fn_get_text_length = [this]() { return (int) text.size(); };
        doc->m_fn_set_sel         = [this](int a, int b) {
            selStart = a;
            selEnd   = (b < 0) ? (int) text.size() : b;
        };
        doc->m_fn_replace_sel     = [this](const std::string& s) {
            const int a = (std::max)(0, (std::min)(selStart, (int) text.size()));
            const int b = (std::max)(a, (std::min)(selEnd, (int) text.size()));
            text.replace((size_t) a, (size_t) (b - a), s);
            selStart = selEnd = a + (int) s.size();
        };
        doc->m_fn_activate        = [this]() { ++activateCalls; };
        doc->m_fn_move_window     = [this](int l, int t, int w, int h) {
            ++moveCalls;
            lastMove[0] = l; lastMove[1] = t; lastMove[2] = w; lastMove[3] = h;
        };
        doc->m_fn_get_rect        = [this]() {
            char buf[64];
            snprintf(buf, sizeof buf, "%d %d %d %d",
                     rect[0], rect[1], rect[2], rect[3]);
            return std::string(buf);
        };
        doc->m_fn_save_to_file    = [this](const std::string& path, bool) {
            ++saveCalls;
            lastSavePath = path;
            return saveSucceeds;
        };
        doc->m_fn_apply_font      = [this]() { ++applyFontCalls; };
        doc->m_fn_apply_colours   = [this]() { ++applyColourCalls; };
        doc->m_fn_set_read_only   = [this](bool ro) { readOnly = ro; };
        doc->m_fn_close_window    = [this]() { ++closeCalls; };

        if (App.m_pNormalDocTemplate)
            App.m_pNormalDocTemplate->AddDocument(doc);
    }

    ~FakeNotepad()
    {
        if (App.m_pNormalDocTemplate && doc)
            App.m_pNormalDocTemplate->RemoveDocument(doc);
        delete doc;
    }

    FakeNotepad(const FakeNotepad&)            = delete;
    FakeNotepad& operator=(const FakeNotepad&) = delete;
};

// Drop every notepad the app currently knows about.  Tests that assert on
// GetNotepadList need a known starting point, and the template is a process-wide
// global that survives between TESTs in the same binary.
inline void ClearAllNotepads()
{
    if (App.m_pNormalDocTemplate)
        App.m_pNormalDocTemplate->m_docs.clear();
}

// ── NotepadCapture ──────────────────────────────────────────────────────────
//
// For the tests that do not care about the notepad API at all, and simply want
// to READ what the engine sent to a notepad — MXP debug output, world-load
// comments, world.Send destinations.
//
// Those tests used to install g_wxhook_append_to_notepad, which was a hook on
// the deleted headless twin.  That worked but modelled the wrong thing: on the
// product, output aimed at a notepad that does not exist yet goes through
// CreateTextWindow, which CREATES one — and only subsequent writes find it and
// append.  Capturing at the append hook skipped the creation step entirely, so
// a test could not tell "created two notepads" from "appended to one".
//
// So this captures at g_wxhook_create_text_window, the seam the product really
// uses, and makes each creation produce a live FakeNotepad registered with the
// document template.  The engine's next append then finds it exactly as it
// would in the app, and `text()` is the concatenation of everything written to
// every notepad — which is what a debug-capture harness wants.
struct NotepadCapture {
    std::vector<FakeNotepad*> pads;

    NotepadCapture()
    {
        ClearAllNotepads();
        Active() = this;
        m_saved = g_wxhook_create_text_window;
        g_wxhook_create_text_window = &NotepadCapture::Create;
    }

    ~NotepadCapture()
    {
        g_wxhook_create_text_window = m_saved;
        Active() = nullptr;
        for (FakeNotepad* p : pads) delete p;
    }

    // Everything written to every notepad, in creation order.
    std::string text() const
    {
        std::string out;
        for (const FakeNotepad* p : pads) out += p->text;
        return out;
    }

    bool contains(const char* s) const { return text().find(s) != std::string::npos; }
    size_t count() const { return pads.size(); }

    NotepadCapture(const NotepadCapture&)            = delete;
    NotepadCapture& operator=(const NotepadCapture&) = delete;

private:
    void* (*m_saved)(const char*, const char*, CMUSHclientDoc*, long long,
                     const char*, long, long, unsigned int, unsigned long,
                     unsigned long, const char*, const char*,
                     bool, bool, bool, bool, bool, int, int) = nullptr;

    // A function-local static rather than a class static, so the header stays
    // header-only with no out-of-line definition to forget.
    static NotepadCapture*& Active()
    {
        static NotepadCapture* p = nullptr;
        return p;
    }

    static void* Create(const char* sText, const char* sTitle,
                        CMUSHclientDoc* pDoc, long long iUniqueDocNum,
                        const char*, long, long, unsigned int,
                        unsigned long, unsigned long, const char*, const char*,
                        bool, bool, bool, bool, bool, int, int)
    {
        NotepadCapture* self = Active();
        if (!self) return nullptr;
        FakeNotepad* p = new FakeNotepad(pDoc, sTitle ? sTitle : "", iUniqueDocNum);
        p->text = sText ? sText : "";
        self->pads.push_back(p);
        return static_cast<void*>(p->doc);
    }
};

#endif // MUSHCLIENT_TESTS_NOTEPAD_FIXTURE_H

// tests/win/test_prefs_pages.cpp — the World- and Global-Preferences pages,
// on Windows.
//
// Why this file is under tests/win/ rather than tests/
// ---------------------------------------------------
// It drives dialogs/world_prefs/prefspropertypages.cpp and
// dialogs/world_prefs/configuration.cpp.  On macOS those are `mushgui_adapters`
// TUs, which the headless test executables deliberately do not link, so the
// port reaches them only from inside the running app (`--gui-bridge-selftest`).
// On Windows they are ordinary members of the 286 engine sources this target
// already compiles, so the same assertions are an ordinary unit test here.
//
// That is the point of the file: `44f0737` fixed eight defects on these pages
// and docs/WINDOWS-VALIDATION.md §9c marked it **RUN** — "the largest dialog
// change in the list", to be checked by walking every page by hand.  Most of it
// does not need a walk.  Four of the five families the port covers in
// gui_bridge_selftest.cpp are pure member arithmetic and are reproduced below;
// what is left for a human is in the note at the end.
//
// A page constructed here has NO WINDOW.  Everything called below is either a
// static, or a member that touches only fields — never a control.  Anything
// that would need `UpdateData` or a live property sheet is called out where it
// is skipped rather than being faked.

#include "mc_test.h"

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "OtherTypes.h"                            // CTrigger, CTimer
#include "dialogs/world_prefs/prefspropertypages.h"
#include "dialogs/world_prefs/triggdlg.h"

// ── 44f0737 #1: un-ticking "multi-line" was thrown away ───────────────────
// CPrefsP8::CheckIfChanged decides whether editing a trigger dirties the world.
// It compared every field of CTrigger EXCEPT bMultiLine, so un-ticking the box
// and changing nothing else was silently discarded — the dialog closed, the
// world was never marked modified, and the edit died with the session.
TEST(PrefsP8, TriggerMultiLineIsAChange)
{
    CMUSHclientDoc doc;
    CPrefsP14 page14;                   // LoadDialog reads its three show-* flags
    CPrefsP8  page8;
    page8.m_doc      = &doc;
    page8.m_prefsp3  = NULL;
    page8.m_prefsp14 = &page14;

    CTrigger trig;
    trig.trigger       = "^You see (.*)$";
    trig.contents      = "look $1";
    trig.bMultiLine    = true;
    trig.iLinesToMatch = 3;

    CTriggerDlg dlg;
    page8.LoadDialog(&dlg, &trig);

    // The control.  Without it a gutted `return true` would pass everything
    // below.
    EXPECT_FALSE(page8.CheckIfChanged(&dlg, &trig));

    dlg.m_bMultiLine = FALSE;
    EXPECT_TRUE(page8.CheckIfChanged(&dlg, &trig));   // was: silently discarded

    // The sibling field was already compared; assert it still is, so a fix that
    // replaced a term rather than adding one would show up here.
    dlg.m_bMultiLine    = TRUE;
    dlg.m_iLinesToMatch = 4;
    EXPECT_TRUE(page8.CheckIfChanged(&dlg, &trig));
}

// ── 44f0737 #2: OK invented a change every time ───────────────────────────
// ChangedPrefsP9 / ChangedPrefsP17 decide whether pressing OK on World
// Properties marks the world modified.  Both undid LoadPrefs*'s SAMECOLOUR
// encoding by subtracting 1, giving -1 — which the unsigned short 65535 can
// never equal.  So a world using the shipped default "same as normal text"
// colour was reported changed on every OK, and the user was asked to save a
// world they had not edited.
TEST(PrefsChange, DefaultColoursAreNotASpuriousChange)
{
    CMUSHclientDoc doc;

    CPrefsP9 page9;
    doc.m_echo_colour = SAMECOLOUR;              // the default
    doc.LoadPrefsP9(page9);
    EXPECT_FALSE(doc.ChangedPrefsP9(page9));

    page9.m_echo_colour = 1;                     // the user picked custom colour 1
    EXPECT_TRUE(doc.ChangedPrefsP9(page9));      // ...so this is not a blanket "no"

    doc.m_echo_colour = 4;                       // a world already using a custom one
    doc.LoadPrefsP9(page9);
    EXPECT_FALSE(doc.ChangedPrefsP9(page9));

    CPrefsP17 page17;
    doc.m_iNoteTextColour = SAMECOLOUR;
    doc.LoadPrefsP17(page17);
    EXPECT_FALSE(doc.ChangedPrefsP17(page17));

    page17.m_iNoteTextColour = 5;
    EXPECT_TRUE(doc.ChangedPrefsP17(page17));

    doc.m_iNoteTextColour = 4;
    doc.LoadPrefsP17(page17);
    EXPECT_FALSE(doc.ChangedPrefsP17(page17));
}

// ── 44f0737 #3: the Timers list's Contents header did not sort ────────────
// CPrefsP16::CompareObjects had no eColumnContents case and fell through to
// `default: iResult = 0`, so clicking that header did nothing at all.
TEST(PrefsP16, TimerListSortsByContents)
{
    CTimer a, b;
    // Everything the other cases key on is deliberately EQUAL, so only the
    // Contents comparison can discriminate.
    a.strContents = "aaa";
    b.strContents = "zzz";

    EXPECT_TRUE(CPrefsP16::CompareObjects(CPrefsP16::eColumnContents, &a, &b) < 0);
    EXPECT_TRUE(CPrefsP16::CompareObjects(CPrefsP16::eColumnContents, &b, &a) > 0);
    EXPECT_EQ(0, CPrefsP16::CompareObjects(CPrefsP16::eColumnContents, &a, &a));

    // Case-insensitively, like every other string column on these pages.
    b.strContents = "AAA";
    EXPECT_EQ(0, CPrefsP16::CompareObjects(CPrefsP16::eColumnContents, &a, &b));

    // The control: a column that has always sorted still does.
    a.strLabel = "aaa";
    b.strLabel = "zzz";
    EXPECT_TRUE(CPrefsP16::CompareObjects(CPrefsP16::eColumnLabel, &a, &b) < 0);
}

// ── 44f0737 #4: "Adjust width to size", the WINDOWS arm ───────────────────
// The port's half of this is covered in tests/test_mfc_ui.cpp, which does not
// compile here.  The Windows arm asks GetFirstOutputWindow() for a CMUSHView
// and answers 0 when there is none, and 0 means "caller keeps its figure" --
// so what is assertable in a host with no view is that the guards hold and
// nothing is invented.  That is worth pinning: the defect this replaced was a
// body that silently did nothing, and "returns 0" must stay distinguishable
// from "returned a number computed from a window that was not there".
TEST(AdjustWidth, WithNoOutputWindowNothingIsInvented)
{
    CMUSHclientDoc doc;

    // No view: the measured width is 0, so the wrap column is "cannot tell".
    EXPECT_EQ(0, doc.OutputWindowWidthPixels());
    EXPECT_EQ(0, doc.WrapColumnForCharWidth(8));

    // And the argument guard fires before anything else, on both platforms.
    EXPECT_EQ(0, doc.WrapColumnForCharWidth(0));
    EXPECT_EQ(0, doc.WrapColumnForCharWidth(-1));
}

// ── 44f0737 #5: the Commands page reverted the colour you just picked ─────
// CPrefsP9::OnSetActive re-seeds both command-window swatches from
// m_input_text_colour / m_input_background_colour, which only OnOK ever wrote.
// So a colour picked on the Commands page was discarded by visiting any other
// page and coming back.  CPrefsP3 and CPrefsP5 already saved theirs in
// OnKillActive; P9 now does the same.
//
// Deliberately last in the file.  OnKillActive does the two assignments and
// then chains to CPropertyPage::OnKillActive, which is real MFC here and calls
// UpdateData on a page with no window.  The assignments -- the whole subject of
// the fix -- have already happened by then, and the return value is not what is
// being asserted; if that chained call ever proves to be more than an assertion
// on stderr, this is the test that will say so first.
TEST(PrefsP9, CommandColoursSurviveAPageChange)
{
    CPrefsP9 page;
    page.m_input_text_colour       = RGB(1, 2, 3);
    page.m_input_background_colour = RGB(4, 5, 6);
    page.m_ctlTextSwatch.m_colour       = RGB(9, 9, 9);   // what the user just picked
    page.m_ctlBackgroundSwatch.m_colour = RGB(8, 8, 8);

    page.OnKillActive();                                  // leaving for another page

    EXPECT_EQ(RGB(9, 9, 9), page.m_input_text_colour);
    EXPECT_EQ(RGB(8, 8, 8), page.m_input_background_colour);
}

// ── What is NOT here, and why ─────────────────────────────────────────────
//
//  * CGlobalPrefsP4::OnFont (the printer-font picker seeding from the page
//    rather than from App).  The port asserts it by substituting
//    g_wxhook_font_dialog; on Windows OnFont opens a real modal CFontDialog,
//    which a test host must never do -- §10k's first lesson was a run wedged
//    behind an invisible modal box.  Checking this one needs the GUI walk.
//
//  * The fitted last-column width on the alias/timer/variable pages, and
//    LoadPrefsP7's wrong-enum indexing.  Both need a created CListCtrl.
//
//  * The physical-RAM warning's DWORD wrap on the output-buffer size.  That
//    ceiling is the PORT's (100,000,000 lines against Windows' 500,000), so
//    the arithmetic it broke does not exist in this configuration.


// The fitted column leaves room for EVERY other column.  The alias page used to
// subtract only Alias (not Sequence, Label or Group) and the timer page only
// Type and When, so once the fitted width was actually applied, both lists'
// last column ran past the list's edge and brought up a horizontal scroll bar.
TEST(PrefsListColumns, TheFittedColumnLeavesRoomForEveryOtherColumn)
{
    // alias page: Alias 100, Sequence 40, Contents (fitted), Label 0, Group 0
    const int alias[5] = { 100, 40, 150, 0, 0 };
    EXPECT_EQ(140, OtherColumnsWidth(alias, 5, 2));
    EXPECT_EQ(600 - 15 - 140, FittedLastColumnWidth(150, 150, 600, 15,
                                                    OtherColumnsWidth(alias, 5, 2)));
    // a column the user has hidden by dragging it to nothing (or a -1 from a
    // damaged saved configuration) takes no room
    const int odd[3] = { 80, -1, 200 };
    EXPECT_EQ(80, OtherColumnsWidth(odd, 3, 2));
}
MC_TEST_MAIN()

// ── Opening World Properties must not rewrite a preference you did not touch ─
//
// `LoadPrefsP23` used to fill the Chat page's "Accept incoming calls on port:"
// tick from `m_pChatListenSocket != NULL` — whether the world was *really*
// listening — while `SavePrefsP23` copies that same tick straight back into
// `m_bAcceptIncomingChatConnections`, the persisted preference. So OK on an
// untouched dialog overwrote the setting with runtime state, in both
// directions:
//
//   * a listen socket that failed to bind (the port already in use) cleared
//     the saved preference, so the next launch did not even try; and
//   * a script's `world.ChatAcceptCalls` set it on a world that had never
//     asked for it.
//
// `docs/UPSTREAM-BUGS.md` 108. The bind failure is still reported when it
// happens — `ChatAcceptCalls` sends a `ChatNote` — so showing the preference
// costs no feedback, and OK on a world whose bind failed now retries it.
TEST(PrefsChange, ChatAcceptTickShowsThePreferenceNotTheSocket)
{
    CMUSHclientDoc doc;
    CPrefsP23 page23;

    // The bind that OK retries really is made, on every address, and Windows
    // Firewall asks the player about each new place an exe doing that is built.
    // Holding the port, on loopback only, makes it the failed bind described above.
    SOCKET holder = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_TRUE(holder != INVALID_SOCKET);
    int iExclusive = 1;
    ::setsockopt(holder, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*) &iExclusive, sizeof iExclusive);
    sockaddr_in sa{};
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(45123);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(0, ::bind(holder, (sockaddr*) &sa, sizeof sa));

    // Seed the four fields LoadPrefsP23 substitutes defaults for, so that
    // ChangedPrefsP23 below isolates the accept tick.  Their defaulting is
    // deliberate and its "changed" verdict is correct — see UPSTREAM-BUGS 95,
    // whose whole point was that the page may differ from the document.
    doc.m_strOurChatName        = "tester";
    doc.m_IncomingChatPort      = 45123;          // not 4050: don't squat a real one
    doc.m_cChatForegroundColour = RGB(255, 0, 0);
    doc.m_cChatBackgroundColour = RGB(0, 0, 0);

    // The preference is on and no socket is listening: a failed bind.
    doc.m_bAcceptIncomingChatConnections = TRUE;
    ASSERT_TRUE(doc.m_pChatListenSocket == NULL);

    doc.LoadPrefsP23(page23);
    EXPECT_TRUE(page23.m_bAcceptIncomingChatConnections);   // shown as set...
    EXPECT_FALSE(doc.ChangedPrefsP23(page23));              // ...so OK is a no-op

    doc.SavePrefsP23(page23);
    EXPECT_TRUE(doc.m_bAcceptIncomingChatConnections);      // and survives OK
    EXPECT_TRUE(doc.m_pChatListenSocket == NULL);           // the retry failed

    // Non-vacuous: the tick still tracks the preference when it is off, so the
    // pass above is not "always true".
    doc.m_bAcceptIncomingChatConnections = FALSE;
    doc.LoadPrefsP23(page23);
    EXPECT_FALSE(page23.m_bAcceptIncomingChatConnections);

    // And a real edit still takes: ticking it is a change, and OK writes it.
    page23.m_bAcceptIncomingChatConnections = TRUE;
    EXPECT_TRUE(doc.ChangedPrefsP23(page23));
    doc.SavePrefsP23(page23);
    EXPECT_TRUE(doc.m_bAcceptIncomingChatConnections);
    EXPECT_TRUE(doc.m_pChatListenSocket == NULL);
    ::closesocket(holder);
}

// ── A world with no chat name adopts the default without announcing it ──────
//
// UPSTREAM-BUGS 95 moved LoadPrefsP23's defaulting off the document and onto
// the page, so SavePrefsP23 saw "Name-not-set" against an empty name and
// called ChatNameChange: every new world printed "Your chat name changed from
// <no name> to Name-not-set" in red.  Loading used to adopt it silently, and
// so does saving now; a name the user actually changes is still announced
// (ChatNameChange is what marks the document modified, so that is the probe).
TEST(PrefsChange, ADefaultChatNameIsAdoptedQuietly)
{
    CMUSHclientDoc doc;
    CPrefsP23 page23;
    doc.m_IncomingChatPort = 45123;     // not 4050: don't squat a real one

    ASSERT_TRUE(doc.m_strOurChatName.IsEmpty());
    doc.m_name.Empty();
    doc.LoadPrefsP23(page23);
    doc.SetModifiedFlag(FALSE);
    doc.SavePrefsP23(page23);
    EXPECT_TRUE(doc.m_strOurChatName == DEFAULT_CHAT_NAME);
    EXPECT_FALSE(doc.IsModified());

    // the character's name is the default when there is one
    doc.m_strOurChatName.Empty();
    doc.m_name = "Rincewind";
    doc.LoadPrefsP23(page23);
    doc.SavePrefsP23(page23);
    EXPECT_TRUE(doc.m_strOurChatName == "Rincewind");
    EXPECT_FALSE(doc.IsModified());

    // and a real change is still announced
    page23.m_strOurChatName = "Twoflower";
    doc.SavePrefsP23(page23);
    EXPECT_TRUE(doc.m_strOurChatName == "Twoflower");
    EXPECT_TRUE(doc.IsModified());
}

// GetItemCount and the selected counts run on every idle while a page is open, so they
// no longer walk the tree (upstream 8c996752).  They rely on what the page keeps true:
// one root per entry in m_GroupsMap, and an emptied group going from both.
TEST(PrefsTreeCounts, CountsAreRightWithoutWalkingTheTree)
{
    HWND hParent = ::CreateWindowExA (0, "STATIC", "", WS_POPUP, 0, 0, 200, 200, NULL, NULL, NULL, NULL);
    CWnd parent;
    parent.Attach (hParent);
    {
        CPrefsP16 page;
        page.m_bWantTreeControl = true;
        CRect r (0, 0, 190, 190);
        ASSERT_TRUE (page.m_cTreeCtrl.Create (WS_CHILD | TVS_HASLINES | TVS_SHOWSELALWAYS, r, &parent, 1) != FALSE);

        EXPECT_EQ (0, page.GetItemCount ());
        EXPECT_EQ (0, page.GetSelectedItemCount ());
        EXPECT_EQ (0, page.GetSelectedGroupCount ());

        HTREEITEM g1 = page.m_cTreeCtrl.InsertItem ("first", TVI_ROOT);
        page.m_GroupsMap ["first"] = g1;
        HTREEITEM g2 = page.m_cTreeCtrl.InsertItem ("second", TVI_ROOT);
        page.m_GroupsMap ["second"] = g2;
        HTREEITEM a = page.m_cTreeCtrl.InsertItem ("a", g1);
        page.m_cTreeCtrl.InsertItem ("b", g1);
        HTREEITEM c = page.m_cTreeCtrl.InsertItem ("c", g2);
        EXPECT_EQ (3, page.GetItemCount ());

        // an item selected: one item, no group
        page.m_cTreeCtrl.SelectItem (a);
        EXPECT_EQ (1, page.GetSelectedItemCount ());
        EXPECT_EQ (0, page.GetSelectedGroupCount ());

        // a group selected: no item, and the size of the group
        page.m_cTreeCtrl.SelectItem (g1);
        EXPECT_EQ (0, page.GetSelectedItemCount ());
        EXPECT_EQ (2, page.GetSelectedGroupCount ());

        // nothing selected
        page.m_cTreeCtrl.SelectItem (NULL);
        EXPECT_EQ (0, page.GetSelectedItemCount ());
        EXPECT_EQ (0, page.GetSelectedGroupCount ());

        // the last item of a group going takes the group, through the page's own code
        page.m_cTreeCtrl.DeleteItem (c);
        page.CheckParentHasChildren (g2);
        EXPECT_EQ (1, (int) page.m_GroupsMap.size ());
        EXPECT_EQ (2, page.GetItemCount ());

        // and a group that still has one stays
        page.CheckParentHasChildren (g1);
        EXPECT_EQ (2, page.GetItemCount ());
    }
    parent.Detach ();
    ::DestroyWindow (hParent);
}

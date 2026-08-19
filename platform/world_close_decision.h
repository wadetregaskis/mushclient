// platform/world_close_decision.h — which questions closing a world asks, and
// the order every world gets closed in when the whole app quits.
//
// Cmd+W on a world window runs a prompt chain inside CWorldChildFrame::OnClose.
// That chain is a pure function of six flags, but it lived inside the wx close
// handler, where nothing can test it: every in-app selftest exits from
// wxApp::OnInit, so there is no event loop and no modal can ever complete.
// Lifted out here it is directly callable and carries no wx or MFC types at all,
// so the whole truth table runs headlessly (tests/test_mfc_ui.cpp) — the
// pal/image_placement.h treatment.
//
// The questions themselves are NOT the port's invention: they are the ones
// CMUSHclientDoc::SaveModified (doc.cpp:2101) asks, which is the chain Cmd+Q
// runs through the quit hook.  Cmd+W used to ask a DIFFERENT set — it never
// warned that closing would end a live session, and it repurposed
// m_bConfirmBeforeClosingWorld into a general "Close <world>?" prompt that
// Windows never shows.  Windows spends that preference on the connected-session
// warning and nothing else, so that is what it buys here now.
//
// NOT modelled here: the world option "save world automatically on close".
// OnClose performs that save BEFORE asking the last two questions and then
// RE-READS IsModified(), because a save that fails must still reach the
// unsaved-changes prompt.  Folding it in would mean predicting whether the save
// succeeded, so the caller does the save and passes the refreshed `modified`
// flag in.  Note the ORDER that implies, and that Windows fixes: askEndSession
// is asked BEFORE that automatic save, so cancelling it leaves the world file
// untouched as well as the window open.

#pragma once

// What OnClose should put up, in this order.  More than one can be set: a world
// that is connected AND has unsaved changes is warned about the session and
// then asked about the file, which is what SaveModified does.
struct WorldClosePlan {
    bool askEndSession    = false;   // "This will end your <world> session."  OK/Cancel
    bool askSaveVariables = false;   // "Variables have changed … save?"  Y/N/Cancel
    bool askSaveModified  = false;   // "Save changes to <world>?"        Y/N/Cancel
};

// canVeto                 — the close event can be refused.  FALSE for a forced
//                           close (Close(true), app shutdown): asking a question
//                           nobody can answer would only wedge the teardown, so
//                           every prompt is skipped.
// connected               — there is a live session to lose: ConnectionOpen()
//                           AND m_iConnectPhase == eConnectConnectedToMud, the
//                           same pair SaveModified tests.  Merely having a
//                           socket part-way through connecting is not enough.
// modified                — the document has unsaved changes RIGHT NOW (i.e.
//                           after any automatic save, see the note above).
// variablesChanged        — script variables changed; this does NOT mark the
//                           document modified, so without its own question the
//                           world would close silently and lose them.
// confirmSavingVariables  — App.m_bConfirmBeforeSavingVariables (global pref).
// confirmClosingWorld     — App.m_bConfirmBeforeClosingWorld (global pref).
inline WorldClosePlan DecideWorldClose(bool canVeto,
                                       bool connected,
                                       bool modified,
                                       bool variablesChanged,
                                       bool confirmSavingVariables,
                                       bool confirmClosingWorld)
{
    WorldClosePlan plan;
    if (!canVeto)
        return plan;                 // nothing may be asked

    // doc.cpp:2103 — the preference guards this and ONLY this.  A disconnected
    // world closes without comment however it is set, and a connected one is
    // warned whether or not it also has unsaved changes.
    plan.askEndSession = connected && confirmClosingWorld;

    // doc.cpp:2125 — variables-only changes, which leave IsModified() false.
    plan.askSaveVariables = !modified && variablesChanged && confirmSavingVariables;

    // doc.cpp:2155, i.e. CDocument::SaveModified's own prompt.
    plan.askSaveModified = modified;

    return plan;
}

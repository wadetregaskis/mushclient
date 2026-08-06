// platform/world_close_decision.h — which questions closing a world asks.
//
// Cmd+W on a world window runs a four-way prompt chain inside
// CWorldChildFrame::OnClose.  That chain is a pure function of five flags, but
// it lived inside the wx close handler, where nothing can test it: every in-app
// selftest exits from wxApp::OnInit, so there is no event loop and no modal can
// ever complete.  Lifted out here it is directly callable and carries no wx or
// MFC types at all, so the whole truth table runs headlessly
// (tests/test_mfc_ui.cpp) — the pal/image_placement.h treatment.
//
// This describes the PORT's Cmd+W chain only.  Cmd+Q takes a different route
// (the canonical CMUSHclientDoc::SaveModified, doc.cpp:2091) and Windows has no
// equivalent of this handler at all, so nothing here is shared with Windows —
// but the two must keep answering the same questions, which is easier to check
// against a table than against a nest of ifs.
//
// NOT modelled here: the world option "save world automatically on close".
// OnClose performs that save BEFORE asking anything and then RE-READS
// IsModified(), because a save that fails must still reach the unsaved-changes
// prompt.  Folding it in would mean predicting whether the save succeeded, so
// the caller does the save and passes the refreshed `modified` flag in.

#pragma once

// What OnClose should put up, in this order.  More than one can be set: a world
// whose variables changed AND whose owner asked to confirm every close is asked
// twice, which is what the code does today.
struct WorldClosePlan {
    bool askSaveVariables = false;   // "Variables have changed … save?"  Y/N/Cancel
    bool askSaveModified  = false;   // "Save changes to <world>?"        Y/N/Cancel
    bool askConfirmClose  = false;   // "Close <world>?"                  Y/N
};

// canVeto                 — the close event can be refused.  FALSE for a forced
//                           close (Close(true), app shutdown): asking a question
//                           nobody can answer would only wedge the teardown, so
//                           every prompt is skipped.
// modified                — the document has unsaved changes RIGHT NOW (i.e.
//                           after any automatic save, see the note above).
// variablesChanged        — script variables changed; this does NOT mark the
//                           document modified, so without its own question the
//                           world would close silently and lose them.
// confirmSavingVariables  — App.m_bConfirmBeforeSavingVariables (global pref).
// confirmClosingWorld     — App.m_bConfirmBeforeClosingWorld (global pref).
inline WorldClosePlan DecideWorldClose(bool canVeto,
                                       bool modified,
                                       bool variablesChanged,
                                       bool confirmSavingVariables,
                                       bool confirmClosingWorld)
{
    WorldClosePlan plan;
    if (!canVeto)
        return plan;                 // nothing may be asked

    plan.askSaveVariables = !modified && variablesChanged && confirmSavingVariables;

    if (modified)
        plan.askSaveModified = true; // the save question covers the close too
    else
        plan.askConfirmClose = confirmClosingWorld;

    return plan;
}

// wx_update.h -- what the port does about updates.  The decisions are in
// update_logic.h and update_service.h; this is the wx face of them, as
// dialogs/UpdateDlg.* is Windows'.

#pragma once

#include <string>

class wxWindow;

// Once the App's preferences are loaded.  Starts the timer that drives the checks.
void WxUpdate_Start ();

// Help > Check for Updates.
void WxUpdate_CheckNow ();

// A click on the badge in a world's status bar.
void WxUpdate_OpenDialog (wxWindow * parent);

// "Update available: 6.10", or "" when there's nothing to say.
std::string WxUpdate_BadgeText ();

// platform/wx_mfc_logic.h
// The engine-shaped decisions lifted out of the wx entry points in
// wx_mfc_impl.cpp, so that something can call them.
//
// Every in-app selftest runs inside wxApp::OnInit and exits before OnRun, so
// there is no event loop: a modal never completes, a paint never fires, and a
// CHAR_HOOK is never delivered.  Logic sealed inside such an entry point is
// untestable by construction — which is how the MB_* decode below shipped three
// separate bugs.  The remedy is the one already used by
// COutputPane::AdoptMeasuredMetrics and CWorldChildFrame::PaneLineToDocLine:
// split the decision from the effect, leave the wx call site calling it, and
// test the decision.
//
// Deliberately NOT included from mfc_ui.h / wx_mfc_hooks.h — those reach every
// TU in the build, and this is only wanted by wx_mfc_impl.cpp and the two
// selftests.

#pragma once

#ifndef _WIN32

#include <string>
#include <vector>

struct MSG;             // platform/mfc_ui.h
class  CMenu;           // platform/mfc_ui.h
class  wxMenu;          // wx/menu.h
class  wxConfigBase;    // wx/confbase.h

// ── ::MessageBox MB_* decode (hook_message_box) ────────────────────────────
// Abort/Retry/Ignore and Retry/Cancel have no wx button set of their own, so
// they borrow one and relabel it.  The style alone therefore cannot say which
// MB_* code a given wx id means; MessageBoxStyleForMbFlags reports the borrow
// and MbResultForWxId consumes it.
enum MbRelabel { eNoRelabel = 0, eAbortRetryIgnore = 1, eRetryCancel = 2 };

// Decode the three independent MB_* fields (button set = low nibble, an
// ENUMERATION not flags; icon = 0xF0; default button = 0x0300) into one wx
// message-dialog style.  *relabel is always written.
long MessageBoxStyleForMbFlags(unsigned flags, MbRelabel* relabel);

// Map a wxMessageDialog::ShowModal result back to the MB_* return code the
// caller is branching on (IDOK / IDCANCEL / IDYES / IDNO / IDABORT / IDRETRY /
// IDIGNORE), honouring the relabelling above.
int MbResultForWxId(MbRelabel relabel, int wxId);

// ── MFC dialog CHAR_HOOK key translation ───────────────────────────────────
// Fill *msg for the three keys the bridge offers to PreTranslateMessage (TAB,
// RETURN — as WM_SYSKEYDOWN when Alt is down — and ESCAPE), and return true.
// Returns false — leaving *msg untouched — for every other key, which the
// caller must Skip() so the focused control's own key handling survives.
// Escape is translated because wx's own Esc→Cancel looks the button up by
// wxID_CANCEL (5101), an id no bridged control carries (template buttons keep
// their Win32 ids, IDCANCEL == 2) — the bridge dispatches the IDCANCEL
// command itself when PreTranslateMessage declines the key.
bool DialogKeyToMsg(int wxKeyCode, bool altDown, MSG* msg);

// ── CMenu → wxMenu (world.WindowMenu, CMiniWindow::Menu) ───────────────────
// Renders the portable CMenu tree as a wxMenu the caller owns.
wxMenu* wx_build_popup_menu(CMenu* m);

// The filename to offer in the Save-As dialog for a world with no path yet.
//
// DefaultWorldFileName (utilities_shared.cpp) does the scrubbing, on the world
// name's raw bytes.  This adds the wx conversion -- AND the fallback, which has
// to come after it: wxString::FromUTF8 is all-or-nothing, so a world name that
// is not valid UTF-8 converts to an EMPTY string, and an empty filename in a
// Save dialog is worse than a generic one.  The pre-hoist code converted first
// and got that fallback for free; converting last loses it unless it is put
// back here.
wxString DefaultWorldFileNameForName(const CString& worldName);

// ── Recent-worlds MRU ──────────────────────────────────────────────────────
// Move `path` to the front of `list`, removing any earlier occurrence, and trim
// to `maxEntries`.  An empty path is ignored.
void RecentWorldsPromote(std::vector<std::string>& list,
                         const std::string& path,
                         int maxEntries);

// Load / persist the MRU through an explicit config object.  The `cfg`
// parameter is what lets a test round-trip these against a scratch file instead
// of the user's real preferences.  The save MUST delete the tail beyond
// `list.size()`, because the load stops at the first gap: leave a stale entry
// behind and it reappears as a phantom recent world.
void RecentWorldsLoadFrom(wxConfigBase& cfg, std::vector<std::string>& out,
                          int maxEntries);
void RecentWorldsSaveTo(wxConfigBase& cfg, const std::vector<std::string>& list,
                        int maxEntries);

// Rebuild `menu` to show `list`, one item per world at consecutive ids from
// `idBase`, or a single DISABLED placeholder when the list is empty.
void RecentWorldsBuildMenu(wxMenu* menu, const std::vector<std::string>& list,
                           int idBase);

// The live list behind File ▸ Open Recent, and the menu-id block it occupies —
// exposed so the menu-bar selftest can check the menu against the list that
// built it.
const std::vector<std::string>& RecentWorldsList();
int RecentWorldsMaxEntries();
int RecentWorldsMenuIdBase();

#endif // !_WIN32

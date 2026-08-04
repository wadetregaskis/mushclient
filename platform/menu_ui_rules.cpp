// platform/menu_ui_rules.cpp — see platform/menu_ui_rules.h.
//
// PORT-ONLY (this TU answers a question MFC answers for itself on Windows), but
// every answer here is read off the canonical source:
//
//   * the world-only set is exactly the commands whose only ON_COMMAND entry is
//     in CMUSHView's (mushview.cpp), CSendView's (sendvw.cpp) or
//     CMUSHclientDoc's (doc.cpp) message map — nothing on CMainFrame or the
//     app — so MFC greys them with no world open;
//   * "Quit From This World" is stricter still: sendvw.cpp routes
//     ID_ACTIONS_QUIT's update-UI to CSendView::OnUpdateSessionOpen, which
//     enables it only while the session is connected;
//   * the three check marks come from the canonical update-UI handlers
//     themselves — CMUSHclientDoc::OnUpdateGameTrace (doc.cpp:4035) and
//     OnUpdateEditDebugincomingpackets (doc.cpp:2896) are dispatched through
//     the real message map here, and Freeze Output mirrors
//     CMUSHView::OnUpdateDisplayFreezeoutput's `SetCheck(m_freeze)`
//     (mushview.cpp:3320) by pulling the freeze state through pal::IWorldView.

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"

#include "platform/menu_ui_rules.h"

// ── The world-only commands ───────────────────────────────────────────────
// Each entry's canonical handler lives ONLY on the view/document, so Windows
// greys it when no world window is active.
static const int kNeedsWorld [] = {
  // CMUSHView (mushview.cpp) — output-window commands
  ID_DISPLAY_FIND,                // 32248
  ID_DISPLAY_FINDAGAIN,           // 32249
  ID_DISPLAY_FREEZEOUTPUT,        // 32201  (also a check item, below)
  ID_DISPLAY_GOTOLINE,            // 32963
  ID_FILE_PRINT_WORLD,            // 32250
  ID_DISPLAY_GOTOURL,             // 32275
  ID_DISPLAY_SENDMAILTO,          // 32276
  ID_DISPLAY_HIGHLIGHTPHRASE,     // 33036
  ID_DISPLAY_MULTILINETRIGGER,    // 33044
  ID_DISPLAY_TEXTATTRIBUTES,      // 32331
  ID_DISPLAY_BOOKMARKSELECTION,   // 32318
  ID_DISPLAY_GOTOBOOKMARK,        // 32319
  // CSendView (sendvw.cpp) — command-window commands
  ID_INPUT_GLOBALCHANGE,          // 32320
  ID_EDIT_SPELLCHECK,             // 32323
  ID_EDIT_GENERATECHARACTERNAME,  // 32326
  ID_REPEAT_LAST_COMMAND,         // 32272
  ID_GAME_COMMANDHISTORY,         // 32152
  ID_EDIT_GOTOMATCHINGBRACE,      // 32946
  ID_EDIT_SELECTTOMATCHINGBRACE,  // 32947
  ID_GAME_FUNCTIONSLIST,          // 33046
  // CMUSHclientDoc (doc.cpp)
  ID_EDIT_FLIPTONOTEPAD,          // 32953
  ID_FILE_PLUGINS,                // 32983
  ID_EDIT_DEBUGINCOMINGPACKETS,   // 32971  (also a check item, below)
  ID_GAME_TRACE,                  // 32334  (also a check item, below)
};

MenuCmdUiState MenuCommandUiState (int id, CMUSHclientDoc* doc)
  {
MenuCmdUiState state;

  // "Quit From This World" sends the world's QUIT macro, so it needs a live
  // session, not merely an open world (CSendView::OnUpdateSessionOpen).
  if (id == ID_ACTIONS_QUIT)
    {
    state.handled = true;
    state.enabled = doc != NULL && doc->m_iConnectPhase == eConnectConnectedToMud;
    return state;
    }

  for (size_t i = 0; i < sizeof (kNeedsWorld) / sizeof (kNeedsWorld [0]); i++)
    if (kNeedsWorld [i] == id)
      {
      state.handled = true;
      state.enabled = doc != NULL;
      break;
      }

  if (!state.handled)
    return state;

  // ── check marks ─────────────────────────────────────────────────────────
  // The two diagnostics toggles are per-WORLD flags, so the tick has to be
  // re-read from the active document every time the menu is drawn — that is
  // what ON_UPDATE_COMMAND_UI does on Windows.  Ask the canonical handler
  // rather than the member, so this cannot drift from doc.cpp.
  if (id == ID_EDIT_DEBUGINCOMINGPACKETS || id == ID_GAME_TRACE)
    {
    state.checked = 0;
    if (doc)
      {
      bool enabled = true;
      int  checked = -1;
      CCmdUI cmdui;
      cmdui.m_nID            = (UINT) id;
      cmdui.m_pCaptureEnable = &enabled;
      cmdui.m_pCaptureCheck  = &checked;
      if (AfxDispatchUpdateUI (doc, &cmdui))
        {
        state.enabled = enabled;
        state.checked = (checked > 0) ? 1 : 0;
        }
      }
    return state;
    }

  // Freeze Output is view state on Windows (CMUSHView::m_freeze); on the port
  // it is the output pane's autoscroll pause, pulled back through the view seam.
  if (id == ID_DISPLAY_FREEZEOUTPUT)
    {
    state.checked = 0;
    if (doc && doc->m_pWorldView && doc->m_pWorldView->isPaused ())
      state.checked = 1;
    return state;
    }

  return state;
  }   // end of MenuCommandUiState

// ── Canonical ids the port's menu bar spells with a wx id ─────────────────
PortMenuAction PortMenuActionForCommandId (int id)
  {
  switch (id)
    {
    case ID_FILE_CLOSE:      return kPortMenuCloseWorld;   // 0xE102
    case ID_EDIT_CUT:        return kPortMenuCut;          // 0xE123
    case ID_EDIT_PASTE:      return kPortMenuPaste;        // 0xE125
    case ID_EDIT_SELECT_ALL: return kPortMenuSelectAll;    // 0xE12A
    case ID_EDIT_UNDO:       return kPortMenuUndo;         // 0xE12B
    case ID_APP_ABOUT:       return kPortMenuAbout;        // 0xE140
    case ID_WINDOW_ARRANGE:  return kPortMenuArrangeIcons; // 0xE131
    case ID_WINDOW_CASCADE:  return kPortMenuCascade;      // 0xE132
    case ID_WINDOW_TILE_HORZ:return kPortMenuTileHorz;     // 0xE133
    case ID_WINDOW_TILE_VERT:return kPortMenuTileVert;     // 0xE134
    }

  return kPortMenuNone;
  }   // end of PortMenuActionForCommandId

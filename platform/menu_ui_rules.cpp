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
//   * Connect and Disconnect are not answered here at all — their canonical
//     ON_UPDATE_COMMAND_UI handlers are on the document and are dispatched
//     through the real message map (doc.cpp:1891 and :1900);
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
  // Edit > Repeat Last Word (CSendView).  Carried over from the input cluster,
  // whose ad-hoc id list this table replaced -- it was the one id in that list
  // with no rule here.
  ID_REPEAT_LAST_WORD,            // 32973
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

// Ask the document's own ON_UPDATE_COMMAND_UI handler for `canonicalId`.
// Nothing is restated: the handler runs, and whatever it enables/checks is what
// the menu gets.  Returns false when no handler answered, so the caller can
// decide the default rather than inheriting a half-filled answer.
//
// "No handler answered" is rarer than it looks, and the reason matters: the
// document's message map ends with ON_UPDATE_COMMAND_UI_RANGE(0, 0xFFFF,
// OnFixMenus) (doc.cpp:203), and OnFixMenus enables nothing.  AfxDispatchUpdateUI
// takes the FIRST matching entry, so every id with a specific ON_UPDATE_COMMAND_UI
// above that line still reaches its own handler — but an id WITHOUT one comes back
// TRUE with `enabled` untouched.  Every caller here names an id that has one.
static bool AskTheDocument (CMUSHclientDoc* doc, int canonicalId,
                            bool& enabled, int& checked)
  {
  if (doc == NULL)
    return false;

  CCmdUI cmdui;
  cmdui.m_nID            = (UINT) canonicalId;
  cmdui.m_pCaptureEnable = &enabled;
  cmdui.m_pCaptureCheck  = &checked;
  return AfxDispatchUpdateUI (doc, &cmdui) != FALSE;
  }

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

  // ── Connect / Disconnect ────────────────────────────────────────────────
  // Deliberately NOT restated: both have canonical update-UI handlers on the
  // document (CMUSHclientDoc::OnUpdateConnectionConnect, doc.cpp:1891, and
  // OnUpdateConnectionDisconnect, doc.cpp:1900) and the message map dispatches
  // them perfectly well with no window in sight.  The frame used to carry a
  // hand-written copy of each, which is exactly how Disconnect's rule — enabled
  // on EVERY non-zero phase, so that a connect to a host that never answers can
  // still be cancelled — has come adrift before.
  if (id == ID_CONNECTION_CONNECT ||
      id == ID_CONNECTION_DISCONNECT || id == kPortMenuIdDisconnect)
    {
    state.handled = true;
    // The handlers are document methods, so with no world open there is nobody
    // to ask — and nothing to connect or disconnect either way.  This is the
    // one part of the answer the canonical code cannot give us.
    state.enabled = false;
    bool enabled = true;
    int  checked = -1;
    if (AskTheDocument (doc,
                        id == ID_CONNECTION_CONNECT ? ID_CONNECTION_CONNECT
                                                    : ID_CONNECTION_DISCONNECT,
                        enabled, checked))
      state.enabled = enabled;
    return state;
    }

  // Reconnect is the port's own item — Windows has no such command — so this
  // is a rule in its own right rather than a copy of one, and it lives here
  // beside the two it sits between in the menu.  It shares Connect's phase test
  // but not Connect's `m_server != "0.0.0.0"` clause, so the two are pinned
  // separately: an unaddressed world greys Connect and not Reconnect.
  if (id == kPortMenuIdReconnect)
    {
    state.handled = true;
    state.enabled = doc != NULL && doc->m_iConnectPhase == eConnectNotConnected;
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
    bool enabled = true;
    int  checked = -1;
    if (AskTheDocument (doc, id, enabled, checked))
      {
      state.enabled = enabled;
      state.checked = (checked > 0) ? 1 : 0;
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

// ── Rules for the port's own menu items ───────────────────────────────────
MenuCmdUiState PortMenuRuleUiState (PortMenuRule rule, CMUSHclientDoc* doc)
  {
MenuCmdUiState state;

  state.handled = true;

  // Fail CLOSED.  MenuCmdUiState::enabled defaults to true, so an enumerator
  // added without a case here would return "handled, and enabled" -- a menu
  // item silently live with no world behind it.  Start from disabled and let
  // each arm earn its way up.
  state.enabled = false;

  switch (rule)
    {
    // Execute Lua… and Reload Script both drive the world's script engine, so
    // they need a world that HAS one.  (Windows greys Reload Script on a
    // stricter test of its own — CMUSHclientDoc::OnUpdateGameReloadScriptFile
    // asks whether the engine actually built — and has no Execute Lua at all.)
    case kPortRuleScripting:
      state.enabled = doc != NULL && doc->m_bEnableScripts;
      break;

    // One menu item stands for two commands here, so its LABEL is the only
    // thing that says which one it currently is.  Windows keeps them apart a
    // different way: File ▸ Log Session is a fixed caption with a tick
    // (CMUSHclientDoc::OnUpdateFileLogsession), and the port's Connection menu
    // has no tick to show.  Same state, read the same way — m_logfile is open
    // exactly while the session is being logged.
    // With NO world the caption is deliberately left ALONE rather than reset to
    // "Start": there is no session to describe either way, and the item is
    // greyed out, so the only thing a reset would change is that the caption of
    // a world you just closed stops matching what it was doing.  (Whether that
    // is the better answer is a separate question; this is what the frame did.)
    case kPortRuleLogToggle:
      state.enabled = doc != NULL;
      if (doc != NULL)
        state.text = doc->m_logfile != NULL ? "Stop &Logging"
                                            : "Start &Logging\xE2\x80\xA6";
      break;
    }

  return state;
  }   // end of PortMenuRuleUiState

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

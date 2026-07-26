// doc_hyperlink_shared.cpp — clicking an MXP send-link / prompt-link.
//
// ONE BODY, EVERY BUILD (Windows, macOS/Linux, headless tests).
//
// This was the ACTION_SEND / ACTION_PROMPT arm of CMUSHView::OnLButtonDown in
// mushview.cpp.  Almost none of it is view work: it decides whether a note-line
// link executes rather than sends, decodes the !!pluginID:sub(arg) plugin-call
// convention (with the canonical ColourNote error reporting), and otherwise
// sends the action with the world's "echo hyperlink" option.  The only genuinely
// view-shaped steps — putting text in the command bar, adding to the command
// history, moving the focus — are handed back to the caller as an
// eHyperlinkOutcome instead of being guessed at here.
//
// Why it moved: the wx port never instantiates CMUSHView (CChildFrame's splitter
// is a stub, so OnCreateClient bails), so this code was dead there and the port
// grew its own link-click lambda with materially different behaviour — it called
// EvaluateCommand (which alias-expands, which Windows deliberately does NOT do
// for a link), so it hardcoded m_display_my_input as the echo flag instead of
// m_bEchoHyperlinkInOutputWindow (backlog item 41), never touched the command
// history (item 40), treated ACTION_PROMPT links as sends, and had no
// !!pluginID:sub(arg) decode at all — so the documented way for a plugin to make
// a link that calls back into itself (lua/mapper.lua uses it) went to the MUD as
// a literal ~60-character command.
//
// Windows behaviour is unchanged: same tests in the same order, same error
// messages, and the two steps that used to touch m_bottomview still happen in
// mushview.cpp, in the same place, guarded by the same world options.

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/errors.h"   // eOK / eNoSuchPlugin / eNoSuchRoutine / ...

eHyperlinkOutcome CMUSHclientDoc::HyperlinkClicked (const CString strActionArg,
                                                    const long    iActionType,
                                                    const bool    bCommentLine)
  {
  CString strAction = strActionArg;

  if (iActionType == ACTION_SEND && bCommentLine)
    {
    // action_send on a note line will be executed, not sent
    //  (for the world.Hyperlink function)
    // However an action in the form !!pluginID:script(arg)
    // eg. !!753ba7e011f3c8943a885f18:mysub(1234)
    // will be passed the nominted sub in the nominated plugin

    // rather elaborate test ...

    // a. Must start with !!
    // b. Must end with )
    // c. Must have a "(" in it
    // d. After the !! there must be 24 hex characters
    // e. After that must be a colon (:)
    // f. After the colon must be a valid subroutine name
    if (strAction.GetLength () >= PLUGIN_UNIQUE_ID_LENGTH + 5 &&
        strAction.Left (2) == "!!" &&
        strAction.Right (1) == ")" &&
        strAction.Find ("(") != -1 &&
        strAction.Mid (PLUGIN_UNIQUE_ID_LENGTH + 2, 1) == ":" &&
        IsPluginID (strAction.Mid (2, PLUGIN_UNIQUE_ID_LENGTH)) &&
        IsSubName (strAction.Mid (PLUGIN_UNIQUE_ID_LENGTH + 3)))
      { // correct syntax for plugin call
      CString strPluginID = strAction.Mid (2, PLUGIN_UNIQUE_ID_LENGTH);
      strAction = strAction.Mid (PLUGIN_UNIQUE_ID_LENGTH + 3);
      int iBracket = strAction.Find ("(");
      if (iBracket != -1)
        {
        CString strScriptName = strAction.Left (iBracket);
        strScriptName.TrimRight ();
        CString strArg = strAction.Mid (iBracket + 1);
        strArg = strArg.Left (strArg.GetLength () - 1); // drop trailing )
        if (strPluginID == DEBUG_PLUGIN_ID)  // just made that up ;)
          DebugHelper (strScriptName, strArg);
        else
          {
          long iResult = CallPlugin (strPluginID, strScriptName, strArg);
          CString strName = strPluginID;  // default to ID
          if (iResult != eOK)
            {
            CPlugin * pPlugin = GetPlugin (strPluginID);
            if (pPlugin)
              strName = pPlugin->m_strName;
            } // end of finding plugin's name
          switch (iResult)
            {
            case eNoSuchPlugin:
              ColourNote ("white", "red",
                    TFormat ("Plugin \"%s\" is not installed",
                            (LPCTSTR) strName));
              break;
            case eNoSuchRoutine:
              ColourNote ("white", "red",
                    TFormat ("Script routine \"%s\" is not in plugin %s",
                            (LPCTSTR) strScriptName,
                            (LPCTSTR) strName));
              break;

            case eErrorCallingPluginRoutine:
              ColourNote ("white", "red",
                    TFormat ("An error occurred calling plugin %s",
                            (LPCTSTR) strName));
              break;

            } // end of switch on errors
          }   // end of not debug helper ID
        }  // end of ( found (this should occur, really)
      }   // end of passing special syntax test
    else
      {  // plugin does not exist - just execute it
      m_iExecutionDepth = 0;
      Execute (strAction);
      }
    return eHyperlinkNothing;
    } // end of note hyperlink

  // not note line

  if (CheckConnected ())
    return eHyperlinkAbort;

  // send to command window?
  if (iActionType == ACTION_PROMPT)
    return eHyperlinkFillCommand;

  // send it
  SendMsg (strAction,
           m_bEchoHyperlinkInOutputWindow,
           false,           // don't queue
           LoggingInput ());

  return m_bHyperlinkAddsToCommandHistory ? eHyperlinkAddHistory
                                          : eHyperlinkNothing;

  } // end of CMUSHclientDoc::HyperlinkClicked

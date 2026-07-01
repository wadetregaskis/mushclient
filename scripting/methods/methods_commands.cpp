// methods_commands.cpp

// Command window  /  command handling

#include "stdafx.h"
#include "../../MUSHclient.h"
#include "../../doc.h"
#include "../../mainfrm.h"
#include "../../sendvw.h"
#include "../../childfrm.h"
#include "../errors.h"

// Implements:

//    DeleteCommandHistory
//    DoCommand
//    Execute
//    GetCommand
//    GetCommandList
//    GetInternalCommandsList
//    GetQueue
//    PasteCommand
//    PushCommand
//    SelectCommand
//    SetCommand
//    SetCommandSelection
//    SetCommandWindowHeight
//    SetInputFont
//    ShiftTabCompleteItem

extern tCommandIDMapping CommandIDs [];

// world.GetCommandList - returns a variant array which is a list of "count" recent commands

VARIANT CMUSHclientDoc::GetCommandList(long Count)
{
  COleSafeArray sa;   // for command list

#ifndef _WIN32
  if (m_fn_get_command_list) {
    std::vector<std::string> hist = m_fn_get_command_list(Count);
    if (!hist.empty()) {
      long n = (long)hist.size();
      sa.CreateOneDim(VT_VARIANT, n);
      for (long i = 0; i < n; ++i) {
        COleVariant v(CString(hist[i].c_str()));
        sa.PutElement(&i, &v);
      }
    }
  }
#else
  for(POSITION pos = GetFirstViewPosition(); pos != NULL; )
	  {
	  CView* pView = GetNextView(pos);

	  if (pView->IsKindOf(RUNTIME_CLASS(CSendView)))
  	  {
		  CSendView* pmyView = (CSendView*)pView;

      POSITION pos;
      long iCount;

      // put the commands into the array
      if (!pmyView->m_msgList.IsEmpty ())    // cannot create empty dimension
        {
        if (Count <= 0)
          Count = pmyView->m_msgList.GetCount ();    // zero means all
        else
          Count = MIN (Count, pmyView->m_msgList.GetCount ());  // don't exceed what we have

        sa.CreateOneDim (VT_VARIANT, Count);

        for (iCount = 0, pos = pmyView->m_msgList.GetTailPosition();
            pos && iCount < Count;
            )
          {
          CString strLine = pmyView->m_msgList.GetPrev (pos);

          // the array must be a bloody array of variants, or VBscript kicks up
          COleVariant v (strLine);
          sa.PutElement (&iCount, &v);
          iCount++;
          }      // end of looping through each command
        } // end of having at least one

	    }	  // end of being a CSendView
    }   // end of loop through views
#endif

	return sa.Detach ();
}    // end of CMUSHclientDoc::GetCommandList

// world.PushCommand - pushes the command in the command window onto the command
//                     stack and then blanks it out
//  returns command that was pushed

BSTR CMUSHclientDoc::PushCommand()
{
	CString strCommand;

#ifndef _WIN32
  if (m_fn_push_command) {
    std::string s = m_fn_push_command();
    strCommand = s.c_str();
  }
#else
  for(POSITION pos = GetFirstViewPosition(); pos != NULL; )
	  {
	  CView* pView = GetNextView(pos);

	  if (pView->IsKindOf(RUNTIME_CLASS(CSendView)))
  	  {
		  CSendView* pmyView = (CSendView*)pView;

      // find what the command is
      pmyView->GetEditCtrl().GetWindowText (strCommand);

      // save the echo flag
      bool bSaveNoEcho = m_bNoEcho;
      m_bNoEcho = false;  // make sure we save it

      // remember it
      pmyView->AddToCommandHistory (strCommand);

      // restore the command
      m_bNoEcho = bSaveNoEcho;

      pmyView->GetEditCtrl().SetSel (0, -1);   // select all
      pmyView->GetEditCtrl().ReplaceSel ("", TRUE);   // blank it out
      pmyView->NotifyPluginCommandChanged ();

      }	  // end of being a CSendView
    }   // end of loop through views
#endif

	return strCommand.AllocSysString();
}   // end of CMUSHclientDoc::PushCommand

// world.SelectCommand - selects the command in the command window

void CMUSHclientDoc::SelectCommand()
{
#ifndef _WIN32
  if (m_fn_select_command)
    m_fn_select_command();
#else
  for(POSITION pos = GetFirstViewPosition(); pos != NULL; )
	  {
	  CView* pView = GetNextView(pos);

	  if (pView->IsKindOf(RUNTIME_CLASS(CSendView)))
  	  {
		  CSendView* pmyView = (CSendView*)pView;

      pmyView->GetEditCtrl().SetSel (0, -1);   // select all

      }	  // end of being a CSendView
    }   // end of loop through views
#endif
}    // end of CMUSHclientDoc::SelectCommand

// world.GetCommand - gets the command in the command window

BSTR CMUSHclientDoc::GetCommand()
{
	CString strCommand;

#ifndef _WIN32
  // Non-Windows: the command input bar is owned by CWorldChildFrame; retrieve
  // its text via the callback wired in wx_world_child.cpp.
  if (m_fn_get_command) {
    std::string s = m_fn_get_command();
    strCommand = s.c_str();
  }
#else
  for(POSITION pos = GetFirstViewPosition(); pos != NULL; )
	  {
	  CView* pView = GetNextView(pos);

	  if (pView->IsKindOf(RUNTIME_CLASS(CSendView)))
  	  {
		  CSendView* pmyView = (CSendView*)pView;

      // find what the command is
      pmyView->GetEditCtrl().GetWindowText (strCommand);

      }	  // end of being a CSendView
    }   // end of loop through views
#endif

	return strCommand.AllocSysString();
}   // end of CMUSHclientDoc::GetCommand



long CMUSHclientDoc::SetCommand(LPCTSTR Message)
{
#ifndef _WIN32
  // Non-Windows: delegate to the wxWidgets input bar.
  if (m_fn_set_command) {
    std::string text(Message ? Message : "");
    return m_fn_set_command(text) ? eOK : eCommandNotEmpty;
  }
  return eOK;
#else
  for(POSITION pos = GetFirstViewPosition(); pos != NULL; )
	  {
	  CView* pView = GetNextView(pos);

	  if (pView->IsKindOf(RUNTIME_CLASS(CSendView)))
  	  {
		  CSendView* pmyView = (CSendView*)pView;

      CString strCurrent;

      pmyView->GetEditCtrl().GetWindowText (strCurrent);
      if (strCurrent.IsEmpty ())
        {
        pmyView->GetEditCtrl().ReplaceSel (Message, TRUE);
        pmyView->NotifyPluginCommandChanged ();
        return eOK;
        }   // end of command being empty
      else
        return eCommandNotEmpty;
	    }	  // end of being a CSendView
    }   // end of loop through views

	return eOK;
#endif
}     // end of CMUSHclientDoc::SetCommand

BSTR CMUSHclientDoc::PasteCommand(LPCTSTR Text)
{
CString strResult;

#ifndef _WIN32
  // Non-Windows: PasteCommand appends Text to the current command bar contents
  // (replacing any selection) and returns the previously selected text.
  // We approximate by calling m_fn_set_command with the appended text.
  if (m_fn_get_command && m_fn_set_command) {
    std::string current = m_fn_get_command();
    std::string appended = current + std::string(Text ? Text : "");
    m_fn_set_command(appended);
    // No selection support on non-Windows; return empty string.
  }
#else
int nStartChar,
    nEndChar;

  for(POSITION pos = GetFirstViewPosition(); pos != NULL; )
	  {
	  CView* pView = GetNextView(pos);

	  if (pView->IsKindOf(RUNTIME_CLASS(CSendView)))
  	  {
		  CSendView* pmyView = (CSendView*)pView;

      CString strCurrent;

      // find the selection range
      pmyView->GetEditCtrl().GetSel(nStartChar, nEndChar);
      // get window text
      pmyView->GetEditCtrl().GetWindowText (strCurrent);
      pmyView->GetEditCtrl().ReplaceSel (Text, TRUE);
      pmyView->NotifyPluginCommandChanged ();

      // get selection if any
      if (nEndChar > nStartChar)
        strResult = strCurrent.Mid (nStartChar, nEndChar - nStartChar);
      break;

	    }	  // end of being a CSendView
    }   // end of loop through views
#endif

	return strResult.AllocSysString();
}   // end of CMUSHclientDoc::PasteCommand


void CMUSHclientDoc::DeleteCommandHistory()
{
#ifndef _WIN32
  if (m_fn_delete_command_history)
    m_fn_delete_command_history();
#else
  for(POSITION pos = GetFirstViewPosition(); pos != NULL; )
	  {
	  CView* pView = GetNextView(pos);

	  if (pView->IsKindOf(RUNTIME_CLASS(CSendView)))
  	  {
		  CSendView* pmyView = (CSendView*)pView;

      // OK, do it ...
	    pmyView->m_msgList.RemoveAll ();
      pmyView->m_HistoryPosition = NULL;
      pmyView->m_inputcount = 0;
      pmyView->m_HistoryFindInfo.m_pFindPosition = NULL;
      pmyView->m_HistoryFindInfo.m_nCurrentLine = 0;
      pmyView->m_HistoryFindInfo.m_bAgain = FALSE;
      pmyView->m_strPartialCommand.Empty ();
      pmyView->m_last_command.Empty ();

      }	  // end of being a CSendView
    }   // end of loop through views
#endif
}   // end of CMUSHclientDoc::DeleteCommandHistory



long CMUSHclientDoc::Execute(LPCTSTR Command) 
{

// remember current plugin
CPlugin *  pCurrentPlugin = m_CurrentPlugin;

// stop an alias from recalling itself indefinitely

if (++m_iExecutionDepth > MAX_EXECUTION_DEPTH)
  {
  m_iExecutionDepth--;
  return eCommandsNestedTooDeeply;
  }

CString strFixedCommand = Command;

// huh?  ASSERT (m_CurrentPlugin == NULL); 

m_CurrentPlugin = NULL;

// look for scripting prefix
if (!m_strScriptPrefix.IsEmpty () &&    // and we *have* a script prefix
    strFixedCommand.Left (m_strScriptPrefix.GetLength ()) == 
                          m_strScriptPrefix)  // and it matches
  {
  CString strCommand = strFixedCommand.Mid (m_strScriptPrefix.GetLength ());

  // if scripting enabled, do it
  if (m_bEnableScripts)
    {
    m_bInSendToScript = false;   // they can do DeleteLines here I think

    if (m_ScriptEngine)      // scripting might be enabled, but not OK to run
      m_ScriptEngine->Parse (strCommand, "Command line");
    else
      ColourNote ("white", "red", 
          Translate ("Scripting is not active yet, or script file had a parse error."));

    m_bInSendToScript = true;
    
    m_iExecutionDepth--;
    m_CurrentPlugin = pCurrentPlugin;  // restore whatever plugin we are in
    return eOK;
    }

  // warn if they seem to want to script but it is not enabled

  strCommand.TrimLeft ();

  if (strCommand.Left (6).CompareNoCase ("world.") == 0 ||
      strCommand.Left (8).CompareNoCase ("$world->") == 0)
   ColourNote ("white", "darkblue", 
    Translate ("Warning - you appear to be doing a script command but scripting is not enabled."));

  }      // end of having a scripting prefix


// do command stacking first, so that other things (like aliases) work within command-stack

// ---------------------- COMMAND STACKING ------------------------------

if (m_enable_command_stack && 
 !m_strCommandStackCharacter.IsEmpty ())
   {
   // new in version 3.74 - command stack character at start of line disables command stacking
   if (!strFixedCommand.IsEmpty () &&
       strFixedCommand [0] == m_strCommandStackCharacter [0])
     strFixedCommand.Delete (0, 1);   // delete command stack character itself
   else
     {
     // still want command stacking
     CString strTwoStacks =  m_strCommandStackCharacter [0];
     strTwoStacks +=  m_strCommandStackCharacter [0];
     // convert two command stacks in a row to 0x01  (eg. ;;)
     strFixedCommand = ::Replace (strFixedCommand, strTwoStacks, "\x01");
     // convert any remaining command stacks to a newline
     strFixedCommand.Replace (CString (m_strCommandStackCharacter [0]), ENDLINE);
     // replace any 0x01 with one command stack character
     strFixedCommand.Replace ('\x01', m_strCommandStackCharacter [0]);
     }
   }

CStringList strList;

// change \r\n to \n
strFixedCommand.Replace (ENDLINE, "\n");

// break up command into a list, terminated by newlines
StringToList (strFixedCommand, "\n", strList);

// if list is empty, make sure we send at least one empty line
if (strList.IsEmpty ())
  strList.AddTail (""); 

for (POSITION command_pos = strList.GetHeadPosition (); command_pos; )
  {
  CString str = strList.GetNext (command_pos);


  CString strOriginalMessage = str;

  //  TRACE1 ("SendCommand = %s\n", str);

  bool bOmitFromLog = false;

  if (!m_bPluginProcessingCommand)
      {
      m_bPluginProcessingCommand = true;  // so we don't go into a loop
      if (!SendToAllPluginCallbacks (ON_PLUGIN_COMMAND, str))
        {
        m_bPluginProcessingCommand = false;
        continue;
        }

      m_bPluginProcessingCommand = false;
      }

  // empty line - just send it

    if (str.IsEmpty ())
      {
      // pressing <enter> might be trying to connect
      if (CheckConnected ())
        {
        m_iExecutionDepth--;
        m_CurrentPlugin = pCurrentPlugin;  // restore whatever plugin we are in
        return eWorldClosed;
        }

      SendMsg (ENDLINE, m_display_my_input, false, LoggingInput ()); 
      
      continue;

      }
    
  if (EvaluateCommand (str, true, bOmitFromLog))
    break;    // error (eg. connection not open, don't keep at it)
 
  }   // end of processing each line individually

// this command has completed, so we can reduce the execution depth

  m_iExecutionDepth--;
  m_CurrentPlugin = pCurrentPlugin;  // restore whatever plugin we are in
	return eOK;
}   // end of CMUSHclientDoc::Execute


long CMUSHclientDoc::DoCommand(LPCTSTR Command) 
{

int nID = StringToCommandID (Command);

  if (nID == 0)
    return eNoSuchCommand;

  Frame.PostMessage(WM_COMMAND, nID, 0);

	return eOK;
}   // end of  CMUSHclientDoc::DoCommand


VARIANT CMUSHclientDoc::GetInternalCommandsList() 
{

  COleSafeArray sa;   // for list

  long iCount;

  // count them
	for (iCount = 0; CommandIDs [iCount].iCommandID; iCount++)
    {};   // end of counting them

  sa.CreateOneDim (VT_VARIANT, iCount);

// put the addresses into the array
	for (long i = 0; CommandIDs [i].iCommandID; i++)
    {
    COleVariant v (CommandIDs [i].sCommandName);
    sa.PutElement (&i, &v);
    }      // end of looping through each command

	return sa.Detach ();
}   // end of CMUSHclientDoc::GetInternalCommandsList


BOOL CMUSHclientDoc::GetEchoInput() 
{
	return m_display_my_input != 0;
}  // end of CMUSHclientDoc::GetEchoInput

void CMUSHclientDoc::SetEchoInput(BOOL bNewValue) 
{
  m_display_my_input = bNewValue != 0;
}  // end of CMUSHclientDoc::SetEchoInput


// world.GetQueue - returns a variant array which is a list of queued commands

VARIANT CMUSHclientDoc::GetQueue() 
{
  COleSafeArray sa;   // for command list

POSITION pos;
long iCount;

  // put the queued commands into the array
  if (!m_QueuedCommandsList.IsEmpty ())    // cannot create empty dimension
    {

    sa.CreateOneDim (VT_VARIANT, m_QueuedCommandsList.GetCount ());

    for (iCount = 0, pos = m_QueuedCommandsList.GetHeadPosition(); 
        pos; 
        )
      {
      CString strLine = m_QueuedCommandsList.GetNext (pos);

      // the array must be a bloody array of variants, or VBscript kicks up
      CString s = strLine.Mid (1);
      COleVariant v (s);  // drop echo flag
      sa.PutElement (&iCount, &v);
      iCount++;
      }      // end of looping through each command
    } // end of having at least one

	return sa.Detach ();
}   // end of CMUSHclientDoc::GetQueue




void CMUSHclientDoc::SetInputFont(LPCTSTR FontName, short PointSize, short Weight, BOOL Italic)
{
  Italic = Italic != 0; // make boolean

#ifndef _WIN32
  // On non-Windows the "input font" also controls the output font (they share
  // one monospace face in the terminal-style layout).  Forward to the output
  // pane via the wxWidgets callback.
  if (m_fn_set_output_font) {
    std::string face(FontName ? FontName : "");
    bool bold = (Weight >= 600);
    m_fn_set_output_font(face, (int)PointSize, bold, (bool)Italic);
  }
#else
  ChangeInputFont(PointSize, FontName, Weight, DEFAULT_CHARSET, Italic);
#endif
}  // end of CMUSHclientDoc::SetInputFont



// set the height of the command window
long CMUSHclientDoc::SetCommandWindowHeight(short Height) 
{
	
  if (Height < 0)
    return eBadParameter;

  CChildFrame * pFrame = GetChildFrame ();

  // fail if we can't find one
  if (pFrame == NULL)
    return eBadParameter;

	CRect rectInside;
	pFrame->m_wndSplitter.GetClientRect(rectInside);
	rectInside.InflateRect(-9, -9); // allow for borders and splitter bar

  // we have to root around like this, because recalclayout lays out the top
  // view first, and allocates the rest (possibly nothing) to the bottom view.
  // I don't really want this, the important thing is that you can see where
  // you are going to type. Thus I work out how much the maxmimum top view
  // can be.

  int iRoom = rectInside.bottom - rectInside.top - Height 
      + 7;    // 7 pixels for the splitter bar

  int cyCurTop = iRoom;

  if (cyCurTop < 20)
    return eBadParameter;  // too small, want to see a line at least

  // set the info for the top view
  pFrame->m_wndSplitter.SetRowInfo (OUTPUT_PANE, cyCurTop, 20);

  // set the info for the bottom view
  pFrame->m_wndSplitter.SetRowInfo (COMMAND_PANE, Height, 9);

  // recalculate it all
  pFrame->m_wndSplitter.RecalcLayout ();


	return eOK;
}   // end of CMUSHclientDoc::SetCommandWindowHeight



// add an item for shift+tab completion




long CMUSHclientDoc::SetCommandSelection(long First, long Last)
{
#ifndef _WIN32
  if (m_fn_set_command_selection)
    m_fn_set_command_selection(First, Last);
  return eOK;
#else
  for(POSITION pos = GetFirstViewPosition(); pos != NULL; )
	  {
	  CView* pView = GetNextView(pos);

	  if (pView->IsKindOf(RUNTIME_CLASS(CSendView)))
  	  {
		  CSendView* pmyView = (CSendView*)pView;
      pmyView->GetEditCtrl().SetSel(First - 1, Last, FALSE);

	    }	  // end of being a CSendView
    }   // end of loop through views

	return eOK;
#endif
}  // end of CMUSHclientDoc::SetCommandSelection


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

  {
    std::vector<std::string> hist = GetCommandListEntries(Count);
    if (!hist.empty()) {
      long n = (long)hist.size();
      sa.CreateOneDim(VT_VARIANT, n);
      for (long i = 0; i < n; ++i) {
        COleVariant v(CString(hist[i].c_str()));
        sa.PutElement(&i, &v);
      }
    }
  }

	return sa.Detach ();
}    // end of CMUSHclientDoc::GetCommandList

// world.PushCommand - pushes the command in the command window onto the command
//                     stack and then blanks it out
//  returns command that was pushed

BSTR CMUSHclientDoc::PushCommand()
{
	CString strCommand;

  {
    std::string s = PushCommandText();
    strCommand = s.c_str();
  }

	return strCommand.AllocSysString();
}   // end of CMUSHclientDoc::PushCommand

// world.SelectCommand - selects the command in the command window

void CMUSHclientDoc::SelectCommand()
{
  SelectCommandText();
}    // end of CMUSHclientDoc::SelectCommand

// world.GetCommand - gets the command in the command window

BSTR CMUSHclientDoc::GetCommand()
{
	CString strCommand;

  // the command input bar is owned by the world view; read its text through
  // the seam (empty if there is no view — strCommand starts empty)
  strCommand = GetCommandText().c_str();

	return strCommand.AllocSysString();
}   // end of CMUSHclientDoc::GetCommand



long CMUSHclientDoc::SetCommand(LPCTSTR Message)
{
  // delegate to the world view's input bar; the seam enforces the
  // "only if the bar is empty" contract (false = not empty)
  {
    std::string text(Message ? Message : "");
    if (!m_pWorldView)
      return eOK;   // no view attached (headless) — silently succeed
    return SetCommandText(text) ? eOK : eCommandNotEmpty;
  }
}     // end of CMUSHclientDoc::SetCommand

BSTR CMUSHclientDoc::PasteCommand(LPCTSTR Text)
{
CString strResult;

#ifndef _WIN32
  // Windows is CEdit::ReplaceSel(Text, TRUE): insert at the caret, replacing
  // any selection, and return whatever was selected.  The port used to append
  // via SetCommandText instead -- but setCommand REFUSES a non-empty bar (it
  // implements SetCommand's "only if empty" contract, wx_world_child.cpp:4137)
  // and its result was discarded, so PasteCommand did nothing at all whenever
  // the user had typed anything, and always returned "".
  //
  // ReplaceCommandSelectionRange is the same splice: one undoable
  // wxTextCtrl::Replace.  Offsets are UTF-8 bytes on both sides, matching
  // GetCommandText.  It changes the control's text, so the frame's wxEVT_TEXT
  // binding fires ON_PLUGIN_COMMAND_CHANGED for us -- notifying explicitly here
  // as the Windows arm does would double-fire.
  if (HaveCommandSink() && m_pWorldView) {
    long nStart = 0, nEnd = 0;
    GetCommandSelection(nStart, nEnd);

    const std::string current = GetCommandText();
    if (nEnd > nStart && nStart >= 0 && (size_t) nEnd <= current.size())
      strResult = current.substr(nStart, nEnd - nStart).c_str();

    ReplaceCommandSelectionRange(nStart, nEnd, Text ? Text : "");
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
  DeleteCommandHistoryEntries();
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

  // One body on both platforms.  The port used to fork here and drive
  // EmitOutputFont instead, on the premise that "the input font also controls
  // the output font (they share one monospace face)" -- which was not true: the
  // command bar has its own font, its own preference and its own live seam
  // (CWorldChildFrame::setInputFont), and ChangeInputFont already reaches it
  // through EmitInputFont.  The fork left world.SetInputFont restyling the MUD
  // output, never touching the command bar, and -- because the port's
  // setOutputFont also writes the doc's output-font members -- persisting an
  // output font the user never chose.
  ChangeInputFont(PointSize, FontName, Weight, DEFAULT_CHARSET, Italic);
}  // end of CMUSHclientDoc::SetInputFont



// set the height of the command window
long CMUSHclientDoc::SetCommandWindowHeight(short Height) 
{
	
  if (Height < 0)
    return eBadParameter;

  // Everything below the parameter check used to be MFC splitter work reached
  // through GetChildFrame(), which walks the doc's views for a CSendView.  The
  // port constructs no such view, so GetChildFrame() returned NULL and this
  // method was a guaranteed eBadParameter that moved nothing -- one of the
  // dead-reader settings this port keeps finding.
  //
  // The splitter dance is now the WINDOWS ARM of
  // pal::IWorldView::setCommandWindowHeight (pal/win_worldview.cpp), verbatim:
  // the GetChildFrame() lookup, the inflated client rect, the +7 splitter-bar
  // allowance, the "top view under 20 pixels" refusal, both SetRowInfo calls
  // and RecalcLayout.  Its two refusal exits return false, which is turned
  // back into the same eBadParameter here.  The wx arm pins the command bar
  // instead (there is no splitter to drive), and refuses a height too short to
  // show the line being typed -- it has no sash to drag back.
  if (!EmitCommandWindowHeight (Height))
    return eBadParameter;

	return eOK;
}   // end of CMUSHclientDoc::SetCommandWindowHeight



// add an item for shift+tab completion




long CMUSHclientDoc::SetCommandSelection(long First, long Last)
{
  SetCommandSelectionRange(First, Last);
  return eOK;
}  // end of CMUSHclientDoc::SetCommandSelection


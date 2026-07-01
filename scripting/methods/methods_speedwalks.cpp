// methods_speedwalks.cpp

// Relating to speedwalking and queued commands in general


#include "stdafx.h"
#include "../../MUSHclient.h"
#include "../../doc.h"
#include "../../mainfrm.h"
#include "../errors.h"
#include "../../speedwalk.h"   // shared ExpandSpeedwalk / ReverseSpeedwalkString

// Implements:

//    DiscardQueue
//    EvaluateSpeedwalk
//    Queue
//    RemoveBacktracks
//    ReverseSpeedwalk


// rewritten 17th August 2002 to make it easier to follow and less buggy :)
//  (and support comments)

// MakeSpeedWalkErrorString, DoEvaluateSpeedwalk and DoReverseSpeedwalk now live
// in the shared speedwalk.cpp (compiled into both the GUI and the portable
// libraries) so the parsing logic cannot drift.  These thin wrappers inject the
// MapDirectionsMap-backed direction lookups that the GUI build uses.

static CString SpeedwalkForwardDir (char letter)
  {
  return CString (MapDirectionsMap [string (1, letter)].m_sDirectionToSend.c_str ());
  }

static CString SpeedwalkReverseDir (char letter)
  {
  return CString (MapDirectionsMap [string (1, letter)].m_sReverseDirection.c_str ());
  }

static CString SpeedwalkReverseWord (const CString & word)
  {
  MapDirectionsIterator i = MapDirectionsMap.find ((LPCTSTR) word);
  if (i != MapDirectionsMap.end ())
    return CString (i->second.m_sReverseDirection.c_str ());
  return CString ();
  }

CString CMUSHclientDoc::DoEvaluateSpeedwalk(LPCTSTR SpeedWalkString)
{
  return ExpandSpeedwalk (SpeedWalkString, m_strSpeedWalkFiller, ENDLINE,
                          SpeedwalkForwardDir);
}   // end of CMUSHclientDoc::DoEvaluateSpeedwalk


BSTR CMUSHclientDoc::EvaluateSpeedwalk(LPCTSTR SpeedWalkString) 
  {
CString strResult = DoEvaluateSpeedwalk (SpeedWalkString);
	return strResult.AllocSysString();
  }   // end of CMUSHclientDoc::EvaluateSpeedwalk

CString CMUSHclientDoc::DoReverseSpeedwalk(LPCTSTR SpeedWalkString)
{
  return ReverseSpeedwalkString (SpeedWalkString, ENDLINE,
                                 SpeedwalkReverseDir, SpeedwalkReverseWord);
}   // end of CMUSHclientDoc::DoReverseSpeedwalk

BSTR CMUSHclientDoc::ReverseSpeedwalk(LPCTSTR SpeedWalkString) 
  {
	CString strResult = DoReverseSpeedwalk (SpeedWalkString);
	return strResult.AllocSysString();
  }   // end of CMUSHclientDoc::ReverseSpeedwalk


long CMUSHclientDoc::Queue(LPCTSTR Message, BOOL Echo) 
{
  if (m_iConnectPhase != eConnectConnectedToMud)
    return eWorldClosed;             

  // cannot change what we are sending in OnPluginSent
  if (m_bPluginProcessingSent)
    return eItemInUse;

  SendMsg (Message, Echo != 0, true, false);
	return eOK;
}   // end of CMUSHclientDoc::Queue

long CMUSHclientDoc::DiscardQueue() 
{
long iCount = m_QueuedCommandsList.GetCount ();
     
  m_QueuedCommandsList.RemoveAll ();	
  ShowQueuedCommands ();    // update status line

	return iCount;
}   // end of CMUSHclientDoc::DiscardQueue

short CMUSHclientDoc::GetSpeedWalkDelay() 
{
	return m_iSpeedWalkDelay;
}    // end of CMUSHclientDoc::GetSpeedWalkDelay

void CMUSHclientDoc::SetSpeedWalkDelay(short nNewValue) 
{
  if (m_pTimerWnd)
    m_pTimerWnd->ChangeTimerRate (nNewValue);
  m_iSpeedWalkDelay = nNewValue;
}   // end of CMUSHclientDoc::SetSpeedWalkDelay


void CMUSHclientDoc::OnInputDiscardqueuedcommands() 
{
  m_QueuedCommandsList.RemoveAll ();	
  ShowQueuedCommands ();    // update status line
	
}  // end of CMUSHclientDoc::OnInputDiscardqueuedcommands

void CMUSHclientDoc::OnUpdateInputDiscardqueuedcommands(CCmdUI* pCmdUI) 
{
  DoFixMenus (pCmdUI);  // remove accelerators from menus
  pCmdUI->SetText (TFormat ("&Discard %i Queued Command%s\tCtrl+D",
                    PLURAL (m_QueuedCommandsList.GetCount ())));
  	
  pCmdUI->Enable (!m_QueuedCommandsList.IsEmpty ());
	
}   // end of CMUSHclientDoc::OnUpdateInputDiscardqueuedcommands


void CMUSHclientDoc::ShowQueuedCommands (void)
  {

  // show previous status line
  if (m_QueuedCommandsList.IsEmpty ())
    {
    ShowStatusLine (true);    // show it now
    return;    
    }

  CString strQueued = "Queued: ";
  const int MAX_SHOWN = 50;

  CString str;
  CString strLastDir;

  int iCount = 0;
  POSITION pos;

  for (pos = m_QueuedCommandsList.GetHeadPosition (); 
      pos && strQueued.GetLength () < MAX_SHOWN; )
    {
    // get next direction from list
    str = m_QueuedCommandsList.GetNext (pos).Mid (1);
    
    // empty lines look a bit silly
    if (str.IsEmpty ())
      continue;

    // if same as before, count them
    if (str == strLastDir)
      iCount++;
    else
      {
      // add to string
      // if direction is not a single char (eg. ne) then we must put it in brackets
      if (strLastDir.GetLength () > 1)
        strLastDir = "(" + strLastDir + ")";
      if (iCount == 1)
        strQueued += strLastDir + " ";
      else if (iCount > 1)
        strQueued += CFormat ("%i%s ", iCount, (LPCTSTR) strLastDir);

      strLastDir = str;
      iCount = 1;
      }   // end of a direction change                                

    }   // end of processing each command

  // output final command
  // if direction is not a single char (eg. ne) then we must put it in brackets
  if (strLastDir.GetLength () > 1)
    strLastDir = "(" + strLastDir + ")";
  if (iCount == 1)
    strQueued += strLastDir + " ";
  else if (iCount > 1)
    strQueued += CFormat ("%i%s ", iCount, (LPCTSTR) strLastDir);

  if (pos)
    strQueued += " ...";

  Frame.SetStatusMessageNow (strQueued);
  m_tStatusDisplayed = CTime::GetCurrentTime ();
  }    // end of CMUSHclientDoc::ShowQueuedCommands


// remove backtracks from a speedwalk string
BSTR CMUSHclientDoc::RemoveBacktracks(LPCTSTR Path) 
{
	CString strResult;

  // first convert to individual items separated by newlines

  CString strWalk = DoEvaluateSpeedwalk (Path);

  // if string empty, or has an error message, just return it
  if (strWalk.IsEmpty () || strWalk.Left (1) == "*")
	  return strWalk.AllocSysString();

  // convert speedwalk string into a nice vector we can walk
  vector<string> w;   // input

  StringToVector ((LPCTSTR) strWalk, w, ENDLINE);

  // empty? ah well, can't be many backtracks to remove
  if (w.empty ())
	  return strResult.AllocSysString();

  // we will push each item onto our queue (stack) and look for
  // backtracks on the top of it

  deque<string> q;    // output
  MapDirectionsIterator it;    // for looking up reverses

  for (vector<string>::const_iterator wi = w.begin ();
       wi != w.end ();
       wi++)
     {
     string sThisDirection = *wi;
     // convert back to a single character if possible
     it = MapDirectionsMap.find (tolower (sThisDirection));
     if (it != MapDirectionsMap.end ())
       sThisDirection = it->second.m_sDirectionToLog;

     // if output stack is empty we can hardly remove a backtrack
     if (q.empty ())
       q.push_back (sThisDirection); // so just add this one
     else
       {
       string sTop = q.back ();
       it = MapDirectionsMap.find (sTop);
       // if new entry is inverse of top of stack, discard both
       if (it != MapDirectionsMap.end () && 
           it->second.m_sReverseDirection == sThisDirection)
         q.pop_back ();
       else
         q.push_back (sThisDirection);
       }  // end of stack not empty

     }  // end of looking at each direction in the input string

  // empty? must have cleaned everything up
  if (q.empty ())
	  return strResult.AllocSysString();

  string sPrev;
  int iCount = 0;
  string sDirection;

  for (deque<string>::const_iterator di = q.begin ();
      di != q.end ();
      di++)
    {
    sDirection = trim (*di);

    if (sDirection.empty ())
      continue;

//  multiple length movements must be put in brackets
//   (eg. up, down, ne, nw)

    if (sDirection.size () > 1)
      sDirection = "(" + sDirection + ")";

    if (sDirection == sPrev && iCount < 99)
      iCount++;
    else
      {
      // output previous speedwalk
      if (!sPrev.empty ())
        {
        if (iCount > 1)
          strResult += CFormat ("%i%s", iCount, sPrev.c_str ());
        else
          strResult += sPrev.c_str ();
        strResult += " ";
        }
      sPrev = sDirection;
      iCount = 1;
      } // end of not multiple identical speedwalks

    } // end of pulling entries out of new queue

  // output final speedwalk
  if (!sPrev.empty ())
    {
    if (iCount > 1)
      strResult += CFormat ("%i%s", iCount, sPrev.c_str ());
    else
      strResult += sPrev.c_str ();
    strResult += " ";
    }

  return strResult.AllocSysString();

  }  // end of CMUSHclientDoc::RemoveBacktracks


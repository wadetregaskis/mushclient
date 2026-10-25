// methods_accelerators.cpp - stuff for the Accelerator command and variants

#include "stdafx.h"
#include "../../MUSHclient.h"
#include "../../doc.h"
#include "../../mushview.h"
#include "../../mainfrm.h"
#include "../../flags.h"
#include "../errors.h"
#include "../../childfrm.h"
#include "../../sendvw.h"

// Implements:

//    Accelerator
//    AcceleratorList
//    AcceleratorTo


// accelerator finding functor

struct fFindAccelerator
  {
  bool operator() (const ACCEL & a1, const ACCEL & a2) const
    {
    return (a1.fVirt | FNOINVERT) == (a2.fVirt | FNOINVERT) &&
           a1.key == a2.key;
    };
  }; // end of fFindAccelerator


long CMUSHclientDoc::AcceleratorTo(LPCTSTR Key, LPCTSTR Send, short SendTo) 
{
BYTE fVirt;
WORD key;

  // check they are sending to somewhere valid

  if (SendTo < 0 || SendTo >= eSendToLast)
    return eOptionOutOfRange;

  try
    {
    StringToKeyCode (Key, fVirt, key);
    }
  catch (CException* e)
    {
    e->Delete ();
    return  eBadParameter;
    } // end of catch

       
  fVirt |= FNOINVERT;   // no inverting of menus

  // ── the Win32 HACCEL table ───────────────────────────────────────────────
  // Windows keeps the live accelerators in an HACCEL owned by the world's MDI
  // child frame, found through the doc's view list.  Off-Windows there is no
  // HACCEL (CopyAcceleratorTable / CreateAcceleratorTable are inert inlines) and
  // dispatch is done instead by CWorldChildFrame::FireAcceleratorIfMatch, which
  // reads m_AcceleratorToCommandMap directly.
  //
  // The view walk therefore has to be Windows-only.  It was not, and the port
  // never constructs CMUSHView, so GetFirstViewPosition() always returned NULL
  // and this function returned eBadParameter before touching the map — which
  // meant world.Accelerator and world.AcceleratorTo could not register ANY key,
  // ever.  The map bookkeeping below is the part the port actually needs, and it
  // now runs on both platforms.
  ACCEL aWanted;            // new accelerator
  aWanted.fVirt = fVirt;
  aWanted.key = key;
  aWanted.cmd = 0;

#ifdef _WIN32
	POSITION pos = NULL;
  
  pos = GetFirstViewPosition();
	if (!pos)
		return eBadParameter;
  

  CMDIChildWnd * pMDIframe = NULL;

	// work out what our MDI frame is - that will have the accelerators in it

	CView* pView = GetNextView(pos);
	CWnd * pFrame =  pView->GetParentFrame ();
	pMDIframe = (CMDIChildWnd *) pFrame;

  vector <ACCEL> vAccel;
  HACCEL hAccel;

  hAccel = GetDefaultAccelerator ();

  // none for the document? Try the MDI frame
  if (hAccel == 0 && pMDIframe)
    hAccel = pMDIframe->GetDefaultAccelerator ();

  // try the multi-doc template
  if (hAccel == 0)
      hAccel = App.m_pWorldDocTemplate->m_hAccelTable;

	// find number of them
	int iCount = CopyAcceleratorTable (hAccel, NULL, 10000);
  vAccel.resize (iCount);   // make room, set "size" to iCount

	CopyAcceleratorTable (hAccel, &vAccel [0], iCount);

  // if accelerator already exists, remove it
  vAccel.erase (remove_if (vAccel.begin (), vAccel.end (),
                           [&aWanted](const ACCEL & a) { return fFindAccelerator()(a, aWanted); }),
                vAccel.end ());
#endif // _WIN32

  // new in version 3.75 - if no Send string, simply delete accelerator
  if (Send [0]) 
    {
    // the command we will allocate (eg. 12000, 12001 etc.)
    WORD command;

    // make a flags/keystroke combination                                                      
    long virt_plus_key = ((long) fVirt) << 16 | key;

    // see if we have previously allocated that accelerator
    map<long, WORD>::const_iterator it = m_AcceleratorToCommandMap.find (virt_plus_key);

    if (it == m_AcceleratorToCommandMap.end ())
      {
      // An id released by a previous un-bind comes back first.  Without this,
      // the erase below made every unbind/rebind cycle cost one of the
      // ACCELERATOR_COUNT ids for the life of the world -- a plugin that binds
      // in OnPluginInstall and clears in OnPluginClose, reloaded often enough,
      // eventually got eBadParameter for every key.  Upstream never leaked
      // because it never erased; it also never stopped listing dead keys.
      if (!m_freeAcceleratorCommands.empty ())
        {
        command = m_freeAcceleratorCommands.back ();
        m_freeAcceleratorCommands.pop_back ();
        }
      else
        {
        // check not too many ;)
        if (m_nextAcceleratorCommand >= (ACCELERATOR_FIRST_COMMAND + ACCELERATOR_COUNT))
          return eBadParameter;   // too many of them

        // allocate new one
        command = m_nextAcceleratorCommand++;
        }
      }
    else
      command = it->second;  // re-use command

    aWanted.cmd = command;

#ifdef _WIN32
    // put command into accelerator table
    vAccel.push_back (aWanted);
#endif

    // remember what the command was for that keystroke
    m_AcceleratorToCommandMap [virt_plus_key] = command;

    // remember what to send if they use that command
    m_CommandToMacroMap [command] = Send;

    // and where to send it
    m_CommandToSendToMap [command] = SendTo;

    // remember which plugin did it
    m_CommandToPluginMap [command].erase ();

    if (m_CurrentPlugin)
      m_CommandToPluginMap [command] = m_CurrentPlugin->m_strID;

    }  // end of having something to do (ie. not deleting accelerator)

#ifdef _WIN32
  // create new handle
  hAccel = CreateAcceleratorTable (&vAccel [0], vAccel.size ());

  // destroy old one, if we had one
  if (m_accelerator)
    DestroyAcceleratorTable (m_accelerator);

  // replace accelerator table in document
  m_accelerator = hAccel;
#endif // _WIN32

  // Deleting: an empty Send means "forget this key", and that has to mean
  // forgetting the BOOKKEEPING too, not just the dispatch.
  //
  // Upstream only half-deleted.  The HACCEL above is rebuilt without the key,
  // so it genuinely stops firing -- but the three maps are written only inside
  // the `if (Send [0])` arm above and were never erased, so the entries
  // survived their own deletion.  AcceleratorList reads them, and therefore
  // went on reporting `Ctrl+F8 = hello` for a key that could no longer do
  // anything; a script asking "is this key taken?" got the wrong answer.
  // m_CommandToPluginMap likewise kept naming a plugin as the owner of a dead
  // binding, which matters when that plugin is unloaded.
  //
  // Off Windows there is no HACCEL at all -- CWorldChildFrame::FireAccelerator-
  // IfMatch dispatches straight from m_AcceleratorToCommandMap -- so the map IS
  // the table and this erase is the whole deletion.  It ran there already; it
  // now runs on both, which is the only Windows-visible change: a key that was
  // already dead stops being listed.  See docs/UPSTREAM-DIVERGENCES.md.
  if (!Send [0])
    {
    const long virt_plus_key = ((long) fVirt) << 16 | key;
    map<long, WORD>::iterator it = m_AcceleratorToCommandMap.find (virt_plus_key);
    if (it != m_AcceleratorToCommandMap.end ())
      {
      m_CommandToMacroMap.erase (it->second);
      m_CommandToSendToMap.erase (it->second);
      m_CommandToPluginMap.erase (it->second);
      // release the id for re-use before dropping the only record of it
      m_freeAcceleratorCommands.push_back (it->second);
      m_AcceleratorToCommandMap.erase (it);
      }
    }

  // test test test

//  App.m_pWorldDocTemplate->m_hAccelTable = hAccel;

	return eOK;
}  // end of CMUSHclientDoc::AcceleratorTo

long CMUSHclientDoc::Accelerator(LPCTSTR Key, LPCTSTR Send) 
{
  return AcceleratorTo (Key, Send, eSendToExecute);
}   // end of CMUSHclientDoc::Accelerator




// /Accelerator ("F5", "sing")
// /Note (Accelerator ("Alt+Shift+Ctrl+V", "hello"))
// /Note (Accelerator ("Ctrl+F8", "hello"))
// /Note (Accelerator ("Shift+F1", "hello"))

/*

/Note (Accelerator ("Ctrl+Alt+Shift+K", "Ctrl+Alt+Shift+K"))
Note (Accelerator ("Ctrl+K", "Ctrl+K"))
Note (Accelerator ("Shift+Ctrl+K", "Shift+Ctrl+K"))
Note (Accelerator ("Shift+Alt+K", "Shift+Alt+K"))
Note (Accelerator ("Ctrl+Alt+K", "Ctrl+Alt+K"))

table.foreach (AcceleratorList (), print)
*/


VARIANT CMUSHclientDoc::AcceleratorList() 
{
  COleSafeArray sa;   // for wildcard list

  long iCount = m_AcceleratorToCommandMap.size ();

  if (iCount) // cannot create empty array dimension
    {
    sa.CreateOneDim (VT_VARIANT, iCount);
  
    map<long, WORD>::const_iterator it;

    for (iCount = 0, it = m_AcceleratorToCommandMap.begin (); 
         it != m_AcceleratorToCommandMap.end (); it++, iCount++)
      {
      CString key = KeyCodeToString (it->first >> 16, it->first);
      string command = m_CommandToMacroMap [it->second];

      CString strSendTo;
      strSendTo.Format ("\t[%i]", m_CommandToSendToMap [it->second]);

      // for backwards compatability, send to Execute is shown as empty
      if (m_CommandToSendToMap [it->second] == eSendToExecute)
        strSendTo = "";
      // the array must be a bloody array of variants, or VBscript kicks up
      CString s = CFormat ("%s = %s%s", (LPCTSTR) key, 
                          command.c_str (), 
                          (LPCTSTR) strSendTo);
      COleVariant v (s);
      sa.PutElement (&iCount, &v);
      }      // end of looping through each accelerator
    } // end of having at least one

	return sa.Detach ();
}   // end of CMUSHclientDoc::AcceleratorList



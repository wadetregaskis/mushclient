// doc_aliasmenu_shared.cpp
//
// The Ctrl+click alias menu, hoisted out of the view family (2026-08-13) so
// the wx frontend can drive it -- the dead-view-family pattern (tab
// completion, ef85fb0, is the worked example).  Three pieces, all formerly
// CMUSHView-only and therefore dead on the port:
//
//   SetWordUnderMenu      -- the word under the mouse, for GetInfo(86)
//                            (CMUSHView::AliasMenu's word scan, verbatim)
//   BuildAliasMenuItems   -- the sorted label[:pluginid] list of every
//                            enabled, labelled alias flagged "menu"
//                            (AliasMenu's two map loops + CompareMenu)
//   ExecuteAliasMenuItem  -- run the chosen alias: %-substitution, script
//                            callback, SendTo (OnMXPMenu's ACTION_ALIAS arm,
//                            pDoc-> dropped, the item argument replacing the
//                            strMXP_menu_item subscript)
//
// Windows' CMUSHView keeps its CMenu/TrackPopupMenu UI and calls these; the
// port pops a wxMenu through CWorldChildFrame.  One body, both platforms.
// (docs/WINDOWS-VALIDATION.md records the needed Windows run.)
//
// docs/UPSTREAM-BUGS.md #14: the original ACTION_ALIAS arm computed the %1
// subscript and then never used it, so a menu alias's own %1 always sent
// empty -- the ONE substitution GetInfo(86) exists to make available.  Fixed
// here, so it is fixed on Windows too now that mushview.cpp calls this body.

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "OtherTypes.h"

#include <algorithm>   // std::sort -- see AliasMenuLess

// was CompareMenu (mushview.cpp): sort by label, ignoring the plugin id.
// std::sort, NOT the original qsort: qsort swaps elements by raw bytes, which
// is legal on MFC's pointer-sized CString but corrupts the port's shim (a
// std::string + scratch-buffer aggregate).  The view's copy never ran here,
// so the hazard was never exercised until the hoist made it live.  Same
// ordering (case-insensitive by label); both sorts are unstable.
static bool AliasMenuLess (const CString & a, const CString & b)
  {
  CString string1 = a;
  CString string2 = b;

  // strip off plugin IDs
  CStringList strList;

  StringToList (string1, ":", strList);
  string1 = strList.GetHead ();

  StringToList (string2, ":", strList);
  string2 = strList.GetHead ();

  return string1.CompareNoCase (string2) < 0;
  }   // end of AliasMenuLess

// The word under (pLine, col) -- col is a BYTE offset into pLine->text --
// stored in m_strWordUnderMenu for GetInfo(86).
void CMUSHclientDoc::SetWordUnderMenu (CLine * pLine, int col)
  {
  if (!pLine || pLine->len <= 0)
    {
    m_strWordUnderMenu.Empty ();
    return;
    }

  int start_col, end_col;
  if (col >= pLine->len)
    col = pLine->len - 1;
  if (col < 0)
    col = 0;
  start_col = end_col = col;

  while (start_col >= 0 &&
        !isspace ((unsigned char) pLine->text [start_col]) &&
        strchr (App.m_strWordDelimitersDblClick, pLine->text [start_col]) == NULL)
    start_col--;
  start_col++;   // now onto the start of that word

  // a word will end on a space, or whatever
  while (end_col < pLine->len &&
        !isspace ((unsigned char) pLine->text [end_col]) &&
        strchr (App.m_strWordDelimitersDblClick, pLine->text [end_col]) == NULL)
    end_col++;

  if (end_col > start_col)
      m_strWordUnderMenu = CString (&pLine->text [start_col],
                                          end_col - start_col);
  else
      m_strWordUnderMenu.Empty ();
  }  // end of CMUSHclientDoc::SetWordUnderMenu

// Fill `items` (capacity maxItems) with label[:pluginid] for every enabled,
// labelled alias flagged "menu", world first then plugins, sorted.  Returns
// the count.  The strings keep their original case; menu display and the
// lower-cased map lookup are the callers' business, as they were in the view.
int CMUSHclientDoc::BuildAliasMenuItems (CString * items, int maxItems)
  {
  int i = 0;

  CAlias * pAlias;
  CString strAliasName;
  POSITION pos;

  for (pos = m_AliasMap.GetStartPosition();
       pos && i < maxItems;
       )
     {
     m_AliasMap.GetNextAssoc (pos, strAliasName, pAlias);
     if (pAlias->strLabel.IsEmpty () ||
        !pAlias->bMenu ||
        !pAlias->bEnabled)
        continue;

     // remember what to send if they click on it
     items [i] = pAlias->strLabel;
     i++;
    } // end of all aliases

  // do plugins
  for (PluginListIterator pit = m_PluginList.begin ();
       pit != m_PluginList.end () && i < maxItems;
       ++pit)
    {
    m_CurrentPlugin = *pit;

    if (m_CurrentPlugin->m_bEnabled)
      for (POSITION apos = GetAliasMap ().GetStartPosition ();
            apos && i < maxItems; )
        {
        GetAliasMap ().GetNextAssoc (apos, strAliasName, pAlias);

         if (pAlias->strLabel.IsEmpty () ||
            !pAlias->bMenu ||
            !pAlias->bEnabled)
            continue;

         // remember what to send if they click on it
         items [i] = pAlias->strLabel;
         items [i] += ":";
         items [i] += m_CurrentPlugin->m_strID;  // need to know which plugin
         i++;

        }  // end of scanning plugin aliases
    } // end of doing plugins list
  m_CurrentPlugin = NULL;

  if (i > 1)
    std::sort (items, items + i, AliasMenuLess);

  return i;
  }  // end of CMUSHclientDoc::BuildAliasMenuItems

// Execute a chosen menu item ("label" or "label:pluginid", LOWER-cased for
// the map lookup, as the view stored them).  Verbatim from OnMXPMenu's
// ACTION_ALIAS arm.
void CMUSHclientDoc::ExecuteAliasMenuItem (const CString & strItem)
  {

      CAlias * pAlias;
      CString strLabel;
      
      // strip off plugin id
      CStringList strList;
      StringToList (strItem, ":", strList);
      strLabel = strList.GetHead ();

      // alias might be in a plugin
      switch (strList.GetCount ())
        {
        case 1:   // main world
            if (!m_AliasMap.Lookup (strLabel, pAlias))
              return;   // not there? strange
        break;

        case 2:   // plugin
          {
            CString strPluginID;
            strPluginID = strList.GetTail ();
            CPlugin * pPlugin = GetPlugin (strPluginID);
            if (!pPlugin)
              return;   // plugin does not exist? strange
            if (!pPlugin->m_AliasMap.Lookup (strLabel, pAlias))
              return;   // not there? strange
          m_CurrentPlugin = pPlugin;     // remember plugin so alias executes in correct space
          }

        break;
            default:   return;   // not 1 or 2 items? very strange
        } // end of switch


      CString strAction;

//---

        // copy contents to strAction area, substituting variables

        const char * p,
                   * start;
        long length;
        int subscript;

        CString strFixedContents = ::FixupEscapeSequences (pAlias->contents);
        p = start = strFixedContents;

        while (*p)
          {
          if (p [0] == '%' && isdigit (p [1]))
            {

        // copy up to the percent sign

            length = p - start;
    
            if (length > 0)
              strAction += CString (start, length);
                
            subscript = *++p - '0';

          // %1 is the word the menu was invoked on (SetWordUnderMenu /
          // GetInfo(86)) -- a menu click has no matched line to capture
          // wildcards from, so it is the only one this can supply; %0 and
          // %2-%9 stay empty, as they always have.
          if (subscript == 1)
            strAction += m_strWordUnderMenu;

          // get ready for next batch from beyond the digit

            start = ++p;

            } // end of finding a percent sign, followed by a number
          else if (p [0] == '%' && toupper (p [1]) == 'C')
            {
            CString strClipboard;

            if (!GetClipboardContents (strClipboard, m_bUTF_8))
              {
              m_CurrentPlugin = NULL;
              return;
              }

        // copy up to the percent sign

            length = p - start;
          
            if (length > 0)
              strAction += CString (start, length);

            strAction += strClipboard;

  // get ready for next batch from beyond the 'c'

            p += 2;
            start = p;

            }   // end of finding %c
        else if (p [0] == '%' && toupper (p [1]) == '%')
          {
// copy up to the percent sign

         length = p - start;
      
          if (length > 0)
           strAction += CString (start, length);

          // get ready for next batch from beyond the %

          start = ++p;
          p++;    // don't reprocess the %

          }   // end of %%
         else if (pAlias->bExpandVariables && p [0] == '@')
           {
// copy up to the @ sign

         length = p - start;
      
          if (length > 0)
           strAction += CString (start, length);
            
          p++;    // skip the @

          // @@ becomes @
          if (*p == '@')
            {
            start = ++p;
            strAction += "@";
            continue;
            }

          const char * pName = p;

          // find end of variable name
          while (*p)
            if (*p == '_' || isalnum (*p))
              p++;
            else
              break;
          
          CString strName (pName, p - pName);

          if (strName.IsEmpty ())
            {
            ::TMessageBox("@ must be followed by a variable name");
            m_CurrentPlugin = NULL;
            return;
            }

          CVariable * variable_item;

          strName.MakeLower ();
          if (!m_VariableMap.Lookup (strName, variable_item))
            {
            ::UMessageBox(TFormat ("Variable '%s' is not defined.", (LPCTSTR) strName));
            m_CurrentPlugin = NULL;
            return;
            }

          strAction += variable_item->strContents;

          // get ready for next batch from beyond the variable

          start = p;
           }
         else
            p++;
          } // end of processing each character in the replacement text

  // copy last group

        strAction += start;

//--- 


      if (m_ScriptEngine)
        if (pAlias->dispid != DISPID_UNKNOWN)        // if we have a dispatch id
          {


          CString strType = "alias";
          CString strReason =  TFormat ("processing alias \"%s\"", 
                                        (LPCTSTR) pAlias->strLabel);

      if (GetScriptEngine () && GetScriptEngine ()->UsesEmbeddedCallback ())
            {
            list<double> nparams;
            list<string> sparams;
            sparams.push_back ((LPCTSTR) strItem);
            sparams.push_back ((LPCTSTR) pAlias->name);
            GetScriptEngine ()->ExecuteLua (pAlias->dispid, 
                                           pAlias->strProcedure, 
                                           eUserMenuAction,
                                           strType, 
                                           strReason, 
                                           nparams,
                                           sparams, 
                                           pAlias->nInvocationCount); 
            }   // end of Lua
          else
            {

            // execute any scripts associated with alias we found
            enum
              {
              eWildcards,
              eInputLine,
              eAliasName,
              eArgCount,     // this MUST be last
              };    

            COleSafeArray sa;   // for wildcard list
            COleVariant args [eArgCount];
            DISPPARAMS params = { args, NULL, eArgCount, 0 };

            args [eAliasName] = strItem;
            args [eInputLine] = pAlias->name;

            // --------------- set up wildcards array ---------------------------
            sa.Clear ();
            // nb - to be consistent with %1, %2 etc. we will make array 1-relative
            sa.CreateOneDim (VT_VARIANT, MAX_WILDCARDS, NULL, 1);
            for (long i = 1; i <= MAX_WILDCARDS; i++)
              {
              CString s ("");
              COleVariant v (s); // no wildcards are relevant
              sa.PutElement (&i, &v);
              }
            args [eWildcards] = sa;

            ExecuteScript (pAlias->dispid,  
                                 pAlias->strProcedure,
                                 eUserMenuAction,
                                 strType, 
                                 strReason,
                                 params, 
                                 pAlias->nInvocationCount); 
            } // end of not Lua

          }     // end of having a dispatch ID

    // send the message

      CString strExtraOutput;

      m_iCurrentActionSource = eUserMenuAction;

      SendTo (pAlias->iSendTo, 
              strAction, 
              pAlias->bOmitFromOutput,
              pAlias->bOmitFromLog,
              TFormat ("Alias: %s", (LPCTSTR) pAlias->strLabel),
              pAlias->strVariable,
              strExtraOutput
              );

      m_iCurrentActionSource = eUnknownActionSource;

      if (!strExtraOutput.IsEmpty ())
         DisplayMsg (strExtraOutput, strExtraOutput.GetLength (), COMMENT);

      m_tLastPlayerInput = CTime::GetCurrentTime();   // for <afk> tests

      pAlias->nMatched++;   // count alias matches

      m_CurrentPlugin = NULL;     // no plugin active right now
  }  // end of CMUSHclientDoc::ExecuteAliasMenuItem

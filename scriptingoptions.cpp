// scriptingoptions.cpp - turn world options on/off from a script

#if defined(_WIN32) || defined(MUSHCLIENT_FULL_BUILD)
#include "stdafx.h"
#include "MUSHclient.h"

#include "doc.h"
#include "MUSHview.h"
#include "scripting/errors.h"
#else
// mushcore (headless test engine): the stub CMUSHclientDoc carries every
// member the option tables name with doc.h-exact types, so the offsetof-based
// machinery below works unchanged (Campaign B option convergence).
#include "platform/doc_stub.h"   // includes scripting/errors.h
#include "OtherTypes.h"
#include <stddef.h>              // offsetof
#include <climits>               // USHRT_MAX etc. (ranges in the .inc tables)
#include <ctype.h>               // isprint/isspace/isxdigit
#endif

#define NL "\r\n"

// Warning - when adding options check the SetOption function at the bottom of this
// file to make sure that some extra action is not needed if the option is changed
// (eg. changing the number of lines in the output window).


/*
 deprecated options not saved any more ...

  m_page_colour        
  m_whisper_colour     
  m_mail_colour        
  m_game_colour        
  m_chat_colour                             
  m_remove_channels1   
  m_remove_channels2   
  m_remove_pages       
  m_remove_whispers    
  m_remove_set         
  m_remove_mail        
  m_remove_game
  m_match_width
  m_timestamps

  */

#define O(arg) offsetof (CMUSHclientDoc, arg), sizeof (((CMUSHclientDoc *)NULL)->arg)

//-----config name ----------------  default ---variable name ---- min, max, flags ---
tConfigurationNumericOption OptionsTable [] = {
#include "scriptingoptions_numeric.inc"
{NULL}   // end of table marker            

  };  // end of OptionsTable 

#define A(arg) offsetof (CMUSHclientDoc, arg)
                    
// first  optional argument: multiline flag
// second optional argument: preserve spaces flag (defaults to true for multiline)

#define MC_OUTPUT_FONT_DEFAULT "FixedSys"
tConfigurationAlphaOption AlphaOptionsTable  [] =
{
#include "scriptingoptions_alpha.inc"
{NULL}  // end of table marker
};

long CMUSHclientDoc::GetOptionIndex (LPCTSTR OptionName) 
  {

int iItem;
int iResult = FindBaseOption (OptionName, OptionsTable, iItem);

  if (iResult != eOK)
    return -1;
  
  return iItem;
  
  }   // end of CMUSHclientDoc::GetOptionIndex 


long CMUSHclientDoc::GetOption(LPCTSTR OptionName) 
{

long i = GetOptionIndex (OptionName);

  if (i == -1)
    return -1;   // not found

  if (m_CurrentPlugin &&
      (OptionsTable [i].iFlags & OPT_PLUGIN_CANNOT_READ))
    return -1;  // not available to plugin

  return GetOptionItem (i);


} // end of CMUSHclientDoc::GetOption

long CMUSHclientDoc::GetAlphaOptionIndex (LPCTSTR OptionName) 
  {

int iItem;
int iResult = FindBaseAlphaOption (OptionName, AlphaOptionsTable, iItem);

  if (iResult != eOK)
    return -1;
  
  return iItem;

  }   // end of CMUSHclientDoc::GetAlphaOptionIndex 


VARIANT CMUSHclientDoc::GetAlphaOption(LPCTSTR OptionName) 
{
	VARIANT vaResult;
	VariantInit(&vaResult);

  vaResult.vt = VT_EMPTY;   

long i = GetAlphaOptionIndex (OptionName);

  if (i == -1)
	  return vaResult;

  if (m_CurrentPlugin &&
      (AlphaOptionsTable [i].iFlags & OPT_PLUGIN_CANNOT_READ))
    return vaResult;  // not available to plugin

  SetUpVariantString (vaResult, GetAlphaOptionItem (i));
  return vaResult;

}

VARIANT CMUSHclientDoc::GetOptionList() 
{
  COleSafeArray sa;   // for list
  long i;

  // count them
  for (i = 0; OptionsTable [i].pName; i++)
    ;

  sa.CreateOneDim (VT_VARIANT, i);

  // put the numeric option names into the array
  for (i = 0; OptionsTable [i].pName; i++)
    {
    // the array must be a bloody array of variants, or VBscript kicks up
    COleVariant v (OptionsTable [i].pName);
    sa.PutElement (&i, &v);
    }      // end of looping through each option

	return sa.Detach ();
}

VARIANT CMUSHclientDoc::GetAlphaOptionList() 
{
  COleSafeArray sa;   // for list
  long i;

  // count them
  for (i = 0; AlphaOptionsTable [i].pName; i++)
    ;

  sa.CreateOneDim (VT_VARIANT, i);

  // put the alpha option names into the array
  for (i = 0; AlphaOptionsTable [i].pName; i++)
    {
    // the array must be a bloody array of variants, or VBscript kicks up
    COleVariant v (AlphaOptionsTable [i].pName);
    sa.PutElement (&i, &v);
    }      // end of looping through each option

	return sa.Detach ();
}

// SetBaseOptionItem: single-sourced in doc_shared.cpp (option-machinery step 1).

long CMUSHclientDoc::SetOptionItem (const int iItem, 
                                    long Value, 
                                    const bool bDoSpecial,
                                    const bool bInclude) 
  {
bool bChanged;

long iResult = SetBaseOptionItem (iItem, 
                                  OptionsTable, 
                                  NUMITEMS (OptionsTable),
                                  (char *) this,
                                  Value,
                                  bChanged);

  if (iResult != eOK)
    return iResult;

  if (bChanged)
    SetModifiedFlag ();

  if (bInclude)
    {
    m_NumericConfiguration [iItem]->bInclude = true;
    m_NumericConfiguration [iItem]->iValue = Value;
    }

  // specials wanted?
  if (!bDoSpecial)
    return eOK;
  
// some special handling

  if (OptionsTable [iItem].iFlags & OPT_FIX_OUTPUT_BUFFER)
    if (m_pCurrentLine)     // a new world might not have a line yet
      FixUpOutputBuffer (Value);

  if (OptionsTable [iItem].iFlags & OPT_FIX_INPUT_WRAP)
	  FixInputWrap();

  if (OptionsTable [iItem].iFlags & OPT_FIX_WRAP_COLUMN)
    {
    if (m_pCurrentLine)     // a new world might not have a line yet
      {
      // save current line text
      CString strLine = CString (m_pCurrentLine->text, m_pCurrentLine->len);

#ifdef USE_REALLOC
      m_pCurrentLine->text  = (char *) 
            realloc (m_pCurrentLine->text, 
                     MAX (m_pCurrentLine->len, Value) 
                     * sizeof (char));  
#else
      delete [] m_pCurrentLine->text;
      m_pCurrentLine->text = new char [MAX (m_pCurrentLine->len, Value)];
#endif

      // put text back
      memcpy (m_pCurrentLine->text, (LPCTSTR) strLine, m_pCurrentLine->len);
      ASSERT (m_pCurrentLine->text);
      }   // end of having a current line
    SendWindowSizes (Value);
    }

  if (OptionsTable [iItem].iFlags & OPT_FIX_SPEEDWALK_DELAY)
    SetSpeedWalkDelay ((short) Value);

  POSITION pos;

  if (OptionsTable [iItem].iFlags & OPT_FIX_TOOLTIP_VISIBLE)
    {
    for(pos = GetFirstViewPosition(); pos != NULL;)
    	{
    	CView* pView = GetNextView(pos);
    	if (pView->IsKindOf(RUNTIME_CLASS(CMUSHView)))
      	{
    		CMUSHView* pmyView = (CMUSHView*)pView;
        if (pmyView->m_ToolTip.m_hWnd)
    		  pmyView->m_ToolTip.SendMessage(TTM_SETDELAYTIME, TTDT_AUTOPOP, Value - 1);  // zero will be default  (-1)
    	  }	  // end if
      }  // end for
    } // end of OPT_FIX_TOOLTIP_VISIBLE

  if (OptionsTable [iItem].iFlags & OPT_FIX_TOOLTIP_START)
    {
    for(pos = GetFirstViewPosition (); pos != NULL;)
    	{
    	CView* pView = GetNextView(pos);
    	if (pView->IsKindOf(RUNTIME_CLASS(CMUSHView)))
      	{
    		CMUSHView* pmyView = (CMUSHView*)pView;
        if (pmyView->m_ToolTip.m_hWnd)
      		pmyView->m_ToolTip.SendMessage(TTM_SETDELAYTIME, TTDT_INITIAL, Value - 1);  // zero will be default  (-1)
    	  }	  // end if
      }  // end for
    } // end of OPT_FIX_TOOLTIP_START

  if (OptionsTable [iItem].iFlags & OPT_USE_MXP)
    {
    if (m_iUseMXP == eNoMXP && m_bMXP)
      MXP_Off (true);
    else if (m_iUseMXP == eUseMXP && !m_bMXP)
      MXP_On (false, true);   // not pueblo, manually on
    }

  if (OptionsTable [iItem].iFlags & OPT_UPDATE_INPUT_FONT)
    ChangeInputFont (m_input_font_height, 
                     m_input_font_name, 
                     m_input_font_weight, 
                     m_input_font_charset,
                     m_input_font_italic);

  if (OptionsTable [iItem].iFlags & OPT_UPDATE_OUTPUT_FONT)
    ChangeFont (m_font_height, 
                m_font_name, 
                m_font_weight, 
                m_font_charset,
                m_bShowBold,
                m_bShowItalic,
                m_bShowUnderline,
                m_iLineSpacing);

  if (OptionsTable [iItem].iFlags & OPT_UPDATE_VIEWS)
    UpdateAllViews (NULL);  


  return eOK;

  } // end of CMUSHclientDoc::SetOptionItem

// basic OptionItem retriever - used for worlds, triggers, aliases etc.

// GetBaseOptionItem: single-sourced in doc_shared.cpp (option-machinery step 1).

long CMUSHclientDoc::GetOptionItem (const int iItem)
  {
  return GetBaseOptionItem (iItem, 
                            OptionsTable, 
                            NUMITEMS (OptionsTable),
                            (char *) this);  
  }


void CMUSHclientDoc::SetDefaults (const bool bDoSpecial)
  {
  for (int i = 0; OptionsTable [i].pName; i++)
      SetOptionItem (i, OptionsTable [i].iDefault, bDoSpecial, false);
  }   // end of CMUSHclientDoc::SetDefaults

// FindBaseOption: single-sourced in doc_shared.cpp (option-machinery step 1).

// FindBaseAlphaOption: single-sourced in doc_shared.cpp (option-machinery step 1).


long CMUSHclientDoc::SetOption(LPCTSTR OptionName, 
                               long Value) 
{

int iItem;
int iResult = FindBaseOption (OptionName, OptionsTable, iItem);

  if (iResult != eOK)
    return iResult;

  if (m_CurrentPlugin &&
      (OptionsTable [iItem].iFlags & OPT_PLUGIN_CANNOT_WRITE))
    return ePluginCannotSetOption;

  return SetOptionItem (iItem, Value, true, false);

} // end of CMUSHclientDoc::SetOption

// SetBaseAlphaOptionItem: single-sourced in doc_shared.cpp (option-machinery step 1).

long CMUSHclientDoc::SetAlphaOptionItem (const int iItem, 
                                        LPCTSTR sValue, 
                                        const bool bDoSpecial,
                                        const bool bInclude)
  {


  CString strValue = sValue;

  if (AlphaOptionsTable [iItem].iFlags & OPT_COMMAND_STACK)
    {

    if (strValue.GetLength () > 1)
      return eOptionOutOfRange;

      if (strValue.IsEmpty ())
        {
        m_enable_command_stack = false;
        return eOptionOutOfRange;
        }

      if (!isprint (strValue [0]) || isspace (strValue [0]))
        {
        m_enable_command_stack = false;
        return eOptionOutOfRange;
        }

    } // end of command stack character

  if (AlphaOptionsTable [iItem].iFlags & OPT_WORLD_ID)
    {

    if (!strValue.IsEmpty ())
      if (strValue.GetLength () != PLUGIN_UNIQUE_ID_LENGTH)
        return eOptionOutOfRange;

    // make sure hex characters
    const char * p = strValue;
    for ( ; *p ; p++)
      if (!isxdigit (*p))
      return eOptionOutOfRange;

    strValue.MakeLower ();

    } // end of world id
  
bool bChanged;
long iResult = SetBaseAlphaOptionItem (iItem,
                        AlphaOptionsTable,
                        NUMITEMS (AlphaOptionsTable),
                        (char *) this,
                        strValue,
                        bChanged);

  if (iResult != eOK)
    return iResult;

  if (bChanged)
    SetModifiedFlag ();

  if (bInclude)
    {
    m_AlphaConfiguration [iItem]->bInclude = true;
    m_AlphaConfiguration [iItem]->sValue = strValue;
    }

  // specials wanted?
  if (!bDoSpecial)
    return eOK;


// any special tests here ...

  if (AlphaOptionsTable [iItem].iFlags & OPT_UPDATE_INPUT_FONT)
    ChangeInputFont (m_input_font_height, 
                     m_input_font_name, 
                     m_input_font_weight, 
                     m_input_font_charset,
                     m_input_font_italic);

  if (AlphaOptionsTable [iItem].iFlags & OPT_UPDATE_OUTPUT_FONT)
    ChangeFont (m_font_height, 
                m_font_name, 
                m_font_weight, 
                m_font_charset,
                m_bShowBold,
                m_bShowItalic,
                m_bShowUnderline,
                m_iLineSpacing);

  if (AlphaOptionsTable [iItem].iFlags & OPT_UPDATE_VIEWS)
    UpdateAllViews (NULL);  

  return eOK;

  }  // end of CMUSHclientDoc:SetAlphaOptionItem 
  
// GetBaseAlphaOptionItem: single-sourced in doc_shared.cpp (option-machinery step 1).

CString CMUSHclientDoc::GetAlphaOptionItem (const int iItem)
  {

  return GetBaseAlphaOptionItem (iItem, 
                                 AlphaOptionsTable,
                                 NUMITEMS (AlphaOptionsTable),
                                 (char *) this);

  }  // end of CMUSHclientDoc::GetAlphaOptionItem

long CMUSHclientDoc::SetAlphaOption(LPCTSTR OptionName, LPCTSTR Value) 
  {

int iItem;
int iResult = FindBaseAlphaOption (OptionName, AlphaOptionsTable, iItem);

  if (iResult != eOK)
    return iResult;

  if (m_CurrentPlugin &&
      (AlphaOptionsTable [iItem].iFlags & OPT_PLUGIN_CANNOT_WRITE))
    return ePluginCannotSetOption;

  return SetAlphaOptionItem (iItem, Value, true, false);

} // end of CMUSHclientDoc::SetAlphaOption

void CMUSHclientDoc::SetAlphaDefaults (const bool bDoSpecial)
  {
  for (int i = 0; AlphaOptionsTable [i].pName; i++)
    if (AlphaOptionsTable [i].iFlags & OPT_WORLD_ID)
      SetAlphaOptionItem (i, ::GetUniqueID (), bDoSpecial, false);
    else
      SetAlphaOptionItem (i, AlphaOptionsTable [i].sDefault, bDoSpecial, false);

  }   // end of CMUSHclientDoc::SetAlphaDefaults


void CMUSHclientDoc::AllocateConfigurationArrays (void)
  {
int i; 

  m_NumericConfiguration.SetSize (NUMITEMS (OptionsTable));

  for (i = 0; i < NUMITEMS (OptionsTable); i++)
     m_NumericConfiguration.SetAt (i, new CNumericConfiguration);


  m_AlphaConfiguration.SetSize (NUMITEMS (AlphaOptionsTable));

  for (i = 0; i < NUMITEMS (AlphaOptionsTable); i++)
     m_AlphaConfiguration.SetAt (i, new CAlphaConfiguration);

  }

void CMUSHclientDoc::ResetConfigurationArrays (void)
  {
int i; 

  for (i = 0; i < NUMITEMS (OptionsTable); i++)
     m_NumericConfiguration [i]->Reset ();


  for (i = 0; i < NUMITEMS (AlphaOptionsTable); i++)
     m_AlphaConfiguration [i]->Reset ();

  }

void CMUSHclientDoc::DeleteConfigurationArrays (void)
  {
int i; 

  for (i = 0; i < NUMITEMS (OptionsTable); i++)
     delete m_NumericConfiguration  [i];

  m_NumericConfiguration.RemoveAll ();

  for (i = 0; i < NUMITEMS (AlphaOptionsTable); i++)
     delete m_AlphaConfiguration  [i];

  m_AlphaConfiguration.RemoveAll ();

  }
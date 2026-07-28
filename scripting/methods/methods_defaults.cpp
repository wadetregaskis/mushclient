// methods_defaults.cpp

// Defaults (fonts, triggers, etc.)

#include "stdafx.h"
#include "../../MUSHclient.h"
#include "../../doc.h"
#include "../../mainfrm.h"
#include "../errors.h"

// Implements:

//    GetCurrentValue
//    GetDefaultValue
//    GetLoadedValue


extern tConfigurationNumericOption OptionsTable [];
extern tConfigurationAlphaOption AlphaOptionsTable [];

VARIANT CMUSHclientDoc::GetLoadedValue(LPCTSTR OptionName) 
{
	VARIANT vaResult;
	VariantInit(&vaResult);

  vaResult.vt = VT_NULL;    // default if not found

int i = GetOptionIndex (OptionName);

  if (i != -1)
    {
    if (m_CurrentPlugin &&
        (OptionsTable [i].iFlags & OPT_PLUGIN_CANNOT_READ))
    	return vaResult;  // not available to plugin
    SetUpVariantLong (vaResult, m_NumericConfiguration [i]->iValue);
    }  // end of found
  else
    {
    i = GetAlphaOptionIndex (OptionName);
    if (i != -1)
      {
      if (m_CurrentPlugin &&
          (AlphaOptionsTable [i].iFlags & OPT_PLUGIN_CANNOT_READ))
    	  return vaResult;  // not available to plugin
      SetUpVariantString (vaResult, m_AlphaConfiguration [i]->sValue);
      }    // end of found
    }
	
	return vaResult;
}   // end of CMUSHclientDoc::GetLoadedValue

VARIANT CMUSHclientDoc::GetDefaultValue(LPCTSTR OptionName) 
{
	VARIANT vaResult;
	VariantInit(&vaResult);

  vaResult.vt = VT_NULL;    // default if not found

int i = GetOptionIndex (OptionName);

  if (i != -1)
    {
    if (m_CurrentPlugin &&
        (OptionsTable [i].iFlags & OPT_PLUGIN_CANNOT_READ))
    	return vaResult;  // not available to plugin
    SetUpVariantLong (vaResult, OptionsTable [i].iDefault);
    }  // end of found
  else
    {
    i = GetAlphaOptionIndex (OptionName);
    if (i != -1)
      {
      if (m_CurrentPlugin &&
          (AlphaOptionsTable [i].iFlags & OPT_PLUGIN_CANNOT_READ))
    	  return vaResult;  // not available to plugin
      SetUpVariantString (vaResult, AlphaOptionsTable [i].sDefault);
      }  // end of found
    }

	return vaResult;
}    // end of CMUSHclientDoc::GetDefaultValue

VARIANT CMUSHclientDoc::GetCurrentValue(LPCTSTR OptionName) 
{
	VARIANT vaResult;
	VariantInit(&vaResult);

  vaResult.vt = VT_NULL;    // default if not found

int i = GetOptionIndex (OptionName);

  if (i != -1)
    {
    if (m_CurrentPlugin &&
        (OptionsTable [i].iFlags & OPT_PLUGIN_CANNOT_READ))
    	return vaResult;  // not available to plugin
    SetUpVariantLong (vaResult, GetOptionItem (i));
    }  // end of found
  else
    {
    i = GetAlphaOptionIndex (OptionName);
    if (i != -1)
      {
      if (m_CurrentPlugin &&
          (AlphaOptionsTable [i].iFlags & OPT_PLUGIN_CANNOT_READ))
    	  return vaResult;  // not available to plugin
      SetUpVariantString (vaResult, GetAlphaOptionItem (i));
      }  // end of found
    }

	return vaResult;
}    // end of CMUSHclientDoc::GetCurrentValue


void CMUSHclientDoc::OnFileReloaddefaults() 
{

  CAliasMap dummy_alias;
  CTriggerMap dummy_trigger;
  CTimerMap dummy_timer;

  if (m_bUseDefaultColours && !App.m_strDefaultColoursFile.IsEmpty ())
    Load_Set (COLOUR, App.m_strDefaultColoursFile, &Frame);

  if (m_bUseDefaultTriggers && !App.m_strDefaultTriggersFile.IsEmpty ())
    Load_Set (TRIGGER, App.m_strDefaultTriggersFile, &Frame);

  if (m_bUseDefaultAliases && !App.m_strDefaultAliasesFile.IsEmpty ())
    Load_Set (ALIAS, App.m_strDefaultAliasesFile, &Frame);

  if (m_bUseDefaultTimers && !App.m_strDefaultTimersFile.IsEmpty ())
    Load_Set (TIMER, App.m_strDefaultTimersFile, &Frame);

  if (m_bUseDefaultMacros && !App.m_strDefaultMacrosFile.IsEmpty ())
    Load_Set (MACRO, App.m_strDefaultMacrosFile, &Frame);

  ApplyDefaultFonts ();

  UpdateAllViews (NULL);

}  // end of CMUSHclientDoc::OnFileReloaddefaults

// Copy the global default fonts onto this world, for whichever of input/output
// it follows.  Split out of OnFileReloaddefaults so the global preferences
// dialog can run exactly the same code when it closes: a font chosen there used
// to sit in the App members until the user happened to find File > Reload
// Defaults, which is not a thing anyone should have to know about.
//
// Deliberately unconditional, as this code has always been: do NOT add a
// "skip if the world already has this font" test.  A world's font fields are
// updated by this very function during world open, BEFORE its view exists, so
// the fields matching the defaults is no evidence that anything was ever drawn
// at that size -- an emit to a null view is silently dropped.  Guarding on them
// makes the later, view-attached call decide there is nothing to do, and the
// font change never reaches the screen.  Rebuilding the fonts costs nothing at
// the two points this runs (a dialog being dismissed, or an explicit reload).

void CMUSHclientDoc::ApplyDefaultFonts (void)
  {
  if (m_bUseDefaultInputFont && !App.m_strDefaultInputFont.IsEmpty ())
    {
    m_input_font_height = App.m_iDefaultInputFontHeight;
    m_input_font_name = App.m_strDefaultInputFont;
    m_input_font_italic = App.m_iDefaultInputFontItalic;
    m_input_font_weight = App.m_iDefaultInputFontWeight;
    m_input_font_charset = App.m_iDefaultInputFontCharset;
    ChangeInputFont (m_input_font_height,
                    m_input_font_name,
                    m_input_font_weight,
                    m_input_font_charset,
                    m_input_font_italic);

    }   // end of input font override

  if (m_bUseDefaultOutputFont && !App.m_strDefaultOutputFont.IsEmpty ())
    {
    m_font_height = App.m_iDefaultOutputFontHeight;
    m_font_name = App.m_strDefaultOutputFont;
    m_font_weight = FW_NORMAL;
    m_font_charset = App.m_iDefaultOutputFontCharset;
    ChangeFont (m_font_height,
                m_font_name,
                m_font_weight,
                m_font_charset,
                m_bShowBold,
                m_bShowItalic,
                m_bShowUnderline,
                m_iLineSpacing);
    }   // end of output font override

  }  // end of CMUSHclientDoc::ApplyDefaultFonts

void CMUSHclientDoc::OnUpdateFileReloaddefaults(CCmdUI* pCmdUI) 
{
  DoFixMenus (pCmdUI);  // remove accelerators from menus
  pCmdUI->Enable ( 
     (m_bUseDefaultColours && !App.m_strDefaultColoursFile.IsEmpty ()) ||
     (m_bUseDefaultTriggers && !App.m_strDefaultTriggersFile.IsEmpty ()) ||
     (m_bUseDefaultAliases && !App.m_strDefaultAliasesFile.IsEmpty ()) ||
     (m_bUseDefaultTimers && !App.m_strDefaultTimersFile.IsEmpty ()) ||
     (m_bUseDefaultMacros && !App.m_strDefaultMacrosFile.IsEmpty ()) ||
     (m_bUseDefaultInputFont && !App.m_strDefaultInputFont.IsEmpty ()) ||
     (m_bUseDefaultOutputFont && !App.m_strDefaultOutputFont.IsEmpty ()));
	
}  // end of CMUSHclientDoc::OnUpdateFileReloaddefaults


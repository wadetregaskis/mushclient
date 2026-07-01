// doc_shared.cpp — CMUSHclientDoc methods that are pure logic (no view/GUI
// dependency) and were previously reimplemented twice: in doc.cpp (the GUI
// engine) and doc_portable.cpp (the headless test engine). Compiling the single
// canonical implementation into BOTH mushapp and mushcore means it cannot drift
// between the builds (a real bug source: GetStyleRGB once bolded the wrong half
// of an inverse pair, speedwalk lost its C/O/L/K codes). Mirrors ansi.cpp /
// speedwalk.cpp, which likewise define shared CMUSHclientDoc methods.

#if defined(_WIN32) || defined(MUSHCLIENT_FULL_BUILD)
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/errors.h"   // eOK / eBadParameter (full build; doc_stub.h defines them for mushcore)
#else
#include "platform/doc_stub.h"
#endif

#if defined(OPENSSL_FOUND)
#include <openssl/ssl.h>
#endif

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char BASED_CODE THIS_FILE[] = __FILE__;
#endif

void CMUSHclientDoc::GetStyleRGB (CStyle * pStyle, COLORREF & colour1, COLORREF & colour2) const
  {
unsigned short style;
int iForeground,
    iBackground;

  style = pStyle->iFlags & STYLE_BITS;

  if ((style & COLOURTYPE) == COLOUR_CUSTOM)
    {
    ASSERT (pStyle->iForeColour >= 0 && pStyle->iForeColour < MAX_CUSTOM);
    if (style & INVERSE)    // inverse inverts foreground and background
      {
      // custom colour is stored in iForeColour only
      colour1 = m_customback [pStyle->iForeColour];
      colour2 = m_customtext [pStyle->iForeColour];
      }
    else
      {
      colour1 = m_customtext [pStyle->iForeColour];
      colour2 = m_customback [pStyle->iForeColour];
      }
    }
  // for RGB colour is just itself
  else if ((style & COLOURTYPE) == COLOUR_RGB)
    {
    if (style & INVERSE)    // inverse inverts foreground and background
      {
      colour1 = pStyle->iBackColour;
      colour2 = pStyle->iForeColour;
      }
    else
      {
      colour1 = pStyle->iForeColour;
      colour2 = pStyle->iBackColour;
      }
    }
  else
    {
    ASSERT (pStyle->iForeColour >= 0 && pStyle->iForeColour < 256);
    ASSERT (pStyle->iBackColour >= 0 && pStyle->iBackColour < 256);

// display bold inverse differently according to user taste

    if (m_bAlternativeInverse)
      {
      iForeground = pStyle->iForeColour;
      iBackground = pStyle->iBackColour;

      if (style & INVERSE)    // inverse inverts foreground and background
        {
        if (iForeground >= 8)
          colour2 = xterm_256_colours [iForeground];
        else
          {
          if (style & HILITE)
            colour2 = m_boldcolour [iForeground];
          else
            colour2 = m_normalcolour [iForeground];
          }
        if (iBackground >= 8)
          colour1 = xterm_256_colours [iBackground];
        else
          colour1 = m_normalcolour [iBackground];
        }
      else
        {
        if (iForeground >= 8)
          colour1 = xterm_256_colours [iForeground];
        else
          {
          if (style & HILITE)
            colour1 = m_boldcolour [iForeground];
          else
            colour1 = m_normalcolour [iForeground];
          }
        if (iBackground >= 8)
          colour2 = xterm_256_colours [iBackground];
        else
          colour2 = m_normalcolour [iBackground];
        }   // end of not inverse


      } // end of alternate way
    else
      {
      if (style & INVERSE)    // inverse inverts foreground and background
        {
        iForeground = pStyle->iBackColour;
        iBackground = pStyle->iForeColour;
        }
      else
        {
        iForeground = pStyle->iForeColour;
        iBackground = pStyle->iBackColour;
        }

      if (style & HILITE)
        {
        if (iForeground >= 8)
          colour1 = xterm_256_colours [iForeground];
        else
          colour1 = m_boldcolour [iForeground];
        }
      else
        {
        if (iForeground >= 8)
          colour1 = xterm_256_colours [iForeground];
        else
          colour1 = m_normalcolour [iForeground];
        }
      if (iBackground >= 8)
        colour2 = xterm_256_colours [iBackground];
      else
        colour2 = m_normalcolour [iBackground];

      } // end of old way

    }   // not custom

  } // end of CMUSHclientDoc::GetStyleRGB 

void CMUSHclientDoc::RememberStyle (const CStyle * pStyle)
  {
  if (!pStyle)
    return;
  
  // for tracking down an obscure bug
  if ((pStyle->iFlags & COLOURTYPE) == COLOUR_CUSTOM)
    {
    ASSERT (pStyle->iForeColour >= 0 && pStyle->iForeColour < MAX_CUSTOM);
    }
  else
  if ((pStyle->iFlags & COLOURTYPE) == COLOUR_ANSI)
    {
    ASSERT (pStyle->iForeColour >= 0 && pStyle->iForeColour < 256);
    ASSERT (pStyle->iBackColour >= 0 && pStyle->iBackColour < 256);
    }


  m_iFlags       = pStyle->iFlags & STYLE_BITS; 
  m_iForeColour  = pStyle->iForeColour;         
  m_iBackColour  = pStyle->iBackColour;           
  
  } // end of CMUSHclientDoc::RememberStyle

// ── ShiftTabCompleteItem (Lua API: world.ShiftTabCompleteItem) ─────────────
long CMUSHclientDoc::ShiftTabCompleteItem(LPCTSTR Item)
  {
  if (strlen (Item) <= 0 || strlen (Item) > 30)
    return eBadParameter;

  if (strcmp (Item, "<clear>") == 0)
    m_ExtraShiftTabCompleteItems.clear ();
  else if (strcmp (Item, "<functions>") == 0)
    m_bTabCompleteFunctions = true;
  else if (strcmp (Item, "<nofunctions>") == 0)
    m_bTabCompleteFunctions = false;
  else
    {
    if (!IsValidName (Item))    // alphanumeric, dot, hyphen, underscore
      return eBadParameter;
    m_ExtraShiftTabCompleteItems.insert (Item);
    }
  return eOK;
  }

// ── SSLCleanup — tear down the OpenSSL session/context ─────────────────────
void CMUSHclientDoc::SSLCleanup (void)
  {
#if defined(OPENSSL_FOUND)
  if (m_pSSL)
    {
    if (m_bSSL_Connected)
      SSL_shutdown (m_pSSL);
    SSL_free (m_pSSL);
    m_pSSL = NULL;
    }
  if (m_pSSL_CTX)
    {
    SSL_CTX_free (m_pSSL_CTX);
    m_pSSL_CTX = NULL;
    }
#endif
  m_bSSL_Connected = false;
  }

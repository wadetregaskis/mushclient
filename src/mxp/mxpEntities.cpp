// mxpEntities.cpp - MXP entities

#include "stdafx.h"
#include "../MUSHclient.h"
#include "../doc.h"
#include "../mushview.h"
#include "../mainfrm.h"
#include "mxp.h"
#include "../protocols.h"
#include "../text_edges.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char BASED_CODE THIS_FILE[] = __FILE__;
#endif


// here when entity collection complete

void CMUSHclientDoc::MXP_collected_entity (void)
  {
  m_strMXPstring.TrimLeft ();
  m_strMXPstring.TrimRight ();

  // case insensitive
//  m_strMXPstring.MakeLower ();

  // count them
  m_iMXPentities++;

//  TRACE1 ("MXP collected entity %s\n", (LPCTSTR) m_strMXPstring);

  MXP_error (DBG_ALL, msgMXP_CollectedEntity,
              TFormat ("MXP entity: &%s;", 
              (LPCTSTR) m_strMXPstring)); 

  if (!IsValidName (m_strMXPstring) && m_strMXPstring.Left (1) != "#")
    {
    MXP_error (DBG_ERROR, errMXP_InvalidEntityName,
                TFormat ("Invalid MXP entity name \"%s\" supplied.",
                (LPCTSTR) m_strMXPstring)); 
    return;
    }

// see if we know of this entity

CString strEntityContents = MXP_GetEntity (m_strMXPstring);
  
  if (!strEntityContents.IsEmpty ())
    {
//  if the entity happens to be < & > etc. don't reprocess it
    m_bMXP = false;
    DisplayMsg (strEntityContents, strEntityContents.GetLength (), 0, true);  // fake packet
    m_bMXP = true;
    }

  } // end of CMUSHclientDoc::MXP_collected_entity



CString CMUSHclientDoc::MXP_GetEntity (CString & strName)
  {

CString strLowerCaseName = strName;

  strLowerCaseName.MakeLower ();

CString strEntityContents;

  // look for &#nnn; 

  if (strName [0] == '#')
    {
    unsigned long iResult = 0;

    // validate and work out number.  Nothing but digits, and stop once it could not
    // be a character, so a long one cannot wrap round into a good one.
    const bool bHex = strName [1] == 'x' || strName [1] == 'X';
    const int iFirstDigit = bHex ? 2 : 1;

    if (strName.GetLength () <= iFirstDigit)
      {
      MXP_error (DBG_ERROR, errMXP_InvalidEntityNumber,
                TFormat ("Invalid number in MXP entity: &%s;" ,
                          (LPCTSTR) strName));
      return "";
      }

    for (int i = iFirstDigit; i < strName.GetLength (); i++)
      {
      const unsigned char c = strName [i];
      if (bHex ? !isxdigit (c) : !isdigit (c))
        {
        MXP_error (DBG_ERROR, errMXP_InvalidEntityNumber,
                  TFormat (bHex ? "Invalid hex number in MXP entity: &%s;"
                                : "Invalid number in MXP entity: &%s;" ,
                            (LPCTSTR) strName));
        return "";
        }
      const unsigned long iDigit = isdigit (c) ? c - '0' : toupper (c) - 'A' + 10;
      iResult = iResult * (bHex ? 16 : 10) + iDigit;
      if (iResult > 0x10FFFF)
        {
        MXP_error (DBG_ERROR, errMXP_DisallowedEntityNumber,
                  TFormat ("Disallowed number in MXP entity: &%s;" ,
                            (LPCTSTR) strName));
        return "";
        }
      }

    // we will accept tabs, newlines and carriage-returns ;)
    if (iResult < 32 && iResult != 9 && iResult != 10 && iResult != 13)
      {
      MXP_error (DBG_ERROR, errMXP_DisallowedEntityNumber,
                TFormat ("Disallowed number in MXP entity: &%s;" ,
                          (LPCTSTR) strName));
      return "";
      }

    // In a UTF-8 world a character is its UTF-8 bytes, whatever its number: 233 is two
    // bytes, and the single byte 233 is not a character there at all.
    if (m_bUTF_8)
      {
      const std::string utf8 = proto::EncodeUtf8 (iResult);
      if (utf8.empty ())
        MXP_error (DBG_ERROR, errMXP_DisallowedEntityNumber,
                  TFormat ("Disallowed number in MXP entity: &%s;" ,
                            (LPCTSTR) strName));
      return utf8.c_str ();
      }

    // Otherwise the byte, up to 255, as it always was.  Beyond that, whatever the world's
    // own code page calls it, if anything.
    if (iResult <= 255)
      {
      unsigned char cOneCharacterLine [2] = { (unsigned char) iResult, 0};
      return (char *) cOneCharacterLine;
      }

#ifdef _WIN32
    // Only if it really has one.  ? or a look-alike would not be what the server said.
    if (iResult <= 0xFFFF)
      {
      const wchar_t wide = (wchar_t) iResult;
      char bytes [8] = "";
      BOOL bDefaultUsed = FALSE;
      const UINT codePage = WorldCodePage (this);
      const int iBytes = WideCharToMultiByte (codePage, WC_NO_BEST_FIT_CHARS, &wide, 1,
                                              bytes, sizeof bytes - 1, NULL, &bDefaultUsed);
      if (iBytes > 0 && !bDefaultUsed)
        return CString (bytes, iBytes);
      }
#endif

    MXP_error (DBG_ERROR, errMXP_DisallowedEntityNumber,
              TFormat ("Disallowed number in MXP entity: &%s;" ,
                        (LPCTSTR) strName));
    return "";
    } // end of entity starting with #

    
  // look up global entities first
  if (App.m_EntityMap.Lookup (strName, strEntityContents))
    {
    // the table is in Latin-1: &eacute; is the byte 233, which UTF-8 must not see
    if (m_bUTF_8 && strEntityContents.GetLength () == 1 && (strEntityContents [0] & 0x80))
      return proto::EncodeUtf8 ((unsigned char) strEntityContents [0]).c_str ();
    return strEntityContents;
    }
  // then try ones for this document
  else if (m_CustomEntityMap.Lookup (strLowerCaseName, strEntityContents))
    return strEntityContents;

  MXP_error (DBG_ERROR, errMXP_UnknownEntity,
              TFormat ("Unknown MXP entity: &%s;" ,
                        (LPCTSTR) strName));

  return "";
  }  // end of CMUSHclientDoc::MXP_GetEntity 


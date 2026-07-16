// methods_clipboard.cpp

// Clipboard handling

#if defined(_WIN32) || defined(MUSHCLIENT_FULL_BUILD)
#include "stdafx.h"
#include "../../MUSHclient.h"
#include "../../doc.h"
#else
#include "platform/doc_stub.h"
#endif

// Implements:

//    GetClipboard
//    SetClipboard


BSTR CMUSHclientDoc::GetClipboard() 
{
CString strContents; 

 GetClipboardContents (strContents, m_bUTF_8, false);   // no warning

	return strContents.AllocSysString();
}  // end of CMUSHclientDoc::GetClipboard



void CMUSHclientDoc::SetClipboard(LPCTSTR Text) 
{
putontoclipboard (Text, m_bUTF_8);
}  // end of CMUSHclientDoc::SetClipboard

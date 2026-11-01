// methods_clipboard.cpp

// Clipboard handling

#include "stdafx.h"
#include "../../MUSHclient.h"
#include "../../doc.h"

// Implements:

//    GetClipboard
//    SetClipboard


BSTR CMUSHclientDoc::GetClipboard() 
{
CString strContents; 

 ClipboardToWorldText (this, strContents, false);   // no warning

	return ScriptBstr (strContents);
}  // end of CMUSHclientDoc::GetClipboard



void CMUSHclientDoc::SetClipboard(LPCTSTR Text) 
{
WorldTextToClipboard (this, Text);
}  // end of CMUSHclientDoc::SetClipboard

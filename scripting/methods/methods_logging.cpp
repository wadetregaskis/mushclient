// methods_logging.cpp

// Relating to logging

#if defined(_WIN32) || defined(MUSHCLIENT_FULL_BUILD)
#include "stdafx.h"
#include "../../MUSHclient.h"
#include "../../doc.h"
#include "../errors.h"
#else
#include "platform/doc_stub.h"
#endif

// Implements:

//    CloseLog
//    FlushLog
//    IsLogOpen
//    OpenLog
//    WriteLog


// OpenLog: single-sourced in doc_shared.cpp (ITransport T3d)



long CMUSHclientDoc::WriteLog(LPCTSTR Message) 
{
  if (m_logfile)
    {

    CString strMessage = Message;

    // append newline if there isn't one already
    if (strMessage.Right (2) != "\n")
      strMessage += "\n";

    size_t count;
    size_t len = strMessage.GetLength ();

    count = fwrite (strMessage, 1, len, m_logfile);

    if (count != len)
      return eLogFileBadWrite;

    return eOK;
    }

	return eLogFileNotOpen;
}   // end of CMUSHclientDoc::WriteLog

BOOL CMUSHclientDoc::IsLogOpen() 
{
	return m_logfile != NULL;
}   // end of CMUSHclientDoc::IsLogOpen


long CMUSHclientDoc::FlushLog() 
{
  
  if (m_logfile)
    {
    fflush (m_logfile);
    return eOK;
    }

	return eLogFileNotOpen;

}  // end of CMUSHclientDoc::FlushLog

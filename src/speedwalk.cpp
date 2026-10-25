// speedwalk.cpp — shared speedwalk expansion/reversal.
//
// One implementation compiled into both mushcore and mushapp (see speedwalk.h).
// The logic is the canonical Windows logic from
// scripting/methods/methods_speedwalks.cpp; direction resolution is injected so
// neither build depends on the GUI-only MapDirectionsMap.

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"

#include "speedwalk.h"

#include <string.h>   // strchr
#include <ctype.h>    // isspace / isdigit / toupper / tolower

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char BASED_CODE THIS_FILE[] = __FILE__;
#endif

// Error strings all start with '*' (callers detect an error by testing [0]).
static CString MakeSpeedWalkErrorString (LPCTSTR sMsg)
{
    CString strResult = "*";
    strResult += sMsg;
    return strResult;
}

CString ExpandSpeedwalk (const char* input,
                         const CString& filler,
                         const char* endline,
                         CString (*lookupDir)(char letter))
{
    CString strResult, str;
    int count;
    const char* p = input;

    while (*p)   // until string runs out
    {
        // bypass spaces
        while (isspace ((unsigned char) *p))
            p++;

        // bypass comments
        if (*p == '{')
        {
            while (*p && *p != '}')
                p++;
            if (*p != '}')
                return MakeSpeedWalkErrorString (Translate ("Comment code of \'{\' not terminated by a \'}\'"));
            p++;        // skip } symbol
            continue;   // back to start of loop
        }

        // get counter, if any (cap checked PER DIGIT so a long run can't overflow)
        count = 0;
        while (isdigit ((unsigned char) *p))
        {
            count = (count * 10) + (*p++ - '0');
            if (count > 99)
                return MakeSpeedWalkErrorString (Translate ("Speed walk counter exceeds 99"));
        }

        // no counter, assume do once
        if (count == 0)
            count = 1;

        // bypass spaces after counter
        while (isspace ((unsigned char) *p))
            p++;

        if (count > 1 && *p == 0)
            return MakeSpeedWalkErrorString (Translate ("Speed walk counter not followed by an action"));
        if (count > 1 && *p == '{')
            return MakeSpeedWalkErrorString (Translate ("Speed walk counter may not be followed by a comment"));

        // might have had trailing space
        if (*p == 0)
            break;

        // action prefix: C/O/L/K = close/open/lock/unlock
        if (strchr ("COLK", toupper ((unsigned char) *p)))
        {
            if (count > 1)
                return MakeSpeedWalkErrorString (Translate ("Action code of C, O, L or K must not follow "
                                                            "a speed walk count (1-99)"));
            switch (toupper ((unsigned char) *p++))
            {
                case 'C': strResult += "close ";  break;
                case 'O': strResult += "open ";   break;
                case 'L': strResult += "lock ";   break;
                case 'K': strResult += "unlock "; break;
            }

            // bypass spaces after open/close/lock/unlock
            while (isspace ((unsigned char) *p))
                p++;

            if (*p == 0 || toupper ((unsigned char) *p) == 'F' || *p == '{')
                return MakeSpeedWalkErrorString (Translate ("Action code of C, O, L or K must be followed "
                                                            "by a direction"));
        }

        // work out which direction we are going
        switch (toupper ((unsigned char) *p))
        {
            case 'N': case 'S': case 'E': case 'W': case 'U': case 'D':
                str = lookupDir ((char) tolower ((unsigned char) *p));
                break;

            case 'F':
                str = filler;
                break;

            case '(':     // special string (eg. (ne/sw) )
            {
                str.Empty ();
                for (p++; *p && *p != ')'; )
                    str += *p++;
                if (*p != ')')
                    return MakeSpeedWalkErrorString (Translate ("Action code of \'(\' not terminated by a \')\'"));
                int iSlash = str.Find ("/");   // only use up to the slash
                if (iSlash != -1)
                    str = str.Left (iSlash);
            }
            break;

            default:
                return MakeSpeedWalkErrorString (TFormat ("*Invalid direction \'%c\' in speed walk, must be "
                                                          "N, S, E, W, U, D, F, or (something)",
                                                          *p));
        }

        p++;    // bypass whatever that character was (or the trailing bracket)

        // output required number of times
        for (int j = 0; j < count; j++)
            strResult += str + endline;
    }

    return strResult;
}

CString ReverseSpeedwalkString (const char* input,
                                const char* endline,
                                CString (*lookupRev)(char letter),
                                CString (*lookupRevWord)(const CString& word))
{
    CString strResult, str, strAction;
    int count;
    const char* p = input;

    while (*p)
    {
        // preserve spaces (prepend, since we build the result in reverse)
        while (isspace ((unsigned char) *p))
        {
            switch (*p)
            {
                case '\r': break;                             // discard carriage returns
                case '\n': strResult = endline + strResult; break;
                default:   strResult = CString ((char) *p) + strResult; break;
            }
            p++;
        }

        // preserve comments
        if (*p == '{')
        {
            str.Empty ();
            for (; *p && *p != '}'; )
                str += *p++;
            if (*p != '}')
                return MakeSpeedWalkErrorString (Translate ("Comment code of \'{\' not terminated by a \'}\'"));
            p++;            // skip } symbol
            str += "}";
            strResult = str + strResult;
            continue;
        }

        // get counter, if any (cap checked PER DIGIT)
        count = 0;
        while (isdigit ((unsigned char) *p))
        {
            count = (count * 10) + (*p++ - '0');
            if (count > 99)
                return MakeSpeedWalkErrorString (Translate ("Speed walk counter exceeds 99"));
        }

        if (count == 0)
            count = 1;

        // bypass spaces after counter
        while (isspace ((unsigned char) *p))
            p++;

        if (count > 1 && *p == 0)
            return MakeSpeedWalkErrorString (Translate ("Speed walk counter not followed by an action"));
        if (count > 1 && *p == '{')
            return MakeSpeedWalkErrorString (Translate ("Speed walk counter may not be followed by a comment"));

        if (*p == 0)
            break;

        // action prefix C/O/L/K (remembered, re-emitted before the reversed direction)
        if (strchr ("COLK", toupper ((unsigned char) *p)))
        {
            if (count > 1)
                return MakeSpeedWalkErrorString (Translate ("Action code of C, O, L or K must not follow "
                                                            "a speed walk count (1-99)"));
            strAction = *p++;

            while (isspace ((unsigned char) *p))
                p++;

            if (*p == 0 || toupper ((unsigned char) *p) == 'F' || *p == '{')
                return MakeSpeedWalkErrorString (Translate ("Action code of C, O, L or K must be followed "
                                                            "by a direction"));
        }
        else
            strAction.Empty ();

        // work out which direction we are going
        switch (toupper ((unsigned char) *p))
        {
            case 'N': case 'S': case 'E': case 'W': case 'U': case 'D': case 'F':
                str = lookupRev ((char) tolower ((unsigned char) *p));
                break;

            case '(':     // special string (eg. (ne/sw) )
            {
                str.Empty ();
                for (p++; *p && *p != ')'; )
                    str += (char) tolower ((unsigned char) *p++);
                if (*p != ')')
                    return MakeSpeedWalkErrorString (Translate ("Action code of \'(\' not terminated by a \')\'"));
                int iSlash = str.Find ("/");
                if (iSlash == -1)
                {
                    // no slash: try to reverse the whole word (eg. ne -> sw)
                    CString rev = lookupRevWord (str);
                    if (!rev.IsEmpty ())
                        str = rev;
                }
                else
                {
                    CString strLeftPart  = str.Left (iSlash);
                    CString strRightPart = str.Mid (iSlash + 1);
                    str = strRightPart + "/" + strLeftPart;   // swap parts
                }
                str = "(" + str;
                str += ")";
            }
            break;

            default:
                return MakeSpeedWalkErrorString (TFormat ("Invalid direction \'%c\' in speed walk, must be "
                                                          "N, S, E, W, U, D, F, or (something)",
                                                          *p));
        }

        p++;    // bypass whatever that character was (or the trailing bracket)

        // output it (prepend, in reverse order)
        if (count > 1)
            strResult = CFormat ("%i%s%s", count, (LPCTSTR) strAction, (LPCTSTR) str) + strResult;
        else
            strResult = strAction + str + strResult;
    }

    return strResult;
}

#pragma once   // unconditional — clang doesn't define _MSC_VER
#if _MSC_VER > 1000
// (legacy MSVC guard — kept for compatibility)
#endif // _MSC_VER > 1000

#include <float.h>
#include <math.h>

#define MAX_XML_DOCUMENT_SIZE (100 * 1024 * 1024)   // max XML file length - 100 MB
#define MAX_XML_NAME_LENGTH 255        // max name of element (eg. <trigger> )

// Elements may nest this deep.  ProcessNode recurses once per level (and the
// tree's readers -- utils.xmlread among them -- do too), so 100,000 nested
// <a> overflowed the stack.  The deepest real file found was 4 levels (world
// files, the bundled plugins, Quow's Discworld plugins); 512 is far past any.
#define MAX_XML_DEPTH 512

// What entity references may add to a document, in all: max(this, twice the
// file).  Each entity is expanded in full where it is defined, so ten
// definitions each naming the last ten times (the "billion laughs") came to
// gigabytes from a few hundred bytes.  Real files use the five standard
// entities, each of which makes the text shorter, and a few short custom ones.
#define MIN_XML_ENTITY_EXPANSION (16 * 1024 * 1024)

class CXMLelement;

class CAttribute : public CObject
  {

  public:

  CString strName;          // element name (eg. colour)
  CString strValue;         // value (eg. red )
  bool    bUsed;            // true if used in document
  UINT    iLine;            // line it appeared on in XML document

  CAttribute () 
    {
    bUsed = false;
    };    // constructor


  };

typedef CTypedPtrMap <CMapStringToPtr, CString, CAttribute*> CAttributeMap;

typedef CTypedPtrList <CPtrList, CXMLelement*> CElementList;

class CXMLelement : public CObject
  {

  public:

  CString strName;                  // element name (eg. <triggers>)
  CAttributeMap   AttributeMap;     // list of attributes to this element (colour="red")
                                    //  nb - attributes must be unique, so we use a map
  CElementList    ChildrenList;     // children (eg. <trigger> )
  CString         strContent;       // content (eg. <match>blah blah</match> )
                                    //   in this case content would be "blah blah"

  bool            bEmpty;           // if empty, eg. <br />  - no closing tag
  bool            bUsed;            // true if used in document
  UINT            iLine;            // line it appeared on in XML document

  CXMLelement () 
    {
    bEmpty = false;
    bUsed = false;
    iLine = 1;
    // The same 127 buckets (so attributes come back in the same order), but
    // allocated by the first SetAt: most elements have no attributes, and
    // every one of them used to allocate the table anyway -- about 1 KB each
    // on x64, most of a large plugin's parse tree.
    AttributeMap.InitHashTable (127, FALSE);
    };    // constructor

  // destructor deletes attributes and children -- without recursing: each
  // child's own children are moved onto the list first, so a deep tree is
  // freed in a loop rather than one stack frame per level.
  ~CXMLelement () 
    { 
    while (!ChildrenList.IsEmpty ())
      {
      CXMLelement * pChild = ChildrenList.RemoveHead ();
      while (!pChild->ChildrenList.IsEmpty ())
        ChildrenList.AddTail (pChild->ChildrenList.RemoveHead ());
      delete pChild;      // childless now: its destructor does not recurse
      }
    DELETE_MAP  (AttributeMap, CAttribute);
    };  // destructor

  };

class CXMLparser : public CObject
  {

  const char * m_xmlBuff; // pointer into buffer for XML parsing
  CString m_strxmlBuffer;   // buffer for parsing
  UINT m_xmlLength;  // size of XML buffer
  UINT m_xmlPos;     // where we are in the buffer
  int  m_iDepth;     // elements open at m_xmlPos (MAX_XML_DEPTH)
  double m_fEntityBytes;      // bytes entity references have added so far
  double m_fMaxEntityBytes;   // ...and the most they may (MIN_XML_ENTITY_EXPANSION)

  void SkipComments (const bool bAndSpaces);
  void SkipSpaces (void);
  void ProcessNode (CXMLelement & parent);
  void ProcessAttributes (CXMLelement & node);
  void ProcessDeclaration (const CString strDeclaration);
  void ProcessDoctype (void);
  void ProcessDTD (void);
  void ProcessEntity (void);
  void AssembleContent (CXMLelement & node, 
                        const UINT iFirstLine, 
                        const UINT iNodeStart);

  CString ReplaceEntities (const CString strSource);
  CString GetName (const char * sType);
  CString GetValue (const char * sType, const char * sName);

  public:

  CMapStringToString m_CustomEntityMap;    // custom XML entities

  CString Get_XML_Entity (CString & strName);

  CXMLparser ()
    {
    m_xmlBuff = NULL; // no buffer
    m_xmlLine = 1;  // first line is line 1
    m_xmlPos = 0;   // start at start of buffer
    m_xmlLength = 0;
    m_iDepth = 0;
    m_fEntityBytes = 0;
    m_fMaxEntityBytes = MIN_XML_ENTITY_EXPANSION;

    }; // end of constructor

  ~CXMLparser ()
    {
    m_strxmlBuffer.Empty ();  
    }; // end of destructor

  CXMLelement   m_xmlRoot;          // root element
  UINT          m_xmlLine;          // which line we are up to
  CString       m_strDocumentName;  // root document name  (from <!DOCTYPE name .. ?> )

  // builds an XML structure from the supplied (and open) file

  void BuildStructure (CFile * file);

  };

// gets an attribute from an element - marks it as used

CAttribute * GetAttribute (CXMLelement & node,
                           const char * sName);

bool Get_XML_boolean (CXMLelement & node,
                          const char * sName,
                          bool & bValue,
                          const bool bUseDefault);

bool Get_XML_boolean_ushort (CXMLelement & node,
                          const char * sName,
                          unsigned short & bValue,
                          const bool bUseDefault);
                   
bool Get_XML_boolean_BOOL (CXMLelement & node,
                          const char * sName,
                          BOOL & bValue,
                          const bool bUseDefault);

bool Get_XML_boolean_ushort_mask (CXMLelement & node,
                          const char * sName,
                          unsigned short & bValue,
                          const unsigned short iMask,
                          const bool bUseDefault);

bool Get_XML_boolean_int_mask (CXMLelement & node,
                          const char * sName,
                          int & bValue,
                          const int iMask,
                          const bool bUseDefault);

bool Get_XML_number (CXMLelement & node,
                        const char * sName,
                        long & iValue,
                        const bool bUseDefault,
                        long iMinimum = LONG_MIN,
                        long iMaximum = LONG_MAX);

bool Get_XML_short (CXMLelement & node,
                        const char * sName,
                        short & iValue,
                        const bool bUseDefault,
                        const long iMinimum = SHRT_MIN,
                        const long iMaximum = SHRT_MAX);

bool Get_XML_int  (CXMLelement & node,
                        const char * sName,
                        int & iValue,
                        const bool bUseDefault,
                        const long iMinimum = INT_MIN,
                        const long iMaximum = INT_MAX);

bool Get_XML_long  (CXMLelement & node,
                        const char * sName,
                        long & iValue,
                        const bool bUseDefault,
                        const long iMinimum = LONG_MIN,
                        const long iMaximum = LONG_MAX);

bool Get_XML_double  (CXMLelement & node,
                        const char * sName,
                        double & dValue,
                        const bool bUseDefault,
                        const double dMinimum = -DBL_MAX,   // not DBL_MIN
                        const double dMaximum = DBL_MAX);

bool Get_XML_ushort (CXMLelement & node,
                        const char * sName,
                        unsigned short & iValue,
                        const bool bUseDefault,
                        const long iMinimum = 0,
                        const long iMaximum = USHRT_MAX);

bool Get_XML_string (CXMLelement & node,
                        const char * sName,
                        CString & strValue,
                        const bool bUseDefault,
                        const bool bTrimSpaces = false);

bool Get_XML_colour (CXMLelement & node,
                        const char * sName,
                        COLORREF & cValue,
                        const bool bUseDefault);

bool Get_XML_date (CXMLelement & node,
                        const char * sName,
                        CTime & tValue,
                        const bool bUseDefault);


CXMLelement * FindNode (CXMLelement & parent,
                        const char * sName);


bool FindNodeContents (CXMLelement & parent,
                       const char * sName,
                       CString & strValue,
                       const bool bUseDefault,
                       bool bTrim = false);


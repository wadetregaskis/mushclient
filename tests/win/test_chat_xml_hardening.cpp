// tests/win/test_chat_xml_hardening.cpp — what a chat peer, or a world or
// plugin file, could do to the client: the 2026-11 review's findings.
//
// Chat (C1-C9): a prompt's socket freed under it, prompts stacking, a caller
// re-handshaking under its own accept prompt, peer text shown past "ignore" and
// the display limits, a leaked CFile, quadratic appends, device names that got
// through, unbounded auto-saved files, ctype on signed chars.
//
// XML (X1-X9): unbounded nesting, entity bombs, a 1 KB hash table per element,
// include loops and depth, include paths that open network shares and devices,
// a modal box per load warning, an unterminated buffer in IsArchiveXML, device
// paths for the script and log files.
//
// Every hostile input is built here.  Each test asserts a clean refusal and
// that nothing crashed.  Windows-only (tests/win/) because the chat ones stand
// in for the user at a modal box (g_pfnMcTestMessageBox), and the box is the
// point: a real one runs a message loop in which the socket closes and the
// reaper runs, which is what the hook does "inside" it.

#include "mc_test.h"

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "chatsock.h"
#include "scripting/errors.h"
#include "scripting/scripting.h"
#include "xml/xmlparse.h"
#include "pal/path_safety.h"
#include "platform/wx_mfc_hooks.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#include <crtdbg.h>
#include <functional>
#include <string>

// ── helpers ─────────────────────────────────────────────────────────────────

// Stand in for the user at every message box while one of these is alive.
static std::function<int (const char *, unsigned int)> s_boxAnswer;
static int BoxThunk (const char * text, unsigned int nType)
  {
  return s_boxAnswer ? s_boxAnswer (text, nType) : HeadlessMessageBoxAnswer (nType);
  }
struct BoxHook
  {
  explicit BoxHook (std::function<int (const char *, unsigned int)> f)
    { s_boxAnswer = f; g_pfnMcTestMessageBox = BoxThunk; }
  ~BoxHook ()
    { g_pfnMcTestMessageBox = nullptr; s_boxAnswer = nullptr; }
  };

// A fresh folder under %TEMP%, with a trailing backslash.
static std::string TempFolder (const char * tag)
  {
  static int s_seq = 0;
  char dir [MAX_PATH];
  ::GetTempPathA (sizeof dir, dir);
  std::string folder = std::string (dir) + "mc_cxh_" + tag + "_" +
                       std::to_string ((int) ::GetCurrentProcessId ()) + "_" +
                       std::to_string (++s_seq) + "\\";
  ::CreateDirectoryA (folder.c_str (), NULL);
  return folder;
  }

static void WriteFile (const std::string & path, const std::string & text)
  {
  FILE * f = fopen (path.c_str (), "wb");
  if (f)
    {
    fwrite (text.data (), 1, text.size (), f);
    fclose (f);
    }
  }

// Everything noted: in the output buffer, and queued for it (Tell queues notes
// made before the output window exists).
static std::string Noted (CMUSHclientDoc & doc)
  {
  std::string s;
  for (POSITION pos = doc.m_LineList.GetHeadPosition (); pos; )
    {
    CLine * pLine = doc.m_LineList.GetNext (pos);
    s.append (pLine->text, pLine->len);
    s += '\n';
    }
  for (const CPaneStyle & style : doc.m_OutstandingLines)
    s += style.m_sText;
  return s;
  }

// A test document has its output buffer from the start; a world being opened
// does not yet, which is when load warnings are boxes rather than notes.
struct NotYetOpen
  {
  CMUSHclientDoc & m_doc;
  explicit NotYetOpen (CMUSHclientDoc & doc) : m_doc (doc)
    { m_doc.m_LinePositions.Clear (); }
  ~NotYetOpen ()
    { m_doc.m_LinePositions.Reset (m_doc.m_LineList.GetHeadPosition ()); }
  };

static bool Contains (const std::string & haystack, const char * needle)
  {
  return haystack.find (needle) != std::string::npos;
  }

static bool TryLoadPlugin (CMUSHclientDoc & doc, const std::string & path)
  {
  try { doc.InternalLoadPlugin (CString (path.c_str ())); return true; }
  catch (CException * e) { e->Delete (); return false; }
  }

// Parse a document held in memory; "" when it parsed, else the error.
static std::string ParseXml (CXMLparser & parser, const std::string & xml)
  {
  try
    {
    CMemFile f ((BYTE *) xml.data (), (UINT) xml.size ());
    parser.BuildStructure (&f);
    return "";
    }
  catch (CException * e)
    {
    char buf [512] = "";
    e->GetErrorMessage (buf, sizeof buf);
    e->Delete ();
    return buf [0] ? buf : "(error)";
    }
  }

// ═══ chat ══════════════════════════════════════════════════════════════════

// C1: the snoop prompt had no ModalGuard, so a peer that hung up while it was
// up was deleted by the reaper (WM_TIMER is delivered inside the box) and the
// prompt's caller carried on with the freed socket.  And a second CHAT_SNOOP
// inside the box asked again.
TEST (ChatHardening, ASnoopPromptKeepsItsSocketWhenThePeerHangsUpUnderIt)
  {
  CMUSHclientDoc doc;
  doc.m_bAutoAllowSnooping = FALSE;
  CChatSocket * sock = new CChatSocket (&doc);
  sock->m_bCanSnoop = true;
  sock->m_strRemoteUserName = "peer";
  doc.m_ChatList.AddTail (sock);

  int iBoxes = 0;
  int iListedInsideTheBox = -1;
  {
  BoxHook hook ([&] (const char *, unsigned int) {
    ++iBoxes;
    sock->Process_Snoop (CString (""));   // asked again: no second box
    sock->OnClose (0);                    // FD_CLOSE
    doc.CheckTimers ();                   // the reaper, from WM_TIMER
    iListedInsideTheBox = (int) doc.m_ChatList.GetCount ();
    return IDYES;
  });
  sock->Process_Snoop (CString (""));
  }

  EXPECT_EQ (1, iBoxes);
  EXPECT_EQ (1, iListedInsideTheBox);         // kept while asked
  ASSERT_EQ (1, (int) doc.m_ChatList.GetCount ());
  EXPECT_FALSE (sock->m_bHeIsSnooping);       // they hung up: not granted
  EXPECT_EQ (0, sock->m_iInModal);
  EXPECT_FALSE (sock->m_bSnoopPending);

  doc.CheckTimers ();                         // and reaped once answered
  EXPECT_EQ (0, (int) doc.m_ChatList.GetCount ());
  }

// C3: a caller re-handshaking while its accept prompt is up.  The packet used
// to be parsed before the "already asking" check, so the user's yes went to
// whatever name and dialect came second.
TEST (ChatHardening, ACallerCannotRehandshakeUnderItsOwnAcceptPrompt)
  {
  CMUSHclientDoc doc;
  doc.m_bValidateIncomingCalls = TRUE;
  CChatSocket * sock = new CChatSocket (&doc);
  sock->m_bIncoming = true;
  sock->m_iChatStatus = eChatAwaitingConnectionRequest;
  doc.m_ChatList.AddTail (sock);

  int iBoxes = 0;
  std::string strAsked, strNameInside;
  int iTypeInside = -1;
  {
  BoxHook hook ([&] (const char * text, unsigned int) {
    ++iBoxes;
    strAsked = text;
    const std::string again = "ZCHAT:Mallory\t\n10.0.0.99" "04050";
    sock->ProcessConnectionRequest (again.data (), (int) again.size ());
    strNameInside = (const char *) sock->m_strRemoteUserName;
    iTypeInside = sock->m_iChatConnectionType;
    return IDNO;
  });
  const std::string first = "CHAT:Fred\n10.0.0.5" "4050 ";
  sock->ProcessConnectionRequest (first.data (), (int) first.size ());
  }

  EXPECT_EQ (1, iBoxes);
  EXPECT_TRUE (Contains (strAsked, "from Fred"));
  EXPECT_EQ (std::string ("Fred"), strNameInside);
  EXPECT_EQ ((int) eChatMudMaster, iTypeInside);
  EXPECT_EQ (500L, sock->m_iFileBlockSize);
  EXPECT_TRUE (sock->m_bDeleteMe);            // the user said no
  EXPECT_EQ (0, CChatSocket::s_iAcceptPromptsUp);
  doc.CheckTimers ();
  EXPECT_EQ (0, (int) doc.m_ChatList.GetCount ());
  }

// C2: accept prompts stacked without limit -- each caller's box ran a message
// loop in which the next caller asked -- and the silent-caller timeout closed
// a caller whose box was up, freeing its listener slot.  One prompt at a time
// now, and the timeout leaves a caller being asked about alone.
TEST (ChatHardening, OnlyOneAcceptPromptIsUpAtATime)
  {
  CMUSHclientDoc doc;
  doc.m_bValidateIncomingCalls = TRUE;
  CChatSocket * a = new CChatSocket (&doc);
  a->m_bIncoming = true;
  a->m_iChatStatus = eChatAwaitingConnectionRequest;
  CChatSocket * b = new CChatSocket (&doc);
  b->m_bIncoming = true;
  b->m_iChatStatus = eChatAwaitingConnectionRequest;
  doc.m_ChatList.AddTail (a);
  doc.m_ChatList.AddTail (b);

  int iBoxes = 0;
  bool bClosedWhileAsked = true, bSecondRefused = false;
  {
  BoxHook hook ([&] (const char *, unsigned int) {
    ++iBoxes;
    // the user takes two minutes to answer
    a->m_tWhenStarted = CTime::GetCurrentTime () - CTimeSpan (0, 0, 2, 0);
    doc.CheckTimers ();
    bClosedWhileAsked = a->m_bDeleteMe;
    // and meanwhile Bob calls
    const std::string bob = "CHAT:Bob\n10.0.0.6" "4050 ";
    b->ProcessConnectionRequest (bob.data (), (int) bob.size ());
    bSecondRefused = b->m_bDeleteMe;
    return IDNO;
  });
  const std::string fred = "CHAT:Fred\n10.0.0.5" "4050 ";
  a->ProcessConnectionRequest (fred.data (), (int) fred.size ());
  }

  EXPECT_EQ (1, iBoxes);                // Bob got no box of his own
  EXPECT_FALSE (bClosedWhileAsked);
  EXPECT_TRUE (bSecondRefused);
  EXPECT_TRUE (Contains (Noted (doc), "another call is waiting for an answer"));
  EXPECT_EQ (0, CChatSocket::s_iAcceptPromptsUp);
  doc.CheckTimers ();
  doc.CheckTimers ();
  EXPECT_EQ (0, (int) doc.m_ChatList.GetCount ());
  }

// C4: snoop data went straight to AnsiNote -- past "ignore", the byte and line
// limits and ON_PLUGIN_CHAT_DISPLAY -- and a name change, a file refusal's
// reason, a bare ping response and a peek list were shown for ignored peers.
TEST (ChatHardening, AnIgnoredPeersTextIsNotShownAndSnoopDataHasTheLimits)
  {
  CMUSHclientDoc doc;
  CChatSocket sock (&doc);
  sock.m_strRemoteUserName = "peer";
  sock.m_bIgnore = true;

  sock.Process_Snoop_data (CString ("SNOOPED-TEXT"));
  sock.Process_Name_change (CString ("NEW-NAME"));
  sock.Process_File_deny (CString ("DENY-REASON"));
  sock.Process_Ping_response (CString ("PING-TEXT"));
  sock.Process_Peek_list (CString ("1.2.3.4~4050~PEEKED~"));
  const std::string ignored = Noted (doc);
  EXPECT_FALSE (Contains (ignored, "SNOOPED-TEXT"));
  EXPECT_FALSE (Contains (ignored, "NEW-NAME"));
  EXPECT_FALSE (Contains (ignored, "DENY-REASON"));
  EXPECT_FALSE (Contains (ignored, "PING-TEXT"));
  EXPECT_FALSE (Contains (ignored, "PEEKED"));
  EXPECT_EQ (std::string ("NEW-NAME"), std::string ((const char *) sock.m_strRemoteUserName));

  // not ignored: shown, but within the world's byte limit
  sock.m_bIgnore = false;
  doc.m_iMaxChatBytesPerMessage = 10;
  sock.Process_Snoop_data (CString ("abcdefghijKLMNOPQRST"));
  const std::string shown = Noted (doc);
  EXPECT_TRUE (Contains (shown, "abcdefghij"));
  EXPECT_FALSE (Contains (shown, "KLMNOPQRST"));
  EXPECT_TRUE (Contains (shown, "truncated"));

  // and a peek list is held to the line limit
  doc.m_iMaxChatBytesPerMessage = 0;
  doc.m_iMaxChatLinesPerMessage = 2;
  sock.Process_Peek_list (CString ("1.1.1.1~1~AA1~2.2.2.2~2~AA2~3.3.3.3~3~AA3~4.4.4.4~4~AA4~"));
  const std::string peeked = Noted (doc);
  EXPECT_TRUE (Contains (peeked, "AA2"));
  EXPECT_FALSE (Contains (peeked, "AA3"));
  EXPECT_TRUE (Contains (peeked, "Peek list truncated"));
  }

// C5: ChatSendFile checked only for a transfer under way, not for an offer
// being asked about, whose yes then put its own CFile over the one opened here.
TEST (ChatHardening, ChatSendFileWaitsForAnOfferBeingAskedAbout)
  {
  const std::string folder = TempFolder ("send");
  const std::string file = folder + "to_send.txt";
  WriteFile (file, "hello");

  CMUSHclientDoc doc;
  CChatSocket * sock = new CChatSocket (&doc);
  sock->m_iChatStatus = eChatConnected;
  sock->m_bFileOfferPending = true;
  doc.m_ChatList.AddTail (sock);

  EXPECT_EQ ((long) eAlreadyTransferringFile, doc.ChatSendFile (sock->m_iChatID, file.c_str ()));
  EXPECT_TRUE (sock->m_pFile == NULL);
  EXPECT_FALSE (sock->m_bDoingFileTransfer);

  sock->m_bFileOfferPending = false;
  sock->m_iChatStatus = eChatClosed;    // the destructor sends nothing
  ::DeleteFileA (file.c_str ());
  ::RemoveDirectoryA (folder.c_str ());
  }

// C6: ChatAppendBytes now appends in place; what it appends must still be
// every byte, NULs included (zChat frames carry them in every header).
TEST (ChatHardening, AFrameStillCarriesEveryByteNulsAndAll)
  {
  std::string data;
  for (int i = 0; i < 3000; i++)
    data += (char) (i % 7 == 0 ? 0 : 'a' + i % 26);
  const CString frame = CChatSocket::ZChatFrame (CHAT_TEXT_PERSONAL,
                                                  CString (data.data (), (int) data.size ()));
  ASSERT_EQ ((int) data.size () + 4, frame.GetLength ());
  EXPECT_TRUE (memcmp (frame.GetString () + 4, data.data (), data.size ()) == 0);
  }

// C7: the device-name check missed COM0/LPT0, the superscript digits Windows
// maps (COM¹ is COM1), CONIN$/CONOUT$, and names Windows trims ("NUL.").
TEST (ChatHardening, EveryDeviceNameIsRefusedAsAFileName)
  {
  const char * const refused [] =
    { "NUL", "nul.txt", "NUL.", "NUL..", "CON .txt", "aux. .", "prn.tar.gz",
      "COM0", "com9.log", "LPT0", "COM\xB9", "com\xB2.txt", "LPT\xB3",
      "COM\xC2\xB9", "lpt\xC2\xB2.dat", "CONIN$", "conout$.txt",
      "", "...", "a:stream", "a?b", "a\x01" "b" };
  for (const char * p : refused)
    {
    EXPECT_FALSE (IsSafeChatFileName (CString (p)));
    if (IsSafeChatFileName (CString (p)))
      printf ("      (accepted \"%s\")\n", p);
    }
  const char * const accepted [] =
    { "report.pdf", "COM10", "COMX", "LPT", "console.txt", "nullable.c",
      "con-fig.ini", "auxiliary.dat", "COM1a" };
  for (const char * p : accepted)
    {
    EXPECT_TRUE (IsSafeChatFileName (CString (p)));
    if (!IsSafeChatFileName (CString (p)))
      printf ("      (refused \"%s\")\n", p);
    }
  }

// C8: auto-allow took any number of files of any size, and when "name (2)" to
// "name (999)" all existed it wrote over "name (999)".
TEST (ChatHardening, AutoSavedFilesAreLimitedAndNeverOverwriteTheLastName)
  {
  const std::string folder = TempFolder ("auto");
  CMUSHclientDoc doc;
  doc.m_bAutoAllowFiles      = TRUE;
  doc.m_strChatSaveDirectory = folder.c_str ();

  int iBoxes = 0;
  BoxHook hook ([&] (const char *, unsigned int) { ++iBoxes; return IDNO; });

  // within the limits: saved unasked, as before
  {
  CChatSocket sock (&doc);
  sock.m_bCanSendFiles = true;
  sock.Process_File_start (CString ("small.bin,10"));
  EXPECT_EQ (0, iBoxes);
  EXPECT_TRUE (sock.m_bDoingFileTransfer);
  sock.StopFileTransfer (true);       // deletes the half-received file
  }

  // too big to take unasked: asked (and declined, by the hook)
  {
  CChatSocket sock (&doc);
  sock.m_bCanSendFiles = true;
  sock.Process_File_start (CString ("big.bin,209715200"));
  EXPECT_EQ (1, iBoxes);
  EXPECT_FALSE (sock.m_bDoingFileTransfer);
  }

  // too many this session: asked too
  {
  CChatSocket sock (&doc);
  sock.m_bCanSendFiles = true;
  sock.m_iAutoFilesReceived = CChatSocket::kMaxAutoFilesPerSession;
  sock.Process_File_start (CString ("one-more.bin,10"));
  EXPECT_EQ (2, iBoxes);
  EXPECT_FALSE (sock.m_bDoingFileTransfer);
  }

  // every name taken: refused, and "f (999).txt" left alone
  WriteFile (folder + "f.txt", "");
  for (int i = 2; i < 1000; i++)
    WriteFile (folder + "f (" + std::to_string (i) + ").txt", "");
  WriteFile (folder + "f (999).txt", "the user's own");
  {
  CChatSocket sock (&doc);
  sock.m_bCanSendFiles = true;
  sock.Process_File_start (CString ("f.txt,10"));
  EXPECT_FALSE (sock.m_bDoingFileTransfer);
  EXPECT_TRUE (sock.m_pFile == NULL);
  }
  char buf [64] = "";
  {
  FILE * f = fopen ((folder + "f (999).txt").c_str (), "rb");
  ASSERT_TRUE (f != NULL);
  fgets (buf, sizeof buf, f);
  fclose (f);
  }
  EXPECT_EQ (std::string ("the user's own"), std::string (buf));

  ::DeleteFileA ((folder + "f.txt").c_str ());
  for (int i = 2; i < 1000; i++)
    ::DeleteFileA ((folder + "f (" + std::to_string (i) + ").txt").c_str ());
  ::RemoveDirectoryA (folder.c_str ());
  }

// C9: IsNumber handed signed chars to isdigit -- a debug-CRT assertion, and
// undefined behaviour, for any byte above 127 (a chat peer's snoop colour
// prefix, a file size, any XML attribute).
TEST (ChatHardening, IsNumberTakesBytesAbove127)
  {
  EXPECT_FALSE (IsNumber (CString ("\xB2\xB3")));
  EXPECT_FALSE (IsNumber (CString ("12\xE9")));
  EXPECT_FALSE (IsNumber (CString ("-\xFF"), true));
  EXPECT_TRUE  (IsNumber (CString ("1234")));
  EXPECT_TRUE  (IsNumber (CString ("-12"), true));
  }

// ═══ XML ═══════════════════════════════════════════════════════════════════

// X1: ProcessNode recursed per element and the destructor freed recursively,
// so 100,000 nested <a> overflowed the stack.
TEST (XmlHardening, DeepNestingIsAClearErrorNotAStackOverflow)
  {
  std::string deep;
  for (int i = 0; i < 100000; i++) deep += "<a>";
  for (int i = 0; i < 100000; i++) deep += "</a>";
  {
  CXMLparser parser;
  const std::string error = ParseXml (parser, deep);
  EXPECT_TRUE (Contains (error, "nested too deeply"));
  }

  // the deepest allowed still parses, and its tree is freed without recursing
  std::string ok;
  for (int i = 0; i < MAX_XML_DEPTH; i++) ok += "<a>";
  ok += "x";
  for (int i = 0; i < MAX_XML_DEPTH; i++) ok += "</a>";
  CXMLparser parser;
  EXPECT_EQ (std::string (""), ParseXml (parser, ok));

  // and a tree built some other way, far deeper, is freed in a loop too
  CXMLelement * root = new CXMLelement;
  CXMLelement * p = root;
  for (int i = 0; i < 200000; i++)
    {
    CXMLelement * child = new CXMLelement;
    p->ChildrenList.AddTail (child);
    p = child;
    }
  delete root;    // would overflow the stack if it recursed
  }

// X2: "billion laughs" -- each entity expanded in full where defined.
TEST (XmlHardening, AnEntityBombIsRefusedBeforeItIsBuilt)
  {
  std::string bomb = "<?xml version=\"1.0\"?>\n<!DOCTYPE lolz [\n"
                     "<!ENTITY lol0 \"lollollollollollollollollollol\">\n";
  for (int i = 1; i < 10; i++)
    {
    bomb += "<!ENTITY lol" + std::to_string (i) + " \"";
    for (int j = 0; j < 10; j++)
      bomb += "&lol" + std::to_string (i - 1) + ";";
    bomb += "\">\n";
    }
  bomb += "]>\n<lolz>&lol9;</lolz>\n";

  CXMLparser parser;
  const std::string error = ParseXml (parser, bomb);
  EXPECT_TRUE (Contains (error, "expand to too much"));

  // ordinary entities are untouched
  CXMLparser parser2;
  EXPECT_EQ (std::string (""), ParseXml (parser2,
      "<!DOCTYPE x [ <!ENTITY me \"Fred\"> ]>\n<x a=\"&me; &lt;&amp;&gt;\">&me;&quot;</x>"));
  CXMLelement * x = FindNode (parser2.m_xmlRoot, "x");
  ASSERT_TRUE (x != NULL);
  EXPECT_EQ (std::string ("Fred\""), std::string ((const char *) x->strContent));
  }

#ifdef _DEBUG
// X3: every element allocated a 127-bucket hash table (about 1 KB) whether or
// not it had attributes.  Measured on the debug heap: what 10,000 attribute-
// less elements cost, per element.
TEST (XmlHardening, AnElementWithoutAttributesAllocatesNoHashTable)
  {
  std::string xml = "<root>";
  for (int i = 0; i < 10000; i++) xml += "<e/>";
  xml += "</root>";

  _CrtMemState before, after, diff;
  _CrtMemCheckpoint (&before);
  {
  CXMLparser * parser = new CXMLparser;
  EXPECT_EQ (std::string (""), ParseXml (*parser, xml));
  _CrtMemCheckpoint (&after);
  delete parser;
  }
  _CrtMemDifference (&diff, &before, &after);
  const size_t perElement = diff.lSizes [_NORMAL_BLOCK] / 10000;
  printf ("      (%u bytes a parsed element)\n", (unsigned) perElement);
  EXPECT_LT (perElement, (size_t) 600);   // 1 KB and more with the table

  // attributes still work, in the same order as ever
  CXMLparser parser;
  EXPECT_EQ (std::string (""), ParseXml (parser, "<e b=\"2\" a=\"1\" c=\"3\"/>"));
  CXMLelement * e = FindNode (parser.m_xmlRoot, "e");
  ASSERT_TRUE (e != NULL);
  EXPECT_EQ (3, (int) e->AttributeMap.GetCount ());
  EXPECT_EQ (127, (int) e->AttributeMap.GetHashTableSize ());
  }
#endif

// utils.xmlread built a table three Lua stack slots a level without checking
// the stack: a C function is promised 20, so seven levels wrote past it.
TEST (XmlHardening, UtilsXmlreadHandlesDeepDocuments)
  {
  CMUSHclientDoc doc;
  CScriptEngine * engine = new CScriptEngine (&doc, "lua");
  ASSERT_TRUE (!engine->CreateScriptEngine ());
  doc.m_ScriptEngine = engine;
  ASSERT_EQ (0, luaL_dostring (engine->L,
      "local s = string.rep('<a>', 400) .. 'x' .. string.rep('</a>', 400)\n"
      "local t = assert (utils.xmlread (s))\n"
      "local depth = 0\n"
      "while t.nodes do t = t.nodes [1]; depth = depth + 1 end\n"
      "xml_depth = depth\n"
      "local _, err = utils.xmlread (string.rep('<a>', 1000) .. string.rep('</a>', 1000))\n"
      "xml_err = err"));
  lua_getglobal (engine->L, "xml_depth");
  EXPECT_EQ (400, (int) lua_tonumber (engine->L, -1));
  lua_getglobal (engine->L, "xml_err");
  const char * err = lua_tostring (engine->L, -1);
  EXPECT_TRUE (err != NULL && Contains (err, "nested too deeply"));
  lua_pop (engine->L, 2);
  }

// X4: includes inside a plugin were never tracked, so a plugin that included
// itself, or two include files that included each other, recursed until the
// stack overflowed.  Now a clean "includes itself", and a depth cap.
TEST (XmlHardening, APluginThatIncludesItselfIsAWarningNotAStackOverflow)
  {
  const std::string folder = TempFolder ("loop");
  const std::string plugin = folder + "selfish.xml";
  const std::string a = folder + "a.xml", b = folder + "b.xml";
  WriteFile (plugin,
    "<?xml version=\"1.0\"?>\n<!DOCTYPE muclient>\n<muclient>\n"
    "<plugin name=\"Selfish\" id=\"0123456789abcdef01234567\" language=\"Lua\" />\n"
    "<include name=\"" + plugin + "\"/>\n"
    "<include name=\"" + a + "\"/>\n"
    "</muclient>\n");
  WriteFile (a, "<?xml version=\"1.0\"?>\n<muclient>\n<include name=\"" + b + "\"/>\n</muclient>\n");
  WriteFile (b, "<?xml version=\"1.0\"?>\n<muclient>\n<include name=\"" + a + "\"/>\n</muclient>\n");

  std::string boxes;
  int iBoxes = 0;
  {
  BoxHook hook ([&] (const char * text, unsigned int) { ++iBoxes; boxes += text; return IDOK; });
  CMUSHclientDoc doc;
  NotYetOpen opening (doc);   // as a world being opened: no output window yet
  TryLoadPlugin (doc, plugin);
  EXPECT_TRUE (doc.m_vLoadingFiles.empty ());
  }
  EXPECT_EQ (1, iBoxes);      // one box for the whole load
  EXPECT_TRUE (Contains (boxes, "includes itself"));

  ::DeleteFileA (plugin.c_str ());
  ::DeleteFileA (a.c_str ());
  ::DeleteFileA (b.c_str ());
  ::RemoveDirectoryA (folder.c_str ());
  }

TEST (XmlHardening, IncludesNestedPastTheCapAreRefused)
  {
  const std::string folder = TempFolder ("deep");
  const int kFiles = 40;
  for (int i = 0; i < kFiles; i++)
    {
    std::string body = "<?xml version=\"1.0\"?>\n<muclient>\n";
    if (i + 1 < kFiles)
      body += "<include name=\"" + folder + "inc" + std::to_string (i + 1) + ".xml\"/>\n";
    body += "</muclient>\n";
    WriteFile (folder + "inc" + std::to_string (i) + ".xml", body);
    }
  const std::string world = folder + "world.mcl";
  WriteFile (world, "<?xml version=\"1.0\"?>\n<!DOCTYPE muclient>\n<muclient>\n"
                    "<include name=\"" + folder + "inc0.xml\"/>\n</muclient>\n");

  std::string boxes;
  {
  BoxHook hook ([&] (const char * text, unsigned int) { boxes += text; return IDOK; });
  CMUSHclientDoc doc;
  NotYetOpen opening (doc);   // as a world being opened: no output window yet
  doc.LoadWorldFromFile (world.c_str ());
  EXPECT_TRUE (doc.m_vLoadingFiles.empty ());
  }
  EXPECT_TRUE (Contains (boxes, "nested too deeply"));

  for (int i = 0; i < kFiles; i++)
    ::DeleteFileA ((folder + "inc" + std::to_string (i) + ".xml").c_str ());
  ::DeleteFileA (world.c_str ());
  ::RemoveDirectoryA (folder.c_str ());
  }

// X5: include names were opened as given -- \\host\share (Windows signs in to
// the host just to open it), \\.\pipe, \\.\COM1, NUL.
TEST (XmlHardening, IncludeNamesThatAreDevicesOrStrangersSharesAreRefused)
  {
  CMUSHclientDoc doc;
  const char * const refused [] =
    { "\\\\evil\\share\\plugin.xml", "//evil/share/plugin.xml", "\\/evil\\share\\x.xml",
      "\\\\.\\pipe\\mc", "\\\\.\\COM1", "\\\\?\\C:\\plugins\\x.xml", "//./pipe/mc",
      "\\??\\C:\\x.xml", "\\\\?\\UNC\\evil\\share\\x.xml" };
  for (const char * p : refused)
    {
    bool bThrew = false;
    try { doc.CheckIncludeName (CString (p)); }
    catch (CException * e) { e->Delete (); bThrew = true; }
    EXPECT_TRUE (bThrew);
    if (!bThrew)
      printf ("      (allowed \"%s\")\n", p);
    }

  const char * const allowed [] =
    { "plugin.xml", "C:\\Program Files\\MUSHclient\\plugins\\x.xml", "..\\..\\mine\\x.xml",
      "$PLUGINSDEFAULTDIR\\x.xml", "./x.xml", "\\rooted\\x.xml" };
  for (const char * p : allowed)
    {
    bool bThrew = false;
    try { doc.CheckIncludeName (CString (p)); }
    catch (CException * e) { e->Delete (); bThrew = true; }
    EXPECT_FALSE (bThrew);
    if (bThrew)
      printf ("      (refused \"%s\")\n", p);
    }

  // a share the client is already using (the world is on it) is fine
  doc.m_vLoadingFiles.push_back ("\\\\nas\\mud\\worlds\\discworld.mcl");
  bool bThrew = false;
  try { doc.CheckIncludeName (CString ("\\\\NAS\\Mud\\plugins\\x.xml")); }
  catch (CException * e) { e->Delete (); bThrew = true; }
  EXPECT_FALSE (bThrew);
  bThrew = false;
  try { doc.CheckIncludeName (CString ("\\\\nas\\other\\x.xml")); }
  catch (CException * e) { e->Delete (); bThrew = true; }
  EXPECT_TRUE (bThrew);     // same host, different share: not one in use
  doc.m_vLoadingFiles.clear ();

  EXPECT_EQ (std::string ("\\\\nas\\mud"), pal::UncShareRoot ("//NAS/Mud/x/y.xml"));
  EXPECT_EQ (std::string (""), pal::UncShareRoot ("\\\\nas"));
  EXPECT_EQ (std::string (""), pal::UncShareRoot ("C:\\x"));
  }

// ...and a device reached through a relative name: refused when loading.
TEST (XmlHardening, AnIncludeOfNulIsRefusedWhenTheWorldLoads)
  {
  const std::string folder = TempFolder ("nul");
  const std::string world = folder + "world.mcl";
  WriteFile (world, "<?xml version=\"1.0\"?>\n<!DOCTYPE muclient>\n<muclient>\n"
                    "<include name=\"NUL\"/>\n<include name=\"x\\COM1.xml\"/>\n</muclient>\n");
  std::string boxes;
  {
  BoxHook hook ([&] (const char * text, unsigned int) { boxes += text; return IDOK; });
  CMUSHclientDoc doc;
  NotYetOpen opening (doc);   // as a world being opened: no output window yet
  doc.LoadWorldFromFile (world.c_str ());
  }
  EXPECT_TRUE (Contains (boxes, "\"NUL\" names a device"));
  EXPECT_TRUE (Contains (boxes, "COM1.xml\" names a device"));
  ::DeleteFileA (world.c_str ());
  ::RemoveDirectoryA (folder.c_str ());
  }

// X6: a modal box per load warning.  Now one, naming the first ten.
TEST (XmlHardening, ALoadWithManyWarningsShowsOneBox)
  {
  const std::string folder = TempFolder ("warn");
  const std::string world = folder + "world.mcl";
  std::string body = "<?xml version=\"1.0\"?>\n<!DOCTYPE muclient>\n<muclient>\n";
  for (int i = 0; i < 15; i++)
    body += "<stale" + std::to_string (i) + "/>\n";
  body += "</muclient>\n";
  WriteFile (world, body);

  int iBoxes = 0;
  std::string boxes;
  std::string noted;
  {
  BoxHook hook ([&] (const char * text, unsigned int) { ++iBoxes; boxes += text; return IDOK; });
  CMUSHclientDoc doc;
  NotYetOpen opening (doc);   // as a world being opened: no output window yet
  doc.LoadWorldFromFile (world.c_str ());
  noted = Noted (doc);
  }
  EXPECT_EQ (1, iBoxes);
  EXPECT_TRUE (Contains (boxes, "<stale0>"));
  EXPECT_TRUE (Contains (boxes, "<stale9>"));
  EXPECT_FALSE (Contains (boxes, "<stale10>"));
  EXPECT_TRUE (Contains (boxes, "and 5 more"));
  EXPECT_TRUE (Contains (noted, "<stale14>"));    // all of them noted
  ::DeleteFileA (world.c_str ());
  ::RemoveDirectoryA (folder.c_str ());
  }

// X7: IsArchiveXML converted UTF-16 into 500 bytes; text that needed more
// failed the conversion, leaving the buffer unterminated for strlen.
TEST (XmlHardening, IsArchiveXMLReadsAUtf16FileFullOfNonAscii)
  {
  std::wstring text = L"<?xml version=\"1.0\"?><muclient>";
  for (int i = 0; i < 300; i++)
    text += (wchar_t) 0x4E2D;     // three bytes each in UTF-8
  std::string bytes = "\xFF\xFE";
  bytes.append ((const char *) text.data (), text.size () * 2);
  CMemFile f ((BYTE *) bytes.data (), (UINT) bytes.size ());
  CArchive ar (&f, CArchive::load);
  EXPECT_TRUE (IsArchiveXML (ar));

  std::wstring junk;
  for (int i = 0; i < 300; i++)
    junk += (wchar_t) 0x4E2D;
  std::string bytes2 = "\xFF\xFE";
  bytes2.append ((const char *) junk.data (), junk.size () * 2);
  CMemFile f2 ((BYTE *) bytes2.data (), (UINT) bytes2.size ());
  CArchive ar2 (&f2, CArchive::load);
  EXPECT_FALSE (IsArchiveXML (ar2));
  }

// X9: the script and log file names may not be devices.
TEST (XmlHardening, AWorldsScriptAndLogFilesMayNotBeDevices)
  {
  const std::string folder = TempFolder ("dev");
  const std::string world = folder + "world.mcl";
  WriteFile (world,
    "<?xml version=\"1.0\"?>\n<!DOCTYPE muclient>\n<muclient>\n"
    "<world defaults=\"y\" name=\"w\" site=\"localhost\" port=\"4000\""
    " script_filename=\"\\\\.\\pipe\\mc\""
    " auto_log_file_name=\"C:\\logs\\NUL.txt\" />\n</muclient>\n");
  std::string boxes;
  {
  BoxHook hook ([&] (const char * text, unsigned int) { boxes += text; return IDOK; });
  CMUSHclientDoc doc;
  NotYetOpen opening (doc);   // as a world being opened: no output window yet
  doc.LoadWorldFromFile (world.c_str ());
  EXPECT_TRUE (doc.m_strScriptFilename.IsEmpty ());
  EXPECT_TRUE (doc.m_strAutoLogFileName.IsEmpty ());
  }
  EXPECT_TRUE (Contains (boxes, "names a device"));

  // a log on a NAS is a real setup, and is kept
  WriteFile (world,
    "<?xml version=\"1.0\"?>\n<!DOCTYPE muclient>\n<muclient>\n"
    "<world defaults=\"y\" name=\"w\" site=\"localhost\" port=\"4000\""
    " auto_log_file_name=\"\\\\nas\\logs\\%Y.txt\" />\n</muclient>\n");
  {
  CMUSHclientDoc doc;
  NotYetOpen opening (doc);   // as a world being opened: no output window yet
  doc.LoadWorldFromFile (world.c_str ());
  EXPECT_EQ (std::string ("\\\\nas\\logs\\%Y.txt"),
             std::string ((const char *) doc.m_strAutoLogFileName));
  }
  ::DeleteFileA (world.c_str ());
  ::RemoveDirectoryA (folder.c_str ());
  }

// tests/test_protocols.cpp
// The native protocols: the renegotiation guard, CHARSET, NEW-ENVIRON, MTTS, MSSP,
// OSC 8 links, the newer MXP tags, MSP and the GMCP helper.
//
// Against the real document and the real display loop, with bytes fed in as a server
// would send them, because the state machines' whole job is surviving being split up.
// protocols.h's proto:: functions are what a hostile server meets, and get tables.

#include "mc_test.h"
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "protocols.h"
#include "pal/https_get.h"
#include "scripting/errors.h"
#include "pal/itransport.h"
#include "fake_transport.h"
#include "mxp/mxp.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include "posix_compat.h"

namespace fs = std::filesystem;

namespace {

// ------------------------------------------------------------- the fixture

struct World
  {
  CMUSHclientDoc doc;
  RecordingTransport * t;

  explicit World (bool bMXP = false)
    {
    t = new RecordingTransport ();
    doc.m_pTransport = t;                   // the doc's destructor tears it down
    doc.m_iConnectPhase = eConnectConnectedToMud;
    if (bMXP)
      {
      doc.m_iUseMXP = eUseMXP;
      doc.MXP_On ();
      }
    }

  void feed (const std::string & s)
    {
    doc.DisplayMsg (s.data (), (int) s.size (), 0);
    }

  // one byte at a time, as a slow connection might
  void feedSplit (const std::string & s)
    {
    for (const char c : s)
      doc.DisplayMsg (&c, 1, 0);
    }

  std::string takeSent ()
    {
    std::string sent = t->sent;
    t->sent.clear ();
    return sent;
    }

  std::string allOutput ()
    {
    std::string out;
    for (POSITION pos = doc.m_LineList.GetHeadPosition (); pos; )
      {
      CLine * l = doc.m_LineList.GetNext (pos);
      out.append (l->text, l->len);
      if (l->hard_return)
        out += "\n";
      }
    return out;
    }

  CStyle * styleAt (int column)
    {
    CLine * line = doc.m_pCurrentLine;
    int at = 0;
    for (POSITION pos = line->styleList.GetHeadPosition (); pos; )
      {
      CStyle * style = line->styleList.GetNext (pos);
      if (column >= at && column < at + style->iLength)
        return style;
      at += style->iLength;
      }
    return nullptr;
    }

  CLine * lastFinishedLine ()
    {
    POSITION pos = doc.m_LineList.GetTailPosition ();
    while (pos)
      {
      CLine * l = doc.m_LineList.GetPrev (pos);
      if (l != doc.m_pCurrentLine && l->hard_return)
        return l;
      }
    return nullptr;
    }
  };

std::string Bytes (std::initializer_list <int> values)
  {
  std::string s;
  for (const int v : values)
    s += (char) v;
  return s;
  }

const std::string kWILL = Bytes ({ IAC, WILL });
const std::string kWONT = Bytes ({ IAC, WONT });
const std::string kDO   = Bytes ({ IAC, DO });
const std::string kDONT = Bytes ({ IAC, DONT });
const std::string kSB   = Bytes ({ IAC, SB });
const std::string kSE   = Bytes ({ IAC, SE });
const std::string kESC  = "\x1B";

std::string SB_ (int option, const std::string & data)
  {
  return kSB + (char) option + data + kSE;
  }

std::string TempDir (const char * tag)
  {
  std::error_code ec;
  const fs::path dir = fs::temp_directory_path (ec) / (std::string ("mc_test_proto_") + tag + "_" + std::to_string ((int) getpid ()));
  fs::remove_all (dir, ec);
  fs::create_directories (dir, ec);
  return dir.string ();
  }

void TouchFile (const fs::path & path)
  {
  std::error_code ec;
  fs::create_directories (path.parent_path (), ec);
  std::ofstream (path) << "x";
  }

std::string WritePlugin (const char * id, const char * name, const char * script)
  {
  static int s_seq = 0;
  const std::string path = TempDir ("plugin") + "/p" + std::to_string (++s_seq) + ".xml";
  std::ofstream f (path);
  f << "<?xml version=\"1.0\"?>\n<!DOCTYPE muclient>\n<muclient>\n"
    << "<plugin name=\"" << name << "\" author=\"T\" id=\"" << id << "\" language=\"Lua\" version=\"1.0\" />\n"
    << "<script><![CDATA[\n" << script << "\n]]></script>\n</muclient>\n";
  return path;
  }

CPlugin * LoadPlugin (CMUSHclientDoc & doc, const char * id, const char * script)
  {
  try { doc.InternalLoadPlugin (CString (WritePlugin (id, "t", script).c_str ())); }
  catch (CException * e) { e->Delete (); return nullptr; }
  catch (...) { return nullptr; }
  return doc.GetPlugin (id);
  }

std::string PluginVar (CPlugin * p, const char * name)
  {
  CVariable * v = nullptr;
  if (!p || !p->m_VariableMap.Lookup (name, v) || !v)
    return "<unset>";
  return (const char *) v->strContents;
  }

} // anonymous namespace

// ------------------------------------------------------------- the guard

TEST (ProtoGuard, TenThenMuted)
{
  proto::NegotiationGuard guard;
  for (int i = 0; i < 10; i++)
    EXPECT_TRUE (guard.Allow (42, 1000 + i));
  EXPECT_FALSE (guard.Allow (42, 1020));          // the eleventh
  EXPECT_FALSE (guard.Allow (42, 1020 + 9999));   // still muted
  EXPECT_TRUE (guard.Allow (42, 1020 + 10000));   // and back
  EXPECT_TRUE (guard.Allow (43, 1021));           // others were never affected
}

TEST (ProtoGuard, SlowOnesAreFine)
{
  proto::NegotiationGuard guard;
  // ten a minute for ten minutes is ten a minute
  for (uint64_t t = 1; t < 600000; t += 6100)
    EXPECT_TRUE (guard.Allow (1, t));
}

TEST (ProtoGuard, ALoopGetsNoReplies)
{
  World w;
  w.doc.m_pProto->testNow = 5000;
  std::string flood;
  for (int i = 0; i < 30; i++)       // the loop dedupe cannot catch: it keeps flipping
    {
    flood += kWILL + (char) 3;
    flood += kWONT + (char) 3;
    }
  w.feed (flood);
  const std::string sent = w.takeSent ();

  // nine or ten replies each way, and then silence
  int replies = 0;
  for (size_t at = 0; (at = sent.find ((char) IAC, at)) != std::string::npos; at += 3)
    replies++;
  EXPECT_LE (replies, 10);

  // ten seconds later it listens again
  w.doc.m_pProto->testNow += 10001;
  w.feed (kWILL + (char) 3);
  EXPECT_FALSE (w.takeSent ().empty ());
}

TEST (ProtoGuard, EchoStillTracksSoPasswordsStayHidden)
{
  World w;
  w.doc.m_pProto->testNow = 5000;
  for (int i = 0; i < 12; i++)
    {
    w.feed (kWILL + (char) TELOPT_ECHO);
    w.feed (kWONT + (char) TELOPT_ECHO);
    }
  w.feed (kWILL + (char) TELOPT_ECHO);
  EXPECT_TRUE (w.doc.m_bNoEcho);        // muted, and it still heard
}

TEST (ProtoGuard, NotedOnceAndReset)
{
  World w;
  w.doc.m_pProto->testNow = 5000;
  for (int i = 0; i < 40; i++)
    w.feed (kWILL + (char) 3);
  EXPECT_TRUE (w.doc.m_pProto->bGuardNoted);
  EXPECT_TRUE (w.allOutput ().find ("renegotiating") != std::string::npos);
  w.doc.m_pProto->ResetForConnection ();
  EXPECT_FALSE (w.doc.m_pProto->bGuardNoted);
  EXPECT_TRUE (w.doc.m_pProto->guard.Allow (3, 6000));
}

// A plugin that counts its handshakes.
static const char * const kHandshakeCounter =
  "n = 0\n"
  "function OnPluginTelnetRequest (opt, what)\n"
  "  if opt ~= 77 then return false end\n"
  "  if what == 'SENT_DO' then n = n + 1; SetVariable ('n', n) end\n"
  "  return true\n"
  "end\n";

TEST (ProtoGuard, RepeatedWillDoesNotRerunHandshakeButACopyoverDoes)
{
  World w;
  w.doc.m_pProto->testNow = 5000;
  CPlugin * p = LoadPlugin (w.doc, "aaaaaaaaaaaaaaaaaaaaaa01", kHandshakeCounter);
  ASSERT_TRUE (p != nullptr);

  w.feed (kWILL + (char) 77);
  EXPECT_EQ (kDO + (char) 77, w.takeSent ());
  EXPECT_EQ (std::string ("1"), PluginVar (p, "n"));

  // the server says it again: no reply, and no second handshake
  w.feed (kWILL + (char) 77);
  EXPECT_TRUE (w.takeSent ().empty ());
  EXPECT_EQ (std::string ("1"), PluginVar (p, "n"));

  // much later, as after a copyover: there is no DO to send, but the plugin must hear
  w.doc.m_pProto->testNow += 61000;
  w.feed (kWILL + (char) 77);
  EXPECT_TRUE (w.takeSent ().empty ());
  EXPECT_EQ (std::string ("2"), PluginVar (p, "n"));
}

// ------------------------------------------------------------- CHARSET

TEST (ProtoCharset, Choosing)
{
  const auto latin = proto::CharsetNames (false, 1252);
  const auto utf8 = proto::CharsetNames (true, 1252);

  // the server's order decides, among what we can honestly do
  EXPECT_EQ (std::string ("ISO-8859-1"), proto::ChooseCharset (",UTF-8,ISO-8859-1,US-ASCII", latin));
  EXPECT_EQ (std::string ("UTF-8"), proto::ChooseCharset (",UTF-8,ISO-8859-1,US-ASCII", utf8));
  EXPECT_EQ (std::string ("US-ASCII"), proto::ChooseCharset (" US-ASCII ISO-8859-1", latin));

  // whatever the case, and the server hears its own spelling back
  EXPECT_EQ (std::string ("utf-8"), proto::ChooseCharset (";utf-8", utf8));
  EXPECT_EQ (std::string ("Latin1"), proto::ChooseCharset (";Latin1", latin));
  EXPECT_EQ (std::string ("UTF8"), proto::ChooseCharset (",UTF8", utf8));

  // what we can't do
  EXPECT_EQ (std::string (""), proto::ChooseCharset (",UTF-8", latin));
  EXPECT_EQ (std::string (""), proto::ChooseCharset (",KOI8-R,BIG5", utf8));
  EXPECT_EQ (std::string (""), proto::ChooseCharset (",ISO-8859-1", utf8));

  // a translation table offer, then the plain list
  EXPECT_EQ (std::string ("UTF-8"), proto::ChooseCharset (std::string ("[TTABLE]\x01;UTF-8"), utf8));
  EXPECT_EQ (std::string (""), proto::ChooseCharset ("", utf8));
  EXPECT_EQ (std::string (""), proto::ChooseCharset (",", utf8));
}

TEST (ProtoCharset, OtherCodePages)
{
  const auto russian = proto::CharsetNames (false, 1251);
  EXPECT_EQ (std::string ("cp1251"), proto::ChooseCharset (",UTF-8,cp1251", russian));
  EXPECT_EQ (std::string (""), proto::ChooseCharset (",ISO-8859-1", russian));
  EXPECT_EQ (std::string ("BIG5"), proto::MnesCharsetName (false, 950));
  EXPECT_EQ (std::string ("UTF-8"), proto::MnesCharsetName (true, 1252));
  EXPECT_EQ (std::string ("ISO-8859-1"), proto::MnesCharsetName (false, 1252));
}

TEST (ProtoCharset, AcceptsWithoutChangingTheWorld)
{
  World w;
  w.doc.m_bUTF_8 = false;
  w.feed (kWILL + (char) TELOPT_CHARSET);
  w.takeSent ();
  w.feed (SB_ (TELOPT_CHARSET, std::string ("\x01,UTF-8,us-ascii")));
  const std::string sent = w.takeSent ();
  EXPECT_TRUE (sent.find ("us-ascii") != std::string::npos);
  EXPECT_FALSE (w.doc.m_bUTF_8);

  w.doc.m_bUTF_8 = true;
  w.feed (SB_ (TELOPT_CHARSET, std::string ("\x01,utf-8")));
  EXPECT_EQ (kSB + (char) TELOPT_CHARSET + (char) 2 + "utf-8" + kSE, w.takeSent ());
}

// ------------------------------------------------------------- MNES and MTTS

TEST (ProtoMnes, BuildingReplies)
{
  proto::EnvVars vars;
  vars.push_back ({ "A", "1" });
  vars.push_back ({ "B", "two" });

  const std::string send (1, '\x01');
  const std::string var (1, '\0');

  // both spellings of "everything"
  const std::string all = std::string (1, '\0') + var + "A" + '\x01' + "1" + var + "B" + '\x01' + "two";
  EXPECT_EQ (all, proto::BuildEnvironReply (send, vars));
  EXPECT_EQ (all, proto::BuildEnvironReply (send + var, vars));

  // one, and one we don't have, which comes back with no value
  EXPECT_EQ (std::string (1, '\0') + var + "B" + '\x01' + "two" + var + "IPADDRESS",
             proto::BuildEnvironReply (send + var + "B" + var + "IPADDRESS", vars));

  // not a SEND
  EXPECT_EQ (std::string (), proto::BuildEnvironReply (std::string (1, '\x02'), vars));
  EXPECT_EQ (std::string (), proto::BuildEnvironReply ("", vars));

  // a value cannot contain syntax
  proto::EnvVars bad;
  bad.push_back ({ "X", std::string ("a\x01""b\xFF") + "c" });
  EXPECT_EQ (std::string (1, '\0') + var + "X" + '\x01' + "abc", proto::BuildEnvironReply (send, bad));
}

TEST (ProtoMnes, Negotiation)
{
  World w;
  w.feed (kDO + (char) proto::kTelOptNewEnviron);
  EXPECT_EQ (kWILL + (char) proto::kTelOptNewEnviron, w.takeSent ());

  w.feed (SB_ (proto::kTelOptNewEnviron, std::string (1, '\x01')));
  const std::string reply = w.takeSent ();
  EXPECT_EQ (0, reply.compare (0, 4, kSB + (char) proto::kTelOptNewEnviron + '\0'));
  EXPECT_TRUE (reply.find ("CLIENT_NAME") != std::string::npos);
  EXPECT_TRUE (reply.find ("MUSHclient") != std::string::npos);
  EXPECT_TRUE (reply.find ("CLIENT_VERSION") != std::string::npos);
  EXPECT_TRUE (reply.find ("CHARSET") != std::string::npos);
  EXPECT_TRUE (reply.find ("MTTS") != std::string::npos);
  EXPECT_TRUE (reply.find ("TERMINAL_TYPE") != std::string::npos);
  EXPECT_TRUE (reply.find ("IPADDRESS") == std::string::npos);    // never
  EXPECT_EQ (kSE, reply.substr (reply.size () - 2));

  // asked for by name, it is still not given
  w.feed (SB_ (proto::kTelOptNewEnviron, std::string ("\x01\0IPADDRESS", 11)));
  const std::string ip = w.takeSent ();
  EXPECT_TRUE (ip.find ("IPADDRESS") != std::string::npos);
  EXPECT_TRUE (ip.find ('\x01', 4) == std::string::npos);    // the name, and no VAL
}

TEST (ProtoMnes, APluginCanHaveIt)
{
  World w;
  CPlugin * p = LoadPlugin (w.doc, "aaaaaaaaaaaaaaaaaaaaaa02",
    "function OnPluginTelnetRequest (opt, what) return opt == 39 end\n");
  ASSERT_TRUE (p != nullptr);
  w.feed (kDO + (char) proto::kTelOptNewEnviron);
  EXPECT_EQ (kWILL + (char) proto::kTelOptNewEnviron, w.takeSent ());
  w.feed (SB_ (proto::kTelOptNewEnviron, std::string (1, '\x01')));
  EXPECT_TRUE (w.takeSent ().empty ());          // it answers, not us
}

TEST (ProtoMtts, Bits)
{
  World w;
  w.doc.m_bUTF_8 = false;
  EXPECT_EQ (777, Proto_MttsBits (w.doc));       // 1 + 8 + 256 + 512
  w.doc.m_bUTF_8 = true;
  EXPECT_EQ (781, Proto_MttsBits (w.doc));
  w.doc.m_bMttsScreenReader = true;
  EXPECT_EQ (845, Proto_MttsBits (w.doc));
  w.doc.m_bUTF_8 = false;
  w.doc.m_bMttsScreenReader = false;

  w.feed (kDO + (char) TELOPT_TERMINAL_TYPE);
  w.takeSent ();
  const std::string send = SB_ (TELOPT_TERMINAL_TYPE, std::string (1, '\x01'));
  w.feed (send);
  w.feed (send);
  w.takeSent ();
  w.feed (send);
  EXPECT_TRUE (w.takeSent ().find ("MTTS 777") != std::string::npos);
}

TEST (ProtoMtts, ScreenReaderIsOffUnlessAsked)
{
  CMUSHclientDoc doc;
  EXPECT_FALSE (doc.m_bMttsScreenReader);
  EXPECT_EQ (0, Proto_MttsBits (doc) & 64);
}

// ------------------------------------------------------------- MSSP

TEST (ProtoMssp, FindingThePort)
{
  const auto mssp = [] (const std::string & name, const std::string & value)
    {
    return std::string ("\x01") + "NAME\x02" + "A MUD" + "\x01" + name + "\x02" + value + "\x01" + "PLAYERS\x02" + "3";
    };
  EXPECT_EQ (4443, proto::FindMsspTlsPort (mssp ("SSL", "4443")));
  EXPECT_EQ (4443, proto::FindMsspTlsPort (mssp ("TLS", "4443")));
  EXPECT_EQ (4443, proto::FindMsspTlsPort (mssp ("ssl", "4443")));
  EXPECT_EQ (0, proto::FindMsspTlsPort (mssp ("SSL", "1")));       // yes, no port
  EXPECT_EQ (0, proto::FindMsspTlsPort (mssp ("SSL", "0")));
  EXPECT_EQ (0, proto::FindMsspTlsPort (mssp ("SSL", "yes")));
  EXPECT_EQ (0, proto::FindMsspTlsPort (mssp ("SSL", "70000")));
  EXPECT_EQ (0, proto::FindMsspTlsPort (mssp ("PORT", "4000")));
  EXPECT_EQ (0, proto::FindMsspTlsPort (""));
  // a list of values
  EXPECT_EQ (4443, proto::FindMsspTlsPort (std::string ("\x01SSL\x02" "1\x02" "4443")));
}

TEST (ProtoMssp, OffersAndNeverSwitches)
{
  World w;
  w.doc.m_port = 4000;

  w.feed (kWILL + (char) proto::kTelOptMssp);
  EXPECT_EQ (kDO + (char) proto::kTelOptMssp, w.takeSent ());

  const std::string packet = SB_ (proto::kTelOptMssp, std::string ("\x01SSL\x02" "4443"));
  w.feed (packet);
  EXPECT_TRUE (w.allOutput ().find ("port 4443") != std::string::npos);
  EXPECT_EQ (4000, (int) w.doc.m_port);
  EXPECT_FALSE (w.doc.m_bUseSSL);

  // once a connection
  const size_t lines = w.doc.m_LineList.GetCount ();
  w.feed (packet);
  EXPECT_EQ (lines, (size_t) w.doc.m_LineList.GetCount ());
}

TEST (ProtoMssp, QuietIfEncryptedOrAskedNotTo)
{
  World w;
  w.doc.m_bUseSSL = true;
  w.feed (kWILL + (char) proto::kTelOptMssp);
  EXPECT_EQ (kDONT + (char) proto::kTelOptMssp, w.takeSent ());

  World w2;
  w2.doc.m_bMsspTlsHint = false;
  w2.feed (kWILL + (char) proto::kTelOptMssp);
  EXPECT_EQ (kDONT + (char) proto::kTelOptMssp, w2.takeSent ());
}

// ------------------------------------------------------------- OSC 8

static const char * const kLinkST = "\x1B]8;;https://example.org/x\x1B\\";
static const char * const kCloseST = "\x1B]8;;\x1B\\";

TEST (ProtoOsc, Classifying)
{
  std::string action;
  EXPECT_EQ ((int) proto::eOscLinkUrl, (int) proto::ClassifyOsc8Target ("http://a.b/", &action));
  EXPECT_EQ (std::string ("http://a.b/"), action);
  EXPECT_EQ ((int) proto::eOscLinkUrl, (int) proto::ClassifyOsc8Target ("HTTPS://a.b/", &action));
  EXPECT_EQ ((int) proto::eOscLinkSend, (int) proto::ClassifyOsc8Target ("send:look%20north", &action));
  EXPECT_EQ (std::string ("look north"), action);
  EXPECT_EQ ((int) proto::eOscLinkPrompt, (int) proto::ClassifyOsc8Target ("prompt:say hi", &action));

  // not on the list
  for (const char * bad : { "file:///c:/windows/x", "javascript:alert(1)", "ftp://a/b", "mailto:a@b",
                            "c:\\windows\\system32\\calc.exe", "\\\\server\\share", "telnet://a", "", "x",
                            "https://", "http://" })
    EXPECT_EQ ((int) proto::eOscLinkNone, (int) proto::ClassifyOsc8Target (bad, &action));

  // a send: cannot hide a second command, or a menu, from the hover
  for (const char * bad : { "send:", "send:a%0Dquit", "send:a%0Aquit", "send:a|quit", "send:a%7Cquit",
                            "send:%1B[0m", "prompt:a%00b" })
    EXPECT_EQ ((int) proto::eOscLinkNone, (int) proto::ClassifyOsc8Target (bad, &action));

  // a stray % is just a %
  EXPECT_EQ ((int) proto::eOscLinkSend, (int) proto::ClassifyOsc8Target ("send:100%", &action));
  EXPECT_EQ (std::string ("100%"), action);
}

TEST (ProtoOsc, ALink)
{
  World w;
  w.feed (std::string ("see ") + kLinkST + "here" + kCloseST + " now");
  EXPECT_EQ (std::string ("see here now"), std::string (w.doc.m_pCurrentLine->text, w.doc.m_pCurrentLine->len));

  CStyle * link = w.styleAt (5);
  ASSERT_TRUE (link != nullptr);
  EXPECT_EQ (ACTION_HYPERLINK, link->iFlags & ACTIONTYPE);
  ASSERT_TRUE (link->pAction != nullptr);
  EXPECT_EQ (CString ("https://example.org/x"), link->pAction->m_strAction);

  EXPECT_EQ (0, w.styleAt (0)->iFlags & ACTIONTYPE);
  EXPECT_EQ (0, w.styleAt (10)->iFlags & ACTIONTYPE);
  EXPECT_TRUE (w.styleAt (10)->pAction == nullptr);
  EXPECT_EQ (NONE, w.doc.m_phase);
}

TEST (ProtoOsc, AllThreeTerminators)
{
  for (const std::string & st : { std::string ("\x07"), std::string ("\x1B\\"), std::string ("\x9C") })
    {
    World w;
    w.doc.m_bUTF_8 = false;
    w.feed ("\x1B]8;;http://a.b/" + st + "link" + "\x1B]8;;" + st + "x");
    EXPECT_EQ (std::string ("linkx"), std::string (w.doc.m_pCurrentLine->text, w.doc.m_pCurrentLine->len));
    EXPECT_EQ (ACTION_HYPERLINK, w.styleAt (0)->iFlags & ACTIONTYPE);
    EXPECT_EQ (0, w.styleAt (4)->iFlags & ACTIONTYPE);
    }
}

TEST (ProtoOsc, SplitAcrossPackets)
{
  World w;
  w.feedSplit (std::string (kLinkST) + "here" + kCloseST + " now\r\n");
  CLine * line = w.lastFinishedLine ();
  ASSERT_TRUE (line != nullptr);
  EXPECT_EQ (std::string ("here now"), std::string (line->text, line->len));
  CStyle * style = line->styleList.GetHead ();
  bool bFoundLink = false;
  for (POSITION pos = line->styleList.GetHeadPosition (); pos; )
    {
    style = line->styleList.GetNext (pos);
    if (style->iLength == 4 && (style->iFlags & ACTIONTYPE) == ACTION_HYPERLINK)
      bFoundLink = true;
    }
  EXPECT_TRUE (bFoundLink);
}

TEST (ProtoOsc, SendShowsWhatItWillSend)
{
  World w;
  w.feed ("\x1B]8;;send:kill%20bob\x07" "click\x1B]8;;\x07");
  CStyle * link = w.styleAt (0);
  EXPECT_EQ (ACTION_SEND, link->iFlags & ACTIONTYPE);
  EXPECT_EQ (CString ("kill bob"), link->pAction->m_strAction);
  EXPECT_TRUE (link->pAction->m_strHint.Find ("kill bob") != -1);

  // and it is the path every send link takes, so it goes to the MUD, as itself
  w.takeSent ();
  w.doc.HyperlinkClicked (link->pAction->m_strAction, ACTION_SEND, false);
  EXPECT_TRUE (w.takeSent ().find ("kill bob") != std::string::npos);
}

TEST (ProtoOsc, PromptGoesToTheCommandWindow)
{
  World w;
  w.feed ("\x1B]8;;prompt:say%20hi\x07" "x\x1B]8;;\x07");
  EXPECT_EQ (ACTION_PROMPT, w.styleAt (0)->iFlags & ACTIONTYPE);
  EXPECT_EQ ((int) eHyperlinkFillCommand,
             (int) w.doc.HyperlinkClicked (w.styleAt (0)->pAction->m_strAction, ACTION_PROMPT, false));
  EXPECT_TRUE (w.takeSent ().empty ());
}

TEST (ProtoOsc, AForbiddenSchemeIsPlainText)
{
  World w;
  w.feed ("\x1B]8;;file:///etc/passwd\x07" "text\x1B]8;;\x07");
  EXPECT_EQ (std::string ("text"), std::string (w.doc.m_pCurrentLine->text, w.doc.m_pCurrentLine->len));
  EXPECT_EQ (0, w.styleAt (0)->iFlags & ACTIONTYPE);
  EXPECT_TRUE (w.styleAt (0)->pAction == nullptr);
}

TEST (ProtoOsc, TheOptionTurnsItOff)
{
  World w;
  w.doc.m_bOsc8Links = false;
  w.feed (std::string (kLinkST) + "text" + kCloseST);
  EXPECT_EQ (std::string ("text"), std::string (w.doc.m_pCurrentLine->text, w.doc.m_pCurrentLine->len));
  EXPECT_EQ (0, w.styleAt (0)->iFlags & ACTIONTYPE);
}

TEST (ProtoOsc, MalformedDoesNotEatText)
{
  // a space: that was never a URL
  {
  World w;
  w.feed ("\x1B]8;;http://a b c");
  EXPECT_EQ (std::string ("http://a b c"), std::string (w.doc.m_pCurrentLine->text, w.doc.m_pCurrentLine->len));
  EXPECT_EQ (NONE, w.doc.m_phase);
  }
  // no terminator, then the line ended
  {
  World w;
  w.feed ("\x1B]8;;http://a.b/c\r\nnext");
  EXPECT_TRUE (w.allOutput ().find ("http://a.b/c") != std::string::npos);
  EXPECT_TRUE (w.allOutput ().find ("next") != std::string::npos);
  EXPECT_EQ (NONE, w.doc.m_phase);
  EXPECT_FALSE (w.doc.m_pProto->bLinkOpen);
  }
  // no terminator, on and on
  {
  World w;
  w.feed ("\x1B]8;;http://a.b/" + std::string (2 * proto::kOsc8MaxPayload, 'x') + " after");
  EXPECT_EQ (NONE, w.doc.m_phase);
  EXPECT_TRUE (w.allOutput ().find (" after") != std::string::npos);
  EXPECT_TRUE (w.allOutput ().find ("http://a.b/") != std::string::npos);
  }
  // params on and on, which are not text
  {
  World w;
  w.feed ("\x1B]8;" + std::string (1000, 'p') + "x");
  EXPECT_EQ (NONE, w.doc.m_phase);
  }
  // a title is still swallowed, whole
  {
  World w;
  w.feed ("\x1B]0;a window title\x07" "after");
  EXPECT_EQ (std::string ("after"), std::string (w.doc.m_pCurrentLine->text, w.doc.m_pCurrentLine->len));
  }
  // a very long one with no end stops being swallowed
  {
  World w;
  w.feed ("\x1B]0;" + std::string (proto::kOscMaxPayload + 10, 'x') + "after");
  EXPECT_EQ (NONE, w.doc.m_phase);
  EXPECT_TRUE (w.allOutput ().find ("after") != std::string::npos);
  }
}

TEST (ProtoOsc, ALinkEndsWithItsLine)
{
  World w;
  w.feed ("\x1B]8;;http://a.b/\x07" "one\r\ntwo");
  EXPECT_FALSE (w.doc.m_pProto->bLinkOpen);
  EXPECT_EQ (0, w.styleAt (0)->iFlags & ACTIONTYPE);
}

TEST (ProtoOsc, ColoursSetInsideTheLinkSurviveItsEnd)
{
  World w;
  w.doc.m_bUseCustomLinkColour = false;
  w.feed ("\x1B]8;;http://a.b/\x07" "\x1B[31m" "red\x1B]8;;\x07" "x");
  EXPECT_EQ ((COLORREF) ANSI_TEXT_RED - ANSI_TEXT_BLACK, w.styleAt (3)->iForeColour);
}

TEST (ProtoOsc, UTF8TailIsNotATerminator)
{
  World w;
  w.doc.m_bUTF_8 = true;
  // U+015C is C5 9C.  In a URL, 0x9C is its tail and not ST.
  w.feed ("\x1B]8;;http://a.b/\xC5\x9C\x07" "x");
  EXPECT_EQ (CString ("http://a.b/\xC5\x9C"), w.styleAt (0)->pAction->m_strAction);
}

// ------------------------------------------------------------- MXP

static const std::string kSecure = "\x1B[1z";

TEST (ProtoMxp, EntityLists)
{
  EXPECT_EQ (std::string ("a"), proto::EntityListAdd ("", "a"));
  EXPECT_EQ (std::string ("a|b"), proto::EntityListAdd ("a", "b"));
  EXPECT_EQ (std::string ("a|c"), proto::EntityListRemove ("a|b|c", "b"));
  EXPECT_EQ (std::string ("b|a"), proto::EntityListRemove ("a|b|a", "a"));   // one at a time
  EXPECT_EQ (std::string ("a"), proto::EntityListRemove ("a", "x"));
  EXPECT_EQ (std::string (""), proto::EntityListRemove ("a", "a"));
}

TEST (ProtoMxp, EntityAddAndRemoveTellPlugins)
{
  World w (true);
  CPlugin * p = LoadPlugin (w.doc, "aaaaaaaaaaaaaaaaaaaaaa03",
    "function OnPluginMXPsetEntity (s) SetVariable ('last', s) end\n");
  ASSERT_TRUE (p != nullptr);

  w.feed (kSecure + "<!ENTITY inv sword>");
  EXPECT_EQ (std::string ("inv=sword"), PluginVar (p, "last"));
  w.feed (kSecure + "<!ENTITY inv shield ADD>");
  EXPECT_EQ (std::string ("inv=sword|shield"), PluginVar (p, "last"));
  w.feed (kSecure + "<!ENTITY inv sword REMOVE>");
  EXPECT_EQ (std::string ("inv=shield"), PluginVar (p, "last"));

  CString value;
  EXPECT_TRUE (w.doc.m_CustomEntityMap.Lookup ("inv", value));
  EXPECT_EQ (CString ("shield"), value);
}

TEST (ProtoMxp, QuowsVitalsAreAsTheyWere)
{
  World w (true);
  CPlugin * p = LoadPlugin (w.doc, "aaaaaaaaaaaaaaaaaaaaaa04",
    "function OnPluginMXPsetEntity (s) SetVariable ('last', s) end\n");
  ASSERT_TRUE (p != nullptr);
  // names are lower-cased, as plugins have always been told them
  w.feed (kSecure + "<!ENTITY Hp '1,234'>");
  EXPECT_EQ (std::string ("hp=1,234"), PluginVar (p, "last"));
}

TEST (ProtoMxp, EntityAttributes)
{
  World w (true);
  w.feed (kSecure + "<!ENTITY hp 5 DESC=\"hit points\" PUBLISH>");
  w.feed (kSecure + "<!ENTITY secret 6 PRIVATE>");

  VARIANT v = w.doc.MXPEntityInfo ("hp", 2);
  EXPECT_EQ ((int) VT_BSTR, (int) v.vt);
  VariantClear (&v);

  v = w.doc.MXPEntityInfo ("hp", 3);
  EXPECT_TRUE (v.boolVal != 0);
  VariantClear (&v);

  v = w.doc.MXPEntityInfo ("secret", 1);
  EXPECT_EQ ((int) VT_NULL, (int) v.vt);      // PRIVATE means the client does not tell
  VariantClear (&v);

  v = w.doc.MXPEntityInfo ("hp", 1);
  EXPECT_EQ ((int) VT_BSTR, (int) v.vt);
  VariantClear (&v);

  EXPECT_EQ (std::string ("hit points"), w.doc.m_pProto->entities ["hp"].description);
  EXPECT_TRUE (w.doc.m_pProto->entities ["hp"].bPublish);
  EXPECT_TRUE (w.doc.m_pProto->entities ["secret"].bPrivate);

  // and an entity can be deleted by keyword with nothing that looks like a value
  w.feed (kSecure + "<!ENTITY hp DELETE>");
  CString value;
  EXPECT_FALSE (w.doc.m_CustomEntityMap.Lookup ("hp", value));
  EXPECT_EQ (0u, w.doc.m_pProto->entities.count ("hp"));
}

TEST (ProtoMxp, NumericEntitiesByWorldEncoding)
{
  // UTF-8 world: a character is its bytes
  {
  World w (true);
  w.doc.m_bUTF_8 = true;
  EXPECT_EQ (std::string ("\xC3\xA9"), std::string ((const char *) w.doc.MXP_GetEntity (CString ("#233") = CString ("#233"))));
  CString name = "#8364";
  EXPECT_EQ (std::string ("\xE2\x82\xAC"), std::string ((const char *) w.doc.MXP_GetEntity (name)));
  name = "#x1F600";
  EXPECT_EQ (std::string ("\xF0\x9F\x98\x80"), std::string ((const char *) w.doc.MXP_GetEntity (name)));
  name = "#xD800";        // a surrogate is no character
  EXPECT_EQ (std::string (), std::string ((const char *) w.doc.MXP_GetEntity (name)));
  name = "#1114112";      // past the end of Unicode
  EXPECT_EQ (std::string (), std::string ((const char *) w.doc.MXP_GetEntity (name)));
  name = "#4294967361";   // wraps round to 65, if you are not careful
  EXPECT_EQ (std::string (), std::string ((const char *) w.doc.MXP_GetEntity (name)));
  name = "eacute";
  EXPECT_EQ (std::string ("\xC3\xA9"), std::string ((const char *) w.doc.MXP_GetEntity (name)));
  w.feed ("&#233;x");
  EXPECT_EQ (std::string ("\xC3\xA9x"), std::string (w.doc.m_pCurrentLine->text, w.doc.m_pCurrentLine->len));
  }
  // other worlds: the byte, as ever
  {
  World w (true);
  w.doc.m_bUTF_8 = false;
  CString name = "#233";
  EXPECT_EQ (std::string ("\xE9"), std::string ((const char *) w.doc.MXP_GetEntity (name)));
  name = "#65";
  EXPECT_EQ (std::string ("A"), std::string ((const char *) w.doc.MXP_GetEntity (name)));
  name = "#x41";
  EXPECT_EQ (std::string ("A"), std::string ((const char *) w.doc.MXP_GetEntity (name)));
  name = "#7";
  EXPECT_EQ (std::string (), std::string ((const char *) w.doc.MXP_GetEntity (name)));
  name = "#x";
  EXPECT_EQ (std::string (), std::string ((const char *) w.doc.MXP_GetEntity (name)));
  name = "#12a";
  EXPECT_EQ (std::string (), std::string ((const char *) w.doc.MXP_GetEntity (name)));
  }
}

TEST (ProtoMxp, Strikeout)
{
  for (const char * tag : { "s", "strike", "strikeout" })
    {
    World w (true);
    w.feed (std::string ("a<") + tag + ">b</" + tag + ">c");
    EXPECT_EQ (std::string ("abc"), std::string (w.doc.m_pCurrentLine->text, w.doc.m_pCurrentLine->len));
    EXPECT_TRUE ((w.styleAt (1)->iFlags & STRIKEOUT) != 0);
    EXPECT_TRUE ((w.styleAt (2)->iFlags & STRIKEOUT) == 0);
    }
}

TEST (ProtoMxp, SmallTtAndHeadings)
{
  World w (true);
  w.doc.m_bCustom16isDefaultColour = false;
  // tt changes nothing, because everything already is, and small only dims
  w.feed ("a<tt>b</tt><small>c</small>");
  EXPECT_EQ (std::string ("abc"), std::string (w.doc.m_pCurrentLine->text, w.doc.m_pCurrentLine->len));
  EXPECT_EQ (w.styleAt (0)->iForeColour, w.styleAt (1)->iForeColour);
  COLORREF fore1, back1, fore2, back2;
  w.doc.GetStyleRGB (w.styleAt (1), fore1, back1);
  w.doc.GetStyleRGB (w.styleAt (2), fore2, back2);
  EXPECT_NE (fore1, fore2);
  EXPECT_EQ (back1, back2);

  World h (true);
  h.feed (kSecure + "<H3>title</H3>");
  EXPECT_TRUE ((h.styleAt (0)->iFlags & HILITE) != 0);
}

TEST (ProtoMxp, NoBreakJoinsLines)
{
  World w (true);
  w.feed (kSecure + "one<NOBR>\r\ntwo\r\nthree");
  EXPECT_TRUE (w.allOutput ().find ("onetwo\n") != std::string::npos);
  EXPECT_TRUE (w.allOutput ().find ("three") != std::string::npos);
}

TEST (ProtoMxp, SoftBreakIsASpaceToWrapAt)
{
  World w (true);
  w.feed (kSecure + "one<SBR>two");
  EXPECT_EQ (std::string ("one two"), std::string (w.doc.m_pCurrentLine->text, w.doc.m_pCurrentLine->len));
}

TEST (ProtoMxp, AHint)
{
  World w (true);
  w.feed (kSecure + "<A href=\"http://a.b/\" hint=\"a place\">x</A>");
  // a closed tag has moved on to its finished style
  bool bFound = false;
  for (POSITION pos = w.doc.m_pCurrentLine->styleList.GetHeadPosition (); pos; )
    {
    CStyle * style = w.doc.m_pCurrentLine->styleList.GetNext (pos);
    if (style->pAction && style->pAction->m_strAction == "http://a.b/" &&
        style->pAction->m_strHint == "a place")
      bFound = true;
    }
  EXPECT_TRUE (bFound);
}

TEST (ProtoMxp, FontFaceAndSizeDoNotUpsetTheGrid)
{
  World w (true);
  w.feed ("a<FONT FACE=Arial SIZE=24 COLOR=red>b</FONT>c");
  EXPECT_EQ (std::string ("abc"), std::string (w.doc.m_pCurrentLine->text, w.doc.m_pCurrentLine->len));
}

TEST (ProtoMxp, Expire)
{
  World w (true);
  w.feed (kSecure + "<SEND href=\"go\" expire=room>north</SEND> <SEND href=\"stay\">here</SEND>\r\n");
  w.feed (kSecure + "<A href=\"http://a.b/\" expire=other>x</A>\r\n");

  const auto linkCount = [&w] (const char * action)
    {
    int n = 0;
    for (POSITION lp = w.doc.m_LineList.GetHeadPosition (); lp; )
      {
      CLine * line = w.doc.m_LineList.GetNext (lp);
      for (POSITION sp = line->styleList.GetHeadPosition (); sp; )
        {
        CStyle * style = line->styleList.GetNext (sp);
        if (style->pAction && (style->iFlags & ACTIONTYPE) && style->pAction->m_strAction == action)
          n++;
        }
      }
    return n;
    };

  EXPECT_GT (linkCount ("go"), 0);
  EXPECT_GT (linkCount ("stay"), 0);
  EXPECT_GT (linkCount ("http://a.b/"), 0);

  // the wrong name does nothing, the right one kills only its own
  w.feed (kSecure + "<EXPIRE nothing>");
  EXPECT_GT (linkCount ("go"), 0);
  w.feed (kSecure + "<EXPIRE room>");
  EXPECT_EQ (0, linkCount ("go"));
  EXPECT_GT (linkCount ("stay"), 0);
  EXPECT_GT (linkCount ("http://a.b/"), 0);

  // no name kills every named one, and never one with no name
  w.feed (kSecure + "<EXPIRE>");
  EXPECT_EQ (0, linkCount ("http://a.b/"));
  EXPECT_GT (linkCount ("stay"), 0);
}

TEST (ProtoMxp, ExpireOnlyWhereSecure)
{
  World w (true);
  w.feed (kSecure + "<SEND href=\"go\" expire=room>north</SEND>\r\n");
  // open mode, which another player's text might be
  w.feed ("<EXPIRE room>\r\n");
  EXPECT_FALSE (w.doc.m_pProto->expireOldest.empty ());
}

TEST (ProtoMxp, SupportsIsTheTruth)
{
  World w (true);
  const auto ask = [&w] (const char * what)
    {
    w.takeSent ();
    w.feed (kSecure + "<SUPPORT " + what + ">");
    return w.takeSent ();
    };

  // what we now have
  for (const char * tag : { "h1", "h6", "s", "strike", "strikeout", "small", "tt", "nobr", "sbr",
                            "expire", "gauge", "stat", "frame", "dest", "image", "sound", "music",
                            "relocate", "send", "a", "a.hint", "a.expire", "send.expire",
                            "dest.eol", "sound.p", "music.c" })
    EXPECT_TRUE (ask (tag).find (std::string ("+") + tag) != std::string::npos);

  // and what we say we don't
  for (const char * tag : { "filter", "script", "center", "nonsense", "font.face", "font.size" })
    EXPECT_TRUE (ask (tag).find (std::string ("-") + tag) != std::string::npos);

  // every tag claimed has an action behind it, for every one not marked otherwise
  w.takeSent ();
  w.feed (kSecure + "<SUPPORT>");
  EXPECT_TRUE (w.takeSent ().find ("+filter") == std::string::npos);
}

TEST (ProtoMxp, ATagAloneInAPacketStillAppliesToTheNext)
{
  World w (true);
  w.feed ("<b>");
  w.feed ("bold");
  EXPECT_TRUE ((w.styleAt (0)->iFlags & HILITE) != 0);
}

TEST (ProtoMxp, VersionIsTheSpecs)
{
  World w (true);
  w.feed (kSecure + "<VERSION>");
  EXPECT_TRUE (w.takeSent ().find ("MXP=\"1.0\"") != std::string::npos);
}

// line tags ---

TEST (ProtoMxpLineTag, RoomNameReachesPluginAndLineInfo)
{
  World w (true);
  CPlugin * p = LoadPlugin (w.doc, "aaaaaaaaaaaaaaaaaaaaaa05",
    "function OnPluginMXPflag (mode, s) SetVariable ('flag', mode .. ':' .. s) end\n");
  ASSERT_TRUE (p != nullptr);

  w.feed ("\x1B[10zThe Square\r\n");
  EXPECT_EQ (std::string ("10:roomname=The Square"), PluginVar (p, "flag"));

  CLine * line = w.lastFinishedLine ();
  ASSERT_TRUE (line != nullptr);
  EXPECT_EQ (std::string ("The Square"), std::string (line->text, line->len));
  EXPECT_EQ (10, (int) line->m_iMXPLineTag);

  const long lineNumber = w.doc.m_LineList.GetCount () - 1;
  VARIANT v = w.doc.GetLineInfo (lineNumber, 14);
  EXPECT_EQ (10, (int) v.iVal);
  VariantClear (&v);

  // and the next line is not tagged
  w.feed ("plain\r\n");
  EXPECT_EQ (0, (int) w.lastFinishedLine ()->m_iMXPLineTag);
}

TEST (ProtoMxpLineTag, TheModeIsUnchanged)
{
  World w (true);
  w.feed ("\x1B[11z<b>bold</b>\r\n");
  EXPECT_EQ (std::string ("bold"), std::string (w.lastFinishedLine ()->text, w.lastFinishedLine ()->len));
  EXPECT_EQ ((int) eMXP_open, w.doc.m_iMXP_mode);
}

TEST (ProtoMxpLineTag, FlagElements)
{
  World w (true);
  CPlugin * p = LoadPlugin (w.doc, "aaaaaaaaaaaaaaaaaaaaaa06",
    "function OnPluginMXPflag (mode, s) SetVariable ('flag', mode .. ':' .. s) end\n"
    "function OnPluginMXPsetVariable (s) SetVariable ('var', s) end\n");
  ASSERT_TRUE (p != nullptr);

  w.feed (kSecure + "<!ELEMENT RName FLAG=RoomName>");
  w.feed (kSecure + "<RName>The Square</RName>\r\n");
  EXPECT_EQ (std::string ("10:roomname=The Square"), PluginVar (p, "flag"));
  EXPECT_EQ (std::string ("mxp_roomname=The Square"), PluginVar (p, "var"));   // as before
  EXPECT_EQ (10, (int) w.lastFinishedLine ()->m_iMXPLineTag);

  // Set x: a variable, and no mapper tag
  w.feed (kSecure + "<!ELEMENT Hp FLAG=\"Set hp\">");
  w.feed (kSecure + "<Hp>100</Hp>\r\n");
  EXPECT_EQ (std::string ("0:hp=100"), PluginVar (p, "flag"));
  EXPECT_EQ (0, (int) w.lastFinishedLine ()->m_iMXPLineTag);
}

TEST (ProtoMxpLineTag, UserTagAppliesItsElement)
{
  World w (true);
  w.feed (kSecure + "<!ELEMENT Chat '<B>' TAG=20>");
  w.feed ("\x1B[20zhello\r\n");
  CLine * line = w.lastFinishedLine ();
  EXPECT_EQ (std::string ("hello"), std::string (line->text, line->len));
  EXPECT_EQ (20, (int) line->m_iMXPLineTag);

  bool bBold = false;
  for (POSITION pos = line->styleList.GetHeadPosition (); pos; )
    {
    CStyle * style = line->styleList.GetNext (pos);
    if (style->iLength && (style->iFlags & HILITE))
      bBold = true;
    }
  EXPECT_TRUE (bBold);

  // closed with its line
  EXPECT_TRUE (w.doc.m_ActiveTagList.IsEmpty ());
  w.feed ("next\r\n");
  bBold = false;
  line = w.lastFinishedLine ();
  for (POSITION pos = line->styleList.GetHeadPosition (); pos; )
    {
    CStyle * style = line->styleList.GetNext (pos);
    if (style->iLength && (style->iFlags & HILITE))
      bBold = true;
    }
  EXPECT_FALSE (bBold);
}

TEST (ProtoMxpLineTag, BangTagGagsColoursDisables)
{
  // gag
  {
  World w (true);
  w.feed (kSecure + "<!TAG 21 GAG>");
  w.feed ("\x1B[21zsecret\r\nvisible\r\n");
  EXPECT_TRUE (w.allOutput ().find ("secret") == std::string::npos);
  EXPECT_TRUE (w.allOutput ().find ("visible") != std::string::npos);
  }
  // colour
  {
  World w (true);
  w.feed (kSecure + "<!TAG 22 FORE=#FF0000 BACK=#000080>");
  w.feed ("\x1B[22zred");
  CStyle * style = w.styleAt (0);
  EXPECT_EQ (COLOUR_RGB, style->iFlags & COLOURTYPE);
  EXPECT_EQ ((COLORREF) RGB (255, 0, 0), style->iForeColour);
  EXPECT_EQ ((COLORREF) RGB (0, 0, 128), style->iBackColour);
  }
  // disable
  {
  World w (true);
  w.feed (kSecure + "<!TAG 23 GAG>");
  w.feed (kSecure + "<!TAG 23 DISABLE>");
  w.feed ("\x1B[23zshown\r\n");
  EXPECT_TRUE (w.allOutput ().find ("shown") != std::string::npos);
  w.feed (kSecure + "<!TAG 23 ENABLE>");
  w.feed ("\x1B[23zgone\r\n");
  EXPECT_TRUE (w.allOutput ().find ("gone") == std::string::npos);
  }
  // not a number, or the wrong one
  {
  World w (true);
  w.feed (kSecure + "<!TAG 5 GAG>");
  w.feed (kSecure + "<!TAG xx GAG>");
  EXPECT_TRUE (w.doc.m_pProto->lineTags.empty ());
  }
  // a definition is not for open mode
  {
  World w (true);
  w.feed ("<!TAG 24 GAG>\r\n");
  EXPECT_TRUE (w.doc.m_pProto->lineTags.empty ());
  }
}

TEST (ProtoMxpLineTag, WindowNameRedirects)
{
  World w (true);
  w.doc.m_bMXPWindows = true;
  w.feed (kSecure + "<!TAG 25 WINDOWNAME=chat>");
  w.feed ("\x1B[25zhello there\r\nmain\r\n");
  EXPECT_TRUE (w.allOutput ().find ("hello") == std::string::npos);
  EXPECT_TRUE (w.allOutput ().find ("main") != std::string::npos);
  ASSERT_EQ (1u, w.doc.m_pProto->frames.count ("chat"));
  EXPECT_EQ (std::string ("hello there"), w.doc.m_pProto->frames ["chat"]->RowText (0));
}

// bindings ---

TEST (ProtoMxp, GaugeAndStatBindings)
{
  World w (true);
  w.doc.m_bMXPWindows = false;      // the bindings without the window
  w.feed (kSecure + "<GAUGE hp Max=maxhp Caption=Life Color=red>");
  w.feed (kSecure + "<STAT mp Max=maxmp Caption=Mana>");
  w.feed (kSecure + "<GAUGE hp Max=maxhp Caption=Health>");    // replaces, not adds

  ASSERT_EQ (2u, w.doc.m_pProto->bindings.size ());
  EXPECT_TRUE (w.doc.m_pProto->bindings [0].bGauge);
  EXPECT_EQ (std::string ("hp"), w.doc.m_pProto->bindings [0].entity);
  EXPECT_EQ (std::string ("maxhp"), w.doc.m_pProto->bindings [0].maxEntity);
  EXPECT_EQ (std::string ("Health"), w.doc.m_pProto->bindings [0].caption);
  EXPECT_FALSE (w.doc.m_pProto->bindings [1].bGauge);

  VARIANT v = w.doc.MXPEntityInfo ("", 6);
  EXPECT_EQ ((int) VT_BSTR, (int) v.vt);
  VariantClear (&v);
}

TEST (ProtoMxp, APluginCanHaveTheTag)
{
  World w (true);
  w.doc.m_bMXPWindows = false;
  CPlugin * p = LoadPlugin (w.doc, "aaaaaaaaaaaaaaaaaaaaaa07",
    "function OnPluginMXPopenTag (s) return not string.match (s, '^gauge') end\n");
  ASSERT_TRUE (p != nullptr);
  w.doc.m_bPluginProcessesOpenTag = true;
  w.feed (kSecure + "<GAUGE hp>");
  EXPECT_TRUE (w.doc.m_pProto->bindings.empty ());
}

// frames ---

TEST (ProtoFrame, TheGrid)
{
  CMxpFrame f;
  f.Put ("a", 1, 0, 0);
  f.Put ("b", 1, 0, 0);
  f.NewLine ();
  f.Put ("c", 1, 0, 0);
  EXPECT_EQ (std::string ("ab"), f.RowText (0));
  EXPECT_EQ (std::string ("c"), f.RowText (1));

  f.MoveTo (1, 0);
  f.Put ("X", 1, 0, 0);
  EXPECT_EQ (std::string ("aX"), f.RowText (0));

  f.MoveTo (5, 0);
  f.Put ("Y", 1, 0, 0);
  EXPECT_EQ (std::string ("aX   Y"), f.RowText (0));

  f.MoveTo (1, 0);
  f.EraseToEndOfLine ();
  EXPECT_EQ (std::string ("a"), f.RowText (0));

  f.Clear ();
  EXPECT_EQ (std::string (), f.RowText (0));

  // numbers from a server
  f.MoveTo (100000, 100000);
  f.Put ("Z", 1, 0, 0);
  EXPECT_LE (f.rows.size (), CMxpFrame::kMaxRows);
  for (size_t i = 0; i < 2 * CMxpFrame::kMaxRows; i++)
    f.NewLine ();
  EXPECT_LE (f.row, CMxpFrame::kMaxRows - 1);
}

TEST (ProtoFrame, Dimensions)
{
  EXPECT_EQ (100, proto::FrameDimension ("100", 800, 8));
  EXPECT_EQ (400, proto::FrameDimension ("50%", 800, 8));
  EXPECT_EQ (160, proto::FrameDimension ("20c", 800, 8));
  EXPECT_EQ (-100, proto::FrameDimension ("-100", 800, 8));
  EXPECT_EQ (0, proto::FrameDimension ("", 800, 8));
  EXPECT_EQ (16384, proto::FrameDimension ("99999999999", 800, 8));
}

TEST (ProtoFrame, DestSendsTextThere)
{
  World w (true);
  w.doc.m_bMXPWindows = true;
  w.feed (kSecure + "<FRAME Name=Status Left=0 Top=0 Width=200 Height=100>");
  ASSERT_EQ (1u, w.doc.m_pProto->frames.count ("status"));

  w.feed (kSecure + "before<DEST Status>inside</DEST>after");
  EXPECT_EQ (std::string ("beforeafter"), std::string (w.doc.m_pCurrentLine->text, w.doc.m_pCurrentLine->len));
  EXPECT_EQ (std::string ("inside"), w.doc.m_pProto->frames ["status"]->RowText (0));
  EXPECT_TRUE (w.doc.m_pProto->destStack.empty ());

  // addressing
  w.feed (kSecure + "<DEST Status X=2 Y=1>xy</DEST>");
  EXPECT_EQ (std::string ("  xy"), w.doc.m_pProto->frames ["status"]->RowText (1));

  // Y and no text erases the line
  w.feed (kSecure + "<DEST Status Y=1></DEST>");
  EXPECT_EQ (std::string (), w.doc.m_pProto->frames ["status"]->RowText (1));

  // no text, no keywords, erases the frame
  w.feed (kSecure + "<DEST Status></DEST>");
  EXPECT_EQ (std::string (), w.doc.m_pProto->frames ["status"]->RowText (0));

  // a frame that isn't there: the main window, not the bit bucket
  w.feed (kSecure + "<DEST Nowhere>seen</DEST>");
  EXPECT_TRUE (w.allOutput ().find ("seen") != std::string::npos);

  // close
  w.feed (kSecure + "<FRAME Name=Status Action=Close>");
  EXPECT_EQ (0u, w.doc.m_pProto->frames.count ("status"));
}

TEST (ProtoFrame, RedirectAndBack)
{
  World w (true);
  w.doc.m_bMXPWindows = true;
  w.feed (kSecure + "<FRAME Name=Log Action=Redirect>");
  w.feed ("all of this\r\nand this\r\n");
  EXPECT_EQ (std::string ("all of this"), w.doc.m_pProto->frames ["log"]->RowText (0));
  EXPECT_EQ (std::string ("and this"), w.doc.m_pProto->frames ["log"]->RowText (1));
  EXPECT_TRUE (w.allOutput ().find ("all of this") == std::string::npos);

  w.feed (kSecure + "<FRAME Name=_top Action=Redirect>");
  w.feed ("back\r\n");
  EXPECT_TRUE (w.allOutput ().find ("back") != std::string::npos);
}

TEST (ProtoFrame, TheOptionTurnsItOffAndTextStaysInMain)
{
  World w (true);
  w.doc.m_bMXPWindows = false;
  w.feed (kSecure + "<FRAME Name=Status>");
  w.feed (kSecure + "<DEST Status>inside</DEST>");
  EXPECT_TRUE (w.doc.m_pProto->frames.empty ());
  EXPECT_TRUE (w.allOutput ().find ("inside") != std::string::npos);
}

TEST (ProtoFrame, AHostileServerGetsOnlySoManyFrames)
{
  World w (true);
  w.doc.m_bMXPWindows = true;
  for (int i = 0; i < 40; i++)
    w.feed (kSecure + "<FRAME Name=f" + std::to_string (i) + ">");
  EXPECT_LE (w.doc.m_pProto->frames.size (), 8u);
  w.feed (kSecure + "<FRAME Name=\"..\\bad\">");
  EXPECT_EQ (0u, w.doc.m_pProto->frames.count ("..\\bad"));
}

TEST (ProtoFrame, ResetEndsDiversion)
{
  World w (true);
  w.doc.m_bMXPWindows = true;
  w.feed (kSecure + "<FRAME Name=Log Action=Redirect>");
  w.feed (kSecure + "<reset>");
  w.feed ("main again");
  EXPECT_TRUE (w.allOutput ().find ("main again") != std::string::npos);
}

TEST (ProtoMxp, ImageIsStillALinkAndFetchesNothing)
{
  World w (true);
  w.feed (kSecure + "<IMAGE fname=\"map.png\" URL=\"http://a.b/\">");
  EXPECT_TRUE (w.allOutput ().find ("[http://a.b/map.png]") != std::string::npos);
  EXPECT_TRUE (w.doc.m_pProto->imageFile.empty ());
  EXPECT_TRUE (w.doc.m_pProto->mspInFlight.empty ());

  // nor can it name something outside the cache
  bool bShown = true;
  Proto_MxpImage (w.doc, "../../../secret.png", "", "", &bShown);
  EXPECT_FALSE (bShown);
  Proto_MxpImage (w.doc, "c:/windows/x.png", "", "", &bShown);
  EXPECT_FALSE (bShown);
}

// RELOCATE ---

TEST (ProtoMxp, RelocateAsksAndNeverSendsThePassword)
{
  World w (true);
  w.doc.m_name = "Bob";
  w.doc.m_password = "hunter2";
  w.doc.m_connect_now = eConnectMXP;
  w.doc.m_server = "first.example";
  w.doc.m_port = 4000;

  w.feed (kSecure + "<RELOCATE other.example 5000>\r\n");

  // a question, and nothing done
  EXPECT_TRUE (w.allOutput ().find ("other.example, port 5000") != std::string::npos);
  EXPECT_EQ (CString ("first.example"), w.doc.m_server);
  EXPECT_EQ (4000, (int) w.doc.m_port);
  EXPECT_FALSE (w.t->closed);

  // and what usually follows gets nothing
  w.takeSent ();
  w.feed (kSecure + "<USER>");
  w.feed (kSecure + "<PASSWORD>");
  const std::string sent = w.takeSent ();
  EXPECT_TRUE (sent.find ("hunter2") == std::string::npos);
  EXPECT_TRUE (sent.find ("Bob") == std::string::npos);

  EXPECT_TRUE (w.doc.m_pProto->bRelocateAsked);
}

TEST (ProtoMxp, RelocateCannotBeNonsense)
{
  World w (true);
  w.feed (kSecure + "<RELOCATE \"evil host\" 5000>\r\n");
  EXPECT_TRUE (w.doc.m_pProto->relocateHost.empty ());
  w.feed (kSecure + "<RELOCATE host.example 70000>\r\n");
  EXPECT_TRUE (w.doc.m_pProto->relocateHost.empty ());
}

TEST (ProtoLinks, AServerCannotClickOrForgeTheActionLinks)
{
  World w;
  w.doc.m_bUseSSL = false;
  const std::string action = Proto_ActionLink ("tls_never", "");

  // on a line from the server, that is just text to send
  w.takeSent ();
  w.doc.HyperlinkClicked (action.c_str (), ACTION_SEND, false);
  EXPECT_TRUE (w.doc.m_bMsspTlsHint);
  EXPECT_TRUE (w.takeSent ().find ("tls_never") != std::string::npos);

  // on a note line it is what it says
  w.doc.HyperlinkClicked (action.c_str (), ACTION_SEND, true);
  EXPECT_FALSE (w.doc.m_bMsspTlsHint);
}

// ------------------------------------------------------------- MSP: parser

TEST (ProtoMsp, ParsingTriggers)
{
  proto::MspTrigger t;
  EXPECT_TRUE (proto::ParseMspTrigger ("punch.wav", false, &t));
  EXPECT_EQ (std::string ("punch.wav"), t.name);
  EXPECT_EQ (100, t.volume);
  EXPECT_EQ (1, t.repeats);
  EXPECT_EQ (50, t.priority);
  EXPECT_EQ (1, t.cont);

  EXPECT_TRUE (proto::ParseMspTrigger ("a/b.wav V=30 L=2 P=80 T=combat U=http://x/y/", false, &t));
  EXPECT_EQ (std::string ("a/b.wav"), t.name);
  EXPECT_EQ (30, t.volume);
  EXPECT_EQ (2, t.repeats);
  EXPECT_EQ (80, t.priority);
  EXPECT_EQ (std::string ("combat"), t.type);
  EXPECT_EQ (std::string ("http://x/y/"), t.url);

  EXPECT_TRUE (proto::ParseMspTrigger ("m.mid L=-1 C=0", true, &t));
  EXPECT_EQ (-1, t.repeats);
  EXPECT_EQ (0, t.cont);

  // out of range is brought in, junk is the default
  EXPECT_TRUE (proto::ParseMspTrigger ("x V=500 L=0 P=-5", false, &t));
  EXPECT_EQ (100, t.volume);
  EXPECT_EQ (1, t.repeats);
  EXPECT_EQ (0, t.priority);
  EXPECT_TRUE (proto::ParseMspTrigger ("x V=loud L=9999999999", false, &t));
  EXPECT_EQ (100, t.volume);

  EXPECT_TRUE (proto::ParseMspTrigger ("Off", false, &t));
  EXPECT_TRUE (t.bOff);
  EXPECT_TRUE (proto::ParseMspTrigger ("off U=http://x/", true, &t));
  EXPECT_TRUE (t.bOff);
  EXPECT_EQ (std::string ("http://x/"), t.url);

  EXPECT_TRUE (proto::ParseMspTrigger ("x U=\"http://x/a(b)/\"", false, &t));
  EXPECT_EQ (std::string ("http://x/a(b)/"), t.url);

  EXPECT_FALSE (proto::ParseMspTrigger ("", false, &t));
  EXPECT_FALSE (proto::ParseMspTrigger ("   ", false, &t));
}

TEST (ProtoMsp, NamesThatMustNotPass)
{
  for (const char * bad : {
         "", "..", "../x.wav", "a/../x.wav", "a/..", "./x.wav", "a/./x.wav", "/x.wav", "//x.wav",
         "a//x.wav", "x.wav/", "c:/x.wav", "C:x.wav", "\\\\server\\x.wav", "a\\b.wav", "..\\x.wav",
         "x.wav:stream", "x.wav::$DATA", "a:b/x.wav",
         "CON", "con.wav", "NUL", "nul.anything", "prn.wav", "aux.wav", "COM1.wav", "com9", "LPT1",
         "lpt3.mp3", "a/CON/x.wav", "conin$", "CONOUT$.wav",
         "x.wav.", "x.wav ", " x.wav", "a /x.wav", "a./x.wav",
         "x\tb.wav", "x\nb.wav", "x\x01.wav", "caf\xC3\xA9.wav", "x%2e.wav", "x|y.wav", "x>y", "x\"y",
         "a/b/c/d/e/f/g/h/i.wav" })
    EXPECT_FALSE (proto::IsSafeMspName (bad, false));

  EXPECT_FALSE (proto::IsSafeMspName (std::string (201, 'a'), false));

  for (const char * good : { "x.wav", "x", "combat/punch.wav", "a/b/c.wav", "Weapons-2/sword_1.WAV",
                             "console.wav", "comet.wav", "com10.wav", "lpt.wav", "a b.wav", "x+y.wav", "n(1).mp3" })
    EXPECT_TRUE (proto::IsSafeMspName (good, false));

  // wildcards only where allowed, and only one *
  EXPECT_FALSE (proto::IsSafeMspName ("x*.wav", false));
  EXPECT_TRUE (proto::IsSafeMspName ("x*.wav", true));
  EXPECT_TRUE (proto::IsSafeMspName ("a/x?.wav", true));
  EXPECT_FALSE (proto::IsSafeMspName ("x**.wav", true));
  EXPECT_FALSE (proto::IsSafeMspName ("x*y*.wav", true));
  EXPECT_FALSE (proto::IsSafeMspName ("a*/x.wav", true));
  EXPECT_FALSE (proto::IsSafeMspName ("../x*.wav", true));
}

TEST (ProtoMsp, Wildcards)
{
  EXPECT_TRUE (proto::MspWildcardMatch ("punch*", "punch1.wav"));
  EXPECT_TRUE (proto::MspWildcardMatch ("punch*.wav", "PUNCH1.WAV"));
  EXPECT_TRUE (proto::MspWildcardMatch ("punch?.wav", "punch1.wav"));
  EXPECT_FALSE (proto::MspWildcardMatch ("punch?.wav", "punch12.wav"));
  EXPECT_FALSE (proto::MspWildcardMatch ("punch*.wav", "kick.wav"));
  EXPECT_TRUE (proto::MspWildcardMatch ("*", "anything"));
  EXPECT_FALSE (proto::MspWildcardMatch ("ab*ab", "aba"));
  EXPECT_TRUE (proto::MspWildcardMatch ("x.wav", "X.WAV"));
}

TEST (ProtoMsp, Extensions)
{
  EXPECT_EQ (std::string ("x.wav"), proto::MspWithDefaultExtension ("x", false));
  EXPECT_EQ (std::string ("x.mid"), proto::MspWithDefaultExtension ("x", true));
  EXPECT_EQ (std::string ("x.mp3"), proto::MspWithDefaultExtension ("x.mp3", true));
  EXPECT_EQ (std::string ("a.b/x.wav"), proto::MspWithDefaultExtension ("a.b/x", false));
  EXPECT_EQ (std::string ("x*"), proto::MspWithDefaultExtension ("x*", false));
  EXPECT_TRUE (proto::IsMidiName ("X.MID"));
  EXPECT_TRUE (proto::IsDownloadableAudioName ("a/x.WAV"));
  EXPECT_FALSE (proto::IsDownloadableAudioName ("x.exe"));
  EXPECT_FALSE (proto::IsDownloadableAudioName ("x.wav.exe"));
  EXPECT_FALSE (proto::IsDownloadableAudioName ("x"));
  EXPECT_FALSE (proto::IsDownloadableAudioName ("x.mid"));
}

TEST (ProtoMsp, WhatAudioLooksLike)
{
  EXPECT_TRUE (proto::LooksLikeAudio ("x.wav", std::string ("RIFF\0\0\0\0WAVEfmt ", 16)));
  EXPECT_FALSE (proto::LooksLikeAudio ("x.wav", "MZ this is a program"));
  EXPECT_TRUE (proto::LooksLikeAudio ("x.mp3", "ID3 and a long enough tail"));
  EXPECT_TRUE (proto::LooksLikeAudio ("x.mp3", std::string ("\xFF\xFB\x90\x00........", 12)));
  EXPECT_TRUE (proto::LooksLikeAudio ("x.flac", "fLaC and a long enough tail"));
  EXPECT_FALSE (proto::LooksLikeAudio ("x.flac", "ID3 and a long enough tail"));
  EXPECT_FALSE (proto::LooksLikeAudio ("x.wav", "RIFF"));
  EXPECT_FALSE (proto::LooksLikeAudio ("x.exe", "RIFF\0\0\0\0WAVE"));
}

TEST (ProtoMsp, Volume)
{
  EXPECT_NEAR (0.0, proto::MspVolumeToDb (100), 0.001);
  EXPECT_NEAR (-6.02, proto::MspVolumeToDb (50), 0.01);
  EXPECT_NEAR (-100.0, proto::MspVolumeToDb (0), 0.001);
}

TEST (ProtoMsp, Urls)
{
  EXPECT_EQ (std::string ("http://a.b/s/x.wav"), proto::MspDownloadUrl ("http://a.b/s/", "x.wav"));
  EXPECT_EQ (std::string ("https://a.b/s/x.wav"), proto::MspDownloadUrl ("https://a.b/s", "x.wav"));
  EXPECT_EQ (std::string ("https://a.b/s/y/x.wav"), proto::MspDownloadUrl ("https://a.b/s", "y/x.wav"));
  EXPECT_EQ (std::string ("https://a.b/s/x.wav"), proto::MspDownloadUrl ("https://a.b/s/x.wav", "x.wav"));
  EXPECT_EQ (std::string (), proto::MspDownloadUrl ("ftp://a.b/", "x.wav"));
  EXPECT_EQ (std::string (), proto::MspDownloadUrl ("file:///c:/", "x.wav"));
  EXPECT_EQ (std::string (), proto::MspDownloadUrl ("http://a b/", "x.wav"));

  EXPECT_EQ (std::string ("a.b"), proto::UrlHost ("https://A.B:8443/c"));
  EXPECT_EQ (std::string ("::1"), proto::UrlHost ("http://[::1]:80/"));
  EXPECT_EQ (std::string (), proto::UrlHost ("https://user:pass@a.b/"));
  EXPECT_EQ (std::string (), proto::UrlHost ("nothing"));
}

TEST (ProtoMsp, HostLists)
{
  EXPECT_EQ (std::string ("a.b"), proto::HostListAdd ("", "A.B"));
  EXPECT_EQ (std::string ("a.b c.d"), proto::HostListAdd ("a.b", "c.d"));
  EXPECT_EQ (std::string ("a.b"), proto::HostListAdd ("a.b", "a.B"));
  EXPECT_TRUE (proto::HostListHas ("a.b c.d", "C.D"));
  EXPECT_FALSE (proto::HostListHas ("a.b c.d", "b"));
  EXPECT_FALSE (proto::HostListHas ("", "a.b"));
}

TEST (ProtoMsp, PublicAddresses)
{
  struct { const char * text; std::initializer_list <int> bytes; bool bPublic; } const v4 [] = {
    { "8.8.8.8",        { 8,8,8,8 },         true  },
    { "93.184.216.34",  { 93,184,216,34 },   true  },
    { "172.15.0.1",     { 172,15,0,1 },      true  },
    { "172.32.0.1",     { 172,32,0,1 },      true  },
    { "100.63.0.1",     { 100,63,0,1 },      true  },
    { "127.0.0.1",      { 127,0,0,1 },       false },
    { "127.255.0.1",    { 127,255,0,1 },     false },
    { "10.1.2.3",       { 10,1,2,3 },        false },
    { "192.168.1.1",    { 192,168,1,1 },     false },
    { "172.16.0.1",     { 172,16,0,1 },      false },
    { "172.31.255.1",   { 172,31,255,1 },    false },
    { "169.254.169.254",{ 169,254,169,254 }, false },
    { "100.64.0.1",     { 100,64,0,1 },      false },
    { "0.0.0.0",        { 0,0,0,0 },         false },
    { "224.0.0.1",      { 224,0,0,1 },       false },
    { "255.255.255.255",{ 255,255,255,255 }, false },
    { "198.18.0.1",     { 198,18,0,1 },      false },
  };
  for (const auto & item : v4)
    {
    unsigned char bytes [4];
    size_t i = 0;
    for (const int b : item.bytes)
      bytes [i++] = (unsigned char) b;
    EXPECT_EQ (item.bPublic, pal::IsPublicAddress (bytes, 4));
    }

  const auto v6 = [] (std::initializer_list <int> values)
    {
    unsigned char bytes [16] = { 0 };
    size_t i = 0;
    for (const int b : values)
      bytes [i++] = (unsigned char) b;
    return pal::IsPublicAddress (bytes, 16);
    };
  EXPECT_TRUE (v6 ({ 0x26,0x06,0x47,0 }));                         // an ordinary one
  EXPECT_FALSE (v6 ({ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 }));         // ::1
  EXPECT_FALSE (v6 ({ 0 }));                                       // ::
  EXPECT_FALSE (v6 ({ 0xFC,0 }));
  EXPECT_FALSE (v6 ({ 0xFD,1 }));
  EXPECT_FALSE (v6 ({ 0xFE,0x80 }));
  EXPECT_FALSE (v6 ({ 0xFF,2 }));
  // an IPv4 address in disguise
  EXPECT_FALSE (v6 ({ 0,0,0,0,0,0,0,0,0,0,0xFF,0xFF,127,0,0,1 }));
  EXPECT_FALSE (v6 ({ 0,0,0,0,0,0,0,0,0,0,0xFF,0xFF,192,168,0,1 }));
  EXPECT_TRUE  (v6 ({ 0,0,0,0,0,0,0,0,0,0,0xFF,0xFF,8,8,8,8 }));
  EXPECT_FALSE (v6 ({ 0,0x64,0xFF,0x9B,0,0,0,0,0,0,0,0,10,0,0,1 }));
  EXPECT_FALSE (v6 ({ 0x20,0x02,127,0,0,1 }));
  EXPECT_FALSE (pal::IsPublicAddress ((const unsigned char *) "abc", 3));
}

TEST (ProtoMsp, SafeGetRefusesTheLocalNetwork)
{
  pal::SafeGetOptions options;
  options.bAllowHttp = true;
  options.timeoutSeconds = 5;
  options.userAgent = "test";

  for (const char * url : { "http://127.0.0.1/x.wav", "https://127.0.0.1/x.wav", "http://localhost/x.wav",
                            "http://[::1]/x.wav", "http://10.0.0.1/x.wav", "http://192.168.0.1/x.wav",
                            "http://169.254.169.254/x.wav", "http://2130706433/x.wav" })
    {
    int status = 0;
    std::string body, error;
    EXPECT_FALSE (pal::SafeGet (url, options, &status, &body, &error));
    EXPECT_FALSE (error.empty ());
    }

  // http is an opt-in, and no other schemes
  options.bAllowHttp = false;
  int status = 0;
  std::string body, error;
  EXPECT_FALSE (pal::SafeGet ("http://8.8.8.8/x.wav", options, &status, &body, &error));
  EXPECT_TRUE (error.find ("http") != std::string::npos);
  EXPECT_FALSE (pal::SafeGet ("file:///c:/x.wav", options, &status, &body, &error));
  EXPECT_FALSE (pal::SafeGet ("https://user@8.8.8.8/x.wav", options, &status, &body, &error));
}

TEST (ProtoMsp, Containment)
{
  const std::string base = TempDir ("contain") + "/cache/";
  std::error_code ec;
  fs::create_directories (base, ec);
  const std::string inside = (fs::weakly_canonical (fs::path (base), ec) / "x.wav").string ();

  EXPECT_EQ (inside, Proto_ContainedPath (base, "x.wav"));
  EXPECT_FALSE (Proto_ContainedPath (base, "a/b/x.wav").empty ());      // need not exist
  EXPECT_TRUE (Proto_ContainedPath (base, "../x.wav").empty ());
  EXPECT_TRUE (Proto_ContainedPath (base, "a/../../x.wav").empty ());

  // a sibling whose name begins the same is not inside
  fs::create_directories (base + "../cache2", ec);
  EXPECT_TRUE (Proto_ContainedPath (base, "../cache2/x.wav").empty ());

#ifdef _WIN32
  EXPECT_TRUE (Proto_ContainedPath (base, "c:\\windows\\x.wav").empty ());
  EXPECT_TRUE (Proto_ContainedPath (base, "\\\\server\\share\\x.wav").empty ());
#else
  EXPECT_TRUE (Proto_ContainedPath (base, "/etc/passwd").empty ());
#endif
}

// ------------------------------------------------------------- MSP: the stream

namespace {

struct MspWorld : World
  {
  std::string dir;
  explicit MspWorld (const char * tag) : World ()
    {
    dir = TempDir (tag);
    doc.m_strMSPSoundDirectory = dir.c_str ();
    doc.m_pProto->bTestSound = true;
    feed (kWILL + (char) proto::kTelOptMsp);
    takeSent ();
    }
  void file (const char * name) { TouchFile (fs::path (dir) / name); }
  std::vector <std::string> played () { return doc.m_pProto->testPlayed; }
  void finishSound ()
    {
    if (doc.m_pProto->sound.buffer)
      doc.m_pProto->testBusy [doc.m_pProto->sound.buffer] = false;
    }
  };

} // anonymous namespace

TEST (ProtoMspStream, NegotiationIsOurs)
{
  World w;
  w.feed (kWILL + (char) proto::kTelOptMsp);
  EXPECT_EQ (kDO + (char) proto::kTelOptMsp, w.takeSent ());
  EXPECT_TRUE (w.doc.m_pProto->bMspNegotiated);
  EXPECT_TRUE (Proto_MspParsing (w.doc));
}

TEST (ProtoMspStream, TheOptionMeansDont)
{
  World w;
  w.doc.m_bNativeMSP = false;
  w.feed (kWILL + (char) proto::kTelOptMsp);
  EXPECT_EQ (kDONT + (char) proto::kTelOptMsp, w.takeSent ());
  EXPECT_FALSE (Proto_MspParsing (w.doc));
  w.feed ("!!SOUND(x)\r\n");
  EXPECT_TRUE (w.allOutput ().find ("!!SOUND(x)") != std::string::npos);
}

TEST (ProtoMspStream, WithoutNegotiationItIsOnlyText)
{
  World w;
  w.feed ("!!SOUND(x)\r\n");
  EXPECT_TRUE (w.allOutput ().find ("!!SOUND(x)") != std::string::npos);

  World w2;
  w2.doc.m_bMSPWithoutNegotiation = true;
  w2.doc.m_pProto->bTestSound = true;
  w2.feed ("!!SOUND(x)\r\n");
  EXPECT_TRUE (w2.allOutput ().find ("SOUND") == std::string::npos);
}

TEST (ProtoMspStream, GaggedAndPlayed)
{
  MspWorld w ("gag");
  w.file ("ouch.wav");
  w.feed ("before\r\n!!SOUND(ouch.wav)\r\nafter\r\n");
  EXPECT_EQ (1u, w.played ().size ());
  EXPECT_TRUE (w.played () [0].find ("ouch.wav") != std::string::npos);

  const std::string output = w.allOutput ();
  EXPECT_TRUE (output.find ("SOUND") == std::string::npos);
  EXPECT_EQ (std::string ("before\nafter\n"), output);       // and no blank line
}

TEST (ProtoMspStream, SplitAcrossPackets)
{
  MspWorld w ("split");
  w.file ("ouch.wav");
  w.feedSplit ("x\r\n!!SOUND(ouch.wav V=50 P=10)\r\ny\r\n");
  EXPECT_EQ (1u, w.played ().size ());
  EXPECT_EQ (50, w.doc.m_pProto->sound.volume);
  EXPECT_EQ (10, w.doc.m_pProto->sound.priority);
  EXPECT_EQ (std::string ("x\ny\n"), w.allOutput ());
}

TEST (ProtoMspStream, MidLineDoesNotFire)
{
  MspWorld w ("mid");
  w.file ("ouch.wav");
  w.feed ("Bob says: !!SOUND(ouch.wav)\r\n");
  EXPECT_TRUE (w.played ().empty ());
  EXPECT_EQ (std::string ("Bob says: !!SOUND(ouch.wav)\n"), w.allOutput ());

  // nor over a prompt that was never finished
  MspWorld w2 ("mid2");
  w2.file ("ouch.wav");
  w2.feed ("> ");
  w2.feed ("!!SOUND(ouch.wav)\r\n");
  EXPECT_TRUE (w2.played ().empty ());
}

TEST (ProtoMspStream, TheLineStartIsTheGate)
{
  // the protocol does let a line start with it, which is why only a safe name plays
  // and nothing downloads without being asked
  MspWorld w ("inject");
  w.file ("ouch.wav");
  w.feed ("!!SOUND(../../../../windows/media/tada.wav)\r\n");
  EXPECT_TRUE (w.played ().empty ());
  EXPECT_TRUE (w.doc.m_pProto->bMspBadNameNoted);
  EXPECT_TRUE (w.doc.m_pProto->mspInFlight.empty ());
}

TEST (ProtoMspStream, NotQuiteTriggersAreText)
{
  MspWorld w ("text");
  w.feed ("!!!\r\n!!SOUNDS\r\n!!SOUND\r\n!hello\r\n!!MUSICAL(x)\r\n!!SOUND(\r\n");
  EXPECT_EQ (std::string ("!!!\n!!SOUNDS\n!!SOUND\n!hello\n!!MUSICAL(x)\n!!SOUND(\n"), w.allOutput ());
  EXPECT_EQ (NONE, w.doc.m_phase);

  // and split up
  MspWorld w2 ("text2");
  w2.feedSplit ("!!SOUNDS\r\n!x\r\n");
  EXPECT_EQ (std::string ("!!SOUNDS\n!x\n"), w2.allOutput ());

  // too long to be one
  MspWorld w3 ("text3");
  const std::string longOne = "!!SOUND(" + std::string (proto::kMspMaxTrigger, 'a');
  w3.feed (longOne);
  EXPECT_EQ (NONE, w3.doc.m_phase);
  EXPECT_TRUE (w3.allOutput ().find ("!!SOUND(aaa") != std::string::npos);
  EXPECT_TRUE (w3.doc.m_pProto->testPlayed.empty ());

  // an ESC or IAC in the middle ends it
  MspWorld w4 ("text4");
  w4.feed ("!!SOUND(a\x1B[0mb)\r\n");
  EXPECT_TRUE (w4.allOutput ().find ("!!SOUND(ab)") != std::string::npos);
}

TEST (ProtoMspStream, TextAfterTheTriggerStays)
{
  MspWorld w ("after");
  w.file ("ouch.wav");
  w.feed ("!!SOUND(ouch.wav) and more\r\n");
  EXPECT_EQ (1u, w.played ().size ());
  EXPECT_EQ (std::string (" and more\n"), w.allOutput ());
}

TEST (ProtoMspStream, TriggersSeeNothing)
{
  MspWorld w ("trig");
  w.file ("ouch.wav");
  CPlugin * p = LoadPlugin (w.doc, "aaaaaaaaaaaaaaaaaaaaaa08",
    "n = 0\nfunction OnPluginLineReceived (s) n = n + 1; SetVariable ('n', n); SetVariable ('s', s) end\n");
  ASSERT_TRUE (p != nullptr);
  w.doc.m_bPluginProcessesLine = true;
  w.feed ("!!SOUND(ouch.wav)\r\nreal\r\n");
  EXPECT_EQ (std::string ("1"), PluginVar (p, "n"));
  EXPECT_EQ (std::string ("real"), PluginVar (p, "s"));
}

TEST (ProtoMspStream, PriorityRules)
{
  MspWorld w ("prio");
  w.file ("a.wav");
  w.file ("b.wav");
  w.file ("c.wav");
  w.feed ("!!SOUND(a.wav P=50)\r\n");
  EXPECT_EQ (1u, w.played ().size ());

  // lower, and the same, do not displace it
  w.feed ("!!SOUND(b.wav P=10)\r\n");
  w.feed ("!!SOUND(b.wav P=50)\r\n");
  EXPECT_EQ (1u, w.played ().size ());

  // higher does
  w.feed ("!!SOUND(c.wav P=51)\r\n");
  EXPECT_EQ (2u, w.played ().size ());
  EXPECT_TRUE (w.played () [1].find ("c.wav") != std::string::npos);

  // and once it is over anything goes
  w.finishSound ();
  w.feed ("!!SOUND(b.wav P=1)\r\n");
  EXPECT_EQ (3u, w.played ().size ());
}

TEST (ProtoMspStream, Off)
{
  MspWorld w ("off");
  w.file ("a.wav");
  w.file ("m.mp3");
  w.feed ("!!SOUND(a.wav)\r\n!!MUSIC(m.mp3)\r\n");
  EXPECT_TRUE (w.doc.m_pProto->sound.bActive);
  EXPECT_TRUE (w.doc.m_pProto->music.bActive);

  w.feed ("!!SOUND(Off)\r\n");
  EXPECT_FALSE (w.doc.m_pProto->sound.bActive);
  EXPECT_TRUE (w.doc.m_pProto->music.bActive);

  // with U= it says where things are, and stops nothing
  w.feed ("!!MUSIC(Off U=https://a.b/m/)\r\n");
  EXPECT_TRUE (w.doc.m_pProto->music.bActive);
  EXPECT_EQ (std::string ("https://a.b/m/"), w.doc.m_pProto->mspDefaultUrl);

  w.feed ("!!MUSIC(off)\r\n");
  EXPECT_FALSE (w.doc.m_pProto->music.bActive);
  EXPECT_EQ (std::string (), w.allOutput ());
}

TEST (ProtoMspStream, RepeatsCountDown)
{
  MspWorld w ("rep");
  w.file ("a.wav");
  w.feed ("!!SOUND(a.wav L=3)\r\n");
  EXPECT_EQ (1u, w.played ().size ());

  Proto_MspTick (w.doc);
  EXPECT_EQ (1u, w.played ().size ());     // still playing

  w.finishSound ();
  Proto_MspTick (w.doc);
  EXPECT_EQ (2u, w.played ().size ());
  w.finishSound ();
  Proto_MspTick (w.doc);
  EXPECT_EQ (3u, w.played ().size ());
  w.finishSound ();
  Proto_MspTick (w.doc);
  EXPECT_EQ (3u, w.played ().size ());
  EXPECT_FALSE (w.doc.m_pProto->sound.bActive);
}

TEST (ProtoMspStream, ForeverIsALoop)
{
  MspWorld w ("loop");
  w.file ("a.wav");
  w.feed ("!!SOUND(a.wav L=-1)\r\n");
  ASSERT_EQ (1u, w.played ().size ());
  EXPECT_TRUE (w.played () [0].find (" loop") != std::string::npos);
}

TEST (ProtoMspStream, MusicContinuesOrRestarts)
{
  MspWorld w ("music");
  w.file ("m.mp3");
  w.file ("n.mp3");
  w.feed ("!!MUSIC(m.mp3)\r\n");
  EXPECT_EQ (1u, w.played ().size ());

  // the same, C=1 (the default): carries on, with the new volume and repeats
  w.feed ("!!MUSIC(m.mp3 V=20 L=4)\r\n");
  EXPECT_EQ (1u, w.played ().size ());
  EXPECT_EQ (20, w.doc.m_pProto->music.volume);
  EXPECT_EQ (3, w.doc.m_pProto->music.repeatsLeft);

  // C=0: from the start
  w.feed ("!!MUSIC(m.mp3 C=0)\r\n");
  EXPECT_EQ (2u, w.played ().size ());

  // something else
  w.feed ("!!MUSIC(n.mp3)\r\n");
  EXPECT_EQ (3u, w.played ().size ());
  EXPECT_TRUE (w.played () [2].find ("n.mp3") != std::string::npos);
}

TEST (ProtoMspStream, WildcardsPickAndSubdirectoriesComeFirst)
{
  MspWorld w ("wild");
  w.file ("punch1.wav");
  w.file ("punch2.wav");
  w.file ("combat/kick.wav");
  w.file ("kick.wav");

  w.feed ("!!SOUND(punch*.wav)\r\n");
  ASSERT_EQ (1u, w.played ().size ());
  EXPECT_TRUE (w.played () [0].find ("punch") != std::string::npos);
  w.finishSound ();

  w.feed ("!!SOUND(combat/kick.wav)\r\n");
  EXPECT_TRUE (w.played ().back ().find ("combat") != std::string::npos);
  w.finishSound ();

  // T= is a subdirectory
  w.feed ("!!SOUND(kick.wav T=combat)\r\n");
  EXPECT_TRUE (w.played ().back ().find ("combat") != std::string::npos);
  w.finishSound ();

  // falling back to the root
  w.feed ("!!SOUND(punch1.wav T=combat)\r\n");
  EXPECT_TRUE (w.played ().back ().find ("punch1") != std::string::npos);
  w.finishSound ();

  // default extension
  w.feed ("!!SOUND(kick)\r\n");
  EXPECT_TRUE (w.played ().back ().find ("kick.wav") != std::string::npos);
}

TEST (ProtoMspStream, MidiIsSaidOnceAndQuietly)
{
  MspWorld w ("midi");
  w.file ("tune.mid");
  w.feed ("!!MUSIC(tune)\r\n");
  EXPECT_TRUE (w.played ().empty ());
  EXPECT_TRUE (w.allOutput ().find ("MIDI") != std::string::npos);
  const size_t lines = w.doc.m_LineList.GetCount ();
  w.feed ("!!MUSIC(tune)\r\n");
  EXPECT_EQ (lines, (size_t) w.doc.m_LineList.GetCount ());

  // a playable sibling is better than nothing
  w.file ("tune.mp3");
  w.feed ("!!MUSIC(tune)\r\n");
  ASSERT_EQ (1u, w.played ().size ());
  EXPECT_TRUE (w.played () [0].find ("tune.mp3") != std::string::npos);
}

TEST (ProtoMspStream, NoFileIsNoFuss)
{
  MspWorld w ("none");
  w.feed ("!!SOUND(missing.wav)\r\n");
  EXPECT_TRUE (w.played ().empty ());
  EXPECT_EQ (std::string (), w.allOutput ());
}

TEST (ProtoMspStream, DownloadsAreOffUnlessAsked)
{
  MspWorld w ("dl");
  w.feed ("!!SOUND(missing.wav U=https://a.b/s/)\r\n");
  EXPECT_TRUE (w.doc.m_pProto->mspInFlight.empty ());
  EXPECT_TRUE (w.doc.m_pProto->mspHostsAsked.empty ());
  EXPECT_TRUE (w.allOutput ().find ("msp_downloads") != std::string::npos);    // once, to say so
}

TEST (ProtoMspStream, ADownloadAsksAboutTheHostOnce)
{
  MspWorld w ("ask");
  w.doc.m_bMSPDownloads = true;
  w.feed ("!!SOUND(a.wav U=https://sounds.example/s/)\r\n");
  EXPECT_TRUE (w.allOutput ().find ("sounds.example") != std::string::npos);
  EXPECT_TRUE (w.doc.m_pProto->mspInFlight.empty ());     // not until the player says

  const size_t lines = w.doc.m_LineList.GetCount ();
  w.feed ("!!SOUND(b.wav U=https://sounds.example/s/)\r\n");
  EXPECT_EQ (lines, (size_t) w.doc.m_LineList.GetCount ());
  EXPECT_EQ (2u, w.doc.m_pProto->mspWaiting.size ());

  // a host it did not offer cannot be allowed
  Proto_MspAllowHost (w.doc, "other.example");
  EXPECT_TRUE (w.doc.m_strMSPAllowedHosts.IsEmpty ());
}

TEST (ProtoMspStream, PlainHttpIsAnOptIn)
{
  MspWorld w ("http");
  w.doc.m_bMSPDownloads = true;
  w.feed ("!!SOUND(a.wav U=http://sounds.example/s/)\r\n");
  EXPECT_TRUE (w.allOutput ().find ("msp_allow_http") != std::string::npos);
  EXPECT_TRUE (w.doc.m_pProto->mspWaiting.empty ());
}

TEST (ProtoMspStream, NothingBadGetsDownloaded)
{
  MspWorld w ("dlbad");
  w.doc.m_bMSPDownloads = true;
  w.doc.m_strMSPAllowedHosts = "sounds.example";
  for (const char * bad : { "!!SOUND(x.exe U=https://sounds.example/)\r\n",
                            "!!SOUND(x.wav.exe U=https://sounds.example/)\r\n",
                            "!!SOUND(x*.wav U=https://sounds.example/)\r\n",
                            "!!SOUND(../x.wav U=https://sounds.example/)\r\n",
                            "!!SOUND(x.wav U=file:///c:/x)\r\n",
                            "!!SOUND(x.wav U=https://u:p@sounds.example/)\r\n" })
    w.feed (bad);
  EXPECT_TRUE (w.doc.m_pProto->mspInFlight.empty ());

  // a world behind a proxy does not give its address away for a sound
  w.doc.m_iSocksProcessing = eProxyServerSocks5;
  w.feed ("!!SOUND(y.wav U=https://sounds.example/)\r\n");
  EXPECT_TRUE (w.doc.m_pProto->mspInFlight.empty ());
}

TEST (ProtoMspStream, APluginThatClaimsItKeepsIt)
{
  World w;
  w.doc.m_pProto->bTestSound = true;
  CPlugin * p = LoadPlugin (w.doc, "aaaaaaaaaaaaaaaaaaaaaa09",
    "function OnPluginTelnetRequest (opt, what) return opt == 90 end\n");
  ASSERT_TRUE (p != nullptr);
  w.feed (kWILL + (char) proto::kTelOptMsp);
  EXPECT_EQ (kDO + (char) proto::kTelOptMsp, w.takeSent ());
  EXPECT_TRUE (w.doc.m_pProto->bMspPluginClaimed);
  EXPECT_FALSE (Proto_MspParsing (w.doc));
  w.feed ("!!SOUND(x)\r\n");
  EXPECT_TRUE (w.allOutput ().find ("!!SOUND(x)") != std::string::npos);    // for its trigger
}

TEST (ProtoMspStream, MspXmlKeepsWorkingAlone)
{
  World w;
  w.doc.m_pProto->bTestSound = true;
  // msp.xml's ID, without its VBScript, which a test build has no engine for
  CPlugin * p = LoadPlugin (w.doc, "7da63d78d9f91bebb5285127", "-- stands in for msp.xml\n");
  ASSERT_TRUE (p != nullptr);
  EXPECT_TRUE (Proto_MspPluginLoaded (w.doc));
  w.feed (kWILL + (char) proto::kTelOptMsp);
  EXPECT_EQ (kDO + (char) proto::kTelOptMsp, w.takeSent ());   // the client can do MSP
  EXPECT_FALSE (Proto_MspParsing (w.doc));
  w.feed ("!!SOUND(x)\r\n");
  EXPECT_TRUE (w.allOutput ().find ("!!SOUND(x)") != std::string::npos);    // its trigger's line
  EXPECT_TRUE (w.doc.m_pProto->testPlayed.empty ());                         // we do not play it
}

TEST (ProtoMspStream, MxpTagsPlayThroughTheSamePlayer)
{
  World w (true);
  const std::string dir = TempDir ("mxpsound");
  TouchFile (fs::path (dir) / "ding.wav");
  TouchFile (fs::path (dir) / "tune.mp3");
  w.doc.m_strMSPSoundDirectory = dir.c_str ();
  w.doc.m_pProto->bTestSound = true;

  w.feed (kSecure + "<SOUND ding.wav V=50>");
  ASSERT_EQ (1u, w.doc.m_pProto->testPlayed.size ());
  EXPECT_EQ (50, w.doc.m_pProto->sound.volume);

  w.feed (kSecure + "<MUSIC \"tune.mp3\" 25 -1>");
  ASSERT_EQ (2u, w.doc.m_pProto->testPlayed.size ());
  EXPECT_EQ (25, w.doc.m_pProto->music.volume);
  EXPECT_TRUE (w.doc.m_pProto->testPlayed [1].find (" loop") != std::string::npos);

  w.feed (kSecure + "<SOUND off>");
  EXPECT_FALSE (w.doc.m_pProto->sound.bActive);

  // secure only
  w.feed ("\r\n");
  w.doc.m_pProto->testPlayed.clear ();
  w.feed ("<SOUND ding.wav>\r\n");
  EXPECT_TRUE (w.doc.m_pProto->testPlayed.empty ());
}

#ifdef _WIN32
#include "platform/ma_decode.h"

static std::string MakeWav (unsigned int channels, unsigned int bits, unsigned long rate, size_t frames)
{
  std::string wav;
  const auto put32 = [&wav] (unsigned long v) { for (int i = 0; i < 4; i++) wav += (char) ((v >> (8 * i)) & 0xFF); };
  const auto put16 = [&wav] (unsigned int v) { wav += (char) (v & 0xFF); wav += (char) ((v >> 8) & 0xFF); };
  const unsigned int frameBytes = channels * bits / 8;
  wav += "RIFF"; put32 ((unsigned long) (36 + frames * frameBytes)); wav += "WAVEfmt ";
  put32 (16); put16 (1); put16 (channels); put32 (rate); put32 (rate * frameBytes); put16 (frameBytes); put16 (bits);
  wav += "data"; put32 ((unsigned long) (frames * frameBytes));
  wav.append (frames * frameBytes, 'x');
  return wav;
}

TEST (ProtoDecode, AWavBecomesAWav)
{
  const std::string dir = TempDir ("decode");
  const std::string path = dir + "//t.wav";
  std::ofstream (path, std::ios::binary) << MakeWav (2, 8, 11025, 1000);
  std::wstring wide (path.begin (), path.end ());
  std::string wav;
  ASSERT_TRUE (MC_DecodeAudioFileToWav (wide.c_str (), &wav));
  EXPECT_EQ (0, wav.compare (0, 4, "RIFF"));
  EXPECT_EQ (0, wav.compare (8, 4, "WAVE"));
  EXPECT_EQ (44u + 1000 * 4, wav.size ());     // always 16 bits
}

TEST (ProtoDecode, RubbishIsRefused)
{
  const std::string dir = TempDir ("decode2");
  const std::string path = dir + "//t.mp3";
  std::ofstream (path, std::ios::binary) << "this is not audio at all, however long it goes on for";
  std::wstring wide (path.begin (), path.end ());
  std::string wav;
  EXPECT_FALSE (MC_DecodeAudioFileToWav (wide.c_str (), &wav));
  std::wstring missing = L"c:/no/such/file.mp3";
  EXPECT_FALSE (MC_DecodeAudioFileToWav (missing.c_str (), &wav));
}
#endif

// ------------------------------------------------------------- GMCP

static const std::string kWillGmcp = kWILL + (char) proto::kTelOptGmcp;
static const std::string kDoGmcp = kDO + (char) proto::kTelOptGmcp;
static const std::string kDontGmcp = kDONT + (char) proto::kTelOptGmcp;

TEST (ProtoGmcp, Helpers)
{
  std::string package, raw;
  proto::SplitGmcp ("Char.Vitals { \"hp\": 1 }", &package, &raw);
  EXPECT_EQ (std::string ("Char.Vitals"), package);
  EXPECT_EQ (std::string ("{ \"hp\": 1 }"), raw);
  proto::SplitGmcp ("Core.Ping", &package, &raw);
  EXPECT_EQ (std::string ("Core.Ping"), package);
  EXPECT_EQ (std::string (), raw);
  proto::SplitGmcp ("room.info\n  [1]  ", &package, &raw);
  EXPECT_EQ (std::string ("room.info"), package);
  EXPECT_EQ (std::string ("[1]"), raw);

  EXPECT_TRUE (proto::GmcpPackageMatches ("Char", "char.vitals"));
  EXPECT_TRUE (proto::GmcpPackageMatches ("char.vitals", "Char.Vitals"));
  EXPECT_FALSE (proto::GmcpPackageMatches ("Char", "Character"));
  EXPECT_FALSE (proto::GmcpPackageMatches ("Char.Vitals", "Char"));

  EXPECT_EQ (kSB + (char) 201 + "a" + (char) IAC + (char) IAC + "b" + kSE,
             proto::BuildGmcpPacket (std::string ("a") + (char) IAC + "b"));

  EXPECT_TRUE (proto::SendsGmcpSupportsSet ((kSB + (char) 201 + "Core.Supports.Set [ ]" + kSE).data (), 26));
  EXPECT_TRUE (proto::SendsGmcpSupportsSet ((kSB + (char) 201 + "core.supports.set [ ]" + kSE).data (), 26));
  EXPECT_FALSE (proto::SendsGmcpSupportsSet ((kSB + (char) 201 + "Core.Supports.Add [ ]" + kSE).data (), 26));
  EXPECT_FALSE (proto::SendsGmcpSupportsSet ((kSB + (char) 200 + "Core.Supports.Set [ ]" + kSE).data (), 26));

  EXPECT_TRUE (proto::IsValidGmcpPackage ("Char.Vitals"));
  EXPECT_FALSE (proto::IsValidGmcpPackage (""));
  EXPECT_FALSE (proto::IsValidGmcpPackage ("a\" ] [\"b"));
  EXPECT_FALSE (proto::IsValidGmcpPackage ("a b"));
}

TEST (ProtoGmcp, Rule2_NobodyWantsItSoDont)
{
  World w;
  w.feed (kWillGmcp);
  EXPECT_EQ (kDontGmcp, w.takeSent ());
}

TEST (ProtoGmcp, ASubscriberMeansOurHandshake)
{
  World w;
  EXPECT_EQ ((long) eOK, w.doc.GMCP_Subscribe ("Char.Vitals", "1"));
  EXPECT_EQ ((long) eOK, w.doc.GMCP_Subscribe ("room.info", ""));
  EXPECT_TRUE (w.takeSent ().empty ());          // nothing to say until the server offers

  w.feed (kWillGmcp);
  const std::string sent = w.takeSent ();
  EXPECT_EQ (0, sent.compare (0, 3, kDoGmcp));
  const size_t hello = sent.find ("Core.Hello");
  const size_t add = sent.find ("Core.Supports.Add");
  EXPECT_TRUE (hello != std::string::npos);
  EXPECT_TRUE (add != std::string::npos);
  EXPECT_LT (hello, add);
  EXPECT_TRUE (sent.find ("MUSHclient") != std::string::npos);
  EXPECT_TRUE (sent.find ("\"Char.Vitals 1\"") != std::string::npos);
  EXPECT_TRUE (sent.find ("\"room.info\"") != std::string::npos);     // case kept, no version
  EXPECT_TRUE (sent.find ("Supports.Set") == std::string::npos);
}

TEST (ProtoGmcp, AnotherSubscriberIsAnAdd)
{
  World w;
  w.doc.GMCP_Subscribe ("Char", "1");
  w.feed (kWillGmcp);
  w.takeSent ();

  w.doc.GMCP_Subscribe ("Room", "1");
  EXPECT_EQ (proto::BuildGmcpPacket ("Core.Supports.Add [ \"Room 1\" ]"), w.takeSent ());

  // already wanted: nothing to tell the server
  w.doc.GMCP_Subscribe ("ROOM", "1");
  EXPECT_TRUE (w.takeSent ().empty ());

  w.doc.GMCP_Unsubscribe ("room");
  EXPECT_EQ (proto::BuildGmcpPacket ("Core.Supports.Remove [ \"Room\" ]"), w.takeSent ());

  // not one that was never there
  w.doc.GMCP_Unsubscribe ("room");
  EXPECT_TRUE (w.takeSent ().empty ());
}

TEST (ProtoGmcp, BadNamesAreRefused)
{
  World w;
  EXPECT_EQ ((long) eBadParameter, w.doc.GMCP_Subscribe ("a\" ] [\"b", ""));
  EXPECT_EQ ((long) eBadParameter, w.doc.GMCP_Subscribe ("Char", "1\""));
  EXPECT_EQ ((long) eBadParameter, w.doc.GMCP_Unsubscribe (""));
  EXPECT_EQ ((long) eBadParameter, w.doc.GMCP_Send ("a b", ""));
  EXPECT_TRUE (w.doc.m_pProto->gmcpSubs.empty ());
}

TEST (ProtoGmcp, ALateSubscriberSaysALateDo)
{
  World w;
  w.feed (kWillGmcp);
  EXPECT_EQ (kDontGmcp, w.takeSent ());

  w.doc.GMCP_Subscribe ("Char", "1");
  const std::string sent = w.takeSent ();
  EXPECT_EQ (0, sent.compare (0, 3, kDoGmcp));
  EXPECT_TRUE (sent.find ("Core.Hello") != std::string::npos);
  EXPECT_TRUE (sent.find ("\"Char 1\"") != std::string::npos);
  EXPECT_TRUE (w.doc.m_bClient_sent_IAC_DO [proto::kTelOptGmcp]);
}

TEST (ProtoGmcp, NoOfferNoDo)
{
  World w;
  w.doc.GMCP_Subscribe ("Char", "1");
  EXPECT_TRUE (w.takeSent ().empty ());
}

static const char * const kQuowLike =
  "function OnPluginTelnetRequest (opt, what)\n"
  "  if opt ~= 201 then return false end\n"
  "  if what == 'WILL' then return true end\n"
  "  if what == 'SENT_DO' then\n"
  "    SendPkt (string.char (255, 250, 201) .. 'Core.Hello { \"client\": \"Quow\" }' .. string.char (255, 240))\n"
  "    SendPkt (string.char (255, 250, 201) .. 'Core.Supports.Set [ \"room.info\" ]' .. string.char (255, 240))\n"
  "    return true\n"
  "  end\n"
  "  return false\n"
  "end\n"
  "function OnPluginTelnetSubnegotiation (opt, s) if opt == 201 then SetVariable ('raw', s) end end\n";

TEST (ProtoGmcp, Rule1_APluginsHandshakeIsItsOwn)
{
  World w;
  CPlugin * p = LoadPlugin (w.doc, "aaaaaaaaaaaaaaaaaaaaaa0a", kQuowLike);
  ASSERT_TRUE (p != nullptr);

  w.feed (kWillGmcp);
  const std::string sent = w.takeSent ();
  EXPECT_EQ (0, sent.compare (0, 3, kDoGmcp));
  EXPECT_TRUE (sent.find ("Quow") != std::string::npos);
  // one Hello, the plugin's, one Set, and no Add with nobody subscribed
  EXPECT_TRUE (sent.find ("MUSHclient") == std::string::npos);
  EXPECT_TRUE (sent.find ("Supports.Add") == std::string::npos);
  EXPECT_TRUE (w.doc.m_pProto->bGmcpPluginClaimed);

  // raw callback as ever
  w.feed (SB_ (proto::kTelOptGmcp, "room.info { \"id\": 1 }"));
  EXPECT_EQ (std::string ("room.info { \"id\": 1 }"), PluginVar (p, "raw"));
}

TEST (ProtoGmcp, Rule3_AddComesAfterTheChain)
{
  World w;
  w.doc.GMCP_Subscribe ("Char", "1");
  CPlugin * p = LoadPlugin (w.doc, "aaaaaaaaaaaaaaaaaaaaaa0b", kQuowLike);
  ASSERT_TRUE (p != nullptr);

  w.feed (kWillGmcp);
  const std::string sent = w.takeSent ();
  const size_t set = sent.find ("Core.Supports.Set");
  const size_t add = sent.find ("Core.Supports.Add");
  EXPECT_TRUE (set != std::string::npos);
  ASSERT_TRUE (add != std::string::npos);
  EXPECT_LT (set, add);                                    // else the Set would wipe it
  EXPECT_EQ (sent.find ("Core.Supports.Add"), sent.rfind ("Core.Supports.Add"));   // once
  EXPECT_TRUE (sent.find ("MUSHclient") == std::string::npos);   // no second Hello
}

TEST (ProtoGmcp, Rule4_ALaterSetGetsAddAgain)
{
  World w;
  w.doc.GMCP_Subscribe ("Char", "1");
  CPlugin * p = LoadPlugin (w.doc, "aaaaaaaaaaaaaaaaaaaaaa0c",
    "function OnPluginTelnetRequest (opt, what) return opt == 201 and what == 'WILL' end\n");
  ASSERT_TRUE (p != nullptr);

  w.feed (kWillGmcp);
  w.takeSent ();

  const std::string set = proto::BuildGmcpPacket ("Core.Supports.Set [ \"a\" ]");
  w.doc.SendPacket (set.data (), (int) set.size ());     // what SendPkt and L_SendPkt both call
  const std::string sent = w.takeSent ();
  const size_t add = sent.find ("Core.Supports.Add");
  ASSERT_TRUE (add != std::string::npos);
  EXPECT_GT (add, sent.find ("Core.Supports.Set"));
  EXPECT_TRUE (sent.find ("\"Char 1\"") != std::string::npos);

  // nobody subscribed: no reaction
  World w2;
  w2.doc.m_bClient_sent_IAC_DO [proto::kTelOptGmcp] = true;
  w2.doc.SendPacket (set.data (), (int) set.size ());
  EXPECT_TRUE (w2.takeSent ().find ("Add") == std::string::npos);
}

TEST (ProtoGmcp, Receiving)
{
  World w;
  CPlugin * sub = LoadPlugin (w.doc, "aaaaaaaaaaaaaaaaaaaaaa0d",
    "GMCP_Subscribe ('char.vitals')\n"
    "function OnPluginGMCP (package, raw) SetVariable ('got', package .. '|' .. raw) end\n");
  CPlugin * other = LoadPlugin (w.doc, "aaaaaaaaaaaaaaaaaaaaaa0e",
    "GMCP_Subscribe ('Room')\n"
    "function OnPluginGMCP (package, raw) SetVariable ('got', package .. '|' .. raw) end\n");
  CPlugin * all = LoadPlugin (w.doc, "aaaaaaaaaaaaaaaaaaaaaa0f",
    "function OnPluginGMCP (package, raw) SetVariable ('got', package .. '|' .. raw) end\n");
  ASSERT_TRUE (sub != nullptr);
  ASSERT_TRUE (other != nullptr);
  ASSERT_TRUE (all != nullptr);

  w.feed (kWillGmcp);
  w.takeSent ();

  // matched regardless of case, and the name given as the server spelled it
  w.feed (SB_ (proto::kTelOptGmcp, "Char.Vitals { \"hp\": 5 }"));
  EXPECT_EQ (std::string ("Char.Vitals|{ \"hp\": 5 }"), PluginVar (sub, "got"));
  EXPECT_EQ (std::string ("<unset>"), PluginVar (other, "got"));
  EXPECT_EQ (std::string ("Char.Vitals|{ \"hp\": 5 }"), PluginVar (all, "got"));

  // a parent module covers its children
  w.feed (SB_ (proto::kTelOptGmcp, "room.info [ 1 ]"));
  EXPECT_EQ (std::string ("room.info|[ 1 ]"), PluginVar (other, "got"));

  // the last value of each, however asked
  std::string raw;
  EXPECT_TRUE (Proto_GmcpLast (w.doc, "CHAR.VITALS", &raw));
  EXPECT_EQ (std::string ("{ \"hp\": 5 }"), raw);
  w.feed (SB_ (proto::kTelOptGmcp, "char.vitals { \"hp\": 6 }"));
  EXPECT_TRUE (Proto_GmcpLast (w.doc, "Char.Vitals", &raw));
  EXPECT_EQ (std::string ("{ \"hp\": 6 }"), raw);
  EXPECT_FALSE (Proto_GmcpLast (w.doc, "Nothing", &raw));

  VARIANT v = w.doc.GMCP_Get ("nothing");
  EXPECT_EQ ((int) VT_NULL, (int) v.vt);
  VariantClear (&v);
  v = w.doc.GMCP_Get ("CHAR.VITALS");
  EXPECT_EQ ((int) VT_BSTR, (int) v.vt);
  VariantClear (&v);
}

TEST (ProtoGmcp, LastValuesWorkWhoeverDidTheHandshake)
{
  World w;
  CPlugin * p = LoadPlugin (w.doc, "aaaaaaaaaaaaaaaaaaaaaa10", kQuowLike);
  ASSERT_TRUE (p != nullptr);
  w.feed (kWillGmcp);
  w.feed (SB_ (proto::kTelOptGmcp, "room.info { \"id\": 1 }"));
  std::string raw;
  EXPECT_TRUE (Proto_GmcpLast (w.doc, "room.info", &raw));
}

TEST (ProtoGmcp, Sending)
{
  World w;
  EXPECT_EQ ((long) eOptionOutOfRange, w.doc.GMCP_Send ("Char.Hello", "{}"));
  EXPECT_TRUE (w.takeSent ().empty ());

  w.doc.GMCP_Subscribe ("Char", "1");
  w.feed (kWillGmcp);
  w.takeSent ();

  EXPECT_EQ ((long) eOK, w.doc.GMCP_Send ("char.login", "{ \"a\": \"b\" }"));
  EXPECT_EQ (proto::BuildGmcpPacket ("char.login { \"a\": \"b\" }"), w.takeSent ());   // case kept

  EXPECT_EQ ((long) eOK, w.doc.GMCP_Send ("Core.Ping", ""));
  EXPECT_EQ (proto::BuildGmcpPacket ("Core.Ping"), w.takeSent ());

  EXPECT_EQ ((long) eOK, w.doc.GMCP_Send ("Say", "\"\xFF\""));
  const std::string sent = w.takeSent ();
  EXPECT_TRUE (sent.find (std::string ("\xFF\xFF")) != std::string::npos);    // escaped

  w.doc.m_iConnectPhase = eConnectNotConnected;
  EXPECT_EQ ((long) eWorldClosed, w.doc.GMCP_Send ("Core.Ping", ""));
  w.doc.m_iConnectPhase = eConnectConnectedToMud;
}

TEST (ProtoGmcp, RepeatsAndReconnects)
{
  World w;
  w.doc.m_pProto->testNow = 5000;
  w.doc.GMCP_Subscribe ("Char", "1");
  w.feed (kWillGmcp);
  w.takeSent ();

  // a repeat right away gets nothing
  w.feed (kWillGmcp);
  EXPECT_TRUE (w.takeSent ().empty ());

  // later it is a copyover, as far as anyone can tell
  w.doc.m_pProto->testNow += 61000;
  w.feed (kWillGmcp);
  const std::string sent = w.takeSent ();
  EXPECT_TRUE (sent.find ("Core.Hello") != std::string::npos);
  EXPECT_TRUE (sent.find ("Supports.Add") != std::string::npos);

  // subscriptions outlive a connection; what it did does not
  w.feed (SB_ (proto::kTelOptGmcp, "x.y 1"));
  w.doc.m_pProto->ResetForConnection ();
  ZeroMemory (w.doc.m_bClient_sent_IAC_DO, sizeof w.doc.m_bClient_sent_IAC_DO);
  std::string raw;
  EXPECT_FALSE (Proto_GmcpLast (w.doc, "x.y", &raw));
  EXPECT_EQ (1u, w.doc.m_pProto->gmcpSubs.size ());
  w.feed (kWillGmcp);
  EXPECT_TRUE (w.takeSent ().find ("Core.Hello") != std::string::npos);
}

TEST (ProtoGmcp, APluginsSubscriptionsGoWithIt)
{
  World w;
  CPlugin * p = LoadPlugin (w.doc, "aaaaaaaaaaaaaaaaaaaaaa11", "GMCP_Subscribe ('Char', 1)\n");
  ASSERT_TRUE (p != nullptr);
  w.doc.GMCP_Subscribe ("Char", "1");      // the world's script wants it too
  EXPECT_EQ (1u, w.doc.m_pProto->gmcpSubs.size ());
  EXPECT_EQ (2u, w.doc.m_pProto->gmcpSubs ["char"].owners.size ());

  w.feed (kWillGmcp);
  w.takeSent ();

  // still wanted by someone, so nothing is said
  Proto_GmcpPluginGone (w.doc, "aaaaaaaaaaaaaaaaaaaaaa11");
  EXPECT_TRUE (w.takeSent ().empty ());
  EXPECT_EQ (1u, w.doc.m_pProto->gmcpSubs.size ());

  // and now nobody does
  w.doc.GMCP_Unsubscribe ("char");
  EXPECT_TRUE (w.takeSent ().find ("Supports.Remove") != std::string::npos);
  EXPECT_TRUE (w.doc.m_pProto->gmcpSubs.empty ());
}

MC_TEST_MAIN ()

// protocols.cpp -- the pure logic of protocols.h, and CProtocolState.  No document here.

#include "stdafx.h"
#include "protocols.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace proto {

namespace {

const char kIAC = (char) 0xFF;
const char kSB  = (char) 0xFA;
const char kSE  = (char) 0xF0;

bool EqualNoCase (const std::string & a, const std::string & b)
  {
  return a.size () == b.size () && Lower (a) == Lower (b);
  }

std::string Trim (const std::string & s)
  {
  size_t start = 0, end = s.size ();
  while (start < end && isspace ((unsigned char) s [start]))
    start++;
  while (end > start && isspace ((unsigned char) s [end - 1]))
    end--;
  return s.substr (start, end - start);
  }

std::vector <std::string> SplitOn (const std::string & s, char delimiter)
  {
  std::vector <std::string> parts;
  size_t start = 0;
  for (;;)
    {
    const size_t at = s.find (delimiter, start);
    if (at == std::string::npos)
      {
      parts.push_back (s.substr (start));
      return parts;
      }
    parts.push_back (s.substr (start, at - start));
    start = at + 1;
    }
  }

std::string Extension (const std::string & name)
  {
  const size_t slash = name.rfind ('/');
  const size_t dot = name.rfind ('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
    return "";
  return Lower (name.substr (dot));
  }

bool IsDeviceName (std::string component)
  {
  // A device name is one with any extension too: NUL.txt opens NUL.
  const size_t dot = component.find ('.');
  if (dot != std::string::npos)
    component.resize (dot);
  component = Lower (component);
  static const char * const kNames [] =
    { "con", "prn", "aux", "nul", "conin$", "conout$", "clock$" };
  for (const char * name : kNames)
    if (component == name)
      return true;
  if (component.size () == 4 &&
      (component.compare (0, 3, "com") == 0 || component.compare (0, 3, "lpt") == 0) &&
      isdigit ((unsigned char) component [3]))
    return true;
  return false;
  }

} // anonymous namespace

std::string Lower (std::string s)
  {
  std::transform (s.begin (), s.end (), s.begin (),
                  [] (unsigned char c) { return (char) tolower (c); });
  return s;
  }

// ---------------------------------------------------------------- telnet

void NegotiationGuard::Reset ()
  {
  memset (m_slots, 0, sizeof m_slots);
  }

bool NegotiationGuard::Allow (unsigned char option, uint64_t nowMs)
  {
  Slot & slot = m_slots [option];

  if (nowMs < slot.mutedUntil)
    return false;

  if (slot.count == 0 || nowMs - slot.windowStart >= kWindowMs)
    {
    slot.windowStart = nowMs;
    slot.count = 0;
    }

  if (++slot.count > kMaxPerWindow)
    {
    slot.mutedUntil = nowMs + kMuteMs;
    slot.count = 0;
    return false;
    }
  return true;
  }

std::string ChooseCharset (const std::string & offerIn,
                           const std::vector <std::string> & acceptable)
  {
  std::string offer = offerIn;

  // RFC 2066: a server may offer a translation table first.  We have no use for one,
  // and take the plain list that follows.
  static const char kTable [] = "[TTABLE]";
  if (offer.compare (0, sizeof kTable - 1, kTable) == 0)
    {
    if (offer.size () < sizeof kTable + 1)
      return "";
    offer = offer.substr (sizeof kTable);    // and its version byte
    }

  if (offer.size () < 2)
    return "";

  for (const std::string & name : SplitOn (offer.substr (1), offer [0]))
    for (const std::string & ours : acceptable)
      if (EqualNoCase (Trim (name), ours))
        return Trim (name);
  return "";
  }

std::vector <std::string> CharsetNames (const bool bUtf8, const unsigned int codePage)
  {
  std::vector <std::string> names;

  if (bUtf8)
    {
    names.push_back ("UTF-8");
    names.push_back ("UTF8");
    }
  else
    switch (codePage)
      {
      case 1252:
      case 28591:
        // A Latin-1 world.  CP1252 is ISO-8859-1 for every printable byte they share.
        names.push_back ("ISO-8859-1");
        names.push_back ("ISO_8859-1");
        names.push_back ("ISO8859-1");
        names.push_back ("LATIN-1");
        names.push_back ("LATIN1");
        names.push_back ("WINDOWS-1252");
        names.push_back ("CP1252");
        break;
      case 932:   names.push_back ("SHIFT_JIS"); names.push_back ("SJIS");
                  names.push_back ("CP932");     break;
      case 936:   names.push_back ("GBK");       names.push_back ("GB2312");
                  names.push_back ("CP936");     break;
      case 949:   names.push_back ("EUC-KR");    names.push_back ("CP949");  break;
      case 950:   names.push_back ("BIG5");      names.push_back ("CP950");  break;
      case 437:   names.push_back ("CP437");     names.push_back ("IBM437"); break;
      default:
        {
        char buffer [32];
        snprintf (buffer, sizeof buffer, "WINDOWS-%u", codePage);
        names.push_back (buffer);
        snprintf (buffer, sizeof buffer, "CP%u", codePage);
        names.push_back (buffer);
        }
        break;
      }

  // Any of them can read ASCII, which is what they would be agreeing to.
  names.push_back ("US-ASCII");
  names.push_back ("ASCII");
  return names;
  }

std::string MnesCharsetName (const bool bUtf8, const unsigned int codePage)
  {
  if (bUtf8)
    return "UTF-8";
  switch (codePage)
    {
    case 1252:
    case 28591: return "ISO-8859-1";
    case 950:   return "BIG5";
    case 949:   return "CP949";
    case 936:   return "GB18030";
    case 437:   return "CP437";
    default:
      {
      char buffer [32];
      snprintf (buffer, sizeof buffer, "CP%u", codePage);
      return buffer;
      }
    }
  }

std::string BuildEnvironReply (const std::string & request, const EnvVars & vars)
  {
  const char kIS = 0, kSEND = 1, kVAR = 0, kVAL = 1, kESC = 2, kUSERVAR = 3;

  if (request.empty () || request [0] != kSEND)
    return "";

  // The variables asked for: VAR name, or USERVAR name.  ESC quotes the next byte.
  std::vector <std::pair <char, std::string> > wanted;
  for (size_t i = 1; i < request.size (); i++)
    {
    const char c = request [i];
    if (c == kVAR || c == kUSERVAR)
      {
      wanted.push_back (std::make_pair (c, std::string ()));
      continue;
      }
    if (wanted.empty ())
      continue;                // junk before any VAR
    if (c == kESC && i + 1 < request.size ())
      wanted.back ().second.append (1, request [++i]);
    else
      wanted.back ().second.append (1, c);
    }

  // "SEND" and "SEND VAR", with nothing named, both mean everything.
  bool bAll = true;
  for (const auto & item : wanted)
    if (!item.second.empty ())
      bAll = false;

  std::string reply (1, kIS);

  const auto append = [&reply, kVAL] (char kind, const std::string & name, const std::string * value)
    {
    reply += kind;
    reply += name;
    if (value)
      {
      reply += kVAL;
      for (const char v : *value)
        if ((unsigned char) v > 3 && (unsigned char) v != 0xFF)   // these bytes are syntax
          reply += v;
      }
    };

  if (bAll)
    {
    for (const auto & var : vars)
      append (kVAR, var.first, &var.second);
    return reply;
    }

  for (const auto & item : wanted)
    {
    if (item.second.empty ())
      continue;
    const std::string * value = NULL;
    for (const auto & var : vars)
      if (var.first == item.second)
        value = &var.second;
    append (item.first, item.second, value);
    }
  return reply;
  }

int FindMsspTlsPort (const std::string & data)
  {
  const char kMsspVar = 1, kMsspVal = 2;

  std::string name;
  size_t i = 0;
  while (i < data.size ())
    {
    const char c = data [i++];
    size_t end = i;
    while (end < data.size () && data [end] != kMsspVar && data [end] != kMsspVal)
      end++;
    const std::string text = data.substr (i, end - i);
    i = end;

    if (c == kMsspVar)
      name = Lower (text);
    else if (c == kMsspVal && (name == "ssl" || name == "tls"))
      {
      bool bDigits = !text.empty () && text.size () <= 5;
      for (const char d : text)
        if (!isdigit ((unsigned char) d))
          bDigits = false;
      if (bDigits)
        {
        const int port = atoi (text.c_str ());
        if (port > 1 && port <= 65535)
          return port;
        }
      }
    }
  return 0;
  }

// ---------------------------------------------------------------- OSC 8

OscLinkKind ClassifyOsc8Target (const std::string & uri, std::string * action)
  {
  action->clear ();

  const std::string lower = Lower (uri.substr (0, 8));
  if (lower.compare (0, 7, "http://") == 0 || lower.compare (0, 8, "https://") == 0)
    {
    if (uri.size () <= (lower [4] == 's' ? 8u : 7u))
      return eOscLinkNone;       // a scheme and nothing else
    *action = uri;
    return eOscLinkUrl;
    }

  OscLinkKind kind;
  size_t start;
  if (lower.compare (0, 5, "send:") == 0)
    {
    kind = eOscLinkSend;
    start = 5;
    }
  else if (lower.compare (0, 7, "prompt:") == 0)
    {
    kind = eOscLinkPrompt;
    start = 7;
    }
  else
    return eOscLinkNone;

  std::string decoded;
  for (size_t i = start; i < uri.size (); i++)
    {
    unsigned char c = (unsigned char) uri [i];
    if (c == '%' && i + 2 < uri.size () && isxdigit ((unsigned char) uri [i + 1]) &&
        isxdigit ((unsigned char) uri [i + 2]))
      {
      const char hex [3] = { uri [i + 1], uri [i + 2], 0 };
      c = (unsigned char) strtoul (hex, NULL, 16);
      i += 2;
      }
    if (c < 0x20 || c == 0x7F || c == '|')
      return eOscLinkNone;
    decoded += (char) c;
    }

  if (decoded.empty ())
    return eOscLinkNone;

  *action = decoded;
  return kind;
  }

// ---------------------------------------------------------------- MXP

std::string EntityListAdd (const std::string & list, const std::string & item)
  {
  if (list.empty ())
    return item;
  return list + "|" + item;
  }

std::string EntityListRemove (const std::string & list, const std::string & item)
  {
  std::string result;
  bool bFirst = true;
  bool bRemoved = false;
  for (const std::string & entry : SplitOn (list, '|'))
    {
    if (!bRemoved && entry == item)
      {
      bRemoved = true;       // one removal per REMOVE
      continue;
      }
    if (!bFirst)
      result += "|";
    result += entry;
    bFirst = false;
    }
  return result;
  }

int LineTagForFlag (const std::string & flag)
  {
  if (flag == "roomname") return 10;
  if (flag == "roomdesc") return 11;
  if (flag == "roomexit") return 12;
  if (flag == "roomnum")  return 13;
  if (flag == "prompt")   return 14;
  return 0;
  }

std::string FlagForLineTag (const int tag)
  {
  switch (tag)
    {
    case 10: return "roomname";
    case 11: return "roomdesc";
    case 12: return "roomexit";
    case 13: return "roomnum";
    case 14: return "prompt";
    case 19: return "welcome";
    default: return "";
    }
  }

int FrameDimension (const std::string & textIn, const int total, const int charSize)
  {
  const std::string text = Lower (Trim (textIn));
  if (text.empty ())
    return 0;

  const double number = atof (text.c_str ());
  double value = number;
  if (text.back () == '%')
    value = number * total / 100.0;
  else if (text.back () == 'c')
    value = number * charSize;

  // a hostile number must not turn into an enormous window
  if (value > 16384.0)
    value = 16384.0;
  if (value < -16384.0)
    value = -16384.0;
  return (int) value;
  }

std::string EncodeUtf8 (const unsigned long cp)
  {
  std::string out;
  if (cp < 0x80)
    out += (char) cp;
  else if (cp < 0x800)
    {
    out += (char) (0xC0 | (cp >> 6));
    out += (char) (0x80 | (cp & 0x3F));
    }
  else if (cp < 0x10000)
    {
    if (cp >= 0xD800 && cp <= 0xDFFF)    // surrogates are not characters
      return "";
    out += (char) (0xE0 | (cp >> 12));
    out += (char) (0x80 | ((cp >> 6) & 0x3F));
    out += (char) (0x80 | (cp & 0x3F));
    }
  else if (cp <= 0x10FFFF)
    {
    out += (char) (0xF0 | (cp >> 18));
    out += (char) (0x80 | ((cp >> 12) & 0x3F));
    out += (char) (0x80 | ((cp >> 6) & 0x3F));
    out += (char) (0x80 | (cp & 0x3F));
    }
  return out;
  }

bool IsPlausibleHostName (const std::string & host)
  {
  if (host.empty () || host.size () > 253)
    return false;
  for (const unsigned char c : host)
    if (!isalnum (c) && c != '.' && c != '-' && c != ':')
      return false;
  return true;
  }

// ---------------------------------------------------------------- MSP

bool ParseMspTrigger (const std::string & inner, const bool bMusic, MspTrigger * out)
  {
  *out = MspTrigger ();
  out->bMusic = bMusic;

  // Space-separated, with quotes allowed round a value (the spec lets a URL have them).
  std::vector <std::string> words;
  std::string word;
  char quote = 0;
  for (const char c : inner)
    {
    if (quote)
      {
      if (c == quote)
        quote = 0;
      else
        word += c;
      }
    else if (c == '\'' || c == '"')
      quote = c;
    else if (c == ' ' || c == '\t')
      {
      if (!word.empty ())
        words.push_back (word);
      word.clear ();
      }
    else
      word += c;
    }
  if (!word.empty ())
    words.push_back (word);

  if (words.empty ())
    return false;

  out->name = words [0];
  out->bOff = EqualNoCase (out->name, "off");

  const auto number = [] (const std::string & text, int fallback)
    {
    if (text.empty ())
      return fallback;
    char * end = NULL;
    const long value = strtol (text.c_str (), &end, 10);
    if (*end || value < -1000000 || value > 1000000)
      return fallback;
    return (int) value;
    };

  for (size_t i = 1; i < words.size (); i++)
    {
    const std::string & item = words [i];
    if (item.size () < 2 || item [1] != '=')
      continue;
    const std::string value = item.substr (2);
    switch (toupper ((unsigned char) item [0]))
      {
      case 'V': out->volume   = (std::min) (100, (std::max) (0, number (value, 100))); break;
      case 'P': out->priority = (std::min) (100, (std::max) (0, number (value, 50)));  break;
      case 'C': out->cont     = number (value, 1) ? 1 : 0;                         break;
      case 'L':
        {
        const int repeats = number (value, 1);
        out->repeats = repeats == -1 ? -1 : (std::max) (1, repeats);
        }
        break;
      case 'T': out->type = value; break;
      case 'U': out->url  = value; break;
      default:  break;     // nothing we know; the spec may grow
      }
    }

  return true;
  }

bool IsSafeMspName (const std::string & name, const bool bWildcards)
  {
  if (name.empty () || name.size () > 200)
    return false;

  // Only characters that mean the same on every file system and in every code page.
  for (const unsigned char c : name)
    if (!isalnum (c) && !strchr ("/._-+ ()", c) && !(bWildcards && (c == '*' || c == '?')))
      return false;

  if (name [0] == '/')        // absolute; spec: invalid
    return false;

  const std::vector <std::string> parts = SplitOn (name, '/');
  if (parts.size () > 8)
    return false;

  for (size_t i = 0; i < parts.size (); i++)
    {
    const std::string & part = parts [i];
    if (part.empty () || part == "." || part == "..")
      return false;
    if (part.back () == '.' || part.back () == ' ' || part [0] == ' ')
      return false;           // Windows would quietly drop them: "x.wav." is x.wav
    if (IsDeviceName (part))
      return false;

    const bool bLast = i + 1 == parts.size ();
    if (bWildcards)
      {
      const size_t stars = (size_t) std::count (part.begin (), part.end (), '*');
      const size_t queries = (size_t) std::count (part.begin (), part.end (), '?');
      if (!bLast && (stars || queries))
        return false;
      if (stars > 1)
        return false;
      }
    }
  return true;
  }

bool MspWildcardMatch (const std::string & patternIn, const std::string & nameIn)
  {
  const std::string pattern = Lower (patternIn);
  const std::string name = Lower (nameIn);

  // At most one *: what is before it must start the name, what is after end it.
  const size_t star = pattern.find ('*');
  const auto same = [] (const std::string & p, const std::string & n)
    {
    if (p.size () != n.size ())
      return false;
    for (size_t i = 0; i < p.size (); i++)
      if (p [i] != '?' && p [i] != n [i])
        return false;
    return true;
    };

  if (star == std::string::npos)
    return same (pattern, name);

  const std::string head = pattern.substr (0, star);
  const std::string tail = pattern.substr (star + 1);
  if (name.size () < head.size () + tail.size ())
    return false;
  return same (head, name.substr (0, head.size ())) &&
         same (tail, name.substr (name.size () - tail.size ()));
  }

std::string MspWithDefaultExtension (const std::string & name, const bool bMusic)
  {
  if (!Extension (name).empty ())
    return name;
  // "bang*" could match any extension, and the spec's default would be wrong for it
  if (name.find ('*') != std::string::npos)
    return name;
  return name + (bMusic ? ".mid" : ".wav");
  }

bool IsMidiName (const std::string & name)
  {
  const std::string extension = Extension (name);
  return extension == ".mid" || extension == ".midi";
  }

bool IsDownloadableAudioName (const std::string & name)
  {
  const std::string extension = Extension (name);
  return extension == ".wav" || extension == ".mp3" || extension == ".flac";
  }

bool LooksLikeAudio (const std::string & name, const std::string & data)
  {
  const std::string extension = Extension (name);
  if (data.size () < 12)
    return false;
  if (extension == ".wav")
    return data.compare (0, 4, "RIFF") == 0 && data.compare (8, 4, "WAVE") == 0;
  if (extension == ".flac")
    return data.compare (0, 4, "fLaC") == 0;
  if (extension == ".mp3")
    return data.compare (0, 3, "ID3") == 0 ||
           ((unsigned char) data [0] == 0xFF && ((unsigned char) data [1] & 0xE0) == 0xE0);
  return false;
  }

double MspVolumeToDb (const int volume)
  {
  if (volume >= 100)
    return 0.0;
  if (volume <= 0)
    return -100.0;
  return 20.0 * log10 (volume / 100.0);
  }

std::string MspDownloadUrl (const std::string & baseIn, const std::string & name)
  {
  const std::string lower = Lower (baseIn.substr (0, 8));
  if (lower.compare (0, 7, "http://") != 0 && lower.compare (0, 8, "https://") != 0)
    return "";

  for (const unsigned char c : baseIn)
    if (c <= ' ' || c >= 0x7F)
      return "";

  std::string base = baseIn;

  // The spec says the file name is added to U=, and some servers give all of it anyway.
  const size_t slash = name.rfind ('/');
  const std::string leaf = slash == std::string::npos ? name : name.substr (slash + 1);
  if (base.size () > leaf.size () && base.compare (base.size () - leaf.size (), leaf.size (), leaf) == 0 &&
      base [base.size () - leaf.size () - 1] == '/')
    return base;

  if (base.back () != '/')
    base += '/';
  return base + name;
  }

std::string UrlHost (const std::string & url)
  {
  const size_t scheme = url.find ("://");
  if (scheme == std::string::npos)
    return "";
  const size_t start = scheme + 3;
  const size_t end = url.find_first_of ("/?#", start);
  std::string authority = url.substr (start, end == std::string::npos ? std::string::npos : end - start);
  if (authority.find ('@') != std::string::npos)     // a user name is nothing a sound URL needs
    return "";
  if (!authority.empty () && authority [0] == '[')
    {
    const size_t close = authority.find (']');
    return close == std::string::npos ? "" : Lower (authority.substr (1, close - 1));
    }
  const size_t colon = authority.rfind (':');
  if (colon != std::string::npos)
    authority.resize (colon);
  return Lower (authority);
  }

bool HostListHas (const std::string & list, const std::string & host)
  {
  for (const std::string & entry : SplitOn (list, ' '))
    if (!entry.empty () && EqualNoCase (entry, host))
      return true;
  return false;
  }

std::string HostListAdd (const std::string & list, const std::string & host)
  {
  if (HostListHas (list, host))
    return list;
  return list.empty () ? Lower (host) : list + " " + Lower (host);
  }

// ---------------------------------------------------------------- GMCP

bool IsValidGmcpPackage (const std::string & package)
  {
  if (package.empty () || package.size () > 128)
    return false;
  for (const unsigned char c : package)
    if (!isalnum (c) && c != '.' && c != '_' && c != '-')
      return false;
  return true;
  }

void SplitGmcp (const std::string & data, std::string * package, std::string * rawText)
  {
  size_t space = 0;
  while (space < data.size () && !isspace ((unsigned char) data [space]))
    space++;
  *package = data.substr (0, space);
  *rawText = Trim (data.substr (space));
  }

bool GmcpPackageMatches (const std::string & subscribed, const std::string & package)
  {
  const std::string sub = Lower (subscribed);
  const std::string pkg = Lower (package);
  if (sub == pkg)
    return true;
  return pkg.size () > sub.size () && pkg.compare (0, sub.size (), sub) == 0 && pkg [sub.size ()] == '.';
  }

std::string BuildGmcpPacket (const std::string & text)
  {
  std::string packet;
  packet += kIAC;
  packet += kSB;
  packet += (char) kTelOptGmcp;
  for (const char c : text)
    {
    packet += c;
    if (c == kIAC)
      packet += kIAC;
    }
  packet += kIAC;
  packet += kSE;
  return packet;
  }

bool SendsGmcpSupportsSet (const char * data, const size_t length)
  {
  static const char kWanted [] = "core.supports.set";
  const size_t wanted = sizeof kWanted - 1;
  for (size_t i = 0; i + 3 + wanted <= length; i++)
    {
    if (data [i] != kIAC || data [i + 1] != kSB || (unsigned char) data [i + 2] != kTelOptGmcp)
      continue;
    bool bSame = true;
    for (size_t j = 0; j < wanted && bSame; j++)
      bSame = tolower ((unsigned char) data [i + 3 + j]) == kWanted [j];
    if (bSame)
      return true;
    }
  return false;
  }

std::string BuildGmcpSupports (const std::string & verb, const std::vector <std::string> & items)
  {
  std::string text = "Core.Supports." + verb + " [ ";
  for (size_t i = 0; i < items.size (); i++)
    {
    if (i)
      text += ", ";
    text += "\"" + items [i] + "\"";
    }
  return text + " ]";
  }

} // namespace proto

// ---------------------------------------------------------------- frames

CMxpFrame::CMxpFrame ()
  : bInternal (false), bScrolling (false), left (0), top (0), width (0), height (0), align (0),
    bDirty (true), bWindowMade (false), row (0), col (0)
  {
  }

void CMxpFrame::Put (const char * ch, const int len, const unsigned long fore, const unsigned long back)
  {
  if (len <= 0 || len > 4)
    return;

  if (rows.size () <= row)
    rows.resize (row + 1);
  std::vector <CMxpCell> & line = rows [row];

  if (col >= kMaxCols)
    return;
  if (line.size () <= col)
    {
    CMxpCell blank;
    memset (&blank, 0, sizeof blank);
    blank.ch [0] = ' ';
    blank.fore = fore;
    blank.back = back;
    line.resize (col + 1, blank);
    }

  CMxpCell & cell = line [col];
  memset (cell.ch, 0, sizeof cell.ch);
  memcpy (cell.ch, ch, (size_t) len);
  cell.fore = fore;
  cell.back = back;
  col++;
  bDirty = true;
  }

void CMxpFrame::NewLine ()
  {
  row++;
  col = 0;
  if (row >= kMaxRows)
    {
    // scroll the oldest off
    if (!rows.empty ())
      rows.erase (rows.begin ());
    row = kMaxRows - 1;
    }
  bDirty = true;
  }

void CMxpFrame::MoveTo (const int x, const int y)
  {
  if (x >= 0)
    col = (std::min) ((size_t) x, kMaxCols - 1);
  if (y >= 0)
    row = (std::min) ((size_t) y, kMaxRows - 1);
  }

void CMxpFrame::EraseToEndOfLine ()
  {
  if (row < rows.size () && col < rows [row].size ())
    rows [row].resize (col);
  bDirty = true;
  }

void CMxpFrame::EraseToEnd ()
  {
  EraseToEndOfLine ();
  if (row + 1 < rows.size ())
    rows.resize (row + 1);
  bDirty = true;
  }

void CMxpFrame::EraseLine (const int y)
  {
  if (y >= 0 && (size_t) y < rows.size ())
    rows [(size_t) y].clear ();
  bDirty = true;
  }

void CMxpFrame::Clear ()
  {
  rows.clear ();
  row = col = 0;
  bDirty = true;
  }

std::string CMxpFrame::RowText (const size_t r) const
  {
  std::string text;
  if (r < rows.size ())
    for (const CMxpCell & cell : rows [r])
      text += cell.ch;
  return text;
  }

// ---------------------------------------------------------------- state

CProtocolState::CProtocolState ()
  : testNow (0),
    bGuardNoted (false),
    bFlushingNotes (false),
    bNewEnviron (false),
    bTlsOffered (false),
    reconnectAfterClose (false),
    oscCount (0),
    bOscReprocess (false),
    bLinkOpen (false),
    linkSavedFlags (0),
    linkSavedFore (0), linkSavedBack (0), linkFore (0), linkBack (0),
    bLinkColoured (false),
    lineTag (0),
    bLineTagFromEscape (false),
    bNoBreak (false),
    bBindingsDirty (false),
    bBindingWindowMade (false),
    bFramesDirty (false),
    bRelocateAsked (false),
    relocatePort (0),
    bRelocated (false),
    relocateOriginalPort (0),
    bMspReplaying (false),
    bMspNegotiated (false),
    bMspPluginClaimed (false),
    bMidiNoted (false),
    bMspBadNameNoted (false),
    bMspDownloadsOffNoted (false),
    downloads (std::make_shared <CMspDownloads> ()),
    bTestSound (false),
    bGmcpWillSeen (false),
    bGmcpPluginClaimed (false),
    bGmcpCoreHandshook (false),
    bGmcpInSentDoChain (false),
    bGmcpResendAdd (false),
    bGmcpSendingOurs (false)
  {
  memset (lastSentDo, 0, sizeof lastSentDo);
  memset (lastSentWill, 0, sizeof lastSentWill);
  memset (testBusy, 0, sizeof testBusy);
  }

uint64_t CProtocolState::Now () const
  {
  if (testNow)
    return testNow;
  using namespace std::chrono;
  return (uint64_t) duration_cast <milliseconds> (steady_clock::now ().time_since_epoch ()).count ();
  }

void CProtocolState::ResetForConnection ()
  {
  guard.Reset ();
  bGuardNoted = false;
  memset (lastSentDo, 0, sizeof lastSentDo);
  memset (lastSentWill, 0, sizeof lastSentWill);
  bNewEnviron = false;
  bTlsOffered = false;
  osc.clear ();
  oscCount = 0;
  bOscReprocess = false;
  bLinkOpen = false;
  lineTag = 0;
  bLineTagFromEscape = false;
  lineElement.clear ();
  bNoBreak = false;
  mspBuffer.clear ();
  bMspReplaying = false;
  bMspNegotiated = false;
  bMspPluginClaimed = false;
  mspDefaultUrl.clear ();
  mspHostsAsked.clear ();
  mspWaiting.clear ();
  mspFailed.clear ();
  gmcpLast.clear ();
  bGmcpWillSeen = false;
  bGmcpPluginClaimed = false;
  bGmcpCoreHandshook = false;
  bGmcpInSentDoChain = false;
  bGmcpResendAdd = false;
  // bRelocated stays: it describes the connection now starting.
  }

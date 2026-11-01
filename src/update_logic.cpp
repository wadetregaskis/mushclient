// update_logic.cpp -- see update_logic.h.

#include "stdafx.h"
#include "update_logic.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <system_error>

#if defined(_WIN32) || defined(OPENSSL_FOUND)
#define MC_UPDATE_HAS_CRYPTO 1
#include <openssl/evp.h>
#include <openssl/err.h>
#endif

#ifndef ZLIB_VERSION
#include <zlib.h>
#endif

namespace fs = std::filesystem;

namespace update {

const char * const kEditionMarker = "discworld-edition.txt";

// ---------------- versions ----------------

bool ParseVersion (const std::string & text, Version * out)
  {
  size_t at = 0;
  if (at < text.size () && (text [at] == 'v' || text [at] == 'V'))
    at++;
  Version v;
  for (;;)
    {
    const size_t start = at;
    int part = 0;
    while (at < text.size () && text [at] >= '0' && text [at] <= '9')
      {
      if (part > 100000000)
        return false;
      part = part * 10 + (text [at] - '0');
      at++;
      }
    if (at == start)
      return false;
    v.parts.push_back (part);
    if (at == text.size ())
      break;
    if (text [at] != '.')
      return false;
    at++;
    }
  *out = v;
  return true;
  }

int CompareVersions (const Version & a, const Version & b)
  {
  const size_t n = (std::max) (a.parts.size (), b.parts.size ());
  for (size_t i = 0; i < n; i++)
    {
    const int x = i < a.parts.size () ? a.parts [i] : 0;
    const int y = i < b.parts.size () ? b.parts [i] : 0;
    if (x != y)
      return x < y ? -1 : 1;
    }
  return 0;
  }

// ---------------- a little JSON ----------------

namespace {

struct JsonValue
  {
  enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
  bool        boolean = false;
  double      number = 0;
  std::string text;
  std::vector<JsonValue> items;
  std::vector<std::pair<std::string, JsonValue>> members;

  const JsonValue * Find (const char * name) const
    {
    if (kind != Object)
      return nullptr;
    for (const auto & m : members)
      if (m.first == name)
        return &m.second;
    return nullptr;
    }

  std::string Text (const char * name) const
    {
    const JsonValue * v = Find (name);
    return v && v->kind == String ? v->text : std::string ();
    }

  bool Boolean (const char * name) const
    {
    const JsonValue * v = Find (name);
    return v && v->kind == Bool && v->boolean;
    }
  };

class JsonParser
  {
  public:
  explicit JsonParser (const std::string & text) : m_p (text.data ()), m_end (text.data () + text.size ()) {}

  bool Parse (JsonValue & out)
    {
    if (!Value (out, 0))
      return false;
    Space ();
    return m_p == m_end;
    }

  private:
  void Space ()
    {
    while (m_p < m_end && (*m_p == ' ' || *m_p == '\t' || *m_p == '\r' || *m_p == '\n'))
      m_p++;
    }

  bool Literal (const char * word)
    {
    const size_t n = strlen (word);
    if ((size_t) (m_end - m_p) < n || memcmp (m_p, word, n) != 0)
      return false;
    m_p += n;
    return true;
    }

  bool Hex4 (unsigned & out)
    {
    if (m_end - m_p < 4)
      return false;
    out = 0;
    for (int i = 0; i < 4; i++)
      {
      const char c = *m_p++;
      out <<= 4;
      if (c >= '0' && c <= '9')
        out |= (unsigned) (c - '0');
      else if (c >= 'a' && c <= 'f')
        out |= (unsigned) (c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        out |= (unsigned) (c - 'A' + 10);
      else
        return false;
      }
    return true;
    }

  static void AppendUtf8 (std::string & s, const unsigned c)
    {
    if (c < 0x80)
      s += (char) c;
    else if (c < 0x800)
      {
      s += (char) (0xC0 | (c >> 6));
      s += (char) (0x80 | (c & 0x3F));
      }
    else if (c < 0x10000)
      {
      s += (char) (0xE0 | (c >> 12));
      s += (char) (0x80 | ((c >> 6) & 0x3F));
      s += (char) (0x80 | (c & 0x3F));
      }
    else
      {
      s += (char) (0xF0 | (c >> 18));
      s += (char) (0x80 | ((c >> 12) & 0x3F));
      s += (char) (0x80 | ((c >> 6) & 0x3F));
      s += (char) (0x80 | (c & 0x3F));
      }
    }

  bool StringBody (std::string & s)
    {
    // m_p is just past the opening quote
    while (m_p < m_end)
      {
      const char c = *m_p++;
      if (c == '"')
        return true;
      if ((unsigned char) c < 0x20)
        return false;
      if (c != '\\')
        {
        s += c;
        continue;
        }
      if (m_p == m_end)
        return false;
      const char e = *m_p++;
      switch (e)
        {
        case '"': s += '"'; break;
        case '\\': s += '\\'; break;
        case '/': s += '/'; break;
        case 'b': s += '\b'; break;
        case 'f': s += '\f'; break;
        case 'n': s += '\n'; break;
        case 'r': s += '\r'; break;
        case 't': s += '\t'; break;
        case 'u':
          {
          unsigned code;
          if (!Hex4 (code))
            return false;
          if (code >= 0xD800 && code < 0xDC00 && m_end - m_p >= 6 && m_p [0] == '\\' && m_p [1] == 'u')
            {
            m_p += 2;
            unsigned low;
            if (!Hex4 (low))
              return false;
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
            }
          AppendUtf8 (s, code);
          }
          break;
        default:
          return false;
        }
      }
    return false;
    }

  bool Value (JsonValue & out, const int depth)
    {
    if (depth > 64)
      return false;
    Space ();
    if (m_p == m_end)
      return false;
    switch (*m_p)
      {
      case '{':
        {
        m_p++;
        out.kind = JsonValue::Object;
        Space ();
        if (m_p < m_end && *m_p == '}')
          {
          m_p++;
          return true;
          }
        for (;;)
          {
          Space ();
          if (m_p == m_end || *m_p != '"')
            return false;
          m_p++;
          std::string name;
          if (!StringBody (name))
            return false;
          Space ();
          if (m_p == m_end || *m_p != ':')
            return false;
          m_p++;
          out.members.emplace_back (std::move (name), JsonValue ());
          if (!Value (out.members.back ().second, depth + 1))
            return false;
          Space ();
          if (m_p == m_end)
            return false;
          if (*m_p == ',')
            {
            m_p++;
            continue;
            }
          if (*m_p == '}')
            {
            m_p++;
            return true;
            }
          return false;
          }
        }
      case '[':
        {
        m_p++;
        out.kind = JsonValue::Array;
        Space ();
        if (m_p < m_end && *m_p == ']')
          {
          m_p++;
          return true;
          }
        for (;;)
          {
          out.items.emplace_back ();
          if (!Value (out.items.back (), depth + 1))
            return false;
          Space ();
          if (m_p == m_end)
            return false;
          if (*m_p == ',')
            {
            m_p++;
            continue;
            }
          if (*m_p == ']')
            {
            m_p++;
            return true;
            }
          return false;
          }
        }
      case '"':
        m_p++;
        out.kind = JsonValue::String;
        return StringBody (out.text);
      case 't':
        out.kind = JsonValue::Bool;
        out.boolean = true;
        return Literal ("true");
      case 'f':
        out.kind = JsonValue::Bool;
        return Literal ("false");
      case 'n':
        return Literal ("null");
      default:
        {
        const char * start = m_p;
        while (m_p < m_end && (isdigit ((unsigned char) *m_p) || strchr ("+-.eE", *m_p)))
          m_p++;
        if (m_p == start)
          return false;
        out.kind = JsonValue::Number;
        out.number = strtod (std::string (start, (size_t) (m_p - start)).c_str (), nullptr);
        return true;
        }
      }
    }

  const char * m_p;
  const char * m_end;
  };

} // anonymous namespace

// ---------------- releases ----------------

bool ParseReleases (const std::string & json, std::vector<Release> * out, std::string * error)
  {
  JsonValue root;
  if (!JsonParser (json).Parse (root))
    {
    *error = "GitHub's answer could not be understood";
    return false;
    }
  if (root.kind != JsonValue::Array)
    {
    // It answers with an object when it is refusing: rate limited, for one.
    const std::string why = root.Text ("message");
    *error = why.empty () ? "GitHub's answer could not be understood" : "GitHub says: " + why;
    return false;
    }

  out->clear ();
  for (const JsonValue & item : root.items)
    {
    if (item.kind != JsonValue::Object)
      continue;
    Release r;
    r.tag = item.Text ("tag_name");
    if (!ParseVersion (r.tag, &r.parsed))
      continue;
    r.version = r.tag;
    if (!r.version.empty () && (r.version [0] == 'v' || r.version [0] == 'V'))
      r.version.erase (0, 1);
    r.notes = item.Text ("body");
    r.pageUrl = item.Text ("html_url");
    r.draft = item.Boolean ("draft");
    r.prerelease = item.Boolean ("prerelease");
    if (const JsonValue * assets = item.Find ("assets"))
      for (const JsonValue & a : assets->items)
        {
        Asset asset;
        asset.name = a.Text ("name");
        asset.url = a.Text ("browser_download_url");
        if (const JsonValue * size = a.Find ("size"))
          if (size->kind == JsonValue::Number && size->number > 0)
            asset.size = (uint64_t) size->number;
        if (!asset.name.empty () && !asset.url.empty ())
          r.assets.push_back (std::move (asset));
        }
    out->push_back (std::move (r));
    }
  return true;
  }

std::vector<Release> ReleasesNewerThan (const std::vector<Release> & all, const std::string & installed)
  {
  Version mine;
  std::vector<Release> newer;
  if (!ParseVersion (installed, &mine))
    return newer;
  for (const Release & r : all)
    if (!r.draft && !r.prerelease && CompareVersions (r.parsed, mine) > 0)
      newer.push_back (r);
  std::stable_sort (newer.begin (), newer.end (),
                    [] (const Release & a, const Release & b) { return CompareVersions (a.parsed, b.parsed) > 0; });
  return newer;
  }

namespace {

// Removes a pair of markers from around text, wherever they appear in the line.
std::string StripMarkers (const std::string & line, const std::string & marker)
  {
  std::string out;
  size_t at = 0;
  for (;;)
    {
    const size_t open = line.find (marker, at);
    if (open == std::string::npos)
      break;
    const size_t close = line.find (marker, open + marker.size ());
    if (close == std::string::npos || close == open + marker.size ())
      break;
    // Not an asterisk in a sum, or a snake_case_name: it has to hug what it marks.
    if (isspace ((unsigned char) line [open + marker.size ()]) || isspace ((unsigned char) line [close - 1]))
      {
      out.append (line, at, close - at);
      at = close;
      continue;
      }
    out.append (line, at, open - at);
    out.append (line, open + marker.size (), close - open - marker.size ());
    at = close + marker.size ();
    }
  out.append (line, at, std::string::npos);
  return out;
  }

// [text](url) becomes "text (url)", and ![alt](url) just "alt".
std::string TidyLinks (const std::string & line)
  {
  std::string out;
  size_t at = 0;
  while (at < line.size ())
    {
    const size_t open = line.find ('[', at);
    if (open == std::string::npos)
      break;
    const size_t middle = line.find ("](", open);
    const size_t close = middle == std::string::npos ? std::string::npos : line.find (')', middle);
    if (close == std::string::npos)
      break;
    const bool bImage = open > at && line [open - 1] == '!';
    out.append (line, at, open - at - (bImage ? 1 : 0));
    const std::string text = line.substr (open + 1, middle - open - 1);
    const std::string url = line.substr (middle + 2, close - middle - 2);
    out += text;
    if (!bImage && !url.empty () && url != text)
      out += " (" + url + ")";
    at = close + 1;
    }
  out.append (line, at, std::string::npos);
  return out;
  }

} // anonymous namespace

std::string TidyNotes (const std::string & markdown)
  {
  std::string out;
  bool bInCode = false;
  bool bBlank = true;
  size_t at = 0;
  while (at <= markdown.size ())
    {
    size_t end = markdown.find ('\n', at);
    if (end == std::string::npos)
      end = markdown.size ();
    std::string line = markdown.substr (at, end - at);
    at = end + 1;
    if (!line.empty () && line.back () == '\r')
      line.pop_back ();

    size_t lead = 0;
    while (lead < line.size () && line [lead] == ' ')
      lead++;

    // ``` fences go; what's between them is left exactly as it is.
    if (line.compare (lead, 3, "```") == 0)
      {
      bInCode = !bInCode;
      continue;
      }
    if (bInCode)
      {
      out += "    " + line + "\n";
      bBlank = false;
      continue;
      }

    // # Heading
    size_t hashes = lead;
    while (hashes < line.size () && line [hashes] == '#')
      hashes++;
    if (hashes > lead && hashes < line.size () && line [hashes] == ' ')
      line = line.substr (hashes + 1);

    // --- rules are noise here
    if (line.find_first_not_of ("-=_* ") == std::string::npos && line.size () >= 3)
      line.clear ();

    // "* item" reads as "- item" in plain text
    if (lead + 1 < line.size () && line [lead] == '*' && line [lead + 1] == ' ')
      line [lead] = '-';

    line = TidyLinks (line);
    line = StripMarkers (line, "**");
    line = StripMarkers (line, "__");
    line = StripMarkers (line, "`");

    // One blank line at a time, and none at the top.
    if (line.find_first_not_of (" \t") == std::string::npos)
      {
      if (!bBlank)
        out += '\n';
      bBlank = true;
      continue;
      }
    out += line + "\n";
    bBlank = false;
    }
  while (!out.empty () && out.back () == '\n')
    out.pop_back ();
  return out;
  }

std::string ConcatenateNotes (const std::vector<Release> & releases)
  {
  std::string out;
  for (const Release & r : releases)
    {
    if (!out.empty ())
      out += "\n\n";
    const std::string heading = "Version " + r.version;
    out += heading + "\n" + std::string (heading.size (), '-') + "\n";
    const std::string notes = TidyNotes (r.notes);
    out += notes.empty () ? std::string ("(no notes for this version)") : notes;
    out += "\n";
    }
  return out;
  }

// ---------------- which file is this copy's ----------------

std::string AssetNameFor (const ThisCopy & copy, const std::string & version)
  {
  if (copy.systemManaged)
    return std::string ();
  const std::string edition = copy.discworld ? "-discworld" : "";
  switch (copy.platform)
    {
    case Platform::Windows:
      if (copy.arch.empty ())
        return std::string ();    // 32-bit: no such package
      return "MUSHclient-" + version + "-windows-" + copy.arch + edition + (copy.portable ? ".zip" : "-setup.exe");
    case Platform::MacOS:
      // The .zip, not the .dmg: it can be unpacked without mounting anything.
      return "MUSHclient-" + version + "-macOS-universal" + edition + ".zip";
    case Platform::Linux:
      if (copy.arch.empty ())
        return std::string ();
      return "mushclient" + edition + "-" + version + "-linux-" + copy.arch + ".tar.xz";
    }
  return std::string ();
  }

const Asset * FindAsset (const Release & release, const std::string & name)
  {
  for (const Asset & asset : release.assets)
    if (asset.name == name)
      return &asset;
  return nullptr;
  }

std::string ManifestName (const std::string & version)
  {
  return "MUSHclient-" + version + "-SHA256SUMS.txt";
  }

std::string SignatureName (const std::string & version)
  {
  return ManifestName (version) + ".sig";
  }

// ---------------- signatures and hashes ----------------

#ifdef MC_UPDATE_HAS_CRYPTO

bool CryptoAvailable ()
  {
  return true;
  }

namespace {

bool FromHex (const std::string & hex, std::string & out)
  {
  if (hex.size () % 2)
    return false;
  out.clear ();
  for (size_t i = 0; i < hex.size (); i += 2)
    {
    int value = 0;
    for (int j = 0; j < 2; j++)
      {
      const char c = hex [i + (size_t) j];
      value <<= 4;
      if (c >= '0' && c <= '9')
        value |= c - '0';
      else if (c >= 'a' && c <= 'f')
        value |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F')
        value |= c - 'A' + 10;
      else
        return false;
      }
    out += (char) value;
    }
  return true;
  }

} // anonymous namespace

bool VerifySignature (const std::string & message, const std::string & signature,
                      const std::string & publicKeyHex, std::string * error)
  {
  std::string key;
  if (publicKeyHex.empty ())
    {
    *error = "this copy has no key to check updates with";
    return false;
    }
  if (!FromHex (publicKeyHex, key) || key.size () != 32)
    {
    *error = "the key in this copy is malformed";
    return false;
    }
  if (signature.size () != 64)
    {
    *error = "the release's signature is the wrong size";
    return false;
    }

  std::unique_ptr<EVP_PKEY, decltype (&EVP_PKEY_free)> pkey (
      EVP_PKEY_new_raw_public_key (EVP_PKEY_ED25519, nullptr, (const unsigned char *) key.data (), key.size ()),
      EVP_PKEY_free);
  std::unique_ptr<EVP_MD_CTX, decltype (&EVP_MD_CTX_free)> ctx (EVP_MD_CTX_new (), EVP_MD_CTX_free);
  bool bGood = pkey && ctx &&
               EVP_DigestVerifyInit (ctx.get (), nullptr, nullptr, nullptr, pkey.get ()) == 1 &&
               EVP_DigestVerify (ctx.get (), (const unsigned char *) signature.data (), signature.size (),
                                 (const unsigned char *) message.data (), message.size ()) == 1;
  ERR_clear_error ();
  if (!bGood)
    *error = "the release's signature is not valid";
  return bGood;
  }

bool HashFile (const std::string & path, std::string * hexOut, std::string * error)
  {
  FILE * f = fopen (path.c_str (), "rb");
  if (!f)
    {
    *error = "could not read " + path;
    return false;
    }
  std::unique_ptr<EVP_MD_CTX, decltype (&EVP_MD_CTX_free)> ctx (EVP_MD_CTX_new (), EVP_MD_CTX_free);
  bool bOK = ctx && EVP_DigestInit_ex (ctx.get (), EVP_sha256 (), nullptr) == 1;
  std::vector<unsigned char> buffer (1024 * 1024);
  size_t got;
  while (bOK && (got = fread (buffer.data (), 1, buffer.size (), f)) > 0)
    bOK = EVP_DigestUpdate (ctx.get (), buffer.data (), got) == 1;
  if (ferror (f))
    bOK = false;
  fclose (f);
  unsigned char digest [EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  if (bOK)
    bOK = EVP_DigestFinal_ex (ctx.get (), digest, &length) == 1;
  if (!bOK)
    {
    *error = "could not read " + path;
    return false;
    }
  static const char kDigits [] = "0123456789abcdef";
  hexOut->clear ();
  for (unsigned int i = 0; i < length; i++)
    {
    *hexOut += kDigits [digest [i] >> 4];
    *hexOut += kDigits [digest [i] & 15];
    }
  return true;
  }

#else // no crypto in this build

bool CryptoAvailable ()
  {
  return false;
  }

bool VerifySignature (const std::string &, const std::string &, const std::string &, std::string * error)
  {
  *error = "this build has no way to check a signature";
  return false;
  }

bool HashFile (const std::string &, std::string *, std::string * error)
  {
  *error = "this build has no way to check a file";
  return false;
  }

#endif

bool ManifestHashFor (const std::string & manifest, const std::string & version,
                      const std::string & fileName, std::string * hexOut, std::string * error)
  {
  size_t at = 0;
  bool bFirst = true;
  while (at < manifest.size ())
    {
    size_t end = manifest.find ('\n', at);
    if (end == std::string::npos)
      end = manifest.size ();
    std::string line = manifest.substr (at, end - at);
    at = end + 1;
    if (!line.empty () && line.back () == '\r')
      line.pop_back ();

    if (bFirst)
      {
      bFirst = false;
      if (line != "MUSHclient " + version)
        {
        *error = "the release's signed list of files is for a different version";
        return false;
        }
      continue;
      }

    // sha256sum's format: hash, two spaces (or space-star), name
    const size_t space = line.find (' ');
    if (space == std::string::npos || space + 2 > line.size ())
      continue;
    std::string name = line.substr (space + 2);
    if (name == fileName)
      {
      *hexOut = line.substr (0, space);
      std::transform (hexOut->begin (), hexOut->end (), hexOut->begin (),
                      [] (unsigned char c) { return (char) tolower (c); });
      return true;
      }
    }
  *error = "the release's signed list of files doesn't include " + fileName;
  return false;
  }

bool VerifyDownload (const std::string & path, const std::string & fileName, const std::string & version,
                     const std::string & manifest, const std::string & signature,
                     const std::string & publicKeyHex, std::string * error)
  {
  if (!VerifySignature (manifest, signature, publicKeyHex, error))
    return false;
  std::string expected;
  if (!ManifestHashFor (manifest, version, fileName, &expected, error))
    return false;
  std::string actual;
  if (!HashFile (path, &actual, error))
    return false;
  if (actual != expected)
    {
    *error = fileName + " is not the file that was signed";
    return false;
    }
  return true;
  }

// ---------------- zip ----------------

namespace {

uint32_t Read32 (const unsigned char * p)
  {
  return (uint32_t) p [0] | ((uint32_t) p [1] << 8) | ((uint32_t) p [2] << 16) | ((uint32_t) p [3] << 24);
  }

uint32_t Read16 (const unsigned char * p)
  {
  return (uint32_t) p [0] | ((uint32_t) p [1] << 8);
  }

struct ZipEntry
  {
  std::string name;
  uint32_t    method;
  uint32_t    crc;
  uint32_t    compressedSize;
  uint32_t    size;
  uint32_t    localOffset;
  };

// Nothing absolute, no "..", nothing Windows would treat as a stream or a drive.
bool SafeRelativePath (const std::string & name)
  {
  if (name.empty () || name [0] == '/' || name [0] == '\\' || name.find (':') != std::string::npos)
    return false;
  size_t at = 0;
  while (at <= name.size ())
    {
    size_t end = name.find_first_of ("/\\", at);
    if (end == std::string::npos)
      end = name.size ();
    if (name.compare (at, end - at, "..") == 0)
      return false;
    at = end + 1;
    }
  return true;
  }

bool ReadFileBytes (const std::string & path, std::vector<unsigned char> & out)
  {
  FILE * f = fopen (path.c_str (), "rb");
  if (!f)
    return false;
  bool bOK = fseek (f, 0, SEEK_END) == 0;
  long long size = 0;
  if (bOK)
    {
#ifdef _WIN32
    size = _ftelli64 (f);
#else
    size = ftello (f);
#endif
    bOK = size >= 0 && fseek (f, 0, SEEK_SET) == 0;
    }
  if (bOK)
    {
    out.resize ((size_t) size);
    bOK = out.empty () || fread (out.data (), 1, out.size (), f) == out.size ();
    }
  fclose (f);
  return bOK;
  }

} // anonymous namespace

bool ExtractZip (const std::string & zipPath, const std::string & destDir, std::string * error)
  {
  std::vector<unsigned char> zip;
  if (!ReadFileBytes (zipPath, zip))
    {
    *error = "could not read " + zipPath;
    return false;
    }

  // The end-of-central-directory record, from the end, past any comment.
  const size_t kEndSize = 22;
  size_t endAt = std::string::npos;
  if (zip.size () >= kEndSize)
    {
    const size_t lowest = zip.size () > 65557 ? zip.size () - 65557 : 0;
    for (size_t i = zip.size () - kEndSize + 1; i-- > lowest; )
      if (Read32 (&zip [i]) == 0x06054B50)
        {
        endAt = i;
        break;
        }
    }
  if (endAt == std::string::npos)
    {
    *error = "the update is not a zip file";
    return false;
    }

  const uint32_t count = Read16 (&zip [endAt + 10]);
  uint64_t at = Read32 (&zip [endAt + 16]);
  std::vector<ZipEntry> entries;
  for (uint32_t i = 0; i < count; i++)
    {
    if (at + 46 > zip.size () || Read32 (&zip [(size_t) at]) != 0x02014B50)
      {
      *error = "the update's zip file is damaged";
      return false;
      }
    const unsigned char * p = &zip [(size_t) at];
    ZipEntry e;
    e.method = Read16 (p + 10);
    e.crc = Read32 (p + 16);
    e.compressedSize = Read32 (p + 20);
    e.size = Read32 (p + 24);
    const uint32_t nameLength = Read16 (p + 28);
    const uint32_t extraLength = Read16 (p + 30);
    const uint32_t commentLength = Read16 (p + 32);
    e.localOffset = Read32 (p + 42);
    if (at + 46 + nameLength > zip.size ())
      {
      *error = "the update's zip file is damaged";
      return false;
      }
    e.name.assign ((const char *) p + 46, nameLength);
    std::replace (e.name.begin (), e.name.end (), '\\', '/');
    if (!SafeRelativePath (e.name))
      {
      *error = "the update's zip file has a file that would land outside its folder: " + e.name;
      return false;
      }
    entries.push_back (e);
    at += 46 + (uint64_t) nameLength + extraLength + commentLength;
    }
  if (entries.empty ())
    {
    *error = "the update's zip file is empty";
    return false;
    }

  // One top-level folder over everything?
  std::string top;
  const size_t slash = entries [0].name.find ('/');
  if (slash != std::string::npos)
    {
    top = entries [0].name.substr (0, slash + 1);
    for (const ZipEntry & e : entries)
      if (e.name.compare (0, top.size (), top) != 0)
        {
        top.clear ();
        break;
        }
    }

  const fs::path dest = fs::u8path (destDir);
  std::error_code ec;
  fs::create_directories (dest, ec);

  for (const ZipEntry & e : entries)
    {
    const std::string relative = e.name.substr (top.size ());
    if (relative.empty ())
      continue;
    const fs::path target = dest / fs::u8path (relative);
    if (relative.back () == '/')
      {
      fs::create_directories (target, ec);
      continue;
      }

    if ((uint64_t) e.localOffset + 30 > zip.size () || Read32 (&zip [e.localOffset]) != 0x04034B50)
      {
      *error = "the update's zip file is damaged";
      return false;
      }
    const unsigned char * local = &zip [e.localOffset];
    const uint64_t dataAt = (uint64_t) e.localOffset + 30 + Read16 (local + 26) + Read16 (local + 28);
    if (dataAt + e.compressedSize > zip.size ())
      {
      *error = "the update's zip file is damaged";
      return false;
      }

    std::vector<unsigned char> data (e.size);
    if (e.method == 0)
      {
      if (e.compressedSize != e.size)
        {
        *error = "the update's zip file is damaged";
        return false;
        }
      if (e.size)
        memcpy (data.data (), &zip [(size_t) dataAt], e.size);
      }
    else if (e.method == 8)
      {
      z_stream z;
      memset (&z, 0, sizeof z);
      if (inflateInit2 (&z, -MAX_WBITS) != Z_OK)
        {
        *error = "could not unpack the update";
        return false;
        }
      z.next_in = &zip [(size_t) dataAt];
      z.avail_in = e.compressedSize;
      z.next_out = data.data ();
      z.avail_out = e.size;
      const int result = inflate (&z, Z_FINISH);
      const bool bComplete = result == Z_STREAM_END && z.total_out == e.size;
      inflateEnd (&z);
      if (!bComplete)
        {
        *error = "the update's zip file is damaged: " + e.name;
        return false;
        }
      }
    else
      {
      *error = "the update's zip file uses a compression this can't read";
      return false;
      }

    if ((uint32_t) crc32 (crc32 (0L, Z_NULL, 0), data.data (), e.size) != e.crc)
      {
      *error = "the update's zip file is damaged: " + e.name;
      return false;
      }

    fs::create_directories (target.parent_path (), ec);
#ifdef _WIN32
    FILE * f = _wfopen (target.c_str (), L"wb");
#else
    FILE * f = fopen (target.c_str (), "wb");
#endif
    if (!f)
      {
      *error = "could not write " + relative;
      return false;
      }
    const bool bWritten = e.size == 0 || fwrite (data.data (), 1, e.size, f) == e.size;
    if (fclose (f) != 0 || !bWritten)
      {
      *error = "could not write " + relative;
      return false;
      }
    }
  return true;
  }

// ---------------- swapping ----------------

namespace {

const char kOldSuffix [] = ".mcold";
const char kSwapNote [] = "update-cleanup.txt";

std::string LowerSlashes (std::string s)
  {
  for (char & c : s)
    c = c == '\\' ? '/' : (char) tolower ((unsigned char) c);
  return s;
  }

} // anonymous namespace

bool IsUserDataPath (const std::string & relativePath)
  {
  const std::string path = LowerSlashes (relativePath);

  // The user's folders, and what they keep beside the exe: user_data.h has the list.
  static const char * const kFolders [] = {
    "worlds/", "logs/", "sounds/", "scripts/", "scrollback/", "backups/", "quow_plugins/" };
  for (const char * folder : kFolders)
    if (path.compare (0, strlen (folder), folder) == 0)
      return true;

  static const char * const kFiles [] = {
    "mushclient.ini", "mushclient_prefs.sqlite", "spell.sqlite", "userdict.txt", "portable.txt" };
  for (const char * file : kFiles)
    if (path == file)
      return true;

  return false;
  }

bool SwapInFiles (const std::string & stageDir, const std::string & installDir, std::string * error)
  {
  const fs::path stage = fs::u8path (stageDir);
  const fs::path install = fs::u8path (installDir);

  struct Step
    {
    fs::path target;
    fs::path old;       // empty if there was nothing there
    fs::path from;
    };
  std::vector<Step> steps;
  std::vector<fs::path> addedFolders;
  std::error_code ec;

  auto undo = [&] ()
    {
    std::error_code ignore;
    for (size_t i = steps.size (); i-- > 0; )
      {
      const Step & step = steps [i];
      fs::rename (step.target, step.from, ignore);
      if (!step.old.empty ())
        fs::rename (step.old, step.target, ignore);
      }
    for (size_t i = addedFolders.size (); i-- > 0; )
      fs::remove (addedFolders [i], ignore);
    };

  // First, what's to be done, so that nothing has moved if the list can't be made.
  std::vector<fs::path> files;
  for (fs::recursive_directory_iterator it (stage, ec), end; !ec && it != end; it.increment (ec))
    if (it->is_regular_file (ec))
      files.push_back (it->path ());
  if (ec)
    {
    *error = "could not read the unpacked update";
    return false;
    }
  std::sort (files.begin (), files.end ());

  // A record of what is being replaced goes first, in case the machine goes down
  // halfway.  CleanUpAfterSwap needs only that one exists.
  {
#ifdef _WIN32
  FILE * note = _wfopen ((install / kSwapNote).c_str (), L"wb");
#else
  FILE * note = fopen ((install / kSwapNote).c_str (), "wb");
#endif
  if (!note)
    {
    *error = "the folder this copy is in can't be written to";
    return false;
    }
  fclose (note);
  }

  for (const fs::path & from : files)
    {
    const fs::path relative = fs::relative (from, stage, ec);
    if (ec)
      {
      undo ();
      *error = "could not read the unpacked update";
      return false;
      }
    const fs::path target = install / relative;
    const bool bExists = fs::exists (target, ec);

    if (bExists && IsUserDataPath (relative.generic_u8string ()))
      continue;     // theirs

    // Make any folders it needs, remembering which, deepest last.
    std::vector<fs::path> missing;
    for (fs::path folder = target.parent_path (); !fs::exists (folder, ec); folder = folder.parent_path ())
      missing.push_back (folder);
    for (size_t i = missing.size (); i-- > 0; )
      {
      fs::create_directory (missing [i], ec);
      if (ec)
        {
        undo ();
        *error = "could not create " + missing [i].u8string ();
        return false;
        }
      addedFolders.push_back (missing [i]);
      }

    Step step;
    step.target = target;
    step.from = from;
    if (bExists)
      {
      step.old = target;
      step.old += kOldSuffix;
      fs::remove (step.old, ec);    // left by a swap whose cleanup hasn't happened
      fs::rename (target, step.old, ec);
      if (ec)
        {
        undo ();
        *error = "could not replace " + relative.u8string () + ": it can't be moved";
        return false;
        }
      }
    fs::rename (from, target, ec);
    if (ec)
      {
      // This step didn't complete, so undo can't be left to deal with it.
      if (!step.old.empty ())
        {
        std::error_code ignore;
        fs::rename (step.old, target, ignore);
        }
      undo ();
      *error = "could not put " + relative.u8string () + " in place";
      return false;
      }
    steps.push_back (step);
    }
  return true;
  }

void CleanUpAfterSwap (const std::string & installDir)
  {
  const fs::path install = fs::u8path (installDir);
  const fs::path note = install / kSwapNote;
  std::error_code ec;
  if (!fs::exists (note, ec))
    return;

  bool bAllGone = true;
  std::vector<fs::path> old;
  for (fs::recursive_directory_iterator it (install, ec), end; !ec && it != end; it.increment (ec))
    {
    const std::string name = it->path ().filename ().u8string ();
    const size_t n = sizeof kOldSuffix - 1;
    if (name.size () > n && name.compare (name.size () - n, n, kOldSuffix) == 0)
      old.push_back (it->path ());
    }
  for (const fs::path & p : old)
    {
    std::error_code removeError;
    if (!fs::remove (p, removeError) || removeError)
      bAllGone = false;
    }
  if (bAllGone)
    fs::remove (note, ec);
  }

} // namespace update

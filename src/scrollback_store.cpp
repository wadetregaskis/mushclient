// scrollback_store.cpp -- see scrollback_store.h.

#include "stdafx.h"
#include "scrollback_store.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <functional>
#include <map>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// stdafx.h has it on Windows and the port.  Included by name for clarity.
#ifndef ZLIB_VERSION
#include <zlib.h>
#endif

namespace scrollback {

namespace {

const char     kFileMagic [8] = { 'M', 'C', 'S', 'C', 'R', 'B', 'K', 0 };
const size_t   kFileHeaderSize = 20;

const uint32_t kRecordMagic = 0x4252434D;    // "MCRB"
const size_t   kRecordHeaderSize = 52;

const uint32_t kRecordLines    = 1;
const uint32_t kRecordTruncate = 2;
const uint32_t kRecordFlags    = 3;

const size_t   kBlockFixedSize = 24;         // six counts at the start of a block

// A block nothing could legitimately write, but which a damaged length might claim.
const uint32_t kMaxRawSize = 1u << 30;

// zlib's own default: 6 gives about 15% smaller blocks than 1, for three times the
// time, and that time is spent on the worker, once, while reading is unaffected.
const int kCompressionLevel = 6;

const size_t   kSmallBlockLines = kBlockLines / 2;  // smaller than this is worth merging
const size_t   kMergeAfterBlocks = 32;              // ... once there are this many in a row
const size_t   kMaxSmallBlocks = 256;               // reclaim if more than this pile up
const uint64_t kMinDeadBytes = 1024 * 1024;         // don't rewrite the file for less
const size_t   kReclaimRetryAfter = 30;             // chances to let go by after a failure
const size_t   kMaxHeldOps = 1000;                  // while the file can't be opened again
const size_t   kMaxHeldBytes = 64 * 1024 * 1024;

void Put32 (std::vector<uint8_t> & out, uint32_t v)
  {
  for (int i = 0; i < 4; i++)
    out.push_back ((uint8_t) (v >> (8 * i)));
  }

void Put64 (std::vector<uint8_t> & out, int64_t value)
  {
  const uint64_t v = (uint64_t) value;
  for (int i = 0; i < 8; i++)
    out.push_back ((uint8_t) (v >> (8 * i)));
  }

uint32_t Get32 (const uint8_t * p)
  {
  return (uint32_t) p [0] | ((uint32_t) p [1] << 8) | ((uint32_t) p [2] << 16) | ((uint32_t) p [3] << 24);
  }

int64_t Get64 (const uint8_t * p)
  {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++)
    v |= (uint64_t) p [i] << (8 * i);
  return (int64_t) v;
  }

void PutVarint (std::vector<uint8_t> & out, uint64_t v)
  {
  while (v >= 0x80)
    {
    out.push_back ((uint8_t) (v | 0x80));
    v >>= 7;
    }
  out.push_back ((uint8_t) v);
  }

uint64_t ZigZag (int64_t v)
  {
  return ((uint64_t) v << 1) ^ (uint64_t) (v >> 63);
  }

int64_t UnZigZag (uint64_t v)
  {
  return (int64_t) (v >> 1) ^ -(int64_t) (v & 1);
  }

class Reader
  {
  public:
  Reader (const uint8_t * p, size_t size) : m_p (p), m_end (p + size) {}

  bool Varint (uint64_t & v)
    {
    v = 0;
    for (int shift = 0; shift < 64; shift += 7)
      {
      if (m_p == m_end)
        return false;
      const uint8_t b = *m_p++;
      v |= (uint64_t) (b & 0x7F) << shift;
      if (!(b & 0x80))
        return true;
      }
    return false;
    }

  bool String (std::string & s)
    {
    uint64_t n;
    if (!Varint (n) || n > (uint64_t) (m_end - m_p))
      return false;
    s.assign ((const char *) m_p, (size_t) n);
    m_p += n;
    return true;
    }

  bool AtEnd () const { return m_p == m_end; }

  private:
  const uint8_t * m_p;
  const uint8_t * m_end;
  };

uint32_t Crc (const void * data, size_t size)
  {
  return (uint32_t) crc32 (crc32 (0L, Z_NULL, 0), (const Bytef *) data, (uInt) size);
  }

double SecondsSince (std::chrono::steady_clock::time_point start)
  {
  return std::chrono::duration<double> (std::chrono::steady_clock::now () - start).count ();
  }

// ---------------- the file, one way or the other ----------------

#ifdef _WIN32

// FILE_SHARE_READ only: a second writer gets a sharing violation, which is what
// makes one world in two copies of the client safe.
HANDLE OpenExclusive (const std::string & path, DWORD disposition)
  {
  return CreateFileA (path.c_str (), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
                      disposition, FILE_ATTRIBUTE_NORMAL, NULL);
  }

void * FileOpen (const std::string & path, bool bCreateNew)
  {
  const HANDLE h = OpenExclusive (path, bCreateNew ? CREATE_ALWAYS : OPEN_ALWAYS);
  return h == INVALID_HANDLE_VALUE ? nullptr : (void *) h;
  }

void FileClose (void * f)
  {
  CloseHandle ((HANDLE) f);
  }

bool FileWriteAt (void * f, uint64_t at, const void * data, size_t size)
  {
  LARGE_INTEGER where;
  where.QuadPart = (LONGLONG) at;
  if (!SetFilePointerEx ((HANDLE) f, where, NULL, FILE_BEGIN))
    return false;
  const char * p = (const char *) data;
  while (size > 0)
    {
    DWORD written = 0;
    const DWORD chunk = (DWORD) (std::min) (size, (size_t) 64 * 1024 * 1024);
    if (!WriteFile ((HANDLE) f, p, chunk, &written, NULL) || written == 0)
      return false;
    p += written;
    size -= written;
    }
  return true;
  }

bool FileReadAt (void * f, uint64_t at, void * data, size_t size)
  {
  LARGE_INTEGER where;
  where.QuadPart = (LONGLONG) at;
  if (!SetFilePointerEx ((HANDLE) f, where, NULL, FILE_BEGIN))
    return false;
  char * p = (char *) data;
  while (size > 0)
    {
    DWORD got = 0;
    const DWORD chunk = (DWORD) (std::min) (size, (size_t) 64 * 1024 * 1024);
    if (!ReadFile ((HANDLE) f, p, chunk, &got, NULL) || got == 0)
      return false;
    p += got;
    size -= got;
    }
  return true;
  }

bool FileTruncate (void * f, uint64_t size)
  {
  LARGE_INTEGER where;
  where.QuadPart = (LONGLONG) size;
  return SetFilePointerEx ((HANDLE) f, where, NULL, FILE_BEGIN) && SetEndOfFile ((HANDLE) f);
  }

bool FileSync (void * f)
  {
  return FlushFileBuffers ((HANDLE) f) != 0;
  }

// A file can't be replaced while anything has it open, and something often does for
// a moment: a virus scanner or the indexer, taking a look at the new file.
bool RenameOver (const std::string & from, const std::string & to)
  {
  for (int attempt = 0; ; attempt++)
    {
    if (MoveFileExA (from.c_str (), to.c_str (), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      return true;
    const DWORD e = GetLastError ();
    if ((e != ERROR_ACCESS_DENIED && e != ERROR_SHARING_VIOLATION) || attempt >= 20)
      return false;
    Sleep (50);
    }
  }

void DeleteTheFile (const std::string & path)
  {
  DeleteFileA (path.c_str ());
  }

// One sequential read, which is the point: Windows reads ahead accordingly.
bool ReadWholeFile (const std::string & path, std::vector<uint8_t> & out, bool * pbMissing)
  {
  const HANDLE h = CreateFileA (path.c_str (), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
  if (h == INVALID_HANDLE_VALUE)
    {
    const DWORD e = GetLastError ();
    *pbMissing = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
    return false;
    }
  LARGE_INTEGER size;
  bool bOK = GetFileSizeEx (h, &size) != 0;
  if (bOK)
    {
    out.resize ((size_t) size.QuadPart);
    size_t at = 0;
    while (at < out.size ())
      {
      DWORD got = 0;
      const DWORD chunk = (DWORD) (std::min) (out.size () - at, (size_t) 64 * 1024 * 1024);
      if (!ReadFile (h, out.data () + at, chunk, &got, NULL))
        {
        bOK = false;
        break;
        }
      if (got == 0)
        {
        out.resize (at);   // it shrank; what we have is what there is
        break;
        }
      at += got;
      }
    }
  CloseHandle (h);
  return bOK;
  }

#else   // POSIX

void * FileOpen (const std::string & path, bool bCreateNew)
  {
  const int fd = open (path.c_str (), O_RDWR | O_CREAT | O_CLOEXEC | (bCreateNew ? O_TRUNC : 0), 0600);
  if (fd < 0)
    return nullptr;
  if (flock (fd, LOCK_EX | LOCK_NB) != 0)
    {
    close (fd);
    return nullptr;
    }
  return (void *) (intptr_t) (fd + 1);
  }

static int FdOf (void * f)
  {
  return (int) (intptr_t) f - 1;
  }

void FileClose (void * f)
  {
  close (FdOf (f));
  }

bool FileWriteAt (void * f, uint64_t at, const void * data, size_t size)
  {
  const char * p = (const char *) data;
  while (size > 0)
    {
    const ssize_t n = pwrite (FdOf (f), p, size, (off_t) at);
    if (n <= 0)
      return false;
    p += n;
    at += (uint64_t) n;
    size -= (size_t) n;
    }
  return true;
  }

bool FileReadAt (void * f, uint64_t at, void * data, size_t size)
  {
  char * p = (char *) data;
  while (size > 0)
    {
    const ssize_t n = pread (FdOf (f), p, size, (off_t) at);
    if (n <= 0)
      return false;
    p += n;
    at += (uint64_t) n;
    size -= (size_t) n;
    }
  return true;
  }

bool FileTruncate (void * f, uint64_t size)
  {
  return ftruncate (FdOf (f), (off_t) size) == 0;
  }

bool FileSync (void * f)
  {
  return fsync (FdOf (f)) == 0;
  }

bool RenameOver (const std::string & from, const std::string & to)
  {
  return rename (from.c_str (), to.c_str ()) == 0;
  }

void DeleteTheFile (const std::string & path)
  {
  unlink (path.c_str ());
  }

bool ReadWholeFile (const std::string & path, std::vector<uint8_t> & out, bool * pbMissing)
  {
  const int fd = open (path.c_str (), O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    {
    *pbMissing = errno == ENOENT || errno == ENOTDIR;
    return false;
    }
  struct stat st;
  bool bOK = fstat (fd, &st) == 0;
  if (bOK)
    {
    out.resize ((size_t) st.st_size);
    size_t at = 0;
    while (at < out.size ())
      {
      const ssize_t n = read (fd, out.data () + at, out.size () - at);
      if (n < 0)
        {
        bOK = false;
        break;
        }
      if (n == 0)
        {
        out.resize (at);
        break;
        }
      at += (size_t) n;
      }
    }
  close (fd);
  return bOK;
  }

#endif

std::vector<uint8_t> MakeFileHeader (bool bUtf8)
  {
  std::vector<uint8_t> h (kFileMagic, kFileMagic + sizeof kFileMagic);
  Put32 (h, kFormatVersion);
  Put32 (h, bUtf8 ? 1 : 0);
  Put32 (h, Crc (h.data (), h.size ()));
  return h;
  }

// A complete record, ready to be written.
std::vector<uint8_t> MakeRecord (uint32_t kind, const std::vector<uint8_t> * raw, uint32_t lineCount,
                                 int64_t first, int64_t last, int64_t replaceFrom)
  {
  std::vector<uint8_t> compressed;
  uint32_t rawSize = 0;
  uint32_t payloadCrc = 0;
  if (raw && !raw->empty ())
    {
    rawSize = (uint32_t) raw->size ();
    uLongf size = compressBound ((uLong) raw->size ());
    compressed.resize (size);
    if (compress2 (compressed.data (), &size, raw->data (), (uLong) raw->size (), kCompressionLevel) != Z_OK)
      return std::vector<uint8_t> ();
    compressed.resize (size);
    payloadCrc = Crc (compressed.data (), compressed.size ());
    }

  std::vector<uint8_t> record;
  record.reserve (kRecordHeaderSize + compressed.size ());
  Put32 (record, kRecordMagic);
  Put32 (record, kind);
  Put32 (record, (uint32_t) compressed.size ());
  Put32 (record, rawSize);
  Put32 (record, lineCount);
  Put32 (record, payloadCrc);
  Put64 (record, first);
  Put64 (record, last);
  Put64 (record, replaceFrom);
  Put32 (record, Crc (record.data (), record.size ()));
  record.insert (record.end (), compressed.begin (), compressed.end ());
  return record;
  }

// What a record header says.  False if it isn't one, or goes past the end of the file.
struct RecordHeader
  {
  uint32_t kind;
  uint32_t compressedSize;
  uint32_t rawSize;
  uint32_t lineCount;
  uint32_t payloadCrc;
  int64_t  first;
  int64_t  last;
  int64_t  replaceFrom;
  };

bool ParseRecordHeader (const uint8_t * p, size_t available, RecordHeader & h)
  {
  if (available < kRecordHeaderSize ||
      Get32 (p) != kRecordMagic ||
      Get32 (p + kRecordHeaderSize - 4) != Crc (p, kRecordHeaderSize - 4))
    return false;
  h.kind           = Get32 (p + 4);
  h.compressedSize = Get32 (p + 8);
  h.rawSize        = Get32 (p + 12);
  h.lineCount      = Get32 (p + 16);
  h.payloadCrc     = Get32 (p + 20);
  h.first          = Get64 (p + 24);
  h.last           = Get64 (p + 32);
  h.replaceFrom    = Get64 (p + 40);
  return h.compressedSize <= available - kRecordHeaderSize && h.rawSize <= kMaxRawSize;
  }

// The next good record header at or after `from`, or 0 if there isn't one.  A header
// is its magic number and then a checksum of its own, so text that happens to contain
// the magic number won't pass for one.
size_t FindRecordHeader (const uint8_t * file, size_t size, size_t from)
  {
  const uint8_t magic [4] = { (uint8_t) kRecordMagic, (uint8_t) (kRecordMagic >> 8),
                              (uint8_t) (kRecordMagic >> 16), (uint8_t) (kRecordMagic >> 24) };
  for (size_t at = from; at + kRecordHeaderSize <= size; at++)
    {
    const uint8_t * p = (const uint8_t *) memchr (file + at, magic [0], size - at);
    if (!p)
      return 0;
    at = (size_t) (p - file);
    if (at + kRecordHeaderSize > size)
      return 0;
    RecordHeader h;
    if (memcmp (p, magic, sizeof magic) == 0 && ParseRecordHeader (p, size - at, h))
      return at;
    }
  return 0;
  }

// The logical effect of "forget every line numbered >= number".
void ApplyTruncate (std::vector<BlockInfo> & blocks, int64_t number)
  {
  while (!blocks.empty () && blocks.back ().first >= number)
    blocks.pop_back ();
  if (blocks.empty () || blocks.back ().last < number)
    return;
  BlockInfo & b = blocks.back ();
  b.cutFrom = b.cutFrom ? (std::min) (b.cutFrom, number) : number;
  b.flagChanges.erase (std::remove_if (b.flagChanges.begin (), b.flagChanges.end (),
                                       [number] (const std::pair<int64_t, uint8_t> & c) { return c.first >= number; }),
                       b.flagChanges.end ());
  }

void ApplyFlags (std::vector<BlockInfo> & blocks, int64_t number, uint8_t flags)
  {
  for (size_t i = blocks.size (); i > 0; i--)
    {
    BlockInfo & b = blocks [i - 1];
    if (number >= b.first && number <= b.last)
      {
      if (!b.cutFrom || number < b.cutFrom)
        b.flagChanges.emplace_back (number, flags);
      return;
      }
    if (b.last < number)
      return;     // gone
    }
  }

bool ParseBlock (DecodedBlock & out);

// A record's block, inflated and parsed.  Works on the record's bytes wherever they
// came from.
bool InflateAndDecode (const uint8_t * record, size_t size, DecodedBlock & out)
  {
  RecordHeader h;
  if (!ParseRecordHeader (record, size, h) || h.kind != kRecordLines)
    return false;
  const uint8_t * payload = record + kRecordHeaderSize;
  if (Crc (payload, h.compressedSize) != h.payloadCrc)
    return false;
  out.raw.resize (h.rawSize);
  uLongf rawSize = h.rawSize;
  if (uncompress ((Bytef *) out.raw.data (), &rawSize, payload, h.compressedSize) != Z_OK || rawSize != h.rawSize)
    return false;
  return ParseBlock (out);
  }

// Copies a line from a decoded block to a builder, mapping its actions across.
void CopyLine (const DecodedBlock & from, const Line & line, std::vector<int32_t> & actionMap, BlockBuilder & to)
  {
  to.AddLine (line.number, line.time, line.flags, line.hardReturn, line.charCount, line.preamble,
              from.text + line.textOffset, line.length);
  for (uint32_t s = 0; s < line.styleCount; s++)
    {
    const Style & style = from.styles [line.firstStyle + s];
    int32_t action = -1;
    if (style.action >= 0)
      {
      int32_t & mapped = actionMap [(size_t) style.action];
      if (mapped < 0)
        {
        const Action & a = from.actions [(size_t) style.action];
        mapped = to.AddAction (a.action, a.hint, a.variable);
        }
      action = mapped;
      }
    to.AddStyle (style.length, style.flags, style.fore, style.back, action);
    }
  }

} // anonymous namespace

// ---------------- encoding a block ----------------

void BlockBuilder::AddLine (int64_t number, int64_t time, uint8_t flags, bool hardReturn,
                            int32_t charCount, int32_t preamble, const char * text, size_t length)
  {
  if (m_lines > 0)
    PutVarint (m_meta, m_pendingStyles);   // the count for the line before this one
  m_pendingStyles = 0;

  if (m_lines == 0)
    m_first = number;
  m_last = number;
  m_lines++;

  PutVarint (m_meta, ZigZag (number - m_previousNumber));
  m_previousNumber = number;
  PutVarint (m_meta, ZigZag (time - m_previousTime));
  m_previousTime = time;
  PutVarint (m_meta, (uint64_t) flags | (hardReturn ? 0x100u : 0u));
  PutVarint (m_meta, length);
  PutVarint (m_meta, ZigZag ((int64_t) length - charCount));
  PutVarint (m_meta, ZigZag (preamble));

  m_text.insert (m_text.end (), text, text + length);
  }

int32_t BlockBuilder::AddAction (const std::string & action, const std::string & hint, const std::string & variable)
  {
  for (size_t i = 0; i < m_actions.size (); i++)
    if (m_actions [i].action == action && m_actions [i].hint == hint && m_actions [i].variable == variable)
      return (int32_t) i;

  m_actions.push_back (Action { action, hint, variable });
  for (const std::string * s : { &action, &hint, &variable })
    {
    PutVarint (m_actionBytes, s->size ());
    m_actionBytes.insert (m_actionBytes.end (), s->begin (), s->end ());
    }
  return (int32_t) m_actions.size () - 1;
  }

void BlockBuilder::AddStyle (uint32_t length, uint32_t flags, uint32_t fore, uint32_t back, int32_t action)
  {
  PutVarint (m_styles, length);
  PutVarint (m_styles, flags);
  PutVarint (m_styles, fore);
  PutVarint (m_styles, back);
  PutVarint (m_styles, (uint64_t) (action + 1));
  m_pendingStyles++;
  m_styleTotal++;
  }

std::vector<uint8_t> BlockBuilder::Finish ()
  {
  if (m_lines > 0)
    PutVarint (m_meta, m_pendingStyles);

  std::vector<uint8_t> out;
  out.reserve (kBlockFixedSize + m_meta.size () + m_styles.size () + m_actionBytes.size () + m_text.size ());
  Put32 (out, (uint32_t) m_lines);
  Put32 (out, (uint32_t) m_styleTotal);
  Put32 (out, (uint32_t) m_actions.size ());
  Put32 (out, (uint32_t) m_meta.size ());
  Put32 (out, (uint32_t) m_styles.size ());
  Put32 (out, (uint32_t) m_actionBytes.size ());
  out.insert (out.end (), m_meta.begin (), m_meta.end ());
  out.insert (out.end (), m_styles.begin (), m_styles.end ());
  out.insert (out.end (), m_actionBytes.begin (), m_actionBytes.end ());
  out.insert (out.end (), m_text.begin (), m_text.end ());

  *this = BlockBuilder ();
  return out;
  }

bool DecodeBlock (const uint8_t * raw, size_t size, DecodedBlock & out)
  {
  out.raw.assign ((const char *) raw, (const char *) raw + size);
  return ParseBlock (out);
  }

namespace {

// out.raw holds the block's bytes; the rest of out is filled in from them.
bool ParseBlock (DecodedBlock & out)
  {
  const uint8_t * raw = (const uint8_t *) out.raw.data ();
  const size_t size = out.raw.size ();
  if (size < kBlockFixedSize)
    return false;
  const uint32_t nLines   = Get32 (raw);
  const uint32_t nStyles  = Get32 (raw + 4);
  const uint32_t nActions = Get32 (raw + 8);
  const uint64_t metaLen  = Get32 (raw + 12);
  const uint64_t styleLen = Get32 (raw + 16);
  const uint64_t actionLen = Get32 (raw + 20);
  if (metaLen + styleLen + actionLen > size - kBlockFixedSize)
    return false;
  // Each line takes at least 7 bytes of meta, each style 5 and each action 3, so a
  // damaged count can't ask for an enormous allocation.
  if (nLines > metaLen / 7 || nStyles > styleLen / 5 || nActions > actionLen / 3)
    return false;

  const uint8_t * meta    = raw + kBlockFixedSize;
  const uint8_t * styles  = meta + metaLen;
  const uint8_t * actions = styles + styleLen;
  const uint8_t * text    = actions + actionLen;
  const size_t textSize   = (size_t) (raw + size - text);

  out.actions.resize (nActions);
  Reader actionReader (actions, (size_t) actionLen);
  for (Action & a : out.actions)
    if (!actionReader.String (a.action) || !actionReader.String (a.hint) || !actionReader.String (a.variable))
      return false;

  out.styles.resize (nStyles);
  Reader styleReader (styles, (size_t) styleLen);
  for (Style & s : out.styles)
    {
    uint64_t length, flags, fore, back, action;
    if (!styleReader.Varint (length) || !styleReader.Varint (flags) || !styleReader.Varint (fore) ||
        !styleReader.Varint (back) || !styleReader.Varint (action) || action > nActions)
      return false;
    s.length = (uint32_t) length;
    s.flags  = (uint32_t) flags;
    s.fore   = (uint32_t) fore;
    s.back   = (uint32_t) back;
    s.action = (int32_t) action - 1;
    }

  out.lines.resize (nLines);
  Reader metaReader (meta, (size_t) metaLen);
  int64_t number = 0;
  int64_t time = 0;
  uint64_t textAt = 0;
  uint64_t styleAt = 0;
  for (Line & line : out.lines)
    {
    uint64_t dNumber, dTime, flags, length, charDelta, preamble, styleCount;
    if (!metaReader.Varint (dNumber) || !metaReader.Varint (dTime) || !metaReader.Varint (flags) ||
        !metaReader.Varint (length) || !metaReader.Varint (charDelta) || !metaReader.Varint (preamble) ||
        !metaReader.Varint (styleCount))
      return false;
    if (length > textSize - textAt || styleCount > nStyles - styleAt)
      return false;
    number += UnZigZag (dNumber);
    time   += UnZigZag (dTime);
    line.number     = number;
    line.time       = time;
    line.flags      = (uint8_t) flags;
    line.hardReturn = (flags & 0x100) != 0;
    line.length     = (uint32_t) length;
    line.textOffset = (uint32_t) textAt;
    line.charCount  = (int32_t) ((int64_t) length - UnZigZag (charDelta));
    line.preamble   = (int32_t) UnZigZag (preamble);
    line.firstStyle = (uint32_t) styleAt;
    line.styleCount = (uint32_t) styleCount;
    textAt  += length;
    styleAt += styleCount;
    }
  if (!metaReader.AtEnd () || !styleReader.AtEnd () || !actionReader.AtEnd () ||
      textAt != textSize || styleAt != nStyles)
    return false;

  out.text = out.raw.data () + (text - raw);
  out.textSize = textSize;
  return true;
  }

} // anonymous namespace

void ApplyToBlock (const BlockInfo & info, DecodedBlock & block)
  {
  if (info.cutFrom)
    while (!block.lines.empty () && block.lines.back ().number >= info.cutFrom)
      block.lines.pop_back ();

  for (const std::pair<int64_t, uint8_t> & change : info.flagChanges)
    {
    const auto it = std::lower_bound (block.lines.begin (), block.lines.end (), change.first,
                                      [] (const Line & l, int64_t n) { return l.number < n; });
    if (it != block.lines.end () && it->number == change.first)
      it->flags = change.second;
    }
  }

// ---------------- loading ----------------

namespace {

LoadResult LoadFile (const std::string & path, bool bUtf8, size_t maxLines, unsigned threads);

} // anonymous namespace

LoadResult Load (const std::string & path, bool bUtf8, size_t maxLines, unsigned threads)
  {
  try
    {
    return LoadFile (path, bUtf8, maxLines, threads);
    }
  catch (...)     // no memory even for the file: leave it be, and carry on without
    {
    LoadResult result;
    result.status = LoadStatus::CouldNotRead;
    result.outOfMemory = true;
    return result;
    }
  }

namespace {

LoadResult LoadFile (const std::string & path, bool bUtf8, size_t maxLines, unsigned threads)
  {
  LoadResult result;

  const auto readStart = std::chrono::steady_clock::now ();
  std::vector<uint8_t> file;
  bool bMissing = false;
  if (!ReadWholeFile (path, file, &bMissing))
    {
    result.status = bMissing ? LoadStatus::NoFile : LoadStatus::CouldNotRead;
    return result;
    }
  result.readSeconds = SecondsSince (readStart);
  result.fileSize = file.size ();

  if (file.empty ())
    return result;      // NoFile: a crash between creating it and the first write

  if (file.size () < kFileHeaderSize ||
      memcmp (file.data (), kFileMagic, sizeof kFileMagic) != 0 ||
      Get32 (file.data () + kFileHeaderSize - 4) != Crc (file.data (), kFileHeaderSize - 4))
    {
    result.status = LoadStatus::Unreadable;
    return result;
    }
  if (Get32 (file.data () + 8) > kFormatVersion)
    {
    result.status = LoadStatus::NewerVersion;
    return result;
    }
  if (((Get32 (file.data () + 12) & 1) != 0) != bUtf8)
    {
    result.status = LoadStatus::WrongEncoding;
    return result;
    }

  // Walk the records.  This touches only their headers.
  const auto decodeStart = std::chrono::steady_clock::now ();
  std::vector<BlockInfo> blocks;
  size_t at = kFileHeaderSize;
  while (at < file.size ())
    {
    RecordHeader h;
    if (!ParseRecordHeader (file.data () + at, file.size () - at, h))
      {
      // A torn last record, or damage in the middle.  Only another good header can
      // tell them apart: if there is one further on, what lies between is lost and
      // the rest stands, and must not be cut off as a torn tail would be.
      const size_t next = FindRecordHeader (file.data (), file.size (), at + 1);
      if (next == 0)
        {
        result.tornTail = true;
        break;
        }
      result.badBlocks++;
      at = next;
      continue;
      }
    const size_t recordSize = kRecordHeaderSize + h.compressedSize;
    switch (h.kind)
      {
      case kRecordLines:
        {
        // A block replaces whatever it overlaps: a merged block says so, and a block
        // whose numbers go back over earlier ones can only follow a TRUNCATE record
        // that has been lost.  Either way, only if its own lines are there to replace
        // them with: a header that made it to the disk without its payload must not
        // cost the blocks it was to replace.
        const int64_t replaceFrom = h.replaceFrom ? h.replaceFrom
                                  : (!blocks.empty () && blocks.back ().last >= h.first) ? h.first : 0;
        if (replaceFrom)
          {
          if (Crc (file.data () + at + kRecordHeaderSize, h.compressedSize) != h.payloadCrc)
            {
            result.badBlocks++;
            break;
            }
          ApplyTruncate (blocks, replaceFrom);
          }
        BlockInfo info;
        info.offset = at;
        info.recordSize = recordSize;
        info.lineCount = h.lineCount;
        info.rawSize = h.rawSize;
        info.first = h.first;
        info.last = h.last;
        blocks.push_back (std::move (info));
        }
        break;
      case kRecordTruncate:
        ApplyTruncate (blocks, h.replaceFrom);
        break;
      case kRecordFlags:
        ApplyFlags (blocks, h.first, (uint8_t) h.lineCount);
        break;
      default:
        break;      // a kind we don't know: its header says how to step over it
      }
    at += recordSize;
    }
  result.validEnd = at;

  // Only the newest blocks are needed if there are more lines than the buffer holds.
  // A block with lines cut off counts for nothing here, so this can't stop short.
  // A block that turns out to be damaged doesn't count either, so then older ones are
  // fetched in its place, until there are enough lines or no more blocks.
  enum State : uint8_t { kNotTried, kGood, kBad, kNoMemory };
  std::vector<State> state (blocks.size (), kNotTried);
  std::vector<std::unique_ptr<DecodedBlock>> decoded (blocks.size ());
  if (threads == 0)
    threads = ThreadsToUse ();

  size_t firstNeeded = blocks.size ();
  size_t lines = 0;
  for (;;)
    {
    const size_t end = firstNeeded;
    while (firstNeeded > 0 && (maxLines == 0 || lines < maxLines))
      {
      firstNeeded--;
      if (!blocks [firstNeeded].cutFrom)
        lines += blocks [firstNeeded].lineCount;
      }
    if (firstNeeded == end)
      break;

    // Inflate and parse.  Blocks are independent, so this is plain parallel work, and it
    // goes newest first: if memory runs out, the oldest are the ones to do without, and
    // once it has, nothing older than the block it ran out on is started.
    const size_t count = end - firstNeeded;
    std::atomic<size_t> next (0);
    std::atomic<size_t> noMemoryAbove (0);    // 1 + the newest block that ran out, or 0
    auto decodeOne = [&] (const size_t i)
      {
      const BlockInfo & info = blocks [i];
      // Out of memory (a huge file in a 32-bit process) costs blocks, and not the
      // client: an exception leaving a thread would end the process.
      try
        {
        std::unique_ptr<DecodedBlock> block (new DecodedBlock);
        if (InflateAndDecode (file.data () + info.offset, (size_t) info.recordSize, *block))
          {
          ApplyToBlock (info, *block);
          decoded [i] = std::move (block);
          state [i] = kGood;
          }
        else
          state [i] = kBad;
        }
      catch (...)
        {
        decoded [i].reset ();
        state [i] = kNoMemory;
        }
      };
    auto worker = [&] ()
      {
      for (;;)
        {
        const size_t n = next.fetch_add (1);
        if (n >= count)
          return;
        const size_t i = end - 1 - n;
        if (i + 1 < noMemoryAbove.load ())
          continue;
        decodeOne (i);
        if (state [i] == kNoMemory)
          {
          size_t seen = noMemoryAbove.load ();
          while (seen < i + 1 && !noMemoryAbove.compare_exchange_weak (seen, i + 1))
            {
            }
          }
        }
      };
    const unsigned nThreads = (unsigned) (std::min) ((size_t) threads, count);
    std::vector<std::thread> pool;
    try
      {
      for (unsigned t = 1; t < nThreads; t++)
        pool.emplace_back (worker);
      }
    catch (...)
      {
      }     // fewer threads, then
    worker ();
    for (std::thread & t : pool)
      t.join ();

    // Other threads were decoding older blocks when one ran out: what they hold is no use
    // past the gap, and may be what it ran out for.  Let it go, and carry on from that
    // block one at a time, until there's no more room.
    bool bNoMemory = false;
    if (noMemoryAbove.load () > 0)
      {
      const size_t newest = noMemoryAbove.load () - 1;
      for (size_t i = firstNeeded; i < newest; i++)
        {
        decoded [i].reset ();
        state [i] = kNotTried;
        }
      for (size_t i = newest + 1; i-- > firstNeeded; )
        {
        decodeOne (i);
        if (state [i] == kNoMemory)
          {
          bNoMemory = true;
          break;
          }
        }
      }

    bool bShort = false;
    for (size_t i = firstNeeded; i < end; i++)
      if (state [i] != kGood && !blocks [i].cutFrom)
        {
        lines -= blocks [i].lineCount;
        bShort |= state [i] == kBad;
        }
    // Short of memory is not a reason to try for more of it.
    if (!bShort || maxLines == 0 || bNoMemory)
      break;
    }

  // What came back stops at the newest block that couldn't be held in memory, so that
  // there is no gap in it.  The rest of the file is kept: it's not damaged.
  size_t firstKept = firstNeeded;
  for (size_t i = blocks.size (); i > firstNeeded; i--)
    if (state [i - 1] == kNoMemory)
      {
      firstKept = i;
      result.outOfMemory = true;
      break;
      }

  // Bad blocks go from the Writer's list as well, so a rewrite won't trip over them.
  std::vector<BlockInfo> good;
  good.reserve (blocks.size ());
  for (size_t i = 0; i < blocks.size (); i++)
    {
    if (state [i] == kBad)
      {
      result.badBlocks++;
      continue;
      }
    good.push_back (blocks [i]);
    if (i >= firstKept && decoded [i] && !decoded [i]->lines.empty ())
      {
      result.lineCount += decoded [i]->lines.size ();
      result.blocks.push_back (std::move (decoded [i]));
      }
    }
  blocks.swap (good);

  // Trim to exactly the newest maxLines.
  if (maxLines && result.lineCount > maxLines)
    {
    size_t excess = result.lineCount - maxLines;
    while (!result.blocks.empty () && excess >= result.blocks.front ()->lines.size ())
      {
      excess -= result.blocks.front ()->lines.size ();
      result.lineCount -= result.blocks.front ()->lines.size ();
      result.blocks.erase (result.blocks.begin ());
      }
    result.firstLineInFirstBlock = excess;
    result.lineCount -= excess;
    }

  result.decodeSeconds = SecondsSince (decodeStart);
  result.liveBlocks = std::move (blocks);
  result.status = LoadStatus::Loaded;
  return result;
  }

} // anonymous namespace

// ---------------- writing ----------------

namespace {

std::function<bool ()> g_reopenHook;

// The file, again, after a rewrite or to catch up.
void * OpenAgain (const std::string & path)
  {
  if (g_reopenHook && !g_reopenHook ())
    return nullptr;
  return FileOpen (path, false);
  }

} // anonymous namespace

void SetReopenHookForTest (std::function<bool ()> hook)
  {
  g_reopenHook = std::move (hook);
  }

static std::atomic<unsigned> g_maxThreads (0);

unsigned ThreadsToUse ()
  {
  unsigned n = (std::min) ((std::max) (std::thread::hardware_concurrency (), 1u), 8u);
  const unsigned limit = g_maxThreads.load ();
  return limit && limit < n ? limit : n;
  }

void SetMaxThreadsForTest (unsigned n)
  {
  g_maxThreads = n;
  }

Writer::Writer (const std::string & path, bool bUtf8, const LoadResult & load)
  : m_path (path), m_bUtf8 (bUtf8)
  {
  MakeFoldersFor (path);
  m_file = FileOpen (path, false);
  if (!m_file)
    return;     // another copy of the client has it, or the folder can't be written

  if (load.status == LoadStatus::Loaded)
    {
    m_end = load.validEnd;
    m_blocks = load.liveBlocks;
    for (const BlockInfo & b : m_blocks)
      m_live += b.recordSize;
    if (load.tornTail)
      FileTruncate (m_file, m_end);
    }
  else if (!WriteHeader (bUtf8))
    {
    FileClose (m_file);
    m_file = nullptr;
    return;
    }

  m_bOpen = true;
  m_thread = std::thread ([this] { Run (); });
  }

Writer::~Writer ()
  {
  Close (false);
  }

void Writer::Post (Op && op)
  {
  std::lock_guard<std::mutex> lock (m_mutex);
  if (!m_bOpen || m_bStop)
    return;
  m_queue.push_back (std::move (op));
  m_wake.notify_one ();
  }

void Writer::AppendLines (std::vector<uint8_t> block, size_t lineCount, int64_t first, int64_t last)
  {
  Op op;
  op.kind = Op::Append;
  op.block = std::move (block);
  op.lineCount = lineCount;
  op.first = first;
  op.last = last;
  Post (std::move (op));
  }

void Writer::TruncateFrom (int64_t number)
  {
  Op op;
  op.kind = Op::Truncate;
  op.first = number;
  Post (std::move (op));
  }

void Writer::SetFlags (int64_t number, uint8_t flags)
  {
  Op op;
  op.kind = Op::Flags;
  op.first = number;
  op.flags = flags;
  Post (std::move (op));
  }

void Writer::SetLiveFrom (int64_t number)
  {
  Op op;
  op.kind = Op::LiveFrom;
  op.first = number;
  Post (std::move (op));
  }

void Writer::Clear (bool bUtf8)
  {
  Op op;
  op.kind = Op::ClearAll;
  op.bUtf8 = bUtf8;
  Post (std::move (op));
  }

void Writer::DropBefore (int64_t number)
  {
  Op op;
  op.kind = Op::Drop;
  op.first = number;
  Post (std::move (op));
  }

void Writer::StopReclaiming ()
  {
  std::lock_guard<std::mutex> lock (m_mutex);
  m_bNoReclaim = true;
  }

void Writer::Flush ()
  {
  std::unique_lock<std::mutex> lock (m_mutex);
  m_idle.wait (lock, [this] { return m_queue.empty () && !m_bBusy; });
  }

void Writer::Close (bool bDeleteFile)
  {
    {
    std::lock_guard<std::mutex> lock (m_mutex);
    m_bStop = true;
    m_wake.notify_all ();
    }
  if (m_thread.joinable ())
    m_thread.join ();
  CloseFile ();
  const bool bWasOpen = m_bOpen;
  m_bOpen = false;
  // Only a file this had: one that couldn't be opened may be another copy's.
  if (bDeleteFile && bWasOpen)
    DeleteTheFile (m_path);
  }

std::string Writer::LastError () const
  {
  std::lock_guard<std::mutex> lock (m_mutex);
  return m_error;
  }

void Writer::Fail (const std::string & why)
  {
  std::lock_guard<std::mutex> lock (m_mutex);
  m_error = why;
  }

void Writer::Run ()
  {
  for (;;)
    {
    Op op;
      {
      std::unique_lock<std::mutex> lock (m_mutex);
      m_bBusy = false;
      if (m_queue.empty ())
        m_idle.notify_all ();
      m_wake.wait (lock, [this] { return m_bStop || !m_queue.empty (); });
      if (m_queue.empty ())
        {
        // Stopping, and everything asked for is done, or held: one last try at that.
        lock.unlock ();
        try
          {
          CatchUp ();
          }
        catch (...)
          {
          }
        return;
        }
      op = std::move (m_queue.front ());
      m_queue.pop_front ();
      m_bBusy = true;
      }

    try
      {
      if (CatchUp ())
        Do (op);
      else
        Hold (std::move (op));
      }
    catch (...)     // out of memory, most likely: losing a block beats losing the client
      {
      Fail ("could not save some of the output buffer");
      }
    }
  }

void Writer::Do (Op & op)
  {
  switch (op.kind)
    {
    case Op::Append:
      DoAppend (op);
      MaybeReclaim ();
      break;
    case Op::Truncate:
      {
      m_tail.clear ();
      m_tailLines = 0;
      if (WriteRecord (kRecordTruncate, nullptr, 0, 0, 0, op.first, nullptr))
        {
        ApplyTruncate (m_blocks, op.first);
        m_live = 0;
        for (const BlockInfo & b : m_blocks)
          m_live += b.recordSize;
        }
      }
      break;
    case Op::Flags:
      m_tail.clear ();
      m_tailLines = 0;
      if (WriteRecord (kRecordFlags, nullptr, op.flags, op.first, 0, 0, nullptr))
        ApplyFlags (m_blocks, op.first, op.flags);
      break;
    case Op::LiveFrom:
      m_liveFrom = op.first;
      MaybeReclaim ();
      break;
    case Op::ClearAll:
      m_tail.clear ();
      m_tailLines = 0;
      m_blocks.clear ();
      m_live = 0;
      m_liveFrom = 0;
      if (!FileTruncate (m_file, 0))
        Fail ("could not clear the saved output buffer");
      m_bUtf8 = op.bUtf8;
      WriteHeader (m_bUtf8);
      FileSync (m_file);
      break;
    case Op::Drop:
      m_tail.clear ();
      m_tailLines = 0;
      Rewrite (op.first, true);
      break;
    case Op::Sync:
      break;
    }
  }

void Writer::CloseFile ()
  {
  if (m_file)
    {
    FileClose (m_file);
    m_file = nullptr;
    }
  }

bool Writer::WriteHeader (bool bUtf8)
  {
  const std::vector<uint8_t> header = MakeFileHeader (bUtf8);
  if (!FileTruncate (m_file, 0) || !FileWriteAt (m_file, 0, header.data (), header.size ()))
    {
    Fail ("could not write the saved output buffer's header");
    return false;
    }
  m_end = header.size ();
  return true;
  }

bool Writer::WriteRecord (uint32_t kind, const std::vector<uint8_t> * raw, uint32_t lineCount,
                          int64_t first, int64_t last, int64_t replaceFrom, BlockInfo * info)
  {
  const std::vector<uint8_t> record = MakeRecord (kind, raw, lineCount, first, last, replaceFrom);
  if (record.empty () || !FileWriteAt (m_file, m_end, record.data (), record.size ()))
    {
    // Take back whatever got there, so that the next record follows a good one.
    FileTruncate (m_file, m_end);
    Fail ("could not write to the saved output buffer (is the disk full?)");
    return false;
    }
  if (info)
    {
    info->offset = m_end;
    info->recordSize = record.size ();
    info->lineCount = lineCount;
    info->rawSize = raw ? (uint32_t) raw->size () : 0;
    info->first = first;
    info->last = last;
    }
  m_end += record.size ();
  return true;
  }

void Writer::DoAppend (Op & op)
  {
  // A big block ends a run of small ones.  They have to be merged before it, because
  // a merged block replaces everything from its first line on.
  if (op.lineCount >= kSmallBlockLines && m_tail.size () > 1)
    MergeTail ();

  BlockInfo info;
  if (!WriteRecord (kRecordLines, &op.block, (uint32_t) op.lineCount, op.first, op.last, 0, &info))
    return;
  m_blocks.push_back (info);
  m_live += info.recordSize;

  if (op.lineCount >= kSmallBlockLines)
    {
    m_tail.clear ();
    m_tailLines = 0;
    return;
    }

  m_tail.push_back (SmallBlock { std::move (op.block), op.lineCount, op.first, op.last });
  m_tailLines += op.lineCount;
  if (m_tail.size () >= kMergeAfterBlocks || m_tailLines >= kBlockLines)
    MergeTail ();
  }

// Lots of little blocks compress badly and have to be inflated one by one, so a run
// of them becomes one big one.  It goes at the end of the file, saying what it
// replaces, in one record: torn, it fails its checksum and the little ones still
// stand.  What they took up is dead space until the next rewrite.
void Writer::MergeTail ()
  {
  if (m_tail.size () < 2)
    return;
  BlockBuilder builder;
  for (const SmallBlock & tailBlock : m_tail)
    {
    DecodedBlock block;
    if (!DecodeBlock (tailBlock.raw.data (), tailBlock.raw.size (), block))
      {
      m_tail.clear ();      // can't happen: we made these
      m_tailLines = 0;
      return;
      }
    std::vector<int32_t> actionMap (block.actions.size (), -1);
    for (const Line & line : block.lines)
      CopyLine (block, line, actionMap, builder);
    }
  const int64_t first = builder.FirstNumber ();
  const int64_t last = builder.LastNumber ();
  const size_t lines = builder.LineCount ();
  const std::vector<uint8_t> raw = builder.Finish ();

  BlockInfo info;
  if (WriteRecord (kRecordLines, &raw, (uint32_t) lines, first, last, first, &info))
    {
    // What it replaces is decided by line number, as Load decides it, and not by
    // counting blocks off the end: the list need not still hold one block per small
    // block written (a rewrite merges them), and counting would take older ones too.
    ApplyTruncate (m_blocks, first);
    m_blocks.push_back (info);
    m_live = 0;
    for (const BlockInfo & b : m_blocks)
      m_live += b.recordSize;
    }
  m_tail.clear ();
  m_tailLines = 0;
  }

void Writer::MaybeReclaim ()
  {
  // A disk too full to rewrite on is not going to get better for trying every time a
  // block is saved, but it may well get better: try again after a while.
  if (m_reclaimRetryIn > 0)
    {
    m_reclaimRetryIn--;
    return;
    }
  // Closing.  Space can as well be reclaimed next session, and a rewrite of a big file
  // now would hold up the close, which waits for this thread.  (An explicit DropBefore
  // still happens: that is about taking text out of the file, not about space.)
    {
    std::lock_guard<std::mutex> lock (m_mutex);
    if (m_bStop || m_bNoReclaim)
      return;
    }
  uint64_t live = 0;
  size_t nSmall = 0;
  for (const BlockInfo & b : m_blocks)
    if (b.last >= m_liveFrom)
      {
      live += b.recordSize;
      if (b.lineCount < kSmallBlockLines)
        nSmall++;
      }
  const uint64_t dead = m_end - kFileHeaderSize - live;
  if ((dead > kMinDeadBytes && dead > live) || nSmall > kMaxSmallBlocks)
    if (!Rewrite (m_liveFrom, false))
      m_reclaimRetryIn = kReclaimRetryAfter;
  }

// Writes the live blocks to a new file and renames it over this one.  Blocks that
// need no change are copied as they are, without being inflated; those that have
// lines cut off or flags changed, or are small enough to be worth merging, are
// decoded and written again.
//
// bExact: lines below keepFrom must really go, even if that means rewriting the block
// they share with lines above it.  Otherwise, only whole blocks are dropped.
bool Writer::Rewrite (int64_t keepFrom, bool bExact)
  {
  const std::string temp = m_path + ".tmp";
  void * out = FileOpen (temp, true);
  if (!out)
    {
    Fail ("could not create a temporary file for the saved output buffer");
    return false;
    }

  bool bOK = true;
  uint64_t end = 0;
  std::vector<BlockInfo> written;

  const std::vector<uint8_t> header = MakeFileHeader (m_bUtf8);
  bOK = FileWriteAt (out, 0, header.data (), header.size ());
  end = header.size ();

  BlockBuilder builder;
  auto flushBuilder = [&] ()
    {
    if (!bOK || builder.LineCount () == 0)
      return;
    const int64_t first = builder.FirstNumber ();
    const int64_t last = builder.LastNumber ();
    const size_t lines = builder.LineCount ();
    const std::vector<uint8_t> raw = builder.Finish ();
    const std::vector<uint8_t> record = MakeRecord (kRecordLines, &raw, (uint32_t) lines, first, last, 0);
    if (record.empty () || !FileWriteAt (out, end, record.data (), record.size ()))
      {
      bOK = false;
      return;
      }
    BlockInfo info;
    info.offset = end;
    info.recordSize = record.size ();
    info.lineCount = (uint32_t) lines;
    info.rawSize = (uint32_t) raw.size ();
    info.first = first;
    info.last = last;
    written.push_back (info);
    end += record.size ();
    };

  std::vector<uint8_t> record;
  for (const BlockInfo & b : m_blocks)
    {
    if (!bOK)
      break;
    if (b.last < keepFrom)
      continue;

    const bool bDecode = b.cutFrom || !b.flagChanges.empty () ||
                         b.lineCount < kSmallBlockLines ||
                         (bExact && b.first < keepFrom);

    record.resize ((size_t) b.recordSize);
    if (!FileReadAt (m_file, b.offset, record.data (), record.size ()))
      {
      bOK = false;
      break;
      }

    if (!bDecode)
      {
      flushBuilder ();
      if (!bOK || !FileWriteAt (out, end, record.data (), record.size ()))
        {
        bOK = false;
        break;
        }
      BlockInfo info = b;
      info.offset = end;
      written.push_back (info);
      end += record.size ();
      continue;
      }

    DecodedBlock block;
    if (!InflateAndDecode (record.data (), record.size (), block))
      continue;     // damaged: all that was going to happen to it anyway
    ApplyToBlock (b, block);
    std::vector<int32_t> actionMap (block.actions.size (), -1);
    for (const Line & line : block.lines)
      {
      if (bExact && line.number < keepFrom)
        continue;
      CopyLine (block, line, actionMap, builder);
      if (builder.LineCount () >= kBlockLines)
        {
        flushBuilder ();
        std::fill (actionMap.begin (), actionMap.end (), -1);
        }
      }
    if (builder.LineCount () >= kSmallBlockLines)
      flushBuilder ();
    }
  flushBuilder ();

  if (bOK)
    bOK = FileSync (out);
  FileClose (out);

  if (!bOK)
    {
    DeleteTheFile (temp);
    Fail ("could not rewrite the saved output buffer");
    return false;
    }

  // On Windows a file can't be renamed over while we hold it.
  CloseFile ();
  const bool bRenamed = RenameOver (temp, m_path);
  if (!bRenamed)
    {
    DeleteTheFile (temp);
    Fail ("could not replace the saved output buffer");
    }
  else
    {
    // It's the new file from here on, whether or not it can be opened again at once:
    // what is known about the old one would be wrong about it.
    m_blocks.swap (written);
    m_end = end;
    m_live = end - header.size ();
    // The small blocks waiting to be merged are in what was just written, merged or
    // not; merging them again would be replacing lines that aren't theirs to replace.
    m_tail.clear ();
    m_tailLines = 0;
    }
  m_file = OpenAgain (m_path);
  if (!m_file)
    Fail ("could not open the saved output buffer again, so it is not being saved for now");
  return bRenamed && m_file;
  }

// After a rewrite that couldn't open the file again (something else had it for a
// moment, say).  Until this works, nothing is written.
bool Writer::Reopen ()
  {
  if (m_file)
    return true;
  m_file = OpenAgain (m_path);
  if (!m_file)
    return false;
  // Anything that got there meanwhile is cut off, as a torn tail would be.
  FileTruncate (m_file, m_end);
  Fail ("");
  return true;
  }

// While the file is shut, what is asked for waits, up to a point.
void Writer::Hold (Op && op)
  {
  if (op.kind == Op::ClearAll)
    {
    m_held.clear ();      // nothing before a clear matters
    m_heldBytes = 0;
    }
  if (m_bHeldTooMuch && op.kind == Op::Append)
    return;               // the gap is there already: these lines are in it
  m_heldBytes += op.block.size ();
  m_held.push_back (std::move (op));
  if (m_held.size () > kMaxHeldOps || m_heldBytes > kMaxHeldBytes)
    {
    m_bHeldTooMuch = true;
    CompactHeld ();
    }
  }

// Too much has waited.  The lines go, all of them from now until the file can be written
// again: the file has a gap there, as it has for a line typed with echo off, and keeps what
// it had before.  What stays is what takes text out of the file, which must still happen
// (a clear, a TRUNCATE for world.DeleteLines, a DropBefore), and the newest flags and live
// line, folded into as few as do the same.  With the lines gone these commute: a TRUNCATE
// and a DropBefore each remove a set of numbers, a clear makes everything before it moot,
// and a flag for a line that isn't in the file (dropped, or since truncated) changes
// nothing, so they can be done in any order, and this one is as good as the original.
void Writer::CompactHeld ()
  {
  bool bClear = false;
  bool bUtf8 = m_bUtf8;
  int64_t truncateFrom = 0;     // 0, none: a TRUNCATE is from line 1 at the least
  int64_t dropBefore = 0;
  bool bLiveFrom = false;
  int64_t liveFrom = 0;
  std::map<int64_t, uint8_t> flags;
  for (const Op & op : m_held)
    switch (op.kind)
      {
      case Op::ClearAll:
        bClear = true;
        bUtf8 = op.bUtf8;
        truncateFrom = 0;
        dropBefore = 0;
        flags.clear ();
        break;
      case Op::Truncate:
        truncateFrom = truncateFrom ? (std::min) (truncateFrom, op.first) : op.first;
        break;
      case Op::Drop:
        dropBefore = (std::max) (dropBefore, op.first);
        break;
      case Op::Flags:
        flags [op.first] = op.flags;
        break;
      case Op::LiveFrom:
        bLiveFrom = true;
        liveFrom = op.first;
        break;
      default:
        break;      // Append, Sync
      }
  // Bookmarks by the thousand: let them go too, rather than hold ever more.
  if (flags.size () > kMaxHeldOps / 2)
    flags.clear ();

  std::deque<Op> kept;
  auto keep = [&kept] (Op::Kind kind, int64_t first)
    {
    Op op;
    op.kind = kind;
    op.first = first;
    kept.push_back (std::move (op));
    return &kept.back ();
    };
  if (bClear)
    keep (Op::ClearAll, 0)->bUtf8 = bUtf8;
  if (truncateFrom)
    keep (Op::Truncate, truncateFrom);
  if (dropBefore)
    keep (Op::Drop, dropBefore);
  for (const auto & f : flags)
    keep (Op::Flags, f.first)->flags = f.second;
  if (bLiveFrom)
    keep (Op::LiveFrom, liveFrom);
  m_held.swap (kept);
  m_heldBytes = 0;
  }

// False if the file is still shut, and then whatever is still waiting stays waiting.
// Otherwise, whatever waited has been done.
bool Writer::CatchUp ()
  {
  if (!Reopen ())
    return false;
  if (m_bHeldTooMuch)
    {
    m_bHeldTooMuch = false;
    Fail ("some of the output buffer could not be saved");
    }
  while (!m_held.empty ())
    {
    Op held = std::move (m_held.front ());
    m_held.pop_front ();
    m_heldBytes -= held.block.size ();
    Do (held);
    // A held DropBefore's rewrite, or a reclaim an append set off, closed the file and
    // couldn't open it again.  The rest, and what was asked for now, wait for next time.
    if (!m_file)
      return false;
    }
  return true;
  }

// ---------------- odds and ends ----------------

bool WriteFileAtomically (const std::string & path, const void * data, size_t size, std::string * error)
  {
  MakeFoldersFor (path);
  const std::string temp = path + ".tmp";
  void * f = FileOpen (temp, true);
  bool bOK = f != nullptr;
  if (bOK)
    {
    // No sync.  It would make this safe against the power going, at the price of a
    // visit to the platter on the UI thread, and an exiting or crashing program's
    // writes survive regardless.
    bOK = FileWriteAt (f, 0, data, size);
    FileClose (f);
    }
  if (bOK)
    bOK = RenameOver (temp, path);
  if (!bOK)
    {
    DeleteTheFile (temp);
    if (error)
      *error = "could not write " + path;
    }
  return bOK;
  }

void MakeFoldersFor (const std::string & path)
  {
  for (size_t i = 1; i < path.size (); i++)
    if (path [i] == '/' || path [i] == '\\')
      {
      const std::string folder = path.substr (0, i);
#ifdef _WIN32
      if (folder.size () == 2 && folder [1] == ':')
        continue;   // the drive
      CreateDirectoryA (folder.c_str (), NULL);
#else
      mkdir (folder.c_str (), 0700);
#endif
      }
  }

} // namespace scrollback

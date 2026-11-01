// scrollback_store.h -- a world's saved output buffer, on disk.
//
// One file per world, <data folder>/scrollback/<world id>.mcsb.  It knows nothing
// about CLine or the document (doc_scrollback_shared.cpp does the translating), so
// it can be tested and timed on its own.
//
// The file is a header followed by records, each with its own checksums:
//
//   LINES        a compressed block of up to kBlockLines lines (zlib)
//   TRUNCATE     "forget every line numbered >= N": world.DeleteLines reached lines
//                already saved
//   FLAGS        "line N's flags are now F": a bookmark on a line already saved
//
// It only ever grows by appending, so the worst a torn write can do is lose the last
// record, which fails its checksum and is ignored and then cut off.  A bad block
// further back (a flipped bit) costs that block and no more, because the record
// header says where the next one starts and has a checksum of its own.
//
// Loading is one sequential read of the whole file, then every block inflated and
// parsed on as many threads as are useful: nothing seeks, which is what a spinning
// disk needs, and the text is around a tenth of its in-memory size.
//
// Saving never holds up the caller for long.  The caller encodes lines into a
// BlockBuilder (no compression, about the cost of copying them) and hands the
// result to a Writer, whose worker thread compresses and writes it.  Space used by
// lines that have since gone (evicted from the buffer, or replaced when small
// blocks are merged) is reclaimed by rewriting the file to a temporary name and
// renaming it, on the same worker, when there is more dead than live.  An explicit
// clear does the same at once, so that cleared text does not sit in the file.

#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace scrollback {

const uint32_t kFormatVersion = 1;

// A block holds this many lines at most.  A few thousand lines compress well and
// decompress in about a millisecond.
const size_t kBlockLines = 4096;

struct Style
  {
  uint32_t length;      // bytes of the line's text
  uint32_t flags;       // CStyle::iFlags
  uint32_t fore;
  uint32_t back;
  int32_t  action;      // index into the block's actions, or -1
  };

struct Action
  {
  std::string action;
  std::string hint;
  std::string variable;
  };

struct Line
  {
  int64_t  number;      // CLine::m_nLineNumber
  int64_t  time;        // CLine::m_theTime, seconds since 1970
  uint32_t textOffset;  // into the block's text
  uint32_t length;
  int32_t  charCount;   // CLine::m_iCharCount
  int32_t  preamble;    // CLine::m_iPreambleOffset
  uint32_t firstStyle;
  uint32_t styleCount;
  uint8_t  flags;       // CLine::flags
  bool     hardReturn;
  };

// One block's lines.  Filled by DecodeBlock, and by nothing else.
struct DecodedBlock
  {
  std::vector<char>   raw;      // what was inflated; text points into this
  const char *        text = nullptr;
  size_t              textSize = 0;
  std::vector<Line>   lines;
  std::vector<Style>  styles;
  std::vector<Action> actions;
  };

// Lines go in in order, each followed by its styles.
class BlockBuilder
  {
  public:
  void AddLine (int64_t number, int64_t time, uint8_t flags, bool hardReturn,
                int32_t charCount, int32_t preamble, const char * text, size_t length);
  // Index to pass to AddStyle.  Found by a linear search, so ask once per distinct
  // action rather than once per style if there can be many.
  int32_t AddAction (const std::string & action, const std::string & hint, const std::string & variable);
  void AddStyle (uint32_t length, uint32_t flags, uint32_t fore, uint32_t back, int32_t action);

  size_t  LineCount () const { return m_lines; }
  size_t  ByteCount () const { return m_meta.size () + m_styles.size () + m_text.size (); }
  int64_t FirstNumber () const { return m_first; }
  int64_t LastNumber () const { return m_last; }

  // The block's bytes, ready for Writer::AppendLines.  The builder is empty again.
  std::vector<uint8_t> Finish ();

  private:
  std::vector<uint8_t> m_meta;
  std::vector<uint8_t> m_styles;
  std::vector<uint8_t> m_actionBytes;
  std::vector<char>    m_text;
  std::vector<Action>  m_actions;
  uint32_t             m_pendingStyles = 0;   // for the line last added
  size_t               m_lines = 0;
  size_t               m_styleTotal = 0;
  int64_t              m_first = 0;
  int64_t              m_last = 0;
  int64_t              m_previousNumber = 0;
  int64_t              m_previousTime = 0;
  };

// False if the bytes aren't a well-formed block.  Every length is checked.
bool DecodeBlock (const uint8_t * raw, size_t size, DecodedBlock & out);

// One saved block, as far as the file's record headers can say without decoding it.
// Shared by the loader and the writer.
struct BlockInfo
  {
  uint64_t offset = 0;        // of the record
  uint64_t recordSize = 0;
  uint32_t lineCount = 0;
  uint32_t rawSize = 0;
  int64_t  first = 0;
  int64_t  last = 0;
  int64_t  cutFrom = 0;       // lines numbered >= this have been deleted (0 = none)
  std::vector<std::pair<int64_t, uint8_t>> flagChanges;   // to apply, in order
  };

enum class LoadStatus
  {
  NoFile,           // or an empty one: nothing to restore, and nothing wrong
  Loaded,
  WrongEncoding,    // saved with the UTF-8 option the other way round
  NewerVersion,     // written by a later client: leave it alone
  CouldNotRead,     // the disk would not give it up: leave it alone, and try next time
  Unreadable,       // not one of ours, or its header is damaged: to be replaced
  };

struct LoadResult
  {
  LoadStatus status = LoadStatus::NoFile;
  // The newest maxLines lines, oldest first.  A block's lines are in order, and a
  // block's first line may be past lines[0] if some were dropped from the head:
  // firstLineInFirstBlock says how many of blocks[0]'s lines to skip.
  std::vector<std::unique_ptr<DecodedBlock>> blocks;
  size_t firstLineInFirstBlock = 0;
  size_t lineCount = 0;
  size_t badBlocks = 0;       // skipped because they failed their checksum
  bool   tornTail = false;    // the file ended in something unreadable
  uint64_t fileSize = 0;
  // For the Writer: every live block in the file, and how much of the file to keep.
  std::vector<BlockInfo> liveBlocks;
  uint64_t validEnd = 0;
  // Time taken, for the measurements in tests.
  double readSeconds = 0;
  double decodeSeconds = 0;
  };

// maxLines 0 is no limit.  threads 0 is "as many as make sense".
LoadResult Load (const std::string & path, bool bUtf8, size_t maxLines, unsigned threads = 0);

// Applies a file's TRUNCATE and FLAGS records to what they refer to.  Load does this;
// it is public for the tests.
void ApplyToBlock (const BlockInfo & info, DecodedBlock & block);

// The worker that owns the open file.  Everything here returns at once, in effect:
// it queues, and the worker does things in the order they were asked for.
class Writer
  {
  public:
  // Opens the file, creating it and any missing folder, and takes it for this
  // process: a second copy of the client with the same world gets IsOpen() false,
  // and carries on without saving rather than interleaving records.  `load` is what
  // Load found: a torn tail is cut off first, and a file it couldn't use is replaced.
  Writer (const std::string & path, bool bUtf8, const LoadResult & load);
  ~Writer ();     // finishes the queue, then closes

  bool IsOpen () const { return m_bOpen; }

  void AppendLines (std::vector<uint8_t> block, size_t lineCount, int64_t first, int64_t last);
  void TruncateFrom (int64_t number);
  void SetFlags (int64_t number, uint8_t flags);
  // Lines numbered below this have gone from the buffer, and need not be kept.  Used
  // to decide when to reclaim space; nothing is rewritten just because of this call.
  void SetLiveFrom (int64_t number);

  // The explicit clears: these take the text out of the file, not merely mark it.
  void Clear (bool bUtf8);
  void DropBefore (int64_t number);

  // Blocks until everything asked for so far is on disk (or has failed).
  void Flush ();
  // Blocks until the worker is finished and the file closed; optionally deletes it.
  void Close (bool bDeleteFile);

  // The last write failure, if any: "" if all is well.
  std::string LastError () const;

  private:
  struct Op
    {
    enum Kind { Append, Truncate, Flags, LiveFrom, ClearAll, Drop, Sync } kind = Sync;
    std::vector<uint8_t> block;
    size_t  lineCount = 0;
    int64_t first = 0;
    int64_t last = 0;
    uint8_t flags = 0;
    bool    bUtf8 = false;
    };

  void Post (Op && op);
  void Run ();
  void Do (Op & op);

  bool OpenFile ();
  void CloseFile ();
  bool WriteHeader (bool bUtf8);
  bool WriteRecord (uint32_t kind, const std::vector<uint8_t> * raw, uint32_t lineCount,
                    int64_t first, int64_t last, int64_t replaceFrom, BlockInfo * info);
  void DoAppend (Op & op);
  void MergeTail ();
  void MaybeReclaim ();
  bool Rewrite (int64_t keepFrom, bool bExact);
  void Fail (const std::string & why);

  const std::string m_path;
  bool     m_bUtf8;
  bool     m_bOpen = false;
  void *   m_file = nullptr;      // HANDLE, or the fd + 1
  uint64_t m_end = 0;             // everything before here is good
  uint64_t m_live = 0;            // bytes of the file that are live blocks
  int64_t  m_liveFrom = 0;
  bool     m_bReclaimFailed = false;
  std::vector<BlockInfo> m_blocks;

  // Blocks written this session that are still small, with their bytes, for merging
  // into one.  Worker only.
  struct SmallBlock
    {
    std::vector<uint8_t> raw;
    size_t  lineCount;
    int64_t first;
    int64_t last;
    };
  std::vector<SmallBlock> m_tail;
  size_t m_tailLines = 0;

  mutable std::mutex      m_mutex;
  std::condition_variable m_wake;
  std::condition_variable m_idle;
  std::deque<Op>          m_queue;
  bool        m_bBusy = false;
  bool        m_bStop = false;
  std::string m_error;
  std::thread m_thread;
  };

// Replaces a file by writing beside it and renaming: a reader, or a crash, sees the
// old one or the new one.  Used for command history as well.
bool WriteFileAtomically (const std::string & path, const void * data, size_t size,
                          std::string * error = nullptr);

// Makes every folder in the path of this file that doesn't exist.
void MakeFoldersFor (const std::string & path);

} // namespace scrollback

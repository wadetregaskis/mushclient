// tests/test_scrollback.cpp -- a world's saved output buffer: the file
// (scrollback_store.*) and the way lines go in and out of a document
// (doc_scrollback_shared.cpp).  Runs on Windows and the port.
//
// Set MC_SCROLLBACK_PERF=1 to add the timings for 5,000, 100,000 and 1,000,000 lines.
// They say nothing useful in a Debug build.

#include "mc_test.h"
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scrollback_store.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "variant_test_helpers.h"
#include "scripting/errors.h"
#include "flags.h"
#include "pal/itransport.h"
#include "fake_transport.h"
#include "command_history_store.h"
#ifdef _WIN32
#include "sendvw.h"
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace scrollback;

namespace {

std::string TempFile (const char * name)
  {
  const char * dir = getenv ("TEMP");
  if (!dir)
    dir = "/tmp";
  std::string path = std::string (dir) + "/mc_scrollback_" + name + ".mcsb";
  remove (path.c_str ());
  remove ((path + ".tmp").c_str ());
  return path;
  }

std::vector<uint8_t> ReadAll (const std::string & path)
  {
  std::vector<uint8_t> out;
  if (FILE * f = fopen (path.c_str (), "rb"))
    {
    uint8_t buffer [65536];
    size_t n;
    while ((n = fread (buffer, 1, sizeof buffer, f)) > 0)
      out.insert (out.end (), buffer, buffer + n);
    fclose (f);
    }
  return out;
  }

void WriteAll (const std::string & path, const std::vector<uint8_t> & data)
  {
  FILE * f = fopen (path.c_str (), "wb");
  ASSERT_TRUE (f != nullptr);
  fwrite (data.data (), 1, data.size (), f);
  fclose (f);
  }

// One block of simple lines, numbered from `first`.  The text says the number.
std::vector<uint8_t> MakeBlock (int64_t first, size_t count, int64_t * pLast = nullptr)
  {
  BlockBuilder b;
  for (size_t i = 0; i < count; i++)
    {
    const int64_t number = first + (int64_t) i;
    const std::string text = "line " + std::to_string (number);
    b.AddLine (number, 1000000 + number, 0, true, (int32_t) text.size (), 0, text.data (), text.size ());
    b.AddStyle ((uint32_t) text.size (), 0, 7, 0, -1);
    }
  if (pLast)
    *pLast = first + (int64_t) count - 1;
  return b.Finish ();
  }

void AppendBlock (Writer & w, int64_t first, size_t count)
  {
  int64_t last;
  std::vector<uint8_t> block = MakeBlock (first, count, &last);
  w.AppendLines (std::move (block), count, first, last);
  }

// A block of lines that compress badly, so that a megabyte of dead space turns up
// after a few of them.
void AppendNoiseBlock (Writer & w, int64_t first, size_t count, unsigned & seed)
  {
  BlockBuilder builder;
  for (size_t i = 0; i < count; i++)
    {
    std::string text;
    for (int c = 0; c < 80; c++)
      {
      seed = seed * 1103515245 + 12345;
      text += (char) (32 + (seed >> 16) % 90);
      }
    builder.AddLine (first + (int64_t) i, 1000, 0, true, 80, 0, text.data (), text.size ());
    }
  const int64_t last = builder.LastNumber ();
  w.AppendLines (builder.Finish (), count, first, last);
  }

// The numbers of every line that came back, in order.
std::vector<int64_t> Numbers (const LoadResult & r)
  {
  std::vector<int64_t> out;
  for (size_t b = 0; b < r.blocks.size (); b++)
    for (size_t i = b == 0 ? r.firstLineInFirstBlock : 0; i < r.blocks [b]->lines.size (); i++)
      out.push_back (r.blocks [b]->lines [i].number);
  return out;
  }

std::vector<int64_t> Range (int64_t first, int64_t last)
  {
  std::vector<int64_t> out;
  for (int64_t i = first; i <= last; i++)
    out.push_back (i);
  return out;
  }

const std::string kNonUtf8 ("caf\xE9 \xFF\xFE", 7);

} // anonymous namespace

// ---------------- the file ----------------

TEST (ScrollbackStore, EveryFieldOfALineRoundTrips)
  {
  const std::string path = TempFile ("fields");
  BlockBuilder b;
  b.AddLine (41, 1700000000, 0x2A, true, 3, 5, "abcdef", 6);
  int32_t action = b.AddAction ("http://example.com", "a hint", "a variable");
  EXPECT_EQ (0, action);
  EXPECT_EQ (0, b.AddAction ("http://example.com", "a hint", "a variable"));
  b.AddStyle (2, 0x0001, 3, 4, -1);
  b.AddStyle (4, 0x0200 | 0x0800, 0x00FF8040, 0x00102030, action);
  b.AddLine (44, 1699999990, 0x08, false, (int32_t) kNonUtf8.size (), -1, kNonUtf8.data (), kNonUtf8.size ());
  b.AddStyle ((uint32_t) kNonUtf8.size (), 0x1000, 0, 0, -1);
  b.AddLine (45, 1700000001, 0, true, 0, 0, "", 0);
  std::vector<uint8_t> raw = b.Finish ();

  {
  LoadResult nothing;
  Writer w (path, false, nothing);
  ASSERT_TRUE (w.IsOpen ());
  w.AppendLines (std::move (raw), 3, 41, 45);
  }

  LoadResult r = Load (path, false, 0);
  ASSERT_TRUE (r.status == LoadStatus::Loaded);
  ASSERT_EQ (1u, r.blocks.size ());
  EXPECT_EQ (3u, r.lineCount);
  const DecodedBlock & block = *r.blocks [0];

  const Line & one = block.lines [0];
  EXPECT_EQ (41, (long) one.number);
  EXPECT_EQ (1700000000, (long) one.time);
  EXPECT_EQ (0x2A, (int) one.flags);
  EXPECT_TRUE (one.hardReturn);
  EXPECT_EQ (3, one.charCount);
  EXPECT_EQ (5, one.preamble);
  EXPECT_EQ (std::string ("abcdef"), std::string (block.text + one.textOffset, one.length));
  ASSERT_EQ (2u, one.styleCount);
  EXPECT_EQ (2u, block.styles [one.firstStyle].length);
  EXPECT_EQ (0x0001u, block.styles [one.firstStyle].flags);
  EXPECT_EQ (3u, block.styles [one.firstStyle].fore);
  EXPECT_EQ (4u, block.styles [one.firstStyle].back);
  EXPECT_EQ (-1, block.styles [one.firstStyle].action);
  const Style & linked = block.styles [one.firstStyle + 1];
  EXPECT_EQ (0x00FF8040u, linked.fore);
  EXPECT_EQ (0x00102030u, linked.back);
  ASSERT_EQ (0, linked.action);
  EXPECT_EQ (std::string ("http://example.com"), block.actions [0].action);
  EXPECT_EQ (std::string ("a hint"), block.actions [0].hint);
  EXPECT_EQ (std::string ("a variable"), block.actions [0].variable);

  const Line & two = block.lines [1];
  EXPECT_EQ (44, (long) two.number);
  EXPECT_EQ (1699999990, (long) two.time);     // the clock can go backwards
  EXPECT_FALSE (two.hardReturn);
  EXPECT_EQ (-1, two.preamble);
  // Bytes that aren't valid in any encoding go through untouched.
  EXPECT_TRUE (kNonUtf8 == std::string (block.text + two.textOffset, two.length));
  EXPECT_EQ (0x1000u, block.styles [two.firstStyle].flags);

  const Line & three = block.lines [2];
  EXPECT_EQ (0u, three.length);
  EXPECT_EQ (0u, three.styleCount);

  remove (path.c_str ());
  }

TEST (ScrollbackStore, NoFileIsNotAnError)
  {
  const std::string path = TempFile ("nofile");
  LoadResult r = Load (path, false, 0);
  EXPECT_TRUE (r.status == LoadStatus::NoFile);
  EXPECT_EQ (0u, r.lineCount);
  }

TEST (ScrollbackStore, TheNewestLinesAreTheOnesKept)
  {
  const std::string path = TempFile ("maxlines");
  {
  LoadResult nothing;
  Writer w (path, false, nothing);
  AppendBlock (w, 1, 4096);
  AppendBlock (w, 4097, 4096);
  AppendBlock (w, 8193, 4096);
  }
  LoadResult r = Load (path, false, 5000);
  EXPECT_EQ (5000u, r.lineCount);
  EXPECT_TRUE (Numbers (r) == Range (12288 - 4999, 12288));
  // Only the blocks needed were inflated.
  EXPECT_EQ (2u, r.blocks.size ());
  remove (path.c_str ());
  }

TEST (ScrollbackStore, AFileWithTheOtherEncodingIsNotUsed)
  {
  const std::string path = TempFile ("encoding");
  {
  LoadResult nothing;
  Writer w (path, true, nothing);
  AppendBlock (w, 1, 10);
  }
  EXPECT_TRUE (Load (path, false, 0).status == LoadStatus::WrongEncoding);
  EXPECT_TRUE (Load (path, true, 0).status == LoadStatus::Loaded);

  // ... and is started afresh, in the right one, when a Writer is given that answer.
  {
  LoadResult wrong = Load (path, false, 0);
  Writer w (path, false, wrong);
  AppendBlock (w, 50, 5);
  }
  LoadResult r = Load (path, false, 0);
  EXPECT_TRUE (Numbers (r) == Range (50, 54));
  remove (path.c_str ());
  }

TEST (ScrollbackStore, AFileFromALaterVersionIsLeftAlone)
  {
  const std::string path = TempFile ("version");
  {
  LoadResult nothing;
  Writer w (path, false, nothing);
  AppendBlock (w, 1, 10);
  }
  std::vector<uint8_t> file = ReadAll (path);
  file [8] = 99;      // the version
  // Its header has a checksum, so a later version's header is a good one.
  const uint32_t crc = (uint32_t) crc32 (crc32 (0L, Z_NULL, 0), file.data (), 16);
  for (int i = 0; i < 4; i++)
    file [16 + i] = (uint8_t) (crc >> (8 * i));
  WriteAll (path, file);
  EXPECT_TRUE (Load (path, false, 0).status == LoadStatus::NewerVersion);
  remove (path.c_str ());
  }

TEST (ScrollbackStore, NotOneOfOursIsUnreadable)
  {
  const std::string path = TempFile ("garbage");
  WriteAll (path, std::vector<uint8_t> (1000, 'x'));
  LoadResult r = Load (path, false, 0);
  EXPECT_TRUE (r.status == LoadStatus::Unreadable);
  EXPECT_EQ (0u, r.lineCount);
  remove (path.c_str ());
  }

// A torn write can leave any prefix of the last record.  None of them can cost the
// records before it, and appending afterwards has to work.
TEST (ScrollbackStore, TruncationAnywhereLosesOnlyTheTail)
  {
  const std::string path = TempFile ("torn");
  {
  LoadResult nothing;
  Writer w (path, false, nothing);
  AppendBlock (w, 1, 3000);
  AppendBlock (w, 3001, 3000);
  }
  const std::vector<uint8_t> whole = ReadAll (path);

  // Where the second record starts.
  LoadResult full = Load (path, false, 0);
  ASSERT_EQ (2u, full.liveBlocks.size ());
  const size_t second = (size_t) full.liveBlocks [1].offset;
  EXPECT_TRUE (Numbers (full) == Range (1, 6000));

  // Every cut in the second record's header, some in its payload, and right at its end.
  std::vector<size_t> cuts;
  for (size_t i = 0; i <= 60; i++)
    cuts.push_back (second + i);
  for (size_t i = 1; i < 8; i++)
    cuts.push_back (second + (whole.size () - second) * i / 8);
  cuts.push_back (whole.size () - 1);

  for (size_t cut : cuts)
    {
    WriteAll (path, std::vector<uint8_t> (whole.begin (), whole.begin () + (ptrdiff_t) cut));
    LoadResult r = Load (path, false, 0);
    ASSERT_TRUE (r.status == LoadStatus::Loaded);
    EXPECT_TRUE (Numbers (r) == Range (1, 3000));
    if (cut > second)
      EXPECT_TRUE (r.tornTail);
    }

  // Now carry on from a torn file.  The junk has to go, or the new block is unreachable.
  WriteAll (path, std::vector<uint8_t> (whole.begin (), whole.begin () + (ptrdiff_t) second + 100));
  {
  LoadResult r = Load (path, false, 0);
  Writer w (path, false, r);
  ASSERT_TRUE (w.IsOpen ());
  AppendBlock (w, 3001, 500);
  }
  LoadResult r = Load (path, false, 0);
  EXPECT_FALSE (r.tornTail);
  EXPECT_TRUE (Numbers (r) == Range (1, 3500));
  remove (path.c_str ());
  }

// A flipped bit further back costs that block and nothing else.
TEST (ScrollbackStore, ACorruptBlockCostsOnlyThatBlock)
  {
  const std::string path = TempFile ("corrupt");
  {
  LoadResult nothing;
  Writer w (path, false, nothing);
  AppendBlock (w, 1, 3000);
  AppendBlock (w, 3001, 3000);
  AppendBlock (w, 6001, 3000);
  }
  std::vector<uint8_t> file = ReadAll (path);
  LoadResult full = Load (path, false, 0);
  ASSERT_EQ (3u, full.liveBlocks.size ());

  // In the middle block's payload.
  const BlockInfo middle = full.liveBlocks [1];
  file [(size_t) (middle.offset + middle.recordSize / 2)] ^= 0x40;
  WriteAll (path, file);

  LoadResult r = Load (path, false, 0);
  EXPECT_EQ (1u, r.badBlocks);
  EXPECT_FALSE (r.tornTail);
  std::vector<int64_t> expected = Range (1, 3000);
  const std::vector<int64_t> last = Range (6001, 9000);
  expected.insert (expected.end (), last.begin (), last.end ());
  EXPECT_TRUE (Numbers (r) == expected);

  // A damaged record HEADER doesn't say where the next record starts, but the next
  // good header can be found, and what follows it is not a torn tail.
  std::vector<uint8_t> file2 = ReadAll (path);
  file2 [(size_t) middle.offset + 3] ^= 0x40;
  WriteAll (path, file2);
  r = Load (path, false, 0);
  EXPECT_FALSE (r.tornTail);
  EXPECT_EQ (1u, r.badBlocks);
  EXPECT_TRUE (Numbers (r) == expected);

  // ... and a session carrying on from it keeps what is after the damage, which it
  // would cut off if it took it for a torn tail.
  {
  Writer w (path, false, r);
  ASSERT_TRUE (w.IsOpen ());
  AppendBlock (w, 9001, 10);
  }
  r = Load (path, false, 0);
  const std::vector<int64_t> more = Range (9001, 9010);
  expected.insert (expected.end (), more.begin (), more.end ());
  EXPECT_TRUE (Numbers (r) == expected);

  // The same damage in the header of the LAST record is a torn tail, as ever.
  std::vector<uint8_t> file3 = ReadAll (path);
  r = Load (path, false, 0);
  file3 [(size_t) r.liveBlocks.back ().offset + 3] ^= 0x40;
  WriteAll (path, file3);
  r = Load (path, false, 0);
  EXPECT_TRUE (r.tornTail);
  EXPECT_TRUE (Numbers (r).back () == 9000);
  remove (path.c_str ());
  }

// Blocks within the newest maxLines that fail their checksum are made up for by older
// ones, so that a damaged block doesn't make the buffer come back short.
TEST (ScrollbackStore, ADamagedNewBlockIsMadeUpForByOlderOnes)
  {
  const std::string path = TempFile ("shortrestore");
  {
  LoadResult nothing;
  Writer w (path, false, nothing);
  AppendBlock (w, 1, 3000);
  AppendBlock (w, 3001, 3000);
  AppendBlock (w, 6001, 3000);
  }
  std::vector<uint8_t> file = ReadAll (path);
  const BlockInfo newest = Load (path, false, 0).liveBlocks [2];
  file [(size_t) (newest.offset + newest.recordSize / 2)] ^= 0x40;
  WriteAll (path, file);

  LoadResult r = Load (path, false, 5000);
  EXPECT_EQ (1u, r.badBlocks);
  EXPECT_EQ (5000u, r.lineCount);
  EXPECT_TRUE (Numbers (r) == Range (1001, 6000));
  EXPECT_EQ (2u, r.liveBlocks.size ());
  remove (path.c_str ());
  }

TEST (ScrollbackStore, DecodeRefusesAnythingMalformed)
  {
  const std::vector<uint8_t> good = MakeBlock (1, 20);
  DecodedBlock block;
  EXPECT_TRUE (DecodeBlock (good.data (), good.size (), block));
  EXPECT_EQ (20u, block.lines.size ());

  // Every prefix, and every byte of the counts and the meta altered.  It may or may
  // not be refused (a changed colour is still well-formed), but it mustn't go wrong.
  for (size_t n = 0; n < good.size (); n++)
    {
    DecodedBlock b;
    EXPECT_FALSE (DecodeBlock (good.data (), n, b));
    }
  for (size_t i = 0; i < good.size (); i++)
    {
    std::vector<uint8_t> bad = good;
    bad [i] ^= 0xFF;
    DecodedBlock b;
    if (DecodeBlock (bad.data (), bad.size (), b))
      for (const Line & line : b.lines)
        {
        EXPECT_TRUE ((size_t) line.textOffset + line.length <= b.textSize);
        EXPECT_TRUE ((size_t) line.firstStyle + line.styleCount <= b.styles.size ());
        }
    }
  }

TEST (ScrollbackStore, TruncateForgetsLinesAndTheirNumbersCanBeUsedAgain)
  {
  const std::string path = TempFile ("truncate");
  {
  LoadResult nothing;
  Writer w (path, false, nothing);
  AppendBlock (w, 1, 3000);
  AppendBlock (w, 3001, 3000);
  w.TruncateFrom (2500);       // all of the second block and some of the first
  AppendBlock (w, 2500, 10);
  w.Flush ();
  }
  LoadResult r = Load (path, false, 0);
  EXPECT_TRUE (Numbers (r) == Range (1, 2509));
  // The one that came back as 2500 is the new one, not the old one.
  const DecodedBlock & last = *r.blocks.back ();
  EXPECT_EQ (2500, (long) last.lines [0].number);
  remove (path.c_str ());
  }

TEST (ScrollbackStore, FlagsChangeOnSavedLines)
  {
  const std::string path = TempFile ("flags");
  {
  LoadResult nothing;
  Writer w (path, false, nothing);
  AppendBlock (w, 1, 3000);
  AppendBlock (w, 3001, 3000);
  w.SetFlags (10, 0x08);
  w.SetFlags (4000, 0x08);
  w.SetFlags (4000, 0);        // and off again
  w.SetFlags (5000, 0x08);
  }
  LoadResult r = Load (path, false, 0);
  EXPECT_EQ (0x08, (int) r.blocks [0]->lines [9].flags);
  EXPECT_EQ (0, (int) r.blocks [1]->lines [999].flags);
  EXPECT_EQ (0x08, (int) r.blocks [1]->lines [1999].flags);
  remove (path.c_str ());
  }

TEST (ScrollbackStore, ClearTakesItOutOfTheFile)
  {
  const std::string path = TempFile ("clear");
  {
  LoadResult nothing;
  Writer w (path, false, nothing);
  AppendBlock (w, 1, 3000);
  w.Clear (false);
  w.Flush ();
  EXPECT_EQ (20u, ReadAll (path).size ());   // the file's header and nothing more
  AppendBlock (w, 1, 5);
  }
  LoadResult r = Load (path, false, 0);
  EXPECT_TRUE (Numbers (r) == Range (1, 5));
  remove (path.c_str ());
  }

TEST (ScrollbackStore, DropBeforeRemovesExactlyThoseLinesFromTheFile)
  {
  const std::string path = TempFile ("drop");
  {
  LoadResult nothing;
  Writer w (path, false, nothing);
  AppendBlock (w, 1, 3000);
  AppendBlock (w, 3001, 3000);
  w.DropBefore (1500);
  w.Flush ();
  }
  LoadResult r = Load (path, false, 0);
  EXPECT_TRUE (Numbers (r) == Range (1500, 6000));

  // Not just skipped on load: a line kept and a line dropped share a block, so that
  // block had to be written again without it.
  for (const BlockInfo & b : r.liveBlocks)
    EXPECT_TRUE (b.first >= 1500);
  remove (path.c_str ());
  }

TEST (ScrollbackStore, SmallBlocksAreMerged)
  {
  const std::string path = TempFile ("merge");
  {
  LoadResult nothing;
  Writer w (path, false, nothing);
  for (int i = 0; i < 100; i++)
    AppendBlock (w, 1 + i * 3, 3);
  }
  LoadResult r = Load (path, false, 0);
  EXPECT_TRUE (Numbers (r) == Range (1, 300));
  // 100 blocks in runs of 32 gives 3 merged blocks and 4 left over.
  EXPECT_EQ (7u, r.liveBlocks.size ());
  remove (path.c_str ());
  }

// A merge that didn't finish: the little blocks are still what counts.
TEST (ScrollbackStore, ATornMergeLeavesTheSmallBlocks)
  {
  const std::string path = TempFile ("tornmerge");
  {
  LoadResult nothing;
  Writer w (path, false, nothing);
  for (int i = 0; i < 32; i++)      // the 32nd triggers the merge
    AppendBlock (w, 1 + i * 3, 3);
  }
  std::vector<uint8_t> file = ReadAll (path);
  file.resize (file.size () - 5);
  WriteAll (path, file);
  LoadResult r = Load (path, false, 0);
  EXPECT_TRUE (r.tornTail);
  EXPECT_TRUE (Numbers (r) == Range (1, 96));
  EXPECT_EQ (32u, r.liveBlocks.size ());
  remove (path.c_str ());
  }

// The power going can also leave a record's header on the disk and not its payload
// (the file grew, and what should be in it is zeros).  The header is good, and says it
// replaces the little blocks: it mustn't, when its own lines aren't there.
TEST (ScrollbackStore, AMergedBlockWithoutItsPayloadLeavesTheSmallBlocks)
  {
  const std::string path = TempFile ("zeromerge");
  {
  LoadResult nothing;
  Writer w (path, false, nothing);
  for (int i = 0; i < 32; i++)      // the 32nd triggers the merge
    AppendBlock (w, 1 + i * 3, 3);
  }
  std::vector<uint8_t> file = ReadAll (path);
  const LoadResult merged = Load (path, false, 0);
  ASSERT_EQ (1u, merged.liveBlocks.size ());
  const BlockInfo m = merged.liveBlocks [0];
  ASSERT_EQ ((size_t) (m.offset + m.recordSize), file.size ());   // it's the last record
  std::fill (file.begin () + (ptrdiff_t) (m.offset + 52), file.end (), (uint8_t) 0);
  WriteAll (path, file);

  LoadResult r = Load (path, false, 0);
  EXPECT_FALSE (r.tornTail);
  EXPECT_EQ (1u, r.badBlocks);
  EXPECT_TRUE (Numbers (r) == Range (1, 96));
  EXPECT_EQ (32u, r.liveBlocks.size ());

  // And carrying on from there loses nothing.
  {
  Writer w (path, false, r);
  AppendBlock (w, 97, 3);
  }
  EXPECT_TRUE (Numbers (Load (path, false, 0)) == Range (1, 99));
  remove (path.c_str ());
  }

TEST (ScrollbackStore, DeadSpaceIsReclaimed)
  {
  const std::string path = TempFile ("reclaim");
  {
  LoadResult nothing;
  Writer w (path, false, nothing);
  // Blocks that compress badly, so that the megabyte's worth of dead space turns up.
  int64_t number = 1;
  unsigned seed = 12345;
  for (int b = 0; b < 40; b++)
    {
    BlockBuilder builder;
    for (int i = 0; i < 3000; i++)
      {
      std::string text;
      for (int c = 0; c < 80; c++)
        {
        seed = seed * 1103515245 + 12345;
        text += (char) (32 + (seed >> 16) % 90);
        }
      builder.AddLine (number++, 1000, 0, true, 80, 0, text.data (), text.size ());
      }
    const int64_t first = builder.FirstNumber ();
    const int64_t last = builder.LastNumber ();
    w.AppendLines (builder.Finish (), 3000, first, last);
    w.SetLiveFrom (number - 3000);       // only the newest block is still in the buffer
    }
  w.Flush ();
  // At most: one block live, and less than the threshold plus a block dead.
  EXPECT_TRUE (ReadAll (path).size () < 2 * 1024 * 1024);
  }
  LoadResult r = Load (path, false, 3000);
  EXPECT_EQ (3000u, r.lineCount);
  EXPECT_EQ (120000, (long) Numbers (r).back ());
  remove (path.c_str ());
  }

// Reclaiming rewrites the file, merging small blocks as it goes, while there are small
// blocks waiting to be merged.  The merge that comes later must replace only those, and
// not the blocks before them, which the rewrite has left fewer of.
TEST (ScrollbackStore, AReclaimBetweenSmallBlocksLosesNothing)
  {
  const std::string path = TempFile ("reclaimmerge");
  {
  LoadResult nothing;
  Writer w (path, false, nothing);
  unsigned seed = 777;
  for (int b = 0; b < 10; b++)
    AppendNoiseBlock (w, 1 + b * 3000, 3000, seed);      // 1 to 30000
  for (int i = 0; i < 5; i++)
    AppendBlock (w, 30001 + i * 3, 3);                  // waiting to be merged
  w.Flush ();
  const size_t before = ReadAll (path).size ();

  // The buffer's head moves on, so most of the file is dead, and it is rewritten.
  w.SetLiveFrom (24001);
  w.Flush ();
  ASSERT_LT (ReadAll (path).size (), before / 2);

  // More small blocks, enough for the merge.
  for (int i = 5; i < 41; i++)
    AppendBlock (w, 30001 + i * 3, 3);                  // to 30123
  // And another rewrite, which writes what the writer thinks the file holds.
  w.DropBefore (24001);
  w.Flush ();
  EXPECT_EQ (std::string (), w.LastError ());
  }
  LoadResult r = Load (path, false, 0);
  EXPECT_EQ (0u, r.badBlocks);
  EXPECT_TRUE (Numbers (r) == Range (24001, 30123));
  remove (path.c_str ());
  }

// A rewrite that fails (here, because something is in the way of its temporary file)
// is tried again later, and not never.
TEST (ScrollbackStore, AFailedReclaimIsTriedAgain)
  {
  const std::string path = TempFile ("reclaimretry");
  const std::string temp = path + ".tmp";
  {
  LoadResult nothing;
  Writer w (path, false, nothing);
  unsigned seed = 4242;
  for (int b = 0; b < 10; b++)
    AppendNoiseBlock (w, 1 + b * 3000, 3000, seed);
  w.Flush ();
  const size_t before = ReadAll (path).size ();

#ifdef _WIN32
  ASSERT_TRUE (CreateDirectoryA (temp.c_str (), NULL) != 0);
#else
  ASSERT_EQ (0, mkdir (temp.c_str (), 0700));
#endif
  w.SetLiveFrom (27001);
  w.Flush ();
  EXPECT_TRUE (w.LastError () != std::string ());
  EXPECT_EQ (before, ReadAll (path).size ());

#ifdef _WIN32
  RemoveDirectoryA (temp.c_str ());
#else
  rmdir (temp.c_str ());
#endif
  // Not straight away: a disk that's full stays full for a while.  But soon.
  for (int i = 0; i < 31; i++)
    w.SetLiveFrom (27001);
  w.Flush ();
  EXPECT_LT (ReadAll (path).size (), before / 2);
  }
  LoadResult r = Load (path, false, 0);
  EXPECT_TRUE (Numbers (r) == Range (27001, 30000));
  remove (path.c_str ());
  }

TEST (ScrollbackStore, OneWriterAtATime)
  {
  const std::string path = TempFile ("exclusive");
  LoadResult nothing;
  Writer first (path, false, nothing);
  ASSERT_TRUE (first.IsOpen ());
  Writer second (path, false, Load (path, false, 0));
  EXPECT_FALSE (second.IsOpen ());
  AppendBlock (first, 1, 3);
  first.Flush ();
  // One that couldn't have the file doesn't delete it from under the one that has.
  second.Close (true);
  EXPECT_EQ (3u, Load (path, false, 0).lineCount);
  first.Close (true);
  EXPECT_TRUE (Load (path, false, 0).status == LoadStatus::NoFile);
  }

TEST (ScrollbackStore, ALargeBufferRoundTrips)
  {
  const std::string path = TempFile ("large");
  const size_t kLines = 100000;
  {
  LoadResult nothing;
  Writer w (path, false, nothing);
  for (size_t at = 0; at < kLines; at += kBlockLines)
    {
    const size_t n = (std::min) (kBlockLines, kLines - at);
    AppendBlock (w, (int64_t) at + 1, n);
    }
  }
  LoadResult r = Load (path, false, 0);
  EXPECT_EQ (kLines, r.lineCount);
  EXPECT_TRUE (Numbers (r) == Range (1, (int64_t) kLines));
  const Line & sample = r.blocks [10]->lines [5];
  EXPECT_EQ (std::string ("line ") + std::to_string (sample.number),
             std::string (r.blocks [10]->text + sample.textOffset, sample.length));
  remove (path.c_str ());
  }

TEST (ScrollbackStore, WriteFileAtomicallyReplaces)
  {
  const std::string path = TempFile ("atomic");
  EXPECT_TRUE (WriteFileAtomically (path, "one", 3));
  EXPECT_TRUE (WriteFileAtomically (path, "two!", 4));
  const std::vector<uint8_t> got = ReadAll (path);
  EXPECT_EQ (std::string ("two!"), std::string (got.begin (), got.end ()));
  EXPECT_TRUE (ReadAll (path + ".tmp").empty ());
  remove (path.c_str ());
  }

// ---------------- the document ----------------

namespace {

// A document whose buffer is set up the way SetUpOutputWindow does it, without the
// banner or the fonts.  Several in turn, with one ID, are sessions of one world.
struct SessionFixture
  {
  CMUSHclientDoc doc;

  explicit SessionFixture (const char * id, const long maxLines = 5000, const bool bUtf8 = false,
                           const bool bStart = true, const bool bNamed = true,
                           const char * worldFile = "a saved world.mcl")
    {
    // The test host's constructor seeds a first line for documents that never get a
    // view.  Back to the state SetUpOutputWindow starts from.
    while (!doc.m_LineList.IsEmpty ())
      {
      delete doc.m_LineList.GetHead ();
      doc.m_LineList.RemoveHead ();
      }
    doc.m_pCurrentLine = nullptr;
    doc.m_LinePositions.Reset (doc.m_LineList.GetHeadPosition ());
    doc.m_strWorldID = id;
    if (bNamed)
      doc.SetPathName (worldFile, FALSE);   // so that its file is kept
    doc.m_maxlines = maxLines;
    doc.m_wrap = FALSE;
    doc.m_nWrapColumn = 1000;
    doc.m_bUTF_8 = bUtf8;
    if (bStart)
      doc.StartOutputBuffer ();
    }

  // Closes this session, as far as the file is concerned.
  void End ()
    {
    doc.FlushSavedScrollback (true);
    }

  long Count ()
    {
    return doc.GetLinesInBufferCount ();
    }

  std::string Text (const long line)
    {
    return vs (doc.GetLineInfo (line, 1));
    }
  };

// The tests' worlds' files go under their IDs in the test's working directory.
void Forget (const char * id)
  {
  CMUSHclientDoc doc;
  doc.m_strWorldID = id;
  remove ((const char *) doc.ScrollbackFilePath ());
  remove ((const char *) (doc.ScrollbackFilePath () + ".tmp"));
  remove ((const char *) doc.CommandHistoryFilePath ());
  const CString strPath = doc.ScrollbackFilePath ();
  remove ((const char *) (strPath.Left (strPath.GetLength () - 5) + ".owner"));
  }

} // anonymous namespace

static void FeedLine (CMUSHclientDoc & doc, const std::string & text)
  {
  const std::string s = text + "\n";
  doc.DisplayMsg (s.c_str (), (int) s.size (), 0);
  }

// Lines handed to the worker reach the disk when it gets to them.
static size_t WaitForSavedLines (CMUSHclientDoc & doc, const size_t lines)
  {
  size_t got = 0;
  for (int i = 0; i < 500; i++)
    {
    got = Load ((const char *) doc.ScrollbackFilePath (), doc.m_bUTF_8 != 0, 0).lineCount;
    if (got >= lines)
      return got;
    std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
  return got;
  }

TEST (SavedScrollback, EverythingInALineComesBack)
  {
  const char * id = "testeverything";
  Forget (id);

  struct SavedStyle { unsigned short length, flags; COLORREF fore, back; };
  struct SavedLine
    {
    std::string text;
    int flags;
    bool hard;
    time_t when;
    long number;
    int charCount;
    short preamble;
    std::vector<SavedStyle> styles;
    };
  std::vector<SavedLine> saved;

  {
  SessionFixture s (id);
  FeedLine (s.doc, "a line from the MUD");
  s.doc.Note ("a note");
  s.doc.ColourNote ("#FF8040", "#102030", "an RGB note");
  s.doc.DisplayMsg ("typed by the player\n", 20, USER_INPUT | LOG_LINE);
  FeedLine (s.doc, kNonUtf8.c_str ());
  FeedLine (s.doc, "a bookmarked line");
  s.doc.Bookmark (s.Count () - 1, TRUE);
  // A style run in mid-line.
  s.doc.ColourTell ("red", "", "red, ");
  s.doc.ColourTell ("blue", "", "then blue");
  s.doc.Note ("");
  // Backdate one, so that "when" means something.
  CLine * pFirst = s.doc.m_LineList.GetHead ();
  pFirst->m_theTime = CTime (pFirst->m_theTime.GetTime () - 86400);
  pFirst->m_iPreambleOffset = 3;

  for (POSITION pos = s.doc.m_LineList.GetHeadPosition (); pos; )
    {
    CLine * p = s.doc.m_LineList.GetNext (pos);
    if (p == s.doc.m_pCurrentLine)
      continue;
    SavedLine l;
    l.text = std::string (p->text, p->len);
    l.flags = p->flags & ~LOG_LINE;
    l.hard = p->hard_return;
    l.when = p->m_theTime.GetTime ();
    l.number = p->m_nLineNumber;
    l.charCount = p->m_iCharCount;
    l.preamble = p->m_iPreambleOffset;
    for (POSITION sp = p->styleList.GetHeadPosition (); sp; )
      {
      CStyle * style = p->styleList.GetNext (sp);
      l.styles.push_back (SavedStyle { style->iLength, style->iFlags, style->iForeColour, style->iBackColour });
      }
    saved.push_back (l);
    }
  ASSERT_GE ((int) saved.size (), 7);
  s.End ();
  }

  SessionFixture s (id);
  EXPECT_EQ (saved.back ().number, s.doc.m_nLastRestoredLine);
  // The saved lines, then the marker, then the line in progress.
  ASSERT_EQ ((long) saved.size () + 2, s.Count ());

  size_t i = 0;
  for (POSITION pos = s.doc.m_LineList.GetHeadPosition (); pos && i < saved.size (); i++)
    {
    CLine * p = s.doc.m_LineList.GetNext (pos);
    const SavedLine & l = saved [i];
    EXPECT_TRUE (l.text == std::string (p->text, p->len));
    EXPECT_EQ (l.flags, (int) p->flags);      // no LOG_LINE, on purpose
    EXPECT_TRUE (l.hard == p->hard_return);
    EXPECT_EQ ((long) l.when, (long) p->m_theTime.GetTime ());
    EXPECT_EQ (l.number, p->m_nLineNumber);
    EXPECT_EQ (l.charCount, p->m_iCharCount);
    EXPECT_EQ ((int) l.preamble, (int) p->m_iPreambleOffset);
    ASSERT_EQ ((long) l.styles.size (), (long) p->styleList.GetCount ());
    size_t n = 0;
    for (POSITION sp = p->styleList.GetHeadPosition (); sp; n++)
      {
      CStyle * style = p->styleList.GetNext (sp);
      EXPECT_EQ ((int) l.styles [n].length, (int) style->iLength);
      EXPECT_EQ ((int) l.styles [n].flags, (int) style->iFlags);
      EXPECT_EQ ((long) l.styles [n].fore, (long) style->iForeColour);
      EXPECT_EQ ((long) l.styles [n].back, (long) style->iBackColour);
      }
    }

  // The marker: a note, and nothing a trigger or a log will care about.
  EXPECT_TRUE (s.Text ((long) saved.size () + 1).find ("End of the saved output") != std::string::npos);
  VARIANT note = s.doc.GetLineInfo ((long) saved.size () + 1, 4);
  EXPECT_TRUE (note.boolVal != 0);

  s.End ();
  Forget (id);
  }

TEST (SavedScrollback, NoTriggerFiresAndNothingIsReprocessed)
  {
  const char * id = "testtriggers";
  Forget (id);
  {
  SessionFixture s (id);
  for (int i = 0; i < 10; i++)
    FeedLine (s.doc, "You are hungry");
  s.End ();
  }

  SessionFixture s (id, 5000, false, false);
  EXPECT_EQ ((long) eOK, s.doc.AddTriggerEx ("hunger", "You are hungry", "", eEnabled, -1, 0, "", "",
                                             eSendToWorld, 100));
  s.doc.StartOutputBuffer ();
  EXPECT_EQ (12L, s.Count ());   // 10, the marker, the new line
  VARIANT matched = s.doc.GetTriggerInfo ("hunger", 21);
  EXPECT_EQ (0L, (long) matched.lVal);

  // It does work: a new line matches.
  FeedLine (s.doc, "You are hungry");
  matched = s.doc.GetTriggerInfo ("hunger", 21);
  EXPECT_EQ (1L, (long) matched.lVal);

  s.End ();
  Forget (id);
  }

TEST (SavedScrollback, WhatPluginsSeeOfARestoredLine)
  {
  const char * id = "testplugins";
  Forget (id);
  {
  SessionFixture s (id);
  for (int i = 1; i <= 250; i++)
    FeedLine (s.doc, "line " + std::to_string (i));
  s.End ();
  }

  SessionFixture s (id);
  // Across the jump index's boundaries, which restoring has to have built.
  EXPECT_EQ (252L, s.Count ());
  for (long n : { 1L, 2L, 100L, 101L, 102L, 200L, 201L, 250L })
    EXPECT_TRUE ("line " + std::to_string (n) == s.Text (n));
  EXPECT_EQ (VT_EMPTY, (int) s.doc.GetLineInfo (253, 1).vt);

  // Numbers go on from the old ones.
  VARIANT v = s.doc.GetLineInfo (250, 10);
  const long lastOld = v.lVal;
  EXPECT_EQ (250L, lastOld);
  FeedLine (s.doc, "the next line");
  v = s.doc.GetLineInfo (s.Count () - 1, 10);
  EXPECT_GT ((long) v.lVal, lastOld);
  EXPECT_GT ((long) s.doc.m_total_lines, lastOld);

  // Not logged, as far as it says.
  v = s.doc.GetLineInfo (1, 6);
  EXPECT_FALSE (v.boolVal != 0);

  // Its elapsed time is how long before this session it arrived: never later than it,
  // to within the second a line's time is held to, and no sillier than the clock.
  v = s.doc.GetLineInfo (1, 13);
  EXPECT_TRUE (v.dblVal <= 1.0);
  EXPECT_TRUE (v.dblVal > -3600.0);

  s.End ();
  Forget (id);
  }

TEST (SavedScrollback, OldSendLinksAreDeadAndHyperlinksAreNot)
  {
  const char * id = "testlinks";
  Forget (id);
  {
  SessionFixture s (id);
  s.doc.Hyperlink ("go north", "a send link", "hint", "", "", FALSE);
  s.doc.Note ("");
  s.doc.Hyperlink ("http://example.com/", "a web link", "web hint", "", "", TRUE);
  s.doc.Note ("");
  s.End ();
  }

  SessionFixture s (id);
  CStyle * pSend = nullptr;
  CStyle * pWeb = nullptr;
  for (POSITION pos = s.doc.m_LineList.GetHeadPosition (); pos; )
    {
    CLine * p = s.doc.m_LineList.GetNext (pos);
    const std::string text (p->text, p->len);
    for (POSITION sp = p->styleList.GetHeadPosition (); sp; )
      {
      CStyle * style = p->styleList.GetNext (sp);
      if (style->iLength == 0)
        continue;      // the one that ends the link
      if (text == "a send link") pSend = style;
      if (text == "a web link") pWeb = style;
      }
    }
  ASSERT_TRUE (pSend != nullptr);
  ASSERT_TRUE (pWeb != nullptr);
  EXPECT_EQ (ACTION_NONE, pSend->iFlags & ACTIONTYPE);
  EXPECT_TRUE (pSend->pAction == nullptr);
  EXPECT_EQ (ACTION_HYPERLINK, pWeb->iFlags & ACTIONTYPE);
  ASSERT_TRUE (pWeb->pAction != nullptr);
  EXPECT_TRUE (std::string ("http://example.com/") == (const char *) pWeb->pAction->m_strAction);
  EXPECT_TRUE (std::string ("web hint") == (const char *) pWeb->pAction->m_strHint);

  s.End ();
  Forget (id);
  }

// ---- every way out of the buffer has to be a way out of the file ----

TEST (SavedScrollback, ClearOutputRemovesItFromTheFile)
  {
  const char * id = "testclear";
  Forget (id);
  {
  SessionFixture s (id);
  FeedLine (s.doc, "before the clear");
  s.doc.FlushSavedScrollback (false);
  ASSERT_EQ (1u, WaitForSavedLines (s.doc, 1));
  s.doc.ClearOutput ();
  size_t lines = 1;
  for (int i = 0; i < 500 && lines != 0; i++)
    {
    lines = Load ((const char *) s.doc.ScrollbackFilePath (), false, 0).lineCount;
    std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
  EXPECT_EQ (0u, lines);
  FeedLine (s.doc, "after the clear");
  s.End ();
  }
  SessionFixture s (id);
  EXPECT_TRUE (std::string ("after the clear") == s.Text (1));
  EXPECT_EQ (3L, s.Count ());
  s.End ();
  Forget (id);
  }

TEST (SavedScrollback, DeleteLinesRemovesSavedLinesAndReusesTheirNumbers)
  {
  const char * id = "testdelete";
  Forget (id);
  {
  SessionFixture s (id);
  for (int i = 1; i <= 5; i++)
    FeedLine (s.doc, "line " + std::to_string (i));
  s.doc.FlushSavedScrollback (false);
  ASSERT_EQ (5u, WaitForSavedLines (s.doc, 5));

  s.doc.m_bInSendToScript = false;    // world.DeleteLines refuses otherwise
  s.doc.DeleteLines (2);
  EXPECT_TRUE (std::string ("line 3") == s.Text (3));
  FeedLine (s.doc, "new four");
  FeedLine (s.doc, "new five");
  s.End ();
  }
  SessionFixture s (id);
  EXPECT_TRUE (std::string ("line 3") == s.Text (3));
  EXPECT_TRUE (std::string ("new four") == s.Text (4));
  EXPECT_TRUE (std::string ("new five") == s.Text (5));
  EXPECT_EQ (7L, s.Count ());
  s.End ();
  Forget (id);
  }

TEST (SavedScrollback, ClearingTheHeadRemovesItFromTheFile)
  {
  const char * id = "testhead";
  Forget (id);
  {
  SessionFixture s (id);
  for (int i = 1; i <= 10; i++)
    FeedLine (s.doc, "line " + std::to_string (i));
  s.doc.FlushSavedScrollback (false);
  ASSERT_EQ (10u, WaitForSavedLines (s.doc, 10));
  s.doc.DeleteHeadLines (7);
  // Mid-session, and without a clean close, the text has gone from the file.
  size_t lines = 10;
  for (int i = 0; i < 500 && lines != 3; i++)
    {
    lines = Load ((const char *) s.doc.ScrollbackFilePath (), false, 0).lineCount;
    std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
  EXPECT_EQ (3u, lines);
  s.End ();
  }
  SessionFixture s (id);
  EXPECT_TRUE (std::string ("line 8") == s.Text (1));
  EXPECT_TRUE (std::string ("line 10") == s.Text (3));
  s.End ();
  Forget (id);
  }

TEST (SavedScrollback, AnOmittedLineIsNeverSaved)
  {
  const char * id = "testomit";
  Forget (id);
  {
  SessionFixture s (id);
  EXPECT_EQ ((long) eOK, s.doc.AddTriggerEx ("gag", "secret", "", eEnabled | eOmitFromOutput, -1, 0, "", "",
                                             eSendToWorld, 100));
  FeedLine (s.doc, "shown");
  FeedLine (s.doc, "secret");
  FeedLine (s.doc, "also shown");
  s.End ();
  }
  SessionFixture s (id);
  EXPECT_TRUE (std::string ("shown") == s.Text (1));
  EXPECT_TRUE (std::string ("also shown") == s.Text (2));
  EXPECT_EQ (4L, s.Count ());
  s.End ();
  Forget (id);
  }

TEST (SavedScrollback, BookmarkOnAnAlreadySavedLine)
  {
  const char * id = "testbookmark";
  Forget (id);
  {
  SessionFixture s (id);
  for (int i = 1; i <= 5; i++)
    FeedLine (s.doc, "line " + std::to_string (i));
  s.doc.FlushSavedScrollback (false);
  ASSERT_EQ (5u, WaitForSavedLines (s.doc, 5));
  s.doc.Bookmark (2, TRUE);
  s.End ();
  }
  SessionFixture s (id);
  VARIANT v = s.doc.GetLineInfo (2, 7);
  EXPECT_TRUE (v.boolVal != 0);
  v = s.doc.GetLineInfo (3, 7);
  EXPECT_FALSE (v.boolVal != 0);
  s.End ();
  Forget (id);
  }

// ---- what's in a session ----

TEST (SavedScrollback, AWrappedLineIsNotSavedUntilItIsFinished)
  {
  const char * id = "testwrap";
  Forget (id);
  SessionFixture s (id);
  s.doc.m_wrap = TRUE;
  s.doc.m_nWrapColumn = 20;
  FeedLine (s.doc, "finished");
  const std::string longLine ("this line is a long way wider than twenty columns, with no newline yet");
  s.doc.DisplayMsg (longLine.c_str (), (int) longLine.size (), 0);
  ASSERT_GT (s.Count (), 3L);

  s.doc.FlushSavedScrollback (false);
  ASSERT_EQ (1u, WaitForSavedLines (s.doc, 1));
  std::this_thread::sleep_for (std::chrono::milliseconds (50));
  EXPECT_EQ (1u, Load ((const char *) s.doc.ScrollbackFilePath (), false, 0).lineCount);

  s.End ();    // closing, everything counts
  LoadResult r = Load ((const char *) s.doc.ScrollbackFilePath (), false, 0);
  EXPECT_GT (r.lineCount, 3u);
  Forget (id);
  }

TEST (SavedScrollback, TheLastLineGetsAHardReturn)
  {
  const char * id = "testprompt";
  Forget (id);
  {
  SessionFixture s (id);
  s.doc.DisplayMsg ("HP: 100> ", 9, 0);   // a prompt, with no newline
  s.End ();
  }
  SessionFixture s (id);
  EXPECT_TRUE (std::string ("HP: 100> ") == s.Text (1));
  VARIANT v = s.doc.GetLineInfo (1, 3);
  EXPECT_TRUE (v.boolVal != 0);
  s.End ();
  Forget (id);
  }

TEST (SavedScrollback, TheMarkerAndWhatPrecedesTheSessionAreNotSaved)
  {
  const char * id = "testmarker";
  Forget (id);
  {
  SessionFixture s (id);
  FeedLine (s.doc, "session one");
  s.End ();
  }
  for (int session = 0; session < 3; session++)
    {
    SessionFixture s (id);
    s.doc.Note ("a banner line");
    s.doc.ScrollbackSessionStarted ();
    // Nothing new this time.  The saved data must not grow by the marker or the banner.
    EXPECT_TRUE (std::string ("session one") == s.Text (1));
    EXPECT_TRUE (s.Text (2).find ("End of the saved output") != std::string::npos);
    EXPECT_TRUE (std::string ("a banner line") == s.Text (3));
    EXPECT_EQ (4L, s.Count ());
    s.End ();
    }
  Forget (id);
  }

TEST (SavedScrollback, OnlyWhatTheBufferHoldsComesBack)
  {
  const char * id = "testcap";
  Forget (id);
  {
  SessionFixture s (id, 0);    // unlimited
  for (int i = 1; i <= 1000; i++)
    FeedLine (s.doc, "line " + std::to_string (i));
  EXPECT_EQ (1001L, s.Count ());
  s.End ();
  }
  {
  SessionFixture s (id, 300);
  EXPECT_LE (s.Count (), 300L);
  EXPECT_TRUE (std::string ("line 1000") == s.Text (s.Count () - 2));
  s.End ();
  }
  // Unlimited means unlimited, in both directions.
  {
  SessionFixture s (id, 0);
  EXPECT_GE (s.Count (), 200L);
  s.End ();
  }
  Forget (id);
  }

TEST (SavedScrollback, TurningItOffDeletesTheFile)
  {
  const char * id = "testoff";
  Forget (id);
  SessionFixture s (id);
  FeedLine (s.doc, "something");
  s.doc.FlushSavedScrollback (false);
  ASSERT_EQ (1u, WaitForSavedLines (s.doc, 1));

  s.doc.m_bSaveScrollback = FALSE;
  s.doc.ScrollbackTick ();
  EXPECT_TRUE (s.doc.m_pScrollback == nullptr);
  EXPECT_TRUE (Load ((const char *) s.doc.ScrollbackFilePath (), false, 0).status == LoadStatus::NoFile);

  // On again: this session's lines go out next time, all of them.
  s.doc.m_bSaveScrollback = TRUE;
  s.doc.ScrollbackTick ();
  EXPECT_TRUE (s.doc.m_pScrollback != nullptr);
  s.End ();
  LoadResult r = Load ((const char *) s.doc.ScrollbackFilePath (), false, 0);
  ASSERT_TRUE (!r.blocks.empty ());
  const Line & line = r.blocks [0]->lines [0];
  EXPECT_TRUE (std::string ("something") == std::string (r.blocks [0]->text + line.textOffset, line.length));
  Forget (id);
  }

TEST (SavedScrollback, AWorldStartedWithItOffSavesNothing)
  {
  const char * id = "testoffstart";
  Forget (id);
  CMUSHclientDoc doc;
  doc.m_strWorldID = id;
  doc.m_bSaveScrollback = FALSE;
  EXPECT_FALSE (doc.RestoreSavedScrollback ());
  EXPECT_TRUE (doc.m_pScrollback == nullptr);
  doc.ScrollbackTick ();
  EXPECT_TRUE (Load ((const char *) doc.ScrollbackFilePath (), false, 0).status == LoadStatus::NoFile);
  }

TEST (SavedScrollback, TheClearButtonForgetsTheFileAndNotTheScreen)
  {
  const char * id = "testbutton";
  Forget (id);
  {
  SessionFixture s (id);
  FeedLine (s.doc, "forget me");
  s.doc.FlushSavedScrollback (false);
  ASSERT_EQ (1u, WaitForSavedLines (s.doc, 1));
  s.doc.ClearSavedScrollback ();
  EXPECT_EQ (0u, Load ((const char *) s.doc.ScrollbackFilePath (), false, 0).lineCount);
  EXPECT_TRUE (std::string ("forget me") == s.Text (1));
  FeedLine (s.doc, "keep me");
  s.End ();
  }
  SessionFixture s (id);
  EXPECT_TRUE (std::string ("keep me") == s.Text (1));
  EXPECT_EQ (3L, s.Count ());
  s.End ();
  Forget (id);
  }

TEST (SavedScrollback, ANeverSavedWorldLeavesNothingBehind)
  {
  const char * id = "testnewworld";
  Forget (id);
  std::string path;
  {
  SessionFixture s (id, 5000, false, true, false);
  FeedLine (s.doc, "a line");
  path = (const char *) s.doc.ScrollbackFilePath ();
  s.End ();
  }
  EXPECT_TRUE (Load (path, false, 0).status == LoadStatus::NoFile);
  }

TEST (SavedScrollback, ChangingUtf8LosesTheOldTextAndNothingElse)
  {
  const char * id = "testutf8";
  Forget (id);
  {
  SessionFixture s (id);
  FeedLine (s.doc, "old encoding");
  s.End ();
  }
  {
  SessionFixture s (id, 5000, true);
  EXPECT_EQ (1L, s.Count ());       // nothing came back
  FeedLine (s.doc, "new encoding");

  // ... and mid-session:
  s.doc.m_bUTF_8 = FALSE;
  s.doc.ScrollbackTick ();
  FeedLine (s.doc, "newer encoding");
  s.End ();
  }
  SessionFixture s (id);
  EXPECT_TRUE (std::string ("newer encoding") == s.Text (1));
  s.End ();
  Forget (id);
  }

// A restored line has no MXP line tag: GetLineInfo (n, 14) is 0, and not whatever the
// heap had there.
TEST (SavedScrollback, ARestoredLineHasNoMxpTag)
  {
  const char * id = "testmxptag";
  Forget (id);
  {
  SessionFixture s (id);
  FeedLine (s.doc, "a line");
  s.End ();
  }
  SessionFixture s (id);
  ASSERT_TRUE (std::string ("a line") == s.Text (1));
  VARIANT v = s.doc.GetLineInfo (1, 14);
  EXPECT_EQ (0, (int) v.iVal);
  EXPECT_EQ (0, (int) s.doc.m_LineList.GetHead ()->m_iMXPLineTag);
  s.End ();
  Forget (id);
  }

// An alias that echoes what was typed draws it even with echo off (upstream's way), and
// so a password matching one is on the screen.  It must not also go to the file.
TEST (SavedScrollback, WhatAnAliasEchoesWithEchoOffIsNotSaved)
  {
  const char * id = "testnoecho";
  Forget (id);
  std::string path;
  {
  SessionFixture s (id);
  path = (const char *) s.doc.ScrollbackFilePath ();
  s.doc.m_enable_aliases = TRUE;
  s.doc.m_display_my_input = TRUE;
  s.doc.m_iConnectPhase = eConnectConnectedToMud;
  s.doc.m_pTransport = new RecordingTransport ();
  CAlias * a = new CAlias ();
  a->name = "pw *";
  a->contents = "connect %1";
  a->bEnabled = TRUE;
  a->bRegexp = false;
  a->bEchoAlias = TRUE;
  a->iSendTo = eSendToWorld;
  a->iSequence = DEFAULT_ALIAS_SEQUENCE;
  a->strLabel = "pw";
  a->regexp = regcomp (ConvertToRegularExpression (a->name, true, true), 0);
  s.doc.GetAliasMap ().SetAt (a->strLabel, a);
  s.doc.GetAliasArray ().Add (a);
  s.doc.GetAliasRevMap () [a] = "pw *";

  FeedLine (s.doc, "Password:");
  bool bOmit = false;
  s.doc.m_bNoEcho = true;
  s.doc.EvaluateCommand (CString ("pw hunter3"), true, bOmit, false);
  s.doc.m_bNoEcho = false;
  s.doc.EvaluateCommand (CString ("pw publicword"), true, bOmit, false);   // the control
  FeedLine (s.doc, "Welcome.");
  EXPECT_EQ (2L, a->nMatched);

  // Both on the screen.
  bool bShown = false;
  for (POSITION pos = s.doc.m_LineList.GetHeadPosition (); pos; )
    {
    const CLine * p = s.doc.m_LineList.GetNext (pos);
    if (std::string (p->text, p->len).find ("hunter3") != std::string::npos)
      bShown = true;
    }
  EXPECT_TRUE (bShown);
  s.End ();
  }

  LoadResult r = Load (path, false, 0);
  std::string all;
  for (const auto & block : r.blocks)
    all += std::string (block->text, block->textSize) + "|";
  EXPECT_TRUE (all.find ("hunter3") == std::string::npos);
  EXPECT_TRUE (all.find ("publicword") != std::string::npos);
  EXPECT_TRUE (all.find ("Password:") != std::string::npos);
  EXPECT_TRUE (all.find ("Welcome.") != std::string::npos);

  // And it comes back without a hole that matters.
  SessionFixture s (id);
  EXPECT_TRUE (std::string ("Password:") == s.Text (1));
  s.End ();
  Forget (id);
  }

namespace {

// A real file, since whether a world file is still there is what decides some of this.
std::string MakeWorldFile (const char * name)
  {
  const char * dir = getenv ("TEMP");
  if (!dir)
    dir = "/tmp";
  const std::string path = std::string (dir) + "/mc_scrollback_" + name + ".mcl";
  if (FILE * f = fopen (path.c_str (), "wb"))
    {
    fputs ("<world/>", f);
    fclose (f);
    }
  return path;
  }

void RemoveSaved (const std::string & scrollbackPath)
  {
  const std::string base = scrollbackPath.substr (0, scrollbackPath.size () - 5);
  for (const char * ext : { ".mcsb", ".history", ".owner", ".mcsb.tmp" })
    remove ((base + ext).c_str ());
  }

} // anonymous namespace

// A world file copied (for an alt, say) has the same ID as the original.  Each has its
// own output and history all the same; and a world file that has been moved finds its
// own again.
TEST (SavedScrollback, ACopiedWorldFileHasFilesOfItsOwn)
  {
  const char * id = "testclone";
  Forget (id);
  const std::string original = MakeWorldFile ("original");
  const std::string copy = MakeWorldFile ("copy");
  const std::string moved = MakeWorldFile ("moved");
  std::string copyFile;
  {
  SessionFixture s (id, 5000, false, true, true, original.c_str ());
  FeedLine (s.doc, "the original's");
  s.End ();
  }
  {
  SessionFixture s (id, 5000, false, true, true, copy.c_str ());
  EXPECT_EQ (1L, s.Count ());       // nothing of the original's
  copyFile = (const char *) s.doc.ScrollbackFilePath ();
  FeedLine (s.doc, "the copy's");
  s.End ();
  }
  {
  SessionFixture s (id, 5000, false, true, true, original.c_str ());
  EXPECT_TRUE (std::string ("the original's") == s.Text (1));
  EXPECT_EQ (3L, s.Count ());
  EXPECT_TRUE (s.doc.CommandHistoryFilePath () != (copyFile.substr (0, copyFile.size () - 5) + ".history").c_str ());
  s.End ();
  }
  {
  SessionFixture s (id, 5000, false, true, true, copy.c_str ());
  EXPECT_TRUE (std::string ("the copy's") == s.Text (1));
  EXPECT_EQ (3L, s.Count ());
  s.End ();
  }

  // The original moves: the world file at the new place is it.
  remove (original.c_str ());
  {
  SessionFixture s (id, 5000, false, true, true, moved.c_str ());
  EXPECT_TRUE (std::string ("the original's") == s.Text (1));
  s.End ();
  }
  // ... and is the owner from now on: the copy still has its own.
  {
  SessionFixture s (id, 5000, false, true, true, copy.c_str ());
  EXPECT_TRUE (std::string ("the copy's") == s.Text (1));
  s.End ();
  }

  RemoveSaved (copyFile);
  remove (copy.c_str ());
  remove (moved.c_str ());
  Forget (id);
  }

// A world file with no ID is given a new one every time it is opened, until it is
// saved.  Its output is found again meanwhile, and when the ID sticks.
TEST (SavedScrollback, AWorldWithANewIDEachTimeFindsItsOutput)
  {
  const std::string world = MakeWorldFile ("noid");
  std::string byPath;
  for (const char * id : { "testnewid1", "testnewid2", "testnewid3" })
    Forget (id);
  {
  SessionFixture s ("testnewid1", 5000, false, false, true, world.c_str ());
  s.doc.m_bWorldIDIsNew = true;
  s.doc.StartOutputBuffer ();
  byPath = (const char *) s.doc.ScrollbackFilePath ();
  EXPECT_TRUE (byPath.find ("testnewid1") == std::string::npos);
  FeedLine (s.doc, "kept by the world file's path");
  s.End ();
  }
  {
  SessionFixture s ("testnewid2", 5000, false, false, true, world.c_str ());
  s.doc.m_bWorldIDIsNew = true;
  s.doc.StartOutputBuffer ();
  EXPECT_TRUE (std::string ("kept by the world file's path") == s.Text (1));
  s.End ();
  }
  // Saved with this one at last: it takes them over.
  {
  SessionFixture s ("testnewid3", 5000, false, true, true, world.c_str ());
  EXPECT_TRUE (std::string ("kept by the world file's path") == s.Text (1));
  EXPECT_TRUE (std::string ((const char *) s.doc.ScrollbackFilePath ()).find ("testnewid3") != std::string::npos);
  s.End ();
  }
  EXPECT_TRUE (Load (byPath, false, 0).status == LoadStatus::NoFile);
  RemoveSaved (byPath);
  remove (world.c_str ());
  Forget ("testnewid3");
  }

#ifdef _WIN32

// The command box's history, on Windows (the port's is in wx_world_child.cpp).
namespace {

struct SendViewFixture
  {
  CMUSHclientDoc * doc;
  CSendView *      view;
  std::string      path;

  explicit SendViewFixture (const char * id)
    {
    Forget (id);
    doc = new CMUSHclientDoc;
    doc->m_strWorldID = id;
    doc->SetPathName ("a saved world.mcl", FALSE);
    doc->m_nHistoryLines = 3;
    view = (CSendView *) RUNTIME_CLASS (CSendView)->CreateObject ();
    doc->AddView (view);
    path = (const char *) doc->CommandHistoryFilePath ();
    }
  ~SendViewFixture ()
    {
    // As in tests/win/test_win_worldview.cpp: the document first, which leaves the
    // view alone, then the view.
    delete doc;
    delete (CObject *) view;
    }
  std::vector<std::string> Saved ()
    {
    std::vector<std::string> got;
    command_history::Load (path, got);
    return got;
    }
  };

} // anonymous namespace

// Nothing kept while echo is off reaches the file, whichever way it was kept.
TEST (SavedCommandHistory, NothingKeptWithEchoOffIsSaved)
  {
  SendViewFixture f ("testhistecho");
  f.doc->m_nHistoryLines = 20;
  f.view->AddToCommandHistory ("north");
  f.doc->m_bNoEcho = true;
  f.view->AddToCommandHistory ("typed");               // not kept at all
  f.view->AddToCommandHistory ("pushed", true);        // world.PushCommand: kept, in memory
  f.view->RememberCommand ("escaped");                 // Escape, saving what it threw away
  f.doc->m_bAlwaysRecordCommandHistory = TRUE;
  f.view->AddToCommandHistory ("always");
  f.doc->m_bNoEcho = false;
  f.view->AddToCommandHistory ("south");
  EXPECT_EQ (5L, (long) f.view->m_msgList.GetCount ());

  f.view->SaveCommandHistoryIfChanged (true);
  const std::vector<std::string> saved = f.Saved ();
  ASSERT_EQ (2u, saved.size ());
  EXPECT_EQ (std::string ("north"), saved [0]);
  EXPECT_EQ (std::string ("south"), saved [1]);
  remove (f.path.c_str ());
  }

// A change that leaves the count and the last command as they were is still a change.
TEST (SavedCommandHistory, EveryChangeIsSaved)
  {
  SendViewFixture f ("testhistchange");
  for (const char * c : { "a", "b", "c" })
    f.view->AddToCommandHistory (c);
  f.view->SaveCommandHistoryIfChanged (true);
  EXPECT_TRUE (f.Saved () == std::vector<std::string> ({ "a", "b", "c" }));

  // Three at most: "x" pushes "a" out, then "c" pushes "b" out.  Three, ending in "c".
  f.view->AddToCommandHistory ("x");
  f.view->AddToCommandHistory ("c");
  f.view->SaveCommandHistoryIfChanged (true);
  EXPECT_TRUE (f.Saved () == std::vector<std::string> ({ "c", "x", "c" }));

  // Clearing is a change too.
  f.view->ForgetCommandHistory ();
  f.view->SaveCommandHistoryIfChanged (true);
  EXPECT_TRUE (f.Saved ().empty ());
  remove (f.path.c_str ());
  }

#endif // _WIN32

// ---------------- how long it all takes ----------------

static std::string RealisticLine (unsigned & seed, const int i)
  {
  static const char * const kWords [] = {
    "The", "goblin", "swings", "its", "club", "at", "you", "and", "misses.", "You", "dodge", "out", "of", "the",
    "way", "with", "a", "flourish", "room", "north", "south", "east", "west", "exits:", "Ankh-Morpork", "street",
    "A", "large", "crowd", "has", "gathered", "around", "fountain", "to", "watch", "wizard", "argue", "seagull." };
  std::string s;
  seed = seed * 1103515245 + 12345;
  const int words = 4 + (seed >> 16) % 12;
  for (int w = 0; w < words; w++)
    {
    seed = seed * 1103515245 + 12345;
    s += kWords [(seed >> 16) % (sizeof kWords / sizeof kWords [0])];
    s += ' ';
    }
  s += std::to_string (i % 997);
  return s;
  }

static void MeasureScrollback (const size_t lines)
  {
  const char * id = "testperf";
  Forget (id);
  typedef std::chrono::steady_clock Clock;
  const auto seconds = [] (Clock::time_point from) { return std::chrono::duration<double> (Clock::now () - from).count (); };

  double fill, encode, wait, load;
  std::string path;
  double worstTick = 0;
  long ticks = 0;
  {
  SessionFixture s (id, 0);
  path = (const char *) s.doc.ScrollbackFilePath ();
  unsigned seed = 42;
  auto start = Clock::now ();
  // Directly, so as to time the saving and not the line processing.  A line has one to
  // three style runs, some in RGB, which is about what a MUD sends.
  POSITION posCurrent = s.doc.m_LineList.GetTailPosition ();
  for (size_t i = 0; i < lines; i++)
    {
    const std::string text = RealisticLine (seed, (int) i);
    CLine * p = new CLine (++s.doc.m_total_lines, CTime (time (NULL)), 0, 0, true, (int) text.size (), 0,
                           text.data (), (int) text.size ());
    int at = 0;
    const int runs = 1 + (int) (i % 3);
    for (int r = 0; r < runs; r++)
      {
      CStyle * style = new CStyle;
      style->iLength = (unsigned short) (r == runs - 1 ? (int) text.size () - at : (int) text.size () / runs);
      style->iFlags = r == 1 ? COLOUR_RGB : COLOUR_ANSI;
      style->iForeColour = r == 1 ? RGB (200, 100, (i & 255)) : (COLORREF) (i % 8);
      style->iBackColour = 0;
      at += style->iLength;
      p->styleList.AddTail (style);
      }
    s.doc.m_LineList.InsertBefore (posCurrent, p);
    }
  // m_pCurrentLine has to stay the tail, and its number the last.
  s.doc.m_pCurrentLine->m_nLineNumber = ++s.doc.m_total_lines;
  s.doc.m_LinePositions.BeginRebuild ();
  long n = 0;
  for (POSITION pos = s.doc.m_LineList.GetHeadPosition (); pos; n++)
    {
    if (n % JUMP_SIZE == 0)
      s.doc.m_LinePositions.Append (pos);
    s.doc.m_LineList.GetNext (pos);
    }
  fill = seconds (start);

  // What the UI thread pays, tick by tick.
  start = Clock::now ();
  for (size_t done = 0; done < lines; done += 40000)
    {
    const auto tick = Clock::now ();
    s.doc.FlushSavedScrollback (false);
    worstTick = (std::max) (worstTick, seconds (tick));
    ticks++;
    }
  encode = seconds (start);

  // And the worker catching up, with all of it compressed and written.
  start = Clock::now ();
  s.doc.FlushSavedScrollback (true);
  wait = seconds (start);
  }

  const size_t size = ReadAll (path).size ();

  // Reading it again from a warm cache.
  LoadResult r = Load (path, false, 0);
  const double storeRead = r.readSeconds;
  const double storeDecode = r.decodeSeconds;
  r = LoadResult ();

  size_t restored = 0;
  {
  const auto start = Clock::now ();
  SessionFixture s (id, 0);
  load = seconds (start);
  restored = (size_t) s.Count ();
  s.End ();
  }

  printf ("  %lu lines: file %.1f MB, %.0f bytes/line; UI thread %.3f s in %ld ticks, worst %.1f ms; "
          "then waiting for the worker %.3f s; restored %lu in %.3f s (of which read %.3f s, inflate+parse %.3f s); "
          "filling the test's buffer %.3f s\n",
          (unsigned long) lines, size / 1048576.0, (double) size / lines, encode, ticks, worstTick * 1000, wait,
          (unsigned long) restored, load, storeRead, storeDecode, fill);
  fflush (stdout);
  remove (path.c_str ());
  }

TEST (ScrollbackPerf, Timings)
  {
  if (!getenv ("MC_SCROLLBACK_PERF"))
    return;
  MeasureScrollback (5000);
  MeasureScrollback (100000);
  MeasureScrollback (1000000);
  }

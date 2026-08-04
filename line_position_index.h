// line_position_index.h — the output buffer's jump index.
//
// m_LineList is a doubly-linked list, so finding line N means walking N nodes.
// To keep GetLinePosition cheap the document remembers the POSITION of every
// JUMP_SIZE'th line ("chunk 0" = line 0, "chunk 1" = line JUMP_SIZE, …) and
// starts the walk from the nearest chunk head.
//
// This used to be a raw `POSITION *` array allocated as
// `new POSITION[(m_maxlines / JUMP_SIZE) + 1]` — sized from the buffer CAP but
// indexed by the live line COUNT.  Nothing bounds-checked the write in
// StartNewLine (`m_pLinePositions[GetCount() / JUMP_SIZE] = pos`); the only
// thing that kept it in range was the eviction in RemoveChunk firing at exactly
// m_maxlines.  Raising or removing the cap would therefore have written one
// POSITION past the end of a heap block on the first line past the old
// boundary — which is why unbounded scrollback could not simply "turn off the
// trim".  Two other costs came with the array: RemoveChunk shuffled every slot
// down by one (O(cap / JUMP_SIZE) per eviction, 5,000 pointer copies at a
// 500,000-line cap), and the whole array was allocated up front from the cap
// whether or not the buffer ever filled.
//
// A deque indexed relative to the current head fixes all three: growth is
// demand-driven, an eviction is one pop_front, and there is no cap in the
// data structure at all.  Chunk numbering is always relative to the head of
// m_LineList, so dropping the first chunk renumbers everything down by one for
// free — exactly what the old shuffle did by hand.
//
// Semantics deliberately preserved from the array version:
//   * a slot may be NULL (DeleteLines and the omit-from-output path in
//     ProcessPreviousLine invalidate the chunk head they removed), and
//     GetLinePosition falls back to the previous chunk and then to the list
//     head, so a NULL is a hint miss and never a wrong answer;
//   * "allocated" is a distinct state from "empty".  The document defers
//     allocation until SetUpOutputWindow, because m_maxlines is not known until
//     the world file is read, and several call sites test the index for
//     allocation as their "is this document usable yet" guard.

#pragma once

#include <deque>

// A JUMP_SIZE-granular index from chunk number to the POSITION of that chunk's
// first line.  Chunk c holds line c * JUMP_SIZE, counting from the current head.
class CLinePositionIndex
  {
  public:

  // Has the document allocated its index yet?  Several callers use this as
  // "was SetUpOutputWindow run" — DisplayMsg, ClearOutput and the Note family
  // all return early when it is false.
  bool IsAllocated () const { return m_bAllocated; }

  // Allocate (or re-seed) with a single chunk pointing at the list head.
  void Reset (POSITION head)
    {
    m_chunks.clear ();
    m_chunks.push_back (head);
    m_bAllocated = true;
    }

  // Back to the un-allocated state (document teardown, and the tests that
  // exercise the "not set up yet" guards).
  void Clear ()
    {
    m_chunks.clear ();
    m_bAllocated = false;
    }

  // Start a rebuild: allocated, but with no chunks recorded yet.  Follow with
  // Append for each chunk head in order.
  void BeginRebuild ()
    {
    m_chunks.clear ();
    m_bAllocated = true;
    }

  // POSITION of chunk c's first line, or NULL when that chunk has no recorded
  // head (never set, invalidated, or beyond the end).
  POSITION Get (long c) const
    {
    if (c < 0 || (size_t) c >= m_chunks.size ())
      return NULL;
    return m_chunks [(size_t) c];
    }

  // Record (or invalidate, with pos == NULL) chunk c's head.
  //
  // c == size() is the ordinary append as the buffer grows.  A NULL append is
  // not recorded at all — an unset trailing chunk and an absent one are the
  // same thing to Get, and not recording it keeps the deque tight.
  void Set (long c, POSITION pos)
    {
    if (c < 0)
      return;
    if ((size_t) c < m_chunks.size ())
      {
      m_chunks [(size_t) c] = pos;
      // Drop invalidated chunks off the tail so the deque does not accumulate
      // NULLs as DeleteLines walks the buffer back.
      while (!m_chunks.empty () && m_chunks.back () == NULL)
        m_chunks.pop_back ();
      return;
      }
    if (pos == NULL)
      return;                       // nothing to record past the end
    // Gaps should not arise (chunks are appended in order), but a resize keeps
    // Get's contract if one ever did.
    while ((size_t) c > m_chunks.size ())
      m_chunks.push_back (NULL);
    m_chunks.push_back (pos);
    }

  // Append the next chunk head.  Equivalent to Set(ChunkCount(), pos).
  void Append (POSITION pos) { m_chunks.push_back (pos); }

  // RemoveChunk evicted the first JUMP_SIZE lines: chunk 1 becomes chunk 0 and
  // so on.  O(1), where the array version copied every remaining slot.
  //
  // The index stays ALLOCATED even when that empties it — allocation is a
  // document lifecycle state, not a statement about content, and the callers
  // that test IsAllocated are asking "is this document set up", which is still
  // yes.  A NULL chunk 0 simply sends GetLinePosition to the list head.
  void DropFirstChunk ()
    {
    if (m_chunks.empty ())
      return;
    m_chunks.pop_front ();
    if (m_chunks.empty ())
      m_chunks.push_back (NULL);
    }

  // How many chunks are currently recorded (for tests and diagnostics).
  long ChunkCount () const { return (long) m_chunks.size (); }

  private:

  std::deque<POSITION> m_chunks;
  bool                 m_bAllocated = false;
  };

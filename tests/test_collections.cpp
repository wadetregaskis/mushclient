// tests/test_collections.cpp — the MFC collection shims.
//
// CTypedPtrList is the backing store for CMUSHclientDoc::m_LineList (the entire
// scrollback), the style list on every line, the plugin list and the MXP tag
// stacks.  It used to wrap std::list plus a side vector of Node{iterator}
// objects and map an iterator back to its Node by LINEAR SEARCH, so GetNext()
// was O(N) and a full walk O(N^2); RemoveHead() was O(N) too.  The display
// drain walks the line list on every received packet, so that was quadratic in
// the scrollback size on the hottest path in the app.
//
// These tests pin BOTH halves: the MFC semantics (so the rewrite is faithful)
// and the complexity (so it cannot silently regress to a scan again).

#include "mc_test.h"
#include "platform/windows_types.h"   // UINT/BOOL, used by the array/map shims below
#include "platform/mfc_collections.h"

#include <chrono>
#include <vector>

typedef CTypedPtrList<CPtrList, int*> IntList;

namespace {

// Distinct heap cells so pointer identity is meaningful, freed en masse.
struct Cells {
    std::vector<int*> owned;
    int* make(int v) { int* p = new int(v); owned.push_back(p); return p; }
    ~Cells() { for (int* p : owned) delete p; }
};

// Wall-clock milliseconds for a full head-to-tail walk of `n` elements.
double walk_ms(int n)
{
    Cells c;
    IntList list;
    for (int i = 0; i < n; ++i) list.AddTail(c.make(i));

    auto t0 = std::chrono::steady_clock::now();
    long sum = 0;
    for (POSITION pos = list.GetHeadPosition(); pos; )
        sum += *list.GetNext(pos);
    auto t1 = std::chrono::steady_clock::now();

    // Consume the result so the walk cannot be optimised away.
    EXPECT_TRUE(sum == (long) n * (n - 1) / 2);
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

} // namespace

// ── MFC semantics ─────────────────────────────────────────────────────────

TEST(PtrList, AddTailAndWalkPreservesOrder)
{
    Cells c; IntList list;
    for (int i = 0; i < 5; ++i) list.AddTail(c.make(i));

    EXPECT_EQ(5, list.GetCount());
    int expect = 0;
    for (POSITION pos = list.GetHeadPosition(); pos; )
        EXPECT_EQ(expect++, *list.GetNext(pos));
    EXPECT_EQ(5, expect);
}

TEST(PtrList, AddHeadPrepends)
{
    Cells c; IntList list;
    list.AddTail(c.make(2));
    list.AddHead(c.make(1));
    list.AddHead(c.make(0));

    EXPECT_EQ(3, list.GetCount());
    EXPECT_EQ(0, *list.GetHead());
    EXPECT_EQ(2, *list.GetTail());
}

TEST(PtrList, GetPrevWalksBackwards)
{
    Cells c; IntList list;
    for (int i = 0; i < 4; ++i) list.AddTail(c.make(i));

    int expect = 3;
    for (POSITION pos = list.GetTailPosition(); pos; )
        EXPECT_EQ(expect--, *list.GetPrev(pos));
    EXPECT_EQ(-1, expect);
}

TEST(PtrList, PositionStaysValidWhenOtherElementsAreRemoved)
{
    // The MFC contract this whole design exists for: a POSITION is a stable
    // handle to ITS element, unaffected by removals elsewhere.  m_pLinePositions
    // (the doc's every-100th-line jump index) depends on exactly this.
    Cells c; IntList list;
    POSITION marks[6];
    for (int i = 0; i < 6; ++i) marks[i] = list.AddTail(c.make(i));

    list.RemoveAt(marks[1]);
    list.RemoveHead();          // removes element 0
    list.RemoveTail();          // removes element 5

    EXPECT_EQ(3, list.GetCount());
    EXPECT_EQ(2, *list.GetAt(marks[2]));   // untouched handles still resolve
    EXPECT_EQ(3, *list.GetAt(marks[3]));
    EXPECT_EQ(4, *list.GetAt(marks[4]));
    EXPECT_EQ(2, *list.GetHead());
    EXPECT_EQ(4, *list.GetTail());
}

TEST(PtrList, RemoveHeadReturnsTheElementAndRelinksTheHead)
{
    Cells c; IntList list;
    for (int i = 0; i < 3; ++i) list.AddTail(c.make(i));

    EXPECT_EQ(0, *list.RemoveHead());
    EXPECT_EQ(1, *list.GetHead());
    EXPECT_EQ(2, list.GetCount());
    EXPECT_EQ(2, *list.RemoveTail());
    EXPECT_EQ(1, *list.GetTail());
    EXPECT_EQ(1, list.GetCount());
}

TEST(PtrList, InsertBeforeAndAfterSplice)
{
    Cells c; IntList list;
    POSITION a = list.AddTail(c.make(10));
    POSITION b = list.AddTail(c.make(30));
    list.InsertBefore(b, c.make(20));
    list.InsertAfter(b, c.make(40));
    list.InsertBefore(a, c.make(5));

    int expect[] = { 5, 10, 20, 30, 40 };
    int i = 0;
    for (POSITION pos = list.GetHeadPosition(); pos; ++i)
        EXPECT_EQ(expect[i], *list.GetNext(pos));
    EXPECT_EQ(5, i);
    EXPECT_EQ(5, *list.GetHead());
    EXPECT_EQ(40, *list.GetTail());
}

TEST(PtrList, SetAtReplacesThroughTheHandle)
{
    Cells c; IntList list;
    POSITION p = list.AddTail(c.make(1));
    list.SetAt(p, c.make(99));
    EXPECT_EQ(99, *list.GetAt(p));
    EXPECT_EQ(99, *list.GetHead());
}

TEST(PtrList, FindLocatesByValueAndReportsMisses)
{
    Cells c; IntList list;
    int* target = c.make(7);
    list.AddTail(c.make(1));
    POSITION want = list.AddTail(target);
    list.AddTail(c.make(9));

    EXPECT_TRUE(list.Find(target) == want);
    EXPECT_TRUE(list.Find(c.make(1234)) == nullptr);
}

TEST(PtrList, EmptyListWalksZeroTimesAndReportsEmpty)
{
    IntList list;
    EXPECT_TRUE(list.IsEmpty());
    EXPECT_EQ(0, list.GetCount());
    EXPECT_TRUE(list.GetHeadPosition() == nullptr);
    EXPECT_TRUE(list.GetTailPosition() == nullptr);
}

TEST(PtrList, RemovingTheOnlyElementLeavesBothEndsNull)
{
    // Head and tail are separate pointers now; the single-element case is the
    // one that catches a missed relink.
    Cells c; IntList list;
    list.AddTail(c.make(1));
    list.RemoveHead();
    EXPECT_TRUE(list.IsEmpty());
    EXPECT_TRUE(list.GetHeadPosition() == nullptr);
    EXPECT_TRUE(list.GetTailPosition() == nullptr);

    list.AddTail(c.make(2));      // and the list is still usable afterwards
    EXPECT_EQ(1, list.GetCount());
    EXPECT_EQ(2, *list.GetHead());
    EXPECT_EQ(2, *list.GetTail());
}

TEST(PtrList, RangeForVisitsEveryValueInOrder)
{
    Cells c; IntList list;
    for (int i = 0; i < 4; ++i) list.AddTail(c.make(i));

    int expect = 0;
    for (int* v : list) EXPECT_EQ(expect++, *v);
    EXPECT_EQ(4, expect);
}

TEST(PtrList, EraseReturnsTheFollowingIterator)
{
    Cells c; IntList list;
    for (int i = 0; i < 4; ++i) list.AddTail(c.make(i));

    IntList::iterator it = list.begin();
    ++it;                              // at element 1
    it = list.erase(it);               // now at element 2
    EXPECT_EQ(2, **it);
    EXPECT_EQ(3, list.GetCount());
}

TEST(PtrList, MoveTransfersOwnershipAndKeepsPositionsValid)
{
    Cells c;
    POSITION mark;
    IntList dest;
    {
        IntList src;
        src.AddTail(c.make(1));
        mark = src.AddTail(c.make(2));
        src.AddTail(c.make(3));
        dest = std::move(src);
        EXPECT_EQ(0, src.GetCount());          // source emptied
        EXPECT_TRUE(src.GetHeadPosition() == nullptr);
    }
    // Nodes are heap-allocated, so handles taken before the move still resolve.
    EXPECT_EQ(3, dest.GetCount());
    EXPECT_EQ(2, *dest.GetAt(mark));
    EXPECT_EQ(1, *dest.GetHead());
    EXPECT_EQ(3, *dest.GetTail());
}

TEST(PtrList, AddTailOfAnotherListAppendsItsElements)
{
    Cells c; IntList a, b;
    a.AddTail(c.make(1));
    b.AddTail(c.make(2));
    b.AddTail(c.make(3));
    a.AddTail(&b);

    EXPECT_EQ(3, a.GetCount());
    EXPECT_EQ(2, b.GetCount());        // source is not consumed
    EXPECT_EQ(3, *a.GetTail());
}

// ── complexity ────────────────────────────────────────────────────────────

TEST(PtrList, FullWalkIsLinearNotQuadratic)
{
    // The regression this guards: mapping an iterator back to its POSITION by
    // scanning a side table, which makes GetNext O(N) and the walk O(N^2).
    //
    // Compare 4x the elements against a generous 8x time budget (plus a floor,
    // so timer noise on a tiny baseline cannot fail it).  Linear would be ~4x;
    // quadratic would be ~16x.  At N=16000 the old implementation took on the
    // order of a second here, so the margin is enormous either way.
    walk_ms(1000);                                  // warm caches/allocator
    const double small = walk_ms(4000);
    const double large = walk_ms(16000);

    const double budget = std::max(small * 8.0, 5.0);
    EXPECT_TRUE(large < budget);
    if (large >= budget)
        printf("    walk(4000)=%.2fms walk(16000)=%.2fms budget=%.2fms\n",
               small, large, budget);
}

TEST(PtrList, HeadTrimIsLinearNotQuadratic)
{
    // RemoveChunk() trims 100 lines off the head every time the buffer fills.
    // The old erase() scanned the side table for each removal.
    Cells c;
    IntList list;
    const int N = 20000;
    for (int i = 0; i < N; ++i) list.AddTail(c.make(i));

    auto t0 = std::chrono::steady_clock::now();
    while (!list.IsEmpty()) list.RemoveHead();
    auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    EXPECT_EQ(0, list.GetCount());
    // 20k O(1) unlinks is microseconds; 20k O(N) scans is ~seconds.
    EXPECT_TRUE(ms < 250.0);
    if (ms >= 250.0) printf("    trim of %d took %.2fms\n", N, ms);
}

MC_TEST_MAIN()

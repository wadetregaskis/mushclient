// pal/scroll_geometry.h — "is the output view scrolled to the bottom?"
//
// Header-only pure arithmetic, so the headless tests can exercise it without
// linking a view.  One definition for every caller: the output pane asks this
// after each user scroll to decide whether to stay pinned to the newest line.
//
// WHY IT IS NOT WRITTEN INLINE AT THE CALL SITES ANY MORE
//
// The wx pane had four copies of an ad-hoc test that reconstructed the document
// height from the line count:
//
//     atBottom = scrollY * unitH + viewH >= m_totalVisualRows * m_lineH - m_lineH
//
// and that total is NOT the scroll range.  UpdateScrollbars sets the virtual
// height to the committed rows PLUS a pending-line row, PLUS the reserved
// text-rectangle band, PLUS the world's pixel offset rounded up to a whole row.
// So at the true bottom the left-hand side overshot by up to a row, and the
// "- m_lineH" existed to stop the test failing there.  The cost of that fudge
// was a dead zone one row deep: scrolling up a single row still counted as "at
// the bottom", so auto-scroll stayed engaged and the very next line of output
// yanked the view back down.  Scroll gently and the pane fought you; scroll
// harder — two rows or more — and it let go.  (Reported as the output view
// "resisting" the scroll wheel and snapping back.)
//
// Asking the scrollbar instead cannot drift from it.  This is also exactly the
// comparison Windows makes, in pixels rather than rows:
//
//     mushview.cpp, CMUSHView::OnUpdateStatuslineFreeze_helper
//     m_bAtBufferEnd = m_ScrollbarPosition.cy >=
//                      (m_ScrollbarSizeTotal.cy - m_ScrollbarSizePage.cy);
//
// i.e. "the scroll position is at its maximum", with no tolerance.

#pragma once

namespace pal {

// The largest scroll position (in whole scroll units) that the view can reach.
// Rounds UP, because the last unit is partial whenever the virtual height is
// not a multiple of the unit height — and that partial unit is a real, reachable
// position, so ignoring it would leave the pane one unit short of the bottom
// forever.
inline int maxScrollUnit(int unitHeight, int clientHeight, int virtualHeight)
{
    if (unitHeight <= 0) return 0;
    const int hidden = virtualHeight - clientHeight;
    if (hidden <= 0) return 0;                  // everything fits: no scrolling
    return (hidden + unitHeight - 1) / unitHeight;
}

// True when `scrollUnit` is the bottom-most position, i.e. the newest output is
// on screen.  Everything smaller is the user having deliberately scrolled back,
// however slightly — one unit is enough, which is the whole point.
//
// A view with nothing to scroll (or a not-yet-measured one, unitHeight <= 0) is
// "at the bottom": there is no earlier content to be looking at, and answering
// false would leave a fresh window frozen before its first line arrives.
inline bool isScrolledToBottom(int scrollUnit, int unitHeight,
                               int clientHeight, int virtualHeight)
{
    if (unitHeight <= 0) return true;
    return scrollUnit >= maxScrollUnit(unitHeight, clientHeight, virtualHeight);
}

// ── 64-bit row -> pixel geometry ──────────────────────────────────────────
//
// Row counts times a row height is the one product in the pane that can
// overflow: at a 15-pixel row, `rows * m_lineH` in `int` wraps NEGATIVE past
// 143,165,576 rows, and every consumer (the virtual size, hit-testing, the
// find-scroll) collapses at once.  That ceiling was unreachable while the
// scrollback was capped at 500,000 lines; with max_output_lines = 0 it is not,
// so the products are computed in 64 bits and saturated where a 32-bit device
// coordinate is genuinely required.
//
// The saturation is not a fix for the ceiling — wxScrolledWindow's virtual size
// is an int, so pixel-addressed scrolling cannot go past INT_MAX however the
// arithmetic is done.  It is there so that reaching it degrades into "the
// scrollbar stops growing" instead of undefined behaviour and a negative
// height.  Lifting the ceiling entirely means scrolling by ROW rather than by
// pixel, which is a much larger change (docs/AUDIT-2026-08.md, step 8).

// rows * rowHeight, in 64 bits.  Negative or zero inputs give 0.
inline long long rowsToPixels(long long rows, int rowHeight)
{
    if (rows <= 0 || rowHeight <= 0) return 0;
    return rows * (long long) rowHeight;
}

// Saturate a 64-bit pixel extent into the int a wx device coordinate wants.
inline int clampToDeviceExtent(long long pixels)
{
    const long long kMax = 2147483647LL;         // INT_MAX
    if (pixels <= 0)    return 0;
    if (pixels >= kMax) return (int) kMax;
    return (int) pixels;
}

// A 64-bit pixel position expressed in whole scroll units, saturated.  Used
// wherever a computed pixel offset is handed to wxScrolledWindow::Scroll.
inline int pixelsToScrollUnits(long long pixels, int unitHeight)
{
    if (unitHeight <= 0 || pixels <= 0) return 0;
    return clampToDeviceExtent(pixels / (long long) unitHeight);
}

} // namespace pal

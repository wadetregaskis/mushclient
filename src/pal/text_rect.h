// pal/text_rect.h — world.TextRectangle → client pixels.
//
// Header-only pure arithmetic, so the headless tests can exercise it without
// linking a view, and so the TWO consumers on the wx side cannot drift:
//
//   COutputPane::TextRect        the drawing clip and the VISUAL wrap width
//   EffectiveTextCols            the ENGINE wrap column (m_nWrapColumn) + NAWS
//
// Both used to re-derive the rectangle from CMUSHclientDoc::m_TextRectangle
// with their own formula, and both dropped the documented offset-from-edge
// form: world.TextRectangle(0, 0, -200, 0) means "200 pixels in from the right
// edge", not "no right edge given".  The renderer then wrapped text at the full
// client width and drew it underneath the docked miniwindow the plugin had just
// reserved space for, and NAWS advertised the wrong width to the MUD.
//
// The canonical resolution is CMUSHView::GetTextRectangle (mushview.cpp):
//
//     if (TextRectangle.right <= 0)
//       { TextRectangle.right += r.right;
//         TextRectangle.right = max (TextRectangle.right, TextRectangle.left + 20); }
//     if (TextRectangle.bottom <= 0)
//       { TextRectangle.bottom += r.bottom;
//         TextRectangle.bottom = max (TextRectangle.bottom, TextRectangle.top + 20); }
//
// where r is the view's client rect.  Left/top are taken as given (they are
// already offsets from the left/top edge, so there is no second form to
// resolve), and the 20-pixel floor keeps a bad offset from inverting the rect.

#pragma once

namespace pal {

// True when the doc's rectangle is the all-zero "no rectangle" sentinel, i.e.
// the text may use the whole client area.  Mirrors CMUSHView::HaveTextRectangle
// (`pDoc->m_TextRectangle != CRect (0, 0, 0, 0)`).
inline bool textRectIsUnset(int left, int top, int right, int bottom)
{
    return left == 0 && top == 0 && right == 0 && bottom == 0;
}

// One axis of the resolution: `far` is right (with near = left, extent =
// client width) or bottom (near = top, extent = client height).
inline int resolveTextRectEdge(int nearEdge, int farEdge, int extent)
{
    if (farEdge > 0) return farEdge;             // already an absolute position
    farEdge += extent;                           // offset from the far side
    if (farEdge < nearEdge + 20)                 // ensure not negative
        farEdge = nearEdge + 20;
    return farEdge;
}

// Resolve the offset-from-edge form in place, given the client extent.
// Call ONLY when textRectIsUnset() is false — Windows resolves nothing for the
// all-zero rectangle, it just returns the client rect.
inline void resolveTextRect(int& left, int& top, int& right, int& bottom,
                            int clientW, int clientH)
{
    right  = resolveTextRectEdge(left, right,  clientW);
    bottom = resolveTextRectEdge(top,  bottom, clientH);
}

// The width the text is allowed to occupy, in client pixels: the resolved
// rectangle's width when one is set and it is narrower than the pane, else the
// full pane width.  This is GetOutputWindowWidth()'s answer — the term Windows
// feeds to its single wrap formula (mushview.cpp:3220,
// `(GetOutputWindowWidth () - pDoc->m_iPixelOffset) / charW`).
inline int textRectWidth(int left, int top, int right, int bottom, int paneW)
{
    if (textRectIsUnset(left, top, right, bottom)) return paneW;
    const int w = resolveTextRectEdge(left, right, paneW) - left;
    return (w > 0 && w < paneW) ? w : paneW;
}

// How many characters fit across that width once the world's pixel offset
// (World Properties ▸ Appearance ▸ Output ▸ "Pixel offset", m_iPixelOffset) has
// inset the text from the left edge.  Windows has exactly ONE wrap formula,
// mushview.cpp:3220:
//
//     (GetOutputWindowWidth () - pDoc->m_iPixelOffset) / charW
//
// and the port needs it in two places — the VISUAL wrap
// (COutputPane::RebuildWrapCache) and the ENGINE wrap column + NAWS
// (EffectiveTextCols in wx_world_child.cpp).  If the visual one ever came out
// narrower, every full-width engine line would spill a one- or two-character
// orphan continuation row, so they are the same call, not two copies.
inline int textWrapColumns(int textWidthPx, int pixelOffset, int charWidthPx)
{
    if (charWidthPx <= 0) return 1;
    const int usable = textWidthPx - pixelOffset;
    const int cols   = usable / charWidthPx;
    return cols > 1 ? cols : 1;
}

} // namespace pal

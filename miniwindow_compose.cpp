// miniwindow_compose.cpp : miniwindow placement — engine-owned, shared by all
// frontends (docs/PAL.md S4, "the composition contract").
//
// This is CMUSHView::Calculate_MiniWindow_Rectangles moved VERBATIM out of
// mushview.cpp so the Windows view and the ports compute placement with the
// SAME code (no twin to drift): the view supplies its geometry (its client
// rect and the owning frame's client rect); the engine writes back each
// miniwindow's m_rect and m_bTemporarilyHide, which WindowInfo and hotspot
// hit-testing then read. Runs per paint pass (bUnderneath true, then false)
// with pass-scoped corner tracking, exactly as the Windows view always has.

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"

void CalculateMiniWindowRectangles (CMUSHclientDoc* pDoc, const bool bUnderneath,
                                    const RECT & clientrect, const RECT & ownerrect)
  {

  MiniWindowVectorIterator mwit;

  // record here how far out each corner extends
  CPoint pt_topleft (0, 0),
         pt_topright (clientrect.right, 0),
         pt_bottomleft (0,clientrect.bottom),
         pt_bottomright (clientrect.right, clientrect.bottom);


  // total amount taken by the centered items along each wall
  long  top_widths = 0,
        right_widths = 0,
        bottom_widths = 0,
        left_widths = 0;

  // remember each one in case there are too many
  list<CMiniWindow *> top_ones,
        right_ones,
        bottom_ones,
        left_ones;

  // do the corners, and fixed positions, and remember how much the centered ones wanted
  for (mwit = pDoc->m_MiniWindowsOrder.begin ();
       mwit != pDoc->m_MiniWindowsOrder.end ();
       mwit++)
   {
   CMiniWindow * mw = mwit->second;

   mw->m_bTemporarilyHide = false;


   // not if not to be shown
   if (!mw->GetShow ())
     continue;

   // not if wrong time
   if (((mw->GetFlags () & MINIWINDOW_DRAW_UNDERNEATH) != 0) != bUnderneath)
     continue;

   long iWidth = mw->GetWidth (),
         iHeight = mw->GetHeight ();

   // if absolute location wanted, just take that and press on
   if (mw->GetFlags () & MINIWINDOW_ABSOLUTE_LOCATION)
      {
      CPoint loc          = mw->GetLocation ();
      mw->m_rect.left     = loc.x;
      mw->m_rect.top      = loc.y;
      mw->m_rect.right    = mw->m_rect.left + iWidth;
      mw->m_rect.bottom   = mw->m_rect.top + iHeight;
      continue;
      }

    switch (mw->GetPosition ())
      {
      case 0 :  // stretch to output window
         mw->m_rect = CRect (0, 0, clientrect.right, clientrect.bottom);
         continue;

      case 1:  // stretch with aspect ratio
        {
        double ratio = ((double) iWidth) / ((double) iHeight);
        mw->m_rect = CRect (0, 0, clientrect.bottom * ratio, clientrect.bottom);
        continue;
        }

      case 2 :  // stretch to frame
         mw->m_rect = CRect (0, 0, ownerrect.right, ownerrect.bottom);
         continue;

      case 3:    // stretch to frame with aspect ratio
        {
        double ratio = ((double) iWidth) / ((double) iHeight);
        mw->m_rect = CRect (0, 0, ownerrect.bottom * ratio, ownerrect.bottom);
        continue;
        }

      case 4: // top left
         mw->m_rect = CRect (0, 0, iWidth, iHeight);
         pt_topleft.x = max (pt_topleft.x, mw->m_rect.right);     // shrinks available
         pt_topleft.y = max (pt_topleft.y, mw->m_rect.bottom);
         break;

      case 5 :  // center left-right at top
        top_widths += iWidth;
        top_ones.push_back (mw);
        break;

      case 6 : // top right
         mw->m_rect = CRect (clientrect.right - iWidth, 0, iWidth, iHeight);
         pt_topright.x = min (pt_topright.x, mw->m_rect.left);     // shrinks available
         pt_topright.y = max (pt_topright.y, mw->m_rect.bottom);
         break;

      case 7 :  // on right, center top-bottom
         right_widths += iHeight;
         right_ones.push_back (mw);
         break;

      case 8 :  // on right at bottom
         mw->m_rect = CRect (clientrect.right - iWidth, clientrect.bottom - iHeight, iWidth, iHeight);
         pt_bottomright.x = min (pt_bottomright.x, mw->m_rect.left);     // shrinks available
         pt_bottomright.y = min (pt_bottomright.y, mw->m_rect.top);
         break;

      case 9 :  // center left-right at bottom
          bottom_widths += iWidth;
          bottom_ones.push_back (mw);
          break;

      case 10 :  // on left, at bottom
         mw->m_rect = CRect (0, clientrect.bottom - iHeight, iWidth, iHeight);
         pt_bottomleft.x = max (pt_bottomleft.x, mw->m_rect.right);     // shrinks available
         pt_bottomleft.y = min (pt_bottomleft.y, mw->m_rect.top);
         break;

      case 11 :  // on left, center top-bottom
          left_widths += iHeight;
          left_ones.push_back (mw);
          break;

      case 12:  // center all
         mw->m_rect = CRect ((clientrect.right - iWidth) / 2, (clientrect.bottom - iHeight) / 2, iWidth, iHeight);
         continue;


      case 13:
          continue;  // tile - ignore

      } // end switch


   }  // end for each window


  // see the size of the gaps on each side
  long top_room = pt_topright.x - pt_topleft.x;
  long right_room = pt_bottomright.y - pt_topright.y;
  long bottom_room = pt_bottomright.x - pt_bottomleft.x;
  long left_room = pt_bottomleft.y - pt_topleft.y;


  // throw away ones that won't fit (may be left with nothing)

  // top
  while (!top_ones.empty () && (top_widths > top_room))
    {
    CMiniWindow * mw = top_ones.back ();
    top_ones.pop_back ();
    top_widths -= mw->GetWidth ();
    mw->m_bTemporarilyHide = true;
    }

  // right
  while (!right_ones.empty () && (right_widths > right_room))
    {
    CMiniWindow * mw = right_ones.back ();
    right_ones.pop_back ();
    right_widths -= mw->GetHeight ();
    mw->m_bTemporarilyHide = true;
    }

  // bottom
  while (!bottom_ones.empty () && (bottom_widths > bottom_room))
    {
    CMiniWindow * mw = bottom_ones.back ();
    bottom_ones.pop_back ();
    bottom_widths -= mw->GetWidth ();
    mw->m_bTemporarilyHide = true;
    }

  // left
  while (!left_ones.empty () && (left_widths > left_room))
    {
    CMiniWindow * mw = left_ones.back ();
    left_ones.pop_back ();
    left_widths -= mw->GetHeight ();
    mw->m_bTemporarilyHide = true;
    }


  // allocate positions
  // in each case the number of gaps will be the number of items + 1
  // (eg. for a single centered item, there is a gap at each side)
  list<CMiniWindow *>::iterator lit;
  long gap;
  long start;


  // along top wall
  gap = (top_room - top_widths) / (top_ones.size () + 1);
  start = pt_topleft.x + gap;

  for (lit = top_ones.begin (); lit != top_ones.end (); lit++)
    {
    CMiniWindow * mw = *lit;
    mw->m_rect = CRect (start, 0, mw->GetWidth (), mw->GetHeight ());
    start += mw->GetWidth () + gap;
    }

  // along right wall
  gap = (right_room - right_widths) / (right_ones.size () + 1);
  start = pt_topright.y + gap;

  for (lit = right_ones.begin (); lit != right_ones.end (); lit++)
    {
    CMiniWindow * mw = *lit;
    mw->m_rect = CRect (clientrect.right - mw->GetWidth (), start, mw->GetWidth (), mw->GetHeight ());
    start += mw->GetHeight () + gap;
    }

  // along bottom wall
  gap = (bottom_room - bottom_widths) / (bottom_ones.size () + 1);
  start = pt_bottomleft.x + gap;

  for (lit = bottom_ones.begin (); lit != bottom_ones.end (); lit++)
    {
    CMiniWindow * mw = *lit;
    mw->m_rect = CRect (start, clientrect.bottom - mw->GetHeight (), mw->GetWidth (), mw->GetHeight ());
    start += mw->GetWidth () + gap;
    }

  // along left wall
  gap = (left_room - left_widths) / (left_ones.size () + 1);
  start = pt_topleft.y + gap;

  for (lit = left_ones.begin (); lit != left_ones.end (); lit++)
    {
    CMiniWindow * mw = *lit;
    mw->m_rect = CRect (0, start, mw->GetWidth (), mw->GetHeight ());
    start += mw->GetHeight () + gap;
    }

  } // end of CalculateMiniWindowRectangles

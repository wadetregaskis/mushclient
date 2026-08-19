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
#include "MUSHview.h"   // MW_MOUSE_* flag constants

// Which miniwindow (and hotspot, if any) is under a client-space point —
// the old CMUSHView::Mouse_Over_Miniwindow, moved verbatim except for the
// window bounds test. The original PtInRect'ed m_rect, whose right/bottom
// hold WIDTH/HEIGHT after a paint (see Blit_Bitmap's mutation) — a
// non-normalized rect PtInRect can never match, so hotspots silently died
// for any window positioned beyond its own size. The explicit
// left/top + size test below fixes that, and for every window the layout
// pass DREW it answers exactly what PtInRect answered.
//
// It is NOT the strict extension this comment used to claim, though: a
// window the pass could not fit keeps the constructor's (0,0,0,0) m_rect,
// which PtInRect matched nothing against but which the size test reads as
// the pane's top-left GetWidth() x GetHeight() corner — swallowing clicks
// meant for the text underneath and hit-testing an undrawn window's
// hotspots. The m_bTemporarilyHide skip below is the bound that keeps the
// widened test honest: if it is not drawn it cannot be clicked, which is
// the test both paint loops already apply.
//
// m_bTemporarilyHide alone was not enough, because it only covers a window
// the layout pass REJECTED. A shown window that has not been through a
// layout pass at all — WindowAddHotspot's immediate re-detect reaches here
// synchronously from the creating script, before any paint — has the flag
// false and the constructor's rect, i.e. the same corner. m_bLaidOut is the
// other half of the bound.
CMiniWindow * MouseOverMiniWindow (CMUSHclientDoc* pDoc,
                                   CPoint point,
                                   string & sHotspotId,
                                   CHotspot * &pHotspot,
                                   string & sMiniwindowId
                                   )
  {
  pHotspot = NULL;

  MiniWindowVectorReverseIterator mwit;

   // mini windows - check backwards so one on top is found first
  for (mwit = pDoc->m_MiniWindowsOrder.rbegin ();
       mwit != pDoc->m_MiniWindowsOrder.rend ();
       mwit++)  // NB you still go "up" to go backwards
         {
         CMiniWindow * mw = mwit->second;
         // not if not being shown, or if the layout pass could not fit it —
         // the same pair both paint loops test (mushview.cpp:1007 and :1507,
         // platform/wx_world_child.cpp:262) — or if no layout pass has run
         // for it yet, in which case m_rect is still the constructor's
         if (!mw->GetShow () || mw->m_bTemporarilyHide || !mw->m_bLaidOut)
           continue;

         // underneath ones can't be clicked on - or if set to ignore the mouse
         if (mw->GetFlags () & (MINIWINDOW_DRAW_UNDERNEATH | MINIWINDOW_IGNORE_MOUSE))
           continue;

         // a tiled window (position 13) is not laid out by the loop above —
         // its m_rect is the whole pane (Blit_Bitmap's `case 13: rect = r`),
         // not left/top + one tile.  Hit-test the whole rect, or clicks past
         // the first tile fall through to the text underneath.
         long iRight  = mw->GetPosition () == 13 ? mw->m_rect.right
                                                 : mw->m_rect.left + mw->GetWidth ();
         long iBottom = mw->GetPosition () == 13 ? mw->m_rect.bottom
                                                 : mw->m_rect.top + mw->GetHeight ();
         if (point.x < mw->m_rect.left || point.y < mw->m_rect.top ||
             point.x >= iRight || point.y >= iBottom)
           continue;  // not in window

         CPoint mw_relative_mouse (point);
         mw_relative_mouse = mw_relative_mouse - mw->m_rect.TopLeft ();
         for (HotspotMapIterator hit = mw->m_Hotspots.begin ();
              hit != mw->m_Hotspots.end ();
              hit++)
                {
                pHotspot = hit->second;

                if (!pHotspot->m_rect.PtInRect(mw_relative_mouse))
                  continue;  // not in hotspot

                sHotspotId = hit->first;
                sMiniwindowId = mwit->first;
                return mw;
                } // for each hotspot


         pHotspot = NULL;
         sMiniwindowId = mwit->first;
         return mw;
         }  // end for each window



  sMiniwindowId.erase ();
  return NULL;
  } // end of MouseOverMiniWindow


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

   // only the pass that lays this one out may clear its flag: the two passes
   // run back to back, so an unconditional reset here had the second pass
   // wipe the first pass's "would not fit" decision, and WindowInfo (6)
   // always answered false for a draw-underneath window that had no room
   if (((mw->GetFlags () & MINIWINDOW_DRAW_UNDERNEATH) != 0) == bUnderneath)
     mw->m_bTemporarilyHide = false;


   // not if not to be shown
   if (!mw->GetShow ())
     continue;

   // not if wrong time
   if (((mw->GetFlags () & MINIWINDOW_DRAW_UNDERNEATH) != 0) != bUnderneath)
     continue;

   // past the show and wrong-time skips, so this pass owns the window and
   // WILL place it (or pop it as "would not fit", which m_bTemporarilyHide
   // bounds): m_rect stops being the ctor's.  NOT set before the show skip —
   // a paint pass sweeping a hidden window would otherwise flag it laid out
   // while m_rect is still (0,0,0,0), and a later WindowShow + hotspot
   // re-detect would hit-test the pane's top-left corner.
   mw->m_bLaidOut = true;

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


// ── engine-owned mouse routing (docs/PAL.md S4) ─────────────────────────────
// The CMUSHView Mouse_* family, moved VERBATIM. Views forward their mouse
// events with client-space coordinates plus a MiniWindowMouseHooks giving the
// engine the view's tooltip / capture / keyboard services; the per-window
// mouse state (m_sPreviousMiniWindow) is doc-homed. Coordinate conversion
// (DPtoLP / ScreenToClient) stays view-side, exactly as before.

void SendMouseEventToPlugin (CMUSHclientDoc* pDoc, MiniWindowMouseHooks & hooks,
                             DISPID iDispatchID,
                             const string m_sPluginID,
                             const string sRoutineName,
                             const string HotspotId,
                             long Flags,
                             bool dont_modify_flags)
  {

  // only if they have a routine
  if (sRoutineName.empty ())
    return;

// also Flags might be (from caller):

// 0x10  - LH mouse
// 0x20  - RH mouse
// 0x40  - double-click
// 0x80  - mouse-over not first time 
// 0x100 - scroll wheel backwards

if (!dont_modify_flags)
  Flags |= hooks.KeyboardModifiers ();   // shift / ctrl / alt, from the view's toolkit


if (m_sPluginID.empty ())
  {
  if (iDispatchID == DISPID_UNKNOWN)
    {
    pDoc->Trace ("Mouse event (%s): Function %s not found.", HotspotId.c_str (), sRoutineName.c_str ());
    return;                       
    }


  pDoc->ExecuteHotspotScript (iDispatchID,  // dispatch ID, will be set to DISPID_UNKNOWN on an error
                              sRoutineName.c_str (),      // what name was in the hotspot callback
                              Flags,              // flags: ctrl, shift, whatever
                              HotspotId.c_str ()       // which hotspot
                              );

  return;


  } // end of no plugin

CPlugin * pPlugin = pDoc->GetPlugin (m_sPluginID.c_str ()); 

  if (!pPlugin) 
    {
    pDoc->Trace ("Mouse event (%s): No plugin ID: %s", HotspotId.c_str (), m_sPluginID.c_str ());
    return;                       
    }

  if (!pPlugin->m_bEnabled)
    {
    pDoc->Trace ("Mouse event (%s): Plugin: %s disabled.", HotspotId.c_str (), m_sPluginID.c_str ());
    return;                       
    }

DISPID iDispid = pPlugin->m_ScriptEngine->GetDispid (sRoutineName.c_str ());

  if (iDispid == DISPID_UNKNOWN)
    {
    pDoc->Trace ("Mouse event (%s): Function %s not found.", HotspotId.c_str (), sRoutineName.c_str ());
    return;                       
    }

  // change to this plugin, call function, put current plugin back
  CPlugin * pSavedPlugin = pDoc->m_CurrentPlugin;
  pDoc->m_CurrentPlugin = pPlugin;   
  CScriptDispatchID dispid_info (iDispid);
  CScriptCallInfo callinfo (sRoutineName.c_str (), dispid_info);
  pPlugin->ExecutePluginScript (callinfo, 
                                Flags,              // keyboard flags
                                HotspotId.c_str ());  // which hotspot
  pDoc->m_CurrentPlugin = pSavedPlugin;


  }  // end of SendMouseEventToPlugin


bool MouseMoveMiniWindow (CMUSHclientDoc* pDoc, CPoint point, MiniWindowMouseHooks & hooks)
  {
  // see if we moused over one of our miniwindows

  CMiniWindow * mw = NULL;
  CHotspot * pHotspot = NULL;
  string sHotspotId;
  string sMiniWindowId;

  // find which miniwindow we are over, if any (and hotspot, if any)
  mw = MouseOverMiniWindow (pDoc, point, sHotspotId, pHotspot, sMiniWindowId);

  // the original hotspot that we moused over, in this window (if any)
  string sOldMouseOverHotspotInThisWindow;

  if (mw)
    {
    // remember mouse position in *this* window
    mw->m_last_mouseposition = CPoint (point.x - mw->m_rect.left, point.y - mw->m_rect.top);
    mw->m_last_mouse_update++;

    // remember original hotspot ID
    sOldMouseOverHotspotInThisWindow = mw->m_sMouseOverHotspot;
    // change to new one for the benefit of WindowInfo (19) - if mouse is not down
    if (mw->m_sMouseDownHotspot.empty ())
      mw->m_sMouseOverHotspot = sHotspotId;
    }


  // report mouse movements: version 4.45
  pDoc->m_lastMousePosition = point;

  pDoc->SendToAllPluginCallbacks (ON_PLUGIN_MOUSE_MOVED, 
                                  point.x, 
                                  point.y, 
                                  sMiniWindowId.c_str (),
                                  false,
                                  false);

  // drag-and-drop stuff

  // see if mouse was already down and captured

  if (!pDoc->m_sPreviousMiniWindow.empty ())
    {

    // here if *something* is up
    MiniWindowMapIterator it = pDoc->m_MiniWindows.find (pDoc->m_sPreviousMiniWindow);

    // find the invoking miniwindow
    if (it != pDoc->m_MiniWindows.end ())
      {

      CMiniWindow * prev_mw = it->second;

      prev_mw->m_client_mouseposition = point;

      // see if activity was mouse-down
      if (!prev_mw->m_sMouseDownHotspot.empty ())   // mouse was clicked
        {
        // lookup that HotspotId
        HotspotMapIterator it = prev_mw->m_Hotspots.find (prev_mw->m_sMouseDownHotspot);

        // call MoveCallback for that hotspot, if it exists
        if (it != prev_mw->m_Hotspots.end ())
          {
          prev_mw->m_bExecutingScript = true;
          SendMouseEventToPlugin (pDoc, hooks,
                              it->second->m_dispid_MoveCallback,
                                      prev_mw->m_sCallbackPlugin,
                                      it->second->m_sMoveCallback, 
                                      prev_mw->m_sMouseDownHotspot,
                                      prev_mw->m_FlagsOnMouseDown);
          prev_mw->m_bExecutingScript = false;
          }
        return true;  // that's all

        }   // we had previous hotspot


      } // previous window still exists


    } // end we have been up to something in a miniwindow


  // end drag-and-drop


  // Next see if they have moved away from our previous miniwindow, if any
  // Note: This code is for when we change miniwindows
  if (sMiniWindowId != pDoc->m_sPreviousMiniWindow  && !pDoc->m_sPreviousMiniWindow.empty ())
    {

    MiniWindowMapIterator it = pDoc->m_MiniWindows.find (pDoc->m_sPreviousMiniWindow);
  
    if (it != pDoc->m_MiniWindows.end ())
      {

      CMiniWindow * old_mw = it->second;

      old_mw->m_last_mouseposition = CPoint (point.x - old_mw->m_rect.left, point.y - old_mw->m_rect.top);
      old_mw->m_last_mouse_update++;

      // cancel previous move-over hotspot
      if (!old_mw->m_sMouseOverHotspot.empty ())   // HotspotId was used
        {
        // lookup that HotspotId
        HotspotMapIterator it = old_mw->m_Hotspots.find (old_mw->m_sMouseOverHotspot);

        string  sOldMouseOverHotspot = old_mw->m_sMouseOverHotspot;
        old_mw->m_sMouseOverHotspot.erase ();  // no mouse-over right now (changed windows)

        // call CancelMouseOver for that hotspot, if it exists
        if (it != old_mw->m_Hotspots.end ())
          {
          hooks.RemoveToolTip ();
          old_mw->m_bExecutingScript = true;
          SendMouseEventToPlugin (pDoc, hooks,
                              it->second->m_dispid_CancelMouseOver,
                                      old_mw->m_sCallbackPlugin,
                                      it->second->m_sCancelMouseOver, 
                                      sOldMouseOverHotspot);
          old_mw->m_bExecutingScript = false;
          }
        pDoc->m_sPreviousMiniWindow.erase ();  // no longer have a previous mouse-over
        }   // we had previous hotspot


      } // previous window still exists

    } // moved to different window


  // here if currently over a miniwindow and mouse is not captured (down)
  if (mw && !hooks.IsCaptured ())
    {


    // Cancel previous move-over hotspot (in this miniwindow)
    //  if either there is no hotspot at all, or there was a previous, different, one.
    // Note: Won't apply to changing miniwindows - we handled that above

    if ((pHotspot == NULL ||                            // not on any hotspot
         sOldMouseOverHotspotInThisWindow != sHotspotId)// or on different hotspot         
         && !sOldMouseOverHotspotInThisWindow.empty ()  // and we previously were over one in this miniwindow
         && sMiniWindowId == pDoc->m_sPreviousMiniWindow)     // and it is the same miniwindow
      {
      HotspotMapIterator it = mw->m_Hotspots.find (sOldMouseOverHotspotInThisWindow);

      if (it != mw->m_Hotspots.end ())
        {
        hooks.RemoveToolTip ();
        mw->m_bExecutingScript = true;
        SendMouseEventToPlugin (pDoc, hooks,
                              it->second->m_dispid_CancelMouseOver,
                                    mw->m_sCallbackPlugin,
                                    it->second->m_sCancelMouseOver, 
                                    sOldMouseOverHotspotInThisWindow);
        mw->m_bExecutingScript = false;
        }

      } // previous one which isn't this one, or we are no longer on one

    if (pHotspot)
      {
      // re-establish hotspot pointer in case previous callback deleted it
      HotspotMapIterator it = mw->m_Hotspots.find (sHotspotId);
      if (it == mw->m_Hotspots.end ())
        pHotspot = NULL;
      else 
        pHotspot = it->second;
      }    // if had a hotspot before

    // now, are we now over a hotspot?
    if (pHotspot)
      {

      // if they have moused-down, ignore mouse moves
      if (!mw->m_sMouseDownHotspot.empty ())
        return true;
  
      pDoc->m_sPreviousMiniWindow = sMiniWindowId;   // remember in case they move outside window

      // if different hotspot from before
      if (sHotspotId != sOldMouseOverHotspotInThisWindow)
        {
        // this is our new one
        mw->m_bExecutingScript = true;
        SendMouseEventToPlugin (pDoc, hooks,
                              pHotspot->m_dispid_MouseOver,
                                    mw->m_sCallbackPlugin, 
                                    pHotspot->m_sMouseOver, 
                                    sHotspotId);
        mw->m_bExecutingScript = false;
        // activate tooltip if possible
        hooks.ActivateToolTip ();


        if (pHotspot)
          {
          // re-establish hotspot pointer in case previous callback deleted it
          HotspotMapIterator it = mw->m_Hotspots.find (sHotspotId);
          if (it == mw->m_Hotspots.end ())
            pHotspot = NULL;
          else 
            pHotspot = it->second;
          }  // if had a hotspot before
              
        // capture mouse movements out of the miniwindow (version 4.46)
        // see: http://www.gammon.com.au/forum/?id=9980

        if (pHotspot && !pHotspot->m_sCancelMouseOver.empty ())
          hooks.TrackMouseLeave ();

        }
      else
        {  // same hotspot (new in version 4.45)
        // see lengthy forum discussion: http://www.gammon.com.au/forum/?id=9942
        if (pHotspot->m_Flags & 1)
          {
          mw->m_bExecutingScript = true;
          SendMouseEventToPlugin (pDoc, hooks,
                              pHotspot->m_dispid_MouseOver,
                                      mw->m_sCallbackPlugin, 
                                      pHotspot->m_sMouseOver, 
                                      sHotspotId, MW_MOUSE_NOT_FIRST);
          mw->m_bExecutingScript = false;
          }

        }
      }

    return true;    // we are over mini-window - don't check for underlying text
    }

  return false;     // keep going

  } // end of MouseMoveMiniWindow 


bool MouseDownMiniWindow (CMUSHclientDoc* pDoc, CPoint point, long flags, MiniWindowMouseHooks & hooks)
  {
  // see if we clicked over one of our miniwindows

  CMiniWindow * mw = NULL;
  CHotspot * pHotspot = NULL;
  string sHotspotId;
  string sMiniWindowId;

  mw = MouseOverMiniWindow (pDoc, point, sHotspotId, pHotspot, sMiniWindowId);

  // the original hotspot that we moused over, in this window (if any)
  string sOldMouseOverHotspotInThisWindow;

  if (mw)
    {
    // remember mouse position in *this* window
    mw->m_last_mouseposition = CPoint (point.x - mw->m_rect.left, point.y - mw->m_rect.top);
    mw->m_last_mouse_update++;

    // remember original hotspot ID
    sOldMouseOverHotspotInThisWindow = mw->m_sMouseOverHotspot;
    // change to new one for the benefit of WindowInfo (19/20)
    mw->m_sMouseDownHotspot = sHotspotId;
    }

  // ANY mouse-down cancels a previous mouse-over
  if (!pDoc->m_sPreviousMiniWindow.empty ())
    {

    MiniWindowMapIterator it = pDoc->m_MiniWindows.find (pDoc->m_sPreviousMiniWindow);
  
    if (it != pDoc->m_MiniWindows.end ())
      {

      CMiniWindow * old_mw = it->second;
      string oldHotSpotID = old_mw->m_sMouseOverHotspot;
      if (old_mw == mw)
         oldHotSpotID = sOldMouseOverHotspotInThisWindow;

      // cancel previous move-over hotspot
      if (!oldHotSpotID.empty ())   // HotspotId was used
        {
        // lookup that HotspotId
        HotspotMapIterator it = old_mw->m_Hotspots.find (oldHotSpotID);

        string  sOldMouseOverHotspot = oldHotSpotID;
        old_mw->m_sMouseOverHotspot.erase ();  // no mouse-over right now

        // call CancelMouseOver for that hotspot, if it exists
        if (it != old_mw->m_Hotspots.end ())
          {
          old_mw->m_bExecutingScript = true;
          SendMouseEventToPlugin (pDoc, hooks,
                              it->second->m_dispid_CancelMouseOver, 
                                      old_mw->m_sCallbackPlugin,
                                      it->second->m_sCancelMouseOver, 
                                      sOldMouseOverHotspot); 
          old_mw->m_bExecutingScript = false;
          }
        pDoc->m_sPreviousMiniWindow.erase ();  // no longer have a previous mouse-over
        }   // we had previous hotspot

      } // previous window still exists

    } // moved to different window

  // here if currently over a miniwindow
  if (mw)
    {
    mw->m_sMouseOverHotspot.erase ();

    if (pHotspot)
      {
      // re-establish hotspot pointer in case previous callback deleted it
      HotspotMapIterator it = mw->m_Hotspots.find (sHotspotId);
      if (it == mw->m_Hotspots.end ())
        pHotspot = NULL;
      else 
        pHotspot = it->second;
      }    // if had a hotspot before

    // now, are we now over a hotspot?
    if (pHotspot)
      {
      mw->m_FlagsOnMouseDown = flags & (MW_MOUSE_LH | MW_MOUSE_RH | MW_MOUSE_DBL | MW_MOUSE_MIDDLE); // remember mouse flags 
      mw->m_bExecutingScript = true;
      SendMouseEventToPlugin (pDoc, hooks,
                              pHotspot->m_dispid_MouseDown, 
                                  mw->m_sCallbackPlugin, 
                                  pHotspot->m_sMouseDown, 
                                  sHotspotId,
                                  flags);   // LH / RH mouse?
      mw->m_bExecutingScript = false;
      }

    pDoc->m_sPreviousMiniWindow = sMiniWindowId;   // remember in case they move outside window
    // have to capture anyway or we get mouse-up without previous mouse-down
    hooks.SetMouseCapture ();       // Capture the mouse until button up.
    return true;    // we are over mini-window - don't check for underlying text
    }

  return false;     // keep going

  } // end of MouseDownMiniWindow 


bool MouseUpMiniWindow (CMUSHclientDoc* pDoc, CPoint point, long flags, MiniWindowMouseHooks & hooks)
  {
  // see if we clicked over one of our miniwindows

  CMiniWindow * mw = NULL;
  CHotspot * pHotspot = NULL;
  string sHotspotId;
  string sMiniWindowId;
  bool bPreviouslyInMiniwindow = false;

  mw = MouseOverMiniWindow (pDoc, point, sHotspotId, pHotspot, sMiniWindowId);

  // the original hotspot that we moused over, in this window (if any)
  string sOldMouseDownHotspotInThisWindow;
  string sOldMouseDownHotspot;


  if (mw)
    {
    // remember mouse position in *this* window
    mw->m_last_mouseposition = CPoint (point.x - mw->m_rect.left, point.y - mw->m_rect.top);
    mw->m_last_mouse_update++;

    // remember original hotspot ID
    sOldMouseDownHotspotInThisWindow = mw->m_sMouseDownHotspot;
    // change to new one for the benefit of WindowInfo (19/20)
    mw->m_sMouseOverHotspot = sHotspotId;
    }

  // find mouse-down hotspot in previous window, if any
  if (!pDoc->m_sPreviousMiniWindow.empty ())
    {
    MiniWindowMapIterator it = pDoc->m_MiniWindows.find (pDoc->m_sPreviousMiniWindow);
    if (it != pDoc->m_MiniWindows.end ())
      {
      sOldMouseDownHotspot = it->second->m_sMouseDownHotspot;
      it->second->m_sMouseDownHotspot.erase ();
      }
    }

  if (mw)
    mw->m_sMouseDownHotspot.erase ();

  // drag-and-drop stuff

  // see if mouse was already down and captured

  if (!pDoc->m_sPreviousMiniWindow.empty ())
    {

    // here if *something* is up
    MiniWindowMapIterator it = pDoc->m_MiniWindows.find (pDoc->m_sPreviousMiniWindow);

    // find the invoking miniwindow
    if (it != pDoc->m_MiniWindows.end ())
      {

      CMiniWindow * prev_mw = it->second;

      prev_mw->m_client_mouseposition = point;

      // see if activity was mouse-down
      if (!sOldMouseDownHotspot.empty ())   // mouse was clicked
        {
        // lookup that HotspotId
        HotspotMapIterator it = prev_mw->m_Hotspots.find (sOldMouseDownHotspot);

        // call ReleaseCallback for that hotspot, if it exists
        if (it != prev_mw->m_Hotspots.end ())
          {

          prev_mw->m_bExecutingScript = true;
          SendMouseEventToPlugin (pDoc, hooks,
                              it->second->m_dispid_ReleaseCallback, 
                                      prev_mw->m_sCallbackPlugin,
                                      it->second->m_sReleaseCallback, 
                                      sOldMouseDownHotspot,
                                      prev_mw->m_FlagsOnMouseDown);
          prev_mw->m_bExecutingScript = false;
          }

        }   // we had previous hotspot


      } // previous window still exists


    } // end we have been up to something in a miniwindow


  // end drag-and-drop


  // A mouse-up not in this miniwindow cancels a previous mouse-down
  if (sMiniWindowId != pDoc->m_sPreviousMiniWindow && !pDoc->m_sPreviousMiniWindow.empty ())
    {
    hooks.ReleaseMouseCapture ();   // Release the mouse capture established at
                                    // the beginning of the mouse click.

    MiniWindowMapIterator it = pDoc->m_MiniWindows.find (pDoc->m_sPreviousMiniWindow);
  
    if (it != pDoc->m_MiniWindows.end ())
      {

      CMiniWindow * old_mw = it->second;

      old_mw->m_last_mouseposition = CPoint (point.x - old_mw->m_rect.left, point.y - old_mw->m_rect.top);
      old_mw->m_last_mouse_update++;

      // cancel previous move-down hotspot
      if (!sOldMouseDownHotspot.empty ())   // HotspotId was used
        {
        // lookup that HotspotId
        HotspotMapIterator it = old_mw->m_Hotspots.find (sOldMouseDownHotspot);

        // call CancelMouseDown for that hotspot, if it exists
        if (it != old_mw->m_Hotspots.end ())
          {
          old_mw->m_bExecutingScript = true;
          SendMouseEventToPlugin (pDoc, hooks,
                              it->second->m_dispid_CancelMouseDown, 
                                      old_mw->m_sCallbackPlugin,
                                      it->second->m_sCancelMouseDown, 
                                      sOldMouseDownHotspot,
                                      old_mw->m_FlagsOnMouseDown);
          old_mw->m_bExecutingScript = false;
          }
        }   // we had previous hotspot

      } // previous window still exists

    pDoc->m_sPreviousMiniWindow.erase ();  // no longer have a previous window

    bPreviouslyInMiniwindow = true;

    } // released mouse in different window

  // here if currently over a miniwindow
  if (mw)
    {
    mw->m_sMouseDownHotspot.erase ();

    hooks.ReleaseMouseCapture ();   // Release the mouse capture established at
                                    // the beginning of the mouse click.

    if (pHotspot)
      {
      // re-establish hotspot pointer in case previous callback deleted it
      HotspotMapIterator it = mw->m_Hotspots.find (sHotspotId);
      if (it == mw->m_Hotspots.end ())
        pHotspot = NULL;
      else 
        pHotspot = it->second;
      }    // if had a hotspot before

    // if mouse-up outside the current hotspot just cancel previous one
    // cancel previous mouse-down hotspot (in this miniwindow)
    if ((pHotspot == NULL ||                      // not on any hotspot
         sOldMouseDownHotspotInThisWindow != sHotspotId)   // or on different hotspot         
         && !sOldMouseDownHotspotInThisWindow.empty ())    // and we previously were down in this miniwindow
      {
      HotspotMapIterator it = mw->m_Hotspots.find (sOldMouseDownHotspotInThisWindow);

      if (it != mw->m_Hotspots.end ())
        {
        mw->m_bExecutingScript = true;
        SendMouseEventToPlugin (pDoc, hooks,
                              it->second->m_dispid_CancelMouseDown, 
                                    mw->m_sCallbackPlugin,
                                    it->second->m_sCancelMouseDown, 
                                    sOldMouseDownHotspotInThisWindow,
                                    mw->m_FlagsOnMouseDown);
        mw->m_bExecutingScript = false;
        }

      } // previous one which isn't this one, or we are no longer on one

    if (pHotspot)
      {
      // re-establish hotspot pointer in case previous callback deleted it
      HotspotMapIterator it = mw->m_Hotspots.find (sHotspotId);
      if (it == mw->m_Hotspots.end ())
        pHotspot = NULL;
      else 
        pHotspot = it->second;
      }    // if had a hotspot before

    // now, did we release mouse over the hotspot it went down in?
    if (pHotspot && sOldMouseDownHotspotInThisWindow == sHotspotId)
      {
        mw->m_bExecutingScript = true;
        SendMouseEventToPlugin (pDoc, hooks,
                              pHotspot->m_dispid_MouseUp, 
                                    mw->m_sCallbackPlugin, 
                                    pHotspot->m_sMouseUp, 
                                    sHotspotId, 
                                    mw->m_FlagsOnMouseDown);  // LH / RH / middle mouse?
        mw->m_bExecutingScript = false;
      }

    pDoc->m_sPreviousMiniWindow.erase ();  // no longer have a previous mouse-over

    mw->m_sMouseOverHotspot.erase ();  // erase it so Mouse_Move_MiniWindow will work
    MouseMoveMiniWindow (pDoc, point, hooks); // we are no longer down so maybe we are over again
    return true;    // we are over mini-window - don't check for underlying text
    }


  return bPreviouslyInMiniwindow;     // keep going unless we started in another miniwindow
  } // end of MouseUpMiniWindow 


bool MouseWheelMiniWindow (CMUSHclientDoc* pDoc, CPoint point, long delta, MiniWindowMouseHooks & hooks)
  {
  // see if we moused over one of our miniwindows

  CMiniWindow * mw = NULL;
  CHotspot * pHotspot = NULL;
  string sHotspotId;
  string sMiniWindowId;

  mw = MouseOverMiniWindow (pDoc, point, sHotspotId, pHotspot, sMiniWindowId);

  if (!mw)
    return false;

  // now, are we now over a hotspot?
  if (pHotspot)
    {
    mw->m_bExecutingScript = true;
    SendMouseEventToPlugin (pDoc, hooks,
                              pHotspot->m_dispid_ScrollwheelCallback,
                                mw->m_sCallbackPlugin, 
                                pHotspot->m_sScrollwheelCallback, 
                                sHotspotId,
                                delta);
    mw->m_bExecutingScript = false;
    }

  return true;    // we are over mini-window - don't scroll underlying text

  } // end of MouseWheelMiniWindow 

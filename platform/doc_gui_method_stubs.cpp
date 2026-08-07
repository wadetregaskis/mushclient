// platform/doc_gui_method_stubs.cpp
//
// Inert headless definitions of the five GUI method families
// (miniwindow / notepad / chat / output-view / accelerator) that
// scripting/lua_methods.cpp binds but whose real TUs
// (scripting/methods/methods_{miniwindows,notepad,chat,output,accelerators}.cpp)
// are mushapp-only.  Each body returns the canonical no-GUI result —
// eNoSuchWindow / eNoChatConnections / empty safearray / VT_NULL / silent eOK —
// per the Campaign C flip inventory; the pre-flip portable twins'
// behaviours are the spec.  Delete family-by-family as the real TUs are
// adopted into mushcore (Phase 2 of the campaign endgame).
//
// Compiled into mushcore ONLY (never mushapp — the real TUs own these
// symbols there).

#ifndef _WIN32

// B-3 atomic flip: real headers (portable stub deleted; mushcore is FULL_BUILD).
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/errors.h"   // eOK / eNoSuchWindow / eNoChatConnections

#include <cstring>   // strlen
#include <cstdlib>   // free

// mxp/mxputils.cpp (mushcore) — returns true on FAILURE ("can't find colour")
bool SetColour (const CString & strName, COLORREF & iColour);

// ── VARIANT helpers ──────────────────────────────────────────────────────

// VT_EMPTY — what every real Window*/Accelerator List method answers when
// there is nothing to list: they either stamp VT_EMPTY explicitly
// (WindowFontList/WindowImageList/WindowHotspotList when the window doesn't
// exist) or never CreateOneDim ("cannot create empty array dimension":
// WindowList/AcceleratorList) so COleSafeArray::Detach hands back VT_EMPTY.
// pushVariant turns it into Lua nil, matching Windows.
static VARIANT EmptySafeArray (void)
  {
  VARIANT v;
  VariantInit (&v);
  v.vt = VT_EMPTY;
  return v;
  }

// VT_NULL — pushVariant's default arm turns it into nil.
static VARIANT NullVariant (void)
  {
  VARIANT v;
  VariantInit (&v);
  v.vt = VT_NULL;
  return v;
  }

// VT_EMPTY — also nil through pushVariant; the exact result the Windows
// list builders produce when they skip CreateOneDim on a zero count.
static VARIANT EmptyVariant (void)
  {
  VARIANT v;
  VariantInit (&v);
  return v;
  }


// ══════════════════════════════════════════════════════════════════════════
// Miniwindow family (49) — engine is CMiniWindow (miniwindow.cpp, mushapp-only)
// ══════════════════════════════════════════════════════════════════════════

// BlendPixel / FilterPixel are NOT stubbed any more: they are pure colour
// arithmetic with no miniwindow in sight, and now live in the shared
// scripting/methods/methods_pixel_shared.cpp, so the headless engine runs
// the real bodies.  The stubs here returned eNoSuchWindow for every call,
// which is a contract no Windows build has ever had.

long CMUSHclientDoc::WindowAddHotspot (LPCTSTR Name, LPCTSTR HotspotId, long Left, long Top, long Right, long Bottom, LPCTSTR MouseOver, LPCTSTR CancelMouseOver, LPCTSTR MouseDown, LPCTSTR CancelMouseDown, LPCTSTR MouseUp, LPCTSTR TooltipText, long Cursor, long Flags)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowAddHotspot

long CMUSHclientDoc::WindowArc (LPCTSTR Name, long Left, long Top, long Right, long Bottom, long x1, long y1, long x2, long y2, long PenColour, long PenStyle, long PenWidth)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowArc

long CMUSHclientDoc::WindowBezier (LPCTSTR Name, LPCTSTR Points, long PenColour, long PenStyle, long PenWidth)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowBezier

long CMUSHclientDoc::WindowBlendImage (LPCTSTR Name, LPCTSTR ImageId, long Left, long Top, long Right, long Bottom, short Mode, double Opacity, long SrcLeft, long SrcTop, long SrcRight, long SrcBottom)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowBlendImage

long CMUSHclientDoc::WindowCircleOp (LPCTSTR Name, short Action, long Left, long Top, long Right, long Bottom, long PenColour, long PenStyle, long PenWidth, long BrushColour, long BrushStyle, long Extra1, long Extra2, long Extra3, long Extra4)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowCircleOp

long CMUSHclientDoc::WindowCreate (LPCTSTR Name, long Left, long Top, long Width, long Height, short Position, long Flags, long BackgroundColour)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowCreate

long CMUSHclientDoc::WindowCreateImage (LPCTSTR Name, LPCTSTR ImageId, long Row1, long Row2, long Row3, long Row4, long Row5, long Row6, long Row7, long Row8)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowCreateImage

long CMUSHclientDoc::WindowDelete (LPCTSTR Name)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowDelete

long CMUSHclientDoc::WindowDeleteAllHotspots (LPCTSTR Name)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowDeleteAllHotspots

long CMUSHclientDoc::WindowDeleteHotspot (LPCTSTR Name, LPCTSTR HotspotId)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowDeleteHotspot

long CMUSHclientDoc::WindowDragHandler (LPCTSTR Name, LPCTSTR HotspotId, LPCTSTR MoveCallback, LPCTSTR ReleaseCallback, long Flags)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowDragHandler

long CMUSHclientDoc::WindowDrawImage (LPCTSTR Name, LPCTSTR ImageId, long Left, long Top, long Right, long Bottom, short Mode, long SrcLeft, long SrcTop, long SrcRight, long SrcBottom)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowDrawImage

long CMUSHclientDoc::WindowDrawImageAlpha (LPCTSTR Name, LPCTSTR ImageId, long Left, long Top, long Right, long Bottom, double Opacity, long SrcLeft, long SrcTop)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowDrawImageAlpha

long CMUSHclientDoc::WindowFilter (LPCTSTR Name, long Left, long Top, long Right, long Bottom, short Operation, double Options)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowFilter

long CMUSHclientDoc::WindowFont (LPCTSTR Name, LPCTSTR FontId, LPCTSTR FontName, double Size, BOOL Bold, BOOL Italic, BOOL Underline, BOOL Strikeout, short Charset, short PitchAndFamily)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowFont

VARIANT CMUSHclientDoc::WindowFontInfo (LPCTSTR Name, LPCTSTR FontId, long InfoType)
  {
  // VT_EMPTY, not VT_NULL: the real bodies stamp vaResult.vt = VT_EMPTY
  // before the lookup and leave it that way when the window/font/image/
  // hotspot is missing (methods_miniwindows.cpp).  Both reach Lua as nil, but
  // the five embedded engines go through mc_dispatch, where VT_NULL and
  // VT_EMPTY are distinct arms -- so this is a real difference for them.
  return EmptyVariant ();
  }   // end of CMUSHclientDoc::WindowFontInfo

VARIANT CMUSHclientDoc::WindowFontList (LPCTSTR Name)
  {
  return EmptySafeArray ();
  }   // end of CMUSHclientDoc::WindowFontList

long CMUSHclientDoc::WindowGetImageAlpha (LPCTSTR Name, LPCTSTR ImageId, long Left, long Top, long Right, long Bottom, long SrcLeft, long SrcTop)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowGetImageAlpha

long CMUSHclientDoc::WindowGetPixel (LPCTSTR Name, long x, long y)
  {
  // -2, not eNoSuchWindow.  The real body (methods_miniwindows.cpp:798) answers
  // the documented SENTINEL for "no such window" here, and plugins test for it
  // numerically -- `if WindowGetPixel(...) == -2 then`.  eNoSuchWindow is an
  // error CODE from a different space; answering it made every such test fail.
  return -2;
  }   // end of CMUSHclientDoc::WindowGetPixel

long CMUSHclientDoc::WindowGradient (LPCTSTR Name, long Left, long Top, long Right, long Bottom, long StartColour, long EndColour, short Mode)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowGradient

VARIANT CMUSHclientDoc::WindowHotspotInfo (LPCTSTR Name, LPCTSTR HotspotId, long InfoType)
  {
  // VT_EMPTY, not VT_NULL: the real bodies stamp vaResult.vt = VT_EMPTY
  // before the lookup and leave it that way when the window/font/image/
  // hotspot is missing (methods_miniwindows.cpp).  Both reach Lua as nil, but
  // the five embedded engines go through mc_dispatch, where VT_NULL and
  // VT_EMPTY are distinct arms -- so this is a real difference for them.
  return EmptyVariant ();
  }   // end of CMUSHclientDoc::WindowHotspotInfo

VARIANT CMUSHclientDoc::WindowHotspotList (LPCTSTR Name)
  {
  return EmptySafeArray ();
  }   // end of CMUSHclientDoc::WindowHotspotList

long CMUSHclientDoc::WindowHotspotTooltip (LPCTSTR Name, LPCTSTR HotspotId, LPCTSTR TooltipText)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowHotspotTooltip

long CMUSHclientDoc::WindowImageFromWindow (LPCTSTR Name, LPCTSTR ImageId, LPCTSTR SourceWindow)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowImageFromWindow

VARIANT CMUSHclientDoc::WindowImageInfo (LPCTSTR Name, LPCTSTR ImageId, long InfoType)
  {
  // VT_EMPTY, not VT_NULL: the real bodies stamp vaResult.vt = VT_EMPTY
  // before the lookup and leave it that way when the window/font/image/
  // hotspot is missing (methods_miniwindows.cpp).  Both reach Lua as nil, but
  // the five embedded engines go through mc_dispatch, where VT_NULL and
  // VT_EMPTY are distinct arms -- so this is a real difference for them.
  return EmptyVariant ();
  }   // end of CMUSHclientDoc::WindowImageInfo

VARIANT CMUSHclientDoc::WindowImageList (LPCTSTR Name)
  {
  return EmptySafeArray ();
  }   // end of CMUSHclientDoc::WindowImageList

long CMUSHclientDoc::WindowImageOp (LPCTSTR Name, short Action, long Left, long Top, long Right, long Bottom, long PenColour, long PenStyle, long PenWidth, long BrushColour, LPCTSTR ImageId, long EllipseWidth, long EllipseHeight)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowImageOp

VARIANT CMUSHclientDoc::WindowInfo (LPCTSTR Name, long InfoType)
  {
  // VT_EMPTY, not VT_NULL: the real bodies stamp vaResult.vt = VT_EMPTY
  // before the lookup and leave it that way when the window/font/image/
  // hotspot is missing (methods_miniwindows.cpp).  Both reach Lua as nil, but
  // the five embedded engines go through mc_dispatch, where VT_NULL and
  // VT_EMPTY are distinct arms -- so this is a real difference for them.
  return EmptyVariant ();
  }   // end of CMUSHclientDoc::WindowInfo

long CMUSHclientDoc::WindowLine (LPCTSTR Name, long x1, long y1, long x2, long y2, long PenColour, long PenStyle, long PenWidth)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowLine

VARIANT CMUSHclientDoc::WindowList ()
  {
  return EmptySafeArray ();
  }   // end of CMUSHclientDoc::WindowList

long CMUSHclientDoc::WindowLoadImage (LPCTSTR Name, LPCTSTR ImageId, LPCTSTR FileName)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowLoadImage

long CMUSHclientDoc::WindowLoadImageMemory (LPCTSTR Name, LPCTSTR ImageId, unsigned char * Buffer, const size_t Length, const bool bAlpha)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowLoadImageMemory

BSTR CMUSHclientDoc::WindowMenu (LPCTSTR Name, long Left, long Top, LPCTSTR Items)
  {
  return CString ("").AllocSysString ();   // no menu shown, nothing chosen
  }   // end of CMUSHclientDoc::WindowMenu

long CMUSHclientDoc::WindowMergeImageAlpha (LPCTSTR Name, LPCTSTR ImageId, LPCTSTR MaskId, long Left, long Top, long Right, long Bottom, short Mode, double Opacity, long SrcLeft, long SrcTop, long SrcRight, long SrcBottom)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowMergeImageAlpha

long CMUSHclientDoc::WindowMoveHotspot (LPCTSTR Name, LPCTSTR HotspotId, long Left, long Top, long Right, long Bottom)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowMoveHotspot

long CMUSHclientDoc::WindowPolygon (LPCTSTR Name, LPCTSTR Points, long PenColour, long PenStyle, long PenWidth, long BrushColour, long BrushStyle, BOOL Close, BOOL Winding)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowPolygon

long CMUSHclientDoc::WindowPosition (LPCTSTR Name, long Left, long Top, short Position, long Flags)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowPosition

long CMUSHclientDoc::WindowRectOp (LPCTSTR Name, short Action, long Left, long Top, long Right, long Bottom, long Colour1, long Colour2)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowRectOp

long CMUSHclientDoc::WindowResize (LPCTSTR Name, long Width, long Height, long BackgroundColour)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowResize

long CMUSHclientDoc::WindowScrollwheelHandler (LPCTSTR Name, LPCTSTR HotspotId, LPCTSTR MoveCallback)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowScrollwheelHandler

long CMUSHclientDoc::WindowSetPixel (LPCTSTR Name, long x, long y, long Colour)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowSetPixel

long CMUSHclientDoc::WindowSetZOrder (LPCTSTR Name, long Order)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowSetZOrder

long CMUSHclientDoc::WindowShow (LPCTSTR Name, BOOL Show)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowShow

long CMUSHclientDoc::WindowText (LPCTSTR Name, LPCTSTR FontId, LPCTSTR Text, long Left, long Top, long Right, long Bottom, long Colour, BOOL Unicode)
  {
  // -1 for an empty name AND for an unknown window (methods_miniwindows.cpp:235,
  // :241) -- WindowText returns the width it drew, so its failure sentinel is a
  // negative width, not an error code.
  return -1;
  }   // end of CMUSHclientDoc::WindowText

long CMUSHclientDoc::WindowTextWidth (LPCTSTR Name, LPCTSTR FontId, LPCTSTR Text, BOOL Unicode)
  {
  // -1: a measurement, so the sentinel is a negative width
  // (methods_miniwindows.cpp:251).
  return -1;
  }   // end of CMUSHclientDoc::WindowTextWidth

long CMUSHclientDoc::WindowTransformImage (LPCTSTR Name, LPCTSTR ImageId, float Left, float Top, short Mode, float Mxx, float Mxy, float Myx, float Myy)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowTransformImage

long CMUSHclientDoc::WindowWrite (LPCTSTR Name, LPCTSTR FileName)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowWrite

// ══════════════════════════════════════════════════════════════════════════
// Notepad family (14) — headless: FindNotepad → NULL; hooks null → FALSE/0/empty
// ══════════════════════════════════════════════════════════════════════════

BOOL CMUSHclientDoc::AppendToNotepad (LPCTSTR Title, LPCTSTR Contents)
  {
  return AppendToTheNotepad (Title, Contents, false);   // append mode (methods_notepad.cpp:207)
  }   // end of CMUSHclientDoc::AppendToNotepad

long CMUSHclientDoc::CloseNotepad (LPCTSTR Title, BOOL QuerySave)
  {
  (void) QuerySave;   // headless close never prompts
  return g_wxhook_close_notepad && g_wxhook_close_notepad (Title);
  }   // end of CMUSHclientDoc::CloseNotepad

CTextDocument * CMUSHclientDoc::FindNotepad (const CString strTitle)
  {
  return NULL;    // headless: no CTextDocument instances exist
  }   // end of CMUSHclientDoc::FindNotepad

long CMUSHclientDoc::GetNotepadLength (LPCTSTR Title)
  {
  if (!g_wxhook_get_notepad_text)
    return 0;
  char * text = g_wxhook_get_notepad_text (Title);
  long iLength = text ? (long) strlen (text) : 0;
  free (text);
  return iLength;
  }   // end of CMUSHclientDoc::GetNotepadLength

VARIANT CMUSHclientDoc::GetNotepadList (BOOL All)
  {
  (void) All;   // hook list is app-wide
  COleSafeArray sa;
  sa.CreateOneDim (VT_VARIANT, 0);
  long iCount = 0;
  if (g_wxhook_get_notepad_list)
    {
    char ** list = g_wxhook_get_notepad_list ();
    if (list)
      {
      long n = 0;
      while (list [n]) n++;
      sa.CreateOneDim (VT_VARIANT, n);
      for (long i = 0; list [i]; i++)
        {
        CString strTitle (list [i]);
        COleVariant v (strTitle);
        sa.PutElement (&iCount, &v);
        iCount++;
        free (list [i]);
        }
      free (list);
      }
    }
  VARIANT vResult;
  VariantInit (&vResult);
  sa.Detach (vResult);
  return vResult;
  }   // end of CMUSHclientDoc::GetNotepadList

BSTR CMUSHclientDoc::GetNotepadText (LPCTSTR Title)
  {
  CString strResult;
  if (g_wxhook_get_notepad_text)
    {
    char * text = g_wxhook_get_notepad_text (Title);
    if (text)
      {
      strResult = text;
      free (text);
      }
    }
  return strResult.AllocSysString ();
  }   // end of CMUSHclientDoc::GetNotepadText

long CMUSHclientDoc::MoveNotepadWindow (LPCTSTR Title, long Left, long Top, long Width, long Height)
  {
  return g_wxhook_move_notepad &&
         g_wxhook_move_notepad (Title, (int) Left, (int) Top,
                                (int) Width, (int) Height);
  }   // end of CMUSHclientDoc::MoveNotepadWindow

long CMUSHclientDoc::NotepadColour (LPCTSTR Title, LPCTSTR TextColour, LPCTSTR BackgroundColour)
  {
  COLORREF iTextColour, iBackColour;
  if (SetColour (TextColour, iTextColour))
    return false;    // can't find colour (as methods_notepad.cpp:424)
  if (SetColour (BackgroundColour, iBackColour))
    return false;    // can't find colour
  return g_wxhook_notepad_colour &&
         g_wxhook_notepad_colour (Title, (unsigned long) iTextColour,
                                  (unsigned long) iBackColour);
  }   // end of CMUSHclientDoc::NotepadColour

long CMUSHclientDoc::NotepadFont (LPCTSTR Title, LPCTSTR FontName, short Size, short Style, short Charset)
  {
  return g_wxhook_notepad_font &&
         g_wxhook_notepad_font (Title, FontName, Size, Style, Charset);
  }   // end of CMUSHclientDoc::NotepadFont

long CMUSHclientDoc::NotepadReadOnly (LPCTSTR Title, BOOL ReadOnly)
  {
  return g_wxhook_notepad_readonly &&
         g_wxhook_notepad_readonly (Title, ReadOnly != 0);
  }   // end of CMUSHclientDoc::NotepadReadOnly

BOOL CMUSHclientDoc::NotepadSaveMethod (LPCTSTR Title, short Method)
  {
  (void) Method;
  return FALSE;   // no notepad found (methods_notepad.cpp:484-485)
  }   // end of CMUSHclientDoc::NotepadSaveMethod

BOOL CMUSHclientDoc::ReplaceNotepad (LPCTSTR Title, LPCTSTR Contents)
  {
  return AppendToTheNotepad (Title, Contents, true);    // replace mode (methods_notepad.cpp:246)
  }   // end of CMUSHclientDoc::ReplaceNotepad

long CMUSHclientDoc::SaveNotepad (LPCTSTR Title, LPCTSTR FileName, BOOL ReplaceExisting)
  {
  return g_wxhook_save_notepad &&
         g_wxhook_save_notepad (Title, FileName, ReplaceExisting != 0);
  }   // end of CMUSHclientDoc::SaveNotepad

BOOL CMUSHclientDoc::SendToNotepad (LPCTSTR Title, LPCTSTR Contents)
  {
  // Windows creates a fresh eNotepadScript window; the hook-backed
  // AppendToTheNotepad is the portable equivalent (append-or-create).
  return AppendToTheNotepad (Title, Contents, false, eNotepadScript);
  }   // end of CMUSHclientDoc::SendToNotepad

// ══════════════════════════════════════════════════════════════════════════
// Chat family (25) — no CChatSocket headless
// ══════════════════════════════════════════════════════════════════════════

long CMUSHclientDoc::ChatAcceptCalls (short Port)
  {
  return eNoChatConnections;
  }   // end of CMUSHclientDoc::ChatAcceptCalls

long CMUSHclientDoc::ChatCall (LPCTSTR Server, long Port)
  {
  return eNoChatConnections;
  }   // end of CMUSHclientDoc::ChatCall

long CMUSHclientDoc::ChatCallzChat (LPCTSTR Server, long Port)
  {
  return eNoChatConnections;
  }   // end of CMUSHclientDoc::ChatCallzChat

long CMUSHclientDoc::ChatDisconnect (long ID)
  {
  return eNoChatConnections;
  }   // end of CMUSHclientDoc::ChatDisconnect

long CMUSHclientDoc::ChatDisconnectAll ()
  {
  return eNoChatConnections;
  }   // end of CMUSHclientDoc::ChatDisconnectAll

long CMUSHclientDoc::ChatEverybody (LPCTSTR Message, BOOL Emote)
  {
  return eNoChatConnections;
  }   // end of CMUSHclientDoc::ChatEverybody

long CMUSHclientDoc::ChatGetID (LPCTSTR Who)
  {
  return eNoChatConnections;
  }   // end of CMUSHclientDoc::ChatGetID

long CMUSHclientDoc::ChatGroup (LPCTSTR Group, LPCTSTR Message, BOOL Emote)
  {
  return eNoChatConnections;
  }   // end of CMUSHclientDoc::ChatGroup

long CMUSHclientDoc::ChatID (long ID, LPCTSTR Message, BOOL Emote)
  {
  return eNoChatConnections;
  }   // end of CMUSHclientDoc::ChatID

long CMUSHclientDoc::ChatMessage (long ID, short Message, LPCTSTR Text)
  {
  return eNoChatConnections;
  }   // end of CMUSHclientDoc::ChatMessage

long CMUSHclientDoc::ChatNameChange (LPCTSTR NewName)
  {
  return eNoChatConnections;
  }   // end of CMUSHclientDoc::ChatNameChange

void CMUSHclientDoc::ChatNote (short NoteType, LPCTSTR Message)
  {
  // no chat system headless — nothing to do
  }   // end of CMUSHclientDoc::ChatNote

long CMUSHclientDoc::ChatPasteEverybody ()
  {
  return eNoChatConnections;
  }   // end of CMUSHclientDoc::ChatPasteEverybody

long CMUSHclientDoc::ChatPasteText (long ID)
  {
  return eNoChatConnections;
  }   // end of CMUSHclientDoc::ChatPasteText

long CMUSHclientDoc::ChatPeekConnections (long ID)
  {
  return eNoChatConnections;
  }   // end of CMUSHclientDoc::ChatPeekConnections

long CMUSHclientDoc::ChatPersonal (LPCTSTR Who, LPCTSTR Message, BOOL Emote)
  {
  return eNoChatConnections;
  }   // end of CMUSHclientDoc::ChatPersonal

long CMUSHclientDoc::ChatPing (long ID)
  {
  return eNoChatConnections;
  }   // end of CMUSHclientDoc::ChatPing

long CMUSHclientDoc::ChatRequestConnections (long ID)
  {
  return eNoChatConnections;
  }   // end of CMUSHclientDoc::ChatRequestConnections

long CMUSHclientDoc::ChatSendFile (long ID, LPCTSTR FileName)
  {
  return eNoChatConnections;
  }   // end of CMUSHclientDoc::ChatSendFile

void CMUSHclientDoc::ChatStopAcceptingCalls ()
  {
  // no chat system headless — nothing to do
  }   // end of CMUSHclientDoc::ChatStopAcceptingCalls

long CMUSHclientDoc::ChatStopFileTransfer (long ID)
  {
  return eNoChatConnections;
  }   // end of CMUSHclientDoc::ChatStopFileTransfer

VARIANT CMUSHclientDoc::GetChatInfo (long ChatID, short InfoType)
  {
  return NullVariant ();    // the Windows missing-socket result (methods_chat.cpp GetChatSocket → NULL)
  }   // end of CMUSHclientDoc::GetChatInfo

VARIANT CMUSHclientDoc::GetChatList ()
  {
  return EmptyVariant ();   // the Windows zero-connection result (methods_chat.cpp:568-602)
  }   // end of CMUSHclientDoc::GetChatList

VARIANT CMUSHclientDoc::GetChatOption (long ID, LPCTSTR OptionName)
  {
  return NullVariant ();    // the Windows missing-socket result (methods_chat.cpp GetChatSocket → NULL)
  }   // end of CMUSHclientDoc::GetChatOption

long CMUSHclientDoc::SetChatOption (long ID, LPCTSTR OptionName, LPCTSTR Value)
  {
  return eNoChatConnections;
  }   // end of CMUSHclientDoc::SetChatOption

// ══════════════════════════════════════════════════════════════════════════
// Output/view family (25) — GUI ops inert; line-list ops keep twin semantics
// ══════════════════════════════════════════════════════════════════════════

void CMUSHclientDoc::ActivateClient ()
  {
  // GUI-only headless — silent no-op
  }   // end of CMUSHclientDoc::ActivateClient

long CMUSHclientDoc::AddFont (LPCTSTR PathName)
  {
  // The real body's FIRST act is a parameter check, ahead of anything GUI:
  //   if (strlen (PathName) <= 0) return eBadParameter;
  // (methods_output.cpp).  Registering the font itself needs a GUI, but
  // answering eOK to an empty path is a contract Windows has never had, and a
  // test asserting it would be pinning the stub.
  if (!PathName || strlen (PathName) == 0)
    return eBadParameter;
  return eOK;   // nothing to register headless
  }   // end of CMUSHclientDoc::AddFont

// Bookmark is no longer stubbed: the real body moved to doc_shared.cpp (its
// only view dependency was the CMUSHView walk, now the IWorldView seam), so
// mushcore links the canonical one.

// DeleteLines is no longer stubbed: the real body moved to
// scripting/methods/methods_output_shared.cpp, so mushcore links the
// canonical one (m_bInSendToScript refusal, m_total_lines, jump index).

void CMUSHclientDoc::DeleteOutput ()
  {
  // The real body (methods_output.cpp) is exactly this one call, and
  // ClearOutput lives in doc.cpp, which mushcore links.  The old stub
  // reimplemented the teardown by hand and skipped three things
  // ClearOutput does: m_sRecentLines.clear (), the m_DisplayFindInfo
  // reset, and EmitClearOutput () -- so world.DeleteOutput left stale
  // trigger context, a live find cursor into freed lines, and a view
  // still showing the text it had just deleted.
  ClearOutput ();
  }   // end of CMUSHclientDoc::DeleteOutput

void CMUSHclientDoc::FlashIcon ()
  {
  // GUI-only headless — silent no-op
  }   // end of CMUSHclientDoc::FlashIcon

long CMUSHclientDoc::GetFrame ()
  {
  return 0;   // HWND-ish null — L_GetFrame pushes it as a null lightuserdata
  }   // end of CMUSHclientDoc::GetFrame

BSTR CMUSHclientDoc::GetRecentLines (long Count)
  {
  // Join the last Count entries of m_sRecentLines (the old portable twin).
  std::string result;
  if (Count > 0)
    {
    long total = (long) m_sRecentLines.size ();
    long start = total - Count;
    if (start < 0) start = 0;
    for (long i = start; i < total; ++i)
      {
      // Guard on the ACCUMULATOR, not the index — methods_output.cpp:116-117 is
      // `if (!s.empty ()) s += '\n';`.  The two differ in exactly one case: when
      // the oldest line in the window is blank.  Blank lines DO reach here —
      // ProcessPreviousLine.cpp:549 pushes every completed line unconditionally —
      // so with {"", "hp: 100"} the index guard returned "\nhp: 100" where the
      // product returns "hp: 100".  Multi-line trigger matching reads this.
      if (!result.empty ()) result += '\n';
      result += m_sRecentLines [(size_t) i];
      }
    }
  return CString (result.c_str ()).AllocSysString ();
  }   // end of CMUSHclientDoc::GetRecentLines

void CMUSHclientDoc::MoveMainWindow (long Left, long Top, long Width, long Height)
  {
  // GUI-only headless — silent no-op
  }   // end of CMUSHclientDoc::MoveMainWindow

void CMUSHclientDoc::MoveWorldWindowX (long Left, long Top, long Width, long Height, short Which)
  {
  // GUI-only headless — silent no-op
  }   // end of CMUSHclientDoc::MoveWorldWindowX

// MoveWorldWindow and GetNotepadWindowPosition were the only two dispatch-map
// entries with no headless body, which nothing noticed because the dispatch
// table itself (platform/mc_dispatch.cpp) had never been linked into a test
// binary -- the archive member was only pulled in once something referenced
// MC_DispatchInvoke, and until tests/test_mc_dispatch.cpp nothing did.  Adding
// them is what lets that whole substrate be tested at all.
void CMUSHclientDoc::MoveWorldWindow (long Left, long Top, long Width, long Height)
  {
  // GUI-only headless — silent no-op
  }   // end of CMUSHclientDoc::MoveWorldWindow

BSTR CMUSHclientDoc::GetNotepadWindowPosition (LPCTSTR Title)
  {
  // GUI-only headless — no notepad windows exist, so no position to report.
  return CString ("").AllocSysString ();
  }   // end of CMUSHclientDoc::GetNotepadWindowPosition

void CMUSHclientDoc::Pause (BOOL Flag)
  {
  // Scroll-lock / release the output pane through the IWorldView seam
  // (the old portable twin's behaviour; m_gui_paused feeds GetInfo(114)).
  EmitPaused (Flag != 0);
  m_gui_paused = Flag != 0;
  }   // end of CMUSHclientDoc::Pause

void CMUSHclientDoc::Repaint ()
  {
  // NOT a no-op.  The real body (methods_output.cpp:509-523) opens with
  // UpdateAllViews (NULL) and then walks the views to force an immediate
  // redraw.  Only the walk is GUI-bound: CMUSHclientDoc::UpdateAllViews is the
  // REAL override in doc_display_shared.cpp:175, which mushcore links, and it
  // is what drains the display buffer into pal::IWorldView.  Dropping it meant
  // world.Repaint () did nothing at all headless.
  UpdateAllViews (NULL);
  }   // end of CMUSHclientDoc::Repaint

void CMUSHclientDoc::ResetStatusTime ()
  {
  // GUI-only headless — silent no-op
  }   // end of CMUSHclientDoc::ResetStatusTime

long CMUSHclientDoc::SetBackgroundImage (LPCTSTR FileName, short Mode)
  {
  return eOK;
  }   // end of CMUSHclientDoc::SetBackgroundImage

long CMUSHclientDoc::SetCursor (long Cursor)
  {
  // Same range gate the real port arm applies before it touches a widget
  // (methods_output.cpp: `if (Cursor < -1 || Cursor > 12) return eBadParameter`).
  if (Cursor < -1 || Cursor > 12)
    return eBadParameter;
  return eOK;   // no pane to set a cursor on headless
  }   // end of CMUSHclientDoc::SetCursor

long CMUSHclientDoc::SetForegroundImage (LPCTSTR FileName, short Mode)
  {
  return eOK;
  }   // end of CMUSHclientDoc::SetForegroundImage

void CMUSHclientDoc::SetOutputFont (LPCTSTR FontName, short PointSize)
  {
  // Drive the IWorldView seam as the old portable twin did.
  EmitOutputFont (std::string (FontName ? FontName : ""), PointSize, false, false);
  }   // end of CMUSHclientDoc::SetOutputFont

long CMUSHclientDoc::SetScroll (long Position, BOOL Visible)
  {
  // MUST match the real body in scripting/methods/methods_output.cpp, which is
  // what mushgui links -- this file is test-link-only.  The two drifted once
  // already: this twin dropped Visible on the floor, so GetInfo(120) reported a
  // hard-coded "true" no matter what a script set, and the headless suite could
  // not see the product's behaviour at all.
  //
  // Visible is RECORDED even though the port's pane always shows its scrollbar:
  // m_bScrollBarWanted has no other writer besides doc_construct.cpp:136, and
  // GetInfo(120) reports it.  -2 means "no change" to the position.
  m_bScrollBarWanted = Visible;

  if (Position != -2)
    EmitScroll (Position);
  return eOK;
  }   // end of CMUSHclientDoc::SetScroll

long CMUSHclientDoc::SetToolBarPosition (short Which, BOOL Float, short Side, long Top, long Left)
  {
  // The real body's switch on Which ends in `default: return eBadParameter`
  // (methods_output.cpp) before any toolbar is touched.  There are no toolbars
  // headless, but the validation is not a GUI concern.
  if (Which < 1 || Which > 4)
    return eBadParameter;
  return eOK;
  }   // end of CMUSHclientDoc::SetToolBarPosition

void CMUSHclientDoc::SetWorldWindowStatus (short Parameter)
  {
  // GUI-only headless — silent no-op
  }   // end of CMUSHclientDoc::SetWorldWindowStatus

long CMUSHclientDoc::TextRectangle (long Left, long Top, long Right, long Bottom, long BorderOffset, long BorderColour, long BorderWidth, long OutsideFillColour, long OutsideFillStyle)
  {
  // The real body (methods_output.cpp:520-580) opens with ValidateBrushStyle
  // (miniwindow.cpp, mushgui-only) and has a CMUSHView walk near the end, but
  // everything else in it is portable: the nine assignments are pure document
  // state that world.GetInfo and the wrap-column plumbing both read back, and
  // the last two statements are NOT view-bound at all.
  //
  // The brush validation is NOT replicated here: duplicating it would be the
  // twin drift this file is supposed to be shrinking, and a headless caller
  // has no brush.  So an invalid OutsideFillStyle is accepted here and refused
  // on the GUI build — recorded rather than hidden.
  m_TextRectangle = CRect (Left, Top, Right, Bottom);
  m_TextRectangleBorderOffset      = BorderOffset;
  m_TextRectangleBorderColour      = BorderColour;
  m_TextRectangleBorderWidth       = BorderWidth;
  m_TextRectangleOutsideFillColour = OutsideFillColour;
  m_TextRectangleOutsideFillStyle  = OutsideFillStyle;
  EmitTextRectChanged ();

  // methods_output.cpp:577-578.  SendWindowSizes is doc_shared.cpp:4570 — a
  // mushcore body, not a view call — so dropping it was pure drift: reserving a
  // text rectangle shrinks the usable width, and the MUD has to be told through
  // NAWS.  The headless engine used to skip that renegotiation silently, which
  // also meant no test could ever see a NAWS regression through this path.
  SendWindowSizes (m_nWrapColumn);
  Redraw ();
  return eOK;
  }   // end of CMUSHclientDoc::TextRectangle

BOOL CMUSHclientDoc::Transparency (long Key, short Amount)
  {
  return FALSE;   // no layered windows headless
  }   // end of CMUSHclientDoc::Transparency

// ══════════════════════════════════════════════════════════════════════════
// Accelerator family: NO LONGER STUBBED.  scripting/methods/methods_accelerators.cpp
// is in mushcore now, so the tests exercise the real bodies — which is the point:
// the off-Windows arm of AcceleratorTo used to be unreachable behind a view-list
// gate, and a stub here would have hidden that forever.

#endif  // !_WIN32

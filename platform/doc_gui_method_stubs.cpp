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

long CMUSHclientDoc::BlendPixel (long Blend, long Base, short Mode, double Opacity)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::BlendPixel

long CMUSHclientDoc::FilterPixel (long Pixel, short Operation, double Options)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::FilterPixel

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
  return NullVariant ();
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
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowGetPixel

long CMUSHclientDoc::WindowGradient (LPCTSTR Name, long Left, long Top, long Right, long Bottom, long StartColour, long EndColour, short Mode)
  {
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowGradient

VARIANT CMUSHclientDoc::WindowHotspotInfo (LPCTSTR Name, LPCTSTR HotspotId, long InfoType)
  {
  return NullVariant ();
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
  return NullVariant ();
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
  return NullVariant ();
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
  return eNoSuchWindow;
  }   // end of CMUSHclientDoc::WindowText

long CMUSHclientDoc::WindowTextWidth (LPCTSTR Name, LPCTSTR FontId, LPCTSTR Text, BOOL Unicode)
  {
  return eNoSuchWindow;
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
  return eOK;   // nothing to register headless (the portable twin's result)
  }   // end of CMUSHclientDoc::AddFont

void CMUSHclientDoc::Bookmark (long LineNumber, BOOL Set)
  {
  // GUI-only headless — silent no-op
  }   // end of CMUSHclientDoc::Bookmark

void CMUSHclientDoc::DeleteLines (long Count)
  {
  // The old portable twin's crash-safe delete (display step 7): the
  // in-progress line is the list TAIL — deleting it would leave
  // m_pCurrentLine dangling (use-after-free on the next output).
  // Delete up to Count COMPLETED lines from just before it.
  if (Count <= 0)
    return;
  for (long i = 0; i < Count; ++i)
    {
    POSITION pos = m_LineList.GetTailPosition ();
    if (!pos) break;
    CLine * pLine = m_LineList.GetAt (pos);
    if (pLine == m_pCurrentLine)
      {
      m_LineList.GetPrev (pos);       // step past the in-progress tail
      if (!pos) break;                // nothing completed left
      pLine = m_LineList.GetAt (pos);
      }
    m_LineList.RemoveAt (pos);
    delete pLine;
    }
  // Invalidate index slots beyond the shrunken count and clamp the drain
  // cursor so already-emitted accounting can't point past the list.
  long count = m_LineList.GetCount ();
  for (long s = (count / JUMP_SIZE) + 1; s <= m_maxlines / JUMP_SIZE; ++s)
    m_pLinePositions [s] = NULL;
  if (m_gui_lines_emitted > count - 1)
    m_gui_lines_emitted = (count > 0) ? count - 1 : 0;
  }   // end of CMUSHclientDoc::DeleteLines

void CMUSHclientDoc::DeleteOutput ()
  {
  // Free every line INCLUDING the in-progress tail, then re-seed a fresh
  // in-list current line and re-prime the index (the old portable twin's
  // display step 7 lifecycle, mirroring the real doc.cpp ClearOutput).
  POSITION pos = m_LineList.GetHeadPosition ();
  while (pos)
    {
    CLine * pLine = m_LineList.GetNext (pos);
    delete pLine;
    }
  m_LineList.RemoveAll ();
  m_total_lines = 0;
  m_pCurrentLine = new CLine (++m_total_lines, m_nWrapColumn,
                              0, WHITE, BLACK, m_bUTF_8);
  m_LineList.AddTail (m_pCurrentLine);
  AllocateLinePositions ();
  m_gui_lines_emitted = 0;
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
      if (i > start) result += '\n';
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

void CMUSHclientDoc::Pause (BOOL Flag)
  {
  // Scroll-lock / release the output pane through the IWorldView seam
  // (the old portable twin's behaviour; m_gui_paused feeds GetInfo(114)).
  EmitPaused (Flag != 0);
  m_gui_paused = Flag != 0;
  }   // end of CMUSHclientDoc::Pause

void CMUSHclientDoc::Repaint ()
  {
  // GUI-only headless — silent no-op
  }   // end of CMUSHclientDoc::Repaint

void CMUSHclientDoc::ResetStatusTime ()
  {
  // GUI-only headless — silent no-op
  }   // end of CMUSHclientDoc::ResetStatusTime

long CMUSHclientDoc::SetBackgroundColour (long Colour)
  {
  (void) Colour; return eOK;
  }   // end of CMUSHclientDoc::SetBackgroundColour

long CMUSHclientDoc::SetBackgroundImage (LPCTSTR FileName, short Mode)
  {
  return eOK;
  }   // end of CMUSHclientDoc::SetBackgroundImage

long CMUSHclientDoc::SetCursor (long Cursor)
  {
  (void) Cursor; return eOK;
  }   // end of CMUSHclientDoc::SetCursor

long CMUSHclientDoc::SetForegroundImage (LPCTSTR FileName, short Mode)
  {
  return eOK;
  }   // end of CMUSHclientDoc::SetForegroundImage

void CMUSHclientDoc::SetMainTitle (LPCTSTR Title)
  {
  // GUI-only headless — silent no-op
  }   // end of CMUSHclientDoc::SetMainTitle

void CMUSHclientDoc::SetOutputFont (LPCTSTR FontName, short PointSize)
  {
  // Drive the IWorldView seam as the old portable twin did.
  EmitOutputFont (std::string (FontName ? FontName : ""), PointSize, false, false);
  }   // end of CMUSHclientDoc::SetOutputFont

long CMUSHclientDoc::SetScroll (long Position, BOOL Visible)
  {
  // Visible is ignored off-Windows (the old portable twin's behaviour);
  // -2 means "no change" on Windows.
  (void) Visible;
  if (Position != -2)
    EmitScroll (Position);
  return eOK;
  }   // end of CMUSHclientDoc::SetScroll

long CMUSHclientDoc::SetToolBarPosition (short Which, BOOL Float, short Side, long Top, long Left)
  {
  return eOK;
  }   // end of CMUSHclientDoc::SetToolBarPosition

void CMUSHclientDoc::SetUnseenLines (long Counter)
  {
  // GUI-only headless — silent no-op
  }   // end of CMUSHclientDoc::SetUnseenLines

void CMUSHclientDoc::SetWorldWindowStatus (short Parameter)
  {
  // GUI-only headless — silent no-op
  }   // end of CMUSHclientDoc::SetWorldWindowStatus

long CMUSHclientDoc::TextRectangle (long Left, long Top, long Right, long Bottom, long BorderOffset, long BorderColour, long BorderWidth, long OutsideFillColour, long OutsideFillStyle)
  {
  return eOK;
  }   // end of CMUSHclientDoc::TextRectangle

BOOL CMUSHclientDoc::Transparency (long Key, short Amount)
  {
  return FALSE;   // no layered windows headless
  }   // end of CMUSHclientDoc::Transparency

// ══════════════════════════════════════════════════════════════════════════
// Accelerator family (3) — inert; tables live in accelerators.cpp (mushcore)
// ══════════════════════════════════════════════════════════════════════════

long CMUSHclientDoc::Accelerator (LPCTSTR Key, LPCTSTR Send)
  {
  return eOK;   // accepted, inert — no key events arrive headless
  }   // end of CMUSHclientDoc::Accelerator

long CMUSHclientDoc::AcceleratorTo (LPCTSTR Key, LPCTSTR Send, short SendTo)
  {
  return eOK;   // accepted, inert
  }   // end of CMUSHclientDoc::AcceleratorTo

VARIANT CMUSHclientDoc::AcceleratorList ()
  {
  return EmptySafeArray ();
  }   // end of CMUSHclientDoc::AcceleratorList

#endif  // !_WIN32

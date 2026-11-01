// protocols_mxp.cpp -- the MXP features that are more than a style change: line tags,
// EXPIRE, GAUGE and STAT, FRAME and DEST, IMAGE, RELOCATE.
//
// The output window's model is a grid of lines, so what does not fit that goes in
// miniwindows the client makes for itself.  Decided here, so it is said once:
//
//   FRAME/DEST  A frame is a miniwindow holding a grid of cells.  An INTERNAL frame is
//               placed by ALIGN and a floating one by LEFT and TOP, both drawn over the
//               output window, which does not move its text aside.  Not done: dragging,
//               resizing, FLOATING (a miniwindow is always on top of the output), and
//               PARENT, which the spec calls obsolete.
//   GAUGE/STAT  One small miniwindow, top right, shown only once a server asks.
//   IMAGE       A file already in this world's cache folder is shown in a miniwindow, if
//               the client can load it (PNG, BMP).  Otherwise the [url] link as before.
//               Nothing is ever fetched: clicking the link is the player's choice.  ISMAP
//               is not done, nor are H and W.
//
// A plugin that wants to do any of this itself can veto the tag in OnPluginMXPopenTag,
// and the option mxp_windows turns all these windows off.  Limits are here because a
// server chooses all the numbers.

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "protocols.h"
#include "mxp/mxp.h"
#include "scripting/errors.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace {

const size_t kMaxFrames = 8;
const size_t kMaxBindings = 32;
const size_t kMaxExpireNames = 256;
const int    kMinWindow = 16;
const int    kMaxWindow = 4096;

const char * const kBindingWindow = "mxp_status";
const char * const kImageWindow   = "mxp_image";

std::string FrameWindowName (const std::string & frame)
  {
  return "mxp_frame_" + frame;
  }

bool IsPlainName (const std::string & name)
  {
  if (name.empty () || name.size () > 64)
    return false;
  for (const unsigned char c : name)
    if (!isalnum (c) && c != '_' && c != '-')
      return false;
  return true;
  }

int OutputExtent (CMUSHclientDoc & doc, long infoType, int fallback)
  {
  VARIANT v = doc.GetInfo (infoType);
  const int result = v.vt == VT_I4 && v.lVal > 0 ? v.lVal : fallback;
  VariantClear (&v);
  return result;
  }

long InfoLong (VARIANT v)
  {
  long result = 0;
  if (v.vt == VT_I4)
    result = v.lVal;
  else if (v.vt == VT_I2)
    result = v.iVal;
  VariantClear (&v);
  return result;
  }

std::string EntityValue (CMUSHclientDoc & doc, const std::string & name)
  {
  CString strValue;
  CString strName = name.c_str ();
  strName.MakeLower ();
  if (!doc.m_CustomEntityMap.Lookup (strName, strValue))
    return "";
  return (const char *) strValue;
  }

// the entity or variable a frame's font comes from
void SetUpWindowFont (CMUSHclientDoc & doc, const char * window)
  {
  const double size = doc.m_font_height > 0 && doc.m_font_height < 72 ? doc.m_font_height : 9;
  doc.WindowFont (window, "f", doc.m_font_name, size, FALSE, FALSE, FALSE, FALSE,
                  (short) doc.m_font_charset, 0);
  }

CMxpFrame * FindFrame (CProtocolState & state, const std::string & name)
  {
  const auto it = state.frames.find (proto::Lower (name));
  return it == state.frames.end () ? NULL : it->second.get ();
  }

CMxpFrame * DivertTarget (CProtocolState & state)
  {
  if (!state.destStack.empty ())
    return state.destStack.back ().frame.empty () ? NULL : FindFrame (state, state.destStack.back ().frame);
  return state.redirect.empty () ? NULL : FindFrame (state, state.redirect);
  }

void MarkFramesDirty (CProtocolState & state)
  {
  state.bFramesDirty = true;
  }

CMxpFrame * OpenFrame (CMUSHclientDoc & doc, const std::string & name)
  {
  CProtocolState & state = *doc.m_pProto;
  if (CMxpFrame * existing = FindFrame (state, name))
    return existing;
  if (!doc.m_bMXPWindows || !IsPlainName (name) || state.frames.size () >= kMaxFrames)
    return NULL;
  auto frame = std::make_shared <CMxpFrame> ();
  frame->name = name;
  frame->title = name;
  state.frames [proto::Lower (name)] = frame;
  return frame.get ();
  }

} // anonymous namespace

// ---------------------------------------------------------------- line tags

void Proto_MxpLineTag (CMUSHclientDoc & doc, const int tag)
  {
  CProtocolState & state = *doc.m_pProto;

  if (!doc.m_bMXP || !doc.m_pCurrentLine)
    return;

  // A tag the server has switched off does nothing at all.
  const auto defined = state.lineTags.find (tag);
  if (defined != state.lineTags.end () && !defined->second.bEnabled)
    return;

  state.lineTag = tag;
  state.bLineTagFromEscape = true;

  if (tag < 20)
    return;

  // The element with this TAG=, if any.  It was defined in secure mode, so it is opened
  // as if in secure mode now, and we close it at the end of the line ourselves.
  for (POSITION pos = doc.m_CustomElementMap.GetStartPosition (); pos; )
    {
    CString strName;
    CElement * pElement;
    doc.m_CustomElementMap.GetNextAssoc (pos, strName, pElement);
    if (pElement->iTag != tag)
      continue;

    const int iSavedMode = doc.m_iMXP_mode;
    doc.m_iMXP_mode = eMXP_secure;
    doc.MXP_StartTag (strName);
    doc.m_iMXP_mode = iSavedMode;
    state.lineElement = (const char *) strName;
    break;
    }

  // !TAG's colours, over whatever the element did.
  if (defined != state.lineTags.end () &&
      (defined->second.bHasFore || defined->second.bHasBack) &&
      !doc.m_bIgnoreMXPcolourChanges &&
      !doc.m_pCurrentLine->styleList.IsEmpty ())
    {
    CStyle * pStyle = doc.m_pCurrentLine->styleList.GetTail ();
    COLORREF fore, back;
    doc.GetStyleRGB (pStyle, fore, back);
    if (defined->second.bHasFore)
      fore = defined->second.fore;
    if (defined->second.bHasBack)
      back = defined->second.back;
    doc.AddStyle ((unsigned short) ((pStyle->iFlags & STYLE_BITS & ~COLOURTYPE) | COLOUR_RGB),
                  fore, back, 0, pStyle->pAction);
    }
  }

void Proto_MxpEndOfLine (CMUSHclientDoc & doc)
  {
  CProtocolState & state = *doc.m_pProto;

  if (state.lineElement.empty ())
    return;

  const std::string name = state.lineElement;
  state.lineElement.clear ();

  // Only if it is still open: the server may have closed it.
  CString strName = name.c_str ();
  bool bOpen = false;
  for (POSITION pos = doc.m_ActiveTagList.GetHeadPosition (); pos; )
    if (doc.m_ActiveTagList.GetNext (pos)->strName == strName)
      bOpen = true;

  if (!bOpen)
    return;

  const int iSavedMode = doc.m_iMXP_mode;
  doc.m_iMXP_mode = eMXP_secure;
  doc.MXP_EndTag (strName);
  doc.m_iMXP_mode = iSavedMode;
  }

bool Proto_MxpLineComplete (CMUSHclientDoc & doc, const CString & strLine, POSITION firstLine)
  {
  CProtocolState & state = *doc.m_pProto;

  const int tag = state.lineTag;
  if (!tag)
    return false;

  const bool bFromEscape = state.bLineTagFromEscape;
  state.lineTag = 0;
  state.bLineTagFromEscape = false;

  // every segment of a wrapped line, for GetLineInfo
  for (POSITION pos = firstLine; pos; )
    {
    CLine * pLine = doc.m_LineList.GetNext (pos);
    pLine->m_iMXPLineTag = (unsigned char) tag;
    }

  bool bOmit = false;

  if (bFromEscape)
    {
    std::string flag = proto::FlagForLineTag (tag);
    if (flag.empty ())
      flag = "tag" + std::to_string (tag);

    doc.SendToAllPluginCallbacks (ON_PLUGIN_MXP_FLAG, tag,
                                  flag + "=" + (const char *) strLine, false, false);

    // what FLAG=RoomName does, for a server that only tags for the automapper
    if (tag == eMXP_room_name && doc.m_bMapping && !strLine.IsEmpty ())
      {
      CString strRoom = strLine;
      strRoom.Replace ('{', '(');
      strRoom.Replace ('}', ')');
      doc.m_strMapList.AddTail ("{" + strRoom + "}");
      doc.DrawMappingStatusLine ();
      }

    // spec: welcome text is not shown after a RELOCATE
    if (tag == eMXP_welcome && state.bRelocated)
      bOmit = true;
    }

  const auto defined = state.lineTags.find (tag);
  if (defined != state.lineTags.end () && defined->second.bEnabled)
    {
    if (!defined->second.window.empty ())
      if (CMxpFrame * pFrame = OpenFrame (doc, defined->second.window))
        {
        const COLORREF fore = defined->second.bHasFore ? defined->second.fore : RGB (192, 192, 192);
        const COLORREF back = defined->second.bHasBack ? defined->second.back : RGB (0, 0, 0);
        std::string text = (const char *) strLine;
        for (size_t i = 0; i < text.size (); )
          {
          int len = 1;
          if (doc.m_bUTF_8)
            {
            const unsigned char lead = (unsigned char) text [i];
            len = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
            if (i + (size_t) len > text.size ())
              len = 1;
            }
          pFrame->Put (text.data () + i, len, fore, back);
          i += (size_t) len;
          }
        pFrame->NewLine ();
        MarkFramesDirty (state);
        bOmit = true;      // it has gone there instead
        }
    if (defined->second.bGag)
      bOmit = true;
    }

  return bOmit;
  }

void Proto_MxpFlagClosed (CMUSHclientDoc & doc, const CString & strFlag, const CString & strText)
  {
  CProtocolState & state = *doc.m_pProto;

  CString strLower = strFlag;
  strLower.MakeLower ();
  const std::string flag = (const char *) strLower;

  const int tag = proto::LineTagForFlag (flag);
  if (tag)
    {
    state.lineTag = tag;
    state.bLineTagFromEscape = false;
    }

  doc.SendToAllPluginCallbacks (ON_PLUGIN_MXP_FLAG, tag,
                                flag + "=" + (const char *) strText, false, false);
  }

// <!TAG index [WINDOWNAME=x] [FORE=c] [BACK=c] [GAG] [ENABLE] [DISABLE]>
void Proto_MxpTagDefinition (CMUSHclientDoc & doc, const CString & strIndex, CString strArgs)
  {
  CProtocolState & state = *doc.m_pProto;

  const int tag = IsNumeric (strIndex) ? atoi (strIndex) : 0;
  if (tag < 20 || tag > 99)
    {
    doc.MXP_error (DBG_ERROR, errMXP_InvalidDefinition,
                   CFormat ("Line tag number must be from 20 to 99: <!TAG %s>", (LPCTSTR) strIndex));
    return;
    }

  CArgumentList ArgumentList;
  if (doc.BuildArgumentList (ArgumentList, strArgs))
    {
    DELETE_LIST (ArgumentList);
    return;
    }

  CMxpLineTag & def = state.lineTags [tag];

  if (GetKeyword (ArgumentList, "gag"))
    def.bGag = true;
  if (GetKeyword (ArgumentList, "enable"))
    def.bEnabled = true;
  if (GetKeyword (ArgumentList, "disable"))
    def.bEnabled = false;

  const CString strWindow = GetArgument (ArgumentList, "windowname", 0, false);
  if (!strWindow.IsEmpty ())
    {
    if (IsPlainName ((const char *) strWindow))
      def.window = (const char *) strWindow;
    else
      doc.MXP_error (DBG_ERROR, errMXP_InvalidDefinition,
                     CFormat ("Bad window name in <!TAG %s>: %s", (LPCTSTR) strIndex, (LPCTSTR) strWindow));
    }

  COLORREF colour = 0;
  CString strColour = GetArgument (ArgumentList, "fore", 0, true);
  if (!strColour.IsEmpty () && !SetColour (strColour, colour))
    {
    def.bHasFore = true;
    def.fore = colour;
    }
  strColour = GetArgument (ArgumentList, "back", 0, true);
  if (!strColour.IsEmpty () && !SetColour (strColour, colour))
    {
    def.bHasBack = true;
    def.back = colour;
    }

  DELETE_LIST (ArgumentList);
  }

// ---------------------------------------------------------------- EXPIRE

void Proto_MxpNoteExpiringLink (CMUSHclientDoc & doc)
  {
  CProtocolState & state = *doc.m_pProto;

  if (state.openExpire.empty () || !doc.m_pCurrentLine)
    return;

  const auto it = state.expireOldest.find (state.openExpire);
  if (it != state.expireOldest.end ())
    it->second = (std::min) (it->second, doc.m_pCurrentLine->m_nLineNumber);
  else if (state.expireOldest.size () < kMaxExpireNames)
    state.expireOldest [state.openExpire] = doc.m_pCurrentLine->m_nLineNumber;
  }

// <EXPIRE name> kills that name's links, <EXPIRE> every link with a name.  Walking back
// only as far as the oldest such link, which is usually not far, and only once, since
// the name is forgotten afterwards: a server expiring on every prompt costs nothing.
void Proto_MxpExpire (CMUSHclientDoc & doc, const CString & strName)
  {
  CProtocolState & state = *doc.m_pProto;

  long oldest = -1;
  if (strName.IsEmpty ())
    {
    for (const auto & item : state.expireOldest)
      if (oldest < 0 || item.second < oldest)
        oldest = item.second;
    state.expireOldest.clear ();
    }
  else
    {
    const auto it = state.expireOldest.find ((const char *) strName);
    if (it != state.expireOldest.end ())
      {
      oldest = it->second;
      state.expireOldest.erase (it);
      }
    }

  if (oldest < 0)
    return;

  bool bChanged = false;

  for (POSITION linepos = doc.m_LineList.GetTailPosition (); linepos; )
    {
    CLine * pLine = doc.m_LineList.GetPrev (linepos);
    if (pLine->m_nLineNumber < oldest)
      break;

    for (POSITION stylepos = pLine->styleList.GetHeadPosition (); stylepos; )
      {
      CStyle * pStyle = pLine->styleList.GetNext (stylepos);
      if (!pStyle->pAction || (pStyle->iFlags & START_TAG) ||
          pStyle->pAction->m_strExpire.IsEmpty ())
        continue;
      if (!strName.IsEmpty () && pStyle->pAction->m_strExpire != strName)
        continue;

      pStyle->pAction->Release ();
      pStyle->pAction = NULL;
      pStyle->iFlags &= ~ACTIONTYPE;
      if (doc.m_bUnderlineHyperlinks)
        pStyle->iFlags &= ~UNDERLINE;
      bChanged = true;
      }
    }

  if (bChanged)
    doc.UpdateAllViews (NULL);
  }

// ---------------------------------------------------------------- bindings

void Proto_MxpBinding (CMUSHclientDoc & doc, const bool bGauge, const std::string & entity,
                       const std::string & maxEntity, const std::string & caption,
                       const std::string & colour)
  {
  CProtocolState & state = *doc.m_pProto;

  if (entity.empty ())
    return;

  CMxpBinding * pBinding = NULL;
  for (CMxpBinding & existing : state.bindings)
    if (existing.bGauge == bGauge && existing.entity == proto::Lower (entity))
      pBinding = &existing;

  if (!pBinding)
    {
    if (state.bindings.size () >= kMaxBindings)
      return;
    state.bindings.push_back (CMxpBinding ());
    pBinding = &state.bindings.back ();
    }

  pBinding->bGauge = bGauge;
  pBinding->entity = proto::Lower (entity);
  pBinding->maxEntity = proto::Lower (maxEntity);
  pBinding->caption = caption;
  pBinding->colour = colour;
  state.bBindingsDirty = true;
  }

void Proto_MxpEntityChanged (CMUSHclientDoc & doc)
  {
  CProtocolState & state = *doc.m_pProto;
  if (!state.bindings.empty ())
    state.bBindingsDirty = true;
  }

static void PaintBindings (CMUSHclientDoc & doc)
  {
  CProtocolState & state = *doc.m_pProto;
  state.bBindingsDirty = false;

  if (!doc.m_bMXPWindows || state.bindings.empty ())
    return;

  const int kWidth = 220;
  const int kRow = 18;
  const int height = (int) state.bindings.size () * kRow + 4;

  doc.WindowCreate (kBindingWindow, 0, 0, kWidth, height, 6 /* top right */, 0, RGB (16, 16, 24));
  SetUpWindowFont (doc, kBindingWindow);

  int y = 2;
  for (const CMxpBinding & binding : state.bindings)
    {
    const std::string value = EntityValue (doc, binding.entity);
    const std::string maxValue = binding.maxEntity.empty () ? "" : EntityValue (doc, binding.maxEntity);
    const std::string caption = binding.caption.empty () ? binding.entity : binding.caption;

    if (binding.bGauge && !maxValue.empty ())
      {
      const double current = atof (value.c_str ());
      const double top = atof (maxValue.c_str ());
      const double fraction = top > 0 ? (std::min) (1.0, (std::max) (0.0, current / top)) : 0;
      COLORREF colour = RGB (0, 160, 0);
      SetColour (binding.colour.c_str (), colour);
      doc.WindowRectOp (kBindingWindow, 2 /* fill */, 2, y, kWidth - 2, y + kRow - 2,
                        RGB (48, 48, 48), 0);
      doc.WindowRectOp (kBindingWindow, 2, 2, y, 2 + (long) ((kWidth - 4) * fraction),
                        y + kRow - 2, colour, 0);
      }

    std::string text = caption + ": " + value;
    if (!maxValue.empty ())
      text += "/" + maxValue;

    doc.WindowText (kBindingWindow, "f", text.c_str (), 5, y, kWidth - 4, y + kRow - 2,
                    RGB (240, 240, 240), doc.m_bUTF_8);
    y += kRow;
    }

  doc.WindowShow (kBindingWindow, TRUE);
  state.bBindingWindowMade = true;
  }

// ---------------------------------------------------------------- frames

void Proto_MxpFrameTag (CMUSHclientDoc & doc, const std::string & name, const std::string & actionIn,
                        const std::string & title, const bool bInternal, const std::string & align,
                        const std::string & left, const std::string & top,
                        const std::string & width, const std::string & height, const bool bScrolling)
  {
  CProtocolState & state = *doc.m_pProto;

  const std::string action = proto::Lower (actionIn);
  const std::string lower = proto::Lower (name);

  // The special names mean the main window.
  if (lower == "_top" || lower == "_previous")
    {
    if (action == "redirect")
      {
      if (lower == "_top")
        {
        state.previousRedirect = state.redirect;
        state.redirect.clear ();
        }
      else
        std::swap (state.redirect, state.previousRedirect);
      }
    return;
    }

  if (action == "close")
    {
    const auto it = state.frames.find (lower);
    if (it != state.frames.end ())
      {
      doc.WindowDelete (FrameWindowName (lower).c_str ());
      state.frames.erase (it);
      }
    if (proto::Lower (state.redirect) == lower)
      state.redirect.clear ();
    for (CProtocolState::Dest & dest : state.destStack)
      if (proto::Lower (dest.frame) == lower)
        dest.frame.clear ();
    doc.Redraw ();
    return;
    }

  CMxpFrame * pFrame = OpenFrame (doc, name);
  if (!pFrame)
    {
    doc.MXP_error (DBG_WARNING, wrnMXP_TagNotImplemented,
                   CFormat ("MXP frame \"%s\" could not be opened", name.c_str ()));
    return;
    }

  if (!title.empty ())
    pFrame->title = title;
  pFrame->bInternal = bInternal;
  pFrame->bScrolling = bScrolling;

  const int outputWidth = OutputExtent (doc, 281, 800);
  const int outputHeight = OutputExtent (doc, 280, 600);
  const int charWidth = doc.m_FontWidth > 0 ? doc.m_FontWidth : 8;
  const int charHeight = doc.m_FontHeight > 0 ? doc.m_FontHeight : 16;

  int w = proto::FrameDimension (width, outputWidth, charWidth);
  int h = proto::FrameDimension (height, outputHeight, charHeight);
  int l = proto::FrameDimension (left, outputWidth, charWidth);
  int t = proto::FrameDimension (top, outputHeight, charHeight);

  const std::string alignment = proto::Lower (align);
  pFrame->align = alignment == "left" ? 11 : alignment == "right" ? 7 :
                  alignment == "bottom" ? 9 : 5;     // spec: TOP is the default

  if (w <= 0)
    w = bInternal && (alignment == "left" || alignment == "right") ? outputWidth / 4 : outputWidth / 2;
  if (h <= 0)
    h = bInternal && (alignment == "left" || alignment == "right") ? outputHeight / 2 : charHeight * 8;

  pFrame->width = (std::max) (kMinWindow, (std::min) (w, kMaxWindow));
  pFrame->height = (std::max) (kMinWindow, (std::min) (h, kMaxWindow));

  // negative is from the far edge
  pFrame->left = l < 0 ? (std::max) (0, outputWidth + l - pFrame->width) : l;
  pFrame->top = t < 0 ? (std::max) (0, outputHeight + t - pFrame->height) : t;

  pFrame->bWindowMade = false;
  pFrame->bDirty = true;
  MarkFramesDirty (state);

  if (action == "redirect")
    {
    state.previousRedirect = state.redirect;
    state.redirect = name;
    }
  }

void Proto_MxpDestOpen (CMUSHclientDoc & doc, const std::string & frame, const std::string & x,
                        const std::string & y, const bool bEol, const bool bEof)
  {
  CProtocolState & state = *doc.m_pProto;

  CProtocolState::Dest dest;
  const std::string lower = proto::Lower (frame);
  dest.frame = (lower == "_top" || lower == "_previous" || !FindFrame (state, frame)) ? "" : frame;
  dest.bEol = bEol;
  dest.bEof = bEof;
  dest.bWrote = false;
  dest.bKeywords = bEol || bEof || !x.empty () || !y.empty ();
  dest.y = y.empty () ? -1 : atoi (y.c_str ());

  if (CMxpFrame * pFrame = dest.frame.empty () ? NULL : FindFrame (state, dest.frame))
    pFrame->MoveTo (x.empty () ? -1 : atoi (x.c_str ()), dest.y);

  state.destStack.push_back (dest);
  }

void Proto_MxpDestClose (CMUSHclientDoc & doc)
  {
  CProtocolState & state = *doc.m_pProto;

  if (state.destStack.empty ())
    return;

  const CProtocolState::Dest dest = state.destStack.back ();
  state.destStack.pop_back ();

  CMxpFrame * pFrame = dest.frame.empty () ? NULL : FindFrame (state, dest.frame);
  if (!pFrame)
    return;

  if (dest.bEof)
    pFrame->EraseToEnd ();
  else if (dest.bEol)
    pFrame->EraseToEndOfLine ();
  else if (!dest.bWrote)
    {
    // spec: Y and no text erases the line, and no text or keywords the frame
    if (dest.y >= 0)
      pFrame->EraseLine (dest.y);
    else if (!dest.bKeywords)
      pFrame->Clear ();
    }

  MarkFramesDirty (state);
  }

bool Proto_MxpTextDiverted (CMUSHclientDoc & doc)
  {
  CProtocolState & state = *doc.m_pProto;
  if (state.destStack.empty () && state.redirect.empty ())
    return false;
  return DivertTarget (state) != NULL;
  }

void Proto_MxpDivertByte (CMUSHclientDoc & doc, const unsigned char c)
  {
  CProtocolState & state = *doc.m_pProto;

  CMxpFrame * pFrame = DivertTarget (state);
  if (!pFrame)
    return;

  if (!state.destStack.empty ())
    state.destStack.back ().bWrote = true;
  MarkFramesDirty (state);

  COLORREF fore = RGB (192, 192, 192), back = RGB (0, 0, 0);
  if (doc.m_pCurrentLine && !doc.m_pCurrentLine->styleList.IsEmpty ())
    doc.GetStyleRGB (doc.m_pCurrentLine->styleList.GetTail (), fore, back);

  if (c == '\n')
    {
    // The line this byte would have ended, as far as the server is concerned.
    if (doc.m_bMXP && !doc.m_bPuebloActive)
      {
      if (!doc.MXP_Open ())
        doc.MXP_CloseOpenTags ();
      doc.MXP_mode_change (-1);
      }
    // Closing a tag above may have left the frame, or the frame the tag.
    if (CMxpFrame * pNow = DivertTarget (state))
      pNow->NewLine ();
    return;
    }

  if (c == '\t')
    {
    const size_t spaces = 8 - (pFrame->col % 8);
    for (size_t i = 0; i < spaces; i++)
      pFrame->Put (" ", 1, fore, back);
    return;
    }

  if (c < ' ' || c == 0x7F)
    return;

  // a whole character to a cell
  if (doc.m_bUTF_8 && c >= 0x80)
    {
    if (c >= 0xC0)
      state.divertSeq.clear ();
    state.divertSeq += (char) c;
    const unsigned char lead = (unsigned char) state.divertSeq [0];
    const size_t wanted = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (state.divertSeq.size () < wanted)
      return;
    pFrame->Put (state.divertSeq.data (), (int) state.divertSeq.size (), fore, back);
    state.divertSeq.clear ();
    return;
    }

  const char ch = (char) c;
  pFrame->Put (&ch, 1, fore, back);
  }

static void PaintFrame (CMUSHclientDoc & doc, CMxpFrame & frame)
  {
  frame.bDirty = false;

  const std::string window = FrameWindowName (proto::Lower (frame.name));

  if (!frame.bWindowMade)
    {
    if (frame.bInternal)
      doc.WindowCreate (window.c_str (), 0, 0, frame.width, frame.height, (short) frame.align,
                        0, RGB (0, 0, 0));
    else
      doc.WindowCreate (window.c_str (), frame.left, frame.top, frame.width, frame.height, 0,
                        MINIWINDOW_ABSOLUTE_LOCATION, RGB (0, 0, 0));
    SetUpWindowFont (doc, window.c_str ());
    frame.bWindowMade = true;
    }

  doc.WindowRectOp (window.c_str (), 2, 0, 0, 0, 0, RGB (0, 0, 0), 0);

  const long rowHeight = (std::max) (1L, InfoLong (doc.WindowFontInfo (window.c_str (), "f", 1)));

  int y = 0;
  if (!frame.title.empty ())
    {
    doc.WindowRectOp (window.c_str (), 2, 0, 0, 0, rowHeight + 1, RGB (64, 64, 96), 0);
    doc.WindowText (window.c_str (), "f", frame.title.c_str (), 3, 0, 0, rowHeight + 1,
                    RGB (240, 240, 240), doc.m_bUTF_8);
    y = (int) rowHeight + 1;
    }

  const size_t visible = (size_t) (std::max) (1L, (frame.height - y) / rowHeight);
  const size_t first = frame.row + 1 > visible ? frame.row + 1 - visible : 0;

  for (size_t r = first; r < frame.rows.size () && r < first + visible; r++, y += (int) rowHeight)
    {
    const std::vector <CMxpCell> & cells = frame.rows [r];
    long x = 2;
    for (size_t start = 0; start < cells.size (); )
      {
      size_t end = start;
      std::string run;
      while (end < cells.size () && cells [end].fore == cells [start].fore &&
             cells [end].back == cells [start].back)
        run += cells [end++].ch;
      const long width = doc.WindowTextWidth (window.c_str (), "f", run.c_str (), doc.m_bUTF_8);
      if (cells [start].back != RGB (0, 0, 0))
        doc.WindowRectOp (window.c_str (), 2, x, y, x + width, y + rowHeight, cells [start].back, 0);
      doc.WindowText (window.c_str (), "f", run.c_str (), x, y, 0, 0, cells [start].fore, doc.m_bUTF_8);
      x += width;
      start = end;
      }
    }

  doc.WindowRectOp (window.c_str (), 1 /* frame */, 0, 0, 0, 0, RGB (96, 96, 96), 0);
  doc.WindowShow (window.c_str (), TRUE);
  }

void Proto_MxpPaint (CMUSHclientDoc & doc)
  {
  CProtocolState & state = *doc.m_pProto;

  if (!state.bBindingsDirty && !state.bFramesDirty)
    return;

  if (state.bBindingsDirty)
    PaintBindings (doc);

  if (state.bFramesDirty)
    {
    state.bFramesDirty = false;
    if (doc.m_bMXPWindows)
      for (auto & item : state.frames)
        if (item.second->bDirty)
          PaintFrame (doc, *item.second);
    }

  doc.Redraw ();
  }

void Proto_MxpReset (CMUSHclientDoc & doc, const bool bClearDefinitions)
  {
  CProtocolState & state = *doc.m_pProto;

  state.bNoBreak = false;
  state.destStack.clear ();
  state.divertSeq.clear ();
  state.lineElement.clear ();
  state.redirect.clear ();
  state.previousRedirect.clear ();

  if (!bClearDefinitions)
    return;

  state.lineTag = 0;
  state.bLineTagFromEscape = false;
  state.lineTags.clear ();
  state.entities.clear ();
  state.expireOldest.clear ();
  state.bindings.clear ();
  state.bBindingsDirty = false;

  bool bHadWindows = false;
  if (state.bBindingWindowMade)
    {
    doc.WindowDelete (kBindingWindow);
    state.bBindingWindowMade = false;
    bHadWindows = true;
    }
  for (auto & item : state.frames)
    {
    if (item.second->bWindowMade)
      bHadWindows = true;
    doc.WindowDelete (FrameWindowName (item.first).c_str ());
    }
  state.frames.clear ();
  state.bFramesDirty = false;
  if (!state.imageFile.empty ())
    {
    doc.WindowDelete (kImageWindow);
    state.imageFile.clear ();
    bHadWindows = true;
    }
  if (bHadWindows && doc.m_pCurrentLine)
    doc.Redraw ();
  }

// ---------------------------------------------------------------- IMAGE

void Proto_MxpImage (CMUSHclientDoc & doc, const std::string & name, const std::string & type,
                     const std::string & align, bool * bShown)
  {
  CProtocolState & state = *doc.m_pProto;
  *bShown = false;

  if (!doc.m_bMXPWindows || name.empty () ||
      !proto::IsSafeMspName (name, false) || (!type.empty () && !proto::IsSafeMspName (type, false)))
    return;

  const std::string lower = proto::Lower (name);
  const bool bKnownKind = lower.size () > 4 &&
                          (lower.compare (lower.size () - 4, 4, ".png") == 0 ||
                           lower.compare (lower.size () - 4, 4, ".bmp") == 0);
  if (!bKnownKind)
    return;

  const std::string base = Proto_WorldCacheDirectory (doc) + "images/";
  const std::string relative = type.empty () ? name : type + "/" + name;
  const std::string path = Proto_ContainedPath (base, relative);

  std::error_code ec;
  if (path.empty () || !std::filesystem::is_regular_file (path, ec))
    return;

  doc.WindowCreate (kImageWindow, 0, 0, 16, 16, 4, 0, RGB (0, 0, 0));
  if (doc.WindowLoadImage (kImageWindow, "image", path.c_str ()) != eOK)
    return;

  const long imageWidth  = InfoLong (doc.WindowImageInfo (kImageWindow, "image", 2));
  const long imageHeight = InfoLong (doc.WindowImageInfo (kImageWindow, "image", 3));
  if (imageWidth <= 0 || imageHeight <= 0)
    return;

  const long width = (std::min) (imageWidth, 640L);
  const long height = (std::min) (imageHeight, 480L);

  const std::string alignment = proto::Lower (align);
  const short position = alignment == "right" ? 6 : alignment == "middle" ? 12 :
                         alignment == "bottom" ? 10 : 4;
  doc.WindowCreate (kImageWindow, 0, 0, width, height, position, 0, RGB (0, 0, 0));
  doc.WindowLoadImage (kImageWindow, "image", path.c_str ());
  doc.WindowDrawImage (kImageWindow, "image", 0, 0, width, height, 2 /* stretch */, 0, 0, 0, 0);
  doc.WindowShow (kImageWindow, TRUE);
  doc.Redraw ();

  state.imageFile = path;
  *bShown = true;
  }

// ---------------------------------------------------------------- RELOCATE

void Proto_MxpRelocate (CMUSHclientDoc & doc, const std::string & host, const std::string & port)
  {
  CProtocolState & state = *doc.m_pProto;

  // From here on this session has been asked to go elsewhere, and a server that does
  // that may well be about to ask for the password.  It does not get it.
  state.bRelocateAsked = true;

  const int iPort = atoi (port.c_str ());
  if (!proto::IsPlausibleHostName (host) || iPort < 1 || iPort > 65535)
    {
    doc.MXP_error (DBG_ERROR, errMXP_InvalidOptionArgument,
                   CFormat ("Bad MXP <RELOCATE>: %s %s", host.c_str (), port.c_str ()));
    return;
    }

  state.relocateHost = host;
  state.relocatePort = iPort;

  CProtocolNote note;
  note.text = (const char *) CFormat ("This server wants you to reconnect to %s, port %i.",
                                      host.c_str (), iPort);
  note.action = Proto_ActionLink ("relocate", "");
  note.linkText = CFormat ("Go to %s:%i (your password will not be sent)", host.c_str (), iPort).GetString ();
  Proto_Note (doc, note);
  }

// ---------------------------------------------------------------- the world method

// world.MXPEntityInfo: what <!ENTITY> said beyond the value.
//   1 value (NULL if the entity is PRIVATE or unknown), 2 DESC, 3 PUBLISH, 4 PRIVATE,
//   5 the names of every PUBLISHed entity, |-delimited (Name is ignored),
//   6 the GAUGE and STAT bindings, a line each: gauge|entity|max|caption|colour
VARIANT CMUSHclientDoc::MXPEntityInfo (LPCTSTR Name, short InfoType)
  {
  VARIANT vaResult;
  VariantInit (&vaResult);
  vaResult.vt = VT_NULL;

  CProtocolState & state = *m_pProto;
  const std::string name = proto::Lower (Name);
  const auto it = state.entities.find (name);

  switch (InfoType)
    {
    case 1:
      {
      if (it != state.entities.end () && it->second.bPrivate)
        break;
      CString strValue;
      if (m_CustomEntityMap.Lookup (name.c_str (), strValue))
        SetUpVariantString (vaResult, strValue);
      }
      break;
    case 2:
      if (it != state.entities.end () && !it->second.description.empty ())
        SetUpVariantString (vaResult, it->second.description.c_str ());
      break;
    case 3:
      SetUpVariantBool (vaResult, it != state.entities.end () && it->second.bPublish);
      break;
    case 4:
      SetUpVariantBool (vaResult, it != state.entities.end () && it->second.bPrivate);
      break;
    case 5:
      {
      std::string names;
      for (const auto & item : state.entities)
        if (item.second.bPublish)
          names += (names.empty () ? "" : "|") + item.first;
      SetUpVariantString (vaResult, names.c_str ());
      }
      break;
    case 6:
      {
      std::string lines;
      for (const CMxpBinding & binding : state.bindings)
        lines += std::string (binding.bGauge ? "gauge" : "stat") + "|" + binding.entity + "|" +
                 binding.maxEntity + "|" + binding.caption + "|" + binding.colour + "\n";
      SetUpVariantString (vaResult, lines.c_str ());
      }
      break;
    default:
      break;
    }

  return vaResult;
  }

// doc_scrollback_shared.cpp -- saving a world's output buffer between sessions.
//
// scrollback_store.* is the file; this is the half that knows what a CLine is.
//
// What gets saved: every finished line, in the order it appeared, with its styles
// and everything else that can be seen through GetLineInfo, as it looked after the
// triggers had finished with it.  Not the banner a session starts with, or the line
// marking where the restored text ends, because the next session prints its own.
//
// What comes back differently, on purpose:
//
//   * LOG_LINE is off.  The line was logged, if it was going to be, last time.  Opening
//     a log replays the buffer, and skips these (see OpenLog).
//   * Links that SEND something are not links any more: clicking one would send a
//     command to a game that has moved on.  They keep their look, and lose the action.
//     (ACTION_PROMPT too.)  Hyperlinks to web pages and the like still work.
//   * The high-performance time stamp means nothing after a reboot, so it is rebuilt
//     from the line's wall-clock time.  GetLineInfo (n, 13), "seconds since the world
//     started", is therefore negative for a restored line: how long before this
//     session it arrived.  GetLineInfo (n, 12) is correspondingly earlier than this
//     session's lines, and no more precise than a second.
//   * The last line gets a hard return, whatever it had: with no more text coming,
//     a new line must not be taken for its continuation by a trigger.
//
// None of this goes through DisplayMsg or the line-processing path, so no trigger,
// plugin callback or log sees a restored line arrive.

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scrollback_store.h"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char BASED_CODE THIS_FILE[] = __FILE__;
#endif

extern char working_dir [_MAX_PATH];

namespace {

// How often a world with new lines hands them over.  A crash loses at most this much
// of the buffer.  Not less, because every hand-over can become a small block.
const int kFlushSeconds = 10;

// How much to encode in one tick, so that saving a whole huge buffer in one go (when
// the option has just been turned on) spreads over several and can't stall play.
const size_t kMaxLinesPerTick = 40000;

// A file's line numbers carry on from where they got to, session after session.  They
// are `long`, which is 32 bits on Windows; a long way short of that, start again.
const int64_t kRenumberAbove = 1000000000;

// What CStyle::iFlags says about clicking on it.
bool ActionSurvivesRestore (const unsigned int iFlags)
  {
  const unsigned int iAction = iFlags & ACTIONTYPE;
  return iAction == ACTION_NONE || iAction == ACTION_HYPERLINK;
  }

} // anonymous namespace

class CScrollbackSession
  {
  public:
  std::unique_ptr<scrollback::Writer> writer;
  long   savedThrough = 0;      // the last line number handed to the writer
  bool   bUtf8 = false;
  time_t lastFlush = 0;
  long   liveFrom = 0;          // what the writer was last told about the buffer's head
  bool   bReportedError = false;
  bool   bRenumbered = false;
  };


CString CMUSHclientDoc::ScrollbackFilePath (void) const
  {
  // An ID is hexadecimal when we make it, but a world file can say anything.
  CString strID;
  for (int i = 0; i < m_strWorldID.GetLength (); i++)
    {
    const char c = m_strWorldID [i];
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_')
      strID += c;
    }
  if (strID.IsEmpty ())
    return CString ();

  CString strPath (working_dir);
#ifdef _WIN32
  strPath += "scrollback\\";
#else
  strPath += "scrollback/";
#endif
  strPath += strID;
  strPath += ".mcsb";
  return strPath;
  }


CString CMUSHclientDoc::CommandHistoryFilePath (void) const
  {
  CString strPath = ScrollbackFilePath ();
  if (!strPath.IsEmpty ())
    {
    strPath = strPath.Left (strPath.GetLength () - 5);    // ".mcsb"
    strPath += ".history";
    }
  return strPath;
  } // end of CMUSHclientDoc::CommandHistoryFilePath


bool CMUSHclientDoc::RestoreSavedScrollback (void)
  {
  if (!m_bSaveScrollback)
    {
    m_bScrollbackWanted = true;    // ScrollbackTick looks after it going on later
    return false;
    }
  if (m_pScrollback)
    return false;

  const CString strPath = ScrollbackFilePath ();
  if (strPath.IsEmpty ())
    return false;

  // Its saved output is filed under its ID, so the ID has to reach the world file.
  if (m_bWorldIDIsNew)
    {
    m_bWorldIDIsNew = false;
    if (App.m_bAutosaveWorlds)
      SetModifiedFlag ();
    }

  // The cap leaves room for the session's first lines, because evicting a chunk
  // before the views are up would be a pointless way to start.
  long iPixelRows = (long) (INT_MAX / (m_FontHeight > 0 ? m_FontHeight : 1)) - 2 * JUMP_SIZE;
  long iMaxLines = iPixelRows;
  if (m_maxlines > 0 && m_maxlines - JUMP_SIZE < iMaxLines)
    iMaxLines = m_maxlines - JUMP_SIZE;
  if (iMaxLines < 1)
    iMaxLines = 1;

  scrollback::LoadResult load = scrollback::Load ((const char *) strPath, m_bUTF_8 != 0, (size_t) iMaxLines);

  if (load.status == scrollback::LoadStatus::NewerVersion ||
      load.status == scrollback::LoadStatus::CouldNotRead)
    return false;     // not ours to touch, and not to be appended to

  // Build the lines.  The blocks are independent, and building a line touches nothing
  // shared but the heap, so this goes across as many threads as Load's decoding did.
  const size_t iBlocks = load.blocks.size ();
  std::vector<std::vector<CLine *>> built (iBlocks);
  struct PendingAction
    {
    CStyle * pStyle;
    const scrollback::Action * pAction;
    };
  std::vector<std::vector<PendingAction>> actions (iBlocks);

  const time_t tStarted = m_whenWorldStarted.GetTime ();
  const LONGLONG iStartedHP = m_whenWorldStartedHighPrecision.QuadPart;
  const LONGLONG iFrequency = App.m_iCounterFrequency;

  bool bRenumber = !load.blocks.empty () && load.blocks.back ()->lines.back ().number > kRenumberAbove;
  long iNextNumber = 1;
  std::vector<long> firstNumbers (iBlocks);
  for (size_t b = 0; b < iBlocks; b++)
    {
    firstNumbers [b] = iNextNumber;
    iNextNumber += (long) load.blocks [b]->lines.size () - (b == 0 ? (long) load.firstLineInFirstBlock : 0);
    }

  std::atomic<size_t> next (0);
  auto buildBlocks = [&] ()
    {
    for (;;)
      {
      const size_t b = next.fetch_add (1);
      if (b >= iBlocks)
        return;
      const scrollback::DecodedBlock & block = *load.blocks [b];
      const size_t iFirst = b == 0 ? load.firstLineInFirstBlock : 0;
      std::vector<CLine *> & lines = built [b];
      lines.reserve (block.lines.size () - iFirst);
      long iNumber = firstNumbers [b];
      for (size_t i = iFirst; i < block.lines.size (); i++)
        {
        const scrollback::Line & line = block.lines [i];
        CLine * pLine = new CLine (bRenumber ? iNumber++ : (long) line.number,
                                   CTime ((time_t) line.time),
                                   iStartedHP + (LONGLONG) (line.time - tStarted) * iFrequency,
                                   (unsigned char) (line.flags & ~LOG_LINE),
                                   line.hardReturn,
                                   line.charCount,
                                   (short) line.preamble,
                                   block.text + line.textOffset,
                                   (int) line.length);
        for (uint32_t s = 0; s < line.styleCount; s++)
          {
          const scrollback::Style & saved = block.styles [line.firstStyle + s];
          CStyle * pStyle = new CStyle;
          pStyle->iLength = (unsigned short) saved.length;
          pStyle->iFlags = (unsigned short) saved.flags;
          pStyle->iForeColour = saved.fore;
          pStyle->iBackColour = saved.back;
          if (!ActionSurvivesRestore (saved.flags))
            pStyle->iFlags &= ~ACTIONTYPE;
          else if (saved.action >= 0)
            actions [b].push_back (PendingAction { pStyle, &block.actions [(size_t) saved.action] });
          pLine->styleList.AddTail (pStyle);
          }
        lines.push_back (pLine);
        }
      }
    };

  unsigned iThreads = (std::min) ((std::max) (std::thread::hardware_concurrency (), 1u), 8u);
  if (iBlocks < iThreads)
    iThreads = iBlocks > 0 ? (unsigned) iBlocks : 1;
  std::vector<std::thread> pool;
  for (unsigned t = 1; t < iThreads; t++)
    pool.emplace_back (buildBlocks);
  buildBlocks ();
  for (std::thread & t : pool)
    t.join ();

  // Actions are the document's, and shared between styles.
  for (const std::vector<PendingAction> & blockActions : actions)
    for (const PendingAction & a : blockActions)
      a.pStyle->pAction = GetAction (a.pAction->action.c_str (), a.pAction->hint.c_str (), a.pAction->variable.c_str ());

  // Into the buffer.
  m_LinePositions.BeginRebuild ();
  long iCount = 0;
  for (const std::vector<CLine *> & lines : built)
    for (CLine * pLine : lines)
      {
      const POSITION pos = m_LineList.AddTail (pLine);
      if (iCount % JUMP_SIZE == 0)
        m_LinePositions.Append (pos);
      iCount++;
      }

  if (iCount > 0)
    {
    CLine * pLast = m_LineList.GetTail ();
    pLast->hard_return = true;
    m_total_lines = pLast->m_nLineNumber;
    m_nLastRestoredLine = pLast->m_nLineNumber;
    }
  else
    m_LinePositions.Reset (m_LineList.GetHeadPosition ());

  // And carry on writing.  A second copy of the client with this world open means no
  // writer: what it restored is still worth seeing, but it can't save.
  std::unique_ptr<scrollback::Writer> writer (new scrollback::Writer ((const char *) strPath, m_bUTF_8 != 0, load));
  if (writer->IsOpen ())
    {
    m_pScrollback = new CScrollbackSession;
    m_pScrollback->writer = std::move (writer);
    m_pScrollback->bUtf8 = m_bUTF_8 != 0;
    m_pScrollback->lastFlush = time (NULL);
    m_pScrollback->bRenumbered = bRenumber;
    if (bRenumber)
      m_pScrollback->writer->Clear (m_bUTF_8 != 0);  // they'll all go again under their new numbers
    else
      m_pScrollback->savedThrough = m_total_lines;
    }

  return iCount > 0;
  } // end of CMUSHclientDoc::RestoreSavedScrollback


void CMUSHclientDoc::ScrollbackSessionStarted (void)
  {
  // m_pCurrentLine is the empty line the banner left for what comes next: everything
  // before it is done with.  If the restored lines were renumbered they all still have
  // to go out again, and one session's banner goes with them, which is no great harm.
  if (m_pScrollback && !m_pScrollback->bRenumbered && m_pCurrentLine)
    m_pScrollback->savedThrough = m_pCurrentLine->m_nLineNumber - 1;
  } // end of CMUSHclientDoc::ScrollbackSessionStarted


void CMUSHclientDoc::ScrollbackTick (void)
  {
  // The option has been turned on (World Properties, or SetOption).
  if (!m_pScrollback)
    {
    if (m_bSaveScrollback && m_LinePositions.IsAllocated () && m_bScrollbackWanted)
      {
      m_bScrollbackWanted = false;
      const CString strPath = ScrollbackFilePath ();
      if (!strPath.IsEmpty ())
        {
        scrollback::LoadResult none;    // anything already there is replaced
        std::unique_ptr<scrollback::Writer> writer (new scrollback::Writer ((const char *) strPath, m_bUTF_8 != 0, none));
        if (writer->IsOpen ())
          {
          m_pScrollback = new CScrollbackSession;
          m_pScrollback->writer = std::move (writer);
          m_pScrollback->bUtf8 = m_bUTF_8 != 0;
          }
        }
      }
    return;
    }

  // Turned off: stop, and don't leave what was saved lying around.
  if (!m_bSaveScrollback)
    {
    m_pScrollback->writer->Close (true);
    delete m_pScrollback;
    m_pScrollback = NULL;
    m_bScrollbackWanted = true;
    return;
    }

  // The UTF-8 option changed under us.  The lines in the buffer are in the old encoding
  // and the file's header says so; start again from here.
  if ((m_bUTF_8 != 0) != m_pScrollback->bUtf8)
    {
    m_pScrollback->bUtf8 = m_bUTF_8 != 0;
    m_pScrollback->writer->Clear (m_pScrollback->bUtf8);
    m_pScrollback->savedThrough = m_pCurrentLine ? m_pCurrentLine->m_nLineNumber - 1 : m_total_lines;
    return;
    }

  // Nothing new, which on an idle world is every time.
  const long iPending = m_total_lines - m_pScrollback->savedThrough;
  if (iPending <= 1)     // the line in progress doesn't count
    return;

  const time_t tNow = time (NULL);
  if (iPending < (long) scrollback::kBlockLines && tNow - m_pScrollback->lastFlush < kFlushSeconds)
    return;

  FlushSavedScrollback (false);
  m_pScrollback->lastFlush = tNow;
  } // end of CMUSHclientDoc::ScrollbackTick


void CMUSHclientDoc::FlushSavedScrollback (const bool bClosing)
  {
  if (!m_pScrollback)
    return;

  CScrollbackSession & session = *m_pScrollback;

  // Find what hasn't been saved, walking back from the end: the first line of it, and
  // the last line that is finished with.  A wrapped line is not finished until its
  // last row is, because a trigger can still omit the lot.  Closing, everything counts.
  POSITION posFirst = NULL;
  POSITION posLast = NULL;
  for (POSITION pos = m_LineList.GetTailPosition (); pos; )
    {
    const POSITION here = pos;
    const CLine * pLine = m_LineList.GetPrev (pos);
    if (pLine->m_nLineNumber <= session.savedThrough)
      break;
    if (!posLast &&
        (bClosing ? (pLine != m_pCurrentLine || pLine->len > 0)
                  : (pLine != m_pCurrentLine && pLine->hard_return)))
      posLast = here;
    posFirst = here;
    }

  if (posLast)
    {
    scrollback::BlockBuilder builder;
    const CAction * pLastAction = NULL;
    int32_t iLastAction = -1;
    size_t iTotal = 0;

    auto handOver = [&] ()
      {
      const int64_t iFirstNumber = builder.FirstNumber ();
      const int64_t iLastNumber = builder.LastNumber ();
      const size_t iLines = builder.LineCount ();
      session.writer->AppendLines (builder.Finish (), iLines, iFirstNumber, iLastNumber);
      session.savedThrough = (long) iLastNumber;
      pLastAction = NULL;     // the action table went with the block
      };

    for (POSITION pos = posFirst; pos; )
      {
      const POSITION here = pos;
      const CLine * pLine = m_LineList.GetNext (pos);

      builder.AddLine (pLine->m_nLineNumber, pLine->m_theTime.GetTime (), pLine->flags, pLine->hard_return,
                       pLine->m_iCharCount, pLine->m_iPreambleOffset, pLine->text, (size_t) pLine->len);

      for (POSITION stylepos = pLine->styleList.GetHeadPosition (); stylepos; )
        {
        const CStyle * pStyle = pLine->styleList.GetNext (stylepos);
        int32_t iAction = -1;
        // Actions that won't be restored needn't be kept, and at least one of them
        // (a send link) is liable to be a command someone wouldn't want lying around.
        if (pStyle->pAction && ActionSurvivesRestore (pStyle->iFlags))
          {
          if (pStyle->pAction != pLastAction)
            {
            pLastAction = pStyle->pAction;
            iLastAction = builder.AddAction ((const char *) pLastAction->m_strAction,
                                             (const char *) pLastAction->m_strHint,
                                             (const char *) pLastAction->m_strVariable);
            }
          iAction = iLastAction;
          }
        builder.AddStyle (pStyle->iLength, pStyle->iFlags, pStyle->iForeColour, pStyle->iBackColour, iAction);
        }

      iTotal++;
      if (builder.LineCount () >= scrollback::kBlockLines)
        handOver ();
      if (here == posLast || (!bClosing && iTotal >= kMaxLinesPerTick))
        break;
      }

    if (builder.LineCount () > 0)
      handOver ();
    }

  // What the buffer no longer holds needn't stay in the file.
  if (!m_LineList.IsEmpty ())
    {
    const long iHead = m_LineList.GetHead ()->m_nLineNumber;
    if (iHead != session.liveFrom)
      {
      session.liveFrom = iHead;
      session.writer->SetLiveFrom (iHead);
      }
    }

  if (bClosing)
    {
    session.writer->Flush ();
    // A world that has never been saved has no way to find this again, unless it is
    // saved after all (at which point this hasn't happened yet: this is the close).
    const bool bForget = m_strPathName.IsEmpty () || !m_bSaveScrollback;
    session.writer->Close (bForget);
    delete m_pScrollback;
    m_pScrollback = NULL;
    return;
    }

  // No dialog from here, ever: this is a timer.  Once, in the window.
  if (!session.bReportedError)
    {
    const std::string strError = session.writer->LastError ();
    if (!strError.empty ())
      {
      session.bReportedError = true;
      Note (TFormat ("Saved output buffer: %s", strError.c_str ()));
      }
    }
  } // end of CMUSHclientDoc::FlushSavedScrollback


void CMUSHclientDoc::ScrollbackTailDeleted (void)
  {
  // Numbers get reused after this, so it can't wait for the next flush to notice.
  const long iLast = m_LineList.IsEmpty () ? 0 : m_LineList.GetTail ()->m_nLineNumber;
  if (m_nLastRestoredLine > iLast)
    m_nLastRestoredLine = iLast;
  if (m_pScrollback && m_pScrollback->savedThrough > iLast)
    {
    m_pScrollback->writer->TruncateFrom ((int64_t) iLast + 1);
    m_pScrollback->savedThrough = iLast;
    }
  } // end of CMUSHclientDoc::ScrollbackTailDeleted


void CMUSHclientDoc::ScrollbackHeadDeleted (void)
  {
  if (m_pScrollback && !m_LineList.IsEmpty ())
    {
    const long iHead = m_LineList.GetHead ()->m_nLineNumber;
    m_pScrollback->liveFrom = iHead;
    m_pScrollback->writer->DropBefore (iHead);
    }
  } // end of CMUSHclientDoc::ScrollbackHeadDeleted


void CMUSHclientDoc::ScrollbackCleared (void)
  {
  m_nLastRestoredLine = 0;
  if (m_pScrollback)
    {
    m_pScrollback->writer->Clear (m_pScrollback->bUtf8);
    m_pScrollback->savedThrough = 0;
    m_pScrollback->liveFrom = 0;
    }
  } // end of CMUSHclientDoc::ScrollbackCleared


void CMUSHclientDoc::ScrollbackLineFlagsChanged (const CLine * pLine)
  {
  if (m_pScrollback && pLine->m_nLineNumber <= m_pScrollback->savedThrough)
    m_pScrollback->writer->SetFlags (pLine->m_nLineNumber, pLine->flags);
  } // end of CMUSHclientDoc::ScrollbackLineFlagsChanged


void CMUSHclientDoc::ClearSavedScrollback (void)
  {
  if (m_pScrollback)
    {
    m_pScrollback->writer->Clear (m_pScrollback->bUtf8);
    m_pScrollback->writer->Flush ();
    // Only what arrives from now on: what is on screen was just asked to be forgotten.
    m_pScrollback->savedThrough = m_pCurrentLine ? m_pCurrentLine->m_nLineNumber - 1 : m_total_lines;
    m_pScrollback->liveFrom = 0;
    return;
    }
  const CString strPath = ScrollbackFilePath ();
  if (!strPath.IsEmpty ())
    {
    scrollback::LoadResult none;
    scrollback::Writer writer ((const char *) strPath, m_bUTF_8 != 0, none);
    writer.Close (true);
    }
  } // end of CMUSHclientDoc::ClearSavedScrollback

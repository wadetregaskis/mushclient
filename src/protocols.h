// protocols.h -- what the client does natively for the MUD protocols beyond telnet
// itself: NEW-ENVIRON (MNES), MSSP's TLS hint, OSC 8 links, the newer MXP tags, MSP,
// and the GMCP helper.
//
// Three layers:
//
//   namespace proto          Pure logic, on std::string.  No document, no MFC.  What
//                            can be got wrong by a hostile server lives here, where
//                            a test can reach it: path validation, parsers.
//   CProtocolState           The per-world state.  CMUSHclientDoc::m_pProto owns one,
//                            which is why it is here and not in doc.h: changing this
//                            does not rebuild the whole client.
//   Proto_* free functions   The glue to the document, in protocols_telnet.cpp,
//                            protocols_mxp.cpp, msp.cpp and gmcp.cpp.
//
// docs/PROTOCOLS.md describes what a plugin author or a player sees.

#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

class CMUSHclientDoc;

namespace proto {

// ---------------------------------------------------------------- telnet

const unsigned char kTelOptNewEnviron = 39;
const unsigned char kTelOptMssp       = 70;
const unsigned char kTelOptMsp        = 90;
const unsigned char kTelOptGmcp       = 201;

// A server stuck in a loop can toggle an option for ever, and answering each toggle
// feeds it.  Ten negotiations of one option in a minute is more than any real server
// needs, so past that the option's negotiation is ignored for ten seconds.
class NegotiationGuard
  {
  public:
    static const int kMaxPerWindow = 10;
    static const uint64_t kWindowMs = 60000;
    static const uint64_t kMuteMs   = 10000;

    NegotiationGuard () { Reset (); }

    // False if this negotiation of the option should be ignored.
    bool Allow (unsigned char option, uint64_t nowMs);
    void Reset ();

  private:
    struct Slot
      {
      uint64_t windowStart;
      uint64_t mutedUntil;
      int count;
      };
    Slot m_slots [256];
  };

// CHARSET (RFC 2066).  `offer` is what follows the REQUEST byte: a delimiter and then
// names.  `acceptable` is what this world can honestly agree to, in no particular
// order; the server's order decides.  Returns the name as the server spelled it,
// or "" to reject.  Compared without regard to case.
std::string ChooseCharset (const std::string & offer,
                           const std::vector <std::string> & acceptable);

// The names a world can accept for CHARSET, given its encoding: UTF-8 or a code page.
std::vector <std::string> CharsetNames (bool bUtf8, unsigned int codePage);

// What CHARSET in NEW-ENVIRON says for that encoding.
std::string MnesCharsetName (bool bUtf8, unsigned int codePage);

// NEW-ENVIRON (RFC 1572), as MNES uses it.  `request` is the subnegotiation's data,
// starting with SEND.  `vars` holds what we are prepared to say, in order.  Returns
// the data to send, starting with IS, or "" if the request is not a SEND.  A requested
// variable we do not have is answered with its name and no value, which is how RFC
// 1572 says "not defined".  IPADDRESS is never in `vars`, so that is its answer too.
typedef std::vector <std::pair <std::string, std::string> > EnvVars;
std::string BuildEnvironReply (const std::string & request, const EnvVars & vars);

// MSSP: the port an MSSP subnegotiation advertises for TLS, from the variable SSL or
// TLS, or 0.  (Both spellings are in the wild; 0 and 1 are "no" and "yes", not ports.)
int FindMsspTlsPort (const std::string & data);

// ---------------------------------------------------------------- OSC 8

const size_t kOsc8MaxPayload = 4096;       // whole of 8;params;URI
const size_t kOsc8MaxParams  = 256;
const size_t kOscMaxPayload  = 64 * 1024;  // any other OSC, which we do not keep

enum OscLinkKind
  {
  eOscLinkNone,      // not a link we allow: its text stays plain
  eOscLinkUrl,       // http or https
  eOscLinkSend,      // send:command
  eOscLinkPrompt,    // prompt:command
  };

// What a link's target means, and for send: and prompt: the decoded command.  Those
// two are refused if they decode to anything that could be more than one visible
// command: a control character, or the | that MXP treats as a menu delimiter.
OscLinkKind ClassifyOsc8Target (const std::string & uri, std::string * action);

// ---------------------------------------------------------------- MXP

// A name in <!ENTITY ... ADD|REMOVE> is an item in a |-delimited list.
std::string EntityListAdd (const std::string & list, const std::string & item);
std::string EntityListRemove (const std::string & list, const std::string & item);

// FLAG's name for an automapper tag, and the reverse.  0 and "" if it has none.
int LineTagForFlag (const std::string & lowercaseFlag);
std::string FlagForLineTag (int tag);

// A FRAME dimension: pixels, "50%" or "20c", negative meaning from the far edge.
int FrameDimension (const std::string & text, int total, int charSize);

// Encode a code point as UTF-8; "" if it is not a character.
std::string EncodeUtf8 (unsigned long codePoint);

// A host name or address fit to show the player and connect to.
bool IsPlausibleHostName (const std::string & host);

// ---------------------------------------------------------------- MSP

const size_t kMspMaxTrigger = 1024;        // !!SOUND(...) longer than this is text

struct MspTrigger
  {
  bool bMusic;
  bool bOff;
  std::string name;
  int volume;       // V= 0 to 100
  int repeats;      // L= -1 for ever, or >= 1
  int priority;     // P= 0 to 100, sounds
  int cont;         // C= 0 or 1, music
  std::string type; // T=
  std::string url;  // U=

  MspTrigger () : bMusic (false), bOff (false), volume (100), repeats (1),
                  priority (50), cont (1) { }
  };

// `inner` is what is between the parentheses.  False if it has no file name.
bool ParseMspTrigger (const std::string & inner, bool bMusic, MspTrigger * out);

// A name the server asked for, relative to a sound directory, using /.  This is the
// wall between the server and the file system, for reading as well as writing.
// Refuses absolute paths, drive letters, .. and ., backslashes, colons (NTFS streams),
// device names on any platform (a world file travels), trailing dots and spaces,
// control characters and anything not plain ASCII.  With bWildcards, * and ? may be
// in the last component, and only one *.
bool IsSafeMspName (const std::string & name, bool bWildcards);

bool MspWildcardMatch (const std::string & pattern, const std::string & name);

// fName with the default extension if it has none: .wav for sound, .mid for music.
std::string MspWithDefaultExtension (const std::string & name, bool bMusic);

bool IsMidiName (const std::string & name);
bool IsDownloadableAudioName (const std::string & name);    // wav mp3 flac

// Does this look like what its extension says?  The file is refused if not, so what
// the decoders get to chew on is at least of the kind they expect.
bool LooksLikeAudio (const std::string & name, const std::string & data);

// The volume PlaySound wants, in decibels, for MSP's 0 to 100.
double MspVolumeToDb (int volume);

// The URL to download a name from, for U=.  "" unless `base` is http or https.
std::string MspDownloadUrl (const std::string & base, const std::string & name);

// The host in an http or https URL, lower case; "" if it has none.
std::string UrlHost (const std::string & url);

// Lists of hosts, as msp_allowed_hosts keeps them: space-separated, lower case.
bool HostListHas (const std::string & list, const std::string & host);
std::string HostListAdd (const std::string & list, const std::string & host);

// ---------------------------------------------------------------- GMCP

std::string Lower (std::string s);

bool IsValidGmcpPackage (const std::string & package);

// Split a message at the first white space.  rawText is what follows, trimmed.
void SplitGmcp (const std::string & data, std::string * package, std::string * rawText);

// Does a subscription to `subscribed` cover a message for `package`?  Exact, or a
// parent module (Char covers Char.Vitals), without regard to case.
bool GmcpPackageMatches (const std::string & subscribed, const std::string & package);

// IAC SB GMCP ... IAC SE, with any IAC in the text doubled.
std::string BuildGmcpPacket (const std::string & text);

// Does this outgoing data send a GMCP Core.Supports.Set?
bool SendsGmcpSupportsSet (const char * data, size_t length);

// Core.Supports.Add or .Remove, for "name version" items, in the JSON servers expect.
std::string BuildGmcpSupports (const std::string & verb, const std::vector <std::string> & items);

} // namespace proto

// ---------------------------------------------------------------- per-world state

// One text cell in a frame.  A frame is a grid so that <DEST x y> can address it.
struct CMxpCell
  {
  char ch [5];         // one character, in the world's encoding
  unsigned long fore;
  unsigned long back;
  };

class CMxpFrame
  {
  public:
    static const size_t kMaxRows = 200;
    static const size_t kMaxCols = 256;

    std::string name;
    std::string title;
    bool bInternal;
    bool bScrolling;
    int left, top, width, height;       // pixels, resolved when opened
    int align;                          // miniwindow position, for internal frames
    bool bDirty;
    bool bWindowMade;
    std::vector <std::vector <CMxpCell> > rows;
    size_t row, col;                    // the cursor

    CMxpFrame ();
    void Put (const char * ch, int len, unsigned long fore, unsigned long back);
    void NewLine ();
    void MoveTo (int x, int y);
    void EraseToEndOfLine ();
    void EraseToEnd ();
    void EraseLine (int y);
    void Clear ();
    std::string RowText (size_t r) const;       // for tests and for plugins
  };

struct CMxpLineTag
  {
  bool bEnabled;
  bool bGag;
  bool bHasFore, bHasBack;
  unsigned long fore, back;
  std::string window;
  CMxpLineTag () : bEnabled (true), bGag (false), bHasFore (false), bHasBack (false),
                   fore (0), back (0) { }
  };

struct CMxpEntityInfo
  {
  std::string description;
  bool bPrivate;
  bool bPublish;
  CMxpEntityInfo () : bPrivate (false), bPublish (false) { }
  };

struct CMxpBinding
  {
  bool bGauge;              // otherwise STAT
  std::string entity;
  std::string maxEntity;
  std::string caption;
  std::string colour;
  };

// A note the protocol code wants in the output window, waiting until the current
// line is finished: nothing may be written into the middle of the server's line.
struct CProtocolNote
  {
  std::string text;
  std::string action;       // a link after the text, if any; runs on click
  std::string linkText;
  std::string action2;      // and a second
  std::string linkText2;
  };

// A sound a voice is playing.
struct CMspVoice
  {
  bool bActive;
  int buffer;               // 1 to MAX_SOUND_BUFFERS
  std::string file;         // the full path playing
  std::string pattern;      // what was asked for, for a new pick on each repeat
  std::string type;
  int priority;
  int repeatsLeft;          // -1 for ever
  int volume;
  bool bLooping;            // DirectSound or miniaudio is doing the repeats itself
  CMspVoice () : bActive (false), buffer (0), priority (0), repeatsLeft (0), volume (100),
                 bLooping (false) { }
  };

// What a finished download reports back.  Shared with its worker, which may outlive
// the world.
struct CMspDownloads
  {
  struct Result
    {
    std::string name;       // what was asked for
    std::string error;      // "" if it is saved
    };
  std::mutex lock;
  std::deque <Result> done;
  };

struct CGmcpSubscription
  {
  std::string package;      // as first spelled: servers can care
  std::string version;
  std::set <std::string> owners;    // plugin IDs; "" is the world's own script
  };

class CProtocolState
  {
  public:
    CProtocolState ();

    // Now, in milliseconds.  Tests can set the clock.
    uint64_t Now () const;
    uint64_t testNow;

    // --- a new connection
    void ResetForConnection ();

    // --- telnet
    proto::NegotiationGuard guard;
    bool bGuardNoted;
    uint64_t lastSentDo [256];     // when a plugin handshake last ran, by option
    uint64_t lastSentWill [256];

    // --- notes
    std::vector <CProtocolNote> notes;
    bool bFlushingNotes;

    // --- MNES, MSSP
    bool bNewEnviron;
    bool bTlsOffered;
    bool reconnectAfterClose;    // the player clicked to move this connection

    // --- OSC
    std::string osc;
    size_t oscCount;
    bool bOscReprocess;    // Phase_OSC gave up: the byte is ordinary text
    bool bLinkOpen;
    unsigned short linkSavedFlags;
    unsigned long linkSavedFore, linkSavedBack, linkFore, linkBack;
    bool bLinkColoured;

    // --- MXP
    int lineTag;                 // the tag this line has, for its completion
    bool bLineTagFromEscape;     // the server said ESC [ n z, as opposed to a FLAG
    std::string lineElement;     // the element a line tag opened, closed at newline
    std::map <int, CMxpLineTag> lineTags;
    bool bNoBreak;               // <NOBR>
    std::string openExpire;      // expire= of the tag being opened
    std::map <std::string, long> expireOldest;   // expire name: oldest line that has one
    std::map <std::string, CMxpEntityInfo> entities;
    std::vector <CMxpBinding> bindings;
    bool bBindingsDirty;
    bool bBindingWindowMade;
    std::map <std::string, std::shared_ptr <CMxpFrame> > frames;
    struct Dest
      {
      std::string frame;         // "" means the main window, however it was named
      bool bEol, bEof, bWrote, bKeywords;
      int y;
      };
    std::vector <Dest> destStack;      // the open <DEST> tags
    std::string divertSeq;       // a UTF-8 character on its way to a frame
    std::string redirect;        // the frame ACTION=REDIRECT sends everything to
    std::string previousRedirect;
    bool bFramesDirty;
    std::string imageFile;       // what the image window shows

    // --- RELOCATE
    bool bRelocateAsked;         // this session has been asked to move: no <USER>, <PASSWORD>
    std::string relocateHost;
    int relocatePort;
    bool bRelocated;             // this connection went where a server said
    std::string relocateOriginalServer;
    unsigned short relocateOriginalPort;

    // --- MSP
    std::string mspBuffer;
    bool bMspReplaying;
    bool bMspNegotiated;         // we said DO, and handle it ourselves
    bool bMspPluginClaimed;      // a plugin said it would
    CMspVoice sound;
    CMspVoice music;
    std::string mspDefaultUrl;
    bool bMidiNoted;
    bool bMspBadNameNoted;
    bool bMspDownloadsOffNoted;
    std::unique_ptr <proto::MspTrigger> mspPendingMusic;   // what to play when it arrives
    std::set <std::string> mspHostsAsked;      // told the player about, this session
    std::vector <std::pair <std::string, proto::MspTrigger> > mspWaiting;  // host, trigger
    std::set <std::string> mspInFlight;
    std::set <std::string> mspFailed;          // do not keep trying these
    std::shared_ptr <CMspDownloads> downloads;

    // Tests have no sound card.  With this set the player records what it would do.
    bool bTestSound;
    std::vector <std::string> testPlayed;
    bool testBusy [11];

    // --- GMCP
    std::map <std::string, CGmcpSubscription> gmcpSubs;     // by lower-case package
    std::map <std::string, std::string> gmcpLast;           // by lower-case package
    bool bGmcpWillSeen;          // the server offered it, this connection
    bool bGmcpPluginClaimed;     // a plugin said yes: Core.Hello is its business
    bool bGmcpCoreHandshook;
    bool bGmcpInSentDoChain;     // plugin handshakes are running: hold Add until after
    bool bGmcpResendAdd;         // something we saw wants Add sent again
    bool bGmcpSendingOurs;
  };

// ---------------------------------------------------------------- telnet glue
// protocols_telnet.cpp

// Queue a note, or write it now if the current line is empty.  The link, if any, is
// clicked to run an action through Proto_RunAction, and only a note line can do that.
void Proto_Note (CMUSHclientDoc & doc, const CProtocolNote & note);
void Proto_Note (CMUSHclientDoc & doc, const std::string & text);
void Proto_FlushNotes (CMUSHclientDoc & doc);

// The hyperlink action for one of the client's own links.
std::string Proto_ActionLink (const char * verb, const std::string & arg);
extern const char * const kProtoActionID;     // the "plugin ID" in those links

// A click on one of those links.  False if the verb is not known.
bool Proto_RunAction (CMUSHclientDoc & doc, const CString & verb, const CString & arg);

// From the timer: repeats, finished downloads, a reconnection the player asked for.
void Proto_Tick (CMUSHclientDoc & doc);
void Proto_ConnectionClosed (CMUSHclientDoc & doc);

bool Proto_NegotiationAllowed (CMUSHclientDoc & doc, unsigned char option);

// Should a plugin handshake run again for this repeated request?  See telnet_phases.cpp.
bool Proto_ShouldRerunHandshake (CProtocolState & state, uint64_t * lastRun);

int Proto_MttsBits (CMUSHclientDoc & doc);
void Proto_HandleNewEnviron (CMUSHclientDoc & doc);
void Proto_HandleMssp (CMUSHclientDoc & doc);
std::vector <std::string> Proto_WorldCharsetNames (CMUSHclientDoc & doc);

// Phase_OSC's half, and the rest.
void Proto_OscByte (CMUSHclientDoc & doc, unsigned char c);
void Proto_OscInterrupted (CMUSHclientDoc & doc, unsigned char c);
void Proto_OscCloseLink (CMUSHclientDoc & doc);

// ---------------------------------------------------------------- MXP glue
// protocols_mxp.cpp

// ESC [ n z for n from 10 to 99.
void Proto_MxpLineTag (CMUSHclientDoc & doc, int tag);
// Called when a \n arrives, before the line is ended.
void Proto_MxpEndOfLine (CMUSHclientDoc & doc);
// Called from ProcessPreviousLine with the whole line.  firstLine is its first segment.
// True if it should go from the output window.
bool Proto_MxpLineComplete (CMUSHclientDoc & doc, const CString & strLine, POSITION firstLine);
// A FLAG element closed.
void Proto_MxpFlagClosed (CMUSHclientDoc & doc, const CString & strFlag, const CString & strText);

void Proto_MxpTagDefinition (CMUSHclientDoc & doc, const CString & strIndex, CString strArgs);
void Proto_MxpExpire (CMUSHclientDoc & doc, const CString & strName);
void Proto_MxpNoteExpiringLink (CMUSHclientDoc & doc);
void Proto_MxpEntityChanged (CMUSHclientDoc & doc);
void Proto_MxpReset (CMUSHclientDoc & doc, bool bClearDefinitions);

void Proto_MxpBinding (CMUSHclientDoc & doc, bool bGauge, const std::string & entity,
                       const std::string & maxEntity, const std::string & caption,
                       const std::string & colour);
void Proto_MxpFrameTag (CMUSHclientDoc & doc, const std::string & name, const std::string & action,
                        const std::string & title, bool bInternal, const std::string & align,
                        const std::string & left, const std::string & top,
                        const std::string & width, const std::string & height, bool bScrolling);
void Proto_MxpDestOpen (CMUSHclientDoc & doc, const std::string & frame, const std::string & x,
                        const std::string & y, bool bEol, bool bEof);
void Proto_MxpDestClose (CMUSHclientDoc & doc);
void Proto_MxpImage (CMUSHclientDoc & doc, const std::string & name, const std::string & type,
                     const std::string & align, bool * bShown);
void Proto_MxpRelocate (CMUSHclientDoc & doc, const std::string & host, const std::string & port);

// Is text going to a frame?  Then DisplayMsg gives it here instead of the line.
bool Proto_MxpTextDiverted (CMUSHclientDoc & doc);
void Proto_MxpDivertByte (CMUSHclientDoc & doc, unsigned char c);

// Paint what changed.  At the end of a packet, so a burst paints once.
void Proto_MxpPaint (CMUSHclientDoc & doc);

// ---------------------------------------------------------------- MSP glue
// msp.cpp

// Is the client itself parsing !!SOUND lines right now?
bool Proto_MspParsing (CMUSHclientDoc & doc);
// Option 90, after the plugins have said no.
bool Proto_MspWillAccept (CMUSHclientDoc & doc);
bool Proto_MspPluginLoaded (CMUSHclientDoc & doc);
// Bytes in phases HAVE_MSP_TRIGGER and HAVE_MSP_AFTER.  True if the byte is used up.
bool Proto_MspByte (CMUSHclientDoc & doc, unsigned char c);
// A \n, \r, ESC or IAC arrived in those phases.  True if it is used up.
bool Proto_MspInterrupted (CMUSHclientDoc & doc, unsigned char c);
void Proto_MspPlay (CMUSHclientDoc & doc, const proto::MspTrigger & trigger);
// Repeats, finished downloads.  From Proto_Tick.
void Proto_MspTick (CMUSHclientDoc & doc);
void Proto_MspStopAll (CMUSHclientDoc & doc);
void Proto_MspAllowHost (CMUSHclientDoc & doc, const std::string & host);

// Where a server may put files, for this world: inside this, and nowhere else.
std::string Proto_WorldCacheDirectory (CMUSHclientDoc & doc);
// `relative` under `base`, if once symbolic links and the like are resolved it is
// still there.  "" if not.  Does not need the file to exist.
std::string Proto_ContainedPath (const std::string & base, const std::string & relative);

// ---------------------------------------------------------------- GMCP glue
// gmcp.cpp

// Option 201 from the server.  Called instead of the generic plugin path.
void Proto_GmcpWill (CMUSHclientDoc & doc);
void Proto_GmcpWont (CMUSHclientDoc & doc);
// A subnegotiation arrived.  After the raw callbacks have had it.
void Proto_GmcpReceived (CMUSHclientDoc & doc);
// Something went out: did a plugin just send Core.Supports.Set?
void Proto_GmcpWatchSent (CMUSHclientDoc & doc, const char * data, int length);
// A plugin went away, so its subscriptions go too.
void Proto_GmcpPluginGone (CMUSHclientDoc & doc, const std::string & pluginID);
// The last raw text for a package: for GMCP_Get, and for Lua, which gets the bytes.
bool Proto_GmcpLast (CMUSHclientDoc & doc, const std::string & package, std::string * rawText);

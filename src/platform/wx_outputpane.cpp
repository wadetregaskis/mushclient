// platform/wx_outputpane.cpp — Custom output pane implementation

#ifndef _WIN32

#include "platform/wx_outputpane.h"
#include <wx/dcbuffer.h>
#include <wx/clipbrd.h>
#include <wx/tooltip.h>
#include <algorithm>
// compat.h provides RGB(), GetRValue(), GetGValue(), GetBValue().
#include "platform/compat.h"
#include "pal/scroll_geometry.h"   // pal::isScrolledToBottom — the shared predicate
#include "pal/text_rect.h"         // pal::resolveTextRect — world.TextRectangle

// Forward declarations for font variant helpers defined after the constructor.
static wxFont MakeFontVariant(const wxFont& base, bool bold, bool underline, bool italic = false);
static void RebuildFontVariants(const wxFont& base,
                                wxFont& outBold,
                                wxFont& outUnderline,
                                wxFont& outBoldUnderline,
                                wxFont& outItalic,
                                wxFont& outBoldItalic,
                                wxFont& outItalicUnderline,
                                wxFont& outBoldItalicUnderline);

// ── Context-menu IDs ─────────────────────────────────────────────────────────
// NOT wxID_COPY / wxID_SELECTALL.  On macOS AppKit claims the standard ids for
// its own Edit-menu actions: it decides their enabled state (overriding
// menu.Enable) and it takes the click, so the pane's EVT_MENU never fires.
// Measured with a live selection: this context menu's "Copy" rendered GREY
// while wx had enabled it, and "Select All" did nothing at all -- while "Copy
// as HTML", the one item in the same menu built with a dynamically-allocated
// id, was enabled and worked.  Same menu, same selection, differing only in
// the id.
//
// The main Edit menu already had this fixed (wx_mfc_impl.cpp uses ID_EDIT_COPY),
// but this menu was never converted.  These are MFC's canonical command ids —
// ID_EDIT_COPY (mfc_ui.h) and ID_EDIT_SELECTALL (resource.h) — which AppKit has
// no opinion about.  Spelled numerically because this file is wx-side and pulls
// in neither header.
static const int ID_COPY       = 0xE122;   // ID_EDIT_COPY
static const int ID_SELECTALL  = 32108;    // ID_EDIT_SELECTALL

// The defect was the VALUE of these two constants, so the guard belongs where a
// value can be checked: reverting either to the wx standard id now fails to
// compile rather than silently producing a dead menu item that still looks
// right.  A runtime test could not see this — the menu is built inside the
// right-click handler, and the symptom (AppKit greying an item wx enabled)
// only appears with a real NSMenu on screen.
static_assert(ID_COPY != wxID_COPY,
              "output-pane context Copy must not use wxID_COPY: AppKit claims "
              "the standard ids, greys the item and swallows the click");
static_assert(ID_SELECTALL != wxID_SELECTALL,
              "output-pane context Select All must not use wxID_SELECTALL: "
              "same AppKit hijack, and it silently does nothing");
static const int ID_CLEAR      = wxID_HIGHEST + 100;

// ── Canonical ids carried by the context menu ────────────────────────────────
// Windows' own output-window popup is CG_IDR_POPUP_MUSHVIEW
// (platform/mushclient_dialogs.rc:2767).  Its items are canonical MFC command
// ids, and they are spelled out here — numerically, because this file is
// wx-side and pulls in neither resource.h nor mfc_ui.h, the same reason ID_COPY
// above is a literal.  The menu ITEM carries the canonical id (so
// menu_ui_rules.cpp can be asked about it by name, and so the selftest can pin
// the menu by id rather than by label); what is DISPATCHED can differ — see
// kCanonNotepadDispatch.
static const int ID_CANON_HIGHLIGHTPHRASE   = 33036;  // ID_DISPLAY_HIGHLIGHTPHRASE
static const int ID_CANON_MULTILINETRIGGER  = 33044;  // ID_DISPLAY_MULTILINETRIGGER
static const int ID_CANON_GOTOURL           = 32275;  // ID_DISPLAY_GOTOURL
static const int ID_CANON_SENDMAILTO        = 32276;  // ID_DISPLAY_SENDMAILTO
static const int ID_CANON_FIND              = 32248;  // ID_DISPLAY_FIND
static const int ID_CANON_FINDAGAIN         = 32249;  // ID_DISPLAY_FINDAGAIN
static const int ID_CANON_RECALLTEXT        = 32324;  // ID_DISPLAY_RECALLTEXT
static const int ID_CANON_BOOKMARKSELECTION = 32318;  // ID_DISPLAY_BOOKMARKSELECTION
static const int ID_CANON_GOTOBOOKMARK      = 32319;  // ID_DISPLAY_GOTOBOOKMARK
static const int ID_CANON_FREEZEOUTPUT      = 32201;  // ID_DISPLAY_FREEZEOUTPUT ("Pause output")
static const int ID_CANON_WRAPLINES         = 32118;  // ID_GAME_WRAPLINES
static const int ID_CANON_FULLSCREENMODE    = 32332;  // ID_VIEW_FULLSCREENMODE
static const int ID_CANON_ACTIVITYLIST      = 32203;  // ID_DISPLAY_ACTIVITYLIST
static const int ID_CANON_GAMEPREFERENCES   = 32158;  // ID_GAME_PREFERENCES
static const int ID_CANON_NOTESWORKAREA     = 32333;  // ID_EDIT_NOTESWORKAREA
static const int ID_CANON_LOGSESSION        = 32117;  // ID_FILE_LOGSESSION

// Notepad is the one item whose id is not also the command to run.  The port's
// standalone-notepad command is 40001, and wx_toolbar.cpp:70 already forwards
// ID_EDIT_NOTESWORKAREA onto it; this menu does the same rather than inventing
// a second mapping.
static const int kCanonNotepadDispatch      = 40001;

// ── UTF-8 character counting ──────────────────────────────────────────────────
// Returns the number of Unicode code points in a UTF-8 encoded std::string.
// This is always <= s.size() (equal for ASCII, less for multi-byte sequences).
// Used wherever we treat character positions as column indices for selection,
// wrapping, and link hit-testing — byte counts are wrong for non-ASCII text.
// Declared in stdafx.h, which this lean wx TU deliberately does not pull in.
size_t UTF8NextChar (const unsigned char * p, size_t n, unsigned int & cp);

// Both of these walk the run with UTF8NextChar (utilities_shared.cpp), so "how
// many characters is this run" and "what characters are they" cannot drift
// apart.  They used to be computed independently — a lead-byte scan here versus
// wxString::FromUTF8 in the decoder — which agreed only while the bytes were
// well-formed.
static inline int UTF8CharLen(const std::string& s)
{
    const unsigned char* p = (const unsigned char*) s.data();
    size_t i = 0;
    int    n = 0;
    while (i < s.size()) {
        unsigned int cp;
        i += UTF8NextChar(p + i, s.size() - i, cp);
        ++n;
    }
    return n;
}

// The tolerant decode.  Declared in wx_outputpane.h — the notepad needs it too,
// for text it is handed as raw bytes (world output, and base64 decode output,
// which is arbitrary binary).
wxString DecodeUTF8Lenient(const char* bytes, size_t n)
{
    const unsigned char* p = (const unsigned char*) bytes;
    wxString out;
    out.reserve(n);
    for (size_t i = 0; i < n; ) {
        unsigned int cp;
        i += UTF8NextChar(p + i, n - i, cp);
        out += wxUniChar(cp);
    }
    return out;
}

// UI-chrome decode: UTF-8, else Latin-1.  See wx_outputpane.h for when to use
// this vs DecodeUTF8Lenient.  Until 2026-08-12 a dozen boundary sites (DDX
// SetWindowText, list rows, window titles, the status bar, the Activity list)
// each called bare FromUTF8, so a legacy-charset string blanked the control —
// and through DDX's save arm, then erased the doc member it came from.
wxString DecodeTextLenient(const char* bytes, size_t n)
{
    if (!bytes || n == 0) return wxString();
    wxString s = wxString::FromUTF8(bytes, n);
    if (s.empty())
        s = wxString::From8BitData(bytes, n);
    return s;
}

// ── Non-UTF-8 world text decoding ─────────────────────────────────────────────
// Map a Windows LOGFONT charset (the world's m_font_charset) to a wxFontEncoding,
// so a non-UTF-8 world's bytes decode the way the Windows 8-bit render path shows
// them (GDI maps bytes→glyphs via the selected font's charset).  Unmapped/ANSI/
// DEFAULT fall through to CP1252 (Windows Latin-1).
// External linkage: wx_surface.cpp reuses it for legacy-charset WindowText
// bytes (the decided ISurface charset fix — see docs/PAL.md).
wxFontEncoding WinCharsetToEncoding(int cs)
{
    switch (cs) {
        case 128: return wxFONTENCODING_CP932;   // SHIFTJIS_CHARSET    (Japanese)
        case 129: return wxFONTENCODING_CP949;   // HANGUL_CHARSET      (Korean)
        case 134: return wxFONTENCODING_CP936;   // GB2312_CHARSET      (Simplified Chinese)
        case 136: return wxFONTENCODING_CP950;   // CHINESEBIG5_CHARSET (Traditional Chinese)
        case 161: return wxFONTENCODING_CP1253;  // GREEK_CHARSET
        case 162: return wxFONTENCODING_CP1254;  // TURKISH_CHARSET
        case 163: return wxFONTENCODING_CP1258;  // VIETNAMESE_CHARSET
        case 177: return wxFONTENCODING_CP1255;  // HEBREW_CHARSET
        case 178: return wxFONTENCODING_CP1256;  // ARABIC_CHARSET
        case 186: return wxFONTENCODING_CP1257;  // BALTIC_CHARSET
        case 204: return wxFONTENCODING_CP1251;  // RUSSIAN_CHARSET     (Cyrillic)
        case 222: return wxFONTENCODING_CP874;   // THAI_CHARSET
        case 238: return wxFONTENCODING_CP1250;  // EASTEUROPE_CHARSET
        case 255: return wxFONTENCODING_CP437;   // OEM_CHARSET
        default:  return wxFONTENCODING_CP1252;  // ANSI / DEFAULT / unmapped
    }
}

// ── The shared charset converter ─────────────────────────────────────────────
// Building a wxCSConv per decode was the whole cost of this path for most
// worlds.  A wxCSConv creates its real backend lazily on first use — an
// iconv_open on this platform — and the old code constructed and destroyed one
// for EVERY run it touched: every run of every appended line, of every painted
// line, of every line walked by a mouse move, and of every line in the
// scrollback on every Find keypress.  That was rare while worlds were being
// silently flipped to UTF-8; since worlds correctly keep their 8-bit charset it
// is the ordinary path.
//
// Keyed by the ENCODING, which is also the whole of the invalidation story: a
// world that changes charset at runtime maps to a different wxFontEncoding and
// therefore a different entry, and wxCSConv(enc) is a pure function of enc, so
// a cached entry can never be stale.  At most fifteen entries exist, one per
// encoding WinCharsetToEncoding can answer, so a linear scan beats a hash.
//
// thread_local rather than shared: wxCSConv builds its backend from a const
// method, so two threads first using the same instance would race inside it.
// (DecodeOutputBytes is reachable off the GUI thread — Print World walks the
// doc's own lines.)  Per-thread copies cost one small object per encoding per
// thread and need no lock on a path that runs per run.
//
// Deliberately never destroyed: these outlive wxEntry, and freeing a wxCSConv
// during static/thread destruction would run wx conversion teardown after the
// library it belongs to has shut down.  Fifteen small objects per thread, once.
std::atomic<unsigned long> g_outputConvBuilds{0};
std::atomic<unsigned long> g_outputDecodes{0};

// Stateless single-byte table converter for CP1255 (Hebrew) and CP1258
// (Vietnamese).  On Linux, glibc's gconv modules for CP1255 and CP1258 are
// stateful NFC-composing converters that buffer base vowels/letters inside
// iconv_t's shift state waiting for a combining mark; wxMBConv_iconv::ToWChar
// does not flush that shift state when called with an explicit byte length,
// which truncates trailing vowels and fails the two-pass wxString sizing call
// with E2BIG.  Windows MultiByteToWideChar and macOS libiconv treat both as
// stateless 256-entry single-byte codepages; these tables match that contract.
class CSingleByteTableConv final : public wxMBConv {
    const uint16_t* m_high; // 128 entries for 0x80..0xFF (0 = unmapped)
public:
    explicit CSingleByteTableConv(const uint16_t* high) : m_high(high) {}
    wxMBConv* Clone() const override { return new CSingleByteTableConv(m_high); }
    size_t ToWChar(wchar_t* dst, size_t dstLen,
                   const char* src, size_t srcLen = wxNO_LEN) const override
    {
        if (!src) return wxCONV_FAILED;
        const size_t n = (srcLen == wxNO_LEN) ? (std::strlen(src) + 1) : srcLen;
        for (size_t i = 0; i < n; ++i) {
            const unsigned char b = (unsigned char)src[i];
            if (b >= 0x80 && m_high[b - 0x80] == 0)
                return wxCONV_FAILED;
        }
        if (!dst) return n;
        if (dstLen < n) return wxCONV_FAILED;
        for (size_t i = 0; i < n; ++i) {
            const unsigned char b = (unsigned char)src[i];
            dst[i] = (b < 0x80) ? (wchar_t)b : (wchar_t)m_high[b - 0x80];
        }
        return n;
    }
    size_t FromWChar(char* dst, size_t dstLen,
                     const wchar_t* src, size_t srcLen = wxNO_LEN) const override
    {
        if (!src) return wxCONV_FAILED;
        const size_t n = (srcLen == wxNO_LEN) ? (std::wcslen(src) + 1) : srcLen;
        for (size_t i = 0; i < n; ++i) {
            const wchar_t wc = src[i];
            if (wc >= 0 && wc < 0x80) continue;
            bool found = false;
            for (int j = 0; j < 128; ++j)
                if (m_high[j] != 0 && (wchar_t)m_high[j] == wc) { found = true; break; }
            if (!found) return wxCONV_FAILED;
        }
        if (!dst) return n;
        if (dstLen < n) return wxCONV_FAILED;
        for (size_t i = 0; i < n; ++i) {
            const wchar_t wc = src[i];
            if (wc >= 0 && wc < 0x80) { dst[i] = (char)wc; continue; }
            for (int j = 0; j < 128; ++j)
                if (m_high[j] != 0 && (wchar_t)m_high[j] == wc) {
                    dst[i] = (char)(0x80 + j);
                    break;
                }
        }
        return n;
    }
};

static const uint16_t kCp1255High[128] = {
    0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0000, 0x2039, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0000, 0x203A, 0x0000, 0x0000, 0x0000, 0x0000,
    0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x20AA, 0x00A5, 0x00A6, 0x00A7,
    0x00A8, 0x00A9, 0x00D7, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
    0x00B8, 0x00B9, 0x00F7, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF,
    0x05B0, 0x05B1, 0x05B2, 0x05B3, 0x05B4, 0x05B5, 0x05B6, 0x05B7,
    0x05B8, 0x05B9, 0x0000, 0x05BB, 0x05BC, 0x05BD, 0x05BE, 0x05BF,
    0x05C0, 0x05C1, 0x05C2, 0x05C3, 0x05F0, 0x05F1, 0x05F2, 0x05F3,
    0x05F4, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x05D0, 0x05D1, 0x05D2, 0x05D3, 0x05D4, 0x05D5, 0x05D6, 0x05D7,
    0x05D8, 0x05D9, 0x05DA, 0x05DB, 0x05DC, 0x05DD, 0x05DE, 0x05DF,
    0x05E0, 0x05E1, 0x05E2, 0x05E3, 0x05E4, 0x05E5, 0x05E6, 0x05E7,
    0x05E8, 0x05E9, 0x05EA, 0x0000, 0x0000, 0x200E, 0x200F, 0x0000,
};

static const uint16_t kCp1258High[128] = {
    0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0000, 0x2039, 0x0152, 0x0000, 0x0000, 0x0000,
    0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0000, 0x203A, 0x0153, 0x0000, 0x0000, 0x0178,
    0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7,
    0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
    0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF,
    0x00C0, 0x00C1, 0x00C2, 0x0102, 0x00C4, 0x00C5, 0x00C6, 0x00C7,
    0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x0300, 0x00CD, 0x00CE, 0x00CF,
    0x0110, 0x00D1, 0x0309, 0x00D3, 0x00D4, 0x01A0, 0x00D6, 0x00D7,
    0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x01AF, 0x0303, 0x00DF,
    0x00E0, 0x00E1, 0x00E2, 0x0103, 0x00E4, 0x00E5, 0x00E6, 0x00E7,
    0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x0301, 0x00ED, 0x00EE, 0x00EF,
    0x0111, 0x00F1, 0x0323, 0x00F3, 0x00F4, 0x01A1, 0x00F6, 0x00F7,
    0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x01B0, 0x20AB, 0x00FF,
};

static wxMBConv* MakeOutputCharsetConv(wxFontEncoding enc)
{
    if (enc == wxFONTENCODING_CP1255) return new CSingleByteTableConv(kCp1255High);
    if (enc == wxFONTENCODING_CP1258) return new CSingleByteTableConv(kCp1258High);
    return new wxCSConv(enc);
}

const wxMBConv& OutputCharsetConv(int charset)
{
    const wxFontEncoding enc = WinCharsetToEncoding(charset);
    struct Entry { wxFontEncoding enc; wxMBConv* conv; };
    static thread_local std::vector<Entry>* cache = new std::vector<Entry>();
    for (const Entry& e : *cache)
        if (e.enc == enc) return *e.conv;
    cache->push_back(Entry{ enc, MakeOutputCharsetConv(enc) });
    g_outputConvBuilds.fetch_add(1, std::memory_order_relaxed);
    return *cache->back().conv;
}

// True when every byte of this encoding decodes to exactly one character, so a
// run's rendered length IS its byte length and counting needs no decode at all.
//
// Whitelisted, not blacklisted, on purpose: an encoding added to
// WinCharsetToEncoding and forgotten here falls to the decode, which is slow
// but right, where the other polarity would silently miscount every column.
// The equality itself is pinned over all 256 byte values of every charset in
// outputpane_selftest.cpp (TestCharCountAvoidsTheDecode), so a wrong entry
// here fails loudly rather than drifting the selection against the glyphs.
static bool EncodingIsSingleByte(wxFontEncoding enc)
{
    switch (enc) {
        case wxFONTENCODING_CP1250: case wxFONTENCODING_CP1251:
        case wxFONTENCODING_CP1252: case wxFONTENCODING_CP1253:
        case wxFONTENCODING_CP1254: case wxFONTENCODING_CP1255:
        case wxFONTENCODING_CP1256: case wxFONTENCODING_CP1257:
        case wxFONTENCODING_CP1258: case wxFONTENCODING_CP874:
        case wxFONTENCODING_CP437:
            return true;
        default:
            // CP932 / CP936 / CP949 / CP950 are DBCS — and so is anything a
            // later charset row adds without touching this list.
            return false;
    }
}

// Decode a run's bytes to a wxString.  UTF-8 worlds (the default, and the common
// modern case) go straight through FromUTF8.  A non-UTF-8 world's bytes are
// decoded with its font charset; if that conversion fails outright (wxCSConv
// yields empty for non-empty input) we fall back to Latin-1 (From8BitData never
// fails), so text is always visible rather than silently dropped — the bug this
// fixes was FromUTF8() of non-UTF-8 bytes returning "" and the whole run vanishing.
// Exposed (declared in wx_outputpane.h) because Print World decodes the doc's
// raw style bytes with the same rule — it reads m_LineList rather than the pane,
// and a bare FromUTF8 on legacy-charset bytes makes whole runs vanish.
wxString DecodeOutputBytes(const char* bytes, size_t n, bool utf8, int charset)
{
    if (utf8) {
        if (n == 0)
            return wxString();
        // FromUTF8 is all-or-nothing: one bad byte and it yields "", blanking
        // the whole run.  Windows draws through MultiByteToWideChar(CP_UTF8, 0,
        // ...), which substitutes U+FFFD per ill-formed subpart and keeps the
        // rest.  Network data is repaired upstream now (telnet_phases.cpp
        // OutputBadUTF8characters), but a script can still hand world.Note
        // arbitrary bytes, so match Windows here too.
        const wxString s = wxString::FromUTF8(bytes, n);
        if (!s.empty())
            return s;
        return DecodeUTF8Lenient(bytes, n);
    }
    if (n == 0)
        return wxString();
    g_outputDecodes.fetch_add(1, std::memory_order_relaxed);
    wxString s(bytes, OutputCharsetConv(charset), n);
    if (s.empty())
        s = wxString::From8BitData(bytes, n);
    return s;
}

static wxString DecodeRunText(const SOutputRun& run)
{
    return DecodeOutputBytes(run.text.c_str(), run.text.size(),
                             run.utf8, run.charset);
}

// Rendered character count of a run — the metric DrawLine measures with, so all
// column/wrap/selection/hit-test bookkeeping must use THIS, not raw lead-byte
// counting.  For UTF-8 runs it equals the lead-byte count; for a non-UTF-8 world
// the bytes decode through the world charset, and a byte in 0x80–0xBF (e.g. a
// CP1252 curly quote) looks like a UTF-8 continuation byte — counting lead bytes
// would undercount and drift the selection/wrap against the rendered grid.
// Character count of a DBCS run, memoised BY CONTENT.  A double-byte charset
// genuinely needs the decode to be counted — a lead-byte scan would need a
// per-codepage range table and could then disagree with the string the renderer
// actually draws, which is the one property that must never give.  So the
// decode stays and the ANSWER is remembered instead.
//
// Keyed by the bytes rather than cached on the SOutputRun for two reasons: the
// run struct is one per styled span of every line of the scrollback and adding
// a field to it costs eight bytes each (72 → 80 after alignment), and a field
// would have to be invalidated by hand every time `text` was touched, where a
// content key cannot go stale by construction.  Sixteen entries is a line's
// worth of runs, so the second walk over a line — the mouse-move pattern, where
// XToChar sums the line and then RunAtCharColumn re-walks the same runs — hits.
//
// Runs above kMemoMax bytes are not memoised: the memcmp and the copy stop
// being cheap relative to the decode they save, and one huge world.Note would
// otherwise pin sixteen copies of itself in the memo.
static int DbcsRunCharLen(const SOutputRun& run)
{
    constexpr size_t kMemoMax     = 4096;
    constexpr size_t kMemoEntries = 16;

    auto measure = [&]() {
        return (int) DecodeOutputBytes(run.text.c_str(), run.text.size(),
                                       false, run.charset).size();
    };
    if (run.text.size() > kMemoMax)
        return measure();

    struct Entry { int charset; std::string bytes; int len; };
    static thread_local std::deque<Entry>* memo = new std::deque<Entry>();
    for (const Entry& e : *memo)
        if (e.charset == run.charset && e.bytes == run.text)
            return e.len;

    const int len = measure();
    memo->push_front(Entry{ run.charset, run.text, len });
    if (memo->size() > kMemoEntries) memo->pop_back();
    return len;
}

int RunCharLen(const SOutputRun& run)
{
    if (run.utf8)
        return UTF8CharLen(run.text);
    // A single-byte charset renders one character per byte, so the count is the
    // byte count and no conversion is needed.  This is the path every CP125x
    // world takes — i.e. nearly all of them.
    if (EncodingIsSingleByte(WinCharsetToEncoding(run.charset)))
        return (int) run.text.size();
    return DbcsRunCharLen(run);
}

// See the header.  No horiz_rule guard: a rule run has no text, so it already
// measures zero -- the two call sites that used to guard were doing nothing.
int LineCharLen(const SOutputLine& line)
{
    int chars = 0;
    for (const SOutputRun& run : line) chars += RunCharLen(run);
    return chars;
}

// See the header.  Runs tile the line's column axis without gaps or overlap, so
// at most one run can contain a given column -- which is why the old hand-rolled
// walks could `break` on the first match and why a zero-length run (a horizontal
// rule) is never returned: `col < c + 0` is false for every col.
//
// A negative column needs no guard of its own: c starts at zero and only grows,
// so `col >= c` can never hold.  (A guard here would be code no test could ever
// distinguish from its absence.)
const SOutputRun* COutputPane::RunAtCharColumn(const SOutputLine& line, int col,
                                               int* colWithinRun)
{
    int c = 0;
    for (const SOutputRun& run : line) {
        const int runLen = RunCharLen(run);
        if (col >= c && col < c + runLen) {
            if (colWithinRun) *colWithinRun = col - c;
            return &run;
        }
        c += runLen;
    }
    return nullptr;
}

// ── Hint bar ──────────────────────────────────────────────────────────────────
// Height (px) of the "↓ new output below" banner painted at the bottom of the
// client area when the user has scrolled up and new lines are arriving.
// Kept as a file-scope constant so OnPaint and OnLeftDown stay in sync.
static constexpr int kHintBarH = 22;

// ── Drag auto-scroll repeat ───────────────────────────────────────────────────
// Windows repeats as fast as MFC's idle loop spins, i.e. paint-bound, so ANY
// fixed rate here is the port's own invention -- stated rather than guessed at.
// 50 ms is 20 lines a second, which is the usual macOS drag-autoscroll feel and
// slow enough to stay controllable at the edge of a 5,000-line scrollback.  No
// distance-proportional acceleration: that would be a second invention with no
// Windows referent.
static constexpr int kAutoScrollMs     = 50;
static constexpr int kAutoScrollTimerId = 4711;

// ── Event table ───────────────────────────────────────────────────────────────
wxBEGIN_EVENT_TABLE(COutputPane, wxScrolledWindow)
    EVT_PAINT(                      COutputPane::OnPaint)
    EVT_SIZE(                       COutputPane::OnSize)
    EVT_LEFT_DOWN(                  COutputPane::OnLeftDown)
    EVT_LEFT_UP(                    COutputPane::OnLeftUp)
    EVT_LEFT_DCLICK(                COutputPane::OnLeftDClick)
    EVT_MIDDLE_DOWN(                COutputPane::OnMiddleDown)
    EVT_MIDDLE_UP(                  COutputPane::OnMiddleUp)
    EVT_RIGHT_DOWN(                 COutputPane::OnRightDown)
    EVT_RIGHT_DCLICK(               COutputPane::OnRightDClick)
    EVT_RIGHT_UP(                   COutputPane::OnRightUp)
    EVT_MOTION(                     COutputPane::OnMotion)
    EVT_LEAVE_WINDOW(               COutputPane::OnLeaveWindow)
    EVT_MOUSE_CAPTURE_LOST(         COutputPane::OnCaptureLost)
    EVT_MOUSEWHEEL(                 COutputPane::OnMouseWheel)
    EVT_KEY_DOWN(                   COutputPane::OnKeyDown)
    EVT_MENU(ID_COPY,               COutputPane::OnMenuCopy)
    EVT_MENU(ID_SELECTALL,          COutputPane::OnMenuSelectAll)
    EVT_MENU(ID_CLEAR,              COutputPane::OnMenuClear)
wxEND_EVENT_TABLE()

// ── Constructor ───────────────────────────────────────────────────────────────
COutputPane::COutputPane(wxWindow* parent, int maxLines)
    : wxScrolledWindow(parent, wxID_ANY,
                       wxDefaultPosition, wxDefaultSize,
                       wxHSCROLL | wxVSCROLL | wxFULL_REPAINT_ON_RESIZE)
    , m_maxLines(maxLines)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(wxColour(0, 0, 0));

    m_font = wxFont(kDefaultFontSize, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL,
                    wxFONTWEIGHT_NORMAL);
    SetFont(m_font);
    RebuildFontVariants(m_font, m_fontBold, m_fontUnderline, m_fontBoldUnderline,
                        m_fontItalic, m_fontBoldItalic,
                        m_fontItalicUnderline, m_fontBoldItalicUnderline);

    // Use wxMemoryDC for reliable font measurement before the window is shown.
    // wxClientDC in the constructor may return zero metrics on macOS/GTK because
    // the native view doesn't exist yet at that point.
    {
        wxBitmap bmp(1, 1, 24);
        wxMemoryDC mdc(bmp);
        mdc.SetFont(m_font);
        wxCoord w, h;
        mdc.GetTextExtent("M", &w, &h);
        if (h > 0) m_lineH = (m_lineSpacing > 0) ? m_lineSpacing : h + 2;
        if (w > 0) m_charW = w;
    }

    // Horizontal scrolling is disabled: MUD output is pre-wrapped at the server
    // side (typically 80 columns) so a horizontal scrollbar is never useful and
    // breaks the feel of a terminal.  Set the horizontal scroll rate to 0 so
    // wxScrolledWindow never shows or reacts to an H-scrollbar.
    SetScrollRate(0, m_lineH);
    UpdateScrollbars(false);

    // Show an I-beam cursor so the pane feels like a text view.
    SetCursor(wxCursor(wxCURSOR_IBEAM));

    // Update m_autoScroll whenever the user scrolls via the scrollbar.
    for (auto evtType : {
            wxEVT_SCROLLWIN_THUMBTRACK,
            wxEVT_SCROLLWIN_THUMBRELEASE,
            wxEVT_SCROLLWIN_LINEUP,
            wxEVT_SCROLLWIN_LINEDOWN,
            wxEVT_SCROLLWIN_PAGEUP,
            wxEVT_SCROLLWIN_PAGEDOWN,
            wxEVT_SCROLLWIN_TOP,
            wxEVT_SCROLLWIN_BOTTOM })
    {
        Bind(evtType, &COutputPane::OnScrollWinUpdate, this);
    }

    // The drag auto-scroll repeat (see AutoScrollTick).  A member wxTimer stops
    // itself when the pane is destroyed, so there is no window in which it can
    // outlive what it scrolls.
    m_autoScrollTimer.SetOwner(this, kAutoScrollTimerId);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        if (!AutoScrollTick()) m_autoScrollTimer.Stop();
    }, kAutoScrollTimerId);
}

// ── Font variant cache ────────────────────────────────────────────────────────
// Rebuilds all 8 font variants (bold × italic × underline) from m_font.
// Call whenever m_font changes so DrawRunChunk uses pre-built variants.
static wxFont MakeFontVariant(const wxFont& base, bool bold, bool underline, bool italic)
{
    wxFontStyle style = italic ? wxFONTSTYLE_ITALIC : wxFONTSTYLE_NORMAL;
    return wxFont(base.GetPointSize(), base.GetFamily(), style,
                  bold ? wxFONTWEIGHT_BOLD : base.GetWeight(),
                  underline, base.GetFaceName());
}

static void RebuildFontVariants(const wxFont& base,
                                wxFont& outBold,
                                wxFont& outUnderline,
                                wxFont& outBoldUnderline,
                                wxFont& outItalic,
                                wxFont& outBoldItalic,
                                wxFont& outItalicUnderline,
                                wxFont& outBoldItalicUnderline)
{
    outBold                = MakeFontVariant(base, true,  false, false);
    outUnderline           = MakeFontVariant(base, false, true,  false);
    outBoldUnderline       = MakeFontVariant(base, true,  true,  false);
    outItalic              = MakeFontVariant(base, false, false, true);
    outBoldItalic          = MakeFontVariant(base, true,  false, true);
    outItalicUnderline     = MakeFontVariant(base, false, true,  true);
    outBoldItalicUnderline = MakeFontVariant(base, true,  true,  true);
}

// ── URL detection ─────────────────────────────────────────────────────────────
// Scans plain-text runs for http:// or https:// URLs and splits them into
// separate link runs.  Each URL run gets action = URL text so OnLeftDown fires
// m_fn_link_clicked, which detects the http prefix and calls
// wxLaunchDefaultBrowser().  Only runs with no existing action and no
// horiz_rule flag are scanned (MXP links are already labelled).

// ── IsUsableLink ─────────────────────────────────────────────────────────────
// Whether a run's link may be ACTED on -- clicked, hand-cursored, offered in
// the context menu, or described in a tooltip.
//
// Windows gates all four on the same pair of tests: the action is not empty,
// and it does not contain the literal "&text;" (mushview.cpp:2158-2160 the
// click, :2848-2850 the cursor, :3538-3541 the menu, :4280-4282 the tooltip;
// the cursor one even carries the comment "we know what &text; is").  "&text;"
// is the MXP entity standing for a tag's own enclosed text, and it survives
// into the action only when the substitution never happened -- the enclosed
// text ran past MAX_TEXT (mxp/mxpClose.cpp:18) -- so what is left is not a
// command anyone meant to send.  The port had no such check: such a run looked
// live, and clicking it sent the literal "say &text;" to the MUD.
//
// Deliberately NOT folded into SOutputRun::isLink(), which DetectURLs also uses
// to decide which runs it may scan for bare URLs -- a different question.
static bool IsUsableLink(const SOutputRun* run)
{
    return run && run->isLink() && !run->action().empty() &&
           run->action().find("&text;") == std::string::npos;
}

static SOutputLine DetectURLs(SOutputLine line)
{
    // Quick scan: skip the whole line if it has no "http" substring at all.
    bool hasHttp = false;
    for (const auto& r : line)
        if (!r.isLink() && !r.horiz_rule &&
            r.text.find("http") != std::string::npos)
            { hasHttp = true; break; }
    if (!hasHttp) return line;

    SOutputLine result;
    // The line-level flags live on SOutputLine itself, so a freshly
    // constructed result drops every one of them -- bookmark, comment,
    // userInput, arrived and hardReturn -- for any line carrying "http".
    // Under the PULL model this function is on the fetch path, so that is the
    // line the pane actually shows.
    result.copyFlagsFrom(line);
    result.reserve(line.size() * 2);

    for (SOutputRun run : line) {
        // Pass through runs that already have a label or are decorative.
        if (run.isLink() || run.text.empty() || run.horiz_rule) {
            result.push_back(std::move(run));
            continue;
        }

        const std::string text = run.text;
        size_t pos = 0;
        bool splitOccurred = false;

        while (pos < text.size()) {
            size_t h1 = text.find("http://",  pos);
            size_t h2 = text.find("https://", pos);
            size_t urlStart = std::min(h1, h2);

            if (urlStart == std::string::npos) {
                // No more URLs.
                if (splitOccurred || pos > 0) {
                    SOutputRun tail = run;
                    tail.text = text.substr(pos);
                    result.push_back(std::move(tail));
                } else {
                    // No URL anywhere in this run — pass it through unmodified.
                    result.push_back(std::move(run));
                }
                break;
            }

            splitOccurred = true;

            // Text before the URL.
            if (urlStart > pos) {
                SOutputRun before = run;
                before.text = text.substr(pos, urlStart - pos);
                result.push_back(std::move(before));
            }

            // URL extends to the next whitespace (or end of run).
            size_t urlEnd = urlStart;
            while (urlEnd < text.size() &&
                   !std::isspace((unsigned char)text[urlEnd]))
                ++urlEnd;

            // Strip trailing punctuation that is almost certainly not part of
            // the URL itself: sentence-ending or bracketing characters.
            // e.g. "See http://example.com." or "link (http://x.com)"
            while (urlEnd > urlStart) {
                char last = text[urlEnd - 1];
                if (last == '.' || last == ',' || last == '!' || last == '?' ||
                    last == ')' || last == ']' || last == '}' || last == '\'' ||
                    last == '"' || last == ':' || last == ';')
                    --urlEnd;
                else
                    break;
            }

            // Emit the URL as a link run.  action = URL so m_fn_link_clicked
            // can distinguish it from MXP commands via the "http" prefix.
            SOutputRun urlRun  = run;
            urlRun.text        = text.substr(urlStart, urlEnd - urlStart);
            // A detected URL is a link run: one allocation, only here.
            urlRun.setLink(urlRun.text, std::string(), 0);
            // These runs are synthesised by the port, not by MXP, so no engine
            // option governs them and nothing else gives them a link look.  The
            // painter no longer force-colours/underlines action-bearing runs (so
            // that the world's link-colour and underline options are honoured),
            // which means the affordance has to be baked in here or a bare
            // http:// in MUD output would render as ordinary text.
            urlRun.fg        = 0x00FFAA44;   // COLORREF 0x00BBGGRR = RGB(0x44,0xAA,0xFF)
            urlRun.underline = true;
            result.push_back(std::move(urlRun));

            pos = urlEnd;
        }
        // If the loop ran out of text normally all segments were already pushed.
    }
    return result;
}

// ── Public API ────────────────────────────────────────────────────────────────

// See the header.  Lifted out of OnPaint so the guard-and-callback pairing is
// callable — and therefore testable — without an event loop; OnPaint still
// calls it at exactly the point Windows' OnDraw fires the plugin callback.
void COutputPane::FirePluginDraw(int firstVisiblePaneLine, int pixelOffset)
{
    if (!m_fn_draw_output_window) return;
    PluginDrawScope guard(*this);
    m_fn_draw_output_window(firstVisiblePaneLine, pixelOffset);
}

void COutputPane::AppendLine(SOutputLine&& line)
{
    // Every line entering the pane carries an arrival time, because the
    // output-buffer fade works out its opacity from one.  The drain stamps the
    // lines it builds from a CLine (that time is the accurate one -- a line can
    // be drained later than it arrived); this is the fallback for the ones
    // built anywhere else, of which there are three, and which would otherwise
    // read as time 0 and so as infinitely old.  One choke point rather than a
    // stamp at each builder: the next builder added cannot forget.
    if (line.arrived == 0)
        line.arrived = time(NULL);

    // Script running inside the mid-paint ON_PLUGIN_DRAW_OUTPUT_WINDOW
    // callback may print (Note etc.), which lands here while OnPaint is
    // iterating m_lines.  Defer such appends until the paint completes
    // (docs/PAL.md S4c re-entrancy guard; Windows relies on the documented
    // draw-only discipline instead).
    //
    // BEFORE the pending-row handling below, deliberately: the paint is drawing
    // the pending row too, so a print from the callback must not replace or
    // clear it either.  The replay runs the whole body, in order, next turn.
    if (m_pluginDrawDepth > 0) {
        auto pending = std::make_shared<SOutputLine>(std::move(line));
        CallAfter([this, pending]() { AppendLine(std::move(*pending)); });
        return;
    }

    // Pending lines (m_pCurrentLine in the telnet engine, not yet completed)
    // are rendered as a dim trailing row below the committed buffer.  They are
    // never added to m_lines; they simply replace m_pendingLine.
    if (line.pending) {
        m_pendingLine = std::move(line);
        ClearSelectionOnNewOutput();
        if (m_batchDepth == 0) {
            UpdateScrollbars(m_autoScroll);
            Refresh(false);
        }
        return;
    }

    // A completed line arriving always clears any pending content (the engine
    // has committed the line; the next pending update will carry new partial text).
    m_pendingLine.clear();
    m_pendingLine.pending = false;

    // Auto-detect plain http:// / https:// URLs and split them into link runs.
    line = DetectURLs(std::move(line));

    int lineChars = LineCharLen(line);
    if (lineChars > m_maxLineChars) m_maxLineChars = lineChars;

    // Append to wrap cache before the line is in m_lines so WrapCount() works.
    {
        int wc = (m_wrapCols > 0)
                 ? std::max(1, (lineChars > 0 ? (lineChars + m_wrapCols - 1) / m_wrapCols : 1))
                 : 1;
        m_visualRowStart.push_back(m_rowBase + m_totalVisualRows);
        m_totalVisualRows += wc;
        // Clear Screen's blank space is consumed by the output that follows it,
        // exactly as Terminal.app's is: the virtual height stays put while real
        // rows replace padding ones, so the newest line lands where the blank
        // region began instead of pushing the whole view down.
        if (m_overscrollRows > 0)
            m_overscrollRows = std::max(0LL, m_overscrollRows - wc);
    }

    m_lineChars.push_back(lineChars);
    // PULL MODEL: keep the wrap metrics, DROP the content.  The engine's CLine
    // already holds it, and m_lines was a second full copy of the entire
    // scrollback -- ~64 bytes per run plus its strings, against the 4 bytes per
    // line the wrap cache actually needs.  The push still happens: it is how
    // the pane learns a line exists and how wide it is.
    const SOutputLine& loggedLine = m_fn_fetch_line ? line : (m_lines.push_back(std::move(line)), m_lines.back());
    // The find cache is content-keyed, so it does have to go (m_bufferGen).  The
    // LINE cache is keyed by index, and a tail append shifts no index, so it
    // does NOT: it used to be dropped here as well, which meant that during
    // continuous output -- i.e. whenever a MUD is talking -- no cached
    // conversion ever survived from one paint to the next and every paint
    // re-materialised the whole viewport from its CLines.  Every path that DOES
    // shift indices invalidates for itself (DropFrontLines, DeleteTailLines,
    // SetMaxLines, ClearOutput), and SetBookmark erases just the one entry.
    ++m_bufferGen;

    // Trim the front back to what the pane should be holding.  -1 = unlimited
    // (max_output_lines 0 in the push model), so nothing is ever dropped.
    //
    // No wrap-cache rebuild: PopFrontWrapCache keeps the cache consistent in
    // O(1) per trimmed line.  This used to be a full O(N) rebuild per APPENDED
    // line once the buffer was at capacity -- i.e. permanently, for any
    // long-running session -- and each rebuild re-scanned every run's bytes.
    const int keep = DerivedTargetLength();
    if (keep >= 0 && (int) m_visualRowStart.size() > keep)
        DropFrontLines((long) m_visualRowStart.size() - keep, true);

    // Notify the logging hook (if any) immediately after the line is accepted.
    // Done before the batch-depth check so every line is logged even in batches.
    if (m_fn_on_line_appended) m_fn_on_line_appended(loggedLine);

    // Before the batch check, not inside it: a batched drain must drop the
    // selection exactly as an unbatched one does, and Windows' addedstuff runs
    // per line either way.
    ClearSelectionOnNewOutput();

    if (m_batchDepth == 0) {
        UpdateScrollbars(m_autoScroll);
        // Always repaint: when not auto-scrolling the hint bar needs to be
        // drawn (or kept visible) as new content arrives.
        Refresh(false);
    }
}

void COutputPane::EndBatch()
{
    if (m_batchDepth <= 0) return;
    if (--m_batchDepth == 0) {
        UpdateScrollbars(m_autoScroll);
        Refresh(false);
        // "Smoother scrolling": paint now rather than at the next idle, so a
        // burst of MUD output scrolls in step instead of in jumps.  This is
        // MFC's "Invalidate(); if (App.m_bSmootherScrolling) UpdateWindow();".
        if (m_smootherScroll) Update();
    }
}

// ── RecolourFromPalette ───────────────────────────────────────────────────
// See the header.  Runs carry the style inputs their fg/bg were resolved from
// (SOutputRun::style_flags / fore_idx / back_idx), so the same
// pal::resolveStyleColours the document uses reproduces them exactly against a
// new palette -- no second implementation, no drift.
//
// RGB runs (MXP <color>, trigger "other" colours) and xterm-256 indices are
// palette-INdependent and re-resolve to themselves, so they cost a branch and
// are left alone rather than special-cased.
void COutputPane::RecolourFromPalette(const pal::StylePalette& palette)
{
    // The 256-entry xterm table (utilities_shared.cpp fills it).  COLORREF is
    // uint32_t, so the cast is width-safe -- doc_shared.cpp static_asserts that
    // against pal::StylePalette's entry type.
    extern COLORREF xterm_256_colours[256];
    const unsigned int* xterm = (const unsigned int*) xterm_256_colours;

    auto recolour = [&](SOutputLine& line) {
        for (SOutputRun& run : line)
            pal::resolveStyleColours(run.style_flags, run.fore_idx, run.back_idx,
                                     palette, xterm, run.fg, run.bg);
    };

    // Push model: recolour the stored copies.  Pull model: the engine owns
    // the lines and re-resolves on the next fetch, so just drop the cache.
    for (SOutputLine& line : m_lines) recolour(line);
    InvalidateLineCache();
    recolour(m_pendingLine);

    // Colours do not change metrics, so the wrap cache, the scroll geometry and
    // the selection are all still valid -- a repaint is the whole update.
    Refresh(false);
}

void COutputPane::SetMaxLines(int n)
{
    // 0 is the unlimited sentinel (world option max_output_lines), and it has
    // to pass through the floor below: clamping it to 100 would make "no limit"
    // the SMALLEST buffer in the app.
    if (n != 0 && n < 100) n = 100;
    m_maxLines = n;
    const int keep = DerivedTargetLength();
    const bool trimmed =
        (keep >= 0 && (int) m_visualRowStart.size() > keep)
        && DropFrontLines((long) m_visualRowStart.size() - keep, false) > 0;
    if (trimmed) {
        m_selAnchor = SelPoint{};
        m_selFocus  = SelPoint{};
        NotifySelectionMaybeChanged();   // the trim's clear notifies (did_jump)
        RebuildWrapCache();
        UpdateScrollbars(m_autoScroll);
        Refresh(false);
    }
}

// ── DeleteHeadLines ───────────────────────────────────────────────────────
// The oldest `count` lines, after the engine has freed the same ones
// (CMUSHclientDoc::DeleteHeadLines).  Uses the SAME bookkeeping the capacity
// trim in AppendLine does -- PopFrontWrapCache keeps the wrap cache and the
// visual-row base consistent in O(1) per line, and the selection indices shift
// down with the content.
void COutputPane::DeleteHeadLines(long count)
{
    if (count <= 0) return;
    // Bounded by the WRAP CACHE, not by LineCountI(): the engine has already
    // freed these lines by the time it calls us, so its count is the one AFTER
    // the deletion and would trim nothing at all under the pull model.
    if (DropFrontLines(count, false) <= 0) return;

    UpdateScrollbars(m_autoScroll);
    Refresh(false);
}

void COutputPane::DeleteTailLines(long count)
{
    // Same reasoning as DeleteHeadLines: world.DeleteLines(n) has already taken
    // them off the engine, so the pane's own derived length is the bound.  In
    // the pull model there is no content deque to pop -- only the metrics.
    if (count <= 0 || m_visualRowStart.empty()) return;
    long toRemove = std::min(count, (long) m_visualRowStart.size());
    for (long i = 0; i < toRemove; ++i) {
        if (!m_fn_fetch_line && !m_lines.empty()) m_lines.pop_back();
        if (!m_visualRowStart.empty()) m_visualRowStart.pop_back();
        if (!m_lineChars.empty()) m_lineChars.pop_back();
    }
    ++m_bufferGen; InvalidateLineCache();
    // Clear any selection that may have referenced deleted lines.
    m_selAnchor = SelPoint{};
    m_selFocus  = SelPoint{};
    NotifySelectionMaybeChanged();       // the trim's clear notifies (did_jump)
    RebuildWrapCache();
    UpdateScrollbars(m_autoScroll);
    Refresh(false);
}

void COutputPane::AppendNote(const std::string& text, unsigned long fg)
{
    // PULL MODEL: the pane keeps no line content of its own, so a line
    // manufactured here has no CLine behind it.  It could never be painted --
    // every read goes back to the engine -- and its wrap-cache entry left the
    // pane one line LONGER than the engine, so the capacity trim at the end of
    // the next AppendLine dropped the oldest REAL line's metrics and every wrap
    // count from then on was off by one.  The frame gives the pane a route to
    // the document instead, and the note comes back through the ordinary drain
    // as a real line like any other.
    if (m_fn_engine_note) {
        m_fn_engine_note(text, fg);
        return;
    }
    // No route to a document: dropping the note is the lesser harm, since the
    // pane-only line below would corrupt the wrap cache and still not show.
    if (m_fn_fetch_line) return;

    SOutputLine noteLine;
    SOutputRun  noteRun;
    noteRun.text      = text;
    noteRun.fg        = fg;
    noteRun.bg        = m_bgColor;
    noteRun.bold      = false;
    noteRun.underline = false;
    noteLine.push_back(std::move(noteRun));
    AppendLine(std::move(noteLine));
}

void COutputPane::SetPaneBackground(unsigned long col)
{
    m_bgColor = col;
    wxColour wxbg(GetRValue(col), GetGValue(col), GetBValue(col));
    SetBackgroundColour(wxbg);
    Refresh(false);
}

// World Properties > Appearance > Output > "Pixel offset".  Changing it changes
// how many characters fit on a row, so the wrap cache and the scrollbars have to
// be rebuilt -- the same order the pane's deferred font-metrics path uses.
void COutputPane::SetTextPixelOffset(int px)
{
    // The dialog and the scripting option both clamp to 0..20
    // (DDV_MinMaxInt at prefspropertypages.cpp:5849, and the 0/20 bounds on
    // "pixel_offset" in scriptingoptions_numeric.inc:122).  Clamp again here so
    // a value written straight into the doc can't produce a negative width.
    // No DPI scaling: wx logical pixels already are Windows' 100%-scale pixels.
    px = std::max(0, std::min(20, px));
    if (px == m_pixelOffset) return;
    m_pixelOffset = px;
    RebuildWrapCache();
    UpdateScrollbars(m_autoScroll);
    Refresh();
}

void COutputPane::ScrollToBottom()
{
    bool wasAuto = m_autoScroll;
    m_autoScroll = true;
    UpdateScrollbars(true);
    if (!wasAuto && m_fn_autoscroll_changed)
        m_fn_autoscroll_changed(true);
}

// See the header.  UpdateScrollbars(true) moves the viewport to the very end
// without touching m_autoScroll, and RefreshAutoScrollFromPosition is the
// ScrollToPosition tail -- the auto_pause gate and the keep_pause_at_bottom
// latch -- so the two together ARE OnTestEnd.  Same shape as ScrollToTop().
void COutputPane::ScrollToBufferEnd()
{
    UpdateScrollbars(true);
    RefreshAutoScrollFromPosition();
    Refresh(false);
}

void COutputPane::SetPaused(bool paused)
{
    if (paused) {
        // Disengage auto-scroll without moving the viewport: new lines will
        // continue to be appended to the buffer but won't scroll into view.
        bool wasAuto = m_autoScroll;
        m_autoScroll = false;
        if (wasAuto && m_fn_autoscroll_changed) m_fn_autoscroll_changed(false);
        Refresh(false);
    } else {
        ScrollToBottom();
    }
}

long COutputPane::GetScrollPositionY() const
{
    int scrollY = 0, unitH = 0;
    const_cast<COutputPane*>(this)->GetViewStart(nullptr, &scrollY);
    const_cast<COutputPane*>(this)->GetScrollPixelsPerUnit(nullptr, &unitH);
    return (long)scrollY * (long)(unitH > 0 ? unitH : m_lineH);
}

void COutputPane::ScrollToTop()
{
    int scrollX;
    GetViewStart(&scrollX, nullptr);
    Scroll(scrollX, 0);
    // Windows reaches the top through ScrollToPosition, whose ENTIRE freeze
    // assignment sits inside `if (pDoc->m_bAutoFreeze)` (mushview.cpp:4600), so
    // in a world with auto_pause off a plain scroll neither freezes nor thaws.
    // The port froze unconditionally, which stopped the output in exactly the
    // world whose option says it must never stop.  RefreshAutoScrollFromPosition
    // IS that tail — gate, keep_pause_at_bottom latch and all — and the position
    // is readable immediately after Scroll(), so it can simply be asked.
    RefreshAutoScrollFromPosition();
    Refresh(false);
}

// Topmost logical line with any pixel of it on screen.  The scrollbar is the
// authority on where we are -- deriving it from a line count instead is the
// mistake that once put a one-row dead zone in the wheel handler.
int COutputPane::FirstVisibleLine() const
{
    if (LineCountI() == 0) return 0;
    int scrollY = 0, unitH = 0;
    const_cast<COutputPane*>(this)->GetViewStart(nullptr, &scrollY);
    const_cast<COutputPane*>(this)->GetScrollPixelsPerUnit(nullptr, &unitH);
    if (unitH <= 0 || m_lineH <= 0) return 0;
    const long long topPixel = (long long) scrollY * unitH;
    const int topRow = (int) (topPixel / m_lineH);
    const int line = VisualRowToLogical(topRow);
    return std::max(0, std::min(LineCountI() - 1, line));
}

// Any pixel of logical line `lineIdx` inside the text viewport?  Same rule as
// FirstVisibleLine: the scrollbar is the authority on where we are.
bool COutputPane::IsLineVisible(int lineIdx) const
{
    if (lineIdx < 0 || lineIdx >= LineCountI() || m_lineH <= 0) return false;
    int scrollY = 0, unitH = 0;
    const_cast<COutputPane*>(this)->GetViewStart(nullptr, &scrollY);
    const_cast<COutputPane*>(this)->GetScrollPixelsPerUnit(nullptr, &unitH);
    if (unitH <= 0) return false;
    const long long topPixel  = (long long) scrollY * unitH;
    const long long lineTop   = pal::rowsToPixels(RowStartOf(lineIdx), m_lineH);
    const long long lineBot   = lineTop + (long long) WrapCount(lineIdx) * m_lineH;
    return lineBot > topPixel && lineTop < topPixel + TextViewportH();
}

// ── ClearScreen ───────────────────────────────────────────────────────────
// Terminal.app's ⌃⌘L: put the last line at the TOP of the viewport and erase
// nothing.  The scrollback is untouched -- scroll up and it is all still
// there -- which is the whole point, and the reason this is the one clear that
// frees no memory.
//
// It needs somewhere to scroll TO.  Ordinarily the last line can only reach
// the BOTTOM of the viewport, because wxScrolled clamps the position to
// (range - page).  So reserve a page of blank rows below it, which
// UpdateScrollbars adds to the virtual height and AppendLine gives back one
// row at a time as real output arrives.
bool COutputPane::ClearScreen()
{
    if (LineCountI() == 0 || m_lineH <= 0) return false;

    int clientH = 0;
    GetClientSize(nullptr, &clientH);
    const int pageRows = clientH / m_lineH;
    if (pageRows <= 1) return false;      // nothing to push it up by

    // Where the last line starts, on the absolute row axis.
    const long long lastRow = RowStartOf(LineCountI() - 1);

    // Enough blank rows that `lastRow` survives Scroll()'s clamp to
    // (range - page).  Anything already reserved counts towards it.
    m_overscrollRows = std::max(m_overscrollRows, (long long) pageRows - 1);
    UpdateScrollbars(false);

    int unitH = 0;
    GetScrollPixelsPerUnit(nullptr, &unitH);
    if (unitH <= 0) return false;
    Scroll(0, pal::pixelsToScrollUnits(pal::rowsToPixels(lastRow, m_lineH), unitH));

    // The user has deliberately parked the view; auto-scroll would undo it on
    // the next line of output.  Same reasoning as ScrollToLine.
    const bool wasAuto = m_autoScroll;
    m_autoScroll = false;
    if (wasAuto && m_fn_autoscroll_changed) m_fn_autoscroll_changed(false);
    Refresh(false);
    return true;
}

void COutputPane::ScrollToLine(int lineIdx)
{
    if (LineCountI() == 0) return;
    lineIdx = std::max(0, std::min(LineCountI() - 1, lineIdx));
    int unitH;
    GetScrollPixelsPerUnit(nullptr, &unitH);
    if (unitH > 0) {
        const long long vr = RowStartOf(lineIdx);
        Scroll(0, pal::pixelsToScrollUnits(pal::rowsToPixels(vr, m_lineH), unitH));
    }
    bool wasAuto = m_autoScroll;
    m_autoScroll = false;
    if (wasAuto && m_fn_autoscroll_changed) m_fn_autoscroll_changed(false);
    Refresh(false);
}

// world.SetScroll's pixel form (2026-08-12): scroll so the given DEVICE-PIXEL
// Y is the top of the view.  The old route divided the pixel by the line
// height and passed the result — a visual-ROW ordinal — to ScrollToLine,
// which expects a LOGICAL line and re-maps through RowStartOf: with any
// wrapped line above the target the view landed below the requested pixel,
// and the GetInfo(296) round-trip drifted.  No quantisation here either;
// Windows scrolls to the raw pixel (ScrollToPosition).
void COutputPane::ScrollToPixelY(long y)
{
    if (LineCountI() == 0) return;
    int unitH;
    GetScrollPixelsPerUnit(nullptr, &unitH);
    if (unitH > 0)
        Scroll(0, pal::pixelsToScrollUnits(y < 0 ? 0 : y, unitH));
    // ScrollToPosition's tail, as in ScrollToTop above: gated on the world's
    // auto_pause, and — unlike the flat `m_autoScroll = false` this replaces —
    // it thaws when the requested pixel clamped to the buffer end, so
    // world.SetScroll(<past the end>) parks at the bottom with output running
    // rather than silently freezing the world.
    RefreshAutoScrollFromPosition();
    Refresh(false);
}

// ── SetFrozen ─────────────────────────────────────────────────────────────
// One place that writes the m_freeze twin from a computed value.  m_autoScroll
// is the INVERSE (see the header), so `frozen` here reads the way the Windows
// arms it mirrors do, and the change hook fires only on a real transition —
// the freeze pane redraws from it.
void COutputPane::SetFrozen(bool frozen)
{
    const bool wantAuto = !frozen;
    if (wantAuto == m_autoScroll) return;
    m_autoScroll = wantAuto;
    if (m_fn_autoscroll_changed) m_fn_autoscroll_changed(m_autoScroll);
}

// The visual row a selection endpoint sits on — see the header for why Windows
// needs no such conversion.  The column-to-row rule is the renderer's own
// (c / m_wrapCols, as in the search-highlight mapping in DrawLine), so a match
// found on the third wrapped row of a long line is scrolled to THAT row.
int COutputPane::SelectionRowOf(const SelPoint& p) const
{
    const int n = LineCountI();
    if (n == 0) return 0;
    const int line = std::max(0, std::min(n - 1, p.line));
    int row = RowStartOf(line);
    if (m_wrapCols > 0 && p.col > 0)
        row += std::min(p.col / m_wrapCols, std::max(0, WrapCount(line) - 1));
    return row;
}

// ── EnsureSelectionVisible ────────────────────────────────────────────────
// CMUSHView::EnsureSelectionVisible (mushview.cpp:5002), kept in the same shape
// so the two can be diffed:
//
//     int startline = (pt.y - pDoc->m_iPixelOffset) / pDoc->m_FontHeight + 1;
//     int endline   = ((pt.y + GetOutputWindowHeight ()) / pDoc->m_FontHeight) - 1;
//     if (startline < 0) startline = 0;
//     int lastline = pDoc->GetLastLine ();
//     if (endline > lastline) endline = lastline;
//     if (m_selend_line < startline || m_selstart_line > endline)
//       {
//       CPoint pt (0, m_selstart_line * pDoc->m_FontHeight -
//                     (GetOutputWindowHeight ()) / 2);
//       if (pt.y < 0) pt.y = 0;
//       lastline_pt.y = (lastline * pDoc->m_FontHeight) - GetOutputWindowHeight ();
//       if (lastline_pt.y < 0) lastline_pt.y = 0;
//       if (pt.y > lastline_pt.y) pt.y = lastline_pt.y;
//       else                      m_freeze = true;
//       ScrollToPosition (pt, App.m_bSmoothScrolling);
//       }
//
// Two details carry the whole behaviour, and neither is an accident:
//
//  (a) BOTH ends of the visible band are shrunk by one row — the `+ 1` on the
//      top and the `- 1` on the bottom — so a row only PARTLY on screen counts
//      as NOT visible and gets scrolled to.  Nothing rounds; that is the point.
//      Go To Line used the port's IsLineVisible instead, which is the
//      any-pixel-showing rule, so a half-visible target was left half-visible.
//
//  (b) The `else` on the clamp.  When centring the target would scroll past the
//      end of the buffer the position is clamped instead and the freeze is
//      SKIPPED — after which ScrollToPosition's own rule (`m_freeze = pt.y <
//      bottom`, and the clamped pt.y IS bottom) evaluates false.  So a jump to
//      something near the end parks at the end and leaves output running, while
//      a jump further back freezes so the user can read what they found.
//
// Note where the freeze is NOT gated: the `m_freeze = true` above runs whatever
// m_bAutoFreeze says, while ScrollToPosition's tail is entirely inside
// `if (pDoc->m_bAutoFreeze)`.  A selection jump therefore freezes even in a
// world with auto_pause off; a plain scroll in that world does not.
void COutputPane::EnsureSelectionVisible()
{
    if (m_lineH <= 0) return;                 // Windows: if (!pDoc->m_FontHeight)

    SelPoint s, e;
    if (!GetNormalizedSel(s, e)) return;      // nothing selected, nothing to show

    int unitH = 0;
    GetScrollPixelsPerUnit(nullptr, &unitH);
    if (unitH <= 0) return;

    const long long scrollY = GetScrollPositionY();
    const long long viewH   = TextViewportH();

    // (a) the FULLY visible band.
    long long startline = (scrollY - m_pixelOffset) / m_lineH + 1;
    long long endline   = (scrollY + viewH) / m_lineH - 1;
    if (startline < 0) startline = 0;

    // pDoc->GetLastLine(): the bottom of the scrollable extent, in rows.  The
    // port's row axis is the VISUAL one, and m_totalVisualRows is what
    // UpdateScrollbars builds the virtual size from, so the clamp below lands
    // exactly where Scroll() would have clamped anyway.  m_overscrollRows is
    // deliberately excluded: Windows has no such thing, and Clear Screen's
    // reserved blank rows are not somewhere a selection can be.
    const long long lastline = m_totalVisualRows;
    if (endline > lastline) endline = lastline;

    const long long selstart_row = SelectionRowOf(s);
    const long long selend_row   = SelectionRowOf(e);

    if (!(selend_row < startline || selstart_row > endline))
        return;                   // already fully visible: no scroll, no freeze

    // "scroll so bookmark is halfway down page"
    long long y = pal::rowsToPixels(selstart_row, m_lineH) - viewH / 2;
    if (y < 0) y = 0;

    long long lastline_y = pal::rowsToPixels(lastline, m_lineH) - viewH;
    if (lastline_y < 0) lastline_y = 0;

    bool freeze = !m_autoScroll;              // the m_freeze carried in

    if (y > lastline_y)
        y = lastline_y;           // (b) clamped to the end: do NOT freeze here
    else
        freeze = true;            // ...and this one is NOT gated on auto_pause

    Scroll(0, pal::pixelsToScrollUnits(y, unitH));

    // ScrollToPosition's tail (mushview.cpp:4598-4606).  `bottom` is
    // m_ScrollbarSizeTotal.cy - m_ScrollbarSizePage.cy, which is identically
    // (lastline * m_FontHeight) - GetOutputWindowHeight() — the UNCLAMPED form,
    // so a buffer shorter than the window gives a negative bottom and thaws.
    if (m_allowAutoFreeze) {
        const bool below = (y < pal::rowsToPixels(lastline, m_lineH) - viewH);
        freeze = m_keepPauseAtBottom ? (freeze || below) : below;
    }

    SetFrozen(freeze);
    Refresh(false);
}

// ── SelectLine — CMUSHView::SelectLine (mushview.cpp:5595) ────────────────
//     m_selstart_line = iLine;  m_selend_line = iLine + 1;
//     m_selstart_col  = 0;      m_selend_col  = 0;
//     SelectionChanged (); Invalidate (); EnsureSelectionVisible ();
// Go To Line and Go To Bookmark are both exactly this call on Windows, which is
// why Go To Bookmark now SELECTS the line it lands on instead of merely
// scrolling to it — and why the two can no longer disagree about when to move
// the viewport or when to freeze.
void COutputPane::SelectLine(int lineIdx)
{
    const int n = LineCountI();
    if (n == 0) return;
    lineIdx = std::max(0, std::min(n - 1, lineIdx));

    // SetSelectionRange is 1-based and reads column 0 as "the whole line", so
    // column 1 is the start-of-line those two want.  The last line has no next
    // one, so it takes the whole-line form directly.
    if (lineIdx + 1 < n)
        SetSelectionRange(lineIdx + 1, lineIdx + 2, 1, 1);
    else
        SetSelectionRange(lineIdx + 1, lineIdx + 1, 1, 0);

    Refresh(false);
    EnsureSelectionVisible();
}

// ── Bookmarks (Display ▸ Bookmark Selection / Go To Bookmark) ─────────────────
// The flag itself belongs to the engine's CLine; this is only the rendering
// mirror, updated when the doc tells us it changed.
void COutputPane::SetBookmark(int lineIdx, bool set)
{
    if (lineIdx < 0 || lineIdx >= LineCountI()) return;
    if (LineAt(lineIdx).bookmark == set) return;
    if (m_fn_fetch_line) {
        // PULL MODEL: the flag lives on the engine's CLine and CMUSHclientDoc::
        // Bookmark has already written it -- this seam only exists to tell the
        // view to repaint.  Drop the cached conversion so the next fetch picks
        // the new flag up; there is no view-side copy to patch.
        m_lineCache.erase(lineIdx);
    } else {
        m_lines[(size_t) lineIdx].bookmark = set;
    }
    Refresh(false);
}

// Which line Display ▸ Bookmark Selection acts on — the selection's start line,
// else the last line (CMUSHView::OnDisplayBookmarkselection, mushview.cpp).
int COutputPane::BookmarkTargetLine() const
{
    SelPoint s, e;
    if (GetNormalizedSel(s, e) && s.line >= 0 && s.line < LineCountI())
        return (int) s.line;
    if (LineCountI() == 0) return -1;
    return LineCountI() - 1;
}

void COutputPane::GotoNextBookmark()
{
    const int n = LineCountI();
    if (n == 0) return;

    // Where to scan FROM: the current selection's start line, read fresh on
    // every invocation, exactly as CMUSHView::OnDisplayGotobookmark reads
    // m_selstart_line (mushview.cpp:4862-4877) -- including its
    // `<= 0 || >= count` arm, which means "no usable origin, start at the head".
    //
    // It used to cycle from m_lastBookmarkLine, a member nothing but this
    // function ever wrote.  Two consequences, both silent: clicking or
    // selecting between two bookmarks did not move the origin (Windows jumps to
    // the next bookmark after the CLICK; the port jumped to wherever its last
    // jump had left off), and a head-trim slid every line index down while
    // leaving this one where it was -- DropFrontLines shifts m_selAnchor and
    // m_selFocus and knew nothing about it -- so after a trim the origin named
    // a different line than the one it had been set from.  Deriving it from the
    // selection closes both, and needs no separate index to keep in step.
    //
    // "Continue from the last jump" still falls out for free, because the
    // SelectLine below leaves the selection on the line it lands at.
    int origin = 0;
    {
        SelPoint s, e;
        if (GetNormalizedSel(s, e) && s.line > 0 && s.line < n)
            origin = s.line;
    }

    for (int i = 1; i <= n; ++i) {     // start just past the origin, and wrap
        const int idx = (origin + i) % n;
        if (LineAt(idx).bookmark) {
            // CMUSHView::OnDisplayGotobookmark ends `SelectLine (nNewLine)`
            // (mushview.cpp:4920) — the same call Go To Line makes.  This used
            // to be a bare ScrollToLine: no selection, no visibility guard and
            // an unconditional freeze.
            SelectLine(idx);
            return;
        }
    }
    // i == n revisited the origin itself, which is Windows' "if we didn't find
    // one *but* we were on one anyway, re-go to it" tail (mushview.cpp:4907).
}

void COutputPane::SetSelectionRange(int startLine, int endLine,
                                     int startCol, int endCol)
{
    if (startLine == 0 && endLine == 0 && startCol == 0 && endCol == 0) {
        // Clear selection.
        m_selAnchor = SelPoint{};
        m_selFocus  = SelPoint{};
        Refresh(false);
        NotifySelectionMaybeChanged();
        return;
    }

    // Nothing to select.  With an empty buffer both indices clamp to 0 and the
    // lineLen lambda below indexes m_lines[0] on an empty deque — world.Note
    // has not run yet, or world.DeleteOutput just emptied the pane, and a
    // script calling world.SetSelection reads past the end.
    //
    // Windows cannot reach this state (ClearOutput re-seeds one CLine,
    // doc.cpp:2358) and its setOutputSelection only stores the four indices
    // without touching the line list, so a selection over a nonexistent line is
    // simply not visible or copyable there.  Clearing the selection reproduces
    // that observable result.
    if (LineCountI() == 0) {
        m_selAnchor = SelPoint{};
        m_selFocus  = SelPoint{};
        Refresh(false);
        NotifySelectionMaybeChanged();
        return;
    }

    int n = LineCountI();
    // Convert 1-based → 0-based; clamp to buffer.
    int sl = std::max(0, std::min(n - 1, startLine - 1));
    int el = std::max(0, std::min(n - 1, endLine   - 1));

    // Column 0 in the API means "start / end of line".
    auto lineLen = [&](int li) { return LineCharLen(LineAt(li)); };
    int sc = (startCol <= 0) ? 0
           : std::min(lineLen(sl), startCol - 1);
    int ec = (endCol   <= 0) ? lineLen(el)
           : std::min(lineLen(el), endCol - 1);

    m_selAnchor = SelPoint{sl, sc};
    m_selFocus  = SelPoint{el, ec};
    Refresh(false);
    NotifySelectionMaybeChanged();
}

bool COutputPane::GetSelectionInfo(long& startLine, long& startCol,
                                    long& endLine,   long& endCol) const
{
    SelPoint s, e;
    if (!GetNormalizedSel(s, e)) return false;
    if (s == e) return false;  // empty / collapsed selection

    // Convert 0-based internal coords to 1-based Windows-compatible values.
    startLine = (long)s.line + 1;
    startCol  = (long)s.col  + 1;
    endLine   = (long)e.line + 1;
    endCol    = (long)e.col  + 1;
    return true;
}

void COutputPane::ScrollPage(int direction)
{
    int clientH;
    GetClientSize(nullptr, &clientH);
    int unitH;
    GetScrollPixelsPerUnit(nullptr, &unitH);
    if (unitH <= 0) return;

    int scrollX, scrollY;
    GetViewStart(&scrollX, &scrollY);
    int viewH = TextViewportH();   // text band height (honours world.TextRectangle)
    int pageUnits = std::max(1, viewH / unitH - 1);

    int newY;
    if (direction < 0)
        newY = std::max(0, scrollY - pageUnits);
    else
        newY = scrollY + pageUnits;   // Scroll() clamps to valid range

    Scroll(scrollX, newY);

    // Re-evaluate whether we're now at the bottom.
    RefreshAutoScrollFromPosition();
}

// See the header.  ScrollByRows deliberately leaves the freeze alone (a drag
// scroll is not a user scroll), so the ScrollToPosition tail is added here --
// the same pairing ScrollPage above uses.
void COutputPane::ScrollLine(int direction)
{
    if (direction == 0) return;
    ScrollByRows(direction < 0 ? -1 : +1);
    RefreshAutoScrollFromPosition();
}

// ── IsAtBufferEnd ─────────────────────────────────────────────────────────
// The m_bAtBufferEnd twin (mushview.cpp:3255): "is the scroll position at its
// maximum?", with no tolerance.  A separate question from the freeze flag,
// because the two really are separate — freezing moves no viewport, and a
// scroll does not always decide the freeze (see below).  The status line needs
// both, and it and RefreshAutoScrollFromPosition ask THIS one function, so they
// cannot drift apart about where the bottom is.
//
// See pal/scroll_geometry.h for why it asks the scrollbar rather than
// recomputing the document height: the two used to disagree by up to a row, and
// the fudge that hid that gave the pane a one-row dead zone at the bottom where
// a gentle scroll up was ignored and the next line of output snapped the view
// back down.
bool COutputPane::IsAtBufferEnd() const
{
    COutputPane* self = const_cast<COutputPane*>(this);
    int scrollY = 0;
    self->GetViewStart(nullptr, &scrollY);
    int unitH = 0;
    self->GetScrollPixelsPerUnit(nullptr, &unitH);
    int clientH = 0;
    self->GetClientSize(nullptr, &clientH);

    return pal::isScrolledToBottom(scrollY, unitH, clientH,
                                   self->GetVirtualSize().y);
}

// ── RefreshAutoScrollFromPosition ─────────────────────────────────────────
// The single answer to "should new output still drag the view down?", shared by
// every path the user can scroll through.
void COutputPane::RefreshAutoScrollFromPosition()
{
    // Windows, mushview.cpp:4598-4606 (CMUSHView::ScrollToPosition):
    //     if (pDoc->m_bAutoFreeze) {
    //       if (pDoc->m_bKeepFreezeAtBottom)
    //          m_freeze = m_freeze || (pt.y < bottom);
    //       else
    //          m_freeze = pt.y < bottom;
    //     }
    // Note where the outer `if` ENDS.  With the world's auto_pause OFF, a
    // scroll does not touch the freeze flag AT ALL — it neither freezes nor
    // thaws.  The port used to write
    //     m_autoScroll = (atBottom && !stayFrozen) || !m_allowAutoFreeze;
    // and that trailing `|| !m_allowAutoFreeze` FORCES the pane back to
    // auto-scrolling, so in a world with auto_pause off every explicit freeze —
    // Display ▸ Freeze Output, world.Pause(true), start_paused, the scroll-to-
    // top and find-match freezes — was cancelled by the next scroll event, and
    // there is no shortage of those.  That was the one place the port genuinely
    // conflated "the user froze it" with "where the viewport is".
    if (!m_allowAutoFreeze)
        return;

    const bool atBottom = IsAtBufferEnd();

    const bool wasAuto = m_autoScroll;
    // keep_pause_at_bottom (m_bKeepFreezeAtBottom) turns the assignment into a
    // LATCH: with the option on, merely ARRIVING at the bottom never unfreezes;
    // the user has to unfreeze explicitly (the hint bar / Pause command, which
    // go through ScrollToBottom and set m_autoScroll directly).  The port had no
    // reader for the option at all, so scrolling back down always resumed and
    // the next line of output dragged the view away again.
    const bool stayFrozen = m_keepPauseAtBottom && !m_autoScroll;
    m_autoScroll = atBottom && !stayFrozen;
    if (m_autoScroll != wasAuto && m_fn_autoscroll_changed)
        m_fn_autoscroll_changed(m_autoScroll);
}

// ── PULL MODEL: the read path ─────────────────────────────────────────────
// See the header.  When m_fn_fetch_line is wired the pane holds no line
// content at all -- m_lines stays EMPTY -- and every read comes from the
// engine's CLine through CMUSHclientDoc::BuildOutputLine, the same converter
// the push drain uses.
int COutputPane::LineCountI() const
{
    if (m_fn_line_count) return m_fn_line_count();
    return (int) m_lines.size();
}

// See the header.  The push model trims to its own cap; the pull model follows
// the engine, which has already done the deleting by the time we are told.
int COutputPane::DerivedTargetLength() const
{
    if (m_fn_line_count) return LineCountI();
    return m_maxLines > 0 ? m_maxLines : -1;
}

// One head-trim primitive for all three callers (the capacity trim in
// AppendLine, SetMaxLines, and DeleteHeadLines), so the pull-model branch
// exists once instead of three times.
//
// It is driven by the length of the WRAP CACHE, never by LineCountI(): when the
// engine calls DeleteHeadLines it has already dropped the lines, so its count is
// the post-delete one and using it here would trim nothing.
long COutputPane::DropFrontLines(long n, bool bAccumulateTrimmedRows)
{
    long dropped = 0;
    for (long i = 0; i < n && !m_visualRowStart.empty(); ++i) {
        if (bAccumulateTrimmedRows) {
            // Visual rows leaving the top, so UpdateScrollbars can compensate
            // the scroll position and the viewport does not jump to different
            // content.
            const long long firstStart = m_visualRowStart[0];
            const long long nextStart  = (m_visualRowStart.size() >= 2)
                             ? m_visualRowStart[1] : (m_rowBase + m_totalVisualRows);
            m_trimmedRowsRemoved += (int)(nextStart - firstStart);
        }
        // Only the push model has content of its own to drop.
        if (!m_fn_fetch_line && !m_lines.empty()) m_lines.pop_front();
        PopFrontWrapCache();
        ++dropped;

        // Selection indices shift down with the content; an anchor on the line
        // that just went is cleared.
        if (m_selAnchor.valid()) {
            if (m_selAnchor.line == 0) m_selAnchor = SelPoint{};
            else --m_selAnchor.line;
        }
        if (m_selFocus.valid()) {
            if (m_selFocus.line == 0) m_selFocus = SelPoint{};
            else --m_selFocus.line;
        }
    }
    if (dropped > 0) {
        NotifySelectionMaybeChanged();   // did_jump, mushview.cpp:2889-2900
        m_trimmedCount += (int) dropped;
        ++m_bufferGen;
        // Every cached line is keyed by INDEX, and they have all just shifted.
        InvalidateLineCache();
    }
    return dropped;
}

const SOutputLine& COutputPane::LineAt(int i) const
{
    static const SOutputLine kEmpty;
    if (!m_fn_fetch_line) {
        if (i < 0 || i >= (int) m_lines.size()) return kEmpty;
        return m_lines[(size_t) i];
    }
    if (i < 0) return kEmpty;

    auto it = m_lineCache.find(i);
    if (it != m_lineCache.end()) return it->second;

    SOutputLine built;
    if (!m_fn_fetch_line(i, built)) return kEmpty;

    // Auto-detect plain http:// / https:// URLs here as well as on the append
    // path.  Under the pull model the appended line's content is dropped and
    // re-fetched from the engine through m_fn_fetch_line, so the detection done
    // at AppendLine never reaches what is actually drawn or hit-tested; without
    // this, a bare URL in MUD output is neither underlined nor clickable, where
    // Windows makes it a link.  Splitting a run leaves the text -- and so the
    // character count the wrap cache is keyed on -- untouched.
    built = DetectURLs(std::move(built));

    // Evict oldest-first, never the entry we are about to hand back.  The cap
    // is far larger than any viewport, so a caller walking the visible rows
    // cannot evict a line it is still looking at.
    while ((int) m_lineCache.size() >= kLineCacheMin && !m_lineCacheOrder.empty()) {
        const int victim = m_lineCacheOrder.front();
        m_lineCacheOrder.pop_front();
        if (victim != i) m_lineCache.erase(victim);
    }
    m_lineCacheOrder.push_back(i);
    return m_lineCache.emplace(i, std::move(built)).first->second;
}

void COutputPane::InvalidateLineCache() const
{
    m_lineCache.clear();
    m_lineCacheOrder.clear();
}

void COutputPane::ClearOutput()
{
    m_lines.clear();
    ++m_bufferGen; InvalidateLineCache();
    // ...including the in-progress line.  The pending row is a port-only
    // concept (Windows' DELETE_LIST takes the partial line away with the rest),
    // and nothing else could ever clear it here: CMUSHclientDoc::ClearOutput
    // re-seeds m_pCurrentLine EMPTY (doc.cpp), and the drain only emits a
    // pending line when `m_pCurrentLine->len > 0` (doc_display_shared.cpp), so
    // there is no follow-up push carrying the empty state.  A MUD prompt with no
    // newline ("HP:100 MP:50 > ") therefore survived Display ▸ Clear Output and
    // sat alone at the top of an otherwise empty pane — indefinitely, if the
    // world was idle or disconnected.
    m_pendingLine.clear();
    m_pendingLine.pending = false;
    m_selAnchor          = SelPoint{};
    m_selFocus           = SelPoint{};
    NotifySelectionMaybeChanged();       // ClearOutput's clear notifies too
    m_maxLineChars       = 80;
    m_trimmedCount       = 0;
    m_trimmedRowsRemoved = 0;
    m_visualRowStart.clear();
    m_lineChars.clear();
    m_rowBase = 0;
    m_totalVisualRows    = 0;
    m_overscrollRows     = 0;   // no last line left to hold at the top
    UpdateScrollbars(true);
    Refresh();
}

// World option line_spacing.  Windows applies it inside ChangeFont --
// `if (iLineSpacing) m_FontHeight = iLineSpacing;` (doc.cpp:1179) -- where
// m_FontHeight is the row pitch CMUSHView paints with.  The port has no
// CMUSHView; the wx pane owns the pitch and derives it from the font, so the
// option round-tripped through the dialog, the XML and GetOption while nothing
// on this side ever read it and the spacing never budged.
//
// 0 means "use the font's own height", which is the option's documented default
// and what the field says in World Properties ("0 = use font default").
void COutputPane::SetLineSpacing(int px)
{
    if (px < 0) px = 0;
    if (px == m_lineSpacing) return;
    m_lineSpacing = px;
    // Re-derive the pitch through the same path a font change takes, so the
    // scroll rate, wrap cache and scrollbars are all rebuilt against it.
    SetOutputFont(m_font);
}

void COutputPane::SetOutputFont(const wxFont& font)
{
    m_font     = font;
    m_fontSize = font.GetPointSize();
    SetFont(m_font);
    RebuildFontVariants(m_font, m_fontBold, m_fontUnderline, m_fontBoldUnderline,
                        m_fontItalic, m_fontBoldItalic,
                        m_fontItalicUnderline, m_fontBoldItalicUnderline);

    // Use wxClientDC here — the window already exists at this point (SetOutputFont
    // is called from wx_world_child.cpp after creation).  wxClientDC inherits the
    // window's DPI and scale factor, so GetTextExtent returns logical pixel widths
    // that match what DrawText will actually render.  A bitmap-backed wxMemoryDC
    // (used in the constructor) may report physical pixel widths on Retina/HiDPI
    // displays (2× the logical width), causing selection rectangles and click-to-
    // character mapping to be off by a factor of ~2.
    wxCoord w = 0, h = 0;
    {
        wxClientDC cdc(this);
        cdc.SetFont(m_font);
        cdc.GetTextExtent("M", &w, &h);
    }
    // Fallback: if wxClientDC didn't give useful metrics (can happen on some GTK
    // configurations before the first paint), retry with a bitmap-backed MemoryDC.
    if (w <= 0 || h <= 0) {
        wxBitmap bmp(1, 1, 24);
        wxMemoryDC mdc(bmp);
        mdc.SetFont(m_font);
        mdc.GetTextExtent("M", &w, &h);
    }
    if (h > 0) m_lineH = (m_lineSpacing > 0) ? m_lineSpacing : h + 2;
    if (w > 0) m_charW = w;

    m_maxLineChars = 80;
    // Keep horizontal scrolling disabled (same as constructor).
    SetScrollRate(0, m_lineH);
    RebuildWrapCache();
    UpdateScrollbars(m_autoScroll);
    Refresh();

    if (m_fn_grid_changed) m_fn_grid_changed();
}

int COutputPane::AdjustFontSize(int delta)
{
    int newSize = std::max(kMinFontSize,
                  std::min(kMaxFontSize, m_fontSize + delta));
    if (newSize == m_fontSize) return m_fontSize;

    m_fontSize = newSize;
    wxFont f(m_fontSize, m_font.GetFamily(), m_font.GetStyle(),
             m_font.GetWeight(), m_font.GetUnderlined(),
             m_font.GetFaceName());
    SetOutputFont(f);
    return m_fontSize;
}

void COutputPane::ResetFontSize()
{
    AdjustFontSize(kDefaultFontSize - m_fontSize);
}

// ── Paint ─────────────────────────────────────────────────────────────────────

// ── character metrics measured from a live DC ─────────────────────────────
// The HiDPI correction.  On macOS the wxMemoryDC used during construction can
// report PHYSICAL pixel widths (2x logical) while the paint DC works in logical
// pixels; left uncorrected, selection rectangles and click-to-char mapping are
// off by the scale factor.
//
// Split out of OnPaint, and not for tidiness: the paint handler never runs in
// an in-app selftest (every one exits before wxApp::OnRun, so AppKit never
// issues drawRect: -- see docs/AUDIT-2026-08.md), so anything left inline there
// has no coverage at all.  These two are the engine-shaped halves -- the
// decision and the reflow -- and both are callable directly.
//
// Returns true when the measurement DIFFERS from what the pane is using, i.e.
// when the caller should reflow.  A non-positive measurement is refused: a DC
// that cannot measure text must not be allowed to set the grid to zero, which
// would divide by zero in the wrap arithmetic.
bool COutputPane::AdoptMeasuredMetrics(int dcW, int dcH)
{
    if (dcW <= 0 || dcH <= 0) return false;
    // line_spacing wins over the measurement, the same way it wins in
    // SetOutputFont.  This is the THIRD place the row pitch is set and the only
    // one the paint reaches, so missing it here made the option look live for
    // exactly as long as it took the pane to repaint -- GetInfo(212) reported
    // the new pitch while the screen kept the old one.  (Not caught by the
    // selftests: they exit before OnRun, so no paint ever happens there.)
    const int newLineH = (m_lineSpacing > 0) ? m_lineSpacing
                                             : dcH + 2;   // 1px leading each side
    if (dcW == m_charW && newLineH == m_lineH) return false;
    m_charW = dcW;
    m_lineH = newLineH;
    return true;
}

// Everything that has to follow a grid change.  The scroll rate is in the new
// row height, the wrap cache is recomputed against the new character width, and
// the frame is told so the ENGINE's wrap column can follow -- without that last
// step the pane and the engine disagree about how wide a line is, and every
// full-width line gains a one-character orphan continuation row.
void COutputPane::ApplyMetricsChange()
{
    SetScrollRate(0, m_lineH);
    RebuildWrapCache();
    UpdateScrollbars(m_autoScroll);
    Refresh();
    if (m_fn_grid_changed) m_fn_grid_changed();
}

// ── the plugin text rectangle, adopted ────────────────────────────────────
// The same shape as AdoptMeasuredMetrics above, and split out of OnPaint for
// the same reason: this comparison is the ONLY thing in the pane that notices a
// script narrowed the text band (world.TextRectangle is polled, not signalled),
// and inline in the paint handler it had no coverage at all.
//
// The l/t zeroing on the inactive path is belt-and-braces, carried over
// verbatim: TextRect already returns l = t = 0 from every one of its false
// paths, so no test can tell whether this line is here (the mutation that
// deletes it survives by construction).  It stays because the values become the
// text origin OnPaint draws at, and that should not silently depend on
// TextRect's out-parameters for a rectangle it just said was not set.
// ── world.TextRectangle decorations (2026-08-12) ─────────────────────────────
// Windows paints an outside fill and a border around the reserved text area
// (CMUSHView::OnDraw, mushview.cpp:926-990); the port honoured only the
// geometry, so layout plugins that frame the output on Windows showed a bare
// text area here.  Style mapping is ValidateBrushStyle's (miniwindow.cpp:438):
// hatches paint in the BORDER colour over an opaque ground of the fill colour
// (the SetBkColor result under OPAQUE bkmode); patterns approximate as
// cross-hatch.  Both rects inflate by offset+width, as Windows' two
// InflateRect calls total.
void COutputPane::DrawTextRectOutsideFill(wxDC& dc, int clientW, int clientH,
                                          const wxColour& bgCol)
{
    int l = 0, t = 0, r = 0, b = 0;
    STextRectDecor d;
    if (!TextRect(l, t, r, b) || !m_fn_text_rect_decor || !m_fn_text_rect_decor(d))
        return;
    if (d.outsideFillStyle == 1 /*BS_NULL*/) return;

    auto col = [](uint32_t c) {
        return wxColour(GetRValue(c), GetGValue(c), GetBValue(c));
    };
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(col(d.outsideFillColour)));
    dc.DrawRectangle(0, 0, clientW, clientH);
    if (d.outsideFillStyle != 0 /*BS_SOLID*/) {
        static const wxBrushStyle kHatch[] = {
            wxBRUSHSTYLE_HORIZONTAL_HATCH, wxBRUSHSTYLE_VERTICAL_HATCH,
            wxBRUSHSTYLE_FDIAGONAL_HATCH,  wxBRUSHSTYLE_BDIAGONAL_HATCH,
            wxBRUSHSTYLE_CROSS_HATCH,      wxBRUSHSTYLE_CROSSDIAG_HATCH };
        const int h = (d.outsideFillStyle >= 2 && d.outsideFillStyle <= 7)
                          ? d.outsideFillStyle - 2
                          : 4;   // patterns (8+): cross-hatch approximation
        dc.SetBrush(wxBrush(col(d.borderColour), kHatch[h]));
        dc.DrawRectangle(0, 0, clientW, clientH);
    }
    // The reserved area itself returns to the normal background — Windows'
    // inner FillRect with the back brush.
    const int inf = d.borderOffset + d.borderWidth;
    dc.SetBrush(wxBrush(bgCol));
    dc.DrawRectangle(l - inf, t - inf, (r - l) + 2 * inf, (b - t) + 2 * inf);
}

void COutputPane::DrawTextRectBorder(wxDC& dc)
{
    int l = 0, t = 0, r = 0, b = 0;
    STextRectDecor d;
    if (!TextRect(l, t, r, b) || !m_fn_text_rect_decor || !m_fn_text_rect_decor(d))
        return;
    if (d.borderWidth <= 0) return;

    dc.SetPen(wxPen(wxColour(GetRValue(d.borderColour), GetGValue(d.borderColour),
                             GetBValue(d.borderColour)),
                    d.borderWidth));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    const int inf = d.borderOffset + d.borderWidth;
    dc.DrawRectangle(l - inf, t - inf, (r - l) + 2 * inf, (b - t) + 2 * inf);
}

void COutputPane::RenderTextRectDecorForTest(wxDC& dc, int clientW, int clientH,
                                             const wxColour& bgCol)
{
    DrawTextRectOutsideFill(dc, clientW, clientH, bgCol);
    DrawTextRectBorder(dc);
}

bool COutputPane::AdoptTextRect()
{
    int trL, trT, trR, trB;
    const bool trActive = TextRect(trL, trT, trR, trB);
    if (!trActive) { trL = 0; trT = 0; }

    if (trActive == m_trActive && trL == m_trL && trT == m_trT &&
        trR == m_trR && trB == m_trB)
        return false;

    m_trActive = trActive; m_trL = trL; m_trT = trT; m_trR = trR; m_trB = trB;
    return true;
}

void COutputPane::OnPaint(wxPaintEvent&)
{
    // Windows counts redraws of the output window in CMUSHView::OnDraw
    // (mushview.cpp:900) and world.GetInfo(295) reports the total; there is no
    // CMUSHView here, so this paint is the equivalent moment.
    if (m_fn_painted) m_fn_painted();

    // wxAutoBufferedPaintDC uses native double-buffering when the platform
    // already provides it (macOS Cocoa, GTK3) and a manual backbuffer only when
    // needed.  wxBufferedPaintDC always allocates a bitmap, which on Retina
    // displays produces a blurry 1× bitmap blown up to 2× physical pixels.
    wxAutoBufferedPaintDC dc(this);
    PrepareDC(dc);

    // Re-measure character metrics from the live rendering DC.  On macOS/HiDPI
    // the wxMemoryDC used during construction may report physical pixel widths
    // (2× logical), while the paint DC works in logical pixels.  If the stored
    // m_charW / m_lineH don't match the DC, selection rectangles and click-to-
    // char mapping are off by the scale factor.  Rebuilding the wrap cache is
    // deferred via CallAfter so it doesn't happen inside the paint handler itself.
    {
        dc.SetFont(m_font);
        wxCoord dcW = 0, dcH = 0;
        dc.GetTextExtent("M", &dcW, &dcH);
        // Defer the reflow so we don't invalidate layout mid-paint.
        if (AdoptMeasuredMetrics(dcW, dcH))
            CallAfter([this]() { ApplyMetricsChange(); });
    }

    wxColour bgCol(GetRValue(m_bgColor), GetGValue(m_bgColor), GetBValue(m_bgColor));
    dc.SetBackground(wxBrush(bgCol));
    dc.Clear();

    int scrollX, scrollY;
    GetViewStart(&scrollX, &scrollY);
    int unitW, unitH;
    GetScrollPixelsPerUnit(&unitW, &unitH);

    int clientW, clientH;
    GetClientSize(&clientW, &clientH);

    int yPixOffset = scrollY * unitH;

    // ── TextRectangle outside fill (under the background image) ───────────
    // Windows' order: outside fill + inner re-fill, THEN the background
    // image, THEN the border (below).  Viewport-anchored, like the image.
    if (m_fn_text_rect_decor) {
        dc.SetDeviceOrigin(0, 0);
        DrawTextRectOutsideFill(dc, clientW, clientH, bgCol);
        dc.SetDeviceOrigin(-scrollX * unitW, -scrollY * unitH);
    }

    // ── Background image (SetBackgroundImage) ────────────────────────────
    // Right after the colour fill, under everything — the Windows OnDraw
    // order. Viewport-anchored (the Windows view has no scroll transform).
    if (m_fn_draw_background_image) {
        dc.SetDeviceOrigin(0, 0);
        m_fn_draw_background_image(dc, clientW, clientH);
    }

    // ── TextRectangle border (over the background image) ──────────────────
    if (m_fn_text_rect_decor) {
        dc.SetDeviceOrigin(0, 0);
        DrawTextRectBorder(dc);
        dc.SetDeviceOrigin(-scrollX * unitW, -scrollY * unitH);
    }

    // ── Text rectangle (world.TextRectangle) ──────────────────────────────
    // A plugin may reserve part of the output area for a docked miniwindow;
    // the text then shifts to its top-left and wraps within its width.
    // If the rectangle changed since the last paint, rebuild the wrap cache
    // (deferred so we don't reflow layout mid-paint).
    if (AdoptTextRect()) {
        CallAfter([this]() {
            RebuildWrapCache();
            UpdateScrollbars(m_autoScroll);
            Refresh();
        });
    }
    // Draw against what was just stored: unchanged or not, the members now hold
    // this paint's rectangle.
    const bool trActive = m_trActive;
    const int  trL = m_trL, trT = m_trT, trR = m_trR, trB = m_trB;

    // ── Miniwindows: draw-underneath pass ─────────────────────────────────
    if (m_fn_composite_miniwindows) {
        dc.SetDeviceOrigin(0, 0);
        m_fn_composite_miniwindows(dc, clientW, clientH, /*bUnder=*/true);
        dc.SetDeviceOrigin(-scrollX * unitW, -scrollY * unitH);
    }

    // ── ON_PLUGIN_DRAW_OUTPUT_WINDOW (docs/PAL.md S4c) ─────────────────────
    // Give plugins a chance to update miniwindows to match the visible text —
    // fired between the underneath composite and the text pass, exactly where
    // Windows' OnDraw fires it.  The callback runs script; FirePluginDraw holds
    // the re-entrancy window open across it so AppendLine defers any prints it
    // causes until this paint completes.  The wiring test stays here rather
    // than relying on FirePluginDraw's own so YToLine(0) — a wrap-cache walk —
    // is not paid on every paint of every world that has no such plugin.
    if (m_fn_draw_output_window)
        FirePluginDraw(YToLine(0), yPixOffset);

    // Shift the text-drawing origin to the text rectangle's top-left so output
    // (and its selection highlight, which DrawLine renders at the same origin)
    // avoids any docked miniwindows.  No-op when no rectangle is set.
    //
    // m_pixelOffset ("pixel offset of text from side of window") is added here
    // rather than to each DrawText x, so text, selection rectangles, search
    // highlights and the pending line all stay in lockstep automatically.  Only
    // the few things deliberately drawn against the TRUE window edge have to
    // compensate; DrawLine names them.  Everything after the text pass resets
    // the origin to (0,0), so background/foreground images, miniwindows and the
    // hint bar are unaffected -- which matches Windows, where the offset applies
    // to the text pass alone.
    dc.SetDeviceOrigin(trL + m_pixelOffset - scrollX * unitW,
                       trT - scrollY * unitH);

    // Clip output (and its selection highlight) to the text rectangle so nothing
    // spills past its edges — in particular below the bottom margin.  Device
    // coordinates, so the shifted origin above doesn't affect it.  Reset after
    // the text + pending line, before the miniwindow over-pass.
    if (trActive)
        dc.SetDeviceClippingRegion(wxRegion(trL, trT, trR - trL, trB - trT));

    m_anyFading = false;      // recomputed by the DrawLine calls below
    if (LineCountI() != 0 && m_lineH > 0) {
        // Rows are drawn within the text viewport: the rectangle's height when a
        // plugin reserved one, else the client (less the hint bar when showing).
        int usableH = trActive ? (trB - trT)
                    : (m_autoScroll ? clientH : clientH - kHintBarH);

        // Map visible pixel range → visible logical lines using the wrap cache.
        int firstVR = yPixOffset / m_lineH;
        int lastVR  = (yPixOffset + usableH + m_lineH - 1) / m_lineH;
        int firstLine = VisualRowToLogical(firstVR);
        int lastLine  = VisualRowToLogical(lastVR);
        firstLine = std::max(0, firstLine);
        lastLine  = std::min(LineCountI() - 1, lastLine);

        dc.SetFont(m_font);

        for (int i = firstLine; i <= lastLine; ++i) {
            // yOff of the first visual row of logical line i.
            int lineVR = RowStartOf(i);
            DrawLine(dc, i, lineVR * m_lineH);
        }
    }

    // ── In-progress (pending) line ─────────────────────────────────────────
    // The telnet engine's m_pCurrentLine is emitted each timer tick as a
    // pending SOutputLine and rendered here as a dim trailing row.  It shows
    // partial server output before the '\n' arrives (useful for MUDs that send
    // a prompt without a trailing newline, e.g. "> ").
    if (!m_pendingLine.empty() && m_lineH > 0) {
        // virtual y, just after committed lines (64-bit product, saturated:
        // a device coordinate is an int whatever the row count is)
        int pendingY = pal::clampToDeviceExtent(
                           pal::rowsToPixels(m_totalVisualRows, m_lineH));
        int clientTop = scrollY * unitH;
        if (pendingY >= clientTop && pendingY < clientTop + clientH)
            DrawPendingRow(dc, pendingY);
    }

    if (trActive)
        dc.DestroyClippingRegion();   // text drawing done; miniwindows aren't clipped

    // ── Foreground image (SetForegroundImage) ─────────────────────────────
    // After the text, before the on-top miniwindows — the Windows OnDraw order.
    if (m_fn_draw_foreground_image) {
        dc.SetDeviceOrigin(0, 0);
        m_fn_draw_foreground_image(dc, clientW, clientH);
    }

    // ── Miniwindows: draw-on-top pass ──────────────────────────────────────
    if (m_fn_composite_miniwindows) {
        dc.SetDeviceOrigin(0, 0);
        m_fn_composite_miniwindows(dc, clientW, clientH, /*bUnder=*/false);
    }

    // ── "Scroll to bottom" hint bar ────────────────────────────────────────
    // When the user has scrolled up and new lines are arriving, paint a
    // yellow banner at the bottom of the visible client area.  Clicking it
    // (handled in OnLeftDown) or pressing Escape in the input bar snaps the
    // view back to the live bottom.  Drawn after miniwindows so it's always
    // on top.  Uses client coordinates (device origin reset to 0,0).
    if (!m_autoScroll && LineCountI() != 0) {
        dc.SetDeviceOrigin(0, 0);

        // Background bar.
        dc.SetBrush(wxBrush(wxColour(255, 200, 0)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, clientH - kHintBarH, clientW, kHintBarH);

        // Centred label.
        dc.SetFont(m_font);
        dc.SetTextForeground(wxColour(30, 30, 30));
        dc.SetBackgroundMode(wxTRANSPARENT);
        wxString msg =
            wxT("↓  New output below — click here or press Escape to scroll down");
        wxCoord tw, th;
        dc.GetTextExtent(msg, &tw, &th);
        dc.DrawText(msg,
                    (clientW - tw) / 2,
                    clientH - kHintBarH + (kHintBarH - th) / 2);
    }
}

// ── DrawPendingRow ────────────────────────────────────────────────────────────
// The in-progress row, drawn through the same DrawRunChunk every committed line
// goes through.
//
// It used to be hand-painted here -- one DrawText per run, advancing x by the
// font's natural extents -- which quietly dropped everything DrawRunChunk
// carries: the run's own BACKGROUND colour (the fill was always the pane's),
// the bold / italic / underline font variant, the strikethrough rule, and the
// fixed character-cell grid.  A prompt is exactly the kind of line a MUD
// colours, so the dim row disagreed with the committed line it turned into a
// moment later, and its glyphs sat off the grid the selection rectangle and
// click hit-testing are computed on.
//
// The dim itself is port design (Windows draws its in-progress line like any
// other) and is kept: blend both colours halfway toward the pane background.
// That is done to a COPY of the run rather than through m_paintOpacity, because
// the opacity is TranslateColour's and scales toward BLACK -- which on a
// light-background world would darken the row and paint a grey band behind it
// instead of fading it out.
void COutputPane::DrawPendingRow(wxDC& dc, int yOff)
{
    // m_paintOpacity is DrawLine's scratch state and still holds whatever the
    // last line painted with.  The pending row is the newest content there is,
    // so it never fades.
    m_paintOpacity = 1.0;

    const int bgR = GetRValue(m_bgColor), bgG = GetGValue(m_bgColor),
              bgB = GetBValue(m_bgColor);
    auto dim = [&](uint32_t c) -> uint32_t {
        return RGB((GetRValue(c) + bgR) / 2,
                   (GetGValue(c) + bgG) / 2,
                   (GetBValue(c) + bgB) / 2);
    };

    int x = 0;
    for (const auto& run : m_pendingLine) {
        const wxString text = DecodeRunText(run);
        if (text.IsEmpty()) continue;
        SOutputRun dimmed = run;          // colours only; every flag rides along
        dimmed.fg = dim(run.fg);
        dimmed.bg = dim(run.bg);
        DrawRunChunk(dc, text, x, yOff, dimmed, false);
        x += (int) text.length() * m_charW;
    }
}

// ── DrawLine ──────────────────────────────────────────────────────────────────

// See the header: the OnInit-scoped selftests can never receive a paint, so
// this is the seam that lets them assert on PIXELS anyway.
void COutputPane::RenderLineForTest(wxDC& dc, int lineIdx, int yOff)
{
    DrawLine(dc, lineIdx, yOff);
}

// ...and the same for the in-progress row, which no committed-line render can
// reach.
void COutputPane::RenderPendingRowForTest(wxDC& dc, int yOff)
{
    DrawPendingRow(dc, yOff);
}

void COutputPane::DrawLine(wxDC& dc, int lineIdx, int yOff)
{
    // The output-buffer fade: one opacity for the whole line, worked out by the
    // frame (it owns the doc, which holds the three fade options, the
    // fade-cancelled time and the freeze state).  Anything below full intensity
    // means something is still moving, which is what AnyLineFading() reports.
    m_paintOpacity = 1.0;
    if (m_fn_line_opacity && lineIdx >= 0 && lineIdx < LineCountI()) {
        m_paintOpacity = m_fn_line_opacity(LineAt(lineIdx).arrived);
        if (m_paintOpacity < 1.0) m_anyFading = true;
    }

    // The PREVIOUS line's arrival time, for the preamble's %D delta — fetched
    // BEFORE the line being drawn, and kept as a time_t rather than a
    // reference.  LineAt caches by index and evicts the oldest entry when it
    // is full, so a second fetch inside the paint could free the very line
    // this function is holding: the cache is sized so a viewport's walk cannot
    // do that, but lineIdx-1 walks BACKWARDS out of the region already cached,
    // which is exactly the eviction the sizing does not cover.
    const time_t prevArrived =
        (lineIdx > 0) ? LineAt(lineIdx - 1).arrived : (time_t) 0;

    const SOutputLine& line = LineAt(lineIdx);

    // ── Horizontal rule ───────────────────────────────────────────────────────
    if (line.size() == 1 && line[0].horiz_rule) {
        int clientW, clientH;
        GetClientSize(&clientW, &clientH);
        int scrollX, scrollY;
        GetViewStart(&scrollX, &scrollY);
        int unitW, unitH;
        GetScrollPixelsPerUnit(&unitW, &unitH);
        // The origin is shifted right by m_pixelOffset, so the true window edges
        // are at -m_pixelOffset and clientW - m_pixelOffset in these coordinates.
        // The row's background covers the full window width; the rule itself is
        // inset from the text edges, as on Windows (mushview.cpp:1460-1464 insets
        // it by m_iPixelOffset + 10 on both sides).
        int xLeft  = scrollX * unitW - m_pixelOffset;
        int xRight = xLeft + clientW;
        int midY   = yOff + m_lineH / 2;

        dc.SetPen(wxPen(wxColour(96, 96, 96), 1));
        dc.DrawLine(xLeft + m_pixelOffset + 4, midY,
                    xRight - m_pixelOffset - 4, midY);

        wxColour bgC(GetRValue(m_bgColor), GetGValue(m_bgColor), GetBValue(m_bgColor));
        dc.SetBrush(wxBrush(bgC));
        dc.SetPen(*wxTRANSPARENT_PEN);
        if (midY > yOff)
            dc.DrawRectangle(xLeft, yOff, clientW, midY - yOff);
        if (midY + 1 < yOff + m_lineH)
            dc.DrawRectangle(xLeft, midY + 1, clientW, yOff + m_lineH - midY - 1);
        return;
    }

    // ── Shared geometry ───────────────────────────────────────────────────────
    int clientW;
    GetClientSize(&clientW, nullptr);
    // Horizontal scrolling is disabled, so the right edge is always clientW —
    // less m_pixelOffset, because OnPaint shifted the origin right by that much
    // and Windows bleeds to the true client right edge (mushview.cpp:1439 uses
    // cr.right, i.e. the offset insets the LEFT only, never the right).
    const int rightEdge = clientW - m_pixelOffset;
    wxColour bgC(GetRValue(m_bgColor), GetGValue(m_bgColor), GetBValue(m_bgColor));
    auto RunBg = [](const SOutputRun& r) {
        return wxColour(GetRValue(r.bg), GetGValue(r.bg), GetBValue(r.bg));
    };
    const int wrapCols = std::max(1, m_wrapCols);

    // ── Bleed to left ─────────────────────────────────────────────────────────
    // With a pixel offset there is now a real left margin, and Windows fills it
    // with the FIRST run's background when "bleed background colour to edge" is
    // on (mushview.cpp:1335-1388, styleList.GetHead()).  Drawn before the text,
    // across every visual row of this logical line.
    if (m_bleedBackground && m_pixelOffset > 0 && !line.empty()) {
        dc.SetBrush(wxBrush(RunBg(line.front())));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(-m_pixelOffset, yOff,
                         m_pixelOffset, WrapCount(lineIdx) * m_lineH);
    }

    // ── Compute selection range for this line ─────────────────────────────────
    SelPoint selStart, selEnd;
    bool hasSel = GetNormalizedSel(selStart, selEnd);
    bool lineInSel = hasSel &&
                     lineIdx >= selStart.line &&
                     lineIdx <= selEnd.line;

    // Selection boundaries as absolute char offsets within this logical line.
    // selColEnd == INT_MAX means "selection continues past end of line".
    int selColStart = 0;
    int selColEnd   = INT_MAX;
    if (lineInSel) {
        selColStart = (lineIdx == selStart.line) ? selStart.col : 0;
        selColEnd   = (lineIdx == selEnd.line)   ? selEnd.col   : INT_MAX;
    }

    // ── Render runs with visual wrapping ─────────────────────────────────────
    // x:          pixel x within current visual row (horizontal scroll = 0 always)
    // currentCol: character column within current visual row [0, wrapCols)
    // currentY:   y-pixel top of current visual row
    // globalChar: absolute char offset in this logical line (for selection)
    int x          = 0;
    int currentCol = 0;
    int currentY   = yOff;
    int globalChar = 0;

    // ── Line preamble (timestamps) ────────────────────────────────────────────
    // Drawn in the plain font on the line's FIRST visual row; the text starts
    // after it.  Whole character cells (the pane is monospace), so XToChar's
    // PreambleCells subtraction stays exact — the port's m_iPreambleOffset.
    // Colours go through the same TranslateColour as the runs, so the fade
    // dims the stamp with its line.
    if (m_fn_line_preamble) {
        SLinePreamble pre;
        if (m_fn_line_preamble(line, prevArrived, lineIdx == 0, pre) &&
            !pre.text.empty()) {
            uint32_t rawFg = pre.fg, rawBg = pre.bg;
            if (m_fn_translate_colour) {
                rawFg = m_fn_translate_colour(rawFg, m_paintOpacity);
                rawBg = m_fn_translate_colour(rawBg, m_paintOpacity);
            }
            const int preW = (int) pre.text.length() * m_charW;
            dc.SetBrush(wxBrush(wxColour(GetRValue(rawBg), GetGValue(rawBg),
                                         GetBValue(rawBg))));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(x, currentY, preW, m_lineH);
            dc.SetFont(m_font);
            dc.SetBackgroundMode(wxTRANSPARENT);
            dc.SetTextForeground(wxColour(GetRValue(rawFg), GetGValue(rawFg),
                                          GetBValue(rawFg)));
            dc.DrawText(pre.text, x, currentY);
            x += preW;
        }
    }

    for (const auto& run : line) {
        if (run.text.empty()) continue;

        wxString wxText = DecodeRunText(run);
        int runLen = (int)wxText.size();
        int pos    = 0;

        while (pos < runLen) {
            // How many chars fit on the current visual row.
            int avail = wrapCols - currentCol;
            int chunk = std::min(avail, runLen - pos);

            // Selection for this sub-chunk (absolute char range).
            int chunkAbs0 = globalChar + pos;
            int chunkAbs1 = chunkAbs0 + chunk;
            bool inSel    = lineInSel
                            && chunkAbs1 > selColStart
                            && chunkAbs0 < (selColEnd == INT_MAX ? INT_MAX : selColEnd);

            if (!inSel) {
                // Whole sub-chunk: no selection overlap.
                DrawRunChunk(dc, wxText.Mid(pos, chunk), x, currentY, run, false);
                x += chunk * m_charW;
            } else {
                // Split into up to three sub-segments: before / inside / after selection.
                int lo = std::max(0, selColStart - chunkAbs0);
                int hi = std::min(chunk, (selColEnd == INT_MAX ? chunk
                                                               : selColEnd - chunkAbs0));
                if (lo > 0) {
                    DrawRunChunk(dc, wxText.Mid(pos, lo), x, currentY, run, false);
                    x += lo * m_charW;
                }
                if (hi > lo) {
                    DrawRunChunk(dc, wxText.Mid(pos + lo, hi - lo), x, currentY, run, true);
                    x += (hi - lo) * m_charW;
                }
                if (hi < chunk) {
                    DrawRunChunk(dc, wxText.Mid(pos + hi, chunk - hi), x, currentY, run, false);
                    x += (chunk - hi) * m_charW;
                }
            }

            currentCol += chunk;
            pos        += chunk;

            // Wrap to next visual row when this row is full and more text follows.
            if (currentCol >= wrapCols && pos < runLen) {
                // Fill remainder of this row with background.  With "bleed
                // background colour to edge" the run that just wrapped owns the
                // gap, so a coloured line stays coloured to the margin.
                if (x < rightEdge) {
                    dc.SetBrush(wxBrush(m_bleedBackground ? RunBg(run) : bgC));
                    dc.SetPen(*wxTRANSPARENT_PEN);
                    dc.DrawRectangle(x, currentY, rightEdge - x, m_lineH);
                }
                currentCol = 0;
                x          = 0;
                currentY  += m_lineH;
            }
        }

        globalChar += runLen;
    }

    // Fill the remainder of the last visual row.
    // When the selection extends past the end of this line (selColEnd == INT_MAX
    // means "selected through to the next line"), fill with the system highlight
    // colour so the selection band stretches to the right margin — this is the
    // standard visual for multi-line selections.
    if (x < rightEdge) {
        bool trailSelected = lineInSel && (selColEnd == INT_MAX);
        // Selection wins over bleed, so the selection band still looks like one.
        wxColour fillCol = trailSelected
            ? wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT)
            : ((m_bleedBackground && !line.empty()) ? RunBg(line.back()) : bgC);
        dc.SetBrush(wxBrush(fillCol));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(x, currentY, rightEdge - x, m_lineH);
    }

    // ── Search term: left-margin marker + in-line text highlight ─────────────
    if (!m_searchTerm.IsEmpty()) {
        // This runs for EVERY visible line on EVERY paint while a search term is
        // set.  It used to decode the whole line into a wxString, allocate a
        // second one from .Lower(), then convert BOTH the line and the term to
        // std::wstring -- four allocations per line per paint, at ~10 paints a
        // second while smooth-scrolling.
        //
        // The fast path works on the runs' raw UTF-8 bytes in a reused member
        // buffer.  Byte offsets equal character offsets only for ASCII, so it is
        // taken only when the whole line is ASCII and UTF-8 -- the same
        // equivalence test LineContains uses -- and anything else falls back to
        // the original decode path unchanged.
        // The scan itself is HighlightHitsForLine (below) so the selftest can
        // drive it directly -- its output is only consumed inside this paint
        // handler otherwise, which a headless test cannot reach.
        std::vector<HighlightHit> hitv;
        HighlightHitsForLine(lineIdx, hitv);
        struct HitRange { int start, end; };
        std::vector<HitRange> hits;
        hits.reserve(hitv.size());
        for (const HighlightHit& hh : hitv) hits.push_back({hh.start, hh.end});

        if (!hits.empty()) {
            // Left-margin bar: 3-pixel yellow stripe on each visual row of this
            // line, against the true window edge (-m_pixelOffset in the shifted
            // coordinates) so it stays a margin marker instead of sliding in
            // over the first character.
            dc.SetBrush(wxBrush(wxColour(255, 220, 0)));
            dc.SetPen(*wxTRANSPARENT_PEN);
            int wc = WrapCount(lineIdx);
            for (int vr = 0; vr < wc; ++vr)
                dc.DrawRectangle(-m_pixelOffset, yOff + vr * m_lineH, 3, m_lineH);

            // In-line highlight: re-render each matching span with yellow bg + black text.
            // Pre-fill background rectangles (consistent with DrawRunChunk strategy).
            dc.SetTextForeground(wxColour(0, 0, 0));
            dc.SetBackgroundMode(wxTRANSPARENT);

            int cx = 0;  // absolute char index in the logical line
            for (const auto& run : line) {
                if (run.text.empty() || run.horiz_rule) continue;
                wxString wxText = DecodeRunText(run);
                int runLen = (int)wxText.size();

                // Underline only when the run actually carries it.  It used to be
                // forced on for any action-bearing run, which overrode the world's
                // "Underline hyperlinks" setting (see DrawRunChunk below).
                bool drawUnderline = run.underline;
                if      (run.bold && run.italic && drawUnderline) dc.SetFont(m_fontBoldItalicUnderline);
                else if (run.bold && run.italic)                  dc.SetFont(m_fontBoldItalic);
                else if (run.italic && drawUnderline)             dc.SetFont(m_fontItalicUnderline);
                else if (run.italic)                              dc.SetFont(m_fontItalic);
                else if (run.bold && drawUnderline)               dc.SetFont(m_fontBoldUnderline);
                else if (run.bold)                                dc.SetFont(m_fontBold);
                else if (drawUnderline)                           dc.SetFont(m_fontUnderline);
                else                                              dc.SetFont(m_font);

                for (const auto& hit : hits) {
                    int lo = std::max(cx, hit.start);
                    int hi = std::min(cx + runLen, hit.end);
                    if (lo >= hi) continue;

                    // Render matching chars at their wrapped visual position,
                    // batching contiguous characters on the same visual row.
                    int c = lo;
                    while (c < hi) {
                        int vr       = c / wrapCols;   // visual row index within this line
                        int vcol     = c % wrapCols;   // column within that visual row
                        int py       = yOff + vr * m_lineH;
                        // The preamble (timestamp) occupies whole cells at the
                        // start of the FIRST visual row and the text was drawn
                        // after it, so the highlight has to shift by the same
                        // amount -- the correction XToChar already makes.
                        // Without it the yellow box and its re-drawn text land
                        // one preamble-width to the left of the characters they
                        // are meant to be covering.
                        int px       = vcol * m_charW
                                     + (vr == 0 ? PreambleCells(lineIdx) * m_charW : 0);
                        // Batch to end-of-match or end-of-visual-row, whichever comes first.
                        int batchEnd = std::min(hi, (vr + 1) * wrapCols);
                        int nChars   = batchEnd - c;
                        // Pre-fill the highlight background rectangle.
                        dc.SetBrush(wxBrush(wxColour(255, 220, 0)));
                        dc.SetPen(*wxTRANSPARENT_PEN);
                        dc.DrawRectangle(px, py, nChars * m_charW, m_lineH);
                        // Per CELL, not one batched DrawText: a monospace font's
                        // natural advance can differ from the integer cell width
                        // m_charW on macOS, so flowing the whole substring drifts
                        // off the grid the rest of the pane draws on -- and the
                        // highlighted text visibly stepped out of its own
                        // background rectangle.  Same loop DrawRunChunk uses, and
                        // for the same reason (see its comment).
                        // c - cx = offset of c within wxText.
                        for (int k = 0; k < nChars; ++k)
                            dc.DrawText(wxText.substr(c - cx + k, 1),
                                        px + k * m_charW, py);
                        c = batchEnd;
                    }
                }

                cx += runLen;
            }

            // Restore DC state so subsequent DrawLine calls start clean.
            dc.SetFont(m_font);
        }
    }

    // ── Bookmark marker ───────────────────────────────────────────────────────
    // A 4-pixel blue bar in the left margin on each visual row of a bookmarked
    // line (drawn last so it sits on top of the text background).
    if (line.bookmark) {
        dc.SetBrush(wxBrush(wxColour(60, 140, 255)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        int wc = WrapCount(lineIdx);
        for (int vr = 0; vr < wc; ++vr)
            dc.DrawRectangle(-m_pixelOffset, yOff + vr * m_lineH, 4, m_lineH);
    }
}

// ── DrawRunChunk ──────────────────────────────────────────────────────────────
// Renders one sub-chunk of a run at pixel position (x, yOff).
//
// Rendering strategy: pre-fill the background rectangle first, then draw text
// with wxTRANSPARENT background mode.  This is superior to wxSOLID mode for
// two reasons:
//   1. The fill width is exactly text.size() * m_charW — our integer grid —
//      which prevents sub-pixel gaps between chunks on HiDPI / Retina displays
//      where the font's ink bounding box may differ from the character cell by
//      a fraction of a pixel.
//   2. Transparent text drawing lets Core Text / Pango anti-alias the glyphs
//      against the already-correct background rather than against a blank DC.

void COutputPane::DrawRunChunk(wxDC& dc, const wxString& text, int x, int yOff,
                                const SOutputRun& run, bool selected)
{
    if (text.IsEmpty()) return;

    // Faded and/or colour-mapped, through the doc's own TranslateColour -- the
    // same function Windows paints through, so world.MapColour applies here as
    // well.  With no fade and no colour map it hands the colour straight back.
    uint32_t rawFg = run.fg, rawBg = run.bg;
    if (m_fn_translate_colour) {
        rawFg = m_fn_translate_colour(rawFg, m_paintOpacity);
        rawBg = m_fn_translate_colour(rawBg, m_paintOpacity);
    }
    wxColour fg(GetRValue(rawFg), GetGValue(rawFg), GetBValue(rawFg));
    wxColour bg(GetRValue(rawBg), GetGValue(rawBg), GetBValue(rawBg));

    // ── Background fill ───────────────────────────────────────────────────────
    // For selected text, use the OS system selection colours (wxSYS_COLOUR_HIGHLIGHT
    // / wxSYS_COLOUR_HIGHLIGHTTEXT) rather than inverting the run's own fg/bg.
    // Inverting can produce invisible results when fg == bg (e.g. white-on-white
    // runs in some MUD colour schemes); the system colours are always legible.
    wxColour bgFill;
    wxColour textFg;
    if (selected) {
        bgFill  = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT);
        textFg  = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT);
    } else {
        bgFill  = bg;
        textFg  = fg;
    }

    dc.SetBrush(wxBrush(bgFill));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(x, yOff, (int)text.size() * m_charW, m_lineH);

    dc.SetTextForeground(textFg);
    dc.SetBackgroundMode(wxTRANSPARENT);

    // Select the pre-built font variant to avoid per-run wxFont allocation.
    // 8 variants: bold × italic × underline.
    //
    // Link runs are NOT force-underlined here any more.  The engine already
    // decides: mxp/mxpOpenAtomic.cpp:180-181,773-774 set UNDERLINE only when the
    // world's "Underline hyperlinks" option is on, and doc_display_shared.cpp
    // carries that through as run.underline.  ORing isLink in overrode the
    // setting so it could never be turned off.  (MXP_ACTION_HYPERLINK ORs
    // UNDERLINE unconditionally at mxpOpenAtomic.cpp:213 — stock Windows
    // behaviour, left alone; the option governs send-links and image links.)
    bool drawUnderline = run.underline;
    if      (run.bold && run.italic && drawUnderline) dc.SetFont(m_fontBoldItalicUnderline);
    else if (run.bold && run.italic)                  dc.SetFont(m_fontBoldItalic);
    else if (run.italic && drawUnderline)             dc.SetFont(m_fontItalicUnderline);
    else if (run.italic)                              dc.SetFont(m_fontItalic);
    else if (run.bold && drawUnderline)               dc.SetFont(m_fontBoldUnderline);
    else if (run.bold)                                dc.SetFont(m_fontBold);
    else if (drawUnderline)                           dc.SetFont(m_fontUnderline);
    else                                              dc.SetFont(m_font);

    // Draw each character at its fixed grid cell (x + i*m_charW) rather than
    // letting one DrawText flow the whole string at the font's natural advances.
    // A monospace font's natural advance can differ from the integer cell width
    // m_charW on macOS, so a single DrawText drifts off the grid — which both
    // misaligns the grid-computed selection rectangle with the glyphs and shifts
    // the text when a run is split for a partial selection.  Per-cell drawing
    // keeps glyph positions identical whether or not the run is split, and lined
    // up with the selection rectangle and click hit-testing (both grid-based).
    for (size_t i = 0; i < text.size(); ++i)
        dc.DrawText(text.Mid(i, 1), x + (int) i * m_charW, yOff);

    // Strikethrough: draw a horizontal line through the vertical centre of the glyph.
    if (run.strikethrough) {
        int midY = yOff + m_lineH / 2;
        int textW = (int)text.size() * m_charW;
        dc.SetPen(wxPen(textFg, 1));
        dc.DrawLine(x, midY, x + textW, midY);
    }
}

// ── Wrap cache helpers ────────────────────────────────────────────────────────

int COutputPane::WrapCount(int lineIdx) const
{
    if (lineIdx < 0 || lineIdx >= LineCountI()) return 0;
    // Pure arithmetic, and it has to stay that way: RebuildWrapCache calls this
    // once per BUFFERED line on every resize, font change and text-rectangle
    // change.  It used to fetch the whole line first, only to test a horiz_rule
    // guard the m_lineChars arithmetic already answers (a rule line's only run
    // carries no text, so `chars == 0` returns 1 anyway) -- which under the pull
    // model made every rebuild one full CLine -> SOutputLine conversion per line.
    // m_lineChars is maintained alongside m_lines, so this costs one division
    // rather than a re-scan of every run's bytes (which for a non-UTF-8 world
    // means decoding each run into a fresh wxString just to take its length).
    const int chars = (lineIdx < (int)m_lineChars.size()) ? m_lineChars[lineIdx] : 0;
    if (chars == 0 || m_wrapCols <= 0) return 1;
    return (chars + m_wrapCols - 1) / m_wrapCols;
}

// Resolve the active text rectangle (world.TextRectangle) into client pixels.
// Returns false (and the full client area) when no rectangle is set.
bool COutputPane::TextRect(int& l, int& t, int& r, int& b) const
{
    int clientW = 0, clientH = 0;
    const_cast<COutputPane*>(this)->GetClientSize(&clientW, &clientH);
    l = 0; t = 0; r = clientW; b = clientH;
    if (!m_fn_text_rect) return false;
    int rl = 0, rt = 0, rr = 0, rb = 0;
    if (!m_fn_text_rect(rl, rt, rr, rb)) return false;
    // Resolve the documented offset-from-edge form BEFORE clamping: a right or
    // bottom <= 0 means "that many pixels in from the far edge", and treating it
    // as "not given" (the old `(rr > 0) ? … : clientW`) handed the text the full
    // client width, so it wrapped late and drew underneath the very miniwindow
    // the plugin had reserved space for.  pal::resolveTextRect is Windows'
    // CMUSHView::GetTextRectangle arithmetic, shared with EffectiveTextCols so
    // the visual wrap and the engine wrap cannot disagree.
    pal::resolveTextRect(rl, rt, rr, rb, clientW, clientH);
    // Clamp to the client area; ignore degenerate rectangles.
    l = std::max(0, rl);
    t = std::max(0, rt);
    r = std::min(clientW, rr);
    b = std::min(clientH, rb);
    if (r <= l || b <= t) { l = 0; t = 0; r = clientW; b = clientH; return false; }
    return true;
}

int COutputPane::TextViewportH() const
{
    int l, t, r, b;
    if (TextRect(l, t, r, b)) return b - t;
    int h = 0;
    const_cast<COutputPane*>(this)->GetClientSize(nullptr, &h);
    return h;
}

void COutputPane::RebuildWrapCache()
{
    int clientW;
    GetClientSize(&clientW, nullptr);
    // Narrow the wrap width to the text rectangle when a plugin has reserved
    // part of the output area (e.g. a docked miniwindow on the left/right).
    int trL, trT, trR, trB;
    // ... and by the pixel offset, which insets the text from the left edge.
    // Windows does exactly this in its own wrap computation:
    //   mushview.cpp:3220  (GetOutputWindowWidth () - pDoc->m_iPixelOffset) / charW
    // where GetOutputWindowWidth() is already the text rectangle's width.
    // CWorldChildFrame::EffectiveTextCols computes the ENGINE's wrap column, and
    // the two must agree -- if this one came out narrower, every full-width
    // engine line would gain a one- or two-character orphan continuation row.
    // They now make the SAME call (pal::textWrapColumns) over the SAME resolved
    // rectangle (pal::resolveTextRect), so they cannot drift.
    int wrapW = TextRect(trL, trT, trR, trB) ? (trR - trL) : clientW;
    m_wrapCols = pal::textWrapColumns(wrapW, m_pixelOffset, m_charW);

    // Renumber from zero: a full rebuild is the one place the absolute row axis
    // may be reset, since every consumer reads through RowStartOf().
    const int n = LineCountI();

    // Self-heal the char-count cache if it ever falls out of step with m_lines.
    // Every mutation site keeps them paired, so this should never fire — but a
    // silently wrong wrap count is a scrambled display, and a rebuild is the
    // natural place to re-establish the invariant rather than assert on it.
    if ((int)m_lineChars.size() != n) {
        m_lineChars.assign((size_t)n, 0);
        for (int i = 0; i < n; ++i)
            m_lineChars[(size_t)i] = LineCharLen(LineAt(i));
    }

    m_visualRowStart.assign((size_t)n, 0);
    m_rowBase         = 0;
    m_totalVisualRows = 0;
    for (int i = 0; i < n; ++i) {
        m_visualRowStart[(size_t)i] = m_totalVisualRows;
        m_totalVisualRows          += WrapCount(i);
    }
}

// Drop logical line 0 from both caches in O(1), leaving the remaining absolute
// row numbers untouched and moving the base instead.
void COutputPane::PopFrontWrapCache()
{
    if (m_visualRowStart.empty()) return;
    m_visualRowStart.pop_front();
    if (!m_lineChars.empty()) m_lineChars.pop_front();
    // The new first line's absolute row becomes the base, so RowStartOf(0) == 0
    // and every other entry keeps its meaning.
    const long long newBase = m_visualRowStart.empty()
                              ? m_rowBase + m_totalVisualRows
                              : m_visualRowStart.front();
    m_totalVisualRows -= (newBase - m_rowBase);   // both 64-bit now
    if (m_totalVisualRows < 0) m_totalVisualRows = 0;
    m_rowBase = newBase;
}

int COutputPane::VisualRowToLogical(int vr) const
{
    if (m_visualRowStart.empty()) return 0;
    vr = std::max(0, vr);
    // Binary search: largest i such that m_visualRowStart[i] <= vr.
    int lo = 0, hi = (int)m_visualRowStart.size() - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (m_visualRowStart[mid] - m_rowBase <= vr) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

// ── Scroll helpers ────────────────────────────────────────────────────────────

void COutputPane::UpdateScrollbars(bool scrollToBottom)
{
    int clientW, clientH;
    GetClientSize(&clientW, &clientH);

    // Add one row for the in-progress (pending) line when it has content.
    int pendingRows = (!m_pendingLine.empty()) ? 1 : 0;
    // 64-bit: this is THE product that overflows.  See pal::rowsToPixels.
    // m_overscrollRows is the blank region Clear Screen (⌃⌘L) opened below the
    // last line so that line could sit at the top of the viewport.  It is part
    // of the scrollable extent and nothing else -- no line maps into it, so it
    // paints as background and every pixel-to-line mapping ignores it.
    const long long totalH64 =
        pal::rowsToPixels(m_totalVisualRows + pendingRows + m_overscrollRows,
                          m_lineH);

    // Virtual width = client width: horizontal scrolling is disabled; long
    // lines are visually wrapped rather than requiring H-scroll.
    // When a plugin reserved part of the area (world.TextRectangle), the text's
    // visible band is shorter than the client; pad the virtual height by the
    // reserved amount so "scroll to bottom" lands the newest line at the
    // rectangle's bottom (not the window's, where the clip would hide it).
    //
    // The pixel offset pads it further, which is what leaves a gap under the
    // last line to match the one to the left of the text -- Windows calls this
    // "text offset from bottom" (mushview.cpp:1747) and gets it by drawing the
    // text -m_iPixelOffset up the document.  Padding the virtual height instead
    // produces the same visible gap without Windows' side effect of clipping the
    // top of the first line when you scroll all the way up, and it leaves every
    // pixel-to-line mapping (YToLine, XToChar) untouched.
    // Not pixel-exact: SetScrollRate quantises scrolling to whole rows, so the
    // gap you actually see is the offset rounded UP to a multiple of m_lineH
    // (20 px with a 15 px row = 30 px).  Windows scrolls by pixels and gets
    // exactly 20.  Making it exact would mean scrolling this pane by pixels
    // rather than rows, which is a much larger change than this setting is
    // worth; the gap is a margin, and erring wide is the harmless direction.
    SetVirtualSize(clientW,
                   pal::clampToDeviceExtent(totalH64 + (clientH - TextViewportH())
                                                     + m_pixelOffset));

    if (scrollToBottom && LineCountI() != 0) {
        int unitH;
        GetScrollPixelsPerUnit(nullptr, &unitH);
        if (unitH > 0) {
            // Scroll() clamps to (scrollRange - pageSize), which is the position
            // that shows the last line at the bottom.  Passing scrollRange directly
            // is equivalent to Scroll(-1, -1) but without the ambiguity.
            //
            // "Very smooth scrolling" walks there instead of jumping.  Windows
            // (mushview.cpp:4632-4653) animates only when the move is LESS THAN
            // one screenful and steps one pixel at a time, repainting each step;
            // a bigger jump is redrawn in one go.  Same rule here, in rows.
            // m_animatingScroll keeps output arriving mid-animation -- Update()
            // pumps paints, and a paint handler can append -- from starting a
            // second one on top of this.
            const int target = GetScrollRange(wxVERTICAL);
            if (m_smoothScroll && !m_animatingScroll) {
                int curY = 0;
                GetViewStart(nullptr, &curY);
                // Scroll() CLAMPS to (range - page), so the range is not where
                // we actually land -- the real move is a page shorter.  Measure
                // the destination, not the range, or the delta is overstated by
                // a whole screenful and never qualifies as "less than one".
                int pageRows = GetScrollThumb(wxVERTICAL);
                if (pageRows <= 0) {
                    GetClientSize(nullptr, &pageRows);
                    pageRows = (m_lineH > 0) ? pageRows / m_lineH : 0;
                }
                const int dest  = (target > pageRows) ? target - pageRows : 0;
                const int delta = dest - curY;
                if (delta > 0 && pageRows > 0 && delta < pageRows) {
                    m_animatingScroll = true;
                    for (int row = 1; row <= delta; ++row) {
                        Scroll(0, curY + row);
                        Update();          // paint this step before the next
                    }
                    m_animatingScroll = false;
                }
            }
            Scroll(0, target);   // land exactly on target either way
        }
        // Both counters, not just one.  While auto-scrolling there is nothing to
        // compensate for -- the view is pinned to the bottom -- but the rows
        // removed by each trim kept accumulating here.  The next time the user
        // scrolled up and a trim landed, the arm below subtracted every row lost
        // since the world opened and the view jumped to the top of the
        // scrollback.
        m_trimmedCount       = 0;
        m_trimmedRowsRemoved = 0;
    } else if (m_trimmedCount > 0 && !m_autoScroll) {
        // The buffer trimmed lines from the front while the user was scrolled up.
        // Compensate: shift the view up by the number of visual rows removed so
        // the visible content stays stable (no jump when old lines scroll off).
        int scrollY, unitH;
        GetViewStart(nullptr, &scrollY);
        GetScrollPixelsPerUnit(nullptr, &unitH);
        if (unitH > 0) {
            int shift = (m_trimmedRowsRemoved * m_lineH + unitH - 1) / unitH;
            Scroll(0, std::max(0, scrollY - shift));
        }
        m_trimmedCount      = 0;
        m_trimmedRowsRemoved = 0;
    }
}

void COutputPane::OnScrollWinUpdate(wxScrollWinEvent& evt)
{
    evt.Skip();
    CallAfter([this]() { RefreshAutoScrollFromPosition(); });
}

void COutputPane::OnSize(wxSizeEvent& evt)
{
    // Windows re-runs OnTestEnd when the view was at the buffer end going
    // into the resize (CMUSHView::sizewindow, mushview.cpp:3196-3197): the
    // jump keeps the tail visible through the reflow -- even for a FROZEN
    // view sitting at the end, which UpdateScrollbars(false) below would
    // otherwise let drift -- and OnTestEnd's clear takes the selection with
    // it.  Sampled BEFORE the reflow, as Windows' m_bAtBufferEnd member is.
    const bool wasAtEnd = IsAtBufferEnd();
    RebuildWrapCache();
    UpdateScrollbars(m_autoScroll);
    if (wasAtEnd) {
        ScrollToBufferEnd();
        SetSelectionRange(0, 0, 0, 0);
    }
    // Notify so the world frame refreshes the doc's GUI geometry (GetInfo
    // 280/281) — the pane resizes on layout changes that don't resize the frame
    // (e.g. the input bar taking its space at startup), which the frame's own
    // OnSize would miss.
    if (m_fn_resized) m_fn_resized();
    evt.Skip();
}

// ── Selection helpers ─────────────────────────────────────────────────────────

int COutputPane::YToLine(int clientY, bool* outside) const
{
    // Windows' bOutside starts TRUE and is cleared only when the raw row lands
    // inside the buffer (CMUSHView::calculate_line_and_column,
    // mushview.cpp:1896-1904), so an empty pane counts as outside.
    if (outside) *outside = true;
    if (LineCountI() == 0) return 0;

    // Honor the text rectangle's vertical offset (text starts at its top).
    { int l, t, r, b; if (TextRect(l, t, r, b)) { clientY -= t; } }

    int scrollY;
    const_cast<COutputPane*>(this)->GetViewStart(nullptr, &scrollY);
    int unitH;
    const_cast<COutputPane*>(this)->GetScrollPixelsPerUnit(nullptr, &unitH);
    const long long yPix   = (long long) scrollY * unitH + clientY;
    const long long rawRow = (m_lineH > 0) ? (yPix / m_lineH) : 0;
    const int visRow = pal::clampToDeviceExtent(rawRow);
    // The row the pixel really named, BEFORE the clamp above -- Windows keeps
    // the same two facts apart: it clamps `line` and still reports that the
    // clamp happened.  Every caller but the tooltip discards the flag, which
    // is parity: a click below the buffer selects the last line there too.
    // Asked of yPix rather than of rawRow or visRow, because C++ truncates
    // toward zero (so -5/15 is 0, not -1) and clampToDeviceExtent floors at 0
    // -- both would swallow "above the first line" entirely.
    if (outside)
        *outside = (yPix < 0) || (rawRow >= m_totalVisualRows);
    // Map visual row → logical line via the wrap cache.
    return std::max(0, std::min(LineCountI() - 1,
                                const_cast<COutputPane*>(this)->VisualRowToLogical(visRow)));
}

// See the header.  Y only, exactly like Windows' bOutside: the column is
// clamped on both platforms and never decides whether a point is on a line.
bool COutputPane::YIsOutsideLines(int clientY) const
{
    bool outside = true;
    YToLine(clientY, &outside);
    return outside;
}

// The preamble's width in character cells for a line — 0 when the option is
// off, the frame is not wired, or the line's kind has an empty preamble.
// Recomputed on demand (a strftime per call): the text depends on the line's
// own timestamp, and paint and hit-test must agree on ONE definition.
int COutputPane::PreambleCells(int lineIdx) const
{
    if (!m_fn_line_preamble || lineIdx < 0 || lineIdx >= LineCountI())
        return 0;
    SLinePreamble pre;
    const time_t prevArr =
        (lineIdx > 0) ? LineAt(lineIdx - 1).arrived : (time_t) 0;
    if (!m_fn_line_preamble(LineAt(lineIdx), prevArr, lineIdx == 0, pre))
        return 0;
    return (int) pre.text.length();
}

int COutputPane::XToChar(int lineIdx, int clientX, int clientY,
                         bool halfway) const
{
    if (lineIdx < 0 || lineIdx >= LineCountI() || m_charW <= 0)
        return 0;

    // Honor the text rectangle: text is drawn from its top-left, so hit-testing
    // must subtract that offset before mapping pixels to characters.
    { int l, t, r, b; if (TextRect(l, t, r, b)) { clientX -= l; clientY -= t; } }
    // The pixel offset shifts the text right whether or not a rectangle is set,
    // so it comes off unconditionally -- inside the block above it would leave
    // click-to-char wrong by m_pixelOffset in the common no-rectangle case.
    clientX -= m_pixelOffset;

    // Count total characters on the line (Unicode code points, not UTF-8 bytes).
    int totalChars = LineCharLen(LineAt(lineIdx));

    // Convert client x to virtual (unscrolled) x.
    // Horizontal scroll is always 0 (H-scrolling is disabled), but keep the
    // arithmetic correct in case that ever changes.
    int scrollX, scrollY;
    const_cast<COutputPane*>(this)->GetViewStart(&scrollX, &scrollY);
    int unitW, unitH;
    const_cast<COutputPane*>(this)->GetScrollPixelsPerUnit(&unitW, &unitH);
    int virtualX = clientX + scrollX * unitW;

    // For wrapped lines we need to know which visual row within the logical
    // line was clicked, so we can add (vrOffset * m_wrapCols) to the column.
    int vrOffset = 0;
    if (m_wrapCols > 0 && m_lineH > 0) {
        int yPix       = scrollY * unitH + clientY;
        int clickVR    = yPix / m_lineH;
        int lineStartVR = RowStartOf(lineIdx);
        vrOffset = std::max(0, clickVR - lineStartVR);
    }

    // Column within the visual row, then offset by wrapped rows. Clamp the
    // per-row column to the wrap width so a click past the text on a non-final
    // visual row can't spill into a later row's character range.
    int colInRow = std::max(0, (virtualX + (halfway ? m_charW / 2 : 0)) / m_charW);
    // The line preamble occupies whole cells at the START of the first visual
    // row; clicks land on the text after it (Windows' m_iPreambleOffset).
    if (vrOffset == 0)
        colInRow = std::max(0, colInRow - PreambleCells(lineIdx));
    // The caret form (halfway) may sit just past the last cell of the row --
    // that is where "click at the end of the row" puts it.  The CONTAINMENT
    // form must not: m_wrapCols is the FIRST column of the next visual row, so
    // a click to the right of the text on a wrapped line resolved into that
    // row and could fire ITS hyperlink (2026-08-18).
    if (m_wrapCols > 0)
        colInRow = std::min(colInRow, halfway ? m_wrapCols : m_wrapCols - 1);
    int col      = vrOffset * m_wrapCols + colInRow;
    return std::max(0, std::min(totalChars, col));
}

SelPoint COutputPane::MouseToSelPoint(const wxMouseEvent& evt) const
{
    return PointToChar(evt.GetX(), evt.GetY());
}

SelPoint COutputPane::PointToChar(int clientX, int clientY, bool halfway) const
{
    int line = YToLine(clientY);
    int col  = XToChar(line, clientX, clientY, halfway);
    return SelPoint{line, col};
}

bool COutputPane::GetNormalizedSel(SelPoint& start, SelPoint& end) const
{
    if (!m_selAnchor.valid() || !m_selFocus.valid()) return false;
    if (m_selAnchor == m_selFocus) return false;

    if (m_selAnchor < m_selFocus) {
        start = m_selAnchor;
        end   = m_selFocus;
    } else {
        start = m_selFocus;
        end   = m_selAnchor;
    }
    return true;
}

// ── FreezeIfSelection ─────────────────────────────────────────────────────────
// "Freeze output so they can copy or print it" — the two lines every
// selection-ending path on Windows carries (mushview.cpp:2453 OnLButtonUp,
// :2613 OnMouseMove, :4837 OnLButtonDblClk, pal/win_worldview.cpp:639
// SetSelection):
//
//     if (!get_selection (newrgn) && pDoc->m_bAutoFreeze)
//        m_freeze = true;
//
// get_selection returns TRUE for an EMPTY region (mushview.cpp:2765, :2826),
// so the condition reads "there IS a selection, and the world wants
// auto-freeze".  Without it, drag-selecting a line on a busy MUD is a race: the
// text scrolls out from under the cursor before the user reaches Cmd-C, and
// once the scrollback trim kicks in the selection indices slide onto different
// text, so the wrong line is copied.
// ── ClearSelectionOnNewOutput ────────────────────────────────────────────────
// See the header.  m_autoScroll is the port's m_freeze twin, so the guard is
// the same `!m_freeze` Windows uses.
//
// In a default world this is a no-op, and deliberately so: making a selection
// FREEZES the view (auto_pause), so m_autoScroll is already false by the time
// the next line lands.  What it changes is the auto_pause-OFF world and the
// script that calls world.SetSelection while output flows -- there the port
// kept the selection indefinitely, sliding it down with every head trim, so a
// later Copy, or world.GetSelectionStartLine, read text chosen minutes ago
// where Windows had long since dropped it.
void COutputPane::ClearSelectionOnNewOutput()
{
    if (!m_autoScroll) return;             // frozen: Windows keeps the selection
    if (!m_selAnchor.valid() && !m_selFocus.valid()) return;   // nothing to drop
    m_selAnchor = SelPoint{};
    m_selFocus  = SelPoint{};
    // Windows' new-output clear is addedstuff() -> `if (!m_freeze)
    // OnTestEnd()` (mushview.cpp:1815) -> SelectionChanged (:1655), under
    // this same not-frozen gate -- so the notify belongs here too.  It runs
    // mid-drain there as well; a plugin that emits output from the callback
    // has the identical re-entrancy exposure on both platforms.
    NotifySelectionMaybeChanged();
}

// ── NotifySelectionMaybeChanged ──────────────────────────────────────────────
// CMUSHView::NotifySelectionChanged (mushview.cpp:6423-6450), pane-shaped: a
// recursion guard first (a plugin's OnPluginSelectionChanged can move the
// selection, e.g. world.SetSelection), then the not-really-changed compare,
// then the broadcast through the frame's callback -- the pane cannot see the
// doc, the same reason m_fn_autoscroll_changed exists.
void COutputPane::NotifySelectionMaybeChanged()
{
    if (m_inSelectionNotify) return;
    // Compare the ORDERED pair, as Windows' m_selstart/m_selend always are:
    // re-selecting the same span from the other direction is not a change.
    SelPoint lo = m_selAnchor, hi = m_selFocus;
    if (hi < lo) std::swap(lo, hi);
    if (lo == m_notifiedSelAnchor && hi == m_notifiedSelFocus)
        return;                            // not really changed
    m_notifiedSelAnchor = lo;
    m_notifiedSelFocus  = hi;
    if (!m_fn_selection_changed) return;
    // The guard is a member where Windows' is a function-local static: if a
    // plugin callback ever destroys this pane synchronously, the reset below
    // writes freed memory.  wx defers window destruction to idle, so this is
    // theoretical today -- noted rather than defended with a static, which
    // would wrongly couple two panes' guards.
    m_inSelectionNotify = true;
    m_fn_selection_changed();
    m_inSelectionNotify = false;
}

void COutputPane::FreezeIfSelection()
{
    if (!m_allowAutoFreeze) return;
    SelPoint s, e;
    if (!GetNormalizedSel(s, e)) return;   // empty region: nothing to freeze for
    if (!m_autoScroll) return;             // already frozen
    SetPaused(true);
}

// ── AutoCopySelection ─────────────────────────────────────────────────────────
// copy_selection_to_clipboard (+ …_in_html for the format).  Windows runs the
// same four lines at the end of BOTH extend_selection (mushview.cpp:1982, the
// drag) and OnLButtonDblClk (mushview.cpp:4884, the word):
//
//     if (pDoc->m_bCopySelectionToClipboard)
//       if (pDoc->m_bAutoCopyInHTML) OnEditCopyashtml (); else OnEditCopy ();
//
// The port only had it on the drag path, so a double-clicked word highlighted
// but left the PREVIOUS drag on the clipboard — paste produced stale text.
// On GTK the X11 primary selection is written regardless of the option, because
// middle-click paste is a universal Unix convention users rely on.
void COutputPane::AutoCopySelection()
{
    SelPoint s, e;
    if (!GetNormalizedSel(s, e)) return;

    // Materialise the selection ONLY if something is going to consume it.  On
    // macOS with copy_selection_to_clipboard off -- the default -- nothing
    // does, yet every mouse release with a selection used to build the whole
    // thing: for a Select All over a large scrollback that is the entire buffer
    // decoded into a wxString, freed immediately, on every click.
#ifdef __WXGTK__
    const bool wanted = true;          // the X11 primary selection always wants it
#else
    const bool wanted = m_copySelToClip;
#endif
    if (!wanted) return;

    const wxString text = SelectedText();
    if (text.IsEmpty()) return;

    if (m_copySelToClip) {
        if (m_autoCopyInHTML) {
            CopySelectionAsHTML(GetFont().GetFaceName());
        } else if (wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxTextDataObject(text));
            wxTheClipboard->Close();
        }
    }
#ifdef __WXGTK__
    if (wxTheClipboard->Open()) {
        wxTheClipboard->UsePrimarySelection(true);
        wxTheClipboard->SetData(new wxTextDataObject(text));
        wxTheClipboard->UsePrimarySelection(false);
        wxTheClipboard->Close();
    }
#endif
}

// ── Mouse ─────────────────────────────────────────────────────────────────────

// Ctrl+click => the alias menu (CMUSHView::OnLButtonDown's arm).  Returns
// true when consumed; the hit is containment (which cell holds the pixel).
bool COutputPane::TryAliasMenu(wxMouseEvent& evt)
{
    if (!m_fn_alias_menu || !evt.RawControlDown()) return false;
    const int lineIdx = YToLine(evt.GetY());
    if (lineIdx < 0 || lineIdx >= LineCountI()) return false;
    const int col = XToChar(lineIdx, evt.GetX(), evt.GetY(),
                            false /*containment*/);
    m_fn_alias_menu(lineIdx, col, evt.GetPosition());
    return true;
}

void COutputPane::OnLeftDown(wxMouseEvent& evt)
{
    // Any real click claims Select All for this pane, even one that (like the
    // alias menu or a plain click below) does not go on to make a selection.
    m_wantsSelectAll = true;

    if (TryAliasMenu(evt)) return;   // Ctrl+click: the alias menu, no selection

    // ── Hint bar: click snaps to the live bottom ───────────────────────────
    // The hint bar is painted on top of everything (miniwindows included), so
    // its click region takes priority.  We check it before the miniwindow hit
    // test so the visual layering is respected.
    if (!m_autoScroll && LineCountI() != 0) {
        int clientH;
        GetClientSize(nullptr, &clientH);
        if (evt.GetY() >= clientH - kHintBarH) {
            ScrollToBottom();
            if (m_fn_return_focus) m_fn_return_focus();
            return;  // consume — don't start a selection
        }
    }

    // ── Triple-click: select the entire line ──────────────────────────────────
    // wxWidgets has no EVT_LEFT_TCLICK.  Detect triple-click as a left-down
    // that arrives within 500 ms of the last double-click on the same line.
    if (LineCountI() != 0 && m_lastDClickLine >= 0) {
        wxLongLong elapsed = wxGetLocalTimeMillis() - m_lastDClickTimeMs;
        int clickLine = YToLine(evt.GetY());
        if (elapsed < 500 && clickLine == m_lastDClickLine) {
            // Build the full line length and select it end-to-end.
            int lineLen = LineCharLen(LineAt(clickLine));
            m_selAnchor      = SelPoint{clickLine, 0};
            m_selFocus       = SelPoint{clickLine, lineLen};
            m_lastDClickLine = -1;   // consume so a 4th click starts fresh
            Refresh(false);
            FreezeIfSelection();     // a selection was just made
            NotifySelectionMaybeChanged();
            evt.Skip();
            return;
        }
        // Stale or wrong line — reset.
        if (elapsed >= 500) m_lastDClickLine = -1;
    }

    // Give miniwindows first chance.
    if (m_fn_miniwindow_mouse_down &&
        m_fn_miniwindow_mouse_down(evt.GetX(), evt.GetY(), 0x10 /*MW_MOUSE_LH*/))
    {
        evt.Skip();
        return;
    }

    m_linkAtDown.clear();

    // Shift+click: extend the existing selection (keep anchor, move focus).
    if (evt.ShiftDown() && m_selAnchor.valid()) {
        m_selFocus = MouseToSelPoint(evt);
        Refresh(false);
        FreezeIfSelection();     // extend_selection's freeze (mushview.cpp:2453)
        NotifySelectionMaybeChanged();       // extend_selection, :1980
        evt.Skip();
        return;
    }

    // Check whether the click landed on a link run.  We store the action now
    // but don't fire it yet — we fire on mouse-up only if no drag occurred so
    // that the user can still select/copy link text by dragging over it.
    // XToChar returns the absolute character column within the logical line,
    // correctly accounting for visual-row wrapping.
    if (m_lineH > 0 && LineCountI() != 0) {
        int lineIdx = YToLine(evt.GetY());
        if (lineIdx >= 0 && lineIdx < LineCountI()) {
            int clickCol = XToChar(lineIdx, evt.GetX(), evt.GetY(),
                                   false /*containment: which cell holds it*/);
            const SOutputRun* run = RunAtCharColumn(LineAt(lineIdx), clickCol);
            if (IsUsableLink(run)) {
                m_linkAtDown     = run->action();
                m_linkTypeAtDown = run->action_type();
                m_linkNoteAtDown = LineAt(lineIdx).comment;
                m_linkEncAtDown  = run->encoding();
            }
        }
    }

    // A click INSIDE the existing selection leaves it alone and takes no
    // capture: `if (oldrgn.PtInRegion (point)) { ...; return; }`
    // (CMUSHView::OnLButtonDown, mushview.cpp:2248-2253).  The port reset the
    // anchor to the click point unconditionally, so clicking anywhere in a
    // highlighted paragraph -- to bring the window forward, or on the way to
    // Edit > Copy -- collapsed the selection and left Copy with nothing.
    //
    // Placed here for two reasons, both taken from the order upstream uses.
    // AFTER the link stash and skipped when a link was hit, because Windows
    // tests the hyperlink FIRST and fires it on the way down (:2158-2225),
    // returning before it ever builds the region.  And after the shift-click
    // branch above, because that one extends the selection wherever the click
    // lands -- Windows' `nFlags & MK_SHIFT` arm is likewise reached only past
    // the region test.
    if (m_linkAtDown.empty()) {
        SelPoint s, e;
        if (GetNormalizedSel(s, e)) {
            const SelPoint click = MouseToSelPoint(evt);
            if (s <= click && click < e) {
                evt.Skip();
                return;
            }
        }
    }

    // Begin a new selection.  Anchor = focus = click position (empty selection).
    m_selAnchor = MouseToSelPoint(evt);
    m_selFocus  = m_selAnchor;
    NotifySelectionMaybeChanged();           // CMUSHView::OnLButtonDown, :2263
    m_dragging  = true;
    m_dragStartY = evt.GetY();
    CaptureMouse();
    Refresh(false);
    evt.Skip();
}

void COutputPane::OnLeftUp(wxMouseEvent& evt)
{
    if (m_fn_miniwindow_mouse_up)
        m_fn_miniwindow_mouse_up(evt.GetX(), evt.GetY(), 0x10 /*MW_MOUSE_LH*/);

    if (m_dragging) {
        m_dragging  = false;
        m_autoScrollTimer.Stop();      // the drag is over; nothing left to chase
        m_selFocus  = MouseToSelPoint(evt);
        if (HasCapture()) ReleaseMouse();
        Refresh(false);
        NotifySelectionMaybeChanged();       // extend_selection, :1980

        // Fire a link click only when the mouse didn't move (no drag).
        // This lets the user drag over link text to select/copy it.
        if (!m_linkAtDown.empty() && m_selAnchor == m_selFocus) {
            if (m_fn_link_clicked)
                m_fn_link_clicked(m_linkAtDown, m_linkTypeAtDown,
                                  m_linkNoteAtDown, m_linkEncAtDown);
        }
        m_linkAtDown.clear();

        // If the release left a non-empty region, freeze so the text stays put
        // under the cursor, then copy it if the world asked for that.  Both are
        // exactly what CMUSHView::OnLButtonUp ends with (mushview.cpp:2453 and,
        // via extend_selection, :1982).  A plain click collapses the region and
        // neither fires.
        FreezeIfSelection();
        AutoCopySelection();
    }

    // Return focus to the input bar so the user can keep typing.
    if (m_fn_return_focus)
        m_fn_return_focus();

    evt.Skip();
}

// ── The one word scanner ─────────────────────────────────────────────────────
// Lifted out of OnLeftDClick unchanged so the macOS select-on-right-click can
// share it.  A second copy would drift: the delimiter set, the halfway-vs-
// containment choice in XToChar and the "no word here" answer all have to match
// what a double-click does, or the same pixel would resolve to two words.
bool COutputPane::WordAtPoint(int x, int y, SelPoint& outStart, SelPoint& outEnd,
                              wxString& outWord) const
{
    if (LineCountI() == 0 || m_lineH <= 0) return false;

    const int lineIdx = YToLine(y);
    if (lineIdx < 0 || lineIdx >= LineCountI()) return false;

    // Build plain text for the line.
    wxString lineText;
    for (const auto& run : LineAt(lineIdx))
        if (!run.horiz_rule)
            lineText += DecodeRunText(run);

    int col = XToChar(lineIdx, x, y);
    col = std::max(0, std::min((int)lineText.size(), col));

    // A character is a word character if it is not in m_wordDelimitersDblClick
    // and not whitespace — matching the Windows MUSHclient convention.
    auto isWordChar = [this](wxUniChar ch) -> bool {
        if (wxIsspace(ch)) return false;
        // Convert to a single-byte char for the delimiter lookup; for
        // non-ASCII code points (which can't be delimiters) treat as word.
        if (ch > 127) return true;
        char c = static_cast<char>(ch.GetValue());
        return m_wordDelimitersDblClick.find(c) == std::string::npos;
    };

    // Expand left to word start.
    int wordStart = col;
    while (wordStart > 0 && isWordChar(lineText[wordStart - 1]))
        --wordStart;

    // Expand right to word end.
    int wordEnd = col;
    while (wordEnd < (int)lineText.size() && isWordChar(lineText[wordEnd]))
        ++wordEnd;

    if (wordEnd <= wordStart) return false;

    outStart = SelPoint{lineIdx, wordStart};
    outEnd   = SelPoint{lineIdx, wordEnd};
    outWord  = lineText.Mid(wordStart, wordEnd - wordStart);
    return true;
}

void COutputPane::OnLeftDClick(wxMouseEvent& evt)
{
    if (m_fn_miniwindow_mouse_down &&
        m_fn_miniwindow_mouse_down(evt.GetX(), evt.GetY(), 0x50 /*LH|DBL*/))
    {
        evt.Skip();
        return;
    }

    // Double-click: select the word under the cursor.
    {
        int lineIdx = YToLine(evt.GetY());
        if (lineIdx >= 0 && lineIdx < LineCountI()) {
            SelPoint wordStartPt, wordEndPt;
            wxString word;
            if (WordAtPoint(evt.GetX(), evt.GetY(), wordStartPt, wordEndPt, word)) {
                m_selAnchor = wordStartPt;
                m_selFocus  = wordEndPt;
                Refresh(false);

                // Windows freezes here, BEFORE the word is dispatched
                // (mushview.cpp:4837, ahead of OutputWordDoubleClicked) --
                // and notifies in the same order (:4790), so a plugin sees
                // the selection change before any send it triggers.
                FreezeIfSelection();
                NotifySelectionMaybeChanged();

                // End the drag before anything else.  Two reasons, and both
                // matter: (a) the handler below can open a modal (the
                // not-connected prompt) and on macOS a nested modal loop
                // swallows the following mouse-up, which would strand the pane
                // mid-drag with the mouse still captured; (b) the platform
                // delivers DOWN, UP, DCLICK, UP, and that trailing UP would
                // otherwise re-run the drag branch of OnLeftUp and collapse the
                // word selection onto the click point.  Windows is immune for
                // the same reason: OnLButtonDblClk never SetCapture()s, so the
                // trailing WM_LBUTTONUP hits `if (GetCapture() != this) return;`
                // (mushview.cpp:2430) and leaves the word alone.  This used to
                // sit INSIDE the callback guard, so it only held when a frame
                // had wired m_fn_word_double_clicked.
                m_dragging = false;
                m_autoScrollTimer.Stop();
                if (HasCapture()) ReleaseMouse();

                // "Double-click Sends Word" / "…Pastes Word".  Dispatch the word
                // computed RIGHT HERE, not SelectedText(): the selection is only
                // written inside this guard, so a double-click that lands on a
                // space or a delimiter leaves the PREVIOUS selection in place --
                // possibly a whole dragged paragraph -- and sending that would be
                // badly wrong.  No word, no dispatch, which is also what Windows
                // does (it computes an empty word and tests IsEmpty first).
                bool aborted = false;
                if (m_fn_word_double_clicked) {
                    aborted = !m_fn_word_double_clicked(word, evt.ShiftDown());
                }

                // ...and copy the word if the world asked for that, which is
                // where OnLButtonDblClk ends on Windows (mushview.cpp:4884).
                // The port only auto-copied on the DRAG path, so a
                // double-clicked word highlighted while the clipboard kept the
                // previous drag — the user pasted stale text.  eDblClickAbort
                // skips it, as the early `return` does on Windows.
                if (!aborted) AutoCopySelection();
            }

            // Record for triple-click line selection.
            m_lastDClickTimeMs = wxGetLocalTimeMillis();
            m_lastDClickLine   = lineIdx;
        }
    }

    evt.Skip();
}

// ── Middle button ─────────────────────────────────────────────────────────────
// Both edges go to the miniwindow hotspot layer first.  MW_MOUSE_MIDDLE (0x200)
// was never dispatched at all once, so a plugin hotspot with a middle-click
// callback never fired; Windows dispatches both edges (CMUSHView::OnMButtonDown
// mushview.cpp:2304, OnMButtonUp mushview.cpp:2420).
//
// Past the hotspot, Windows does NOTHING with a middle click over the output
// window: both handlers chain to CView::OnMButtonDown/Up, which are the MFC
// defaults, straight through to DefWindowProc, and CSendView has no
// middle-button handling at all.  In particular there is no paste.
void COutputPane::OnMiddleDown(wxMouseEvent& evt)
{
    if (m_fn_miniwindow_mouse_down &&
        m_fn_miniwindow_mouse_down(evt.GetX(), evt.GetY(), 0x200 /*MW_MOUSE_MIDDLE*/))
        return;
    evt.Skip();
}

// OnMiddleUp is where the paste lived.  On GTK the X11 primary-selection paste
// is the platform convention and is deliberately kept.  Everywhere else the
// same code read the SYSTEM clipboard and dropped it into the command bar,
// which is a convention neither macOS nor Windows has — a stray middle click
// silently rewrote whatever the user was typing.  That arm is gone, and the
// event is consumed rather than skipped so "middle-click does nothing" is a
// decision the handler makes and a test can observe, not the accident of no
// one further up the chain caring.
void COutputPane::OnMiddleUp(wxMouseEvent& evt)
{
    if (m_fn_miniwindow_mouse_up &&
        m_fn_miniwindow_mouse_up(evt.GetX(), evt.GetY(), 0x200 /*MW_MOUSE_MIDDLE*/))
        return;

#ifdef __WXGTK__
    // On GTK (Linux/X11), primary selection = last text highlighted anywhere,
    // and middle-click pasting it is the desktop-wide convention.  wxWidgets
    // exposes it by temporarily switching the clipboard into primary-selection
    // mode.  The text is inserted verbatim without being sent to the MUD, so
    // the user can review and edit it before pressing Enter.
    wxString text;
    if (wxTheClipboard->Open()) {
        wxTheClipboard->UsePrimarySelection(true);
        if (wxTheClipboard->IsSupported(wxDF_TEXT) ||
            wxTheClipboard->IsSupported(wxDF_UNICODETEXT))
        {
            wxTextDataObject data;
            wxTheClipboard->GetData(data);
            text = data.GetText();
        }
        wxTheClipboard->UsePrimarySelection(false);
        wxTheClipboard->Close();
    }

    if (!text.IsEmpty() && m_fn_put_in_input)
        m_fn_put_in_input(text);

    evt.Skip();
#else
    // macOS (and Windows, if ever reached): match CView::OnMButtonUp — the
    // click is over the output window, the hotspot layer did not want it, and
    // nothing else may act on it.  Deliberately no evt.Skip().
    (void) evt;
#endif
}

void COutputPane::OnRightUp(wxMouseEvent& evt)
{
    if (m_fn_miniwindow_mouse_up)
        m_fn_miniwindow_mouse_up(evt.GetX(), evt.GetY(), 0x20 /*MW_MOUSE_RH*/);
    evt.Skip();
}

// CHotspot::m_Cursor (-1..12) -> wx stock cursor, mirroring the exact
// Windows mapping in CMUSHView::OnSetCursor (docs/PAL.md S4: the cursor
// parity fix — the port previously ignored hotspot cursors entirely).
// 5 (up arrow) has no wx stock equivalent -> plain arrow (best effort).
static wxCursor MiniWindowCursor(int id)
{
    switch (id) {
    default:
    case -1: return wxCursor(wxCURSOR_BLANK);           // no cursor
    case  0: return wxCursor(wxCURSOR_ARROW);           // arrow
    case  1: return wxCursor(wxCURSOR_HAND);            // hand
    case  2: return wxCursor(wxCURSOR_IBEAM);           // I-beam
    case  3: return wxCursor(wxCURSOR_CROSS);           // + (cross)
    case  4: return wxCursor(wxCURSOR_WAIT);            // wait
    case  5: return wxCursor(wxCURSOR_ARROW);           // up arrow (approx.)
    case  6: return wxCursor(wxCURSOR_SIZENWSE);        // arrow nw-se
    case  7: return wxCursor(wxCURSOR_SIZENESW);        // arrow ne-sw
    case  8: return wxCursor(wxCURSOR_SIZEWE);          // arrow e-w
    case  9: return wxCursor(wxCURSOR_SIZENS);          // arrow n-s
    case 10: return wxCursor(wxCURSOR_SIZING);          // all ways
    case 11: return wxCursor(wxCURSOR_NO_ENTRY);        // (X) no
    case 12: return wxCursor(wxCURSOR_QUESTION_ARROW);  // help
    }
}

// ── ScrollByRows ─────────────────────────────────────────────────────────────
// One visual row per unit, up or down.
//
// This exists because the drag auto-scroll called wxWindow::ScrollLines, and on
// this toolkit that is the BASE CLASS body -- `virtual bool ScrollLines(int)
// { return false; }` (wx/window.h:1423).  wxScrolledWindow does not override
// it; only two ribbon classes in the whole library do.  So the pane's
// drag-past-the-edge scroll has never moved anything at all, in either
// direction: the call compiled, ran, returned false and was discarded.  Scroll()
// is what every other scrolling site in this class already uses.
//
// Deliberately does NOT re-evaluate the freeze from the new position
// (RefreshAutoScrollFromPosition).  A drag scroll is not a user scroll: the
// freeze during a drag is decided by FreezeIfSelection, which both callers run,
// and re-deciding it from the position here THAWS a just-frozen selection
// whenever the viewport is already against the end -- which is exactly the
// state a drag near the bottom of a short buffer is in.
void COutputPane::ScrollByRows(int rows)
{
    if (rows == 0) return;
    int unitH = 0;
    GetScrollPixelsPerUnit(nullptr, &unitH);
    if (unitH <= 0) return;
    // Ask for the row pitch rather than assuming the scroll rate is still
    // m_lineH: AdoptMeasuredMetrics can change it after construction.
    const int units = std::max(1, pal::pixelsToScrollUnits(m_lineH, unitH));
    int scrollX = 0, scrollY = 0;
    GetViewStart(&scrollX, &scrollY);
    Scroll(scrollX, std::max(0, scrollY + rows * units));   // Scroll clamps the far end
}

bool COutputPane::PointIsOverUsableLink(int clientX, int clientY) const
{
    SelPoint pt = PointToChar(clientX, clientY,
                              false /*containment: hover, not caret*/);
    if (!pt.valid() || pt.line < 0 || pt.line >= LineCountI()) return false;
    // pt.col is the absolute char column in the logical line (already
    // wrap-aware via XToChar in PointToChar).
    return IsUsableLink(RunAtCharColumn(LineAt(pt.line), pt.col));
}

int COutputPane::DragBottomEdge() const
{
    int clientH;
    GetClientSize(nullptr, &clientH);
    // When the hint bar is visible it is painted over the last rows, so treat
    // its upper edge as the boundary and the drag does not fight the hint bar's
    // own click region.
    return (!m_autoScroll && LineCountI() != 0) ? clientH - kHintBarH - 10
                                                : clientH - 10;
}

// ── AutoScrollTick ───────────────────────────────────────────────────────────
// See the header.  One repeat of CMUSHView::mouse_still_down
// (mushview.cpp:2550-2634): scroll a line, drag the selection out to the
// remembered pointer position, and ask to be called again.
bool COutputPane::AutoScrollTick()
{
    // Windows' gate is `GetCapture () != this` (mushview.cpp:2556).  m_dragging
    // is the port's twin: set only by the mouse-down that captures, cleared by
    // the release, by the double-click's early drag end, and by OnCaptureLost.
    if (!m_dragging) return false;

    const int bottomEdge = DragBottomEdge();
    if (m_lastDragY > bottomEdge)
        ScrollByRows(1);
    else if (m_lastDragY < 10)
        ScrollByRows(-1);
    else
        return false;      // back inside the text: mushview.cpp:2604's
                           // `else return 0; // don't want any more idle calls`

    // Re-extend to the SAME pointer position, which now names a different line
    // because the view moved under it.  Deliberately NOT Windows'
    // `m_last_mouse_position.y += m_FontHeight` walk (mushview.cpp:2601): that
    // exists because calculate_line_and_column is handed a PRE-scroll device
    // point, where PointToChar already folds GetViewStart into its answer, so
    // bumping the stored y here would double-count and run the selection ahead
    // of the scroll.
    m_selFocus = PointToChar(m_lastDragX, m_lastDragY);
    FreezeIfSelection();
    Refresh(false);
    NotifySelectionMaybeChanged();           // extend_selection, :1980
    return true;
}

// Capture taken away from under a drag -- a modal, a system gesture, or another
// window grabbing it.  wx requires this handler once CaptureMouse has been
// called, and it is what stops an auto-scroll repeat that would otherwise have
// no release to end it.
void COutputPane::OnCaptureLost(wxMouseCaptureLostEvent&)
{
    m_dragging = false;
    m_autoScrollTimer.Stop();
}

void COutputPane::OnMotion(wxMouseEvent& evt)
{
    int mwCursor = -999;
    if (m_fn_miniwindow_mouse_move)
        mwCursor = m_fn_miniwindow_mouse_move(evt.GetX(), evt.GetY());

    // Tooltip update (skip during drag to avoid flicker).
    if (!m_dragging && m_fn_get_tooltip) {
        // Already decoded by the frame -- see m_fn_get_tooltip.  It used to
        // arrive as bytes and be read with bare FromUTF8, so an MXP hint with
        // one legacy-charset byte in it showed NO tooltip at all.
        const wxString wxTip = m_fn_get_tooltip(evt.GetX(), evt.GetY());
        if (wxTip.empty()) {
            UnsetToolTip();
        } else {
            wxToolTip* existing = GetToolTip();
            if (!existing || existing->GetTip() != wxTip)
                SetToolTip(wxTip);
        }
    }

    if (m_dragging && evt.LeftIsDown()) {
        m_selFocus = MouseToSelPoint(evt);
        Refresh(false);
        NotifySelectionMaybeChanged();       // extend_selection, :1980

        // Windows freezes the instant the dragged region becomes non-empty, not
        // at the release (mushview.cpp:2613, CMUSHView::OnMouseMove) — on a busy
        // MUD the difference is a whole drag's worth of scrolling.
        FreezeIfSelection();

        // Auto-scroll when dragging past the top or bottom edges.  Windows
        // does exactly this one scroll per move too (CMUSHView::OnMouseMove,
        // mushview.cpp:2510-2517); what it ALSO has, and what the timer below
        // supplies, is the repeat while the pointer sits still past the edge.
        m_lastDragX = evt.GetX();
        m_lastDragY = evt.GetY();
        const int bottomEdge = DragBottomEdge();
        if (evt.GetY() > bottomEdge)
            ScrollByRows(1);
        else if (evt.GetY() < 10)
            ScrollByRows(-1);

        if (evt.GetY() > bottomEdge || evt.GetY() < 10) {
            if (!m_autoScrollTimer.IsRunning())
                m_autoScrollTimer.Start(kAutoScrollMs);
        } else {
            m_autoScrollTimer.Stop();   // back inside: mouse_still_down's
        }                               // `else return 0` (mushview.cpp:2604)
    } else {
        // Update cursor.  Priority order:
        //   1. Hint bar area → pointing hand (clicking snaps to bottom).
        //   2. Over a miniwindow → its hotspot cursor (-1..12), or arrow.
        //   3. Over an MXP / URL link run → pointing hand.
        //   4. Anywhere else → I-beam text cursor.
        int clientH;
        GetClientSize(nullptr, &clientH);
        if (!m_autoScroll && LineCountI() != 0 &&
            evt.GetY() >= clientH - kHintBarH)
        {
            SetCursor(wxCursor(wxCURSOR_HAND));
        } else if (mwCursor != -999) {
            SetCursor(MiniWindowCursor(mwCursor));
        } else {
            SetCursor(PointIsOverUsableLink(evt.GetX(), evt.GetY())
                          ? wxCursor(wxCURSOR_HAND)
                          : wxCursor(wxCURSOR_IBEAM));
        }
    }
    evt.Skip();
}

// Right DOUBLE-click.  wxOSX sends the second rapid right-click as
// EVT_RIGHT_DCLICK, not a second EVT_RIGHT_DOWN, and nothing bound it -- so it
// reached neither the hotspot nor the context menu.  Windows has the arm:
// mushview.cpp:4893 OnRButtonDblClk dispatches MW_MOUSE_RH | MW_MOUSE_DBL.
// Falling through to OnRightDown afterwards keeps the context menu reachable.
void COutputPane::OnRightDClick(wxMouseEvent& evt)
{
    if (m_fn_miniwindow_mouse_down &&
        m_fn_miniwindow_mouse_down(evt.GetX(), evt.GetY(),
                                   0x60 /*MW_MOUSE_RH | MW_MOUSE_DBL*/))
        return;
    OnRightDown(evt);
}

// Pointer left the pane.  Windows arms TME_LEAVE for a hotspot that has a
// CancelMouseOver handler (miniwindow_compose.cpp:624) and its WM_MOUSELEAVE
// arm calls Mouse_Move_MiniWindow with CPoint(-1,-1) (mushview.cpp:6785) so the
// hotspot un-highlights.  Nothing bound the wx equivalent, so a hotspot stayed
// lit after the pointer left the window entirely.
void COutputPane::OnLeaveWindow(wxMouseEvent& evt)
{
    // Not during a drag: wx delivers leave events while the button is held and
    // the pane has capture, where Windows keeps tracking the same hotspot.
    if (!HasCapture() && m_fn_miniwindow_mouse_move)
        m_fn_miniwindow_mouse_move(-1, -1);
    evt.Skip();
}

void COutputPane::OnRightDown(wxMouseEvent& evt)
{
    if (TryAliasMenu(evt)) return;   // macOS: Ctrl+click arrives as right-click

    if (m_fn_miniwindow_mouse_down &&
        m_fn_miniwindow_mouse_down(evt.GetX(), evt.GetY(), 0x20 /*MW_MOUSE_RH*/))
    {
        // A hotspot claimed the click: the plugin gets it, and NOTHING else
        // happens — no selection change, no menu.  Deliberately before the
        // word selection below, because a miniwindow sits ON TOP of the text
        // and the characters behind it were never the target.
        evt.Skip();
        return;
    }

    // ── DELIBERATE macOS-vs-Windows difference ───────────────────────────────
    // Windows does not resolve a right-click to anything: CMUSHView::OnRButtonDown
    // (mushview.cpp:2283) offers the point to the miniwindow layer and then
    // falls straight through to CView, and every context-dependent handler —
    // OnDisplayHighlightphrase (:5921), OnDisplayGotourl (:4416),
    // OnDisplaySendmailto (:4456), OnDisplayBookmarkselection (:4924),
    // OnDisplayGotobookmark (:4854) — reads the selection the user made
    // EARLIER.  Right-clicking a word you have not selected therefore offers a
    // menu whose interesting half acts on something else entirely.
    //
    // macOS text views select the word under the pointer instead, and this port
    // follows the platform (owner's decision, 2026-08).  The rule has two
    // halves and the second is the load-bearing one:
    //
    //   * no selection  → select the word under the pointer, so the menu's
    //                     context-dependent items have something sensible to
    //                     act on;
    //   * a selection   → leave it EXACTLY as it is.  Copy is the most-used
    //                     item in this menu and it copies the selection; a
    //                     right-click that moved the selection first would make
    //                     it copy the wrong thing.
    //
    // What counts as "not on a word" is the SCANNER's answer rather than the
    // pixel's, and deliberately so: it is the same scanner the double-click
    // uses (WordAtPoint), so right-clicking a point and double-clicking it can
    // never disagree.  The pointer resolves to a caret POSITION and the word
    // grows outwards from there, which gives:
    //
    //   * the single gap beside a word takes that word — its caret position is
    //     adjacent to it — exactly as a double-click in the same gap does;
    //   * past the end of a line, or in the empty area below the last one (which
    //     YToLine clamps onto it), the caret lands at the end of that text and
    //     takes the last word, where NSTextView puts its caret too;
    //   * a caret with no word character on EITHER side — inside a run of two
    //     or more spaces or delimiters, or on an empty line — takes nothing and
    //     clears nothing.  The selection-dependent items are then greyed, which
    //     is honest.
    //
    // This is a macOS/Windows platform difference rather than a divergence from
    // upstream on both platforms, so it is recorded here at the site and not in
    // docs/UPSTREAM-DIVERGENCES.md.
    {
        SelPoint curStart, curEnd;
        if (!GetNormalizedSel(curStart, curEnd)) {
            SelPoint wordStart, wordEnd;
            wxString word;
            if (WordAtPoint(evt.GetX(), evt.GetY(), wordStart, wordEnd, word)) {
                m_selAnchor = wordStart;
                m_selFocus  = wordEnd;
                Refresh(false);
            }
        }
    }

    NotifySelectionMaybeChanged();     // the macOS select-on-right-click
    wxMenu menu;
    BuildContextMenu(menu, evt.GetX(), evt.GetY());
    if (m_fn_popup_context_menu) m_fn_popup_context_menu(menu, evt.GetPosition());
    else                         PopupMenu(&menu, evt.GetPosition());
}

// ── The right-click menu ─────────────────────────────────────────────────────
// Windows' CG_IDR_POPUP_MUSHVIEW and the port's own output-pane menu, merged.
// Grouped by WHAT THE USER IS TRYING TO DO rather than by which of the two
// menus an item came from — the groups, top (nearest the pointer) to bottom:
//
//   1. the link under the pointer   only when there is one; Safari-shaped, and
//                                   if you right-clicked a link that IS what
//                                   you meant.  (The port's stand-in for the
//                                   IDR_MXP_MENU arm CMUSHView::OnContextMenu
//                                   runs before its own popup, mushview.cpp:3560.)
//   2. clipboard                    Copy / Copy as HTML / Select All — Copy is
//                                   the single most-used item, so it leads.
//   3. do something with the        Put in Input Bar, Send to MUD, Go to URL,
//      selection, right now         Send Mail To
//   4. turn the selection into a    Highlight Word, Multi-Line Trigger — same
//      standing rule                input, but these outlive the click, which
//                                   is why they are not in group 3
//   5. search and navigate          Find / Find Again / Recall Text /
//      the scrollback               Bookmark Selection / Go to Bookmark
//   6. how the output looks         Pause, Wrap, Zoom In/Out/Reset, Full Screen
//   7. this world and this session  Logging, Activity List, Notepad,
//                                   World Configuration
//   8. destructive, on its own      Clear Output Buffer
//
// Order within a group is by expected frequency.  Nothing is omitted for
// lacking a port equivalent; anything unwired is appended greyed instead.
void COutputPane::BuildContextMenu(wxMenu& menu, int x, int y)
{
    SelPoint s, e;
    bool hasSel = GetNormalizedSel(s, e);

    // Append a canonical MFC command.  The ITEM carries the canonical id;
    // `dispatchId` is what actually gets run, which differs only for Notepad.
    // Enable/check come from wx_canonical_menu_ui_state through
    // m_fn_menu_ui_state — the same two sources the menu bar's update-UI asks,
    // in the same order — rather than from a third set of predicates here.
    auto canon = [&](int id, const wxString& label,
                     bool checkable = false, bool needsSel = false,
                     int dispatchId = 0) -> wxMenuItem*
    {
        wxMenuItem* item = checkable ? menu.AppendCheckItem(id, label)
                                     : menu.Append(id, label);
        // Unwired ⇒ greyed.  An item that cannot reach a handler must not look
        // available; this is also what the headless/selftest pane gets.
        bool enabled = (m_fn_menu_command != nullptr);
        int  checked = -1;
        if (enabled && m_fn_menu_ui_state) {
            bool en = true;
            int  ck = -1;
            if (m_fn_menu_ui_state(id, en, ck)) { enabled = en; checked = ck; }
        }
        // The selection gate, only where Windows has one:
        // ON_UPDATE_COMMAND_UI(…, OnUpdateNeedSelection) (mushview.cpp:128, :159).
        if (needsSel && !hasSel) enabled = false;
        menu.Enable(id, enabled);
        if (checkable && checked >= 0) item->Check(checked != 0);
        if (m_fn_menu_command) {
            const int cmd = dispatchId ? dispatchId : id;
            menu.Bind(wxEVT_MENU,
                      [this, cmd](wxCommandEvent&) { m_fn_menu_command(cmd); }, id);
        }
        return item;
    };

    // Detect whether the click landed on a link run.
    // XToChar returns the absolute char column in the logical line,
    // correctly accounting for visual-row wrapping.
    std::string linkAtClick;
    std::string linkHintAtClick;   // the MUD's labels for the alternatives
    int  linkTypeAtClick = 0;
    bool linkNoteAtClick = false;
    SRunEncoding linkEncAtClick;   // how to read linkAtClick's bytes
    {
        int lineIdx = YToLine(y);
        if (lineIdx >= 0 && lineIdx < LineCountI()) {
            int col = XToChar(lineIdx, x, y,
                              false /*containment*/);
            const SOutputRun* run = RunAtCharColumn(LineAt(lineIdx), col);
            if (IsUsableLink(run)) {
                linkAtClick     = run->action();
                linkHintAtClick = run->hint();
                linkTypeAtClick = run->action_type();
                linkNoteAtClick = LineAt(lineIdx).comment;
                linkEncAtClick  = run->encoding();
            }
        }
    }

    // ── 1. the link under the pointer ────────────────────────────────────────
    // First because if you right-clicked a link, the link IS what you meant —
    // the same order Safari and every other macOS app uses.  This cluster is
    // the port's stand-in for the IDR_MXP_MENU arm CMUSHView::OnContextMenu
    // runs BEFORE it ever reaches CG_IDR_POPUP_MUSHVIEW (mushview.cpp:3560),
    // so it is parity behaviour and leads for the same reason it does there.
    if (!linkAtClick.empty()) {
        bool isURL = (linkAtClick.compare(0, 7, "http://")  == 0 ||
                      linkAtClick.compare(0, 8, "https://") == 0);

        if (isURL) {
            wxMenuItem* itemOpen = menu.Append(wxID_ANY, "Open URL in Browser");
            menu.Bind(wxEVT_MENU,
                [this, linkAtClick, linkTypeAtClick, linkNoteAtClick,
                 linkEncAtClick](wxCommandEvent&) {
                    if (m_fn_link_clicked)
                        m_fn_link_clicked(linkAtClick, linkTypeAtClick,
                                          linkNoteAtClick, linkEncAtClick);
                }, itemOpen->GetId());

            wxMenuItem* itemCopyURL = menu.Append(wxID_ANY, "Copy URL");
            menu.Bind(wxEVT_MENU,
                [linkAtClick, linkEncAtClick](wxCommandEvent&) {
                    if (wxTheClipboard->Open()) {
                        wxTheClipboard->SetData(new wxTextDataObject(
                            DecodeRunBytes(linkAtClick, linkEncAtClick)));
                        wxTheClipboard->Close();
                    }
                }, itemCopyURL->GetId());
        } else {
            // MXP send-link or Lua Hyperlink() action.
            // Split pipe-delimited alternatives and show each as a menu item.
            // ONE splitter for the actions and the hints: the pairing below
            // compares their COUNTS, so two splitters with different ideas
            // about an empty segment would silently mislabel every item.
            auto splitPipes = [](const std::string& s) {
                std::vector<std::string> out;
                size_t start = 0;
                for (;;) {
                    size_t sep = s.find('|', start);
                    std::string part = s.substr(start,
                        (sep == std::string::npos) ? std::string::npos : sep - start);
                    if (!part.empty()) out.push_back(std::move(part));
                    if (sep == std::string::npos) break;
                    start = sep + 1;
                }
                return out;
            };
            const std::vector<std::string> choices = splitPipes(linkAtClick);
            const std::vector<std::string> hints   = splitPipes(linkHintAtClick);
            // "more hints than actions? first one must be the tooltip text"
            // (mushview.cpp:3556-3558) -- <send "n|s" hint="Exits|Go north|Go
            // south"> gives the run's TOOLTIP first and then one label each.
            const size_t hintBase = (hints.size() > choices.size()) ? 1 : 0;
            for (size_t i = 0; i < choices.size(); ++i) {
                const std::string& ch = choices[i];
                // Windows labels each alternative with the MUD's HINT, falling
                // back to the action itself when there is none (mushview.cpp:
                // 3571-3581).  The action is still what gets sent; only the
                // label changes.  DecodeRunBytes, not FromUTF8: one
                // legacy-charset byte used to leave the label empty.
                const size_t h = hintBase + i;
                wxString label = DecodeRunBytes(
                    (h < hints.size()) ? hints[h] : ch, linkEncAtClick);
                // "ampersands won't come out right" (mushview.cpp:3583): a bare
                // '&' is a mnemonic marker to wx exactly as it is to Win32.
                label.Replace(wxT("&"), wxT("&&"));
                wxMenuItem* item = menu.Append(wxID_ANY, label);
                menu.Bind(wxEVT_MENU,
                    [this, cmd = ch, linkTypeAtClick, linkNoteAtClick,
                     linkEncAtClick](wxCommandEvent&) {
                        if (m_fn_link_clicked)
                            m_fn_link_clicked(cmd, linkTypeAtClick,
                                              linkNoteAtClick, linkEncAtClick);
                    }, item->GetId());
            }
            // No "Send All (in order)" item.  It handed the WHOLE pipe-
            // delimited string to m_fn_link_clicked, which sends the FIRST
            // alternative and only that (the MXP default action —
            // wx_world_child.cpp's link handler, after mushview.cpp:2166-2176),
            // so it never sent all of them in order and, before that handler
            // was fixed, sent nothing at all.  Windows offers no such thing
            // either: IDR_MXP_MENU is exactly one item per alternative
            // (mushview.cpp:3565-3595), and the items above are that.
            wxMenuItem* itemCopy = menu.Append(wxID_ANY, "Copy Action");
            menu.Bind(wxEVT_MENU,
                [linkAtClick, linkEncAtClick](wxCommandEvent&) {
                    if (wxTheClipboard->Open()) {
                        wxTheClipboard->SetData(new wxTextDataObject(
                            DecodeRunBytes(linkAtClick, linkEncAtClick)));
                        wxTheClipboard->Close();
                    }
                }, itemCopy->GetId());
        }
        menu.AppendSeparator();
    }

    // ── 2. clipboard ─────────────────────────────────────────────────────────
    // Copy leads the fixed part of the menu: it is the item the user reaches
    // for most, and on macOS the menu opens with its top edge at the pointer.
    menu.Append(ID_COPY,      "Copy\tCtrl+C");

    // "Copy as HTML": same selection, but as colour-preserving HTML markup
    // (canonical ID_EDIT_COPYASHTML).  Only meaningful with a selection.
    // NOT routed through m_fn_menu_command: the pane's own CopySelectionAsHTML
    // acts on THIS pane's selection, where the frame's arm would resolve the
    // active child all over again.
    {
        wxMenuItem* itemHtml = menu.Append(wxID_ANY, wxT("Copy as HTML"));
        menu.Enable(itemHtml->GetId(), hasSel);
        menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            CopySelectionAsHTML(GetFont().GetFaceName());
        }, itemHtml->GetId());
    }

    menu.Append(ID_SELECTALL, "Select All\tCtrl+A");
    menu.Enable(ID_COPY, hasSel);

    // ── 3. do something with the selection, right now ────────────────────────
    // Four one-shot actions on the selected text.  They are always PRESENT and
    // greyed when there is nothing to act on, rather than appearing and
    // disappearing: a menu whose items move between right-clicks cannot be
    // learned.  (The first two used to be conditional.)
    menu.AppendSeparator();
    {
        // "Put in Input Bar": copy selection into the command input for editing.
        // Ctrl+G is also wired in OnKeyDown for keyboard-only use.
        wxMenuItem* itemInput = menu.Append(wxID_ANY,
            wxT("Put in Input Bar\tCtrl+G"));
        menu.Enable(itemInput->GetId(), hasSel && m_fn_put_in_input != nullptr);
        menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            if (m_fn_put_in_input) m_fn_put_in_input(SelectedText());
        }, itemInput->GetId());

        // "Send to MUD": evaluate the selection as a command immediately.
        wxMenuItem* itemSend = menu.Append(wxID_ANY, wxT("Send to MUD"));
        menu.Enable(itemSend->GetId(), hasSel && m_fn_send_to_mud != nullptr);
        menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            if (m_fn_send_to_mud) m_fn_send_to_mud(SelectedText());
        }, itemSend->GetId());
    }
    canon(ID_CANON_GOTOURL,    wxT("Go To URL…"),    false, true /*needs selection*/);
    canon(ID_CANON_SENDMAILTO, wxT("Send Mail To…"), false, true /*needs selection*/);

    // ── 4. turn the selection into a standing rule ───────────────────────────
    // Same input as group 3, different lifetime: these two create configuration
    // that outlives the click, which is worth a separator's worth of warning.
    // No selection gate — Windows has none on either (no ON_UPDATE_COMMAND_UI
    // in CMUSHView's map), and both open a dialog that is simply pre-filled
    // from whatever is selected.
    menu.AppendSeparator();
    canon(ID_CANON_HIGHLIGHTPHRASE,  wxT("Highlight Word…"));
    canon(ID_CANON_MULTILINETRIGGER, wxT("Multi-Line Trigger…"));

    // ── 5. search and navigate the scrollback ────────────────────────────────
    menu.AppendSeparator();
    canon(ID_CANON_FIND,              wxT("Find…"));
    canon(ID_CANON_FINDAGAIN,         wxT("Find Again"));
    canon(ID_CANON_RECALLTEXT,        wxT("Recall Text…"));
    canon(ID_CANON_BOOKMARKSELECTION, wxT("Bookmark Selection"));
    canon(ID_CANON_GOTOBOOKMARK,      wxT("Go To Bookmark"));

    // ── 6. how the output looks and behaves ──────────────────────────────────
    // The two canonical toggles carry their tick from the canonical update-UI
    // handler; the three zoom items are the port's own and act on this pane.
    menu.AppendSeparator();
    canon(ID_CANON_FREEZEOUTPUT, wxT("Pause Output"), true /*check item*/);
    canon(ID_CANON_WRAPLINES,    wxT("Wrap Output"),  true /*check item*/);
    {
        wxMenuItem* zi = menu.Append(wxID_ANY, "Zoom In\tCtrl++");
        menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { AdjustFontSize(1); }, zi->GetId());
        wxMenuItem* zo = menu.Append(wxID_ANY, "Zoom Out\tCtrl+-");
        menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { AdjustFontSize(-1); }, zo->GetId());
        wxMenuItem* zr = menu.Append(wxID_ANY, "Reset Zoom\tCtrl+0");
        menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { ResetFontSize(); }, zr->GetId());
    }
    canon(ID_CANON_FULLSCREENMODE, wxT("Full Screen Mode"));

    // ── 7. this world and this session ───────────────────────────────────────
    // Windows' own slot: MENUITEM "&Log session...", ID_FILE_LOGSESSION in
    // CG_IDR_POPUP_MUSHVIEW (mushclient_dialogs.rc:2781).  It is the SAME
    // command as File ▸ Log Session and as the Connection menu's Start/Stop
    // Logging arm — all three dispatch 32117 to the document, so there is one
    // session log per world and one implementation of it.
    //
    // It used to be a port-local item driving CWorldChildFrame's own
    // fopen(…,"a") onto a second file handle, which meant "Start Logging" here
    // and "Start Logging" in the menu bar wrote different files in different
    // formats, and only the menu bar's honoured m_bLogOutput / m_log_input /
    // m_bLogNotes, the preamble/postamble and the scrollback dump.
    //
    // Checkable because the canonical update-UI ticks it while a log is open
    // (CMUSHclientDoc::OnUpdateFileLogsession → SetCheck(m_logfile != NULL),
    // doc.cpp:1008); the tick, not a changing caption, is how Windows says
    // "logging is on".
    menu.AppendSeparator();
    canon(ID_CANON_LOGSESSION,      wxT("Log Session…"), true /*check item*/);
    canon(ID_CANON_ACTIVITYLIST,    wxT("Activity List"));
    canon(ID_CANON_NOTESWORKAREA,   wxT("Notepad"), false, false,
          kCanonNotepadDispatch);
    canon(ID_CANON_GAMEPREFERENCES, wxT("World Configuration…"));

    // ── 8. destructive, alone at the far end ─────────────────────────────────
    // The canonical command's own label (kCanonCmds, wx_mfc_impl.cpp:705) —
    // this item IS that command, not a second, weaker one.  Windows binds it to
    // Shift+Ctrl+C; physical Ctrl+L is an extra binding the port adds
    // (COutputPane::OnKeyDown / CWorldChildFrame::OnInputKeyDown, both gated on
    // RawControlDown so it is the literal Control key, not Cmd).
    //
    // NO "\tCtrl+L" on the label, deliberately.  wx renders "Ctrl+" as Cmd on
    // macOS, so that text would promise Cmd+L — and Cmd+L is not free: the
    // View menu's "Clear To Mark" (kAccelClearToMark, wx_mfc_impl.cpp) is a
    // real, natively-registered Cmd+L, a DIFFERENT and much less destructive
    // operation (it keeps everything above the mark; this command keeps
    // nothing). A user who presses what this label promised would silently
    // get that command instead of this one — worse than the accelerator
    // simply doing nothing, since the wrong amount of history survives with
    // no error. Same reasoning, and the same fix shape, as this menu's Find
    // item above (ID_CANON_FIND): its "…\tCtrl+F" was dropped for the same
    // reason -- see "No Ctrl+F accelerator here" at wx_mfc_impl.cpp's Display
    // menu, which this item now matches too.
    menu.AppendSeparator();
    menu.Append(ID_CLEAR, wxT("Clear Output Buffer…"));
}

// Hotspots get FIRST refusal, before the modifier is looked at.  Windows orders
// it that way (mushview.cpp:5160 dispatches to Mouse_Wheel_MiniWindow, and only
// returns to CView at :5165 for Ctrl/Shift), so Ctrl+wheel over a hotspot is a
// scrollwheel callback rather than a zoom.  Testing the modifier first, as this
// used to, made every Ctrl+wheel callback unreachable while the pointer was
// over a miniwindow.
//
// The ctrl bit: the flags the CALLER builds do not carry it, but the flags the
// CALLBACK receives do -- and this comment claimed otherwise between 2026-08-09
// and 2026-08-10, which is worth spelling out because the two are one function
// call apart.
//
// `(delta < 0 ? MW_MOUSE_SCROLL_BACK : 0) | (abs (delta) << 16)` -- built at
// mushview.cpp:5160 here and wx_world_child.cpp there -- indeed never consults
// the keyboard.  But both sides then call the SHARED MouseWheelMiniWindow,
// which reaches SendMouseEventToPlugin (miniwindow_compose.cpp) without passing
// dont_modify_flags; that defaults to false (miniwindow.h), so
// `Flags |= hooks.KeyboardModifiers ()` runs before the callback is dispatched,
// and both frontends' KeyboardModifiers set MW_MOUSE_CTRL from the live key
// state.  So a Ctrl+wheel over a hotspot should reach the plugin as 0x780002.
//
// Proven on this side by platform/miniwindow_selftest.cpp, which drives
// MouseWheelMiniWindow with a hooks object that reports Ctrl and asserts the
// modifiers are consulted -- plus the control that they are NOT consulted when
// the hotspot has no scrollwheel callback, since SendMouseEventToPlugin returns
// before the OR in that case.  Adding `, true` to that call fails exactly the
// first of those and nothing else.
//
// UNRESOLVED, and deliberately not papered over: WINDOWS-VALIDATION.md §8r
// reports a live Windows plugin logging `flags=0x780000` with Ctrl held, i.e.
// no ctrl bit.  The code that computes it is shared, and GetKeyState(VK_CONTROL)
// is what the Windows hooks read, so the two cannot both be right.  The likely
// explanation is that the measurement was read at the call site rather than
// inside the callback, but that is a guess and it needs a Windows re-run before
// anyone relies on either number over there.  What is NOT in doubt is the macOS
// behaviour, which the selftest pins.
//
// Pulled out of the handler because that is a rule, not plumbing, and a wheel
// event cannot be delivered from an in-app selftest (there is no event loop).
COutputPane::WheelAction COutputPane::DecideWheelAction(bool hotspotTookIt,
                                                        bool rawCtrlDown)
{
    if (hotspotTookIt) return WheelAction::HotspotTookIt;
    if (rawCtrlDown)   return WheelAction::Zoom;
    return WheelAction::Scroll;
}

void COutputPane::OnMouseWheel(wxMouseEvent& evt)
{
    // Offered unconditionally, and BEFORE the decision, so the plugin sees the
    // Ctrl+wheel it is entitled to -- see DecideWheelAction.
    const bool hotspotTookIt =
        m_fn_miniwindow_mouse_wheel &&
        m_fn_miniwindow_mouse_wheel(evt.GetX(), evt.GetY(),
                                    evt.GetWheelRotation());

    switch (DecideWheelAction(hotspotTookIt, evt.RawControlDown())) {
    case WheelAction::HotspotTookIt:
        return;
    case WheelAction::Zoom:
        AdjustFontSize((evt.GetWheelRotation() > 0) ? 1 : -1);
        return;
    case WheelAction::Scroll:
        evt.Skip();
        CallAfter([this]() { RefreshAutoScrollFromPosition(); });
        return;
    }
}

void COutputPane::OnKeyDown(wxKeyEvent& evt)
{
    int code = evt.GetKeyCode();

    // Lua Accelerator() API — check before built-in key bindings so scripts
    // can override any key.
    if (m_fn_accelerator_check && m_fn_accelerator_check(evt))
        return;

    // Escape: scroll to bottom (if paused) then return focus to the input bar.
    if (code == WXK_ESCAPE) {
        if (!m_autoScroll && LineCountI() != 0) ScrollToBottom();
        if (m_fn_return_focus) m_fn_return_focus();
        return;
    }

    if (code == 'C' && evt.RawControlDown()) { CopySelection(); return; }
    if (code == 'A' && evt.RawControlDown()) { SelectAll();     return; }
    if (code == 'L' && evt.RawControlDown()) { ClearOutputBuffer(); return; }

    // Ctrl+G: put selected text into the input bar (output→input shortcut).
    if (code == 'G' && evt.RawControlDown()) {
        if (m_fn_put_in_input) {
            wxString sel = SelectedText();
            if (!sel.IsEmpty()) m_fn_put_in_input(sel);
        }
        return;
    }

    if (code == WXK_F3) {
        if (m_fn_find_next_key) m_fn_find_next_key(!evt.ShiftDown());
        return;
    }

    // Ctrl+F: show the find bar and search forward.
    if (code == 'F' && evt.RawControlDown()) {
        if (m_fn_find_next_key) m_fn_find_next_key(true);
        return;
    }

    if (evt.RawControlDown() &&
        (code == '=' || code == '+' || code == WXK_NUMPAD_ADD)) {
        AdjustFontSize(1); return;
    }
    if (evt.RawControlDown() &&
        (code == '-' || code == WXK_NUMPAD_SUBTRACT)) {
        AdjustFontSize(-1); return;
    }
    if (evt.RawControlDown() && code == '0') { ResetFontSize(); return; }

    if (evt.RawControlDown() && code == WXK_HOME) {
        // Ctrl+Home: scroll to the very top of the buffer.
        int scrollX;
        GetViewStart(&scrollX, nullptr);
        Scroll(scrollX, 0);
        bool wasAuto = m_autoScroll;
        m_autoScroll = false;
        if (wasAuto && m_fn_autoscroll_changed) m_fn_autoscroll_changed(false);
        return;
    }
    if (evt.RawControlDown() && code == WXK_END) {
        // Ctrl+End: jump to the end of the buffer -- ID_TEST_END, i.e.
        // CMUSHView::OnTestEnd, which reaches it through ScrollToPosition and
        // so does NOT unfreeze a view whose world says keep_pause_at_bottom.
        // This called ScrollToBottom, the explicit-unfreeze API, so Ctrl+End
        // always resumed output and the next burst scrolled the text away.
        // OnTestEnd also clears the selection (mushview.cpp:1649-1652).
        ScrollToBufferEnd();
        SetSelectionRange(0, 0, 0, 0);
        return;
    }
    if (evt.RawControlDown() &&
        (code == WXK_UP || code == WXK_DOWN)) {
        // Ctrl+Up / Ctrl+Down: nudge the view one line, the Display menu's
        // Line Up / Line Down (ID_TEST_LINEUP / ID_TEST_LINEDOWN).  Here as
        // well as on the command bar for the same reason Ctrl+Home and
        // Ctrl+End are: the key should mean the same thing whichever half of
        // the window has the focus.
        ScrollLine(code == WXK_UP ? -1 : +1);
        return;
    }

    // Forward printable characters to the command input bar so the user can
    // type without having to click back into it after scrolling through output.
    // Only forward when no modifier is held (Ctrl/Alt combos are shortcuts).
    if (!evt.RawControlDown() && !evt.AltDown() && !evt.MetaDown()) {
        wxChar ch = (wxChar)evt.GetUnicodeKey();
        if (ch != WXK_NONE && wxIsprint(ch)) {
            if (m_fn_key_to_input) { m_fn_key_to_input(ch); return; }
        }
    }

    bool isScrollKey = (code == WXK_PAGEUP   || code == WXK_PAGEDOWN ||
                        code == WXK_HOME      || code == WXK_END      ||
                        code == WXK_UP        || code == WXK_DOWN);
    evt.Skip();

    if (isScrollKey)
        CallAfter([this]() { RefreshAutoScrollFromPosition(); });
}

// ── Context menu handlers ─────────────────────────────────────────────────────

void COutputPane::OnMenuCopy(wxCommandEvent&)      { CopySelection(); }
void COutputPane::OnMenuSelectAll(wxCommandEvent&) { SelectAll(); }
// ── ClearOutputBuffer ─────────────────────────────────────────────────────
// Route Ctrl+L and the context-menu item to the canonical Display ▸ Clear
// Output Buffer through the frame, instead of clearing the pane behind the
// engine's back.  See m_fn_clear_output_buffer in the header for why.  The
// direct ClearOutput() is kept as the fallback so a pane with no frame wired
// (the selftest harness) still responds.
void COutputPane::ClearOutputBuffer()
{
    if (m_fn_clear_output_buffer) m_fn_clear_output_buffer();
    else                          ClearOutput();
}

void COutputPane::OnMenuClear(wxCommandEvent&)     { ClearOutputBuffer(); }

// ── Selection / clipboard ─────────────────────────────────────────────────────

void COutputPane::CopySelection()
{
    wxString text = SelectedText();
    if (text.IsEmpty()) return;
    if (wxTheClipboard->Open()) {
        wxTheClipboard->SetData(new wxTextDataObject(text));
        wxTheClipboard->Close();
    }
}

void COutputPane::CopySelectionAsHTML(const wxString& fontName)
{
    SelPoint s, e;
    if (!GetNormalizedSel(s, e)) return;

    int lo = std::max(0, s.line);
    int hi = std::min(LineCountI() - 1, e.line);
    if (lo > hi) return;

    // HTML-escape one byte string (matches Utilities.cpp FixHTMLString:
    // < > & " → entities; UTF-8 bytes pass through untouched).
    auto fixHTML = [](const std::string& in) {
        std::string out;
        out.reserve(in.size());
        for (char ch : in)
            switch (ch) {
            case '<': out += "&lt;";   break;
            case '>': out += "&gt;";   break;
            case '&': out += "&amp;";  break;
            case '"': out += "&quot;"; break;
            default:  out += ch;       break;
            }
        return out;
    };
    // Slice `count` rendered characters from a run starting at character index
    // `startChar`, returned as UTF-8 bytes.  DecodeRunText handles both UTF-8 and
    // legacy-charset worlds and slices in the same RunCharLen space the selection
    // columns use, so this is correct for every world (the HTML is emitted as
    // UTF-8 — see the charset <meta> below).
    auto runSlice = [](const SOutputRun& run, int startChar, int count) -> std::string {
        if (count <= 0) return std::string();
        wxString s = DecodeRunText(run).Mid(startChar, count);
        return std::string(s.utf8_str());
    };

    // Default background = first run's bg on the start line (canonical uses the
    // start line's first style); fall back to the pane background.
    uint32_t defaultBack = m_bgColor;
    if (!LineAt(lo).empty()) defaultBack = LineAt(lo)[0].bg;

    char buf[256];
    std::string html;
    html += "<!-- Produced by MUSHclient - www.mushclient.com -->\n";
    // The slices below are emitted as UTF-8 regardless of the world's charset, so
    // declare it (legacy-charset worlds decode to UTF-8 rather than copying raw
    // 8-bit bytes that a clipboard consumer would mis-render).
    html += "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">\n";
    std::snprintf(buf, sizeof buf,
                  "<table border=0 cellpadding=5 bgcolor=\"#%02X%02X%02X\">\n",
                  (int)GetRValue(defaultBack), (int)GetGValue(defaultBack),
                  (int)GetBValue(defaultBack));
    html += buf;
    html += "<tr><td>\n";

    std::string face;
    if (!fontName.IsEmpty() && fontName != wxT("FixedSys"))
        face = std::string(fontName.utf8_str()) + ", ";
    html += "<pre><code><font size=2 face=\"" + face +
            "FixedSys, Lucida Console, Courier New, Courier\"><font color=\"#0\">\n";

    uint32_t lastFore = 0, lastBack = 0;
    bool bInSpan = false, firstLine = true;

    for (int li = lo; li <= hi; ++li) {
        const SOutputLine& line = LineAt(li);

        int lineLen = LineCharLen(line);

        int startCol = (li == lo) ? std::max(0, (int)s.col) : 0;
        int endCol   = (li == hi) ? std::min(lineLen, (int)e.col) : lineLen;

        if (!firstLine) html += "\n";   // newline between selected lines
        firstLine = false;

        if (!line.empty() && line[0].horiz_rule) {
            html += "&mdash;&mdash;&mdash;&mdash;&mdash;&mdash;&mdash;&mdash;";
            continue;
        }
        if (endCol <= startCol) continue;   // nothing selected on this line

        int col = 0;   // running char position within the line
        for (const auto& run : line) {
            if (run.horiz_rule) continue;
            int runLen = RunCharLen(run);
            if (runLen <= 0) continue;

            int runStart = col;
            int runEnd   = col + runLen;
            col = runEnd;

            int from = std::max(runStart, startCol);
            int to   = std::min(runEnd,   endCol);
            if (to <= from) {
                if (runStart >= endCol) break;   // past the selection on this line
                continue;
            }

            uint32_t colour1 = run.fg, colour2 = run.bg;
            if (colour1 != lastFore || colour2 != lastBack) {
                if (bInSpan) { html += "</span>"; bInSpan = false; }
                std::snprintf(buf, sizeof buf, "</font><font color=\"#%02X%02X%02X\">",
                              (int)GetRValue(colour1), (int)GetGValue(colour1),
                              (int)GetBValue(colour1));
                html += buf;
                // Only open a <span> when the background differs from the default
                // (canonical optimisation — keeps the markup small).
                if (colour2 != defaultBack) {
                    std::snprintf(buf, sizeof buf,
                        "<span style=\"color: #%02X%02X%02X; background: #%02X%02X%02X\">",
                        (int)GetRValue(colour1), (int)GetGValue(colour1), (int)GetBValue(colour1),
                        (int)GetRValue(colour2), (int)GetGValue(colour2), (int)GetBValue(colour2));
                    html += buf;
                    bInSpan = true;
                }
                lastFore = colour1;
                lastBack = colour2;
            }

            if (run.underline) html += "<u>";
            html += fixHTML(runSlice(run, from - runStart, to - from));
            if (run.underline) html += "</u>";

            if (col >= endCol) break;
        }
    }

    if (bInSpan) html += "</span>";
    html += "</font></font></code></pre>\n</td></tr></table>\n";

    if (wxTheClipboard->Open()) {
        wxTheClipboard->SetData(
            new wxTextDataObject(wxString::FromUTF8(html.c_str(), html.size())));
        wxTheClipboard->Close();
    }
}

void COutputPane::SelectAll()
{
    if (LineCountI() == 0) return;
    int lastLine = LineCountI() - 1;
    int lastCol  = LineCharLen(LineAt(lastLine));

    m_selAnchor = SelPoint{0, 0};
    m_selFocus  = SelPoint{lastLine, lastCol};
    Refresh(false);
    NotifySelectionMaybeChanged();
}

wxString COutputPane::SelectedText() const
{
    SelPoint selStart, selEnd;
    if (!GetNormalizedSel(selStart, selEnd)) return wxEmptyString;

    int lo = std::max(0, selStart.line);
    int hi = std::min(LineCountI() - 1, selEnd.line);

    wxString result;
    // Reserve from the wrap cache's per-line character counts rather than
    // growing geometrically: a multi-hundred-megabyte selection otherwise
    // reallocates and copies its way up in doubling steps, and wxString on this
    // build is not a cheap thing to copy.  An estimate is enough -- reserve
    // never has to be exact, only close.
    {
        size_t estimate = 0;
        for (int i = lo; i <= hi && i < (int)m_lineChars.size(); ++i)
            estimate += (size_t)m_lineChars[(size_t)i] + 1;   // +1 for the newline
        if (estimate) result.reserve(estimate);
    }
    for (int i = lo; i <= hi; ++i) {
        if (!LineAt(i).empty() && LineAt(i)[0].horiz_rule) {
            result += wxT("────────────────────────────────────────\n");
            continue;
        }

        // Build plain text for this line.
        wxString lineText;
        for (const auto& run : LineAt(i))
            if (!run.horiz_rule)
                lineText += DecodeRunText(run);

        int startCol = (i == lo) ? selStart.col : 0;
        int endCol   = (i == hi) ? selEnd.col
                                 : (int)lineText.size();
        startCol = std::max(0, std::min((int)lineText.size(), startCol));
        endCol   = std::max(startCol, std::min((int)lineText.size(), endCol));

        result += lineText.substr(startCol, endCol - startCol);
        // A break only where the LINE ended, never where the engine wrapped
        // it: CMUSHView::GetSelection writes ENDLINE only `if
        // (pLine->hard_return)` (mushview.cpp:4069-4085), so a soft-wrapped
        // paragraph reaches the clipboard -- and Send to MUD -- as the one
        // command the user sees, not as one per screen row.  ("\n" rather than
        // ENDLINE's "\r\n" stays a separate, deliberate macOS choice.)  Copy
        // as HTML keeps its unconditional break: OnEditCopyashtml writes one
        // after EVERY line (mushview.cpp:5861/5873/5881).
        if (i < hi && LineAt(i).hardReturn) result += "\n";
    }
    return result;
}

// ── Find / Search ─────────────────────────────────────────────────────────────

// ── LineContains ──────────────────────────────────────────────────────────
// See the header.  The fast path walks the runs' raw UTF-8 bytes into a REUSED
// buffer and folds ASCII in place; the old code built a fresh wxString per line
// and then a second one from .Lower(), i.e. two allocations and a full decode
// for every line of the scrollback on every keypress.
bool COutputPane::LineContains(int i, const wxString& term,
                               const std::string& termLowerAscii,
                               bool caseSensitive, std::string& scratch) const
{
    const SOutputLine& line = LineAt(i);

    bool decodeNeeded = false;
    scratch.clear();
    for (const SOutputRun& run : line) {
        if (run.horiz_rule) continue;
        if (!run.utf8) { decodeNeeded = true; break; }   // 8-bit world: must decode
        scratch += run.text;
    }

    if (!decodeNeeded && !caseSensitive) {
        // ASCII folding is only equivalent to wxString::Lower() when there is
        // nothing above ASCII in play.  A high byte anywhere sends the line to
        // the slow path rather than risking a different answer.
        for (unsigned char c : scratch)
            if (c >= 0x80) { decodeNeeded = true; break; }
    }

    if (decodeNeeded) {
        wxString s;
        for (const SOutputRun& run : line)
            if (!run.horiz_rule)
                s += DecodeRunText(run);
        return caseSensitive ? s.Contains(term) : s.Lower().Contains(term.Lower());
    }

    if (caseSensitive)
        return scratch.find(termLowerAscii) != std::string::npos;

    for (char& c : scratch)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return scratch.find(termLowerAscii) != std::string::npos;
}

// ── RebuildFindCache ──────────────────────────────────────────────────────
// One scan per (term, case flag, buffer generation).  The old code scanned the
// whole buffer a SECOND time on every keypress purely to label the match "n of
// m" -- the 9999 cap bounds the match count, not the lines walked -- so Find
// Next cost two full passes each time it was pressed.
void COutputPane::RebuildFindCache(const wxString& term, bool caseSensitive)
{
    if (m_findCacheValid && m_findCacheGen == m_bufferGen &&
        m_findCacheCase == caseSensitive && m_findCacheTerm == term)
        return;                                    // still good

    m_findCache.clear();
    // NOTE the named local.  utf8_str() can hand back a NON-owning view into the
    // wxString's own buffer, so taking it from the temporary `term.Lower()`
    // leaves it dangling the moment the full expression ends -- which made every
    // search silently match nothing.
    const wxString wanted = caseSensitive ? term : term.Lower();
    const wxScopedCharBuffer utf8 = wanted.utf8_str();
    std::string termBytes(utf8.data(), utf8.length());
    std::string scratch;
    scratch.reserve(256);

    const int n = LineCountI();
    for (int i = 0; i < n && (int)m_findCache.size() < 9999; ++i)
        if (LineContains(i, term, termBytes, caseSensitive, scratch))
            m_findCache.push_back(i);

    m_findCacheTerm  = term;
    m_findCacheCase  = caseSensitive;
    m_findCacheGen   = m_bufferGen;
    m_findCacheValid = true;
}

bool COutputPane::FindNext(const wxString& term, bool forward,
                            int* outMatchLine, int* outOrdinal, int* outTotalMatches,
                            bool caseSensitive)
{
    if (outMatchLine)    *outMatchLine    = -1;
    if (outOrdinal)      *outOrdinal      = 0;
    if (outTotalMatches) *outTotalMatches = 0;

    if (term.IsEmpty() || LineCountI() == 0) return false;

    // For case-insensitive search, lower-case the term once and compare against
    // lower-cased line text.  For case-sensitive search, compare as-is.
    wxString searchTerm = caseSensitive ? term : term.Lower();
    int n = LineCountI();

    SelPoint selStart, selEnd;
    GetNormalizedSel(selStart, selEnd);

    int startIdx;
    if (forward) {
        startIdx = selEnd.valid() ? selEnd.line + 1 : n - 1;
        if (startIdx >= n) startIdx = 0;
    } else {
        startIdx = selStart.valid() ? selStart.line - 1 : 0;
        if (startIdx < 0) startIdx = n - 1;
    }

    // One scan for this (term, case flag, buffer generation); repeated Find
    // Next presses reuse it.
    RebuildFindCache(term, caseSensitive);
    if (m_findCache.empty()) return false;

    // ── the rest of the CURRENT line first ────────────────────────────────
    // Windows visits every OCCURRENCE, not every matching line:
    // CSendView::DoFind sets m_bRepeatOnSameLine (sendvw.cpp:1399, comment "we
    // can find multiple instances on the same line") and FindRoutine builds the
    // line's whole match list, popping one entry per press and only moving on
    // when it is empty (Finding.cpp:123-130, :189-219).  The port's cache is
    // per LINE, so without this the second "gold" in "gold coins and more gold
    // dust" was unreachable -- Find Next jumped past it to the next matching
    // line, or, if that line was the only match, reselected the first "gold"
    // for ever.
    //
    // Only the ONE line the selection is on is re-scanned, so the
    // O(1)-per-keypress cost of the cache is untouched.
    int foundIdx = -1;
    int matchStart = -1, matchEnd = -1;
    std::vector<HighlightHit> hits;
    {
        const int curLine = forward ? (selEnd.valid()   ? selEnd.line   : -1)
                                    : (selStart.valid() ? selStart.line : -1);
        if (curLine >= 0 && curLine < n) {
            MatchesOnLine(curLine, searchTerm, caseSensitive, hits);
            for (const HighlightHit& hit : hits) {
                // Strictly past the current selection, so a repeated press
                // cannot land on the occurrence it is already sitting on.
                if (forward) {
                    if (hit.start >= selEnd.col)
                        { foundIdx = curLine; matchStart = hit.start;
                          matchEnd = hit.end; break; }
                } else {
                    if (hit.end <= selStart.col)
                        { foundIdx = curLine; matchStart = hit.start;
                          matchEnd = hit.end; }   // keep the LAST such one
                }
            }
        }
    }

    // ── otherwise the next matching LINE ──────────────────────────────────
    // The first cached match at or after (forward) / at or before (backward)
    // the start point, wrapping.  upper_bound/lower_bound rather than a walk so
    // this stays O(log matches) with a capped-at-9999 cache.
    if (foundIdx < 0) {
        if (forward) {
            auto it = std::lower_bound(m_findCache.begin(), m_findCache.end(), startIdx);
            foundIdx = (it != m_findCache.end()) ? *it : m_findCache.front();
        } else {
            auto it = std::upper_bound(m_findCache.begin(), m_findCache.end(), startIdx);
            foundIdx = (it != m_findCache.begin()) ? *(it - 1) : m_findCache.back();
        }

        // Select the precise matched substring (not the whole line) so the user
        // can see exactly which occurrence was found: the FIRST on the line
        // going forwards, the LAST going backwards, so that the very next press
        // continues through the line's own occurrences in the direction of
        // travel rather than skipping them.
        MatchesOnLine(foundIdx, searchTerm, caseSensitive, hits);
        if (!hits.empty()) {
            const HighlightHit& hit = forward ? hits.front() : hits.back();
            matchStart = hit.start;
            matchEnd   = hit.end;
        } else {
            // The cache said this line matches and the enumerator disagrees:
            // select from the head rather than nothing, which is what the old
            // `lt.find(...) == npos` arm did.
            matchStart = 0;
            matchEnd   = (int) searchTerm.size();
        }
    }
    (void) n;

    m_selAnchor = SelPoint{foundIdx, matchStart};
    m_selFocus  = SelPoint{foundIdx, matchEnd};
    NotifySelectionMaybeChanged();           // CSendView::DoFind, sendvw.cpp:1437

    // CSendView::DoFind's last act on the view is
    // `m_topview->EnsureSelectionVisible ()` (sendvw.cpp:1475) — the SAME
    // function Go To Line and Go To Bookmark reach through SelectLine.  Find
    // used to centre the match unconditionally and always freeze, so a match
    // already on screen jumped the view anyway, and a match at the very end of
    // the buffer froze the world when Windows leaves it running.
    EnsureSelectionVisible();
    Refresh(false);

    if (outMatchLine) *outMatchLine = foundIdx;

    // "n of m", straight off the cache.  This used to be a SECOND full scan of
    // the buffer on every keypress -- the 9999 cap bounds the match count, not
    // the lines walked -- which is what made Find Next cost two passes.
    if (outOrdinal || outTotalMatches) {
        if (outTotalMatches) *outTotalMatches = (int) m_findCache.size();
        if (outOrdinal) {
            auto it = std::upper_bound(m_findCache.begin(), m_findCache.end(), foundIdx);
            *outOrdinal = (int)(it - m_findCache.begin());
        }
    }

    return true;
}

// ── HighlightHitsForLine ──────────────────────────────────────────────────
// See the header.  Character ranges of the CURRENT search term in line
// `lineIdx` -- the paint's caller, which asks about the term the find bar last
// highlighted with.
void COutputPane::HighlightHitsForLine(int lineIdx, std::vector<HighlightHit>& out)
{
    MatchesOnLine(lineIdx, m_searchTermLower, m_searchCaseSensitive, out);
}

// ── MatchesOnLine ─────────────────────────────────────────────────────────
// See the header.  Every occurrence on one line, using the raw-bytes path
// where that is exactly equivalent.
void COutputPane::MatchesOnLine(int lineIdx, const wxString& termFolded,
                                bool caseSensitive,
                                std::vector<HighlightHit>& out)
{
    out.clear();
    if (termFolded.IsEmpty()) return;
    if (lineIdx < 0 || lineIdx >= LineCountI()) return;
    const SOutputLine& line = LineAt(lineIdx);


    bool fastable = true;
    m_hlScratch.clear();
    for (const auto& run : line) {
        if (run.horiz_rule) continue;
        if (!run.utf8) { fastable = false; break; }
        m_hlScratch += run.text;
    }
    if (fastable)
        for (unsigned char c : m_hlScratch)
            if (c >= 0x80) { fastable = false; break; }

    if (fastable) {
        const wxScopedCharBuffer tb = termFolded.utf8_str();
        const std::string term(tb.data(), tb.length());
        if (!term.empty()) {
            if (!caseSensitive)
                for (char& c : m_hlScratch)
                    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            size_t p = 0;
            while ((p = m_hlScratch.find(term, p)) != std::string::npos) {
                out.push_back({(int)p, (int)(p + term.size())});
                ++p;
            }
        }
    } else {
        wxString plain;
        for (const auto& run : line)
            if (!run.horiz_rule)
                plain += DecodeRunText(run);

        wxString cmpPlain = caseSensitive ? plain : plain.Lower();
        const wxString& cmpTerm = termFolded;
        if (!cmpTerm.empty()) {
            std::wstring pw = cmpPlain.ToStdWstring();
            std::wstring tw = cmpTerm.ToStdWstring();
            size_t p = 0;
            while ((p = pw.find(tw, p)) != std::wstring::npos) {
                out.push_back({(int)p, (int)(p + tw.size())});
                ++p;
            }
        }
    }
}

void COutputPane::HighlightSearch(const wxString& term, bool caseSensitive)
{
    m_searchTerm          = term;
    m_searchCaseSensitive = caseSensitive;
    // Fold the term ONCE here rather than on every line of every paint.
    m_searchTermLower     = caseSensitive ? term : term.Lower();
    Refresh(false);
}

#endif // !_WIN32

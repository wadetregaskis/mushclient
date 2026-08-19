// platform/wx_mfc_hooks.h
// Function-pointer bridge between the MFC compatibility layer (mfc_ui.h) and
// the real wxWidgets implementation (wx_mfc_impl.cpp).
//
// Rules:
//  • This header must NOT include any wxWidgets or MFC/compat headers.
//    It is included from mfc_ui.h (which is included by every TU) so it must
//    compile in any context.
//  • wx_mfc_impl.cpp calls wx_mfc_install_hooks() to populate all pointers
//    before InitInstance() runs.
//  • In mushcore / test builds the pointers stay null and all hooks are no-ops.
//
// Calling convention:
//  All hook functions receive void* for HWND parameters so that mfc_ui.h
//  (which only knows HWND = void*) can pass m_hWnd without needing wx types.

#pragma once

#ifndef _WIN32

#include <cstdint>
#include <cstddef>   // size_t (g_wxhook_sound_play_memory)

// ── Types used in signatures (no wx headers) ─────────────────────────────

struct RECT;      // fully defined in platform/windows_types.h, included first

// ── Hook function pointers ─────────────────────────────────────────────────

// Refresh / repaint
extern void (*g_wxhook_invalidate)(void* hwnd, bool eraseBg);

// Window visibility
extern void (*g_wxhook_show_window)(void* hwnd, int nCmdShow);

// Sync repaint
extern void (*g_wxhook_update_window)(void* hwnd);

// Client-area size
extern void (*g_wxhook_get_client_rect)(const void* hwnd, RECT* r);

// Window title / label
extern void (*g_wxhook_set_window_text)(void* hwnd, const char* text);

// Window position / size
extern void (*g_wxhook_move_window)(void* hwnd, int x, int y, int w, int h, bool repaint);

// Destroy window
extern void (*g_wxhook_destroy_window)(void* hwnd);

// Keyboard focus
extern void (*g_wxhook_set_focus)(void* hwnd);

// Synchronous message send
extern long (*g_wxhook_send_message)(void* hwnd, unsigned msg,
                                      uintptr_t wp, intptr_t lp);

// Asynchronous message post
extern int  (*g_wxhook_post_message)(void* hwnd, unsigned msg,
                                      uintptr_t wp, intptr_t lp);

// MessageBox (replaces ::MessageBox on non-Windows)
// ── the headless answer to a message box ─────────────────────────────────────
// When no hook is installed there is no user to ask, so answer the LEAST
// DESTRUCTIVE button of whichever set was requested: decline, never proceed.
//
// Inline and shared because there are two UMessageBox definitions -- one in
// platform/windows_stubs.cpp for the headless test builds and one in
// platform/mushapp_stubs.cpp for mushgui -- and they are in the two adapter
// sets that never co-link, so they cannot be merged into one function.  They
// CAN share the decision, which is the part that was wrong in both.
//
// Both used to say `(nType & MB_YESNO) ? IDNO : IDOK`, treating the button set
// as a bit field.  It is the LOW NIBBLE and an ENUMERATION: MB_YESNOCANCEL is
// 3 and MB_YESNO is 4, so `3 & 4 == 0` answered a three-way "save your changes
// before closing?" with IDOK -- and a caller testing for IDYES and IDCANCEL
// falls straight through both, closing the world and discarding the work.  It
// is the same mis-reading that hook_message_box's own comment records having
// been fixed there.
// This header is deliberately free of the MFC/Win32 shim (it is the hook table
// every side includes), so the ID* values are written out rather than named.
// The two callers static_assert them against the real constants, so the numbers
// here cannot drift from mfc_ui.h without a compile error.
inline int HeadlessMessageBoxAnswer(unsigned int nType)
{
    switch (nType & 0x0F) {   // the button set: an enumeration, not bit flags
        case 1:  return 2;    // MB_OKCANCEL         -> IDCANCEL
        case 2:  return 3;    // MB_ABORTRETRYIGNORE -> IDABORT
        case 3:  return 2;    // MB_YESNOCANCEL      -> IDCANCEL
        case 4:  return 7;    // MB_YESNO            -> IDNO
        case 5:  return 2;    // MB_RETRYCANCEL      -> IDCANCEL
        default: return 1;    // MB_OK               -> IDOK (nothing to decline)
    }
}

extern int  (*g_wxhook_message_box)(const char* text,
                                     const char* caption,
                                     unsigned    flags);

// CMDIFrameWnd::LoadFrame — creates a wxMDIParentFrame, stores ptr in m_hWnd.
// Returns TRUE on success.
extern int  (*g_wxhook_load_mdi_frame)(void* self, unsigned id,
                                        unsigned long style,
                                        void* parentHwnd);

// ── InitInstance bootstrap ─────────────────────────────────────────────────
// Declared here; defined in wx_mfc_impl.cpp.
// wx_app.cpp calls this to boot the real MUSHclient app.
extern int  (*g_wxhook_init_instance)();

// ── New-world hook ─────────────────────────────────────────────────────────
// Called from CMultiDocTemplate::OpenDocumentFile(NULL) on non-Windows.
// Creates a CMUSHclientDoc, shows a Quick Connect dialog, creates the child
// MDI frame with output pane, and calls OnNewDocument().
// Returns the new CMUSHclientDoc* cast to void*, or nullptr on failure/cancel.
extern void* (*g_wxhook_new_world)();
// Show/raise the Activity window.  CMultiDocTemplate::OpenDocumentFile uses
// this for the IDR_ACTIVITYTYPE template (mfc_ui.h); null in headless builds.
extern void  (*g_wxhook_show_activity_window)();

// ── Open-world-file hook ───────────────────────────────────────────────────
// Called from CMultiDocTemplate::OpenDocumentFile(path) on non-Windows.
// Parses the world XML file at 'path' and creates the child MDI frame.  It does
// NOT connect: OnOpenDocument -> OpenSession already applies the canonical
// App.m_bAutoConnectWorlds rule (doc.cpp:500).
// Returns the new CMUSHclientDoc* cast to void*, or nullptr on failure.
extern void* (*g_wxhook_open_world_file)(const char* path);
// Open the bundled default world (Discworld edition) if no world is open yet.
// No-op for a normal build (no Contents/Resources/default-world marker).
extern void  (*g_wxhook_open_default_world)();
// macOS quit: prompt to save modified worlds + persist plugin layout before the
// process terminates.  Returns 1 to allow the quit, 0 to cancel it.
extern int   (*g_wxhook_query_quit_save)();
// MFC-parity top-level exception net (CWinApp::ProcessWndProcException).
// Called from wxApp::OnExceptionInMainLoop's implicit catch block: rethrows the
// in-flight exception, and if it is an MFC CException* (e.g. ThrowErrorException
// from a bad regexp in the Find/Recall dialogs), reports it in a message box,
// frees it, and returns 1 (keep running — Windows behaviour).  Any other
// exception type propagates back out (unchanged wx termination behaviour).
extern int   (*g_wxhook_report_mfc_exception)();

// (Quick Connect is no longer a hook: the canonical CQuickConnectDlg renders via
// the MFC-dialog bridge from IDD_QUICK_CONNECT — see dialogs/QuickConnectDlg.cpp.)

// ── Status bar ────────────────────────────────────────────────────────────
// Set the text shown in the MDI parent frame's status bar.
// msg is UTF-8; passing nullptr clears the bar.  (Transient global messages; the
// per-world status line lives on each CWorldChildFrame's own status bar.)
extern void (*g_wxhook_set_status)(const char* msg);

// (World preferences are no longer a hook: the canonical CMUSHclientDoc::
// GamePreferences runs on the port and shows the real CTreePropertySheet as a
// native wxTreebook — see g_run_mfc_property_sheet / hook_run_property_sheet.)

// ── Save world file ────────────────────────────────────────────────────────
// Saves the world file for a specific doc (CMUSHclientDoc*).
// If saveAs is true or the doc has no path, shows a save-file dialog.
// Returns true if the file was saved, false if the user cancelled.
// The parent window (wxWindow*) is used as the dialog parent.
extern bool (*g_wxhook_save_world)(void* doc, bool saveAs, void* parent);

// ── Generic file dialog hook ───────────────────────────────────────────────
// Called from CFileDialog::DoModal() on non-Windows.
// bOpen:       true = open dialog (file must exist), false = save dialog
// title:       dialog title (UTF-8), or nullptr for default
// defaultDir:  initial directory (UTF-8), or nullptr
// defaultFile: suggested filename without path (UTF-8), or nullptr
// filter:      MFC-style filter string "Desc|*.ext|Desc2|*.ext2||" (| separators, || end)
// overwrite:   if true (OFN_OVERWRITEPROMPT), confirm before overwriting
// multiSelect: if true (OFN_ALLOWMULTISELECT, open panels only), let the user
//              pick more than one file
// Returns a heap-allocated (malloc) result the caller must free() as ONE block,
// or nullptr if the user cancelled.  Everything is UTF-8.  The shape of the
// block depends on what was asked for:
//   multiSelect false — a plain NUL-terminated path, exactly as before.
//   multiSelect true  — the full paths NUL-separated and the block closed by a
//                       further NUL ("a\0b\0\0"), the shape Windows' own
//                       OPENFILENAME lpstrFile uses.  An empty block ("\0")
//                       means nothing was selected.
// The caller only parses the second shape when it asked for it, so a hook that
// answers single-select with strdup() is never read past its allocation.
extern char* (*g_wxhook_file_dialog)(bool bOpen,
                                     const char* title,
                                     const char* defaultDir,
                                     const char* defaultFile,
                                     const char* filter,
                                     bool overwrite,
                                     bool multiSelect);

// ── Font chooser hook (CFontDialog → wxFontDialog) ─────────────────────────
// Exchange struct (no wx/MFC types so mfc_ui.h can use it).  On input:
// faceName + lfHeight (MFC LOGFONT height, negative device units) + weight
// (FW_NORMAL=400 / FW_BOLD=700) + italic + color (the COLORREF the colour
// picker opens on, from CHOOSEFONT::rgbColors) seed the dialog.  On OK
// (return 1): faceName, pointSize (whole points), weight,
// italic/underline/strikeout, and color are filled with the chosen font.
// Returns 0 if the user cancels.
struct WxFontChoice {
    char     faceName[64];
    long     lfHeight;     // in:  LOGFONT height (typically negative)
    int      pointSize;    // out: chosen size in whole points
    int      weight;       // in/out: FW_NORMAL=400 / FW_BOLD=700
    int      italic;       // in/out: 0 or 1
    int      underline;    // out: 0 or 1
    int      strikeout;    // out: 0 or 1
    uint32_t color;        // in/out: COLORREF the picker opens on / was chosen
};
extern int (*g_wxhook_font_dialog)(void* parentHwnd, struct WxFontChoice* io);

// ── Clipboard hooks ────────────────────────────────────────────────────────
// Get the system clipboard text as a UTF-8 malloc'd string (caller must free),
// or nullptr if the clipboard is empty or contains no text.
extern char* (*g_wxhook_get_clipboard_text)();

// Set the system clipboard to the given UTF-8 string.
extern void  (*g_wxhook_set_clipboard_text)(const char* text);

// True if the clipboard holds content in one of the common NON-text formats
// (an image, a PDF, a file reference, rich text).  Setting the clipboard
// replaces the whole pasteboard, and content this seam cannot read as text also
// cannot be put back afterwards, so a caller about to overwrite the user's
// clipboard can ask first.  --gui-bridge-selftest is the only caller: it skips
// its round trip rather than destroy a copied image on an ordinary ctest run.
// Best effort — wxClipboard answers about named formats but cannot enumerate,
// so a format private to another application is invisible here.
extern bool  (*g_wxhook_clipboard_has_nontext)();

// ── Sound hooks ───────────────────────────────────────────────────────────
// Play a sound file asynchronously.  path is a UTF-8 filesystem path.
// Returns true if playback started successfully, false on error or if no
// sound backend is available.  Used by MSP / trigger sounds and the
// world-properties "test" button (auto-buffer, full volume, centred).
extern bool (*g_wxhook_play_sound)(const char* path, bool loop);
// Stop ALL sounds currently playing via the sound engine (CancelSound).
extern void (*g_wxhook_stop_sound)();

// Buffered sound API (world.PlaySound): up to MAX_SOUND_BUFFERS concurrent
// voices with per-voice volume + pan + looping, mixed by the audio engine.
// buffer is 1..N, or 0 to auto-select a free slot.  volume is a percent in
// [-100,0] (0 = full); pan is a percent in [-100,+100].  Backed by miniaudio
// (platform/ma_sound.*).  Null in mushcore/CLI/tests → no audio.
//   play   → 1-based buffer used, or 0 on failure
//   adjust → 1 if an already-playing buffer was retuned, else 0
//   stop   → buffer 1..N, or 0 for all
//   status → -2 free, 0 not playing, 1 playing, 2 playing+looping
// `owner` is the CMUSHclientDoc* whose buffers these are: on Windows the ten
// LPDIRECTSOUNDBUFFERs are a doc member, so the ten are PER WORLD, and one
// world's StopSound(0) must not silence another's.  release_owner is the doc
// destructor's counterpart to Windows releasing its buffers.
extern int  (*g_wxhook_sound_play)(const void* owner, int buffer, const char* path,
                                   int loop, double volume, double pan);
extern int  (*g_wxhook_sound_play_memory)(const void* owner, int buffer,
                                          const void* data, size_t len,
                                          int loop, double volume, double pan);
extern int  (*g_wxhook_sound_adjust)(const void* owner, int buffer, int loop,
                                     double volume, double pan);
extern void (*g_wxhook_sound_stop_buffer)(const void* owner, int buffer);
extern int  (*g_wxhook_sound_status)(const void* owner, int buffer);
extern void (*g_wxhook_sound_release_owner)(const void* owner);

// ── Bundled "TEXT" resources ──────────────────────────────────────────────
// Win32 FindResource/LoadResource/LockResource for RT-"TEXT" int-resources map
// to bundled files via this hook (the licence + credits text shown by the About
// box's CCreditsDlg).  Returns a pointer to the cached text (NUL-terminated) and
// its length via *size, or null if the id isn't a bundled text resource.  The
// returned pointer stays valid for the process lifetime (cached).
extern const char* (*g_wxhook_load_text_resource)(int id, unsigned* size);

// ── Fonts ─────────────────────────────────────────────────────────────────
// utils.getfontfamilies(): a NULL-terminated array of installed font family
// names (each string and the array itself are heap — caller free()s them).
extern char** (*g_wxhook_get_font_families)();
// utils.glyph_available(face, codepoint): 1 if the named font has a glyph for
// the Unicode code point, else 0.  macOS uses CoreText; 0 where unsupported.
extern int    (*g_wxhook_font_has_glyph)(const char* facename, unsigned int codepoint);

// ── System queries (world.GetSysColor / GetSystemMetrics / GetPixel) ────────
// GetSysColor(COLOR_*): map a Win32 system-colour index to the live desktop
// theme colour (wxSystemSettings).  Returns 0xC0C0C0 from the stub when unset.
extern unsigned long (*g_wxhook_get_sys_color)(int index);
// GetSystemMetrics(SM_*): map a Win32 metric index to the real value (screen
// size, icon/scrollbar dimensions, …).  Returns 0 from the stub when unset.
extern int           (*g_wxhook_get_system_metric)(int index);
// ::GetPixel(HDC,x,y): read a pixel from the wx device context that owns hdc —
// for loaded images this reads the materialised DIBSection bits, so colour-key
// transparency keys on the real top-left pixel rather than black.
extern unsigned long (*g_wxhook_get_pixel)(void* hdc, int x, int y);
// GetKeyState(VK_*): live keyboard-modifier / mouse-button state for world.GetInfo
// (294).  High bit (0x8000) set = key/button down.  Returns 0 from the stub when
// unset, and for keys wx cannot report (L/R-specific modifiers, lock toggles).
extern short         (*g_wxhook_get_key_state)(int vk);
// CEdit programmatic selection / line / insertion ops (hwnd is the wxTextCtrl*).
// GetWindowText/SetWindowText already work; these add the helpers the canonical
// dialogs use (Go-To-Line, insert-at-caret, Find-enable on multiline edits).
extern void (*g_wxhook_edit_get_sel)(void* hwnd, int* start, int* end);
extern void (*g_wxhook_edit_set_sel)(void* hwnd, int start, int end);   // s<0 deselect; e<0 → end
extern void (*g_wxhook_edit_replace_sel)(void* hwnd, const char* text); // replace selection / insert at caret
// op: 0=line count, 1=char index of line `arg` (-1=current), 2=length of line
// holding char `arg` (-1=current), 3=line of char `arg` (-1=current), 4=text length.
extern int  (*g_wxhook_edit_line_info)(void* hwnd, int op, int arg);
// CEdit::LimitText / CEdit::SetReadOnly — EM_LIMITTEXT and ES_READONLY.  These
// carry the `max_length` and `read_only` options of utils.inputbox/editbox, both
// of which were silently dropped while the shim methods were `{}`.
extern void (*g_wxhook_edit_set_max_length)(void* hwnd, int chars);  // 0 = no limit
extern void (*g_wxhook_edit_set_readonly)(void* hwnd, bool readOnly);
// CWnd::SetFont — `font` is the CFont's wxFont* (CGdiObject::m_impl).  The wx
// side copies it, so the caller may destroy the CFont afterwards.
extern void (*g_wxhook_set_font)(void* hwnd, void* font);
// world.SetCursor(n): set the mouse cursor over the doc's output pane.  n is the
// MUSHclient cursor index (-1 = hidden, 0 = arrow, 1 = hand, …, 12 = help).
extern void (*g_wxhook_set_cursor)(void* doc, int cursorNum);
// utils.setbackgroundcolour / world.SetFrameBackgroundColour: set the MDI parent
// frame's backdrop colour (behind world windows).  colour is a COLORREF
// (0x00BBGGRR); 0xFFFFFFFF resets to the system default.
extern void (*g_wxhook_set_main_background)(unsigned long colour);

// ── Notepad hook ───────────────────────────────────────────────────────────
// Create a notepad window.  All string parameters are UTF-8.
// sText           — initial content (may be "")
// sTitle          — display title
// pDoc            — owning CMUSHclientDoc (may be nullptr)
// iUniqueDocNum   — m_iUniqueDocumentNumber of pDoc
// strFontName     — font face name
// iFontSize       — point size
// iFontWeight     — FW_NORMAL (400) or FW_BOLD (700)
// iFontCharset    — Windows charset value (ignored on non-Windows)
// textColour      — COLORREF foreground
// backColour      — COLORREF background
// strSearchString — search/recall string stored on the doc
// strRecallLinePreamble — recall preamble stored on the doc
// bMatchCase, bRegexp, bCommands, bOutput, bNotes — recall flags
// iLines          — recall line count (stored; 0 = not a recall)
// iNotepadType    — eNotepad* enum value
// Returns a heap-allocated CTextDocument* (caller must not free directly;
// the frame owns the doc lifetime).  Returns nullptr on failure.
class CMUSHclientDoc;
// Open an existing text file into a notepad window and return the
// CTextDocument* (as void*), or nullptr if it cannot be read.  Backs
// CWinApp::OpenDocumentFile for non-world files — the "edit script with
// notepad" paths (world_debug.cpp) and the Plugins dialog's Edit button, which
// cast the result to CTextDocument*.  Distinct from create_text_window below,
// which builds a notepad from CONTENT and has no file on disk behind it.
extern void* (*g_wxhook_open_text_file)(const char* path);

extern void* (*g_wxhook_create_text_window)(
    const char* sText,
    const char* sTitle,
    CMUSHclientDoc* pDoc,
    long long iUniqueDocNum,
    const char* strFontName,
    long  iFontSize,
    long  iFontWeight,
    unsigned int iFontCharset,
    unsigned long textColour,
    unsigned long backColour,
    const char* strSearchString,
    const char* strRecallLinePreamble,
    bool bMatchCase,
    bool bRegexp,
    bool bCommands,
    bool bOutput,
    bool bNotes,
    int  iLines,
    int  iNotepadType);

// ── Notepad hooks: REMOVED ────────────────────────────────────────────────
// Eleven g_wxhook_*_notepad declarations stood here.  Nine had exactly one
// caller (the headless stub file) and one installer (wx_mfc_impl.cpp), in the
// two adapter sets, which never co-link — so they were nullptr wherever they
// were called and uncalled wherever they were installed.  The notepad family is
// adopted into mushcore now (docs/STUB-ADOPTION-2026-08.md) and both builds go
// through the real bodies, which reach the window via CTextDocument's m_fn_*
// callbacks.  create_text_window (above) and open_text_file stay — those are
// live seams with real callers in both builds.

// ── Colour picker hook ────────────────────────────────────────────────────
// Open a colour-picker dialog.  suggested is a COLORREF (0x00BBGGRR), or
// -1 to start with no pre-selected colour.  Returns the picked COLORREF, or
// -1 if the user cancelled.
extern long (*g_wxhook_pick_colour)(long suggested);

// ── Menu / choice hook ─────────────────────────────────────────────────────
// Show a single-choice dialog populated with the null-terminated strings in
// items[0..count-1].  defaultItem is the item to pre-select (may be "").
// Returns the chosen string as a heap-allocated char* (caller must free()),
// or nullptr if the user cancelled.
extern char* (*g_wxhook_menu_dialog)(const char* const* items, int count,
                                     const char* defaultItem);

// ── Flash icon / request user attention ───────────────────────────────────
// Flashes the main application window's taskbar button to alert the user.
// Equivalent to the Windows FlashWindow(TRUE) call.
extern void (*g_wxhook_flash_icon)();

// ── Move main window ──────────────────────────────────────────────────────
// Repositions and/or resizes the MDI parent frame.  Coordinates are screen
// pixels (same convention as wxWindow::SetSize).
extern void (*g_wxhook_move_main_window)(long left, long top, long width, long height);

// ── utils.inputbox / utils.editbox hook ──────────────────────────────────
// Shows a modal text-entry dialog.
//   title:     window title
//   prompt:    label text shown above the entry field
//   defValue:  initial content of the entry field
//   multiline: true for a multi-line edit area, false for single-line
// Returns a heap-allocated string that the caller must free(), or nullptr if
// the user cancelled.
extern char* (*g_wxhook_inputbox)(const char* title,
                                  const char* prompt,
                                  const char* defValue,
                                  bool        multiline);

// ── password dialog hook ──────────────────────────────────────────────────
// Shows a modal masked-input dialog for a MUD login password.
//   charName:    character/login name shown in the prompt (may be "").
//   outPassword: receives a heap-allocated password string; caller must
//                free() it.  Set only when IDOK is returned.
// Returns IDOK (1) if the user entered a password and confirmed,
//         IDCANCEL (2) if the user cancelled or left the field empty.
extern int (*g_wxhook_password_dialog)(const char* charName,
                                       char**      outPassword);

// ── utils.directorypicker hook ────────────────────────────────────────────
// Shows a modal directory-chooser dialog.
//   title:   window title / message
//   defPath: directory to pre-select (may be "" for cwd)
// Returns a heap-allocated path string that the caller must free(), or
// nullptr if the user cancelled.
// ── Progress dialog (CProgressDlg, port only) ─────────────────────────────
// The canonical CProgressDlg is a MODELESS MFC dialog and the bridge builds
// only modal ones, so it never had a window here.  These back it with a
// wxProgressDialog, which supplies the bar, the status line, the Cancel button
// AND the event-loop pumping that keeps the app responsive during a long send.
// _update returns false once the user has cancelled.
extern void* (*g_wxhook_progress_open)(const char* caption,
                                       const char* status,
                                       int maximum);
extern void  (*g_wxhook_progress_set_range)(void* handle, int maximum);
extern bool  (*g_wxhook_progress_update)(void* handle, int pos,
                                         const char* status);
extern void  (*g_wxhook_progress_close)(void* handle);
// Retitle an open progress dialog.  The callers all do Create() and THEN
// SetWindowText("Sending..." / "Recalling..." / ...), so without this every one
// of them read a generic "Progress".  (The "Loading plugins ..." caller this
// comment used to cite is inside #if IT_GET_GOATS_BLOOD_OUT and is dead on
// every platform, Windows included.)
extern void  (*g_wxhook_progress_set_title)(void* handle, const char* title);

extern char* (*g_wxhook_directorypicker)(const char* title,
                                         const char* defPath);

// ── WindowMenu popup hook ─────────────────────────────────────────────────
// Shows a plugin's miniwindow popup menu (built by CMiniWindow::Menu into the
// portable CMenu tree from platform/mfc_ui.h) at screen pixel (x, y), and
// returns the selected command id, or 0 if the user cancelled.  `flags` are the
// Win32 TPM_* bits: the alignment half says which corner/edge of the menu the
// anchor names (see TpmAlignAnchor in platform/mfc_ui.h).
class CMenu;
extern int (*g_wxhook_popup_menu)(CMenu* menu, int x, int y, unsigned flags);

// Convert a point in a world's OUTPUT PANE client coordinates to screen
// coordinates (`doc` is a CMUSHclientDoc*).  Off-Windows there is no CMUSHView
// for CMiniWindow::Menu to convert its anchor through, so it asks the frontend
// for the pane that is standing in for one.  Leaves the point alone if the doc
// has no window (headless, or mid-teardown).
extern void (*g_wxhook_output_client_to_screen)(void* doc, int* x, int* y);

// ── MFC dialog bridge hooks ───────────────────────────────────────────────
// g_run_mfc_dialog: CDialog::DoModal calls this with `self` = the CDialog* and
// `idd` = its template id.  The bridge builds a wxDialog from the recovered
// template, sets self->m_hWnd, runs OnInitDialog, shows it modally, dispatches
// OK/Cancel (and, later, control events) to the canonical handlers, and returns
// the result (IDOK/IDCANCEL/...).  nullptr in non-GUI builds -> DoModal returns
// IDCANCEL (the prior stub behaviour).
extern intptr_t (*g_run_mfc_dialog)(void* self, unsigned idd);
// g_end_mfc_dialog: CDialog::EndDialog ends the modal loop of the wxDialog
// backing `self`, with the given result.
extern void (*g_end_mfc_dialog)(void* self, int result);
// g_run_mfc_property_sheet: CTreePropertySheet::DoModal calls this with `self` =
// the CTreePropertySheet* (port-only lightweight class).  The bridge builds a
// wxDialog hosting a wxTreebook — one page per recorded CPropertyPage, branch/
// node/endbranch -> AddPage/AddSubPage — populates each page panel from its
// recovered template, runs OnInitDialog (DDX load) + control dispatch, selects
// the wanted page, and on OK runs each page's UpdateData(TRUE).  Returns
// IDOK/IDCANCEL.  nullptr -> IDCANCEL.
extern intptr_t (*g_run_mfc_property_sheet)(void* self);
// g_run_mfc_property_sheet_flat: base CPropertySheet::DoModal calls this with
// `self` = the CPropertySheet* (e.g. CGlobalPrefsSheet — File ▸ Preferences).
// Same as above but builds a flat wxNotebook (tabs) from the base m_flatPages
// list (no tree structure); tab labels come from each page template's caption.
// Returns IDOK/IDCANCEL.  nullptr -> IDCANCEL.
extern intptr_t (*g_run_mfc_property_sheet_flat)(void* self);

// ── Dialog control accessors (GetDlgItem / DDX) ───────────────────────────
// g_wxhook_get_dlg_item: the wxWindow* of `parent`'s child with control id `id`
// (FindWindowById), or nullptr.  CWnd::GetDlgItem wraps it via FromHandle.
extern void* (*g_wxhook_get_dlg_item)(void* parent, int id);

// g_wxhook_subclass_dlg_item: called by CWnd::SubclassDlgItem so the wx adapter
// can register the binding and, for control classes the port cannot drive by
// message reflection, SUBSTITUTE a different widget.  Returns the wxWindow* the
// CWnd should adopt as m_hWnd, or null to keep the child that is already there.
//
// The substitution exists because the port has no WM_ dispatch path at all:
// ON_WM_LBUTTONDOWN / ON_WM_SETCURSOR / ON_WM_NCHITTEST / ON_WM_CTLCOLOR_REFLECT
// are empty macros (mfc_ui.h), so a subclass's BEGIN_MESSAGE_MAP for those
// expands to nothing and AfxDispatchCommand/Notify only ever match WM_COMMAND /
// WM_NOTIFY.  Message delivery therefore has to be solved per control class.
extern void* (*g_wxhook_subclass_dlg_item)(void* parent, int id, void* pWnd);
// g_wxhook_get_window_text: the control's text, heap-allocated (caller free()s).
extern char* (*g_wxhook_get_window_text)(void* hwnd);
// Checkbox/radio state (BST_CHECKED=1 / BST_UNCHECKED=0 / BST_INDETERMINATE=2).
extern void (*g_wxhook_button_set_check)(void* hwnd, int state);
extern int  (*g_wxhook_button_get_check)(void* hwnd);
// Enable/disable a control window.
extern void (*g_wxhook_enable_window)(void* hwnd, int enable);

// ── CListCtrl → wxListCtrl (entity-manager object lists) ──────────────────
// `hwnd` is the wxListCtrl* (a CListCtrl member's m_hWnd, bound by DDX_Control).
// Report-mode list ops mirroring the CListCtrl methods the canonical
// CGenPropertyPage list pages use.  Item "data" is the canonical CString* the
// page stores per row (object name); we keep it in the row's item data.
extern void (*g_wxhook_listctrl_insert_column)(void* hwnd, int col, const char* heading, int fmt, int width);
extern int  (*g_wxhook_listctrl_insert_item)(void* hwnd, int item, const char* text);
extern void (*g_wxhook_listctrl_set_item_text)(void* hwnd, int item, int col, const char* text);
extern int  (*g_wxhook_listctrl_get_item_count)(void* hwnd);
extern void (*g_wxhook_listctrl_set_item_data)(void* hwnd, int item, void* data);
extern void* (*g_wxhook_listctrl_get_item_data)(void* hwnd, int item);
// Row/column label; caller free()s.  NULL when there is no control behind the
// handle, which is what an unattached CListCtrl has.
extern char* (*g_wxhook_listctrl_get_item_text)(void* hwnd, int item, int subitem);
extern int  (*g_wxhook_listctrl_get_next_item)(void* hwnd, int start, int flags);
extern void (*g_wxhook_listctrl_set_item_state)(void* hwnd, int item, unsigned state, unsigned mask);
extern unsigned (*g_wxhook_listctrl_get_item_state)(void* hwnd, int item, unsigned mask);
extern int  (*g_wxhook_listctrl_get_selected_count)(void* hwnd);
extern void (*g_wxhook_listctrl_delete_item)(void* hwnd, int item);
extern void (*g_wxhook_listctrl_delete_all_items)(void* hwnd);
extern void (*g_wxhook_listctrl_ensure_visible)(void* hwnd, int item);
extern void (*g_wxhook_listctrl_set_column_width)(void* hwnd, int col, int width);
// Read side of the pair.  Needed because CMUSHclientApp::SaveColumnConfiguration
// persists GetColumnWidth() -- without this it stored 0 for every column, and
// RestoreColumnConfiguration turns a stored 0 into a 10-pixel column.
extern int  (*g_wxhook_listctrl_get_column_width)(void* hwnd, int col);
// SortItems: `cmp(lp1, lp2, lparam)` compares two rows' item data; we sort the
// rows and reorder them, preserving per-row text + item data.
extern void (*g_wxhook_listctrl_sort_items)(void* hwnd,
        int (*cmp)(intptr_t, intptr_t, intptr_t), intptr_t lparam);

// ── CListBox → wxListBox (e.g. the spell-check suggestion list) ───────────────
// `hwnd` is the wxListBox* (a CListBox member's m_hWnd, bound by DDX_Control).
// GetCurSel returns LB_ERR (-1) when there's no selection (Win32 convention).
extern int   (*g_wxhook_listbox_add_string)(void* hwnd, const char* text);
extern void  (*g_wxhook_listbox_reset_content)(void* hwnd);
extern int   (*g_wxhook_listbox_get_count)(void* hwnd);
extern int   (*g_wxhook_listbox_get_cur_sel)(void* hwnd);
extern int   (*g_wxhook_listbox_set_cur_sel)(void* hwnd, int sel);
extern char* (*g_wxhook_listbox_get_text)(void* hwnd, int index);   // heap; caller free()s
// Mutators + search (2026-08-12): these three had NO hooks — hard LB_ERR stubs
// — so Global Prefs' Remove/Move Up/Move Down did nothing and its duplicate
// checks always answered "not found".  Win32 semantics: delete returns the
// remaining count (LB_ERR for a bad index), insert returns the inserted index
// (index -1 = append), find is a case-insensitive exact match starting after
// `after`, wrapping, LB_ERR when absent.
// Status text WITH doc attribution (2026-08-12): a background world's
// "Connecting to X" used to land on the ACTIVE world's status bar (the
// plain set_status hook resolves purely by GetActiveChild).  This variant
// delivers to the producing doc's own frame; unknown doc falls back to the
// active child (app-scoped messages keep their old route).
extern void  (*g_wxhook_set_status_for)(void* doc, const char* msg);

extern int   (*g_wxhook_listbox_delete_string)(void* hwnd, int index);
extern int   (*g_wxhook_listbox_insert_string)(void* hwnd, int index, const char* text);
extern int   (*g_wxhook_listbox_find_string_exact)(void* hwnd, int after, const char* text);
// Per-item data + multi-selection (Send-to-All world list, Choose-Notepad).
extern void     (*g_wxhook_listbox_set_item_data)(void* hwnd, int index, intptr_t data);
extern intptr_t (*g_wxhook_listbox_get_item_data)(void* hwnd, int index);
extern int      (*g_wxhook_listbox_get_sel_count)(void* hwnd);
extern int      (*g_wxhook_listbox_get_sel_items)(void* hwnd, int maxItems, int* buffer);
extern int      (*g_wxhook_listbox_get_sel)(void* hwnd, int index);          // >0 if selected
extern int      (*g_wxhook_listbox_set_sel)(void* hwnd, int index, int select);

// ── CComboBox → wxComboBox (drop-down selectors: proxy type, colour combos…) ─
// `hwnd` is the wxComboBox* (a CComboBox member's m_hWnd, bound by DDX_Control).
// Return values match the Win32 CB_* conventions the canonical code expects:
// GetCurSel/FindStringExact return -1 (CB_ERR) when there's no match/selection.
extern int  (*g_wxhook_combo_add_string)(void* hwnd, const char* text);
extern int  (*g_wxhook_combo_get_count)(void* hwnd);
extern int  (*g_wxhook_combo_get_cur_sel)(void* hwnd);
extern void (*g_wxhook_combo_set_cur_sel)(void* hwnd, int sel);
extern int  (*g_wxhook_combo_find_string_exact)(void* hwnd, int after, const char* text);
extern char* (*g_wxhook_combo_get_lb_text)(void* hwnd, int index);  // heap; caller free()s
extern void (*g_wxhook_combo_reset_content)(void* hwnd);
// Per-item payload.  CComboBox item data is not decoration: CLuaChooseBox binds
// the caller's table index to each entry with it and reads it back in OnOK, so
// utils.choose returns the wrong key without these (the combo is sorted, and
// the position is not the index).
extern void     (*g_wxhook_combo_set_item_data)(void* hwnd, int index, intptr_t data);
extern intptr_t (*g_wxhook_combo_get_item_data)(void* hwnd, int index);

// ── Radio-button group (DDX_Radio) ────────────────────────────────────────
// `dlg` is the dialog/page wxWindow; `firstId` is the group's first radio (the
// one carrying WS_GROUP / wxRB_GROUP).  The group is the run of wxRadioButtons
// from firstId up to (not incl.) the next wxRB_GROUP — matching MFC's DDX_Radio,
// which walks consecutive controls until a new WS_GROUP.  get returns the 0-based
// index of the checked member (-1 if none); set checks the index'th member.
extern int  (*g_wxhook_radio_get)(void* dlg, int firstId);
extern void (*g_wxhook_radio_set)(void* dlg, int firstId, int index);

// ── Owner-draw buttons (BS_OWNERDRAW, e.g. CColourButton swatches) ─────────
// make_owner_draw_button: build a self-painting wx control for a BS_OWNERDRAW
// button; its paint handler runs the canonical CButton::DrawItem.  Returns the
// wxWindow* (as void*).  Defined in wx_mfc_impl.cpp (needs OwnerDrawButton).
extern void* (*g_wxhook_make_owner_draw_button)(void* parent, int id,
                                                const char* text,
                                                int x, int y, int w, int h);
// bind_owner_draw: associate the control's wxWindow (hwnd) with the canonical
// CWnd* that backs it (set by DDX_Control), so its paint handler can call that
// control's virtual DrawItem.  `ctl` is a CWnd* (as void*); the hook works out
// from the wx widget whether it is really a CButton (owner-draw button) or a
// CComboBox (owner-draw combo), and no-ops for every other control.
extern void (*g_wxhook_bind_owner_draw)(void* hwnd, void* ctl);

// ── Owner-draw combos (CBS_OWNERDRAWFIXED, e.g. CColourComboBox swatches) ──
// make_owner_draw_combo: build a self-painting drop-down for an owner-draw
// combo template; its item-paint callback runs the canonical CComboBox::DrawItem
// (CColourComboBox paints each row in that custom colour instead of naming it).
// `readOnly` is CBS_DROPDOWNLIST — no editable text field.  Returns the
// wxWindow* (as void*).  Defined in wx_mfc_impl.cpp (needs OwnerDrawCombo).
//
// The result is a wxOwnerDrawnComboBox, NOT a wxChoice/wxComboBox, so anything
// that reaches a combo by wxDynamicCast must know about it — see CB() in
// wx_mfc_impl.cpp, which is what keeps DDX_CBIndex round-tripping.
extern void* (*g_wxhook_make_owner_draw_combo)(void* parent, int id,
                                               int x, int y, int w,
                                               int readOnly);

// ── CSliderCtrl → wxSlider (msctls_trackbar32, e.g. ColourPickerDlg RGB) ────
// `hwnd` is the wxSlider* (a CSliderCtrl member's m_hWnd, bound by DDX_Control).
extern void (*g_wxhook_slider_set_range)(void* hwnd, int lo, int hi);
extern int  (*g_wxhook_slider_get_pos)(void* hwnd);
extern void (*g_wxhook_slider_set_pos)(void* hwnd, int pos);
extern void (*g_wxhook_slider_set_page_size)(void* hwnd, int n);
// bind_slider: when a slider moves it must drive the canonical CWnd::OnHScroll
// (the ON_WM_HSCROLL handler that recomputes the colour).  `dlg` is the owning
// CDialog* (as void*), so the slider-event handler can call dlg->OnHScroll.
extern void (*g_wxhook_bind_slider)(void* hwnd, void* dlg);

// ── CSpinButtonCtrl → wxSpinButton (msctls_updown32, e.g. margins / lines-per-
// page / timer hour & minute fields).  `hwnd` is the wxSpinButton* (the result
// of GetDlgItem(IDC_SPIN_*) cast to CSpinButtonCtrl*).  The spin nudges its
// "buddy" edit (wired by bind_spin) within [lo,hi]; the buddy holds the value.
// make_spin builds the control (defined in wx_mfc_impl.cpp, needs WxSpinButton).
extern void* (*g_wxhook_make_spin)(void* parent, int id, int x, int y, int w, int h);
extern void (*g_wxhook_spin_set_range)(void* hwnd, int lo, int hi);
extern void (*g_wxhook_spin_set_pos)(void* hwnd, int pos);
extern int  (*g_wxhook_spin_get_pos)(void* hwnd);
// bind_spin: associate a spin with its buddy edit (the preceding control, per
// UDS_AUTOBUDDY) and wire its up/down arrows to step the buddy's integer value.
extern void (*g_wxhook_bind_spin)(void* spin, void* buddyEdit);

// ── CHotKeyCtrl → key-capture control (msctls_hotkey32, the Key Name dialog) ──
// make_hotkey builds a control that captures a key + modifiers, shows the chord
// (via the canonical KeyCodeToString), and stores the Windows VK / HOTKEYF_* for
// hotkey_get.  Returns the wxWindow* (as void*).  Defined in wx_mfc_impl.cpp.
extern void* (*g_wxhook_make_hotkey)(void* parent, int id, int x, int y, int w, int h);
// hotkey_get: read the captured key — *vk = Windows virtual-key, *mod = HOTKEYF_*.
extern void  (*g_wxhook_hotkey_get)(void* hwnd, unsigned short* vk, unsigned short* mod);

// ── CWnd geometry (screen/client rects + point conversion); `hwnd` is wxWindow*.
// get_client_rect (declared far above) gives the {0,0,w,h} client box; these add
// the screen-coordinate bounds and point conversions used by a few CWnd callers.
extern void (*g_wxhook_get_window_rect)(const void* hwnd, RECT* r);
// get_placement_rect: the rectangle GetWindowPlacement reports as
// rcNormalPosition — a child window's bounds in its PARENT's client
// coordinates, a top-level window's in screen coordinates.  That is what the
// canonical relayout code reads (e.g. CEditMultiLine::OnSize sizes the text box
// to just above the Cancel button's top), so it must NOT be the {0,0,w,h}
// client box: a control's top would always be 0.
extern void (*g_wxhook_get_placement_rect)(const void* hwnd, RECT* r);
extern void (*g_wxhook_client_to_screen)(const void* hwnd, int* x, int* y);
extern void (*g_wxhook_screen_to_client)(const void* hwnd, int* x, int* y);

// ── Installer ─────────────────────────────────────────────────────────────
// Called from wx_app.cpp::OnInit() before anything else.
// View > Status Bar, app-wide (Windows has one bar on the frame; the port
// gives each world its own because the MDI parent's is hidden).  Defined in
// wx_mfc_impl.cpp; read by CWorldChildFrame's constructor.
bool wx_status_bar_visible();

void wx_mfc_install_hooks();

#endif // !_WIN32

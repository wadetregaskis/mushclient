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
extern int  (*g_wxhook_message_box)(const char* text,
                                     const char* caption,
                                     unsigned    flags);

// CMDIFrameWnd::LoadFrame — creates a wxMDIParentFrame, stores ptr in m_hWnd.
// Returns TRUE on success.
extern int  (*g_wxhook_load_mdi_frame)(void* self, unsigned id,
                                        unsigned long style,
                                        void* parentHwnd);

// Run the wxApp event loop (called after InitInstance from wx_app.cpp's OnInit)
extern int  (*g_wxhook_run_app_loop)();

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

// ── Open-world-file hook ───────────────────────────────────────────────────
// Called from CMultiDocTemplate::OpenDocumentFile(path) on non-Windows.
// Parses the world XML file at 'path', creates the child MDI frame, and
// auto-connects if the world's m_connect_now setting says to.
// Returns the new CMUSHclientDoc* cast to void*, or nullptr on failure.
extern void* (*g_wxhook_open_world_file)(const char* path);

// (Quick Connect is no longer a hook: the canonical CQuickConnectDlg renders via
// the MFC-dialog bridge from IDD_QUICK_CONNECT — see dialogs/QuickConnectDlg.cpp.)

// ── Status bar ────────────────────────────────────────────────────────────
// Set the text shown in the MDI parent frame's status bar.
// msg is UTF-8; passing nullptr clears the bar.
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
// Returns a heap-allocated (malloc) selected path (caller must free()), or
// nullptr if the user cancelled.  The path is UTF-8.
extern char* (*g_wxhook_file_dialog)(bool bOpen,
                                     const char* title,
                                     const char* defaultDir,
                                     const char* defaultFile,
                                     const char* filter,
                                     bool overwrite);

// ── Font chooser hook (CFontDialog → wxFontDialog) ─────────────────────────
// Exchange struct (no wx/MFC types so mfc_ui.h can use it).  On input:
// faceName + lfHeight (MFC LOGFONT height, negative device units) + weight
// (FW_NORMAL=400 / FW_BOLD=700) + italic seed the dialog.  On OK (return 1):
// faceName, pointSize (whole points), weight, italic/underline/strikeout, and
// color are filled with the chosen font.  Returns 0 if the user cancels.
struct WxFontChoice {
    char     faceName[64];
    long     lfHeight;     // in:  LOGFONT height (typically negative)
    int      pointSize;    // out: chosen size in whole points
    int      weight;       // in/out: FW_NORMAL=400 / FW_BOLD=700
    int      italic;       // in/out: 0 or 1
    int      underline;    // out: 0 or 1
    int      strikeout;    // out: 0 or 1
    uint32_t color;        // out: COLORREF of the chosen colour (if any)
};
extern int (*g_wxhook_font_dialog)(void* parentHwnd, struct WxFontChoice* io);

// ── Clipboard hooks ────────────────────────────────────────────────────────
// Get the system clipboard text as a UTF-8 malloc'd string (caller must free),
// or nullptr if the clipboard is empty or contains no text.
extern char* (*g_wxhook_get_clipboard_text)();

// Set the system clipboard to the given UTF-8 string.
extern void  (*g_wxhook_set_clipboard_text)(const char* text);

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
extern int  (*g_wxhook_sound_play)(int buffer, const char* path, int loop,
                                   double volume, double pan);
extern int  (*g_wxhook_sound_adjust)(int buffer, int loop,
                                     double volume, double pan);
extern void (*g_wxhook_sound_stop_buffer)(int buffer);
extern int  (*g_wxhook_sound_status)(int buffer);

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

// ── Notepad append hook ───────────────────────────────────────────────────
// Find an existing notepad window by title (case-insensitive) and append or
// replace its text.  Returns true if a matching window was found and updated,
// false if no such window exists (caller should create one instead).
extern bool (*g_wxhook_append_to_notepad)(const char* title,
                                          const char* text,
                                          bool replace);

// ── Notepad get-text hook ─────────────────────────────────────────────────
// Find an existing notepad by title (case-insensitive) and return a
// heap-allocated copy of its text content, or nullptr if not found.
// The caller is responsible for free()-ing the returned pointer.
extern char* (*g_wxhook_get_notepad_text)(const char* title);

// ── Notepad list hook ─────────────────────────────────────────────────────
// Return a null-terminated array of heap-allocated title strings for all
// currently open notepad windows.  The caller must free each string and
// the array itself.
extern char** (*g_wxhook_get_notepad_list)();

// ── Notepad activate / close / move / position hooks ────────────────────────
// All operate on a notepad identified by title (case-insensitive).
// activate: bring window to front; returns true if found.
extern bool (*g_wxhook_activate_notepad)(const char* title);
// close: close the notepad window; returns true if found.
extern bool (*g_wxhook_close_notepad)(const char* title);
// move: reposition and/or resize; returns true if found.
extern bool (*g_wxhook_move_notepad)(const char* title, int x, int y, int w, int h);
// position: "x y width height" string for an open notepad, or nullptr if not found.
// Caller must free().
extern char* (*g_wxhook_get_notepad_position)(const char* title);

// ── Notepad save hook ─────────────────────────────────────────────────────
// Save the content of a named notepad to a file.
// filename may be "" to trigger a save-as dialog.
// Returns true if the save succeeded, false if not found or cancelled.
extern bool (*g_wxhook_save_notepad)(const char* title,
                                     const char* filename,
                                     bool replaceExisting);

// ── Notepad property hooks ─────────────────────────────────────────────────
// Set foreground/background colours on a named notepad (COLORREFs: 0x00BBGGRR).
extern bool (*g_wxhook_notepad_colour)(const char* title,
                                       unsigned long fg,
                                       unsigned long bg);
// Set font on a named notepad.
// style bits: 1=bold, 2=italic, 4=underline, 8=strikeout (matches Windows API).
// size=0 → keep current; charset is ignored on non-Windows.
extern bool (*g_wxhook_notepad_font)(const char* title,
                                     const char* fontName,
                                     int size,
                                     int style,
                                     int charset);
// Set or clear read-only mode on a named notepad.
extern bool (*g_wxhook_notepad_readonly)(const char* title, bool readOnly);

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
extern char* (*g_wxhook_directorypicker)(const char* title,
                                         const char* defPath);

// ── WindowMenu popup hook ─────────────────────────────────────────────────
// Shows a plugin's miniwindow popup menu (built by CMiniWindow::Menu into the
// portable CMenu tree from platform/mfc_ui.h) at screen pixel (x, y), and
// returns the selected command id, or 0 if the user cancelled.
class CMenu;
extern int (*g_wxhook_popup_menu)(CMenu* menu, int x, int y);

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
extern int  (*g_wxhook_listctrl_get_next_item)(void* hwnd, int start, int flags);
extern void (*g_wxhook_listctrl_set_item_state)(void* hwnd, int item, unsigned state, unsigned mask);
extern unsigned (*g_wxhook_listctrl_get_item_state)(void* hwnd, int item, unsigned mask);
extern int  (*g_wxhook_listctrl_get_selected_count)(void* hwnd);
extern void (*g_wxhook_listctrl_delete_item)(void* hwnd, int item);
extern void (*g_wxhook_listctrl_delete_all_items)(void* hwnd);
extern void (*g_wxhook_listctrl_ensure_visible)(void* hwnd, int item);
extern void (*g_wxhook_listctrl_set_column_width)(void* hwnd, int col, int width);
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
// CButton* that backs it (set by DDX_Control), so its paint handler can call
// that button's virtual DrawItem.  `cbtn` is a CButton* (as void*).
extern void (*g_wxhook_bind_owner_draw)(void* hwnd, void* cbtn);

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

// ── Installer ─────────────────────────────────────────────────────────────
// Called from wx_app.cpp::OnInit() before anything else.
void wx_mfc_install_hooks();

#endif // !_WIN32

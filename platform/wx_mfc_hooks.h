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

// ── Quick Connect dialog hook ──────────────────────────────────────────────
// Shows a wxWidgets dialog to collect world name, server address, and port.
// On OK:  fills *outName / *outAddr (caller must free()) / *outPort; returns IDOK.
// On cancel: returns IDCANCEL; output pointers are unchanged.
extern int   (*g_wxhook_quick_connect_dlg)(char** outName, char** outAddr, int* outPort);

// ── Status bar ────────────────────────────────────────────────────────────
// Set the text shown in the MDI parent frame's status bar.
// msg is UTF-8; passing nullptr clears the bar.
extern void (*g_wxhook_set_status)(const char* msg);

// ── World preferences dialog ───────────────────────────────────────────────
// Shows a wxWidgets dialog for editing the world's connection settings.
// doc is a CMUSHclientDoc*; iPage is the requested tab (ignored if < 0).
// Returns true if the user clicked OK (settings were applied), false on cancel.
extern bool (*g_wxhook_game_preferences)(void* doc, int iPage);

// ── Installer ─────────────────────────────────────────────────────────────
// Called from wx_app.cpp::OnInit() before anything else.
void wx_mfc_install_hooks();

#endif // !_WIN32

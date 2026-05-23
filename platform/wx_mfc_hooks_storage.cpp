// platform/wx_mfc_hooks_storage.cpp
// Defines (owns) all wx hook function pointer globals declared extern in
// wx_mfc_hooks.h.  All pointers are initialised to nullptr here; mushgui's
// wx_mfc_impl.cpp overwrites them via wx_mfc_install_hooks() at startup.
//
// This file is compiled into BOTH mushcore and mushapp so that every link
// target that includes windows_stubs.cpp (or any other TU that calls through
// a hook) can resolve the symbols:
//   • mushcli  links mushcore  → gets these definitions
//   • mushgui  links mushapp   → gets these definitions
//   • tests    link mushcore   → get these definitions
// No target links both libraries, so there is no ODR conflict.

#ifndef _WIN32

#include "platform/wx_mfc_hooks.h"

void (*g_wxhook_invalidate)(void*, bool)                         = nullptr;
void (*g_wxhook_show_window)(void*, int)                         = nullptr;
void (*g_wxhook_update_window)(void*)                            = nullptr;
void (*g_wxhook_get_client_rect)(const void*, RECT*)             = nullptr;
void (*g_wxhook_set_window_text)(void*, const char*)             = nullptr;
void (*g_wxhook_move_window)(void*, int, int, int, int, bool)    = nullptr;
void (*g_wxhook_destroy_window)(void*)                           = nullptr;
void (*g_wxhook_set_focus)(void*)                                = nullptr;
long (*g_wxhook_send_message)(void*, unsigned, uintptr_t, intptr_t) = nullptr;
int  (*g_wxhook_post_message)(void*, unsigned, uintptr_t, intptr_t) = nullptr;
int  (*g_wxhook_message_box)(const char*, const char*, unsigned) = nullptr;
int  (*g_wxhook_load_mdi_frame)(void*, unsigned, unsigned long, void*) = nullptr;
int  (*g_wxhook_run_app_loop)()                                  = nullptr;
int  (*g_wxhook_init_instance)()                                 = nullptr;
void* (*g_wxhook_new_world)()                                    = nullptr;
void* (*g_wxhook_open_world_file)(const char*)                   = nullptr;
int   (*g_wxhook_quick_connect_dlg)(char**, char**, int*)        = nullptr;
void  (*g_wxhook_set_status)(const char*)                        = nullptr;
bool  (*g_wxhook_game_preferences)(void*, int)                   = nullptr;

#endif // !_WIN32

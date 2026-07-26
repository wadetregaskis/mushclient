// platform/wx_mfc_hooks_storage.cpp
// Defines (owns) all wx hook function pointer globals declared extern in
// wx_mfc_hooks.h.  All pointers are initialised to nullptr here; mushgui's
// wx_mfc_impl.cpp overwrites them via wx_mfc_install_hooks() at startup.
//
// This file is compiled into BOTH mushcore and mushapp so that every link
// target that includes windows_stubs.cpp (or any other TU that calls through
// a hook) can resolve the symbols:
//   • the tests link mushcore    → get these definitions
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
int  (*g_wxhook_init_instance)()                                 = nullptr;
void* (*g_wxhook_new_world)()                                    = nullptr;
void* (*g_wxhook_open_world_file)(const char*)                   = nullptr;
void  (*g_wxhook_open_default_world)()                           = nullptr;
int   (*g_wxhook_query_quit_save)()                              = nullptr;
int   (*g_wxhook_report_mfc_exception)()                         = nullptr;
void  (*g_wxhook_set_status)(const char*)                        = nullptr;
bool  (*g_wxhook_save_world)(void*, bool, void*)                 = nullptr;
char* (*g_wxhook_file_dialog)(bool, const char*, const char*,
                               const char*, const char*, bool)   = nullptr;
int   (*g_wxhook_font_dialog)(void*, struct WxFontChoice*)       = nullptr;
char* (*g_wxhook_get_clipboard_text)()                           = nullptr;
void  (*g_wxhook_set_clipboard_text)(const char*)                = nullptr;
void* (*g_wxhook_open_text_file)(const char*) = nullptr;

void* (*g_wxhook_create_text_window)(
    const char*, const char*, CMUSHclientDoc*, long long,
    const char*, long, long, unsigned int,
    unsigned long, unsigned long,
    const char*, const char*,
    bool, bool, bool, bool, bool, int, int)                      = nullptr;
bool (*g_wxhook_play_sound)(const char*, bool)                   = nullptr;
void (*g_wxhook_stop_sound)()                                    = nullptr;
int  (*g_wxhook_sound_play)(int, const char*, int, double, double) = nullptr;
int  (*g_wxhook_sound_play_memory)(int, const void*, size_t, int, double, double) = nullptr;
int  (*g_wxhook_sound_adjust)(int, int, double, double)          = nullptr;
void (*g_wxhook_sound_stop_buffer)(int)                          = nullptr;
int  (*g_wxhook_sound_status)(int)                               = nullptr;
const char* (*g_wxhook_load_text_resource)(int, unsigned*)       = nullptr;
char** (*g_wxhook_get_font_families)()                           = nullptr;
int    (*g_wxhook_font_has_glyph)(const char*, unsigned int)     = nullptr;
unsigned long (*g_wxhook_get_sys_color)(int)                     = nullptr;
int           (*g_wxhook_get_system_metric)(int)                = nullptr;
unsigned long (*g_wxhook_get_pixel)(void*, int, int)            = nullptr;
short         (*g_wxhook_get_key_state)(int)                    = nullptr;
void          (*g_wxhook_edit_get_sel)(void*, int*, int*)       = nullptr;
void          (*g_wxhook_edit_set_sel)(void*, int, int)         = nullptr;
void          (*g_wxhook_edit_replace_sel)(void*, const char*)  = nullptr;
int           (*g_wxhook_edit_line_info)(void*, int, int)       = nullptr;
void          (*g_wxhook_set_font)(void*, void*)                = nullptr;
void          (*g_wxhook_set_cursor)(void*, int)                = nullptr;
void          (*g_wxhook_set_main_background)(unsigned long)    = nullptr;
bool (*g_wxhook_append_to_notepad)(const char*, const char*, bool) = nullptr;
char* (*g_wxhook_get_notepad_text)(const char*)                    = nullptr;
char** (*g_wxhook_get_notepad_list)()                              = nullptr;
bool   (*g_wxhook_activate_notepad)(const char*)                   = nullptr;
bool   (*g_wxhook_close_notepad)(const char*)                      = nullptr;
bool   (*g_wxhook_move_notepad)(const char*, int, int, int, int)   = nullptr;
char*  (*g_wxhook_get_notepad_position)(const char*)               = nullptr;
bool   (*g_wxhook_save_notepad)(const char*, const char*, bool)    = nullptr;
bool   (*g_wxhook_notepad_colour)(const char*, unsigned long, unsigned long) = nullptr;
bool   (*g_wxhook_notepad_font)(const char*, const char*, int, int, int)     = nullptr;
bool   (*g_wxhook_notepad_readonly)(const char*, bool)                       = nullptr;
long (*g_wxhook_pick_colour)(long)                               = nullptr;
char* (*g_wxhook_menu_dialog)(const char* const*, int,
                               const char*)                      = nullptr;
void (*g_wxhook_flash_icon)()                                    = nullptr;
void (*g_wxhook_move_main_window)(long, long, long, long)        = nullptr;
char* (*g_wxhook_inputbox)(const char*, const char*,
                            const char*, bool)                   = nullptr;
char* (*g_wxhook_directorypicker)(const char*, const char*)      = nullptr;
int   (*g_wxhook_password_dialog)(const char*, char**)           = nullptr;
int   (*g_wxhook_popup_menu)(CMenu*, int, int)                   = nullptr;
intptr_t (*g_run_mfc_dialog)(void*, unsigned)                    = nullptr;
void  (*g_end_mfc_dialog)(void*, int)                            = nullptr;
intptr_t (*g_run_mfc_property_sheet)(void*)                      = nullptr;
intptr_t (*g_run_mfc_property_sheet_flat)(void*)                 = nullptr;
void* (*g_wxhook_get_dlg_item)(void*, int)                       = nullptr;
void* (*g_wxhook_subclass_dlg_item)(void*, int, void*)           = nullptr;
char* (*g_wxhook_get_window_text)(void*)                         = nullptr;
void  (*g_wxhook_button_set_check)(void*, int)                   = nullptr;
int   (*g_wxhook_button_get_check)(void*)                        = nullptr;
void  (*g_wxhook_enable_window)(void*, int)                      = nullptr;
void (*g_wxhook_listctrl_insert_column)(void*, int, const char*, int, int) = nullptr;
int  (*g_wxhook_listctrl_insert_item)(void*, int, const char*)   = nullptr;
void (*g_wxhook_listctrl_set_item_text)(void*, int, int, const char*) = nullptr;
int  (*g_wxhook_listctrl_get_item_count)(void*)                  = nullptr;
void (*g_wxhook_listctrl_set_item_data)(void*, int, void*)       = nullptr;
void* (*g_wxhook_listctrl_get_item_data)(void*, int)             = nullptr;
int  (*g_wxhook_listctrl_get_next_item)(void*, int, int)         = nullptr;
void (*g_wxhook_listctrl_set_item_state)(void*, int, unsigned, unsigned) = nullptr;
unsigned (*g_wxhook_listctrl_get_item_state)(void*, int, unsigned) = nullptr;
int  (*g_wxhook_listctrl_get_selected_count)(void*)              = nullptr;
void (*g_wxhook_listctrl_delete_item)(void*, int)                = nullptr;
void (*g_wxhook_listctrl_delete_all_items)(void*)                = nullptr;
void (*g_wxhook_listctrl_ensure_visible)(void*, int)             = nullptr;
void (*g_wxhook_listctrl_set_column_width)(void*, int, int)      = nullptr;
int  (*g_wxhook_listctrl_get_column_width)(void*, int)           = nullptr;
void (*g_wxhook_listctrl_sort_items)(void*,
        int (*)(intptr_t, intptr_t, intptr_t), intptr_t)         = nullptr;
int   (*g_wxhook_listbox_add_string)(void*, const char*)         = nullptr;
void  (*g_wxhook_listbox_reset_content)(void*)                   = nullptr;
int   (*g_wxhook_listbox_get_count)(void*)                       = nullptr;
int   (*g_wxhook_listbox_get_cur_sel)(void*)                     = nullptr;
int   (*g_wxhook_listbox_set_cur_sel)(void*, int)                = nullptr;
char* (*g_wxhook_listbox_get_text)(void*, int)                   = nullptr;
void     (*g_wxhook_listbox_set_item_data)(void*, int, intptr_t) = nullptr;
intptr_t (*g_wxhook_listbox_get_item_data)(void*, int)           = nullptr;
int      (*g_wxhook_listbox_get_sel_count)(void*)                = nullptr;
int      (*g_wxhook_listbox_get_sel_items)(void*, int, int*)     = nullptr;
int      (*g_wxhook_listbox_get_sel)(void*, int)                 = nullptr;
int      (*g_wxhook_listbox_set_sel)(void*, int, int)            = nullptr;
int  (*g_wxhook_combo_add_string)(void*, const char*)            = nullptr;
int  (*g_wxhook_combo_get_count)(void*)                          = nullptr;
int  (*g_wxhook_combo_get_cur_sel)(void*)                        = nullptr;
void (*g_wxhook_combo_set_cur_sel)(void*, int)                   = nullptr;
int  (*g_wxhook_combo_find_string_exact)(void*, int, const char*) = nullptr;
char* (*g_wxhook_combo_get_lb_text)(void*, int)                  = nullptr;
void (*g_wxhook_combo_reset_content)(void*)                      = nullptr;
int  (*g_wxhook_radio_get)(void*, int)                           = nullptr;
void (*g_wxhook_radio_set)(void*, int, int)                      = nullptr;
void* (*g_wxhook_make_owner_draw_button)(void*, int, const char*, int, int, int, int) = nullptr;
void (*g_wxhook_bind_owner_draw)(void*, void*)                   = nullptr;
void (*g_wxhook_slider_set_range)(void*, int, int)               = nullptr;
int  (*g_wxhook_slider_get_pos)(void*)                           = nullptr;
void (*g_wxhook_slider_set_pos)(void*, int)                      = nullptr;
void (*g_wxhook_slider_set_page_size)(void*, int)                = nullptr;
void (*g_wxhook_bind_slider)(void*, void*)                       = nullptr;
void* (*g_wxhook_make_spin)(void*, int, int, int, int, int)      = nullptr;
void (*g_wxhook_spin_set_range)(void*, int, int)                 = nullptr;
void (*g_wxhook_spin_set_pos)(void*, int)                        = nullptr;
int  (*g_wxhook_spin_get_pos)(void*)                             = nullptr;
void (*g_wxhook_bind_spin)(void*, void*)                         = nullptr;
void* (*g_wxhook_make_hotkey)(void*, int, int, int, int, int)    = nullptr;
void  (*g_wxhook_hotkey_get)(void*, unsigned short*, unsigned short*) = nullptr;
void (*g_wxhook_get_window_rect)(const void*, RECT*)             = nullptr;
void (*g_wxhook_client_to_screen)(const void*, int*, int*)       = nullptr;
void (*g_wxhook_screen_to_client)(const void*, int*, int*)       = nullptr;

#endif // !_WIN32

// tests/test_mfc_ui.cpp
// Unit tests for the pure-logic parts of the wx/MFC UI shim
// (platform/mfc_ui.h + the CWnd core in platform/compat.h).
//
// This IS shipping code on the mac product: every dialog, control, collection,
// DDX exchange, VARIANT/SAFEARRAY marshal and message-map dispatch in the app
// runs through these classes.  The hook-gated control classes are driven here
// through a fake in-memory "wx" widget store installed into the g_wxhook_*
// function-pointer table (the same seam the real wx layer uses), so the tests
// exercise the real shim logic on both the null-hook (headless) and hooked
// paths.
//
// Unreachable-by-design in this build (skip list):
//  * MUSHCLIENT_FULL_BUILD arms of CDC/CPen/CBrush/... (wx_gdi_compat.cpp
//    bodies — mushgui only; the stub arms are what mushcore ships).
//  * ShellExecute itself (runs system(); side-effectful by contract) --
//    but its argument handling is now covered via BuildShellExecuteCommand.
//  * sizeof(wchar_t)==2 surrogate arms of MultiByteToWideChar /
//    WideCharToMultiByte (wchar_t is 4 bytes on macOS/Linux).

#include "mc_test.h"
// B-3 atomic flip: real headers (portable stub deleted; tests are FULL_BUILD).
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "mainfrm.h"   // CMainFrame (platform/mainfrm_stub.h here) — FramePostMessage tests
#include "scripting/errors.h"
#include "platform/world_close_decision.h"  // DecideWorldClose — the Cmd+W prompt chain
#include "dialogs/ProgDlg.h"   // CProgressDlg — adopted into mushcore 2026-08-07
#include "dialogs/plugins/PluginWizard.h"  // CPluginWizardPage7 — language gates
#include "dialogs/plugins/PluginWizardSheet.h"  // CPluginWizardSheet — OnKickIdle
#include "dialogs/world_prefs/prefspropertypages.h"  // FittedLastColumnWidth,
                                                     // OutputBufferBytesNeeded
#include "dialogs/InsertUnicodeDlg.h"  // CInsertUnicodeDlg — no wx dependency at
                                        // all, so tests/CMakeLists.txt compiles
                                        // the real .cpp into this test binary too
#include "platform/wx_mfc_hooks.h"     // g_wxhook_message_box — captures the text
                                        // DDV_MinMaxString actually puts up

#include <algorithm>
#include <chrono>     // CStringList walk-complexity pin
#include <cerrno>     // ENOENT — CFile open-failure cause checks
#include <csignal>    // SIGXFSZ — Files.OnSaveDocumentReportsWriteFailure
#include <cstring>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <strings.h>  // strcasecmp — ComboBox FindStringExact's case fold
#include <sys/resource.h>  // RLIMIT_FSIZE — simulated full disk
#include <sys/stat.h>
#include <unistd.h>

// ═══════════════════════════════════════════════════════════════════════════
// Fake wx widget store (drives the g_wxhook_* seam)
// ═══════════════════════════════════════════════════════════════════════════

namespace fakewx {

struct Widget {
    std::string text;
    int         check = 0;
    bool        enabled = true;
    std::vector<std::string> items;      // combo / listbox strings
    std::map<int, intptr_t>  itemData;
    std::set<int>            sel;        // listbox multi-selection
    int         cursel = -1;
};

// listctrl rows: per-row column texts + item data + state
struct Row {
    std::map<int, std::string> cols;
    void*    data  = nullptr;
    unsigned state = 0;
};

inline std::map<void*, Widget>& widgets() { static std::map<void*, Widget> m; return m; }
inline std::vector<Row>&        rows()    { static std::vector<Row> r; return r; }
inline std::map<int, int>&      radios()  { static std::map<int, int> r; return r; }  // firstId -> index

// Stable fake HWND per control id (parent ignored: one dialog at a time).
inline void* handle_for(int id) { return (void*)(uintptr_t)(0x10000 + id); }

inline Widget& widget(int id) { return widgets()[handle_for(id)]; }

inline void reset_store() { widgets().clear(); rows().clear(); radios().clear(); }

// ── hook implementations ──────────────────────────────────────────────────
inline void* hk_get_dlg_item(void*, int id)      { return handle_for(id); }
inline char* hk_get_window_text(void* h)         { return strdup(widgets()[h].text.c_str()); }
inline void  hk_set_window_text(void* h, const char* t) { widgets()[h].text = t ? t : ""; }
inline void  hk_button_set_check(void* h, int s) { widgets()[h].check = s; }
inline int   hk_button_get_check(void* h)        { return widgets()[h].check; }
inline void  hk_enable_window(void* h, int e)    { widgets()[h].enabled = (e != 0); }
inline int   hk_radio_get(void*, int firstId)    { return radios().count(firstId) ? radios()[firstId] : -1; }
inline void  hk_radio_set(void*, int firstId, int index) { radios()[firstId] = index; }

// combo
inline int  hk_combo_add_string(void* h, const char* t) { widgets()[h].items.push_back(t); return (int)widgets()[h].items.size() - 1; }
inline int  hk_combo_get_count(void* h)                { return (int)widgets()[h].items.size(); }
inline int  hk_combo_get_cur_sel(void* h)              { return widgets()[h].cursel; }
inline void hk_combo_set_cur_sel(void* h, int s)       { widgets()[h].cursel = s; }
inline int  hk_combo_find_string_exact(void* h, int after, const char* t) {
    // Mirrors the shipping hook (wx_mfc_impl.cpp hook_combo_find_string_exact):
    // the real CB_FINDSTRINGEXACT contract wraps once past the end and folds
    // case, so the double has to as well or it proves nothing about the
    // hooked path it stands in for.
    auto& it = widgets()[h].items;
    int n = (int)it.size();
    if (n == 0) return -1;
    for (int i = 0; i < n; ++i) {
        int idx = (after + 1 + i) % n;
        if (strcasecmp(it[idx].c_str(), t) == 0) return idx;
    }
    return -1;
}
inline char* hk_combo_get_lb_text(void* h, int i) {
    auto& it = widgets()[h].items;
    return (i >= 0 && i < (int)it.size()) ? strdup(it[i].c_str()) : nullptr;
}
inline void hk_combo_reset_content(void* h) {
    widgets()[h].items.clear(); widgets()[h].cursel = -1; widgets()[h].itemData.clear();
}
// Range-checked, like the real wx hooks (hook_combo_set/get_item_data): wx
// would assert on an out-of-range index, so the shim drops those calls, and the
// fake has to do the same or the test proves nothing about the shipping path.
inline bool in_range(void* h, int i) { return i >= 0 && i < (int) widgets()[h].items.size(); }
inline void     hk_combo_set_item_data(void* h, int i, intptr_t d) { if (in_range(h, i)) widgets()[h].itemData[i] = d; }
inline intptr_t hk_combo_get_item_data(void* h, int i)             { return (in_range(h, i) && widgets()[h].itemData.count(i)) ? widgets()[h].itemData[i] : 0; }

// listbox
inline int  hk_lb_add_string(void* h, const char* t) { widgets()[h].items.push_back(t); return (int)widgets()[h].items.size() - 1; }
inline void hk_lb_reset(void* h)                     { widgets()[h].items.clear(); widgets()[h].sel.clear(); widgets()[h].cursel = -1; }
inline int  hk_lb_get_count(void* h)                 { return (int)widgets()[h].items.size(); }
inline int  hk_lb_get_cur_sel(void* h)               { return widgets()[h].cursel; }
inline int  hk_lb_set_cur_sel(void* h, int s)        { widgets()[h].cursel = s; return s; }
inline char* hk_lb_get_text(void* h, int i) {
    auto& it = widgets()[h].items;
    return (i >= 0 && i < (int)it.size()) ? strdup(it[i].c_str()) : nullptr;
}
inline void     hk_lb_set_item_data(void* h, int i, intptr_t d) { if (in_range(h, i)) widgets()[h].itemData[i] = d; }
inline intptr_t hk_lb_get_item_data(void* h, int i)             { return (in_range(h, i) && widgets()[h].itemData.count(i)) ? widgets()[h].itemData[i] : 0; }
inline int      hk_lb_get_sel_count(void* h)                    { return (int)widgets()[h].sel.size(); }
inline int      hk_lb_get_sel_items(void* h, int maxItems, int* buf) {
    int n = 0;
    for (int i : widgets()[h].sel) { if (n >= maxItems) break; buf[n++] = i; }
    return n;
}
inline int hk_lb_get_sel(void* h, int i)          { return widgets()[h].sel.count(i) ? 1 : 0; }
inline int hk_lb_set_sel(void* h, int i, int on)  { if (on) widgets()[h].sel.insert(i); else widgets()[h].sel.erase(i); return 0; }

// listctrl
inline void hk_lc_insert_column(void*, int, const char*, int, int) {}
inline int  hk_lc_insert_item(void*, int item, const char* text) {
    Row r; r.cols[0] = text ? text : "";
    if (item < 0 || item > (int)rows().size()) item = (int)rows().size();
    rows().insert(rows().begin() + item, r);
    return item;
}
inline void hk_lc_set_item_text(void*, int item, int col, const char* text) {
    if (item >= 0 && item < (int)rows().size()) rows()[item].cols[col] = text ? text : "";
}
inline int   hk_lc_get_item_count(void*) { return (int)rows().size(); }
inline void  hk_lc_set_item_data(void*, int item, void* d) { if (item >= 0 && item < (int)rows().size()) rows()[item].data = d; }
inline void* hk_lc_get_item_data(void*, int item) { return (item >= 0 && item < (int)rows().size()) ? rows()[item].data : nullptr; }
inline int   hk_lc_get_next_item(void*, int start, int) {
    for (int i = start + 1; i < (int)rows().size(); ++i)
        if (rows()[i].state & 0x0002 /*LVIS_SELECTED*/) return i;
    return -1;
}
inline void hk_lc_set_item_state(void*, int item, unsigned state, unsigned mask) {
    if (item >= 0 && item < (int)rows().size())
        rows()[item].state = (rows()[item].state & ~mask) | (state & mask);
}
inline unsigned hk_lc_get_item_state(void*, int item, unsigned mask) {
    return (item >= 0 && item < (int)rows().size()) ? (rows()[item].state & mask) : 0;
}
inline int  hk_lc_get_selected_count(void*) {
    int n = 0;
    for (auto& r : rows()) if (r.state & 0x0002) ++n;
    return n;
}
inline void hk_lc_delete_item(void*, int item) { if (item >= 0 && item < (int)rows().size()) rows().erase(rows().begin() + item); }
inline void hk_lc_delete_all(void*)            { rows().clear(); }
inline void hk_lc_ensure_visible(void*, int)   {}
inline void hk_lc_set_col_width(void*, int, int) {}
inline void hk_lc_sort_items(void*, int (*cmp)(intptr_t, intptr_t, intptr_t), intptr_t lp) {
    std::stable_sort(rows().begin(), rows().end(), [&](const Row& a, const Row& b) {
        return cmp((intptr_t)a.data, (intptr_t)b.data, lp) < 0;
    });
}

// RAII installer: installs every store hook, restores all to null on exit.
struct Install {
    Install() {
        reset_store();
        g_wxhook_get_dlg_item        = hk_get_dlg_item;
        g_wxhook_get_window_text     = hk_get_window_text;
        g_wxhook_set_window_text     = hk_set_window_text;
        g_wxhook_button_set_check    = hk_button_set_check;
        g_wxhook_button_get_check    = hk_button_get_check;
        g_wxhook_enable_window       = hk_enable_window;
        g_wxhook_radio_get           = hk_radio_get;
        g_wxhook_radio_set           = hk_radio_set;
        g_wxhook_combo_add_string        = hk_combo_add_string;
        g_wxhook_combo_get_count         = hk_combo_get_count;
        g_wxhook_combo_get_cur_sel       = hk_combo_get_cur_sel;
        g_wxhook_combo_set_cur_sel       = hk_combo_set_cur_sel;
        g_wxhook_combo_find_string_exact = hk_combo_find_string_exact;
        g_wxhook_combo_get_lb_text       = hk_combo_get_lb_text;
        g_wxhook_combo_reset_content     = hk_combo_reset_content;
        g_wxhook_combo_set_item_data     = hk_combo_set_item_data;
        g_wxhook_combo_get_item_data     = hk_combo_get_item_data;
        g_wxhook_listbox_add_string      = hk_lb_add_string;
        g_wxhook_listbox_reset_content   = hk_lb_reset;
        g_wxhook_listbox_get_count       = hk_lb_get_count;
        g_wxhook_listbox_get_cur_sel     = hk_lb_get_cur_sel;
        g_wxhook_listbox_set_cur_sel     = hk_lb_set_cur_sel;
        g_wxhook_listbox_get_text        = hk_lb_get_text;
        g_wxhook_listbox_set_item_data   = hk_lb_set_item_data;
        g_wxhook_listbox_get_item_data   = hk_lb_get_item_data;
        g_wxhook_listbox_get_sel_count   = hk_lb_get_sel_count;
        g_wxhook_listbox_get_sel_items   = hk_lb_get_sel_items;
        g_wxhook_listbox_get_sel         = hk_lb_get_sel;
        g_wxhook_listbox_set_sel         = hk_lb_set_sel;
        g_wxhook_listctrl_insert_column      = hk_lc_insert_column;
        g_wxhook_listctrl_insert_item        = hk_lc_insert_item;
        g_wxhook_listctrl_set_item_text      = hk_lc_set_item_text;
        g_wxhook_listctrl_get_item_count     = hk_lc_get_item_count;
        g_wxhook_listctrl_set_item_data      = hk_lc_set_item_data;
        g_wxhook_listctrl_get_item_data      = hk_lc_get_item_data;
        g_wxhook_listctrl_get_next_item      = hk_lc_get_next_item;
        g_wxhook_listctrl_set_item_state     = hk_lc_set_item_state;
        g_wxhook_listctrl_get_item_state     = hk_lc_get_item_state;
        g_wxhook_listctrl_get_selected_count = hk_lc_get_selected_count;
        g_wxhook_listctrl_delete_item        = hk_lc_delete_item;
        g_wxhook_listctrl_delete_all_items   = hk_lc_delete_all;
        g_wxhook_listctrl_ensure_visible     = hk_lc_ensure_visible;
        g_wxhook_listctrl_set_column_width   = hk_lc_set_col_width;
        g_wxhook_listctrl_sort_items         = hk_lc_sort_items;
    }
    ~Install() {
        g_wxhook_get_dlg_item        = nullptr;
        g_wxhook_get_window_text     = nullptr;
        g_wxhook_set_window_text     = nullptr;
        g_wxhook_button_set_check    = nullptr;
        g_wxhook_button_get_check    = nullptr;
        g_wxhook_enable_window       = nullptr;
        g_wxhook_radio_get           = nullptr;
        g_wxhook_radio_set           = nullptr;
        g_wxhook_combo_add_string        = nullptr;
        g_wxhook_combo_get_count         = nullptr;
        g_wxhook_combo_get_cur_sel       = nullptr;
        g_wxhook_combo_set_cur_sel       = nullptr;
        g_wxhook_combo_find_string_exact = nullptr;
        g_wxhook_combo_get_lb_text       = nullptr;
        g_wxhook_combo_reset_content     = nullptr;
        g_wxhook_combo_set_item_data     = nullptr;
        g_wxhook_combo_get_item_data     = nullptr;
        g_wxhook_listbox_add_string      = nullptr;
        g_wxhook_listbox_reset_content   = nullptr;
        g_wxhook_listbox_get_count       = nullptr;
        g_wxhook_listbox_get_cur_sel     = nullptr;
        g_wxhook_listbox_set_cur_sel     = nullptr;
        g_wxhook_listbox_get_text        = nullptr;
        g_wxhook_listbox_set_item_data   = nullptr;
        g_wxhook_listbox_get_item_data   = nullptr;
        g_wxhook_listbox_get_sel_count   = nullptr;
        g_wxhook_listbox_get_sel_items   = nullptr;
        g_wxhook_listbox_get_sel         = nullptr;
        g_wxhook_listbox_set_sel         = nullptr;
        g_wxhook_listctrl_insert_column      = nullptr;
        g_wxhook_listctrl_insert_item        = nullptr;
        g_wxhook_listctrl_set_item_text      = nullptr;
        g_wxhook_listctrl_get_item_count     = nullptr;
        g_wxhook_listctrl_set_item_data      = nullptr;
        g_wxhook_listctrl_get_item_data      = nullptr;
        g_wxhook_listctrl_get_next_item      = nullptr;
        g_wxhook_listctrl_set_item_state     = nullptr;
        g_wxhook_listctrl_get_item_state     = nullptr;
        g_wxhook_listctrl_get_selected_count = nullptr;
        g_wxhook_listctrl_delete_item        = nullptr;
        g_wxhook_listctrl_delete_all_items   = nullptr;
        g_wxhook_listctrl_ensure_visible     = nullptr;
        g_wxhook_listctrl_set_column_width   = nullptr;
        g_wxhook_listctrl_sort_items         = nullptr;
    }
};

} // namespace fakewx

// ═══════════════════════════════════════════════════════════════════════════
// Geometry: CPoint / CSize / CRect
// ═══════════════════════════════════════════════════════════════════════════

TEST(Geometry, PointArithmeticAndComparison) {
    CPoint a(3, 4), b(1, 2);
    EXPECT_TRUE(a != b);
    EXPECT_TRUE(CPoint(3, 4) == a);
    CPoint sum = a + b;
    EXPECT_EQ(4, sum.x); EXPECT_EQ(6, sum.y);
    CPoint diff = a - b;
    EXPECT_EQ(2, diff.x); EXPECT_EQ(2, diff.y);
    a += b;  EXPECT_EQ(4, a.x); EXPECT_EQ(6, a.y);
    a -= b;  EXPECT_EQ(3, a.x); EXPECT_EQ(4, a.y);
    a.Offset(10, -10);
    EXPECT_EQ(13, a.x); EXPECT_EQ(-6, a.y);
}

TEST(Geometry, PointFromDwordSignExtends) {
    // Packed (y << 16) | x with negative coordinates: LOWORD/HIWORD are cast
    // through (short), so 0xFFFF unpacks to -1, not 65535.
    DWORD dw = ((DWORD)(WORD)(short)-1 << 16) | (WORD)(short)-2;
    CPoint p(dw);
    EXPECT_EQ(-2, p.x);
    EXPECT_EQ(-1, p.y);
}

TEST(Geometry, PointPOINTConversions) {
    POINT raw; raw.x = 7; raw.y = 8;
    CPoint p(raw);
    EXPECT_EQ(7, p.x);
    POINT back = p;
    EXPECT_EQ(7, (int)back.x); EXPECT_EQ(8, (int)back.y);
    SIZE sz; sz.cx = 9; sz.cy = 10;
    p = sz;                      // MFC's CPoint = SIZE
    EXPECT_EQ(9, p.x); EXPECT_EQ(10, p.y);
    POINT* pp = p;               // operator POINT*
    EXPECT_EQ(9, (int)pp->x);
}

TEST(Geometry, SizeOperators) {
    CSize a(5, 6), b(1, 2);
    EXPECT_TRUE(a != b);
    EXPECT_TRUE(CSize(5, 6) == a);
    CSize sum = a + b;  EXPECT_EQ(6, sum.cx); EXPECT_EQ(8, sum.cy);
    CSize dif = a - b;  EXPECT_EQ(4, dif.cx); EXPECT_EQ(4, dif.cy);
    // Windows scroll shorthand: assigning an int sets cy only
    a = 42;
    EXPECT_EQ(5, a.cx); EXPECT_EQ(42, a.cy);
    a = 43L;
    EXPECT_EQ(43, a.cy);
    SIZE raw = a;
    EXPECT_EQ(5, (int)raw.cx);
}

TEST(Geometry, RectBasics) {
    CRect r(10, 20, 110, 70);
    EXPECT_EQ(100, r.Width());
    EXPECT_EQ(50, r.Height());
    EXPECT_FALSE(r.IsRectEmpty());
    EXPECT_FALSE(r.IsRectNull());
    EXPECT_EQ(10, r.TopLeft().x);
    EXPECT_EQ(110, r.BottomRight().x);
    EXPECT_EQ(60, r.CenterPoint().x);
    EXPECT_EQ(45, r.CenterPoint().y);
    EXPECT_EQ(100, r.Size().cx);
    CRect nullr;
    EXPECT_TRUE(nullr.IsRectNull());
    EXPECT_TRUE(nullr.IsRectEmpty());
}

TEST(Geometry, RectConstructionForms) {
    RECT raw; raw.left = 1; raw.top = 2; raw.right = 3; raw.bottom = 4;
    CRect fromRaw(raw);
    EXPECT_EQ(1, (int)fromRaw.left); EXPECT_EQ(4, (int)fromRaw.bottom);
    CRect fromPts(CPoint(1, 2), CPoint(11, 22));
    EXPECT_EQ(11, (int)fromPts.right);
    CRect fromSize(CPoint(5, 6), CSize(20, 30));
    EXPECT_EQ(25, (int)fromSize.right);
    EXPECT_EQ(36, (int)fromSize.bottom);
}

TEST(Geometry, RectPtInRectIsHalfOpen) {
    CRect r(0, 0, 10, 10);
    EXPECT_TRUE(r.PtInRect(CPoint(0, 0)));
    EXPECT_TRUE(r.PtInRect(CPoint(9, 9)));
    EXPECT_FALSE(r.PtInRect(CPoint(10, 9)));   // right edge exclusive
    EXPECT_FALSE(r.PtInRect(CPoint(9, 10)));   // bottom edge exclusive
    POINT raw; raw.x = 5; raw.y = 5;
    EXPECT_TRUE(r.PtInRect(raw));
}

TEST(Geometry, RectInflateDeflateOffsetMove) {
    CRect r(10, 10, 20, 20);
    r.InflateRect(2, 3);
    EXPECT_EQ(8, (int)r.left); EXPECT_EQ(7, (int)r.top);
    EXPECT_EQ(22, (int)r.right); EXPECT_EQ(23, (int)r.bottom);
    r.DeflateRect(2, 3);
    EXPECT_TRUE(r == CRect(10, 10, 20, 20));
    r.InflateRect(1, 2, 3, 4);   // four-sided form
    EXPECT_TRUE(r == CRect(9, 8, 23, 24));
    r.SetRect(0, 0, 10, 5);
    r.OffsetRect(100, 200);
    EXPECT_TRUE(r == CRect(100, 200, 110, 205));
    r.MoveToXY(0, 0);
    EXPECT_TRUE(r == CRect(0, 0, 10, 5));
    r.SetRectEmpty();
    EXPECT_TRUE(r.IsRectNull());
}

TEST(Geometry, RectIntersectAndUnion) {
    CRect a(0, 0, 10, 10), b(5, 5, 15, 15), out;
    EXPECT_TRUE(out.IntersectRect(&a, &b));
    EXPECT_TRUE(out == CRect(5, 5, 10, 10));
    CRect c(20, 20, 30, 30);
    EXPECT_FALSE(out.IntersectRect(&a, &c));   // disjoint -> empty -> FALSE
    EXPECT_FALSE(out.IntersectRect(nullptr, &a));
    CRect u;
    EXPECT_TRUE(u.UnionRect(&a, &c));
    EXPECT_TRUE(u == CRect(0, 0, 30, 30));
    EXPECT_FALSE(u.UnionRect(&a, nullptr));
    // operator&= / |=
    CRect i(0, 0, 10, 10); i &= CRect(5, -5, 20, 8);
    EXPECT_TRUE(i == CRect(5, 0, 10, 8));
    CRect o(0, 0, 1, 1);  o |= CRect(-5, 2, 0, 9);
    EXPECT_TRUE(o == CRect(-5, 0, 1, 9));
}

TEST(Geometry, RectNormalize) {
    CRect r(10, 20, 3, 4);
    r.NormalizeRect();
    EXPECT_TRUE(r == CRect(3, 4, 10, 20));
}

TEST(Geometry, GlobalRectHelpers) {
    RECT r;
    SetRect(&r, 1, 2, 3, 4);
    EXPECT_EQ(1, (int)r.left); EXPECT_EQ(4, (int)r.bottom);
    OffsetRect(&r, 10, 20);
    EXPECT_EQ(11, (int)r.left); EXPECT_EQ(24, (int)r.bottom);
    InflateRect(&r, 1, 1);
    EXPECT_EQ(10, (int)r.left); EXPECT_EQ(25, (int)r.bottom);
    RECT a, b, d;
    SetRect(&a, 0, 0, 10, 10);
    SetRect(&b, 5, 5, 15, 15);
    EXPECT_TRUE(IntersectRect(&d, &a, &b));
    EXPECT_EQ(5, (int)d.left); EXPECT_EQ(10, (int)d.right);
    SetRect(&b, 50, 50, 60, 60);
    EXPECT_FALSE(IntersectRect(&d, &a, &b));
    EXPECT_TRUE(IsRectEmpty(&d));
    EXPECT_FALSE(IsRectEmpty(&a));
    EXPECT_TRUE(EqualRect(&a, &a));
    EXPECT_FALSE(EqualRect(&a, &b));
    POINT p; p.x = 3; p.y = 3;
    EXPECT_TRUE(PtInRect(&a, p));
    p.x = 10;
    EXPECT_FALSE(PtInRect(&a, p));   // half-open
}

// ═══════════════════════════════════════════════════════════════════════════
// Collections: CStringList / CStringArray / CArray / CMapStringToString /
//              CUIntArray / CList
// ═══════════════════════════════════════════════════════════════════════════

TEST(Collections, StringListAddIterateRemove) {
    CStringList l;
    EXPECT_TRUE(l.IsEmpty());
    l.AddTail("b");
    l.AddTail("c");
    l.AddHead("a");
    EXPECT_EQ(3, l.GetCount());
    EXPECT_STREQ("a", (LPCTSTR)l.GetHead());
    EXPECT_STREQ("c", (LPCTSTR)l.GetTail());

    // forward iteration
    std::string all;
    for (POSITION pos = l.GetHeadPosition(); pos; )
        all += (LPCTSTR)l.GetNext(pos);
    EXPECT_STREQ("abc", all.c_str());

    // backward iteration
    all.clear();
    for (POSITION pos = l.GetTailPosition(); pos; )
        all += (LPCTSTR)l.GetPrev(pos);
    EXPECT_STREQ("cba", all.c_str());

    EXPECT_STREQ("a", (LPCTSTR)l.RemoveHead());
    EXPECT_STREQ("c", (LPCTSTR)l.RemoveTail());
    EXPECT_EQ(1, l.GetCount());
    l.RemoveAll();
    EXPECT_TRUE(l.IsEmpty());
    EXPECT_EQ((POSITION)nullptr, l.GetHeadPosition());
    EXPECT_EQ((POSITION)nullptr, l.GetTailPosition());
}

TEST(Collections, StringListFindInsertRemoveAt) {
    CStringList l;
    l.AddTail("one");
    l.AddTail("two");
    l.AddTail("three");
    POSITION p = l.Find("two");
    ASSERT_NE((POSITION)nullptr, p);
    EXPECT_STREQ("two", (LPCTSTR)l.GetAt(p));
    EXPECT_EQ((POSITION)nullptr, l.Find("missing"));

    POSITION p1 = l.FindIndex(1);
    EXPECT_STREQ("two", (LPCTSTR)l.GetAt(p1));
    EXPECT_EQ((POSITION)nullptr, l.FindIndex(99));

    l.InsertAfter(p, "two-and-a-half");
    l.InsertBefore(l.Find("one"), "zero");
    std::string all;
    for (POSITION pos = l.GetHeadPosition(); pos; )
        all += std::string((LPCTSTR)l.GetNext(pos)) + ",";
    EXPECT_STREQ("zero,one,two,two-and-a-half,three,", all.c_str());

    l.RemoveAt(l.Find("two"));
    EXPECT_EQ((POSITION)nullptr, l.Find("two"));
    EXPECT_EQ(4, l.GetCount());

    // MFC's Find searches strictly AFTER the position handed to it, so a caller
    // can walk the duplicates rather than being handed the first one forever.
    CStringList dups;
    dups.AddTail("x");
    POSITION second = dups.AddTail("x");
    POSITION first  = dups.GetHeadPosition();
    EXPECT_EQ(second, dups.Find("x", first));
    EXPECT_EQ((POSITION)nullptr, dups.Find("x", second));
}

TEST(Collections, StringListPositionsSurviveOtherRemovals) {
    // The MFC contract the node rewrite exists for: a POSITION is a stable
    // handle to ITS element, unaffected by removals elsewhere.
    CStringList l;
    POSITION marks[5];
    const char* words[5] = { "a", "b", "c", "d", "e" };
    for (int i = 0; i < 5; i++) marks[i] = l.AddTail(words[i]);

    l.RemoveAt(marks[1]);
    l.RemoveHead();                    // "a"
    l.RemoveTail();                    // "e"
    EXPECT_EQ(2, l.GetCount());
    EXPECT_STREQ("c", (LPCTSTR)l.GetAt(marks[2]));
    EXPECT_STREQ("d", (LPCTSTR)l.GetAt(marks[3]));

    // A NULL POSITION resolves to an empty string rather than reading off the
    // end of an empty list (the old not-found path was m_data.back(), UB).
    CStringList empty;
    EXPECT_TRUE(empty.GetAt(nullptr).IsEmpty());
    EXPECT_TRUE(empty.GetHead().IsEmpty());
    EXPECT_TRUE(empty.GetTail().IsEmpty());
    EXPECT_TRUE(empty.RemoveHead().IsEmpty());
    EXPECT_TRUE(empty.RemoveTail().IsEmpty());
    POSITION p = nullptr;
    EXPECT_TRUE(empty.GetNext(p).IsEmpty());
    EXPECT_TRUE(empty.GetPrev(p).IsEmpty());
}

TEST(Collections, StringListCopiesDeeply) {
    // The std::list member gave copy/assign for free; owned nodes do not, and a
    // shallow copy would double-free at the second destructor.
    CStringList src;
    src.AddTail("one");
    src.AddTail("two");

    CStringList copy(src);
    EXPECT_EQ(2, copy.GetCount());
    EXPECT_NE(src.GetHeadPosition(), copy.GetHeadPosition());   // own nodes
    copy.AddTail("three");
    EXPECT_EQ(2, src.GetCount());                               // src untouched

    CStringList assigned;
    assigned.AddTail("junk");
    assigned = src;
    EXPECT_EQ(2, assigned.GetCount());
    EXPECT_STREQ("one", (LPCTSTR)assigned.GetHead());
    CStringList& same = assigned;   // through a reference: clang flags a literal x = x
    assigned = same;                                            // self-assign
    EXPECT_EQ(2, assigned.GetCount());
}

namespace {
// Wall-clock milliseconds for a full head-to-tail walk of `n` entries.
double string_list_walk_ms(int n)
{
    CStringList l;
    for (int i = 0; i < n; i++) l.AddTail("entry");

    auto t0 = std::chrono::steady_clock::now();
    long chars = 0;
    for (POSITION pos = l.GetHeadPosition(); pos; )
        chars += l.GetNext(pos).GetLength();
    auto t1 = std::chrono::steady_clock::now();

    // Consume the result so the walk cannot be optimised away -- and so a walk
    // that returns nothing fails rather than looking fast.
    EXPECT_TRUE(chars == (long) n * 5);
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}
} // namespace

TEST(Collections, StringListFullWalkIsLinearNotQuadratic) {
    // CStringList used to hand out element addresses and then map each one back
    // to its ITERATOR by scanning, so every cursor step was O(N) and a full walk
    // O(N^2) -- the K-784 defect, fixed for CTypedPtrList in 5cb6681 but left
    // here.  The list that pays is m_strMapList, which CalculateSpeedWalkString
    // walks in full and which grows unbounded across a mapping session.
    //
    // 4x the entries against a generous 8x time budget (plus a floor, so timer
    // noise on a tiny baseline cannot fail it): linear is ~4x, quadratic ~16x.
    string_list_walk_ms(1000);                        // warm caches/allocator
    const double small = string_list_walk_ms(4000);
    const double large = string_list_walk_ms(16000);

    const double budget = std::max(small * 8.0, 5.0);
    EXPECT_TRUE(large < budget);
    if (large >= budget)
        printf("    walk(4000)=%.2fms walk(16000)=%.2fms budget=%.2fms\n",
               small, large, budget);
}

TEST(Collections, StringArray) {
    CStringArray a;
    EXPECT_TRUE(a.IsEmpty());
    EXPECT_EQ(0, a.Add("x"));
    EXPECT_EQ(1, a.Add(CString("y")));
    EXPECT_EQ(2, a.GetSize());
    EXPECT_EQ(2, a.GetCount());
    EXPECT_STREQ("x", (LPCTSTR)a.GetAt(0));
    EXPECT_STREQ("y", (LPCTSTR)a[1]);
    a.SetAt(0, "z");
    EXPECT_STREQ("z", (LPCTSTR)a[0]);
    a.SetSize(4);
    EXPECT_EQ(4, a.GetSize());
    EXPECT_STREQ("", (LPCTSTR)a[3]);
    const CStringArray& ca = a;
    EXPECT_STREQ("z", (LPCTSTR)ca.GetAt(0));
    EXPECT_STREQ("z", (LPCTSTR)ca[0]);
    a.RemoveAll();
    EXPECT_TRUE(a.IsEmpty());
}

TEST(Collections, ArrayTemplate) {
    CArray<int> a;
    EXPECT_TRUE(a.IsEmpty());
    a.Add(10);
    a.Add(20);
    EXPECT_EQ(2, a.GetSize());
    EXPECT_EQ(2, a.GetCount());
    EXPECT_EQ(10, a.GetAt(0));
    EXPECT_EQ(20, a[1]);
    a[0] = 11;
    EXPECT_EQ(11, *a.GetData());
    const CArray<int>& ca = a;
    EXPECT_EQ(11, ca.GetAt(0));
    EXPECT_EQ(20, ca[1]);
    EXPECT_EQ(11, *ca.GetData());
    a.SetSize(5);
    EXPECT_EQ(5, a.GetSize());
    a.RemoveAll();
    EXPECT_TRUE(a.IsEmpty());
}

TEST(Collections, MapStringToString) {
    CMapStringToString m;
    EXPECT_TRUE(m.IsEmpty());
    m.SetAt("k1", "v1");
    m.SetAt(CString("k2"), CString("v2"));
    m["k3"] = "v3";
    EXPECT_EQ(3, m.GetCount());
    CString v;
    EXPECT_TRUE(m.Lookup("k1", v));
    EXPECT_STREQ("v1", (LPCTSTR)v);
    EXPECT_TRUE(m.Lookup(CString("k2"), v));
    EXPECT_STREQ("v2", (LPCTSTR)v);
    EXPECT_FALSE(m.Lookup("nope", v));
    EXPECT_TRUE(m.RemoveKey("k1"));
    EXPECT_FALSE(m.RemoveKey("k1"));
    EXPECT_TRUE(m.RemoveKey(CString("k2")));

    // full iteration via GetStartPosition / GetNextAssoc
    m.RemoveAll();
    m.SetAt("a", "1");
    m.SetAt("b", "2");
    int count = 0;
    std::string seen;
    CString key, val;
    for (POSITION pos = m.GetStartPosition(); pos; ) {
        m.GetNextAssoc(pos, key, val);
        seen += std::string((LPCTSTR)key) + "=" + (LPCTSTR)val + ";";
        ++count;
    }
    EXPECT_EQ(2, count);
    EXPECT_STREQ("a=1;b=2;", seen.c_str());   // std::map iterates in key order
    m.RemoveAll();
    EXPECT_EQ((POSITION)nullptr, m.GetStartPosition());
}

TEST(Collections, UIntArray) {
    CUIntArray a;
    a.Add(3);
    a.Add(7);
    EXPECT_EQ(2, a.GetSize());
    EXPECT_EQ(2, a.GetCount());
    EXPECT_EQ(3u, a.GetAt(0));
    EXPECT_EQ(7u, a[1]);
    a.SetAt(0, 5);
    EXPECT_EQ(5u, a[0]);
    a[1] = 9;
    const CUIntArray& ca = a;
    EXPECT_EQ(9u, ca[1]);
    a.SetSize(4);
    EXPECT_EQ(4, a.GetSize());
    a.RemoveAll();
    EXPECT_EQ(0, a.GetSize());
}

TEST(Collections, ListTemplate) {
    CList<int> l;
    EXPECT_TRUE(l.IsEmpty());
    l.AddTail(2);
    l.AddTail(3);
    l.AddHead(1);
    EXPECT_EQ(3, l.GetCount());
    EXPECT_EQ(1, l.GetHead());
    EXPECT_EQ(3, l.GetTail());
    std::string order;
    for (POSITION pos = l.GetHeadPosition(); pos; ) {
        char buf[8]; snprintf(buf, sizeof buf, "%d", l.GetNext(pos));
        order += buf;
    }
    EXPECT_STREQ("123", order.c_str());
    EXPECT_EQ(1, l.RemoveHead());
    EXPECT_EQ(3, l.RemoveTail());
    EXPECT_EQ(1, l.GetCount());
    EXPECT_NE((POSITION)nullptr, l.FindIndex(0));
    EXPECT_EQ((POSITION)nullptr, l.FindIndex(5));
    l.RemoveAll();
    EXPECT_TRUE(l.IsEmpty());
    EXPECT_EQ(0, l.RemoveHead());   // empty-list guard returns TYPE{}
    EXPECT_EQ(0, l.RemoveTail());
}

// Find was `return nullptr` and four siblings answered plausibly-wrong things,
// which is why the Chat List dialog lost the user's selection and focus on every
// refresh: CChatListDlg asks Find whether each chat id was selected before, and
// the answer was always "no".
TEST(Collections, ListTemplateFindsAndAddressesItsElements) {
    CList<int> l;
    l.AddTail(1);
    l.AddTail(2);
    l.AddTail(3);

    EXPECT_EQ(l.FindIndex(2), l.Find(3));
    EXPECT_NE((POSITION)nullptr, l.Find(3));
    EXPECT_EQ(l.FindIndex(0), l.Find(1));
    EXPECT_EQ((POSITION)nullptr, l.Find(99));

    // GetTailPosition used to answer GetHeadPosition, and GetPrev used to run
    // FORWARDS, so a tail-to-head walk visited exactly one element.
    std::string back;
    for (POSITION pos = l.GetTailPosition(); pos; ) {
        char buf[8]; snprintf(buf, sizeof buf, "%d", l.GetPrev(pos));
        back += buf;
    }
    EXPECT_STREQ("321", back.c_str());

    // GetAt returned a static dummy whatever it was handed.
    EXPECT_EQ(2, l.GetAt(l.FindIndex(1)));
    EXPECT_EQ(0, l.GetAt(nullptr));           // out of range: the empty answer

    // InsertBefore/InsertAfter used to be AddHead/AddTail, so both ignored the
    // position entirely.
    l.InsertAfter(l.Find(1), 15);
    l.InsertBefore(l.Find(3), 25);
    std::string order;
    for (POSITION pos = l.GetHeadPosition(); pos; ) {
        char buf[8]; snprintf(buf, sizeof buf, "%d,", l.GetNext(pos));
        order += buf;
    }
    EXPECT_STREQ("1,15,2,25,3,", order.c_str());

    // RemoveAt was a no-op.
    l.RemoveAt(l.Find(25));
    EXPECT_EQ(4, l.GetCount());
    EXPECT_EQ((POSITION)nullptr, l.Find(25));

    // MFC searches strictly AFTER the given position.
    CList<int> dups;
    POSITION a = dups.AddTail(7);
    POSITION b = dups.AddTail(7);
    EXPECT_EQ(b, dups.Find(7, a));
    EXPECT_EQ((POSITION)nullptr, dups.Find(7, b));
}

// ═══════════════════════════════════════════════════════════════════════════
// Files & archives: CFile / CStdioFile / CMemFile / CArchive
// ═══════════════════════════════════════════════════════════════════════════

static std::string temp_path(const char* leaf) {
    char buf[256];
    snprintf(buf, sizeof buf, "/tmp/mc_test_mfcui_%d_%s", (int)getpid(), leaf);
    return buf;
}

TEST(Files, CFileWriteReadRoundtrip) {
    std::string path = temp_path("roundtrip.bin");
    {
        CFile f;
        ASSERT_TRUE(f.Open(path.c_str(), CFile::modeCreate | CFile::modeWrite));
        EXPECT_TRUE(f.IsOpen());
        f.Write("hello world", 11);
        EXPECT_EQ((DWORD)11, f.GetPosition());
        f.Flush();
        EXPECT_EQ((DWORD)11, f.GetLength());
        // GetLength must preserve the current position
        EXPECT_EQ((DWORD)11, f.GetPosition());
        f.Close();
        EXPECT_FALSE(f.IsOpen());
    }
    {
        CFile f(path.c_str(), CFile::modeRead);
        ASSERT_TRUE(f.IsOpen());
        char buf[32] = {};
        EXPECT_EQ((UINT)11, f.Read(buf, sizeof buf));
        EXPECT_STREQ("hello world", buf);
        f.SeekToBegin();
        EXPECT_EQ((DWORD)0, f.GetPosition());
        EXPECT_EQ((LONG)5, f.Seek(5, CFile::begin));
        EXPECT_EQ((LONG)7, f.Seek(2, CFile::current));
        EXPECT_EQ((LONG)10, f.Seek(-1, CFile::end));
        f.SeekToEnd();
        EXPECT_EQ((DWORD)11, f.GetPosition());
    }
    CFile::Remove(path.c_str());
}

TEST(Files, CFileOpenMissingFails) {
    CFile f;
    EXPECT_FALSE(f.Open("/nonexistent/dir/nope.txt", CFile::modeRead));
    EXPECT_FALSE(f.IsOpen());
    // 3-arg Open (MFC): returns FALSE and FILLS the caller's CFileException
    // (does not throw)
    CFileException fe;
    EXPECT_FALSE(f.Open("/nonexistent/dir/nope.txt", CFile::modeRead, &fe));
    EXPECT_EQ((int)CFileException::fileNotFound, fe.m_cause);
    EXPECT_EQ((LONG)ENOENT, fe.m_lOsError);
    EXPECT_STREQ("/nonexistent/dir/nope.txt", (LPCTSTR)fe.m_strFileName);
    // Constructor form is MFC-faithful: THROWS a heap CFileException* on
    // failure (round 5; it used to silently leave the file unopened)
    bool threw = false;
    try {
        CFile g("/nonexistent/dir/nope.txt", CFile::modeRead);
    } catch (CFileException* e) {
        threw = true;
        EXPECT_EQ((int)CFileException::fileNotFound, e->m_cause);
        EXPECT_EQ((LONG)ENOENT, e->m_lOsError);
        char msg[256] = "";
        EXPECT_TRUE(e->GetErrorMessage(msg, sizeof msg));
        EXPECT_TRUE(strstr(msg, "nope.txt") != nullptr);   // names the file
        e->Delete();
    }
    EXPECT_TRUE(threw);
    // CStdioFile's path constructor inherits the throw
    threw = false;
    try {
        CStdioFile s("/nonexistent/dir/nope.txt", CFile::modeRead);
    } catch (CFileException* e) { threw = true; e->Delete(); }
    EXPECT_TRUE(threw);
    // plain modeWrite (no modeCreate) opens EXISTING files only ("r+b")
    CFile h;
    EXPECT_FALSE(h.Open(temp_path("never-created.bin").c_str(), CFile::modeWrite));
    // Read/Write on a closed CFile are no-ops
    char buf[4];
    EXPECT_EQ((UINT)0, f.Read(buf, 4));
    f.Write(buf, 4);
    EXPECT_EQ((DWORD)0, f.GetLength());
    EXPECT_EQ((DWORD)0, f.GetPosition());
    EXPECT_EQ((LONG)0, f.Seek(10, CFile::begin));
}

// ── I/O failures throw (2026-08-12) ─────────────────────────────────────────
// Until then CFile::Write/Flush/Close swallowed every stdio error, so a save
// to a full disk reported SUCCESS: the hooks cleared the modified flag, the
// close-time prompt was suppressed, and a truncated .mcl silently replaced a
// good one.  MFC's contract — which plugins.cpp SaveState and OnSaveDocument's
// catch were already written against — is a thrown CFileException*.

namespace {
// m_fp is protected; the failure injection needs the raw stream.
struct ExposedFile : public CFile {
    FILE* fp() { return m_fp; }
};
}

TEST(Files, WriteFailureThrowsFileException) {
    std::string path = temp_path("writefail.bin");
    ExposedFile f;
    ASSERT_TRUE(f.Open(path.c_str(), CFile::modeCreate | CFile::modeWrite));
    setvbuf(f.fp(), nullptr, _IONBF, 0);   // unbuffered: fail AT the fwrite
    close(fileno(f.fp()));                 // yank the fd out from under stdio
    bool threw = false;
    try { f.Write("x", 1); }
    catch (CFileException* e) {
        threw = true;
        EXPECT_EQ((LONG)EBADF, e->m_lOsError);
        e->Delete();
    }
    EXPECT_TRUE(threw);
    f.Abort();                             // Abort never throws, even now
    EXPECT_FALSE(f.IsOpen());
    CFile::Remove(path.c_str());
}

TEST(Files, CloseFailureThrowsButOnlyForWriteFiles) {
    // A buffered write "succeeds" into stdio's buffer; the error surfaces at
    // fclose — exactly the ENOSPC shape.  Close must throw AND still leave
    // the file closed.
    std::string path = temp_path("closefail.bin");
    {
        ExposedFile f;
        ASSERT_TRUE(f.Open(path.c_str(), CFile::modeCreate | CFile::modeWrite));
        f.Write("buffered", 8);            // lands in the buffer, no error yet
        close(fileno(f.fp()));
        bool threw = false;
        try { f.Close(); }
        catch (CFileException* e) { threw = true; e->Delete(); }
        EXPECT_TRUE(threw);
        EXPECT_FALSE(f.IsOpen());          // closed even on the throwing path
    }
    // A READ file's Close never throws, even with a pending stream error:
    // OnOpenDocument calls Close() outside its catch, and a reader has
    // nothing to lose.  (Failure injection: kill the fd, then read.)
    {
        FILE* seed = fopen(path.c_str(), "wb");
        ASSERT_TRUE(seed != nullptr);
        fputs("data", seed);
        fclose(seed);
        ExposedFile r;
        ASSERT_TRUE(r.Open(path.c_str(), CFile::modeRead));
        setvbuf(r.fp(), nullptr, _IONBF, 0);
        close(fileno(r.fp()));
        char buf[4];
        EXPECT_EQ((UINT)0, r.Read(buf, 4));  // fails, sets ferror
        bool threw = false;
        try { r.Close(); }
        catch (CFileException* e) { threw = true; e->Delete(); }
        EXPECT_FALSE(threw);
        EXPECT_FALSE(r.IsOpen());
    }
    CFile::Remove(path.c_str());
}

TEST(Files, OnSaveDocumentReportsWriteFailure) {
    // End to end through the save machinery: Serialize → CArchive::WriteString
    // → CFile::Write, against a REAL failing filesystem write.  RLIMIT_FSIZE=0
    // forbids file growth (the kernel's own "disk full"); SIGXFSZ must be
    // ignored or the process is killed instead of the write failing.
    struct BigDoc : public CDocument {
        void Serialize(CArchive& ar) override {
            std::string line(1024, 'x');
            for (int i = 0; i < 256; i++)      // 256 KB — far past any buffer
                ar.WriteString(line.c_str());
        }
    };
    std::string path = temp_path("savefail.mcl");

    struct rlimit old_rl;
    ASSERT_EQ(0, getrlimit(RLIMIT_FSIZE, &old_rl));
    void (*old_sig)(int) = signal(SIGXFSZ, SIG_IGN);
    struct rlimit rl = old_rl;
    rl.rlim_cur = 0;
    ASSERT_EQ(0, setrlimit(RLIMIT_FSIZE, &rl));

    BigDoc doc;
    BOOL ok = doc.OnSaveDocument(path.c_str());

    setrlimit(RLIMIT_FSIZE, &old_rl);
    signal(SIGXFSZ, old_sig);

    EXPECT_FALSE(ok);                      // the save FAILED and said so

    // With the limit restored the identical save succeeds — proving both the
    // restore and that the throwing path did not wedge the machinery.
    EXPECT_TRUE(doc.OnSaveDocument(path.c_str()));
    CFile check;
    ASSERT_TRUE(check.Open(path.c_str(), CFile::modeRead));
    EXPECT_EQ((DWORD)(256 * 1024), check.GetLength());
    check.Close();
    CFile::Remove(path.c_str());
}

TEST(Files, CArchiveCloseFlushesAndReportsWriteFailure) {
    // The CPlugin::SaveState / variable-export / Save_Set shape: a SMALL store
    // archive whose writes all fit in one stdio buffer, so the only place a
    // full-disk failure can be caught is ar.Close()'s flush.  Before the shim
    // forwarded that flush, CArchive::Close() was a no-op: the bytes sat in the
    // buffer, the export declared success, and the only (unchecked) flush was
    // ~CFile's fclose -- a truncated file reported as a good save.  Contrast the
    // test above, whose 256 KB overflows the buffer and so fails at an
    // intermediate CFile::Write even with a no-op Close().
    std::string path = temp_path("archivewritefail.txt");

    struct rlimit old_rl;
    ASSERT_EQ(0, getrlimit(RLIMIT_FSIZE, &old_rl));
    void (*old_sig)(int) = signal(SIGXFSZ, SIG_IGN);
    struct rlimit rl = old_rl;
    rl.rlim_cur = 0;                       // the kernel's own "disk full"
    ASSERT_EQ(0, setrlimit(RLIMIT_FSIZE, &rl));

    bool threw = false;
    {
        CFile f;
        // modeCreate truncates to 0 (allowed at rlim 0); the write is what fails.
        ASSERT_TRUE(f.Open(path.c_str(),
                           CFile::modeCreate | CFile::modeReadWrite));
        CArchive ar(&f, CArchive::store);
        ar.WriteString("a few hundred bytes of variable state\r\n");  // < 1 buffer
        try {
            ar.Close();                   // must FLUSH -> throw on the full disk
        }
        catch (CFileException* e) { threw = true; e->Delete(); }
    }

    setrlimit(RLIMIT_FSIZE, &old_rl);
    signal(SIGXFSZ, old_sig);

    EXPECT_TRUE(threw);                    // reported, not swallowed

    CFile::Remove(path.c_str());
}

TEST(Files, CFileBackslashPathsAreNormalized) {
    // MUSHclient embeds Windows separators; the shim maps '\' -> '/'.
    std::string dir = temp_path("bsdir");
    mkdir(dir.c_str(), 0755);
    std::string winPath = dir + "\\file.txt";
    {
        CFile f;
        ASSERT_TRUE(f.Open(winPath.c_str(), CFile::modeCreate | CFile::modeWrite));
        f.Write("x", 1);
    }
    CFile g;
    EXPECT_TRUE(g.Open((dir + "/file.txt").c_str(), CFile::modeRead));
    g.Close();
    CFile::Remove((dir + "/file.txt").c_str());
    rmdir(dir.c_str());
}

TEST(Files, CFileNamePathAccessors) {
    std::string path = temp_path("names.txt");
    CFile f;
    ASSERT_TRUE(f.Open(path.c_str(), CFile::modeCreate | CFile::modeWrite));
    CString name = f.GetFileName();
    EXPECT_TRUE(strstr((LPCTSTR)name, "names.txt") != nullptr);
    EXPECT_TRUE(strchr((LPCTSTR)name, '/') == nullptr);   // leaf only
    EXPECT_STREQ(path.c_str(), (LPCTSTR)f.GetFilePath());
    f.SetFilePath("/other/place.txt");
    EXPECT_STREQ("place.txt", (LPCTSTR)f.GetFileName());
    f.Close();
    CFile::Remove(path.c_str());
}

TEST(Files, CFileRenameRemoveStatus) {
    std::string a = temp_path("stat_a.txt"), b = temp_path("stat_b.txt");
    {
        CFile f;
        ASSERT_TRUE(f.Open(a.c_str(), CFile::modeCreate | CFile::modeWrite));
        f.Write("12345", 5);
    }
    EXPECT_TRUE(CFile::Rename(a.c_str(), b.c_str()));
    CFileStatus st;
    EXPECT_TRUE(CFile::GetStatus(b.c_str(), st));
    EXPECT_EQ((LONG)5, st.m_size);
    EXPECT_EQ(0, (int)st.m_attribute);            // plain writable file
    EXPECT_FALSE(CFile::GetStatus(a.c_str(), st)); // renamed away
    EXPECT_FALSE(CFile::GetStatus(nullptr, st));
    // directory attribute
    EXPECT_TRUE(CFile::GetStatus("/tmp", st));
    EXPECT_EQ(0x10, (int)st.m_attribute);
    EXPECT_TRUE(CFile::Remove(b.c_str()));
    EXPECT_FALSE(CFile::Remove(b.c_str()));
}

TEST(Files, CStdioFileReadWriteString) {
    std::string path = temp_path("stdio.txt");
    {
        CStdioFile f(path.c_str(), CFile::modeCreate | CFile::modeWrite);
        ASSERT_TRUE(f.IsOpen());
        f.WriteString("line one\n");
        f.WriteString("line two");   // no trailing newline
    }
    {
        CStdioFile f(path.c_str(), CFile::modeRead);
        ASSERT_TRUE(f.IsOpen());
        CString s;
        EXPECT_TRUE(f.ReadString(s));
        EXPECT_STREQ("line one", (LPCTSTR)s);
        EXPECT_TRUE(f.ReadString(s));       // last line, no '\n': still true
        EXPECT_STREQ("line two", (LPCTSTR)s);
        EXPECT_FALSE(f.ReadString(s));      // EOF
    }
    {
        // buffer variant goes through fgets (retains the newline)
        CStdioFile f(path.c_str(), CFile::modeRead);
        char buf[64];
        EXPECT_TRUE(f.ReadString(buf, sizeof buf));
        EXPECT_STREQ("line one\n", buf);
        EXPECT_FALSE(f.ReadString(buf, 0));   // zero-size guard
    }
    CFile::Remove(path.c_str());
    CStdioFile closed;
    CString s;
    EXPECT_FALSE(closed.ReadString(s));
}

// Windows reads a CFile::typeText / fopen("r") file through the CRT's text
// mode, which turns every CRLF into a lone LF before the caller sees it.  POSIX
// has no such mode, so the shim has to do it: without this, the notepad, the
// name generator, the variables import and -- the one caller that does not
// defensively TrimRight -- Tip of the Day all saw a trailing '\r' the Windows
// build never sees.  tips.txt itself ships CRLF.
//
// LF-only input is the control: it must come back byte-identical, so a fix that
// merely chopped the last character would fail here.
TEST(Files, TextModeReadsCollapseCrlfLikeTheCrtDoes) {
    std::string path = temp_path("crlf.txt");
    {
        FILE* f = fopen(path.c_str(), "wb");        // raw bytes, exactly as shipped
        ASSERT_TRUE(f != nullptr);
        fputs("line one\r\nline two\r\n\r\nlf only\nbare\rcr\r\n", f);
        fclose(f);
    }
    {
        CStdioFile f(path.c_str(), CFile::modeRead);
        ASSERT_TRUE(f.IsOpen());
        CString s;
        EXPECT_TRUE(f.ReadString(s));
        EXPECT_STREQ("line one", (LPCTSTR)s);
        EXPECT_TRUE(f.ReadString(s));
        EXPECT_STREQ("line two", (LPCTSTR)s);
        EXPECT_TRUE(f.ReadString(s));               // a CRLF blank line is BLANK
        EXPECT_STREQ("", (LPCTSTR)s);
        EXPECT_TRUE(f.ReadString(s));               // LF-only line: unchanged
        EXPECT_STREQ("lf only", (LPCTSTR)s);
        EXPECT_TRUE(f.ReadString(s));               // a lone '\r' mid-line stays,
        EXPECT_STREQ("bare\rcr", (LPCTSTR)s);       // exactly as on Windows
    }
    {
        // Buffer variant: MFC keeps the newline here, so the '\r' must go and
        // the '\n' must stay -- "line one\n", not "line one\r\n" and not
        // "line one".
        CStdioFile f(path.c_str(), CFile::modeRead);
        ASSERT_TRUE(f.IsOpen());
        char buf[64];
        EXPECT_TRUE(f.ReadString(buf, sizeof buf));
        EXPECT_STREQ("line one\n", buf);
        EXPECT_TRUE(f.ReadString(buf, sizeof buf));
        EXPECT_STREQ("line two\n", buf);
        EXPECT_TRUE(f.ReadString(buf, sizeof buf));
        EXPECT_STREQ("\n", buf);
    }
    {
        // _fgetts is the alias CTipDlg::GetNextTipString reads its lines with,
        // straight off a raw fopen(path, "r") FILE* -- not a CStdioFile.
        FILE* fp = fopen(path.c_str(), "r");
        ASSERT_TRUE(fp != nullptr);
        char buf[64];
        // Boolean forms, not EXPECT_EQ/ASSERT_NE: mc_test's describe() streams a
        // const char* into an ostringstream, and libc++ strlen()s a null one, so
        // a FAILING comparison against a null char* crashes inside the report.
        ASSERT_TRUE(_fgetts(buf, (int)sizeof buf, fp) != nullptr);
        EXPECT_STREQ("line one\n", buf);
        EXPECT_TRUE(strchr(buf, '\r') == nullptr);
        ASSERT_TRUE(_fgetts(buf, (int)sizeof buf, fp) != nullptr);
        ASSERT_TRUE(_fgetts(buf, (int)sizeof buf, fp) != nullptr);
        // TipDlg.cpp:139 decides "blank or comment" from the FIRST byte, so a
        // CRLF blank line has to arrive with '\n' there, not '\r'.
        EXPECT_EQ('\n', buf[0]);
        fclose(fp);
    }
    CFile::Remove(path.c_str());
}

TEST(Files, CMemFileWritableGrowsAndDetaches) {
    CMemFile mf;
    EXPECT_EQ((DWORD)0, mf.GetLength());
    EXPECT_EQ((BYTE*)nullptr, mf.GetMemoryPos());   // empty owned buffer
    mf.Write("abcdef", 6);
    EXPECT_EQ((DWORD)6, mf.GetLength());
    mf.SeekToBegin();
    char buf[8] = {};
    EXPECT_EQ((UINT)3, mf.Read(buf, 3));
    EXPECT_STREQ("abc", buf);
    EXPECT_EQ((DWORD)3, mf.GetPosition());
    // Compared by length, not as a C string: Write() stored exactly the six
    // bytes it was given and CMemFile appends no terminator, so strcmp would
    // run off the end of the buffer.  ASan reports that as a
    // heap-buffer-overflow READ 0 bytes after the 6-byte region; without a
    // sanitizer it silently reads whatever the allocator left there.
    EXPECT_EQ(0, memcmp("def", mf.GetMemoryPos(), 3));
    // Read past end is clamped
    char big[16] = {};
    EXPECT_EQ((UINT)3, mf.Read(big, 16));
    // Seek clamping
    EXPECT_EQ((LONG)0, mf.Seek(-100, CMemFile::begin));
    EXPECT_EQ((LONG)6, mf.Seek(100, CMemFile::begin));
    EXPECT_EQ((LONG)4, mf.Seek(-2, CMemFile::end));
    EXPECT_EQ((LONG)5, mf.Seek(1, CMemFile::current));
    mf.SeekToEnd();
    EXPECT_EQ((DWORD)6, mf.GetPosition());
    // Detach: malloc'd, NUL-terminated copy; file resets
    BYTE* p = mf.Detach();
    ASSERT_TRUE(p != nullptr);
    EXPECT_STREQ("abcdef", (const char*)p);
    free(p);
    EXPECT_EQ((DWORD)0, mf.GetLength());
    mf.Close();   // no-op
    mf.Flush();   // no-op
}

TEST(Files, CMemFileReadOnlyWrapsExternalBuffer) {
    BYTE data[] = "wrapped";
    CMemFile mf(data, 7);
    EXPECT_EQ((DWORD)7, mf.GetLength());
    mf.Write("IGNORED", 7);              // read-only: silently dropped
    EXPECT_EQ((DWORD)7, mf.GetLength());
    char buf[8] = {};
    EXPECT_EQ((UINT)7, mf.Read(buf, 7));
    EXPECT_STREQ("wrapped", buf);
    EXPECT_EQ((BYTE*)nullptr, mf.Detach());   // never detaches external buffers
    mf.SeekToBegin();
    EXPECT_EQ(data, mf.GetMemoryPos());
}

TEST(Files, CArchiveStringIO) {
    std::string path = temp_path("archive.txt");
    {
        CFile f;
        ASSERT_TRUE(f.Open(path.c_str(), CFile::modeCreate | CFile::modeWrite));
        CArchive ar(&f, CArchive::store);
        EXPECT_TRUE(ar.IsStoring());
        EXPECT_FALSE(ar.IsLoading());
        EXPECT_EQ(&f, ar.GetFile());
        ar.WriteString("first\r\n");
        ar.WriteString("second\n");
        ar.Write("!", 1);
        ar.Flush();
        ar.Close();
    }
    {
        CFile f;
        ASSERT_TRUE(f.Open(path.c_str(), CFile::modeRead));
        CArchive ar(&f, CArchive::load);
        EXPECT_TRUE(ar.IsLoading());
        CString s;
        EXPECT_TRUE(ar.ReadString(s));
        EXPECT_STREQ("first", (LPCTSTR)s);     // CR stripped, LF terminates
        char buf[16];
        EXPECT_TRUE(ar.ReadString(buf, sizeof buf));
        EXPECT_STREQ("second", buf);
        EXPECT_TRUE(ar.ReadString(s));          // trailing "!" with no newline
        EXPECT_STREQ("!", (LPCTSTR)s);
        EXPECT_FALSE(ar.ReadString(s));         // EOF
        char raw[4] = {};
        ar.Read(raw, 2);                        // EOF: reads nothing, no crash
    }
    // Null-file archive guards
    CArchive nullAr(nullptr, CArchive::load);
    CString s;
    EXPECT_FALSE(nullAr.ReadString(s));
    char buf[4];
    EXPECT_FALSE(nullAr.ReadString(buf, sizeof buf));
    nullAr.WriteString("dropped");
    CFile::Remove(path.c_str());
}

TEST(Files, CArchiveBinaryOperatorsAreInert) {
    // The << / >> operators are deliberate no-ops on the port (world files are
    // XML text); they must chain and not touch the file.  A null-file archive
    // cannot prove that -- "did nothing" and "there was nowhere to write
    // anyway" look identical -- so the store half runs against a REAL temp
    // file and asserts it is still empty afterwards.
    std::string path = temp_path("archive_binary_inert.bin");
    CFile::Remove(path.c_str());   // start clean; a stale file would pass vacuously

    {
        CFile f;
        ASSERT_TRUE(f.Open(path.c_str(), CFile::modeCreate | CFile::modeWrite));
        CArchive ar(&f, CArchive::store);
        ar << (BYTE)1 << (WORD)2 << (DWORD)3 << (int)4 << (long)5
           << 1.5f << 2.5 << true << CString("s");
        POINT pt = {}; RECT rc = {};
        ar << pt << rc << (const CObject*)nullptr;
        f.Close();
    }
    {
        CFile check;
        ASSERT_TRUE(check.Open(path.c_str(), CFile::modeRead));
        EXPECT_EQ((DWORD)0, check.GetLength());   // the << chain wrote nothing
        check.Close();
    }
    CFile::Remove(path.c_str());

    // Extraction: seed every local with a sentinel before the >> chain, and
    // assert each sentinel SURVIVED unchanged -- a >> that actually read
    // through the (empty/null) archive would leave some of these zeroed or
    // blanked instead, which today's bare declarations could never catch.
    BYTE     b  = 0xAB;
    WORD     w  = 0xBEEF;
    DWORD    dw = 0xDEADBEEF;
    int      i  = -42;
    long     l  = -99;
    float    fl = 3.25f;
    double   d  = 6.5;
    bool     bo = true;
    CString  cs("sentinel");
    CObject* obj = (CObject*)0x1234;
    POINT    pt = { 7, 8 };
    RECT     rc = { 1, 2, 3, 4 };

    CArchive in(nullptr, CArchive::load);
    in >> b >> w >> dw >> i >> l >> fl >> d >> bo >> cs >> obj >> pt >> rc;

    EXPECT_EQ((BYTE)0xAB, b);
    EXPECT_EQ((WORD)0xBEEF, w);
    EXPECT_EQ((DWORD)0xDEADBEEF, dw);
    EXPECT_EQ(-42, i);
    EXPECT_EQ(-99L, l);
    EXPECT_EQ(3.25f, fl);
    EXPECT_EQ(6.5, d);
    EXPECT_TRUE(bo);
    EXPECT_STREQ("sentinel", (LPCTSTR)cs);
    EXPECT_EQ((CObject*)0x1234, obj);
    EXPECT_EQ(7L, pt.x);
    EXPECT_EQ(8L, pt.y);
    EXPECT_EQ(1L, rc.left);
    EXPECT_EQ(2L, rc.top);
    EXPECT_EQ(3L, rc.right);
    EXPECT_EQ(4L, rc.bottom);
}

// ═══════════════════════════════════════════════════════════════════════════
// CDocument / CDocTemplate
// ═══════════════════════════════════════════════════════════════════════════

namespace {
class SerializingDoc : public CDocument {
public:
    CString m_payload;
    bool    m_throwOnSerialize = false;
    void Serialize(CArchive& ar) override {
        if (m_throwOnSerialize)
            AfxThrowArchiveException(CArchiveException::badSchema, "");
        if (ar.IsStoring())
            ar.WriteString((LPCTSTR)m_payload);
        else
            ar.ReadString(m_payload);
    }
};
} // namespace

TEST(Document, OpenSaveRoundtripThroughSerialize) {
    std::string path = temp_path("doc.txt");
    SerializingDoc out;
    out.m_payload = "the payload";
    EXPECT_TRUE(out.OnSaveDocument(path.c_str()));
    SerializingDoc in;
    EXPECT_TRUE(in.OnOpenDocument(path.c_str()));
    EXPECT_STREQ("the payload", (LPCTSTR)in.m_payload);
    CFile::Remove(path.c_str());
}

TEST(Document, OpenMissingAndSaveUnwritableFail) {
    SerializingDoc doc;
    EXPECT_FALSE(doc.OnOpenDocument("/nonexistent/nowhere.txt"));
    EXPECT_FALSE(doc.OnSaveDocument("/nonexistent/dir/out.txt"));
}

// Real MFC's CDocument::OnSaveDocument ends with SetModifiedFlag(FALSE) on
// success; the shim used to omit it, so a save reached directly (bypassing
// the hand-written clear in hook_save_world) left the doc dirty even though
// the file was written.  A failed save must NOT clear it either way.
TEST(Document, OnSaveDocumentClearsModifiedFlagOnSuccessOnly) {
    std::string path = temp_path("doc_modflag.txt");
    SerializingDoc doc;
    doc.m_payload = "x";
    doc.SetModifiedFlag();
    EXPECT_TRUE(doc.IsModified());
    EXPECT_TRUE(doc.OnSaveDocument(path.c_str()));
    EXPECT_FALSE(doc.IsModified());
    CFile::Remove(path.c_str());

    doc.SetModifiedFlag();
    EXPECT_TRUE(doc.IsModified());
    EXPECT_FALSE(doc.OnSaveDocument("/nonexistent/dir/out.txt"));
    EXPECT_TRUE(doc.IsModified());
}

TEST(Document, SerializeExceptionIsCaught) {
    std::string path = temp_path("doc_throw.txt");
    SerializingDoc doc;
    doc.m_payload = "x";
    ASSERT_TRUE(doc.OnSaveDocument(path.c_str()));   // create the file first
    doc.m_throwOnSerialize = true;
    EXPECT_FALSE(doc.OnOpenDocument(path.c_str()));  // CException* caught -> FALSE
    EXPECT_FALSE(doc.OnSaveDocument(path.c_str()));
    CFile::Remove(path.c_str());
}

TEST(Document, TitlePathModifiedFlags) {
    CDocument d;
    d.SetTitle("My World");
    EXPECT_STREQ("My World", (LPCTSTR)d.GetTitle());
    d.SetPathName("/a/b.mcl");
    EXPECT_STREQ("/a/b.mcl", (LPCTSTR)d.GetPathName());
    EXPECT_FALSE(d.IsModified());
    d.SetModifiedFlag();
    EXPECT_TRUE(d.IsModified());
    d.SetChangedFlag(FALSE);
    EXPECT_FALSE(d.IsModified());
    EXPECT_TRUE(d.OnNewDocument());
    EXPECT_TRUE(d.SaveModified());
    EXPECT_EQ((CDocTemplate*)nullptr, d.GetDocTemplate());
    EXPECT_EQ((LPVOID)nullptr, d.GetIDispatch());
}

// CDocument::SaveModified was `return TRUE` — the answer a document with
// nothing to lose gives.  Cmd-Q / the app menu / the Dock reach it through
// CMUSHclientDoc::SaveModified (doc.cpp:2105), whose own prompts cover only the
// connection and the variables-only case, so a world with unsaved edits was
// discarded without a word.  (CWorldChildFrame::OnClose's prompt chain is the
// Cmd+W path and never comes through here, so this cannot double-ask.)
TEST(Document, SaveModifiedPromptsOnlyForADirtyDocument) {
    static int s_prompts = 0, s_answer = IDCANCEL;
    static std::string s_text;
    int (*const saved)(const char*, const char*, unsigned) = g_wxhook_message_box;
    g_wxhook_message_box = [](const char* text, const char*, unsigned) -> int {
        ++s_prompts; s_text = text ? text : ""; return s_answer;
    };

    // Clean document: no question at all.
    CDocument clean;
    clean.SetPathName("/a/b.mcl");
    EXPECT_TRUE(clean.SaveModified());
    EXPECT_EQ(0, s_prompts);

    // Dirty + Cancel: the close is refused.  The stub could never say FALSE.
    CDocument d;
    d.SetPathName("/a/b.mcl");
    d.SetModifiedFlag();
    s_answer = IDCANCEL;
    EXPECT_FALSE(d.SaveModified());
    EXPECT_EQ(1, s_prompts);
    EXPECT_TRUE(s_text.find("/a/b.mcl") != std::string::npos);  // names the doc

    // Dirty + No: discard and continue — asked exactly once, not once per view.
    s_answer = IDNO;
    EXPECT_TRUE(d.SaveModified());
    EXPECT_EQ(2, s_prompts);

    // With no path, MFC falls back to the caption.
    CDocument untitled;
    untitled.SetTitle("Fresh World");
    untitled.SetModifiedFlag();
    s_answer = IDNO;
    EXPECT_TRUE(untitled.SaveModified());
    EXPECT_TRUE(s_text.find("Fresh World") != std::string::npos);

    // Dirty + Yes: DoFileSave must really write.  It was `return TRUE`, so a
    // Yes answer reported success and produced no file.
    char path[256];
    snprintf(path, sizeof path, "/tmp/mc_test_savemod_%d.mcl", (int)getpid());
    unlink(path);
    CDocument writer;
    writer.SetPathName(path);
    writer.SetModifiedFlag();
    s_answer = IDYES;
    EXPECT_TRUE(writer.SaveModified());
    struct stat st;
    EXPECT_EQ(0, stat(path, &st));           // the file exists now
    unlink(path);

    // Yes with nowhere to write it: DoSave fails, so the close is refused
    // rather than silently losing the document.
    CDocument nowhere;
    nowhere.SetTitle("Nowhere");
    nowhere.SetModifiedFlag();
    s_answer = IDYES;
    EXPECT_FALSE(nowhere.SaveModified());

    g_wxhook_message_box = saved;
}

// MFC's CDocument::DoSave ends `if (bReplace) SetPathName(newName);` -- the
// FALSE arm is "Save Copy As" and must leave the document pointing at its
// original file.  The shim always re-pointed; harmless only because every
// world-side caller passes TRUE (the notepad's own lambda had the same bug
// with real consequences -- world.SaveNotepad's Lua default is FALSE).
TEST(Document, CopyAsDoesNotRetargetTheDocument) {
    char pathA[256], pathB[256];
    snprintf(pathA, sizeof pathA, "/tmp/mc_test_copyas_a_%d.mcl", (int)getpid());
    snprintf(pathB, sizeof pathB, "/tmp/mc_test_copyas_b_%d.mcl", (int)getpid());
    unlink(pathA);
    unlink(pathB);

    CDocument d;
    d.SetPathName(pathA);

    // FALSE: the file is written, the document is NOT re-pointed.
    EXPECT_TRUE(d.DoSave(pathB, FALSE));
    struct stat st;
    EXPECT_EQ(0, stat(pathB, &st));                       // the copy exists
    EXPECT_STREQ(pathA, (LPCTSTR)d.GetPathName());        // still the original

    // TRUE: the control -- Save As really does re-point, so a DoSave that
    // stopped assigning the path entirely cannot pass this test.
    EXPECT_TRUE(d.DoSave(pathB, TRUE));
    EXPECT_STREQ(pathB, (LPCTSTR)d.GetPathName());

    unlink(pathA);
    unlink(pathB);
}

TEST(Document, ViewListIteration) {
    CDocument d;
    EXPECT_EQ((POSITION)nullptr, d.GetFirstViewPosition());
    CView v1, v2;
    d.AddView(&v1);
    d.AddView(&v2);
    POSITION pos = d.GetFirstViewPosition();
    EXPECT_EQ(&v1, d.GetNextView(pos));
    EXPECT_EQ(&v2, d.GetNextView(pos));
    EXPECT_EQ((POSITION)nullptr, pos);
    EXPECT_EQ((CView*)nullptr, d.GetNextView(pos));   // exhausted
    d.RemoveView(&v1);
    pos = d.GetFirstViewPosition();
    EXPECT_EQ(&v2, d.GetNextView(pos));
    d.UpdateAllViews(nullptr);
}

TEST(Document, MultiDocTemplateRegistryAndIteration) {
    CMultiDocTemplate t(0, nullptr, nullptr, nullptr);
    EXPECT_EQ((POSITION)nullptr, t.GetFirstDocPosition());
    CDocument d1, d2;
    t.AddDocument(&d1);
    t.AddDocument(&d2);
    POSITION pos = t.GetFirstDocPosition();
    EXPECT_EQ(&d1, t.GetNextDoc(pos));
    EXPECT_EQ(&d2, t.GetNextDoc(pos));
    EXPECT_EQ((POSITION)nullptr, pos);
    EXPECT_EQ((CDocument*)nullptr, t.GetNextDoc(pos));
    t.RemoveDocument(&d1);
    pos = t.GetFirstDocPosition();
    EXPECT_EQ(&d2, t.GetNextDoc(pos));
    t.RemoveDocument(&d1);   // absent: no-op
}

static void* s_openedWorldFile;
static int   s_newWorldCalls;

TEST(Document, TemplateOpenDocumentFileRoutesThroughHooks) {
    CMultiDocTemplate t(0, nullptr, nullptr, nullptr);
    // No hooks -> nullptr both ways
    EXPECT_EQ((CDocument*)nullptr, t.OpenDocumentFile("/a.mcl"));
    EXPECT_EQ((CDocument*)nullptr, t.OpenDocumentFile(nullptr));

    static CDocument fakeDoc;
    s_openedWorldFile = nullptr;
    s_newWorldCalls = 0;
    g_wxhook_open_world_file = [](const char* path) -> void* {
        s_openedWorldFile = (void*)path;
        return &fakeDoc;
    };
    g_wxhook_new_world = []() -> void* { ++s_newWorldCalls; return &fakeDoc; };
    EXPECT_EQ(&fakeDoc, t.OpenDocumentFile("/b.mcl"));
    // Pin the forwarded path VALUE, not just non-null — a mangled/empty path
    // handed to the hook would otherwise pass unnoticed.
    EXPECT_STREQ("/b.mcl", (const char*)s_openedWorldFile);
    EXPECT_EQ(&fakeDoc, t.OpenDocumentFile(""));   // empty path -> Quick Connect
    EXPECT_EQ(1, s_newWorldCalls);
    g_wxhook_open_world_file = nullptr;
    g_wxhook_new_world = nullptr;

    // Base-class CDocTemplate always returns null
    CDocTemplate base;
    EXPECT_EQ((CDocument*)nullptr, base.OpenDocumentFile("/x"));
    CSingleDocTemplate single(0, nullptr, nullptr, nullptr);
}

// ═══════════════════════════════════════════════════════════════════════════
// Message maps: AfxDispatchCommand / AfxDispatchNotify / AfxDispatchUpdateUI
// ═══════════════════════════════════════════════════════════════════════════

namespace {
class MapTarget : public CCmdTarget {
public:
    int cmdA = 0, clicks = 0;
    UINT lastRangeId = 0;
    UINT notifyCode = 0;
    void OnCmdA()          { ++cmdA; }
    void OnClickB()        { ++clicks; }
    void OnRange(UINT id)  { lastRangeId = id; }
    void OnNotifyX(NMHDR* pnmh, LRESULT* pResult) { notifyCode = pnmh->code; *pResult = 7; }
    void OnUpdateX(CCmdUI* pCmdUI) { pCmdUI->Enable(FALSE); pCmdUI->SetCheck(1); }
    DECLARE_MESSAGE_MAP()
};

BEGIN_MESSAGE_MAP(MapTarget, CCmdTarget)
    ON_COMMAND(101, OnCmdA)
    ON_BN_CLICKED(102, OnClickB)
    ON_COMMAND_RANGE(200, 205, OnRange)
    ON_NOTIFY(77, 300, OnNotifyX)
    ON_UPDATE_COMMAND_UI(400, OnUpdateX)
END_MESSAGE_MAP()

class DerivedTarget : public MapTarget {
public:
    int cmdD = 0;
    void OnCmdD() { ++cmdD; }
    DECLARE_MESSAGE_MAP()
};

BEGIN_MESSAGE_MAP(DerivedTarget, MapTarget)
    ON_COMMAND(500, OnCmdD)
END_MESSAGE_MAP()
} // namespace

TEST(MessageMap, CommandDispatchHitsHandler) {
    MapTarget t;
    EXPECT_TRUE(AfxDispatchCommand(&t, 0, 101));
    EXPECT_EQ(1, t.cmdA);
    EXPECT_TRUE(AfxDispatchCommand(&t, BN_CLICKED, 102));
    EXPECT_EQ(1, t.clicks);
    // BN_CLICKED is 0, so it is the SAME notification code as ON_COMMAND —
    // exactly as on Windows, where the two macros emit identical entries.
    EXPECT_TRUE(AfxDispatchCommand(&t, BN_CLICKED, 101));
    EXPECT_EQ(2, t.cmdA);
    // Unknown id / wrong (nonzero) notification code / null target
    EXPECT_FALSE(AfxDispatchCommand(&t, 0, 999));
    EXPECT_FALSE(AfxDispatchCommand(&t, CBN_SELCHANGE, 101));
    EXPECT_FALSE(AfxDispatchCommand(nullptr, 0, 101));
}

TEST(MessageMap, RangeDispatchPassesTheId) {
    MapTarget t;
    EXPECT_TRUE(AfxDispatchCommand(&t, 0, 203));
    EXPECT_EQ(203u, t.lastRangeId);
    EXPECT_TRUE(AfxDispatchCommand(&t, 0, 200));   // inclusive lower bound
    EXPECT_EQ(200u, t.lastRangeId);
    EXPECT_TRUE(AfxDispatchCommand(&t, 0, 205));   // inclusive upper bound
    EXPECT_EQ(205u, t.lastRangeId);
    EXPECT_FALSE(AfxDispatchCommand(&t, 0, 206));
}

TEST(MessageMap, DerivedChainsToBaseMap) {
    DerivedTarget t;
    EXPECT_TRUE(AfxDispatchCommand(&t, 0, 500));   // own map
    EXPECT_EQ(1, t.cmdD);
    EXPECT_TRUE(AfxDispatchCommand(&t, 0, 101));   // walks to MapTarget's map
    EXPECT_EQ(1, t.cmdA);
}

TEST(MessageMap, NotifyDispatch) {
    MapTarget t;
    NMHDR hdr; hdr.hwndFrom = nullptr; hdr.idFrom = 300; hdr.code = 77;
    LRESULT res = 0;
    EXPECT_TRUE(AfxDispatchNotify(&t, 77, 300, &hdr, &res));
    EXPECT_EQ(77u, t.notifyCode);
    EXPECT_EQ((LRESULT)7, res);
    EXPECT_FALSE(AfxDispatchNotify(&t, 78, 300, &hdr, &res));   // wrong code
    EXPECT_FALSE(AfxDispatchNotify(&t, 77, 301, &hdr, &res));   // wrong id
    EXPECT_FALSE(AfxDispatchNotify(nullptr, 77, 300, &hdr, &res));
}

TEST(MessageMap, UpdateUIDispatchAndCmdUICapture) {
    MapTarget t;
    CCmdUI ui;
    bool enabled = true;
    int  check   = -1;
    ui.m_nID = 400;
    ui.m_pCaptureEnable = &enabled;
    ui.m_pCaptureCheck  = &check;
    EXPECT_TRUE(AfxDispatchUpdateUI(&t, &ui));
    EXPECT_FALSE(enabled);   // handler called Enable(FALSE)
    EXPECT_EQ(1, check);     // ... and SetCheck(1)
    ui.m_nID = 401;
    EXPECT_FALSE(AfxDispatchUpdateUI(&t, &ui));
    EXPECT_FALSE(AfxDispatchUpdateUI(nullptr, &ui));
    EXPECT_FALSE(AfxDispatchUpdateUI(&t, nullptr));
    // SetRadio routes through SetCheck
    ui.m_nID = 400;
    ui.SetRadio(FALSE);
    EXPECT_EQ(0, check);
    ui.ContinueRouting();
}

static std::string s_cmduiText;
static long s_cmduiMsg;

TEST(MessageMap, CmdUIHookModeRoutesToControl) {
    fakewx::Install hooks;
    CCmdUI ui;
    ui.m_hWndCtrl = fakewx::handle_for(55);
    ui.Enable(FALSE);
    EXPECT_FALSE(fakewx::widget(55).enabled);
    ui.Enable(TRUE);
    EXPECT_TRUE(fakewx::widget(55).enabled);
    ui.SetText("Updated");
    EXPECT_STREQ("Updated", fakewx::widget(55).text.c_str());
    // SetCheck sends BM_SETCHECK through the send_message hook
    s_cmduiMsg = -1;
    g_wxhook_send_message = [](void*, unsigned msg, uintptr_t wp, intptr_t) -> long {
        s_cmduiMsg = (long)((msg == 0x00F1) ? (long)wp : -2);
        return 0;
    };
    ui.SetCheck(1);
    EXPECT_EQ(1L, s_cmduiMsg);
    g_wxhook_send_message = nullptr;
    // Null-control CCmdUI is inert
    CCmdUI bare;
    bare.Enable(FALSE);
    bare.SetCheck(1);
    bare.SetText("x");
}

// ═══════════════════════════════════════════════════════════════════════════
// CDialog + DDX / DDV
// ═══════════════════════════════════════════════════════════════════════════

namespace {
enum {
    ID_TEXT = 1, ID_INT = 2, ID_SHORT = 3, ID_LONG = 4, ID_UINT = 5,
    ID_FLOAT = 6, ID_DOUBLE = 7, ID_CHECK = 8, ID_COMBO = 9, ID_COMBO2 = 10,
    ID_RADIO_FIRST = 20, ID_LISTCTL = 30,
};

class DdxDialog : public CDialog {
public:
    CString m_text = "hello";
    int     m_int = 42;
    short   m_short = -7;
    long    m_long = 123456789L;
    UINT    m_uint = 77u;
    float   m_float = 1.5f;
    double  m_double = 2.25;
    int     m_check = 1;
    int     m_comboIndex = 1;
    CString m_comboString = "beta";
    int     m_radio = 2;
    CWnd    m_boundCtl;
    bool    m_failValidation = false;

    DdxDialog() : CDialog(999) { m_hWnd = (HWND)(void*)this; }

    void DoDataExchange(CDataExchange* pDX) override {
        pDX->PrepareCtrl(ID_TEXT);
        pDX->PrepareEditCtrl(ID_INT);
        DDX_Text(pDX, ID_TEXT, m_text);
        DDX_Text(pDX, ID_INT, m_int);
        DDX_Text(pDX, ID_SHORT, m_short);
        DDX_Text(pDX, ID_LONG, m_long);
        DDX_Text(pDX, ID_UINT, m_uint);
        DDX_Text(pDX, ID_FLOAT, m_float);
        DDX_Text(pDX, ID_DOUBLE, m_double);
        DDX_Check(pDX, ID_CHECK, m_check);
        DDX_CBIndex(pDX, ID_COMBO, m_comboIndex);
        DDX_CBString(pDX, ID_COMBO2, m_comboString);
        DDX_Radio(pDX, ID_RADIO_FIRST, m_radio);
        DDX_Control(pDX, ID_LISTCTL, m_boundCtl);
        DDV_MinMaxInt(pDX, m_int, 0, 1000);
        if (m_failValidation && pDX->m_bSaveAndValidate)
            pDX->Fail();
    }
};
} // namespace

TEST(Ddx, LoadDirectionWritesControls) {
    fakewx::Install hooks;
    // combo needs items so SetCurSel/SelectString have something to select
    fakewx::widget(ID_COMBO).items = { "alpha", "beta", "gamma" };
    fakewx::widget(ID_COMBO2).items = { "alpha", "beta", "gamma" };
    DdxDialog dlg;
    EXPECT_TRUE(dlg.UpdateData(FALSE));   // members -> controls
    EXPECT_STREQ("hello", fakewx::widget(ID_TEXT).text.c_str());
    EXPECT_STREQ("42", fakewx::widget(ID_INT).text.c_str());
    EXPECT_STREQ("-7", fakewx::widget(ID_SHORT).text.c_str());
    EXPECT_STREQ("123456789", fakewx::widget(ID_LONG).text.c_str());
    EXPECT_STREQ("77", fakewx::widget(ID_UINT).text.c_str());
    EXPECT_STREQ("1.5", fakewx::widget(ID_FLOAT).text.c_str());
    EXPECT_STREQ("2.25", fakewx::widget(ID_DOUBLE).text.c_str());
    EXPECT_EQ(1, fakewx::widget(ID_CHECK).check);
    EXPECT_EQ(1, fakewx::widget(ID_COMBO).cursel);
    EXPECT_EQ(1, fakewx::widget(ID_COMBO2).cursel);   // SelectString found "beta"
    EXPECT_EQ(2, fakewx::radios()[ID_RADIO_FIRST]);
    // DDX_Control bound the member's m_hWnd to the control handle
    EXPECT_EQ(fakewx::handle_for(ID_LISTCTL), (void*)dlg.m_boundCtl.m_hWnd);
}

TEST(Ddx, SaveDirectionParsesControls) {
    fakewx::Install hooks;
    fakewx::widget(ID_TEXT).text = "changed";
    fakewx::widget(ID_INT).text = "314";
    fakewx::widget(ID_SHORT).text = "-2";
    fakewx::widget(ID_LONG).text = "987654321";
    fakewx::widget(ID_UINT).text = "3000000000";
    fakewx::widget(ID_FLOAT).text = "0.25";
    fakewx::widget(ID_DOUBLE).text = "6.5";
    fakewx::widget(ID_CHECK).check = 0;
    fakewx::widget(ID_COMBO).items = { "alpha", "beta", "gamma" };
    fakewx::widget(ID_COMBO).cursel = 2;
    fakewx::widget(ID_COMBO2).items = { "alpha", "beta", "gamma" };
    fakewx::widget(ID_COMBO2).cursel = 0;
    fakewx::radios()[ID_RADIO_FIRST] = 1;

    DdxDialog dlg;
    EXPECT_TRUE(dlg.UpdateData(TRUE));   // controls -> members
    EXPECT_STREQ("changed", (LPCTSTR)dlg.m_text);
    EXPECT_EQ(314, dlg.m_int);
    EXPECT_EQ((short)-2, dlg.m_short);
    EXPECT_EQ(987654321L, dlg.m_long);
    EXPECT_EQ(3000000000u, dlg.m_uint);
    EXPECT_NEAR(0.25f, dlg.m_float, 1e-6);
    EXPECT_NEAR(6.5, dlg.m_double, 1e-9);
    EXPECT_EQ(0, dlg.m_check);
    EXPECT_EQ(2, dlg.m_comboIndex);
    EXPECT_STREQ("alpha", (LPCTSTR)dlg.m_comboString);
    EXPECT_EQ(1, dlg.m_radio);
}

TEST(Ddx, ComboWithNoSelectionPreservesMember) {
    // Recovered .rc templates can produce empty combos; CB_ERR must not
    // clobber a valid member with -1.
    fakewx::Install hooks;
    DdxDialog dlg;
    dlg.m_comboIndex = 3;
    EXPECT_TRUE(dlg.UpdateData(FALSE));     // as OnInitDialog does: fill the controls
    fakewx::widget(ID_COMBO).cursel = -1;   // no selection
    EXPECT_TRUE(dlg.UpdateData(TRUE));
    EXPECT_EQ(3, dlg.m_comboIndex);
}

TEST(Ddx, CBStringFallsBackToEditTextWhenNotInList) {
    fakewx::Install hooks;
    fakewx::widget(ID_COMBO2).items = { "alpha" };
    DdxDialog dlg;
    dlg.m_comboString = "freetext";        // not in the list
    EXPECT_TRUE(dlg.UpdateData(FALSE));
    EXPECT_STREQ("freetext", fakewx::widget(ID_COMBO2).text.c_str());
    // save with no selection -> reads the edit text
    fakewx::widget(ID_COMBO2).cursel = -1;
    fakewx::widget(ID_COMBO2).text = "typed";
    EXPECT_TRUE(dlg.UpdateData(TRUE));
    EXPECT_STREQ("typed", (LPCTSTR)dlg.m_comboString);
}

TEST(Ddx, ValidationFailVetoesUpdateAndOnOK) {
    fakewx::Install hooks;
    DdxDialog dlg;
    EXPECT_TRUE(dlg.UpdateData(FALSE));   // as OnInitDialog does: fill the controls
    dlg.m_failValidation = true;
    EXPECT_FALSE(dlg.UpdateData(TRUE));   // pDX->Fail() -> FALSE, no throw escape
    static int s_endCalls = 0;
    s_endCalls = 0;
    g_end_mfc_dialog = [](void*, int) { ++s_endCalls; };
    dlg.OnOK();                            // veto: EndDialog must NOT run
    EXPECT_EQ(0, s_endCalls);
    dlg.m_failValidation = false;
    dlg.OnOK();
    EXPECT_EQ(1, s_endCalls);
    g_end_mfc_dialog = nullptr;
}

TEST(Ddx, NullDlgItemsLeaveEveryMemberAlone) {
    // No hooks at all (headless): every DDX form must leave its member alone.
    // The numeric family used to parse the empty text of a control that is not
    // there and store the resulting 0 -- and the bridge legitimately omits
    // controls Windows hides (the WS_VISIBLE arm), so that was a live way for a
    // prefs page to zero a setting nobody had touched.  It must not now fail the
    // exchange either: there is no field for the user to correct.
    DdxDialog dlg;
    EXPECT_TRUE(dlg.UpdateData(TRUE));
    EXPECT_STREQ("hello", (LPCTSTR)dlg.m_text);   // CString form: guarded
    EXPECT_EQ(42, dlg.m_int);                      // numeric forms: guarded too
    EXPECT_EQ((short)-7, dlg.m_short);
    EXPECT_EQ(123456789L, dlg.m_long);
    EXPECT_EQ(77u, dlg.m_uint);
    EXPECT_NEAR(1.5f, dlg.m_float, 1e-6);
    EXPECT_NEAR(2.25, dlg.m_double, 1e-12);
    EXPECT_EQ(1, dlg.m_check);                     // control-class forms: guarded
    EXPECT_EQ(1, dlg.m_comboIndex);
    EXPECT_STREQ("beta", (LPCTSTR)dlg.m_comboString);
    EXPECT_EQ(2, dlg.m_radio);
}

// MFC formats a double with DBL_DIG (15) significant digits; "%g" gives 6, so
// merely OPENING a dialog on a timer of 59.999999 seconds rewrote it as 60 --
// and a value near the top of its DDV range could round PAST the range, leaving
// a dialog whose OK button no longer worked on a value the user never touched.
TEST(Ddx, DoubleLoadKeepsFifteenSignificantDigits) {
    fakewx::Install hooks;
    DdxDialog dlg;
    dlg.m_double = 59.999999;
    EXPECT_TRUE(dlg.UpdateData(FALSE));            // member -> control
    EXPECT_STREQ("59.999999", fakewx::widget(ID_DOUBLE).text.c_str());

    // ...and the round trip is lossless, which "%g" made it not.
    EXPECT_TRUE(dlg.UpdateData(TRUE));
    EXPECT_NEAR(59.999999, dlg.m_double, 1e-12);

    // The float overload keeps "%g": MFC uses FLT_DIG, which IS 6.
    dlg.m_float = 1.25f;
    EXPECT_TRUE(dlg.UpdateData(FALSE));
    EXPECT_STREQ("1.25", fakewx::widget(ID_FLOAT).text.c_str());
}

// MFC rejects text it cannot scan a number out of ("Please enter an integer.")
// and calls pDX->Fail(), so the dialog stays open with the member untouched.
// The port fed the text to strtol/strtod and kept their 0, so typing rubbish
// into any of the ~100 numeric prefs fields set the setting to zero.
TEST(Ddx, UnparseableNumericTextIsRejectedRatherThanRead) {
    fakewx::Install hooks;
    static int s_boxes = 0;
    static std::string s_msg;
    int (*const saved)(const char*, const char*, unsigned) = g_wxhook_message_box;
    g_wxhook_message_box = [](const char* text, const char*, unsigned) -> int {
        ++s_boxes; s_msg = text ? text : ""; return IDOK;
    };

    DdxDialog dlg;
    EXPECT_TRUE(dlg.UpdateData(FALSE));            // seed every control
    fakewx::widget(ID_INT).text = "abc";
    EXPECT_FALSE(dlg.UpdateData(TRUE));            // vetoed
    EXPECT_EQ(42, dlg.m_int);                      // member kept its old value
    EXPECT_EQ(1, s_boxes);                         // and the user was told why
    EXPECT_TRUE(s_msg.find("integer") != std::string::npos);

    // An emptied field is the same failure — not a zero.
    fakewx::widget(ID_INT).text = "";
    EXPECT_FALSE(dlg.UpdateData(TRUE));
    EXPECT_EQ(42, dlg.m_int);

    // Only ONE box per UpdateData: Fail() aborts the rest of the exchange.
    s_boxes = 0;
    fakewx::widget(ID_INT).text = "x";
    fakewx::widget(ID_DOUBLE).text = "y";
    EXPECT_FALSE(dlg.UpdateData(TRUE));
    EXPECT_EQ(1, s_boxes);

    // The real overload reports the real noun.
    fakewx::widget(ID_INT).text = "7";
    s_boxes = 0;
    EXPECT_FALSE(dlg.UpdateData(TRUE));
    EXPECT_EQ(1, s_boxes);
    EXPECT_TRUE(s_msg.find("number") != std::string::npos);
    EXPECT_NEAR(2.25, dlg.m_double, 1e-12);

    // Like the scanf it stands in for, a valid number with trailing rubbish is
    // still accepted, so this cannot start rejecting text Windows takes.
    s_boxes = 0;
    fakewx::widget(ID_INT).text = "  12abc";
    fakewx::widget(ID_DOUBLE).text = "3.5 seconds";
    EXPECT_TRUE(dlg.UpdateData(TRUE));
    EXPECT_EQ(0, s_boxes);
    EXPECT_EQ(12, dlg.m_int);
    EXPECT_NEAR(3.5, dlg.m_double, 1e-12);

    g_wxhook_message_box = saved;
}

// MFC's CDataExchange::Fail() puts the user IN the offending control: focus
// returns to the last-prepared control, and an edit's text is selected so the
// bad value is retype-ready.  The port's Fail() only threw -- after "Please
// enter an integer between 20 and 500." on a dense page, nothing said WHICH
// of the numeric fields was rejected.
namespace {
enum { ID_DDV_FIRST = 41, ID_DDV_SECOND = 42, ID_DDV_CHECK = 43 };

class DdvFocusDialog : public CDialog {
public:
    int m_first = 5;
    int m_second = 5;
    DdvFocusDialog() : CDialog(998) { m_hWnd = (HWND)(void*)this; }
    void DoDataExchange(CDataExchange* pDX) override {
        // The MFC contract: each DDV immediately follows the DDX of the field
        // it validates, which is what makes "last prepared" the right control.
        DDX_Text(pDX, ID_DDV_FIRST, m_first);
        DDV_MinMaxInt(pDX, m_first, 0, 10);
        DDX_Text(pDX, ID_DDV_SECOND, m_second);
        DDV_MinMaxInt(pDX, m_second, 0, 10);
    }
};

// A non-edit control prepared last: focus moves, but no text selection.
class DdvCheckFailDialog : public CDialog {
public:
    int m_check = 0;
    DdvCheckFailDialog() : CDialog(997) { m_hWnd = (HWND)(void*)this; }
    void DoDataExchange(CDataExchange* pDX) override {
        DDX_Check(pDX, ID_DDV_CHECK, m_check);
        if (pDX->m_bSaveAndValidate) pDX->Fail();
    }
};

void* s_ddvFocused;
int   s_ddvFocusCalls;
void* s_ddvSelWnd;
int   s_ddvSelStart, s_ddvSelEnd;
} // namespace

TEST(Ddv, FailurePutsFocusInTheOffendingEdit) {
    fakewx::Install hooks;
    s_ddvFocused = nullptr; s_ddvFocusCalls = 0;
    s_ddvSelWnd = nullptr;  s_ddvSelStart = s_ddvSelEnd = 99;   // pre-poisoned
    g_wxhook_set_focus = [](void* h) { s_ddvFocused = h; ++s_ddvFocusCalls; };
    g_wxhook_edit_set_sel = [](void* h, int s, int e) {
        s_ddvSelWnd = h; s_ddvSelStart = s; s_ddvSelEnd = e;
    };
    int (*const savedBox)(const char*, const char*, unsigned) = g_wxhook_message_box;
    g_wxhook_message_box = [](const char*, const char*, unsigned) -> int { return IDOK; };

    DdvFocusDialog dlg;
    EXPECT_TRUE(dlg.UpdateData(FALSE));

    // Fail the SECOND field: "focus the first field" and "focus anything"
    // both flunk this, so the fixture can discriminate the mapping.
    fakewx::widget(ID_DDV_SECOND).text = "999";
    EXPECT_FALSE(dlg.UpdateData(TRUE));
    EXPECT_EQ(fakewx::handle_for(ID_DDV_SECOND), s_ddvFocused);
    EXPECT_EQ(1, s_ddvFocusCalls);
    EXPECT_EQ(fakewx::handle_for(ID_DDV_SECOND), s_ddvSelWnd);
    EXPECT_EQ(0,  s_ddvSelStart);              // CEdit::SetSel(0, -1)
    EXPECT_EQ(-1, s_ddvSelEnd);

    // ...and failing the FIRST focuses the first (the degenerate-fixture guard:
    // the two cases must land on DIFFERENT controls).
    s_ddvFocused = nullptr; s_ddvFocusCalls = 0;
    fakewx::widget(ID_DDV_SECOND).text = "5";
    fakewx::widget(ID_DDV_FIRST).text  = "999";
    EXPECT_FALSE(dlg.UpdateData(TRUE));
    EXPECT_EQ(fakewx::handle_for(ID_DDV_FIRST), s_ddvFocused);

    // A non-edit control focuses but does NOT select text.
    s_ddvFocused = nullptr; s_ddvSelWnd = nullptr;
    DdvCheckFailDialog cdlg;
    EXPECT_FALSE(cdlg.UpdateData(TRUE));
    EXPECT_EQ(fakewx::handle_for(ID_DDV_CHECK), s_ddvFocused);
    EXPECT_EQ((void*)nullptr, s_ddvSelWnd);

    g_wxhook_set_focus = nullptr;
    g_wxhook_edit_set_sel = nullptr;
    g_wxhook_message_box = savedBox;
}

// DDV_MaxChars was a permanent no-op (its old comment blamed
// DDV_validation.cpp, but on Windows MFC itself supplies it), so every
// maximum-length validation was silently skipped: the 7 direct call sites and
// the max half of every DDV_MinMaxString/DDV_MinMaxCBString, which delegate
// their max check to it.
namespace {
class MaxCharsDialog : public CDialog {
public:
    CString m_text = "hi";
    int     m_check = 0;
    MaxCharsDialog() : CDialog(996) { m_hWnd = (HWND)(void*)this; }
    void DoDataExchange(CDataExchange* pDX) override {
        DDX_Text(pDX, ID_DDV_FIRST, m_text);
        DDV_MaxChars(pDX, m_text, 20);
        DDX_Check(pDX, ID_DDV_CHECK, m_check);  // a later non-edit exchange
    }
};
int         s_ddvBoxes;
std::string s_ddvMsg;
} // namespace

TEST(Ddv, MaxCharsRefusesOverlongStrings) {
    fakewx::Install hooks;
    s_ddvBoxes = 0; s_ddvMsg.clear();
    int (*const savedBox)(const char*, const char*, unsigned) = g_wxhook_message_box;
    g_wxhook_message_box = [](const char* text, const char*, unsigned) -> int {
        ++s_ddvBoxes; s_ddvMsg = text ? text : ""; return IDOK;
    };

    CDataExchange dx;              // no dialog window: DDX inert, DDV decides
    dx.m_bSaveAndValidate = TRUE;

    CString ok("xxxxxxxxxx");      // exactly 10: at the limit is allowed
    DDV_MaxChars(&dx, ok, 10);
    EXPECT_EQ(0, s_ddvBoxes);

    CString over("xxxxxxxxxxY");   // 11 > 10: refused, message names the limit
    bool threw = false;
    try { DDV_MaxChars(&dx, over, 10); }
    catch (const CDdxValidationFail&) { threw = true; }
    EXPECT_TRUE(threw);
    EXPECT_EQ(1, s_ddvBoxes);
    EXPECT_TRUE(s_ddvMsg.find("no more than 10 characters") != std::string::npos);

    // The DELEGATED path: 28 DDV_MinMaxString/CBString sites reach the max
    // check only through DDV_MaxChars, so pin it too -- asserting only the
    // direct call would leave that half uncovered.
    s_ddvBoxes = 0;
    threw = false;
    try { DDV_MinMaxString(&dx, over, 1, 10); }
    catch (const CDdxValidationFail&) { threw = true; }
    EXPECT_TRUE(threw);
    EXPECT_EQ(1, s_ddvBoxes);

    // The load direction never vetoes.
    dx.m_bSaveAndValidate = FALSE;
    s_ddvBoxes = 0;
    DDV_MaxChars(&dx, over, 10);
    EXPECT_EQ(0, s_ddvBoxes);

    g_wxhook_message_box = savedBox;
}

// The other half of MFC's DDV_MaxChars: on load it applies EM_LIMITTEXT to
// the last-prepared edit, so the user cannot even type past the limit.
namespace {
void* s_limWnd;
int   s_limChars;
int   s_limCalls;
} // namespace

TEST(Ddv, MaxCharsLimitsTypingOnLoad) {
    fakewx::Install hooks;
    s_limWnd = nullptr; s_limChars = -1; s_limCalls = 0;
    g_wxhook_edit_set_max_length = [](void* h, int n) {
        s_limWnd = h; s_limChars = n; ++s_limCalls;
    };

    MaxCharsDialog dlg;
    EXPECT_TRUE(dlg.UpdateData(FALSE));            // load
    EXPECT_EQ(1, s_limCalls);                      // the DDV'd edit and nothing else
    EXPECT_EQ(fakewx::handle_for(ID_DDV_FIRST), s_limWnd);
    EXPECT_EQ(20, s_limChars);

    // The save direction validates; it does not re-limit.
    s_limCalls = 0;
    EXPECT_TRUE(dlg.UpdateData(TRUE));
    EXPECT_EQ(0, s_limCalls);

    g_wxhook_edit_set_max_length = nullptr;
}

// A real shipping page, headless: the Plugin Wizard's ID field is
// DDV_MinMaxString(24, 24), whose max side never fired -- a 30-character ID
// was accepted and written into the generated plugin.
TEST(Ddv, PluginWizardIdLengthIsEnforced) {
    s_ddvBoxes = 0;
    int (*const savedBox)(const char*, const char*, unsigned) = g_wxhook_message_box;
    g_wxhook_message_box = [](const char*, const char*, unsigned) -> int {
        ++s_ddvBoxes; return IDOK;
    };

    struct P1 : CPluginWizardPage1 { using CPluginWizardPage1::DoDataExchange; };
    CMUSHclientDoc doc;
    P1 page;
    CPluginWizardPage2 page2;          // the save arm dereferences m_pPage2
    page.m_doc = &doc;                 // CheckObjectName dereferences it
    page.m_pPage2 = &page2;
    page.m_strName = "TestPlugin";

    CDataExchange dx;                  // control-less: only the DDV decides
    dx.m_bSaveAndValidate = TRUE;

    page.m_strID = "abcdefghijklmnopqrstuvwx";        // 24: the required length
    bool threw = false;
    try { page.DoDataExchange(&dx); }
    catch (const CDdxValidationFail&) { threw = true; }
    EXPECT_FALSE(threw);
    EXPECT_EQ(0, s_ddvBoxes);

    page.m_strID = "abcdefghijklmnopqrstuvwxyz0123";  // 30: Windows refuses it
    threw = false;
    try { page.DoDataExchange(&dx); }
    catch (const CDdxValidationFail&) { threw = true; }
    EXPECT_TRUE(threw);
    EXPECT_EQ(1, s_ddvBoxes);

    g_wxhook_message_box = savedBox;
}

static unsigned s_modalIdd;

TEST(Dialog, DoModalRoutesThroughBridgeHook) {
    CDialog dlg(4242);
    EXPECT_EQ((INT_PTR)IDCANCEL, dlg.DoModal());   // headless: no bridge
    g_run_mfc_dialog = [](void* self, unsigned idd) -> intptr_t {
        s_modalIdd = idd;
        return 77;
    };
    s_modalIdd = 0;
    EXPECT_EQ((INT_PTR)77, dlg.DoModal());
    EXPECT_EQ(4242u, s_modalIdd);
    g_run_mfc_dialog = nullptr;
}

static int s_endResult;

TEST(Dialog, OnCancelEndsWithIdCancel) {
    CDialog dlg(1);
    s_endResult = 0;
    g_end_mfc_dialog = [](void*, int result) { s_endResult = result; };
    dlg.OnCancel();
    EXPECT_EQ(IDCANCEL, s_endResult);
    dlg.OnOK();   // base dialog: UpdateData(TRUE) trivially succeeds
    EXPECT_EQ(IDOK, s_endResult);
    g_end_mfc_dialog = nullptr;
    EXPECT_TRUE(dlg.OnInitDialog());
}

TEST(Dialog, RadioAndCheckButtonHelpers) {
    fakewx::Install hooks;
    CDialog dlg(1);
    dlg.m_hWnd = (HWND)(void*)&dlg;
    dlg.CheckRadioButton(60, 63, 62);
    EXPECT_EQ(0, fakewx::widget(60).check);
    EXPECT_EQ(0, fakewx::widget(61).check);
    EXPECT_EQ(1, fakewx::widget(62).check);
    EXPECT_EQ(0, fakewx::widget(63).check);
    dlg.CheckDlgButton(65, 1);
    EXPECT_EQ(1, fakewx::widget(65).check);
    EXPECT_TRUE(dlg.IsDlgButtonChecked(65));
    EXPECT_FALSE(dlg.IsDlgButtonChecked(60));
    dlg.SetDlgItemText(66, "label");
    EXPECT_STREQ("label", fakewx::widget(66).text.c_str());
    CString got;
    dlg.GetDlgItemText(66, got);
    EXPECT_STREQ("label", (LPCTSTR)got);
}

// ═══════════════════════════════════════════════════════════════════════════
// Control classes over the fake widget store
// ═══════════════════════════════════════════════════════════════════════════

TEST(Controls, ListBoxHookedOperations) {
    fakewx::Install hooks;
    CListBox lb;
    lb.m_hWnd = (HWND)fakewx::handle_for(70);
    EXPECT_EQ(0, lb.AddString("first"));
    EXPECT_EQ(1, lb.AddString("second"));
    EXPECT_EQ(2, lb.GetCount());
    EXPECT_EQ(LB_ERR, lb.GetCurSel());
    lb.SetCurSel(1);
    EXPECT_EQ(1, lb.GetCurSel());
    CString s;
    lb.GetText(0, s);
    EXPECT_STREQ("first", (LPCTSTR)s);
    lb.GetText(99, s);
    EXPECT_TRUE(s.IsEmpty());
    lb.SetItemData(0, 111);
    EXPECT_EQ((DWORD_PTR)111, lb.GetItemData(0));
    lb.SetSel(0);
    lb.SetSel(1);
    EXPECT_EQ(2, lb.GetSelCount());
    EXPECT_EQ(1, lb.GetSel(0));
    lb.SetSel(0, FALSE);
    EXPECT_EQ(0, lb.GetSel(0));
    int items[4] = {};
    EXPECT_EQ(1, lb.GetSelItems(4, items));
    EXPECT_EQ(1, items[0]);
    lb.ResetContent();
    EXPECT_EQ(0, lb.GetCount());
}

// LB_FINDSTRING / LB_SELECTSTRING were permanent LB_ERR, which is why the
// Complete Word dialog never pre-selected the entry matching what the user had
// typed -- so pressing OK (or Enter) inserted nothing at all.  The match is a
// case-insensitive PREFIX, not an exact one: the filter is the part of the word
// already typed, so an exactly-equal entry usually does not exist.
TEST(Controls, ListBoxFindsAndSelectsByPrefix) {
    fakewx::Install hooks;
    CListBox lb;
    lb.m_hWnd = (HWND)fakewx::handle_for(78);
    lb.AddString("world.Note");
    lb.AddString("world.NoteStyle");
    lb.AddString("world.Send");

    // A prefix nothing matches exactly still finds the entry, and selects it.
    EXPECT_EQ(0, lb.SelectString(-1, "world.no"));   // and case-insensitively
    EXPECT_EQ(0, lb.GetCurSel());
    EXPECT_EQ(0, lb.FindString(-1, "WORLD.NOTE"));

    // FindStringExact would answer 1 here; the prefix search must answer 0.
    EXPECT_EQ(0, lb.FindString(-1, "world.Note"));

    // `after` resumes past that index and wraps, so a repeated search walks the
    // matches rather than sticking on the first.
    EXPECT_EQ(1, lb.FindString(0, "world.no"));
    EXPECT_EQ(0, lb.FindString(1, "world.no"));

    // No match: LB_ERR, and the selection is left where it was.
    lb.SetCurSel(2);
    EXPECT_EQ(LB_ERR, lb.SelectString(-1, "zzz"));
    EXPECT_EQ(2, lb.GetCurSel());

    // An empty filter is a prefix of everything (MFC's behaviour), and an empty
    // list has nothing to find.
    EXPECT_EQ(0, lb.FindString(-1, ""));
    lb.ResetContent();
    EXPECT_EQ(LB_ERR, lb.FindString(-1, "world"));
}

TEST(Controls, ListBoxNullHooksReturnErrors) {
    CListBox lb;
    EXPECT_EQ(LB_ERR, lb.AddString("x"));
    EXPECT_EQ(0, lb.GetCount());
    EXPECT_EQ(LB_ERR, lb.GetCurSel());
    EXPECT_EQ(LB_ERR, lb.SetCurSel(0));
    EXPECT_EQ((DWORD_PTR)0, lb.GetItemData(0));
    EXPECT_EQ(0, lb.GetSelCount());
    EXPECT_EQ(LB_ERR, lb.SetSel(0));
    CString s;
    lb.GetText(0, s);
    EXPECT_TRUE(s.IsEmpty());
    lb.ResetContent();
    lb.SetItemData(0, 1);
}

TEST(Controls, ComboBoxHookedOperations) {
    fakewx::Install hooks;
    CComboBox cb;
    cb.m_hWnd = (HWND)fakewx::handle_for(71);
    EXPECT_EQ(0, cb.AddString("red"));
    EXPECT_EQ(1, cb.AddString("green"));
    EXPECT_EQ(2, cb.AddString("blue"));
    EXPECT_EQ(3, cb.GetCount());
    EXPECT_EQ(CB_ERR, cb.GetCurSel());
    EXPECT_EQ(1, cb.SetCurSel(1));
    EXPECT_EQ(1, cb.GetCurSel());
    CString s;
    cb.GetLBText(2, s);
    EXPECT_STREQ("blue", (LPCTSTR)s);
    cb.GetLBText(9, s);
    EXPECT_TRUE(s.IsEmpty());
    EXPECT_EQ(2, cb.FindStringExact(-1, "blue"));
    EXPECT_EQ(2, cb.FindStringExact(2, "blue"));   // search starts after and wraps
    EXPECT_EQ(2, cb.FindStringExact(-1, "BLUE"));  // CB_FINDSTRINGEXACT folds case
    EXPECT_EQ(CB_ERR, cb.FindStringExact(-1, "mauve"));
    EXPECT_EQ(2, cb.FindString(-1, "blue"));
    EXPECT_EQ(0, cb.SelectString(-1, "red"));
    EXPECT_EQ(0, cb.GetCurSel());
    EXPECT_EQ(CB_ERR, cb.SelectString(-1, "mauve"));    // unchanged selection
    EXPECT_EQ(0, cb.GetCurSel());
    cb.ResetContent();
    EXPECT_EQ(0, cb.GetCount());
}

// Item data is what makes a sorted combo usable: the caller stores its own
// index against each entry and reads it back from the selected position.
// CLuaChooseBox does exactly that, so with these as no-op stubs (which they
// were) GetItemData always answered 0 and Lua's utils.choose returned the first
// key in the table no matter what the user picked.
TEST(Controls, ComboBoxItemDataRoundTrips) {
    fakewx::Install hooks;
    CComboBox cb;
    cb.m_hWnd = (HWND)fakewx::handle_for(72);
    ASSERT_EQ(0, cb.AddString("zebra"));
    ASSERT_EQ(1, cb.AddString("aardvark"));
    ASSERT_EQ(2, cb.AddString("moose"));

    // the caller's own ordering, deliberately not the display ordering
    cb.SetItemData(0, 20);
    cb.SetItemData(1, 10);
    cb.SetItemData(2, 30);

    EXPECT_EQ((DWORD_PTR)20, cb.GetItemData(0));
    EXPECT_EQ((DWORD_PTR)10, cb.GetItemData(1));
    EXPECT_EQ((DWORD_PTR)30, cb.GetItemData(2));

    // an entry that was never given data reads as 0, as MFC's does
    ASSERT_EQ(3, cb.AddString("newt"));
    EXPECT_EQ((DWORD_PTR)0, cb.GetItemData(3));

    // out-of-range indices are inert, not a crash
    cb.SetItemData(99, 7);
    EXPECT_EQ((DWORD_PTR)0, cb.GetItemData(99));
    EXPECT_EQ((DWORD_PTR)0, cb.GetItemData(-1));

    // ResetContent drops the payloads with the strings
    cb.ResetContent();
    EXPECT_EQ((DWORD_PTR)0, cb.GetItemData(0));
}

// The CLuaChooseBox round trip in miniature: build the combo the way
// OnInitDialog does, select an entry, and read the key back the way OnOK does.
TEST(Controls, ComboBoxItemDataSurvivesTheChooseBoxPattern) {
    fakewx::Install hooks;
    CComboBox cb;
    cb.m_hWnd = (HWND)fakewx::handle_for(73);

    static const char* const kValues[] = { "Third", "First", "Second" };
    for (int i = 0; i < 3; i++) {
        int pos = cb.AddString(kValues[i]);
        ASSERT_NE(CB_ERR, pos);
        cb.SetItemData(pos, i);
    }

    ASSERT_NE(CB_ERR, cb.SelectString(-1, "Second"));
    const int which = cb.GetCurSel();
    ASSERT_NE(CB_ERR, which);
    EXPECT_EQ((DWORD_PTR)2, cb.GetItemData(which));   // NOT 0
}

TEST(Controls, ComboBoxNullHooksReturnErrors) {
    CComboBox bare;
    EXPECT_EQ(CB_ERR, bare.AddString("x"));
    EXPECT_EQ(0, bare.GetCount());
    EXPECT_EQ(CB_ERR, bare.GetCurSel());
    EXPECT_EQ(CB_ERR, bare.FindStringExact(-1, "x"));
    bare.ResetContent();
    CString s;
    bare.GetLBText(0, s);
    EXPECT_TRUE(s.IsEmpty());
    bare.SetItemData(0, 1);
    EXPECT_EQ((DWORD_PTR)0, bare.GetItemData(0));
}

TEST(Controls, ButtonAndStaticAndEdit) {
    fakewx::Install hooks;
    CButton b;
    b.m_hWnd = (HWND)fakewx::handle_for(72);
    b.SetCheck(1);
    EXPECT_EQ(1, b.GetCheck());
    b.SetCheck(0);
    EXPECT_EQ(0, b.GetCheck());
    CButton bare;
    bare.m_hWnd = nullptr;
    // null hooks (uninstalled) exercised in ListBoxNullHooksReturnErrors

    CStatic st;
    st.m_hWnd = (HWND)fakewx::handle_for(73);
    st.SetWindowText("static text");
    CString s;
    st.GetWindowText(s);
    EXPECT_STREQ("static text", (LPCTSTR)s);

    // CEdit line info routes op codes through one hook
    static int s_lastArg;
    g_wxhook_edit_line_info = [](void*, int op, int arg) -> int {
        s_lastArg = arg;
        return 10 + op;
    };
    CEdit e;
    e.m_hWnd = (HWND)fakewx::handle_for(74);
    EXPECT_EQ(10, e.GetLineCount());     // op 0
    EXPECT_EQ(11, e.LineIndex(3));       // op 1
    EXPECT_EQ(3, s_lastArg);
    EXPECT_EQ(12, e.LineLength(-1));     // op 2
    EXPECT_EQ(13, e.LineFromChar(5));    // op 3
    EXPECT_EQ(14, e.GetWindowTextLength());   // op 4
    g_wxhook_edit_line_info = nullptr;
    EXPECT_EQ(0, e.GetLineCount());

    static int s_selStart, s_selEnd;
    static std::string s_replaced;
    g_wxhook_edit_set_sel = [](void*, int s2, int e2) { s_selStart = s2; s_selEnd = e2; };
    g_wxhook_edit_get_sel = [](void*, int* s2, int* e2) { *s2 = 4; *e2 = 9; };
    g_wxhook_edit_replace_sel = [](void*, const char* t) { s_replaced = t; };
    e.SetSel(1, 5);
    EXPECT_EQ(1, s_selStart);
    EXPECT_EQ(5, s_selEnd);
    int a = 0, z = 0;
    e.GetSel(a, z);
    EXPECT_EQ(4, a); EXPECT_EQ(9, z);
    DWORD da = 0, dz = 0;
    e.GetSel(da, dz);
    EXPECT_EQ((DWORD)4, da); EXPECT_EQ((DWORD)9, dz);
    e.ReplaceSel("typed");
    EXPECT_STREQ("typed", s_replaced.c_str());
    g_wxhook_edit_set_sel = nullptr;
    g_wxhook_edit_get_sel = nullptr;
    g_wxhook_edit_replace_sel = nullptr;
}

// LimitText and SetReadOnly were `{}`, so utils.inputbox / utils.editbox's
// max_length and read_only options did nothing at all on the port: the reply
// field took unlimited text, and a display-only box stayed editable.
TEST(Controls, EditLengthLimitAndReadOnlyReachTheControl) {
    static int  s_limit = -1;
    static bool s_readOnly = false, s_readOnlyCalled = false;
    void (*const savedLimit)(void*, int)  = g_wxhook_edit_set_max_length;
    void (*const savedRo)(void*, bool)    = g_wxhook_edit_set_readonly;
    g_wxhook_edit_set_max_length = [](void*, int n) { s_limit = n; };
    g_wxhook_edit_set_readonly   = [](void*, bool ro) { s_readOnly = ro; s_readOnlyCalled = true; };

    CEdit e;
    e.m_hWnd = (HWND)(void*)0x77;
    e.LimitText(80);
    EXPECT_EQ(80, s_limit);
    e.LimitText();                      // MFC's default argument: remove the limit
    EXPECT_EQ(0, s_limit);

    e.SetReadOnly(TRUE);
    EXPECT_TRUE(s_readOnlyCalled);
    EXPECT_TRUE(s_readOnly);
    e.SetReadOnly(FALSE);               // and it really is a two-way switch
    EXPECT_FALSE(s_readOnly);
    e.SetReadOnly();                    // MFC's default argument is TRUE
    EXPECT_TRUE(s_readOnly);

    // Headless (no hooks): both stay safe rather than dereferencing a handle.
    g_wxhook_edit_set_max_length = nullptr;
    g_wxhook_edit_set_readonly   = nullptr;
    e.LimitText(10);
    e.SetReadOnly(TRUE);

    g_wxhook_edit_set_max_length = savedLimit;
    g_wxhook_edit_set_readonly   = savedRo;
}

TEST(Controls, ListCtrlHookedOperations) {
    fakewx::Install hooks;
    CListCtrl lc;
    lc.m_hWnd = (HWND)fakewx::handle_for(75);
    EXPECT_EQ(0, lc.InsertColumn(0, "Name"));
    EXPECT_EQ(1, lc.InsertColumn(1, "Value", LVCFMT_LEFT, 80));
    EXPECT_EQ(0, lc.InsertItem(0, "rowA"));
    EXPECT_EQ(1, lc.InsertItem(1, "rowB"));
    LVITEM item = {};
    item.iItem = 2;
    item.pszText = (char*)"rowC";
    EXPECT_EQ(2, lc.InsertItem(&item));
    EXPECT_EQ(-1, lc.InsertItem(nullptr));
    EXPECT_EQ(3, lc.GetItemCount());
    EXPECT_TRUE(lc.SetItemText(1, 1, "forty-two"));
    EXPECT_STREQ("forty-two", fakewx::rows()[1].cols[1].c_str());
    EXPECT_TRUE(lc.SetItemData(0, (DWORD_PTR)30));
    EXPECT_TRUE(lc.SetItemData(1, (DWORD_PTR)10));
    EXPECT_TRUE(lc.SetItemData(2, (DWORD_PTR)20));
    EXPECT_EQ((DWORD_PTR)10, lc.GetItemData(1));
    // selection state
    EXPECT_EQ(0, lc.GetSelectedCount());
    EXPECT_TRUE(lc.SetItemState(1, 0x0002, 0x0002));   // LVIS_SELECTED
    EXPECT_EQ(1, lc.GetSelectedCount());
    EXPECT_EQ(0x0002u, lc.GetItemState(1, 0x0002));
    EXPECT_EQ(0u, lc.GetItemState(0, 0x0002));
    EXPECT_EQ(1, lc.GetNextItem(-1, 0x0002));
    EXPECT_EQ(-1, lc.GetNextItem(1, 0x0002));
    // sort by item data ascending
    lc.SortItems([](LPARAM a, LPARAM b, LPARAM) -> int {
        return (a < b) ? -1 : (a > b) ? 1 : 0;
    }, 0);
    EXPECT_EQ((DWORD_PTR)10, lc.GetItemData(0));
    EXPECT_EQ((DWORD_PTR)20, lc.GetItemData(1));
    EXPECT_EQ((DWORD_PTR)30, lc.GetItemData(2));
    lc.EnsureVisible(2);
    EXPECT_TRUE(lc.SetColumnWidth(0, 120));
    EXPECT_TRUE(lc.DeleteItem(0));
    EXPECT_EQ(2, lc.GetItemCount());
    lc.DeleteAllItems();
    EXPECT_EQ(0, lc.GetItemCount());
    // null-hook arms
    CListCtrl bare;
    fakewx::Install cleanup2;   // fresh store, then drop hooks below
    g_wxhook_listctrl_insert_item = nullptr;
    g_wxhook_listctrl_get_item_count = nullptr;
    g_wxhook_listctrl_get_item_data = nullptr;
    g_wxhook_listctrl_get_next_item = nullptr;
    g_wxhook_listctrl_get_selected_count = nullptr;
    g_wxhook_listctrl_get_item_state = nullptr;
    EXPECT_EQ(-1, bare.InsertItem(0, "x"));
    EXPECT_EQ(0, bare.GetItemCount());
    EXPECT_EQ((DWORD_PTR)0, bare.GetItemData(0));
    EXPECT_EQ(-1, bare.GetNextItem(-1, 0));
    EXPECT_EQ(0, bare.GetSelectedCount());
    EXPECT_EQ(0u, bare.GetItemState(0, 0xFF));
}

// ═══════════════════════════════════════════════════════════════════════════
// CMenu
// ═══════════════════════════════════════════════════════════════════════════

TEST(Menu, ArenaAndHandleLookup) {
    CMenu::ResetArena();
    CMenu* m1 = CMenu::NewPopup();
    CMenu* m2 = CMenu::NewPopup();
    ASSERT_TRUE(m1 != nullptr);
    ASSERT_TRUE(m2 != nullptr);
    EXPECT_NE((intptr_t)m1->m_hMenu, (intptr_t)m2->m_hMenu);
    EXPECT_EQ(m1, CMenu::FromMenuHandle(m1->m_hMenu));
    EXPECT_EQ(m2, CMenu::FromMenuHandle(m2->m_hMenu));
    EXPECT_EQ((CMenu*)nullptr, CMenu::FromMenuHandle((HMENU)(intptr_t)999));
    EXPECT_EQ((CMenu*)nullptr, CMenu::FromMenuHandle((HMENU)(intptr_t)0));
    CMenu::ResetArena();
    EXPECT_EQ((CMenu*)nullptr, CMenu::FromMenuHandle((HMENU)(intptr_t)1));
}

TEST(Menu, AppendAndTrackPopup) {
    CMenu m;
    EXPECT_TRUE(m.CreatePopupMenu());
    EXPECT_TRUE(m.AppendMenu(MF_STRING, 101, "First"));
    EXPECT_TRUE(m.AppendMenu(MF_SEPARATOR));
    EXPECT_TRUE(m.AppendMenu(MF_STRING | MF_CHECKED, 102, "Second"));
    EXPECT_EQ(3u, m.GetMenuItemCount());
    EXPECT_STREQ("First", m.m_items[0].text.c_str());
    EXPECT_EQ((UINT_PTR)102, m.m_items[2].arg);

    // Headless (no hook): TrackPopupMenu returns 0
    EXPECT_EQ(0u, m.TrackPopupMenu(0, 10, 10, nullptr));

    static int s_seenCount;
    g_wxhook_popup_menu = [](CMenu* menu, int, int, unsigned) -> int {
        s_seenCount = (int)menu->GetMenuItemCount();
        return 102;
    };
    EXPECT_EQ(102u, m.TrackPopupMenu(0, 10, 10, nullptr));
    EXPECT_EQ(3, s_seenCount);
    g_wxhook_popup_menu = nullptr;

    // The anchor AND the TPM_* alignment bits must reach the hook: Win32
    // TrackPopupMenu places the popup at (x, y) with the alignment deciding
    // which edge of the menu lands there, and CMiniWindow::Menu builds that
    // alignment word from world.WindowMenu's leading "~xy" spec.  The shim used
    // to drop `flags` on the floor (and the wx hook then ignored x/y too), so
    // every plugin menu opened at the mouse cursor, top-left aligned.
    static int s_x, s_y; static unsigned s_flags;
    g_wxhook_popup_menu = [](CMenu*, int x, int y, unsigned flags) -> int {
        s_x = x; s_y = y; s_flags = flags;
        return 101;
    };
    EXPECT_EQ(101u, m.TrackPopupMenu(TPM_CENTERALIGN | TPM_BOTTOMALIGN |
                                     TPM_RETURNCMD | TPM_NONOTIFY,
                                     317, 208, nullptr));
    EXPECT_EQ(317, s_x);
    EXPECT_EQ(208, s_y);
    EXPECT_TRUE((s_flags & TPM_CENTERALIGN) != 0);
    EXPECT_TRUE((s_flags & TPM_BOTTOMALIGN) != 0);
    EXPECT_TRUE((s_flags & TPM_RETURNCMD) != 0);
    g_wxhook_popup_menu = nullptr;

    // Attach / Detach round-trip
    HMENU h = m.Detach();
    EXPECT_EQ((HMENU)0, m.m_hMenu);
    EXPECT_TRUE(m.Attach(h));
    EXPECT_EQ(h, m.GetSafeHmenu());
    EXPECT_TRUE(m.DestroyMenu());
    EXPECT_EQ(0u, m.GetMenuItemCount());
    EXPECT_TRUE(m.CreateMenu());
}

// GetCursorPos: headless has no hook (g_wxhook_get_cursor_pos is null, same as
// every other g_wxhook_* in this library), so it must report failure and leave
// its out-param alone -- and, per the null-pointer contract every other
// GetXxx-style stub in this header follows, a null POINT*/CPoint* must not be
// dereferenced.  Then with a hook installed (standing in for wxGetMousePosition,
// which mushcore cannot call -- wx is not linked here), it must report SUCCESS
// and hand back exactly what the hook supplied: not merely "not (0,0)", which
// is a legitimate on-screen position and would not catch e.g. a swapped x/y or
// a hook whose return value is ignored.
TEST(MfcUi, GetCursorPosNullHookFails) {
    EXPECT_EQ(nullptr, (void*)g_wxhook_get_cursor_pos);
    POINT p{111, 222};
    EXPECT_FALSE(GetCursorPos(&p));
    // Untouched -- a stub that fails must not have written through the pointer.
    EXPECT_EQ(111, p.x);
    EXPECT_EQ(222, p.y);
    EXPECT_FALSE(GetCursorPos((POINT*)nullptr));

    CPoint cp(111, 222);
    EXPECT_FALSE(GetCursorPos(&cp));
    EXPECT_EQ(111, cp.x);
    EXPECT_EQ(222, cp.y);
    EXPECT_FALSE(GetCursorPos((CPoint*)nullptr));
}

TEST(MfcUi, GetCursorPosWithHookSucceedsAndMatches) {
    static int s_x, s_y;
    s_x = 317; s_y = -42;   // negative is a legitimate secondary-monitor coord
    g_wxhook_get_cursor_pos = [](int* x, int* y) -> int {
        *x = s_x; *y = s_y;
        return 1;
    };

    POINT p{0, 0};
    ASSERT_TRUE(GetCursorPos(&p));
    EXPECT_EQ(s_x, p.x);
    EXPECT_EQ(s_y, p.y);

    CPoint cp;
    ASSERT_TRUE(GetCursorPos(&cp));
    EXPECT_EQ(s_x, cp.x);
    EXPECT_EQ(s_y, cp.y);

    // The pointer really did move: a second "sample" taken after it disagrees
    // with the first, and GetCursorPos tracks it -- proving this is reading
    // the hook's current answer each call, not a value cached at install time.
    s_x = -8; s_y = 900;
    ASSERT_TRUE(GetCursorPos(&p));
    EXPECT_EQ(-8, p.x);
    EXPECT_EQ(900, p.y);

    // A null out-param must still fail even with a working hook installed.
    EXPECT_FALSE(GetCursorPos((POINT*)nullptr));
    EXPECT_FALSE(GetCursorPos((CPoint*)nullptr));

    g_wxhook_get_cursor_pos = nullptr;
}

// TpmAlignAnchor: the port's stand-in for what Win32 does with the TPM_*ALIGN
// bits.  wx can only place a popup by its top-left corner, so the anchor moves
// by the menu's size instead.
TEST(Menu, TpmAlignAnchorMovesTheAnchorByTheMenuSize) {
    const int W = 120, H = 80;

    // Default alignment (both flags are zero) must not move anything.
    int x = 500, y = 400;
    TpmAlignAnchor(TPM_LEFTALIGN | TPM_TOPALIGN, W, H, &x, &y);
    EXPECT_EQ(500, x);
    EXPECT_EQ(400, y);

    // Right/bottom: the menu's right-bottom corner lands on the anchor.
    x = 500; y = 400;
    TpmAlignAnchor(TPM_RIGHTALIGN | TPM_BOTTOMALIGN, W, H, &x, &y);
    EXPECT_EQ(500 - W, x);
    EXPECT_EQ(400 - H, y);

    // Centre/vcentre: the menu straddles the anchor.
    x = 500; y = 400;
    TpmAlignAnchor(TPM_CENTERALIGN | TPM_VCENTERALIGN, W, H, &x, &y);
    EXPECT_EQ(500 - W / 2, x);
    EXPECT_EQ(400 - H / 2, y);

    // The two axes are independent: "~cb" (centre horizontally, bottom
    // vertically) is the idiom for dropping a menu under a toolbar button.
    x = 500; y = 400;
    TpmAlignAnchor(TPM_CENTERALIGN | TPM_TOPALIGN, W, H, &x, &y);
    EXPECT_EQ(500 - W / 2, x);
    EXPECT_EQ(400, y);

    // CENTER wins over RIGHT if both are somehow set, as Win32 documents.
    x = 500; y = 400;
    TpmAlignAnchor(TPM_CENTERALIGN | TPM_RIGHTALIGN, W, H, &x, &y);
    EXPECT_EQ(500 - W / 2, x);

    // Null outs are tolerated (one axis only).
    x = 500;
    TpmAlignAnchor(TPM_RIGHTALIGN, W, H, &x, nullptr);
    EXPECT_EQ(500 - W, x);
}

// ═══════════════════════════════════════════════════════════════════════════
// COleVariant / SAFEARRAY / COleSafeArray
// ═══════════════════════════════════════════════════════════════════════════

TEST(Variant, ConstructorsAndAssignment) {
    COleVariant empty;
    EXPECT_EQ((int)VT_EMPTY, (int)empty.vt);
    COleVariant l(123L);
    EXPECT_EQ((int)VT_I4, (int)l.vt);
    EXPECT_EQ(123L, (long)l);
    COleVariant d(1.25);
    EXPECT_EQ((int)VT_R8, (int)d.vt);
    EXPECT_NEAR(1.25, (double)d, 1e-12);
    COleVariant s("text");
    EXPECT_EQ((int)VT_BSTR, (int)s.vt);
    EXPECT_STREQ("text", s.bstrVal);
    COleVariant b(true);
    EXPECT_EQ((int)VT_BOOL, (int)b.vt);
    EXPECT_EQ(-1, (int)b.boolVal);
    COleVariant bf(false);
    EXPECT_EQ(0, (int)bf.boolVal);

    COleVariant v;
    v = 5L;          EXPECT_EQ((int)VT_I4, (int)v.vt);
    v = 2.5;         EXPECT_EQ((int)VT_R8, (int)v.vt);
    v = "str";       EXPECT_EQ((int)VT_BSTR, (int)v.vt);
    v = CString("cstr");
    EXPECT_EQ((int)VT_BSTR, (int)v.vt);
    v = true;        EXPECT_EQ((int)VT_BOOL, (int)v.vt);
    v.Clear();
    EXPECT_EQ((int)VT_EMPTY, (int)v.vt);
}

TEST(Variant, ChangeTypeToI4) {
    COleVariant v(2.9);
    v.ChangeType(VT_I4);
    EXPECT_EQ((int)VT_I4, (int)v.vt);
    EXPECT_EQ(2L, v.lVal);          // C truncation, not rounding
    COleVariant b(true);
    b.ChangeType(VT_I4);
    EXPECT_EQ(1L, b.lVal);
    COleVariant s("42abc");
    s.ChangeType(VT_I4);
    EXPECT_EQ(42L, s.lVal);
    COleVariant same(7L);
    same.ChangeType(VT_I4);          // no-op fast path
    EXPECT_EQ(7L, same.lVal);
}

TEST(Variant, ChangeTypeToR8AndBool) {
    COleVariant l(3L);
    l.ChangeType(VT_R8);
    EXPECT_EQ((int)VT_R8, (int)l.vt);
    EXPECT_NEAR(3.0, l.dblVal, 1e-12);
    COleVariant b(true);
    b.ChangeType(VT_R8);
    EXPECT_NEAR(1.0, b.dblVal, 1e-12);
    COleVariant s("2.5");
    s.ChangeType(VT_R8);
    EXPECT_NEAR(2.5, s.dblVal, 1e-12);

    COleVariant toB(0L);
    toB.ChangeType(VT_BOOL);
    EXPECT_EQ((int)VT_BOOL, (int)toB.vt);
    EXPECT_EQ(0, (int)toB.boolVal);
    COleVariant toB2(0.5);
    toB2.ChangeType(VT_BOOL);
    EXPECT_EQ(-1, (int)toB2.boolVal);
    COleVariant toB3("");
    toB3.ChangeType(VT_BOOL);
    EXPECT_EQ(0, (int)toB3.boolVal);   // empty string is false
    COleVariant toB4("x");
    toB4.ChangeType(VT_BOOL);
    EXPECT_EQ(-1, (int)toB4.boolVal);
}

TEST(Variant, ChangeTypeToBstrAndNull) {
    COleVariant l(42L);
    l.ChangeType(VT_BSTR);
    EXPECT_EQ((int)VT_BSTR, (int)l.vt);
    EXPECT_STREQ("42", l.bstrVal);
    COleVariant d(2.5);
    d.ChangeType(VT_BSTR);
    EXPECT_STREQ("2.5", d.bstrVal);
    COleVariant b(true);
    b.ChangeType(VT_BSTR);
    EXPECT_STREQ("1", b.bstrVal);
    COleVariant n(7L);
    n.ChangeType(VT_NULL);
    EXPECT_EQ((int)VT_NULL, (int)n.vt);
}

TEST(Variant, AttachDetachTransferOwnership) {
    VARIANT raw;
    raw.vt = VT_I4;
    raw.lVal = 99;
    COleVariant v;
    v.Attach(raw);
    EXPECT_EQ((int)VT_I4, (int)v.vt);
    EXPECT_EQ(99L, v.lVal);
    EXPECT_EQ((int)VT_EMPTY, (int)raw.vt);   // source cleared
    VARIANT out;
    v.Detach(out);
    EXPECT_EQ((int)VT_I4, (int)out.vt);
    EXPECT_EQ(99L, out.lVal);
    EXPECT_EQ((int)VT_EMPTY, (int)v.vt);
}

TEST(SafeArray, CreatePutGetDetach) {
    COleSafeArray sa;
    EXPECT_EQ(0L, sa.GetOneDimSize());
    sa.CreateOneDim(VT_VARIANT, 3);
    EXPECT_EQ(3L, sa.GetOneDimSize());

    COleVariant el;
    long idx = 0;
    el = 10L;             sa.PutElement(&idx, &el);
    idx = 1; el = "two";  sa.PutElement(&idx, &el);
    idx = 2; el = 3.5;    sa.PutElement(&idx, &el);
    idx = 99;             sa.PutElement(&idx, &el);   // out of range: dropped
    idx = -1;             sa.PutElement(&idx, &el);
    sa.PutElement(nullptr, &el);
    idx = 0;
    sa.PutElement(&idx, nullptr);

    VARIANT got;
    idx = 0;
    sa.GetElement(&idx, &got);
    EXPECT_EQ((int)VT_I4, (int)got.vt);
    EXPECT_EQ(10L, got.lVal);
    idx = 1;
    sa.GetElement(&idx, &got);
    EXPECT_EQ((int)VT_BSTR, (int)got.vt);
    EXPECT_STREQ("two", got.bstrVal);

    void* data = nullptr;
    sa.AccessData(&data);
    EXPECT_TRUE(data != nullptr);
    EXPECT_EQ(10L, ((VARIANT*)data)[0].lVal);
    sa.UnaccessData();

    // bounds queries through the free functions
    long lb = -99, ub = -99;
    SafeArrayGetLBound(sa.m_psa, 1, &lb);
    SafeArrayGetUBound(sa.m_psa, 1, &ub);
    EXPECT_EQ(0L, lb);
    EXPECT_EQ(2L, ub);

    COleVariant detached = sa.Detach();
    EXPECT_EQ((int)(VT_ARRAY | VT_VARIANT), (int)detached.vt);
    EXPECT_TRUE(detached.parray != nullptr);
    EXPECT_EQ(0L, sa.GetOneDimSize());               // ownership moved out
    SafeArrayDestroy(detached.parray);               // we own it now
}

TEST(SafeArray, NeverCreatedDetachesAsEmpty) {
    // MFC contract: COleSafeArray IS a VARIANT starting VT_EMPTY; a
    // never-created array must detach as VT_EMPTY (Lua nil), not VT_ARRAY.
    COleSafeArray sa;
    COleVariant v = sa.Detach();
    EXPECT_EQ((int)VT_EMPTY, (int)v.vt);
    VARIANT raw;
    COleSafeArray sb;
    sb.Detach(raw);
    EXPECT_EQ((int)VT_EMPTY, (int)raw.vt);
    EXPECT_EQ((SAFEARRAY*)nullptr, raw.parray);
    // conversion operator has the same contract
    COleVariant conv = (COleVariant)sb;
    EXPECT_EQ((int)VT_EMPTY, (int)conv.vt);
}

TEST(SafeArray, AttachClearAndSharedConversion) {
    COleSafeArray sa;
    sa.CreateOneDim(VT_VARIANT, 1);
    long idx = 0;
    COleVariant el("elem");
    sa.PutElement(&idx, &el);

    // conversion shares (does not detach)
    COleVariant shared = (COleVariant)sa;
    EXPECT_EQ((int)(VT_ARRAY | VT_VARIANT), (int)shared.vt);
    EXPECT_EQ(1L, sa.GetOneDimSize());

    // Detach into raw VARIANT, re-Attach into a fresh wrapper
    VARIANT raw;
    sa.Detach(raw);
    COleSafeArray sb;
    sb.Attach(raw);
    EXPECT_EQ((int)VT_EMPTY, (int)raw.vt);
    EXPECT_EQ(1L, sb.GetOneDimSize());
    sb.Clear();
    EXPECT_EQ(0L, sb.GetOneDimSize());
    sb.Clear();   // double-clear safe
}

TEST(SafeArray, FreeFunctionFailureArms) {
    long idx = 0;
    VARIANT out;
    EXPECT_EQ(E_FAIL, SafeArrayGetElement((SAFEARRAY*)nullptr, &idx, &out));
    COleSafeArray sa;
    sa.CreateOneDim(VT_VARIANT, 2);
    EXPECT_EQ(E_FAIL, SafeArrayGetElement(sa.m_psa, (long*)nullptr, &out));
    EXPECT_EQ(E_FAIL, SafeArrayGetElement(sa.m_psa, &idx, nullptr));
    idx = 5;
    EXPECT_EQ(E_FAIL, SafeArrayGetElement(sa.m_psa, &idx, &out));
    idx = -1;
    EXPECT_EQ(E_FAIL, SafeArrayGetElement(sa.m_psa, &idx, &out));
    long lb = 1, ub = 1;
    SafeArrayGetLBound((SAFEARRAY*)nullptr, 1, &lb);
    SafeArrayGetUBound((SAFEARRAY*)nullptr, 1, &ub);
    EXPECT_EQ(0L, lb);
    EXPECT_EQ(-1L, ub);
    EXPECT_EQ(S_OK, SafeArrayDestroy(nullptr));
}

// ═══════════════════════════════════════════════════════════════════════════
// COleDateTime / COleDateTimeSpan
// ═══════════════════════════════════════════════════════════════════════════

TEST(OleDateTime, FormatAndSystemTimeAgree) {
    // Port convention: m_dt stores raw Unix seconds.
    time_t t = 1700000000;   // 2023-11-14T22:13:20Z
    COleDateTime dt((double)t);
    struct tm expected = {};
    localtime_r(&t, &expected);

    SYSTEMTIME st;
    dt.GetAsSystemTime(st);
    EXPECT_EQ(expected.tm_year + 1900, (int)st.wYear);
    EXPECT_EQ(expected.tm_mon + 1, (int)st.wMonth);
    EXPECT_EQ(expected.tm_mday, (int)st.wDay);
    EXPECT_EQ(expected.tm_hour, (int)st.wHour);
    EXPECT_EQ(expected.tm_min, (int)st.wMinute);
    EXPECT_EQ(expected.tm_sec, (int)st.wSecond);
    EXPECT_EQ(expected.tm_wday, (int)st.wDayOfWeek);

    CString s = dt.Format((DWORD)0, (LCID)0);
    // Locked rendering: "%m/%d/%Y %I:%M:%S %p" — 22 chars, slashes fixed.
    EXPECT_EQ(22, s.GetLength());
    EXPECT_EQ('/', s[2]);
    EXPECT_EQ('/', s[5]);
    EXPECT_EQ(' ', s[10]);
}

TEST(OleDateTime, ComparisonsAndStatus) {
    COleDateTime a(100.0), b(200.0);
    EXPECT_TRUE(a < b);
    EXPECT_TRUE(a <= b);
    EXPECT_TRUE(b > a);
    EXPECT_TRUE(b >= a);
    EXPECT_TRUE(a != b);
    EXPECT_TRUE(a == COleDateTime(100.0));
    EXPECT_NEAR(100.0, (double)a, 1e-12);
    EXPECT_TRUE(a.IsValid());
    EXPECT_FALSE(a.IsNull());
    EXPECT_EQ((int)COleDateTime::valid, (int)a.GetStatus());
}

TEST(OleDateTime, SpanTotals) {
    COleDateTimeSpan sp(1, 12, 0, 0);   // 1.5 days
    EXPECT_NEAR(1.5, sp.GetTotalDays(), 1e-9);
    EXPECT_NEAR(36.0, sp.GetTotalHours(), 1e-9);
    EXPECT_NEAR(2160.0, sp.GetTotalMinutes(), 1e-9);
    EXPECT_NEAR(129600.0, sp.GetTotalSeconds(), 1e-9);
    COleDateTimeSpan raw(0.25);
    EXPECT_NEAR(6.0, raw.GetTotalHours(), 1e-9);
}

// ═══════════════════════════════════════════════════════════════════════════
// Free utility functions
// ═══════════════════════════════════════════════════════════════════════════

TEST(Utils, MulDivRoundsToNearestLikeWin32) {
    EXPECT_EQ(5, MulDiv(3, 3, 2));       // 4.5 rounds away from zero
    EXPECT_EQ(-5, MulDiv(-3, 3, 2));
    EXPECT_EQ(33, MulDiv(10, 10, 3));    // 33.3 -> 33
    EXPECT_EQ(-1, MulDiv(1, 1, 0));      // divide-by-zero sentinel
    EXPECT_EQ(-5, MulDiv(3, 3, -2));     // negative divisor
    // The font-height case the comment cites: -MulDiv(pts, dpi, 72)
    EXPECT_EQ(13, MulDiv(10, 96, 72));   // 13.33 -> 13
}

TEST(Utils, PerformanceCounterAndTickCount) {
    LARGE_INTEGER freq, c1, c2;
    EXPECT_TRUE(QueryPerformanceFrequency(&freq));
    EXPECT_EQ((int64_t)1000000000LL, (int64_t)freq.QuadPart);
    EXPECT_TRUE(QueryPerformanceCounter(&c1));
    EXPECT_TRUE(QueryPerformanceCounter(&c2));
    EXPECT_GE((int64_t)c2.QuadPart, (int64_t)c1.QuadPart);
    QueryPerformanceFrequency(nullptr);   // null-arg guard
    QueryPerformanceCounter(nullptr);

    DWORD t1 = GetTickCount();
    Sleep(2);
    DWORD t2 = timeGetTime();
    EXPECT_GE(t2, t1);
}

TEST(Utils, CurrentDirectoryRoundtrip) {
    char expect[1024];
    ASSERT_TRUE(getcwd(expect, sizeof expect) != nullptr);
    char buf[1024];
    DWORD n = GetCurrentDirectory(sizeof buf, buf);
    EXPECT_EQ((DWORD)strlen(expect), n);
    EXPECT_STREQ(expect, buf);
    // size-query form returns needed size including NUL
    EXPECT_EQ((DWORD)strlen(expect) + 1, GetCurrentDirectory(0, nullptr));
    EXPECT_EQ((DWORD)strlen(expect) + 1, GetCurrentDirectory(1, buf));
    // SetCurrentDirectory round-trip
    EXPECT_TRUE(SetCurrentDirectory("/tmp"));
    char now[1024];
    getcwd(now, sizeof now);
    EXPECT_TRUE(strstr(now, "tmp") != nullptr);
    EXPECT_TRUE(SetCurrentDirectory(expect));
    EXPECT_FALSE(SetCurrentDirectory("/nonexistent/dir"));
    EXPECT_FALSE(SetCurrentDirectory(nullptr));
}

TEST(Utils, GetModuleFileNameFindsOurExecutable) {
    char buf[4096];
    DWORD n = GetModuleFileName(nullptr, buf, sizeof buf);
    EXPECT_GT((int)n, 0);
    EXPECT_TRUE(strstr(buf, "test_mfc_ui") != nullptr);
    EXPECT_EQ((DWORD)0, GetModuleFileName(nullptr, nullptr, 0));
}

TEST(Utils, FormatMessageMapsErrnoToStrerror) {
    // Allocate-buffer mode (the CException::GetErrorMessage path)
    char* heap = nullptr;
    DWORD n = FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                            nullptr, (DWORD)ENOENT, 0, (LPTSTR)&heap, 0, nullptr);
    ASSERT_TRUE(heap != nullptr);
    EXPECT_EQ((DWORD)strlen(strerror(ENOENT)), n);
    EXPECT_STREQ(strerror(ENOENT), heap);
    LocalFree(heap);
    // Fixed-buffer mode with truncation
    char small[6];
    n = FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, (DWORD)ENOENT, 0,
                      small, sizeof small, nullptr);
    EXPECT_EQ((DWORD)5, n);
    EXPECT_EQ('\0', small[5]);
    EXPECT_EQ(0, strncmp(strerror(ENOENT), small, 5));
    // Zero-size guard
    EXPECT_EQ((DWORD)0, FormatMessage(0, nullptr, (DWORD)ENOENT, 0, nullptr, 0, nullptr));
}

TEST(Utils, Utf8WideRoundtrip) {
    // 1-, 2-, 3- and 4-byte UTF-8 sequences.
    const char* utf8 = "A\xC3\xA9\xE2\x82\xAC\xF0\x9D\x84\x9E";   // A é € 𝄞
    // Count mode (no output buffer)
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    EXPECT_EQ(5, needed);   // 4 code points + NUL (wchar_t is 4 bytes here)
    wchar_t wide[8] = {};
    int written = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, 8);
    EXPECT_EQ(5, written);
    EXPECT_EQ((wchar_t)'A', wide[0]);
    EXPECT_EQ((wchar_t)0xE9, wide[1]);
    EXPECT_EQ((wchar_t)0x20AC, wide[2]);
    EXPECT_EQ((wchar_t)0x1D11E, wide[3]);
    EXPECT_EQ((wchar_t)0, wide[4]);
    // Back to UTF-8
    char back[32] = {};
    int nb = WideCharToMultiByte(CP_UTF8, 0, wide, -1, back, sizeof back, nullptr, nullptr);
    EXPECT_EQ((int)strlen(utf8) + 1, nb);
    EXPECT_STREQ(utf8, back);
    // Count mode for the reverse direction
    EXPECT_EQ(nb, WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr));
    // Null-input guards
    EXPECT_EQ(0, MultiByteToWideChar(CP_UTF8, 0, nullptr, -1, wide, 8));
    EXPECT_EQ(0, WideCharToMultiByte(CP_UTF8, 0, nullptr, -1, back, 8, nullptr, nullptr));
    // Explicit-length form (no NUL)
    EXPECT_EQ(1, MultiByteToWideChar(CP_UTF8, 0, "AB", 1, nullptr, 0));
}

TEST(Utils, MbsToWcsHelpers) {
    wchar_t wide[16];
    _mbstowcsz(wide, "abc", 16);
    EXPECT_EQ((wchar_t)'a', wide[0]);
    EXPECT_EQ((wchar_t)0, wide[3]);
    char narrow[16];
    _wcstombsz(narrow, wide, 16);
    EXPECT_STREQ("abc", narrow);
    // n == 0 early-outs
    EXPECT_EQ(wide, _mbstowcsz(wide, "x", 0));
    EXPECT_EQ(narrow, _wcstombsz(narrow, wide, 0));
}

TEST(Utils, CoCreateGuidIsVersion4) {
    GUID g1 = {}, g2 = {};
    EXPECT_EQ(S_OK, CoCreateGuid(&g1));
    EXPECT_EQ(S_OK, CoCreateGuid(&g2));
    EXPECT_EQ(0x4000, (int)(g1.Data3 & 0xF000));         // version nibble
    EXPECT_EQ(0x80, (int)(g1.Data4[0] & 0xC0));          // variant bits
    // Two GUIDs must differ (128 random bits)
    EXPECT_TRUE(memcmp(&g1, &g2, sizeof(GUID)) != 0);
    EXPECT_EQ(E_INVALIDARG, CoCreateGuid(nullptr));
}

TEST(Utils, AfxStringHelpers) {
    // AfxGetFileTitle: strips directory AND extension when a buffer is given
    char title[64];
    EXPECT_EQ(0u, AfxGetFileTitle("/a/b/world.mcl", title, sizeof title));
    EXPECT_STREQ("world", title);
    EXPECT_EQ(0u, AfxGetFileTitle("noext", title, sizeof title));
    EXPECT_STREQ("noext", title);
    // size-query form returns the length of the leaf WITH extension
    EXPECT_EQ((UINT)strlen("world.mcl"), AfxGetFileTitle("/a/b/world.mcl", nullptr, 0));
    // truncation to the buffer
    char tiny[3];
    AfxGetFileTitle("/a/longname.txt", tiny, sizeof tiny);
    EXPECT_STREQ("lo", tiny);

    CString out;
    AfxFormatString1(out, 0, "sub");
    EXPECT_STREQ("sub", (LPCTSTR)out);
    AfxFormatString1(out, 0, nullptr);
    EXPECT_STREQ("", (LPCTSTR)out);
    // IDS_MIN_STRING_SIZE (resource.h:1442) is the one id that has to carry a
    // sentence: DDV_MinMaxString builds its "too short" prompt through it, and
    // the passthrough made that modal read as a bare number.  The passthrough
    // stays for every other id -- the two assertions above are the control.
    AfxFormatString1(out, 32337, "24");
    EXPECT_STREQ("Please enter at least 24 characters.", (LPCTSTR)out);
    AfxFormatString1(out, 32337, nullptr);
    EXPECT_STREQ("Please enter at least  characters.", (LPCTSTR)out);

    CString sub;
    EXPECT_TRUE(AfxExtractSubString(sub, "a\nbb\nccc", 1));
    EXPECT_STREQ("bb", (LPCTSTR)sub);
    EXPECT_TRUE(AfxExtractSubString(sub, "a\nbb\nccc", 2));
    EXPECT_STREQ("ccc", (LPCTSTR)sub);
    EXPECT_TRUE(AfxExtractSubString(sub, "a\nbb\nccc", 0));
    EXPECT_STREQ("a", (LPCTSTR)sub);
    EXPECT_FALSE(AfxExtractSubString(sub, "a\nbb", 5));
    EXPECT_FALSE(AfxExtractSubString(sub, nullptr, 0));
    EXPECT_TRUE(AfxExtractSubString(sub, "x,y", 1, ','));
    EXPECT_STREQ("y", (LPCTSTR)sub);

    // AfxMessageBox must put up the SAME box TMessageBox does, not a stderr
    // line.  It was a stub returning a hardcoded IDOK, which made all twelve of
    // its live call sites in shared code invisible on this platform -- the
    // startup "Can't open global preferences database", the plugins' save-state
    // folder complaint, "Cannot open the Clipboard", the corrupt-tips-file
    // reports, the Lua PANIC handler and the per-item world-load errors.
    //
    // Both halves are asserted: that the hook SAW the text (a stub prints to
    // stderr and the hook never fires), and that the hook's ANSWER comes back
    // (a stub returns IDOK whatever the user picked -- which is not even one of
    // the buttons when the caller passed MB_YESNO).
    {
        static std::string s_seen; static UINT s_type = 0; static int s_answer = IDOK;
        s_seen.clear(); s_type = 0;
        int (*const saved)(const char*, const char*, unsigned) = g_wxhook_message_box;
        g_wxhook_message_box = [](const char* text, const char*, unsigned type) -> int {
            s_seen = text ? text : ""; s_type = type; return s_answer;
        };

        s_answer = IDOK;
        EXPECT_EQ(IDOK, AfxMessageBox("test message"));
        EXPECT_STREQ("test message", s_seen.c_str());

        s_seen.clear();
        s_answer = IDNO;
        EXPECT_EQ(IDNO, AfxMessageBox("really?", MB_YESNO));
        EXPECT_STREQ("really?", s_seen.c_str());
        EXPECT_EQ((UINT) MB_YESNO, s_type);

        // The resource-id overload has no string table to look in, but must
        // still SHOW something rather than swallow the error.
        s_seen.clear(); s_answer = IDOK;
        EXPECT_EQ(IDOK, AfxMessageBox((UINT)1234));
        EXPECT_TRUE(s_seen.find("1234") != std::string::npos);

        g_wxhook_message_box = saved;
    }
    EXPECT_STREQ("MUSHclient", AfxGetAppName());
}

TEST(Utils, GlobalAndLocalAllocators) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, 16);
    ASSERT_TRUE(h != nullptr);
    void* p = GlobalLock(h);
    EXPECT_EQ(h, p);
    memset(p, 0xAB, 16);
    EXPECT_TRUE(GlobalUnlock(h));
    h = GlobalReAlloc(h, 32, 0);
    ASSERT_TRUE(h != nullptr);
    EXPECT_EQ((HGLOBAL)nullptr, GlobalFree(h));
    HLOCAL lh = LocalAlloc(0, 8);
    ASSERT_TRUE(lh != nullptr);
    EXPECT_EQ(lh, LocalLock(lh));
    LocalUnlock(lh);
    EXPECT_EQ((HLOCAL)nullptr, LocalFree(lh));
}

static int s_beeps;

TEST(Utils, MessageBeepRoutesToBeepHook) {
    s_beeps = 0;
    g_fn_system_beep = []() { ++s_beeps; };
    EXPECT_TRUE(MessageBeep(0));
    EXPECT_TRUE(Beep(440, 100));
    EXPECT_EQ(2, s_beeps);
    g_fn_system_beep = nullptr;
    EXPECT_TRUE(MessageBeep(0));   // silent but still TRUE
    EXPECT_EQ(2, s_beeps);
}

static int s_msgboxType;

TEST(Utils, MessageBoxHookAndFallback) {
    // Headless fallback: IDNO for MB_YESNO, IDOK otherwise
    EXPECT_EQ(IDOK, MessageBox(nullptr, "msg", "cap", MB_OK));
    EXPECT_EQ(IDNO, MessageBox(nullptr, "msg", nullptr, MB_YESNO));
    g_wxhook_message_box = [](const char*, const char*, unsigned type) -> int {
        s_msgboxType = (int)type;
        return IDYES;
    };
    EXPECT_EQ(IDYES, MessageBox(nullptr, "msg", "cap", MB_YESNO));
    EXPECT_EQ(MB_YESNO, s_msgboxType & MB_YESNO);
    g_wxhook_message_box = nullptr;
}

TEST(Utils, ClipboardFormatAvailability) {
    EXPECT_FALSE(IsClipboardFormatAvailable(CF_TEXT));   // no hook
    g_wxhook_get_clipboard_text = []() -> char* { return strdup("clip"); };
    EXPECT_TRUE(IsClipboardFormatAvailable(CF_TEXT));
    EXPECT_TRUE(IsClipboardFormatAvailable(CF_UNICODETEXT));
    EXPECT_FALSE(IsClipboardFormatAvailable(CF_BITMAP));   // only text bridged
    g_wxhook_get_clipboard_text = []() -> char* { return nullptr; };
    EXPECT_FALSE(IsClipboardFormatAvailable(CF_TEXT));     // empty clipboard
    g_wxhook_get_clipboard_text = nullptr;
    EXPECT_EQ(0u, RegisterClipboardFormat("MUSHclient"));
    EXPECT_FALSE(OpenClipboard(nullptr));
    EXPECT_FALSE(CloseClipboard());
}

TEST(Utils, SysColorAndMetrics) {
    EXPECT_EQ((DWORD)0xC0C0C0, GetSysColor(1));   // headless light-grey default
    g_wxhook_get_sys_color = [](int idx) -> unsigned long { return (unsigned long)(idx * 2); };
    EXPECT_EQ((DWORD)10, GetSysColor(5));
    g_wxhook_get_sys_color = nullptr;
    EXPECT_EQ(0, GetSystemMetrics(SM_CXSCREEN));
    g_wxhook_get_system_metric = [](int idx) -> int { return idx + 100; };
    EXPECT_EQ(100, GetSystemMetrics(0));
    g_wxhook_get_system_metric = nullptr;
    EXPECT_EQ(0, (int)GetPixel((HDC)nullptr, 1, 2));
    g_wxhook_get_pixel = [](void*, int x, int y) -> unsigned long { return (unsigned long)(x + y); };
    EXPECT_EQ(3, (int)GetPixel((HDC)nullptr, 1, 2));
    g_wxhook_get_pixel = nullptr;
}

TEST(Utils, ResourceLoadingRoutesThroughTextHook) {
    // FindResource only recognizes ("TEXT", MAKEINTRESOURCE(id)) pairs
    HRSRC h = FindResource(nullptr, (LPCTSTR)(uintptr_t)42, "TEXT");
    EXPECT_EQ((HRSRC)(uintptr_t)42, h);
    EXPECT_EQ((HRSRC)nullptr, FindResource(nullptr, (LPCTSTR)(uintptr_t)42, (LPCTSTR)(uintptr_t)10));
    EXPECT_EQ((HRSRC)nullptr, FindResource(nullptr, "name", "TEXT"));   // string name -> null
    EXPECT_EQ((HRSRC)nullptr, FindResource(nullptr, (LPCTSTR)(uintptr_t)42, "BITMAP"));
    EXPECT_EQ((HANDLE)h, LoadResource(nullptr, h));
    // No hook: zero size, null bytes
    EXPECT_EQ((DWORD)0, SizeofResource(nullptr, h));
    EXPECT_EQ((void*)nullptr, LockResource((HANDLE)h));
    g_wxhook_load_text_resource = [](int id, unsigned* size) -> const char* {
        static const char text[] = "licence text";
        if (size) *size = (unsigned)(sizeof text - 1);
        return (id == 42) ? text : nullptr;
    };
    EXPECT_EQ((DWORD)12, SizeofResource(nullptr, h));
    EXPECT_STREQ("licence text", (const char*)LockResource((HANDLE)h));
    g_wxhook_load_text_resource = nullptr;
}

TEST(Utils, CreateTextWindowHook) {
    EXPECT_FALSE(CreateTextWindow("body", "title", nullptr, 1, "Courier", 10,
                                  400, 0, 0, 0xFFFFFF, "", "", false, false,
                                  false, false, false, 0, 0));
    static long long s_docNum;
    g_wxhook_create_text_window = [](const char*, const char*, CMUSHclientDoc*,
                                     long long num, const char*, long, long,
                                     unsigned int, unsigned long, unsigned long,
                                     const char*, const char*, bool, bool, bool,
                                     bool, bool, int, int) -> void* {
        s_docNum = num;
        return (void*)1;
    };
    EXPECT_TRUE(CreateTextWindow("body", "title", nullptr, 7, "Courier", 10,
                                 400, 0, 0, 0xFFFFFF, "", "", false, false,
                                 false, false, false, 0, 0));
    EXPECT_EQ((long long)7, s_docNum);
    g_wxhook_create_text_window = nullptr;
}

TEST(Utils, ThreadStateAndVersion) {
    _AFX_THREAD_STATE* st = AfxGetThreadState();
    ASSERT_TRUE(st != nullptr);
    EXPECT_EQ(st, AfxGetThreadState());   // stable singleton
    OSVERSIONINFO vi = {};
    EXPECT_TRUE(GetVersionEx(&vi));
    EXPECT_EQ((DWORD)10, vi.dwMajorVersion);
    EXPECT_EQ((DWORD)VER_PLATFORM_WIN32_NT, vi.dwPlatformId);
    EXPECT_TRUE(GetVersionEx(nullptr));   // null-safe
}

// ═══════════════════════════════════════════════════════════════════════════
// CWnd basics
// ═══════════════════════════════════════════════════════════════════════════

TEST(Wnd, FromHandleIsStableAndNullSafe) {
    EXPECT_EQ((CWnd*)nullptr, CWnd::FromHandle(nullptr));
    void* h = (void*)0xABCD;
    CWnd* w1 = CWnd::FromHandle(h);
    CWnd* w2 = CWnd::FromHandle(h);
    ASSERT_TRUE(w1 != nullptr);
    EXPECT_EQ(w1, w2);                     // stable wrapper per handle
    EXPECT_EQ(h, (void*)w1->m_hWnd);
    EXPECT_EQ(w1, CWnd::FromHandlePermanent(h));
    CWnd* other = CWnd::FromHandle((void*)0xBEEF);
    EXPECT_NE(w1, other);
}

TEST(Wnd, TextAndStateRouteThroughHooks) {
    fakewx::Install hooks;
    CWnd w;
    w.m_hWnd = (HWND)fakewx::handle_for(80);
    w.SetWindowText("window text");
    CString s;
    w.GetWindowText(s);
    EXPECT_STREQ("window text", (LPCTSTR)s);
    w.EnableWindow(FALSE);
    EXPECT_FALSE(fakewx::widget(80).enabled);
    w.EnableWindow(TRUE);
    EXPECT_TRUE(fakewx::widget(80).enabled);
    EXPECT_EQ((HWND)fakewx::handle_for(80), (HWND)w);   // operator HWND
    // GetDlgItem returns the shared FromHandle wrapper for the child
    CWnd* child = w.GetDlgItem(81);
    ASSERT_TRUE(child != nullptr);
    EXPECT_EQ(fakewx::handle_for(81), (void*)child->m_hWnd);
}

TEST(Wnd, GetWindowTextWithoutHookIsEmpty) {
    CWnd w;
    CString s = "stale";
    w.GetWindowText(s);
    EXPECT_TRUE(s.IsEmpty());
    EXPECT_EQ((CWnd*)nullptr, w.GetDlgItem(5));
    EXPECT_EQ(0, w.GetWindowTextLength());
}

// mfc_ui.h CWnd::GetWindowPlacement: "rcNormalPosition is the window's bounds
// the way Windows reports them: parent-client coordinates for a child".  The
// position matters as much as the size — relayout code positions one control
// against another (CEditMultiLine::OnSize sizes its text box to the Cancel
// button's top), and this used to report the {0,0,w,h} client box, which pins
// every control's top at 0 and collapsed that dialog's text area to nothing.
TEST(Wnd, GetWindowPlacementReportsThePositionNotJustTheSize) {
    static RECT s_placement;
    s_placement.left = 5; s_placement.top = 217;
    s_placement.right = 55; s_placement.bottom = 231;
    g_wxhook_get_placement_rect = [](const void*, RECT* r) { *r = s_placement; };
    CWnd w;
    WINDOWPLACEMENT wp;
    EXPECT_TRUE(w.GetWindowPlacement(&wp));
    EXPECT_EQ(5,   (int)wp.rcNormalPosition.left);
    EXPECT_EQ(217, (int)wp.rcNormalPosition.top);      // NOT 0
    EXPECT_EQ(55,  (int)wp.rcNormalPosition.right);
    EXPECT_EQ(231, (int)wp.rcNormalPosition.bottom);
    EXPECT_EQ(1u,  wp.showCmd);
    // GetButtonSize() reads only the extent, which must survive the change.
    EXPECT_EQ(50, (int)(wp.rcNormalPosition.right - wp.rcNormalPosition.left));
    EXPECT_EQ(14, (int)(wp.rcNormalPosition.bottom - wp.rcNormalPosition.top));

    // Headless has no window geometry: fall back to the client rect, so the
    // size stays meaningful and the origin is 0,0.
    static RECT s_client;
    s_client.left = 0; s_client.top = 0; s_client.right = 120; s_client.bottom = 40;
    g_wxhook_get_placement_rect = nullptr;
    g_wxhook_get_client_rect = [](const void*, RECT* r) { *r = s_client; };
    EXPECT_TRUE(w.GetWindowPlacement(&wp));
    EXPECT_EQ(120, (int)wp.rcNormalPosition.right);
    EXPECT_EQ(40,  (int)wp.rcNormalPosition.bottom);

    g_wxhook_get_client_rect = nullptr;
    EXPECT_FALSE(w.GetWindowPlacement(nullptr));
    EXPECT_TRUE(w.GetWindowPlacement(&wp));   // no hook at all: zero rect
    EXPECT_EQ(0, (int)wp.rcNormalPosition.right);
}

// ═══════════════════════════════════════════════════════════════════════════
// Common dialogs
// ═══════════════════════════════════════════════════════════════════════════

TEST(CommonDialogs, ColorDialogCarriesColour) {
    CColorDialog dlg(RGB(10, 20, 30));
    EXPECT_EQ(RGB(10, 20, 30), dlg.GetColor());
}

static long s_pickSeed;
static int  s_pickCalls;

// CColorDialog had no DoModal of its own, so it inherited CDialog's -- which
// runs the dialog bridge on template 0, a template that does not exist.  The
// Colour Picker's "Other colour..." button and its large swatch both bottom out
// in `if (dlg.DoModal () != IDOK) return;`, so both were silent no-ops.
TEST(CommonDialogs, ColorDialogHeadlessCancelsHookedPicks) {
    long (*const saved)(long) = g_wxhook_pick_colour;

    // No hook: IDCANCEL, and the seeded colour still comes back out.
    g_wxhook_pick_colour = nullptr;
    CColorDialog none(RGB(10, 20, 30));
    EXPECT_EQ((INT_PTR)IDCANCEL, none.DoModal());
    EXPECT_EQ(RGB(10, 20, 30), none.GetColor());

    // Hooked: the ctor colour must reach the picker (the mutation that drops the
    // seed is what opens the panel on black), and the answer must land in m_cc.
    s_pickCalls = 0;
    g_wxhook_pick_colour = [](long suggested) -> long {
        s_pickSeed = suggested; ++s_pickCalls; return (long)RGB(1, 2, 3);
    };
    CColorDialog ok(RGB(10, 20, 30));
    EXPECT_EQ((INT_PTR)IDOK, ok.DoModal());
    EXPECT_EQ(1, s_pickCalls);
    EXPECT_EQ((long)RGB(10, 20, 30), s_pickSeed);
    EXPECT_EQ(RGB(1, 2, 3), ok.GetColor());

    // -1 is the hook's cancel sentinel: IDCANCEL, and the seed is left alone
    // rather than being overwritten with 0x00FFFFFF.
    g_wxhook_pick_colour = [](long) -> long { return -1; };
    CColorDialog cancelled(RGB(10, 20, 30));
    EXPECT_EQ((INT_PTR)IDCANCEL, cancelled.DoModal());
    EXPECT_EQ(RGB(10, 20, 30), cancelled.GetColor());

    // Back to null, NOT to `saved`: test_lua_scripting's PickColourIsHeadlessNoop
    // asserts world.PickColour(255) == -1, which needs this hook unset.
    g_wxhook_pick_colour = nullptr;
    (void)saved;
}

static WxFontChoice s_fontIn;

TEST(CommonDialogs, FontDialogHeadlessCancelsHookedFills) {
    LOGFONT lf = {};
    strcpy(lf.lfFaceName, "Courier");
    lf.lfHeight = -13;
    lf.lfWeight = 0;   // unset: DoModal must seed FW_NORMAL
    CFontDialog cancel(&lf);
    EXPECT_EQ((INT_PTR)IDCANCEL, cancel.DoModal());   // no hook

    g_wxhook_font_dialog = [](void*, WxFontChoice* io) -> int {
        s_fontIn = *io;
        strcpy(io->faceName, "Menlo");
        io->pointSize = 14;
        io->weight = FW_BOLD;
        io->italic = 1;
        io->underline = 1;
        io->strikeout = 0;
        io->color = 0x123456;
        return 1;
    };
    CFontDialog dlg(&lf);
    EXPECT_EQ((INT_PTR)IDOK, dlg.DoModal());
    EXPECT_STREQ("Courier", s_fontIn.faceName);       // seeded from LOGFONT
    EXPECT_EQ((long)-13, s_fontIn.lfHeight);
    EXPECT_EQ((int)FW_NORMAL, s_fontIn.weight);       // 0 -> FW_NORMAL default
    EXPECT_STREQ("Menlo", (LPCTSTR)dlg.GetFaceName());
    EXPECT_EQ(140, dlg.GetSize());                    // tenths of a point
    EXPECT_EQ((int)FW_BOLD, dlg.GetWeight());
    EXPECT_TRUE(dlg.IsBold());
    EXPECT_TRUE(dlg.IsItalic());
    EXPECT_TRUE(dlg.IsUnderline());
    EXPECT_FALSE(dlg.IsStrikeOut());
    EXPECT_EQ((COLORREF)0x123456, dlg.GetColor());
    // Result mirrored back into m_lf (callers read m_cf.lpLogFont)
    EXPECT_STREQ("Menlo", dlg.m_lf.lfFaceName);
    EXPECT_EQ((LONG)FW_BOLD, dlg.m_lf.lfWeight);
    EXPECT_EQ(1, (int)dlg.m_lf.lfItalic);

    // Hook returning 0 = user cancelled
    g_wxhook_font_dialog = [](void*, WxFontChoice*) -> int { return 0; };
    CFontDialog dlg2(&lf);
    EXPECT_EQ((INT_PTR)IDCANCEL, dlg2.DoModal());
    g_wxhook_font_dialog = nullptr;
}

// CHOOSEFONT::rgbColors is IN/OUT on Windows under CF_EFFECTS: it is the colour
// the dialog's picker opens on, and GetColor() reads it back.  DoModal used to
// copy only face/height/weight/italic into the exchange struct, so the caller's
// colour never reached the dialog and every fontpicker round-trip came back
// black — utils.fontpicker(name, size, colour) silently dropped its third
// argument (scripting/lua_utils.cpp sets dlg.m_cf.rgbColors from it).
TEST(CommonDialogs, FontDialogSeedsAndReturnsColour) {
    LOGFONT lf = {};
    strcpy(lf.lfFaceName, "Courier");
    lf.lfHeight = -13;

    // No hook at all (headless/mushcore): the caller's colour survives, it is
    // not replaced by black.
    CFontDialog headless(&lf);
    headless.m_cf.rgbColors = RGB(0, 255, 0);
    EXPECT_EQ((INT_PTR)IDCANCEL, headless.DoModal());
    EXPECT_EQ((COLORREF)RGB(0, 255, 0), headless.GetColor());

    // OK: the seed reaches the dialog, and the dialog's choice comes back.
    g_wxhook_font_dialog = [](void*, WxFontChoice* io) -> int {
        s_fontIn = *io;
        strcpy(io->faceName, "Menlo");
        io->pointSize = 12;
        io->color = RGB(1, 2, 3);   // user picked something else
        return 1;
    };
    CFontDialog dlg(&lf);
    dlg.m_cf.rgbColors = RGB(0, 255, 0);          // e.g. mapper's room-name colour
    EXPECT_EQ((INT_PTR)IDOK, dlg.DoModal());
    EXPECT_EQ((uint32_t)RGB(0, 255, 0), s_fontIn.color);   // seeded the dialog
    EXPECT_EQ((COLORREF)RGB(1, 2, 3), dlg.GetColor());     // chosen colour back
    EXPECT_EQ((COLORREF)RGB(1, 2, 3), dlg.m_cf.rgbColors); // MFC mirrors it here

    // Cancel: GetColor() still reports the colour the caller asked for.
    g_wxhook_font_dialog = [](void*, WxFontChoice*) -> int { return 0; };
    CFontDialog dlg2(&lf);
    dlg2.m_cf.rgbColors = RGB(0, 255, 0);
    EXPECT_EQ((INT_PTR)IDCANCEL, dlg2.DoModal());
    EXPECT_EQ((COLORREF)RGB(0, 255, 0), dlg2.GetColor());
    g_wxhook_font_dialog = nullptr;
}

static std::string s_fdLeaf, s_fdDir;
static bool s_fdOpen, s_fdOverwrite;

TEST(CommonDialogs, FileDialogPathPlumbing) {
    // Headless: cancel
    CFileDialog cancel(TRUE);
    EXPECT_EQ((INT_PTR)IDCANCEL, cancel.DoModal());
    EXPECT_EQ((POSITION)nullptr, cancel.GetStartPosition());

    HookGuard<decltype(g_wxhook_file_dialog)> fdGuard(g_wxhook_file_dialog);
    g_wxhook_file_dialog = [](bool bOpen, const char*, const char* dir,
                              const char* leaf, const char*, bool ow,
                              bool) -> char* {
        s_fdOpen = bOpen;
        s_fdDir = dir ? dir : "";
        s_fdLeaf = leaf ? leaf : "";
        s_fdOverwrite = ow;
        return strdup("/picked/dir/result.mcl");
    };
    // _MAX_PATH, not sizeof("/worlds/dir/start.mcl"): the buffer is what DoModal
    // writes the chosen path back into, and the constructor's nMaxFile default
    // is _MAX_PATH -- the same bargain the canonical GetBuffer(_MAX_PATH)
    // callers strike.
    char initial[_MAX_PATH] = "/worlds/dir/start.mcl";
    CFileDialog dlg(FALSE, "mcl", nullptr, OFN_OVERWRITEPROMPT, "Worlds|*.mcl||");
    dlg.m_ofn.lpstrFile = initial;
    EXPECT_EQ((INT_PTR)IDOK, dlg.DoModal());
    EXPECT_FALSE(s_fdOpen);                       // save dialog
    EXPECT_TRUE(s_fdOverwrite);
    EXPECT_STREQ("start.mcl", s_fdLeaf.c_str());  // leaf stripped of directory
    EXPECT_STREQ("/worlds/dir", s_fdDir.c_str()); // dir derived from full path
    EXPECT_STREQ("/picked/dir/result.mcl", (LPCTSTR)dlg.GetPathName());
    EXPECT_STREQ("result.mcl", (LPCTSTR)dlg.GetFileName());
    EXPECT_STREQ("mcl", (LPCTSTR)dlg.GetFileExt());
    EXPECT_STREQ("result", (LPCTSTR)dlg.GetFileTitle());
    POSITION pos = dlg.GetStartPosition();
    ASSERT_NE((POSITION)nullptr, pos);
    EXPECT_STREQ("/picked/dir/result.mcl", (LPCTSTR)dlg.GetNextPathName(pos));
    EXPECT_EQ((POSITION)nullptr, pos);
    // The OPENFILENAME write-back: Windows' common dialog puts the chosen path
    // in the caller's lpstrFile buffer, and three canonical File>Open handlers
    // read it from there rather than calling GetPathName().  Asserting the
    // buffer, not GetPathName(), is the whole point -- GetPathName() was right
    // all along, which is why nothing caught this.
    EXPECT_STREQ("/picked/dir/result.mcl", initial);
    EXPECT_EQ((DWORD)_MAX_PATH, dlg.m_ofn.nMaxFile);   // supplied by the ctor

    // A caller with its own smaller buffer must say so, exactly as on Windows;
    // the write-back then truncates to nMaxFile-1 and stays NUL-terminated.
    // The sentinel byte one past the end must survive.
    char small[12];
    memset(small, '#', sizeof small);
    small[0] = '\0';   // OPENFILENAME: lpstrFile is an init STRING; NUL = none
    CFileDialog snug(FALSE, "mcl", nullptr, 0, "Worlds|*.mcl||");
    snug.m_ofn.lpstrFile = small;
    snug.m_ofn.nMaxFile  = 8;
    EXPECT_EQ((INT_PTR)IDOK, snug.DoModal());
    EXPECT_STREQ("/picked", small);              // 7 chars + NUL, not 22
    EXPECT_EQ('#', small[8]);                    // nothing written past nMaxFile
    EXPECT_STREQ("/picked/dir/result.mcl", (LPCTSTR)snug.GetPathName());

    // A cancel must leave the caller's buffer exactly as it was -- the three
    // canonical callers seed it with the name to reopen on.
    {
        HookGuard<decltype(g_wxhook_file_dialog)> nope(g_wxhook_file_dialog);
        g_wxhook_file_dialog = [](bool, const char*, const char*, const char*,
                                  const char*, bool, bool) -> char* {
            return nullptr;
        };
        char keep[_MAX_PATH] = "/keep/me.mcl";
        CFileDialog cancelled(TRUE, nullptr, nullptr, 0, nullptr);
        cancelled.m_ofn.lpstrFile = keep;
        EXPECT_EQ((INT_PTR)IDCANCEL, cancelled.DoModal());
        EXPECT_STREQ("/keep/me.mcl", keep);
    }

    // A null lpstrFile is the common case (most callers only read GetPathName)
    // and must not crash.
    CFileDialog noBuf(FALSE, nullptr, nullptr, 0, nullptr);
    EXPECT_EQ((INT_PTR)IDOK, noBuf.DoModal());
    EXPECT_STREQ("/picked/dir/result.mcl", (LPCTSTR)noBuf.GetPathName());

    // lpstrInitialDir takes precedence; filename ctor arg is the hint
    CFileDialog dlg2(TRUE, nullptr, "hint.txt");
    dlg2.m_ofn.lpstrInitialDir = "/explicit/dir";
    EXPECT_EQ((INT_PTR)IDOK, dlg2.DoModal());
    EXPECT_TRUE(s_fdOpen);
    EXPECT_FALSE(s_fdOverwrite);
    EXPECT_STREQ("/explicit/dir", s_fdDir.c_str());
    EXPECT_STREQ("hint.txt", s_fdLeaf.c_str());
    g_wxhook_file_dialog = nullptr;

    // "Open OR create" — an open-style dialog that warns before overwriting and
    // does NOT require the file to exist.  That is the world-prefs Notes page's
    // "Save notes", the only call site in the tree shaped this way, and it could
    // not create a new file because an NSOpenPanel will not return a name that
    // is not there (and wxFD_FILE_MUST_EXIST is a no-op on macOS, so honouring
    // the flag would not have helped).  It has to reach the hook as a SAVE.
    g_wxhook_file_dialog = [](bool bOpen, const char*, const char*,
                              const char*, const char*, bool ow,
                              bool) -> char* {
        s_fdOpen = bOpen;
        s_fdOverwrite = ow;
        return strdup("/picked/notes.txt");
    };
    CFileDialog openOrCreate(TRUE, "txt", "",
                             OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT,
                             "Text files (*.txt)|*.txt||");
    EXPECT_EQ((INT_PTR)IDOK, openOrCreate.DoModal());
    EXPECT_FALSE(s_fdOpen);                       // a save panel
    EXPECT_TRUE(s_fdOverwrite);

    // The control: a genuine open dialog stays an open dialog, so this cannot
    // be a blanket flip.  Both halves of the predicate are exercised — the
    // must-exist flag, and (below) a flagless CFileDialog(TRUE).
    CFileDialog mustExist(TRUE, "txt", "",
                          OFN_HIDEREADONLY | OFN_FILEMUSTEXIST | OFN_OVERWRITEPROMPT,
                          "Text files (*.txt)|*.txt||");
    EXPECT_EQ((INT_PTR)IDOK, mustExist.DoModal());
    EXPECT_TRUE(s_fdOpen);
    CFileDialog plainOpen(TRUE, "txt", "", OFN_HIDEREADONLY,
                          "Text files (*.txt)|*.txt||");
    EXPECT_EQ((INT_PTR)IDOK, plainOpen.DoModal());
    EXPECT_TRUE(s_fdOpen);
    g_wxhook_file_dialog = nullptr;

    // extension-less result
    CFileDialog noExt(TRUE);
    noExt.m_strResult = "plain";
    EXPECT_STREQ("plain", (LPCTSTR)noExt.GetFileName());
    EXPECT_STREQ("", (LPCTSTR)noExt.GetFileExt());
    EXPECT_STREQ("plain", (LPCTSTR)noExt.GetFileTitle());
}

// ═══════════════════════════════════════════════════════════════════════════
// CFileDialog multi-select (OFN_ALLOWMULTISELECT)
// ═══════════════════════════════════════════════════════════════════════════
// Three dialogs ask for it — Add Startup Worlds and Add Global Plugins
// (dialogs/global_prefs/GlobalPrefs.cpp) and Add plugin
// (dialogs/plugins/PluginsDlg.cpp) — and all three are canonical Windows code
// written against MFC's iteration protocol: GetStartPosition() hands out an
// opaque POSITION and each GetNextPathName() yields one path and advances it,
// NULLing it after the last.  The shim used to answer that with the one
// selected file, so every "Add …" button could only ever add one file per
// click.
//
// The panel itself is a wxFileDialog living in mushgui_adapters, which mushcore
// deliberately does not link, so it cannot be shown from a headless test.  What
// IS testable here — and is where the protocol actually lives — is the shim's
// iteration layer over whatever the hook returned; these drive it through the
// hook seam, the same way every other dialog test in this file does.

static std::vector<std::string> s_msPaths;   // what the fake panel "selected"
static bool s_msAskedMulti;                  // what the shim asked the hook for
static int  s_msCalls;

// Pack "a", "b" into the hook's multi-select block shape: "a\0b\0\0".
static char* MultiSelectFakeHook(bool /*bOpen*/, const char*, const char*,
                                 const char*, const char*, bool /*overwrite*/,
                                 bool bMulti)
{
    ++s_msCalls;
    s_msAskedMulti = bMulti;
    if (!bMulti)                             // single-select: a plain C string
        return s_msPaths.empty() ? nullptr : strdup(s_msPaths[0].c_str());

    size_t total = 1;                        // the block's closing NUL
    for (const std::string& s : s_msPaths) total += s.size() + 1;
    char* block = (char*) malloc(total);
    char* w = block;
    for (const std::string& s : s_msPaths) {
        memcpy(w, s.c_str(), s.size() + 1);
        w += s.size() + 1;
    }
    *w = '\0';
    return block;
}

// Run the callers' own loop shape.  The iteration cap turns "never terminates"
// into a failed assertion rather than a hung test binary.
static std::vector<std::string> CollectPathNames(CFileDialog& dlg)
{
    std::vector<std::string> out;
    for (POSITION pos = dlg.GetStartPosition(); pos; ) {
        out.push_back((LPCTSTR) dlg.GetNextPathName(pos));
        if (out.size() > 64) { out.push_back("<enumeration did not end>"); break; }
    }
    return out;
}

// (a) the multi-path case.  Two of the three paths are the ones a
// delimiter-based implementation gets wrong: one contains a space, one is
// non-ASCII (UTF-8, split across literals so no hex escape can swallow the
// next character).
TEST(CommonDialogs, MultiSelectYieldsEveryPathExactlyOnceInOrder)
{
    HookGuard<decltype(g_wxhook_file_dialog)> fdGuard(g_wxhook_file_dialog);
    g_wxhook_file_dialog = MultiSelectFakeHook;

    const char* kSpaced   = "/Volumes/My Worlds/second world.mcl";
    const char* kNonAscii = "/W" "\xc3\xb6" "rter/" "\xe6\x97\xa5\xe6\x9c\xac"
                            "\xe8\xaa\x9e" ".mcl";
    s_msPaths = { "/worlds/first.mcl", kSpaced, kNonAscii };
    s_msCalls = 0;

    // Exactly the flags and buffer plumbing of CGlobalPrefsP1::OnNewStartupWorld.
    CFileDialog dlg(TRUE, "mcl", NULL,
                    OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
                    "MUSHclient worlds (*.mcl)|*.mcl||");
    char szFileBuffer[4096] = { 0 };
    dlg.m_ofn.lpstrFile = szFileBuffer;
    dlg.m_ofn.nMaxFile  = sizeof(szFileBuffer);

    EXPECT_EQ((INT_PTR)IDOK, dlg.DoModal());
    EXPECT_EQ(1, s_msCalls);
    EXPECT_TRUE(s_msAskedMulti);          // the flag reached the panel

    std::vector<std::string> got = CollectPathNames(dlg);
    ASSERT_EQ((size_t)3, got.size());     // every one, and no extras
    EXPECT_EQ(std::string("/worlds/first.mcl"), got[0]);
    EXPECT_EQ(std::string(kSpaced),   got[1]);   // the space survived
    EXPECT_EQ(std::string(kNonAscii), got[2]);   // and so did the UTF-8

    // The enumeration is repeatable — GetStartPosition starts a fresh one.
    EXPECT_EQ(got, CollectPathNames(dlg));

    // The single-file accessors describe ONE file, so they report the first
    // path chosen.  (Windows' MFC ASSERTs against calling them under
    // multi-select and none of the three callers does; see mfc_ui.h.)
    EXPECT_STREQ("/worlds/first.mcl", (LPCTSTR)dlg.GetPathName());
    EXPECT_STREQ("first.mcl", (LPCTSTR)dlg.GetFileName());
    EXPECT_STREQ("mcl",       (LPCTSTR)dlg.GetFileExt());
    EXPECT_STREQ("first",     (LPCTSTR)dlg.GetFileTitle());
}

// (b) the single-path regression guard: without OFN_ALLOWMULTISELECT the shim
// must ask for a single file and must NOT parse the answer as a block — the
// hook returns a plain strdup()ed string with one NUL, so reading it as a block
// would run off the end of the allocation.  This test passes both before and
// after the multi-select change.
TEST(CommonDialogs, SingleSelectIterationIsUnchanged)
{
    HookGuard<decltype(g_wxhook_file_dialog)> fdGuard(g_wxhook_file_dialog);
    g_wxhook_file_dialog = MultiSelectFakeHook;

    s_msPaths = { "/worlds/only one.mcl" };
    s_msCalls = 0;
    s_msAskedMulti = true;                // must be cleared by the shim

    CFileDialog dlg(TRUE, "mcl", NULL, OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
                    "MUSHclient worlds (*.mcl)|*.mcl||");
    EXPECT_EQ((INT_PTR)IDOK, dlg.DoModal());
    EXPECT_EQ(1, s_msCalls);
    EXPECT_FALSE(s_msAskedMulti);         // no flag, no multi-select

    std::vector<std::string> got = CollectPathNames(dlg);
    ASSERT_EQ((size_t)1, got.size());
    EXPECT_EQ(std::string("/worlds/only one.mcl"), got[0]);
    EXPECT_STREQ("/worlds/only one.mcl", (LPCTSTR)dlg.GetPathName());

    // ...and a multi-select dialog on which the user picked exactly one file
    // behaves identically, so the two shapes agree at their boundary.
    CFileDialog one(TRUE, "mcl", NULL,
                    OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST,
                    "MUSHclient worlds (*.mcl)|*.mcl||");
    EXPECT_EQ((INT_PTR)IDOK, one.DoModal());
    EXPECT_TRUE(s_msAskedMulti);
    EXPECT_EQ(got, CollectPathNames(one));
}

// (c) cancelled, and the degenerate "OK with nothing in it".  Neither may loop
// for ever, invent a path, or dereference anything.
TEST(CommonDialogs, MultiSelectCancelledOrEmptyYieldsNothing)
{
    HookGuard<decltype(g_wxhook_file_dialog)> fdGuard(g_wxhook_file_dialog);

    // Cancel: the hook returns nullptr.
    g_wxhook_file_dialog = [](bool, const char*, const char*, const char*,
                              const char*, bool, bool) -> char* {
        return nullptr;
    };
    CFileDialog cancelled(TRUE, "mcl", NULL,
                          OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST,
                          "MUSHclient worlds (*.mcl)|*.mcl||");
    EXPECT_EQ((INT_PTR)IDCANCEL, cancelled.DoModal());
    EXPECT_EQ((POSITION)nullptr, cancelled.GetStartPosition());
    EXPECT_TRUE(CollectPathNames(cancelled).empty());

    // An empty block — "OK, nothing selected".  wx cannot produce it, but the
    // shim must not report IDOK with no path for the caller to read.
    g_wxhook_file_dialog = MultiSelectFakeHook;
    s_msPaths.clear();
    s_msCalls = 0;
    CFileDialog empty(TRUE, "mcl", NULL,
                      OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST,
                      "MUSHclient worlds (*.mcl)|*.mcl||");
    EXPECT_EQ((INT_PTR)IDCANCEL, empty.DoModal());
    EXPECT_EQ(1, s_msCalls);
    EXPECT_EQ((POSITION)nullptr, empty.GetStartPosition());
    EXPECT_TRUE(CollectPathNames(empty).empty());
}

// (d) multi-select is an *open*-panel idea; wxFD_MULTIPLE is illegal with
// wxFD_SAVE and wx asserts on it.  A save panel therefore never asks for it,
// however it is flagged — including the open-or-create shape that reaches the
// hook as a save.
TEST(CommonDialogs, MultiSelectIsNeverAskedForOnASavePanel)
{
    HookGuard<decltype(g_wxhook_file_dialog)> fdGuard(g_wxhook_file_dialog);
    g_wxhook_file_dialog = MultiSelectFakeHook;
    s_msPaths = { "/worlds/saved.mcl" };

    CFileDialog save(FALSE, "mcl", NULL,
                     OFN_ALLOWMULTISELECT | OFN_OVERWRITEPROMPT,
                     "MUSHclient worlds (*.mcl)|*.mcl||");
    s_msAskedMulti = true;
    EXPECT_EQ((INT_PTR)IDOK, save.DoModal());
    EXPECT_FALSE(s_msAskedMulti);

    // Open-or-create (the Notes page's "Save notes"): flagged open, delivered
    // as a save, so it must not ask for multi-select either.
    CFileDialog openOrCreate(TRUE, "txt", "",
                             OFN_ALLOWMULTISELECT | OFN_OVERWRITEPROMPT,
                             "Text files (*.txt)|*.txt||");
    s_msAskedMulti = true;
    EXPECT_EQ((INT_PTR)IDOK, openOrCreate.DoModal());
    EXPECT_FALSE(s_msAskedMulti);

    // The control: a plain open panel WITH the flag does ask for it, so this is
    // not a blanket refusal.
    CFileDialog open(TRUE, "mcl", NULL,
                     OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST,
                     "MUSHclient worlds (*.mcl)|*.mcl||");
    s_msAskedMulti = false;
    EXPECT_EQ((INT_PTR)IDOK, open.DoModal());
    EXPECT_TRUE(s_msAskedMulti);
}

// ═══════════════════════════════════════════════════════════════════════════
// App.platform, and the suggested name every file dialog puts in the box
// ═══════════════════════════════════════════════════════════════════════════

// Seven call sites decide whether to suggest a file name by testing
// App.platform against VER_PLATFORM_WIN32s — and Win32s is zero, so a platform
// that was never assigned reads as Win32s and every suggestion is thrown away.
// Windows assigns it from GetVersionEx in InitInstance; off Windows nothing
// did, and InitInstance does not run in this binary at all.
TEST(AppPlatform, ItIsNotWin32sBeforeInitInstanceRuns)
{
    EXPECT_EQ((DWORD) VER_PLATFORM_WIN32_NT, App.platform);
    EXPECT_NE((DWORD) VER_PLATFORM_WIN32s, App.platform);
}

// The command handler is a protected message-map entry; re-publish it rather
// than route a WM_COMMAND through a frame this binary does not have.
struct LogSessionDoc : public CMUSHclientDoc
{
    using CMUSHclientDoc::OnFileLogsession;
};

static std::string s_logLeaf;
static bool        s_logHookCalled;

// Install a file-dialog hook that records the leaf name and then cancels, so
// no log file is created.
static void InstallLogLeafRecorder ()
{
    s_logLeaf.clear ();
    s_logHookCalled = false;
    g_wxhook_file_dialog = [](bool, const char*, const char*,
                              const char* leaf, const char*, bool,
                              bool) -> char* {
        s_logHookCalled = true;
        s_logLeaf = leaf ? leaf : "";
        return nullptr;      // as if the user cancelled the Save panel
    };
}

// File ▸ Log Session put up a Save panel with an empty "Save As" field,
// because the suggested name went down the Win32s arm and was replaced by "".
TEST(LogSession, TheSavePanelArrivesWithTheSuggestedLogFileName)
{
    HookGuard<decltype(g_wxhook_file_dialog)> fdGuard(g_wxhook_file_dialog);
    InstallLogLeafRecorder ();

    LogSessionDoc doc;
    doc.m_bLogRaw = TRUE;                    // skips the modal CLogDlg
    doc.m_strAutoLogFileName = "session log.txt";
    doc.OnFileLogsession ();

    EXPECT_TRUE(s_logHookCalled);
    EXPECT_STREQ("session log.txt", s_logLeaf.c_str ());
}

// ...and the copy into the dialog's buffer is bounded.  Nothing limits how
// long an auto-log file name may be, and CString::GetBuffer (_MAX_PATH) hands
// out exactly _MAX_PATH + 1 bytes — so the raw strcpy that used to be here was
// an overrun waiting for a long enough name, and it became reachable the
// moment platform stopped saying Win32s.
TEST(LogSession, AnOverlongSuggestedNameIsTruncatedRatherThanOverrunning)
{
    HookGuard<decltype(g_wxhook_file_dialog)> fdGuard(g_wxhook_file_dialog);
    InstallLogLeafRecorder ();

    LogSessionDoc doc;
    doc.m_bLogRaw = TRUE;
    doc.m_strAutoLogFileName = CString ('x', 400);
    doc.OnFileLogsession ();

    EXPECT_TRUE(s_logHookCalled);
    EXPECT_EQ((size_t) (_MAX_PATH - 1), s_logLeaf.size ());
    EXPECT_EQ(std::string (_MAX_PATH - 1, 'x'), s_logLeaf);
}

// ═══════════════════════════════════════════════════════════════════════════
// Cheap sweeps: inert stub families that ship in mushcore
// ═══════════════════════════════════════════════════════════════════════════

TEST(StubSweep, GdiObjectsAndDC) {
    CPen pen;
    EXPECT_FALSE(pen.CreatePen(0, 1, RGB(1, 2, 3)));
    EXPECT_FALSE(pen.CreatePenIndirect(nullptr));
    EXPECT_FALSE(pen.Attach(nullptr) == TRUE);
    EXPECT_EQ((HGDIOBJ)nullptr, pen.Detach());
    CBrush br;
    EXPECT_FALSE(br.CreateSolidBrush(RGB(1, 2, 3)));
    EXPECT_FALSE(br.CreateHatchBrush(0, 0));
    EXPECT_FALSE(br.CreatePatternBrush((CBitmap*)nullptr));
    EXPECT_FALSE(br.CreateStockObject(0));
    EXPECT_EQ((HBRUSH)nullptr, br.GetSafeHandle());
    CFont fnt;
    EXPECT_FALSE(fnt.CreateFont(10, 0, 0, 0, 400, 0, 0, 0, 0, 0, 0, 0, 0, "x"));
    EXPECT_FALSE(fnt.CreateFontIndirect(nullptr));
    EXPECT_FALSE(fnt.CreatePointFont(100, "x"));
    LOGFONT lf;
    EXPECT_EQ(0, fnt.GetLogFont(&lf));
    EXPECT_EQ(0, fnt.GetObject((int)sizeof lf, &lf));
    CBitmap bmp;
    EXPECT_EQ((HBITMAP)nullptr, bmp.CreateCompatibleBitmap(nullptr, 1, 1));
    EXPECT_FALSE(bmp.CreateBitmap(1, 1, 1, 1, nullptr));
    BITMAP bmInfo;
    EXPECT_FALSE(bmp.GetBitmap(&bmInfo));
    CRgn rgn;
    EXPECT_FALSE(rgn.CreateRectRgn(0, 0, 1, 1));
    EXPECT_FALSE(rgn.PtInRegion(0, 0));
    EXPECT_EQ(NULLREGION, rgn.OffsetRgn(1, 1));
    CPalette pal;
    EXPECT_FALSE(pal.CreateHalftonePalette(nullptr));
    EXPECT_EQ(0, pal.GetEntryCount());

    CDC dc;
    EXPECT_EQ((HDC)nullptr, dc.GetSafeHdc());
    EXPECT_TRUE(CDC::FromHandle(nullptr) != nullptr);
    EXPECT_EQ((CPen*)nullptr, dc.SelectObject(&pen));
    EXPECT_EQ((CBrush*)nullptr, dc.SelectObject(&br));
    EXPECT_EQ((CFont*)nullptr, dc.SelectObject(&fnt));
    EXPECT_EQ((CBitmap*)nullptr, dc.SelectObject(&bmp));
    EXPECT_EQ((CGdiObject*)nullptr, dc.SelectStockObject(0));
    EXPECT_EQ(0, dc.SelectClipRgn(&rgn));
    EXPECT_FALSE(dc.Rectangle(0, 0, 1, 1));
    EXPECT_FALSE(dc.Ellipse(0, 0, 1, 1));
    EXPECT_FALSE(dc.Ellipse(CRect(0, 0, 1, 1)));
    EXPECT_FALSE(dc.RoundRect(CRect(0, 0, 4, 4), CPoint(1, 1)));
    EXPECT_FALSE(dc.Chord(CRect(0, 0, 4, 4), CPoint(0, 0), CPoint(1, 1)));
    EXPECT_FALSE(dc.LineTo(1, 1));
    EXPECT_FALSE(dc.MoveTo(0, 0));
    EXPECT_FALSE(dc.Polyline(nullptr, 0));
    EXPECT_FALSE(dc.Polygon(nullptr, 0));
    EXPECT_FALSE(dc.FloodFill(0, 0, 0));
    dc.FillSolidRect(0, 0, 1, 1, 0);
    dc.Draw3dRect(0, 0, 1, 1, 0, 0);
    EXPECT_FALSE(dc.PatBlt(0, 0, 1, 1, 0));
    EXPECT_FALSE(dc.BitBlt(0, 0, 1, 1, nullptr, 0, 0, SRCCOPY));
    EXPECT_FALSE(dc.StretchBlt(0, 0, 1, 1, nullptr, 0, 0, 2, 2, SRCCOPY));
    EXPECT_FALSE(dc.TextOut(0, 0, "x", 1));
    EXPECT_FALSE(dc.ExtTextOut(0, 0, 0, nullptr, "x", 1, nullptr));
    EXPECT_EQ(0, dc.DrawText("x", 1, nullptr, 0));
    EXPECT_EQ((COLORREF)0, dc.SetTextColor(1));
    EXPECT_EQ((COLORREF)0, dc.SetBkColor(1));
    EXPECT_EQ(0, dc.SetBkMode(1));
    EXPECT_EQ(0, dc.SetROP2(1));
    EXPECT_EQ(0u, dc.SetTextAlign(0));
    TEXTMETRIC tm;
    EXPECT_FALSE(dc.GetTextMetrics(&tm));
    CSize ext = dc.GetTextExtent("hello", 5);
    EXPECT_EQ(0, ext.cx);
    EXPECT_EQ(0, dc.GetDeviceCaps(0));
    dc.SetViewportOrg(1, 2);
    dc.SetWindowOrg(1, 2);
    EXPECT_FALSE(dc.SetMapMode(1));
    EXPECT_FALSE(dc.CreateCompatibleDC(&dc));
    EXPECT_FALSE(dc.CreateCompatibleDC(nullptr));
    EXPECT_TRUE(dc.DeleteDC());
    EXPECT_EQ(0, dc.SaveDC());
    EXPECT_FALSE(dc.RestoreDC(1));
    EXPECT_FALSE(dc.IsPrinting());
    CPoint p(1, 2);
    EXPECT_TRUE(dc.DPtoLP(&p));
    EXPECT_TRUE(dc.LPtoDP(&p));
    EXPECT_EQ((CFont*)nullptr, dc.GetCurrentFont());
    CClientDC cdc(nullptr);
    CWindowDC wdc(nullptr);
    CPaintDC pdc(nullptr);
    CRect r(0, 0, 1, 1);
    CMemDC mdc(&cdc, &r);
    (void)mdc;
}

TEST(StubSweep, GlobalGdiFunctions) {
    EXPECT_EQ(0, SaveDC((HDC)nullptr));
    EXPECT_FALSE(RestoreDC((HDC)nullptr, 1));
    EXPECT_FALSE(DeleteDC((HDC)nullptr));
    EXPECT_EQ((HDC)nullptr, CreateCompatibleDC((HDC)nullptr));
    EXPECT_EQ((HGDIOBJ)nullptr, SelectObject((HDC)nullptr, (HGDIOBJ)nullptr));
    EXPECT_FALSE(DeleteObject((HGDIOBJ)nullptr));
    EXPECT_FALSE(BitBlt(nullptr, 0, 0, 1, 1, nullptr, 0, 0, SRCCOPY));
    EXPECT_FALSE(Rectangle(nullptr, 0, 0, 1, 1));
    EXPECT_FALSE(MoveToEx(nullptr, 0, 0, nullptr));
    EXPECT_FALSE(LineTo(nullptr, 0, 0));
    EXPECT_EQ((COLORREF)0, SetTextColor(nullptr, 1));
    EXPECT_EQ((COLORREF)0, SetBkColor(nullptr, 1));
    EXPECT_EQ(0, SetBkMode(nullptr, 1));
    EXPECT_FALSE(TextOut(nullptr, 0, 0, "x", 1));
    EXPECT_FALSE(GetTextMetrics(nullptr, nullptr));
    EXPECT_EQ(0, GetDeviceCaps(nullptr, 0));
    EXPECT_FALSE(FillRect(nullptr, (LPCRECT)nullptr, (HBRUSH)nullptr));
    EXPECT_EQ((HBRUSH)nullptr, CreateSolidBrush(0));
    EXPECT_EQ((HPEN)nullptr, CreatePen(0, 1, 0));
    EXPECT_EQ((HFONT)nullptr, CreateFontIndirect(nullptr));
    EXPECT_EQ((HFONT)nullptr, GetStockObject(0));
    EXPECT_FALSE(SetWindowPos(nullptr, nullptr, 0, 0, 1, 1, 0));
    RECT r;
    EXPECT_FALSE(GetClientRect((HWND)nullptr, &r));
    EXPECT_FALSE(GetWindowRect((HWND)nullptr, &r));
    EXPECT_FALSE(InvalidateRect((HWND)nullptr, nullptr, TRUE));
    EXPECT_FALSE(UpdateWindow((HWND)nullptr));
    EXPECT_FALSE(ShowWindow((HWND)nullptr, 0));
    EXPECT_FALSE(EnableWindow((HWND)nullptr, TRUE));
    EXPECT_EQ((HWND)nullptr, GetDlgItem((HWND)nullptr, 1));
    EXPECT_FALSE(PostMessage((HWND)nullptr, 0, 0, 0));
    EXPECT_EQ((LRESULT)0, SendMessage((HWND)nullptr, 0, 0, 0));
    EXPECT_FALSE(PeekMessage(nullptr, nullptr, 0, 0, 0));
    EXPECT_EQ((SHORT)0, GetKeyState(1));
    EXPECT_EQ((HWND)nullptr, GetCapture());
    EXPECT_TRUE(IsWindow((HWND)(void*)1));
    EXPECT_FALSE(IsWindow((HWND)nullptr));
    EXPECT_FALSE(IsIconic((HWND)nullptr));
    EXPECT_FALSE(ScreenToClient((HWND)nullptr, (POINT*)nullptr));
    EXPECT_FALSE(ClientToScreen((HWND)nullptr, (POINT*)nullptr));
    EXPECT_FALSE(GetCursorPos((POINT*)nullptr));
    EXPECT_EQ((HMENU)0, GetSystemMenu((HWND)nullptr, FALSE));
    EXPECT_FALSE(AppendMenu((HMENU)0, 0, 0, "x"));
    EXPECT_EQ(0, GetMenuItemCount((HMENU)0));
    EXPECT_FALSE(DrawMenuBar((HWND)nullptr));
    EXPECT_FALSE(PlaySound("x", nullptr, 0));
    EXPECT_TRUE(DestroyIcon(nullptr));
    EXPECT_STREQ("MUSHclientWnd", AfxRegisterWndClass(0));
    EXPECT_EQ((HINSTANCE)nullptr, AfxGetResourceHandle());
    EXPECT_EQ((HINSTANCE)nullptr, AfxGetInstanceHandle());
    EXPECT_EQ((HMODULE)nullptr, LoadLibrary("lib"));
    EXPECT_EQ((void*)nullptr, GetProcAddress(nullptr, "fn"));
    EXPECT_FALSE(FreeLibrary(nullptr));
    // Pre-poison with recognisable non-zero sentinels -- SCROLLINFO's default
    // member initialisers are all 0, so an unpoisoned struct would pass
    // EXPECT_EQ(0, si.nPos) whether or not GetScrollInfo touched it (the old
    // vacuous form of this check). Poisoning first makes the assertion
    // actually depend on the function's behaviour: a NULL window must return
    // FALSE (matching both CWnd::GetScrollInfo member overloads and real
    // Win32) and must leave the struct completely untouched, so the
    // sentinels must survive unchanged.
    SCROLLINFO si;
    si.cbSize    = 0xAAAA;
    si.fMask     = 0xBBBB;
    si.nMin      = 111;
    si.nMax      = 222;
    si.nPage     = 333;
    si.nPos      = 444;
    si.nTrackPos = 555;
    EXPECT_FALSE(GetScrollInfo((HWND)nullptr, 0, &si));
    EXPECT_EQ((UINT)0xAAAA, si.cbSize);
    EXPECT_EQ((UINT)0xBBBB, si.fMask);
    EXPECT_EQ(111, si.nMin);
    EXPECT_EQ(222, si.nMax);
    EXPECT_EQ((UINT)333, si.nPage);
    EXPECT_EQ(444, si.nPos);
    EXPECT_EQ(555, si.nTrackPos);
}

TEST(StubSweep, ViewAndFrameFamilies) {
    CView v;
    EXPECT_EQ((CDocument*)nullptr, v.GetDocument());
    v.OnDraw(nullptr);
    v.OnInitialUpdate();
    v.OnChar(0, 0, 0);
    v.OnKeyDown(0, 0, 0);
    v.OnLButtonDown(0, CPoint(1, 1));
    v.OnMouseMove(0, CPoint(1, 1));
    EXPECT_FALSE(v.OnMouseWheel(0, 120, CPoint(0, 0)));
    v.OnSize(0, 10, 10);
    v.OnVScroll(0, 0, nullptr);
    EXPECT_FALSE(v.OnPreparePrinting(nullptr));
    v.EnableScrollBarCtrl(0);
    v.ScrollWindow(0, 0);
    EXPECT_EQ((CFrameWnd*)nullptr, v.GetParentFrame());
    RECT r;
    v.GetClientRect(&r);
    EXPECT_EQ(0, (int)r.right);   // no hook: zeroed
    CRect cr(5, 5, 6, 6);
    v.GetWindowRect(cr);
    EXPECT_EQ(0, (int)cr.right);
    v.Invalidate();
    v.InvalidateRect((const RECT*)nullptr);

    CScrollView sv;
    sv.SetScrollSizes(0, CSize(10, 10));
    EXPECT_EQ(0, sv.GetScrollPosition().x);
    EXPECT_EQ(0, sv.GetTotalSize().cx);
    sv.ScrollToPosition(CPoint(0, 0));

    CEditView ev;
    ev.OnEditSelectAll();
    ev.OnEditCopy();
    ev.SetWindowText("x");
    EXPECT_EQ(0, ev.GetLineCount());
    ev.GetEditCtrl().LimitText(10);
    ev.SetTabStops(4);

    CFrameWnd fw;
    EXPECT_EQ((CView*)nullptr, fw.GetActiveView());
    EXPECT_EQ((CDocument*)nullptr, fw.GetActiveDocument());
    fw.RecalcLayout();
    fw.ActivateFrame();
    CMDIChildWnd child;
    child.MDIMaximize();
    child.MDIRestore();
    EXPECT_EQ((CMDIChildWnd*)nullptr, child.MDIGetActive());
    EXPECT_EQ((HACCEL)nullptr, child.GetDefaultAccelerator());
    CMDIFrameWnd frame;
    EXPECT_EQ((CMDIChildWnd*)nullptr, frame.MDIGetActive());
    frame.MDINext();
    frame.MDITile();
    frame.MDICascade();
    frame.EnableDocking(0);
    MSG msg;
    EXPECT_FALSE(frame.PreTranslateMessage(&msg));
    frame.OnUpdateFrameTitle(TRUE);

    CToolBar tb;
    EXPECT_FALSE(tb.Create(nullptr));
    EXPECT_FALSE(tb.LoadToolBar(1));
    tb.GetToolBarCtrl();
    CStatusBar sb;
    EXPECT_FALSE(sb.Create(nullptr));
    EXPECT_FALSE(sb.SetIndicators(nullptr, 0));
    sb.SetPaneText(0, "x");
    CReBar rb;
    EXPECT_FALSE(rb.Create(nullptr));
    CDialogBar db;
    EXPECT_FALSE(db.Create(nullptr, 0, 0, 0));
    CSplitterWnd sw;
    EXPECT_FALSE(sw.CreateStatic(nullptr, 1, 2));
    EXPECT_EQ(0, sw.GetRowCount());
}

TEST(StubSweep, MiscControlClasses) {
    CTabCtrl tab;
    EXPECT_EQ(-1, tab.GetCurSel());
    EXPECT_FALSE(tab.InsertItem(0, (TCITEM*)nullptr));
    EXPECT_EQ(0, tab.GetItemCount());
    CTreeCtrl tree;
    EXPECT_EQ((HTREEITEM)nullptr, tree.InsertItem("node"));
    EXPECT_FALSE(tree.DeleteItem(nullptr));
    tree.DeleteAllItems();
    EXPECT_EQ((HTREEITEM)nullptr, tree.GetSelectedItem());
    CProgressCtrl prog;
    prog.SetRange(0, 100);
    CSpinButtonCtrl spin;
    CIPAddressCtrl ip;
    EXPECT_TRUE(ip.IsBlank());
    CHotKeyCtrl hot;
    WORD vk = 1, mod = 1;
    hot.GetHotKey(vk, mod);
    CRichEditCtrl rich;
    rich.AppendText("x");
    CScrollBar sbar;
    sbar.SetScrollRange(0, 10);
    CToolTipCtrl tip;
    EXPECT_FALSE(tip.Create(nullptr));
    CHeaderCtrl hdr;
    EXPECT_EQ(0, hdr.GetItemCount());
    CStatusBarCtrl sbc;
    EXPECT_FALSE(sbc.SetParts(0, nullptr));
    CImageList il;
    EXPECT_FALSE(il.Create(16, 16, 0, 0, 0));
    EXPECT_EQ(-1, il.Add((HICON)nullptr));
    EXPECT_EQ(0, il.GetImageCount());
    CPropertyPage page(123);
    EXPECT_TRUE(page.OnSetActive());
    EXPECT_TRUE(page.OnKillActive());
    EXPECT_TRUE(page.OnApply());
    page.SetModified();
    // NOTE: CPropertySheet itself is not instantiable in mushcore — its
    // virtual DoModal() lives in wx_tree_property_sheet.cpp (mushgui only),
    // so constructing one here would need that vtable.  Skip-list item.
    CFindReplaceDialog fr;
    EXPECT_FALSE(fr.IsTerminating());
    EXPECT_EQ((CFindReplaceDialog*)nullptr, CFindReplaceDialog::GetNotifier(0));
    CPrintDialog pd(FALSE);
    EXPECT_EQ(1, pd.GetCopies());
    EXPECT_TRUE(pd.PrintAll());
    EXPECT_EQ(1, pd.GetFromPage());
    CPrintInfo pi;
    EXPECT_EQ(1u, pi.GetMinPage());
    EXPECT_EQ(1u, pi.GetMaxPage());
    EXPECT_FALSE(pi.GetDirect());
    CDumpContext dump;
    dump << "text" << 1 << 2L << 3.5 << CString("s") << (const CObject*)nullptr;
    dump.SetDepth(1);
    CWaitCursor wait;
    wait.Restore();
    CCommandLineInfo cli;
    EXPECT_TRUE(cli.m_bShowSplash);
    EXPECT_EQ(0, cli.m_nShellCommand);
    COleDataSource ods;
    ods.Empty();
    EXPECT_EQ((DROPEFFECT)0, ods.DoDragDrop());
    CWinThread thread;
    EXPECT_TRUE(thread.InitInstance());
    EXPECT_EQ(0, thread.ExitInstance());
}

TEST(StubSweep, WinAppProfileAndGlobals) {
    CWinApp* saved = g_pWinApp;
    {
        CWinApp app;
        EXPECT_EQ(&app, AfxGetApp());        // ctor publishes itself
        EXPECT_EQ((CWnd*)nullptr, AfxGetMainWnd());
        CWnd main;
        app.m_pMainWnd = &main;
        EXPECT_EQ(&main, AfxGetMainWnd());
        EXPECT_STREQ("def", (LPCTSTR)app.GetProfileString("sec", "key", "def"));
        EXPECT_STREQ("", (LPCTSTR)app.GetProfileString("sec", "key"));
        EXPECT_EQ(9u, app.GetProfileInt("sec", "key", 9));
        EXPECT_FALSE(app.WriteProfileString("sec", "key", "v"));
        EXPECT_FALSE(app.WriteProfileInt("sec", "key", 1));
        EXPECT_TRUE(app.InitInstance());
        EXPECT_EQ(0, app.ExitInstance());
        EXPECT_EQ(0, app.Run());
        EXPECT_FALSE(app.OnIdle(0));
        EXPECT_EQ(IDOK, app.DoMessageBox("m", 0, 0));
        EXPECT_EQ((HICON)nullptr, app.LoadIcon(1u));
        EXPECT_EQ((HCURSOR)nullptr, app.LoadCursor(1u));
        EXPECT_EQ((HBITMAP)nullptr, app.LoadBitmap(1u));
        EXPECT_TRUE(app.SaveAllModified());
        EXPECT_FALSE(app.PumpMessage());
        app.AddToRecentFileList("/x.mcl");
        app.LoadStdProfileSettings();
        EXPECT_EQ((CDocument*)nullptr, app.OpenDocumentFile("/x.mcl"));
    }
    g_pWinApp = saved;   // restore the real App global for later tests
}

// ═══ Second-pass sweep of the remaining inert shim surface ═════════════════
// Everything here ships in mushcore (the stub arms of the shim); the calls
// pin the inert contract (FALSE / 0 / nullptr / no-op) each stub promises.

TEST(StubSweep, DialogAndDdxRemainder) {
    CDialog dlg(1);
    EXPECT_EQ(0, dlg.GetDlgItemInt(5));
    dlg.SetDlgItemInt(5, 7);
    dlg.GotoDlgCtrl(nullptr);
    dlg.NextDlgCtrl();
    dlg.PrevDlgCtrl();
    CREATESTRUCT cs = {};
    EXPECT_EQ(0, dlg.OnCreate(&cs));
    dlg.OnDestroy();
    dlg.OnSize(0, 1, 1);
    dlg.OnHScroll(0, 0, nullptr);
    dlg.OnSysCommand(0, 0);
    dlg.PostNcDestroy();
    MSG msg = {};
    EXPECT_FALSE(dlg.PreTranslateMessage(&msg));
    dlg.OnTimer(1);
    EXPECT_EQ((HICON)nullptr, dlg.SetIcon(nullptr));
    EXPECT_EQ((HBRUSH)nullptr, dlg.OnCtlColor(nullptr, nullptr, 0));
    EXPECT_EQ((CWnd*)nullptr, dlg.GetSafeOwner());
    EXPECT_EQ(&dlg, CDialog::GetSafeOwner(&dlg));
    dlg.UpdateDialogControls(nullptr);
    dlg.MapDialogRect(nullptr);
    EXPECT_EQ((LRESULT)0, dlg.SendDlgItemMessage(1, 0));
    EXPECT_FALSE(dlg.Create(1u));
    EXPECT_FALSE(dlg.CreateIndirect(nullptr, nullptr));
    EXPECT_FALSE(dlg.DestroyWindow());
    EXPECT_FALSE(dlg.IsDialogMessage(&msg));

    // DDX/DDV family with no dialog hooks: must be inert.  (DDV_MaxChars and
    // the DDV_MinMax* are real now -- the values here are all IN range, which
    // is what a hook-less pass must leave untouched.)
    CDataExchange dx;
    dx.m_pDlgWnd = &dlg;
    int radio = 5;
    DDX_Radio(&dx, 1, radio);
    EXPECT_EQ(5, radio);   // no radio hooks: untouched
    CString s = "keep";
    int i = 3; long l = 4; UINT u = 5; float f = 1; double d = 2;
    DDV_MinMaxInt(&dx, i, 0, 10);
    DDV_MinMaxLong(&dx, l, 0, 10);
    DDV_MinMaxUInt(&dx, u, 0, 10);
    DDV_MinMaxFloat(&dx, f, 0, 10);
    DDV_MinMaxDouble(&dx, d, 0, 10);
    DDV_MaxChars(&dx, s, 10);
    DDX_MinMaxString(&dx, 1, s);
    DDV_MinMaxString(&dx, s, 0, 10);
    DDX_MinMaxCBString(&dx, 1, s);
    DDV_MinMaxCBString(&dx, s, 0, 10);
    DDX_CBStringExact(&dx, 1, s);
    int idx = 2;
    DDX_LBIndex(&dx, 1, idx);
    DDX_LBString(&dx, 1, s);
    EXPECT_STREQ("keep", (LPCTSTR)s);
}

// The end-to-end half of the AfxFormatString1 fix: DDV_MinMaxString
// (DDV_validation.cpp, the real MFC-shared source) builds MFC's "too short"
// prompt through AfxFormatString1(IDS_MIN_STRING_SIZE), so with the shim's
// blanket passthrough a field below its minimum length put up a modal box whose
// entire text was a bare number.  Live callers: the Plugin Wizard's plugin-ID
// page (24, 24) and the Global Preferences locale (2, 3).
//
// This asserts the STRING the box was asked to show, not merely that a box
// appeared -- the passthrough shows one too.
TEST(Ddv, MinMaxStringExplainsTheMinimumLength) {
    static CString s_text;
    static int     s_boxes;
    s_text = "";
    s_boxes = 0;
    int (*const saved)(const char*, const char*, unsigned) = g_wxhook_message_box;
    g_wxhook_message_box = [](const char* t, const char*, unsigned) -> int {
        s_text = t ? t : ""; ++s_boxes; return IDOK;
    };

    CDataExchange dx;
    dx.m_bSaveAndValidate = TRUE;

    // Too short, minimum > 1 -> the AfxFormatString1(IDS_MIN_STRING_SIZE) arm.
    bool failed = false;
    CString id = "not-24-chars";
    try { DDV_MinMaxString(&dx, id, 24, 24); }
    catch (const CDdxValidationFail&) { failed = true; }
    EXPECT_TRUE(failed);                       // the veto itself was never broken
    EXPECT_EQ(1, s_boxes);
    EXPECT_STREQ("Please enter at least 24 characters.", (LPCTSTR)s_text);

    // The minimum == 1 arm has its own literal message and must stay untouched.
    CString blank = "";
    failed = false;
    try { DDV_MinMaxString(&dx, blank, 1, 10); }
    catch (const CDdxValidationFail&) { failed = true; }
    EXPECT_TRUE(failed);
    EXPECT_EQ(2, s_boxes);
    EXPECT_STREQ("This field may not be blank", (LPCTSTR)s_text);

    // A long-enough value shows nothing at all -- the control that proves the
    // two assertions above are not just reading a box that always appears.
    CString ok = "123456789012345678901234";
    DDV_MinMaxString(&dx, ok, 24, 24);
    EXPECT_EQ(2, s_boxes);

    g_wxhook_message_box = saved;
}

TEST(StubSweep, ControlClassRemainder) {
    CListBox lb;
    EXPECT_EQ(LB_ERR, lb.InsertString(0, "x"));
    EXPECT_EQ(LB_ERR, lb.DeleteString(0));
    EXPECT_EQ(LB_ERR, lb.GetText(0, (LPTSTR)nullptr));
    EXPECT_EQ(LB_ERR, lb.GetTextLen(0));
    EXPECT_EQ(LB_ERR, lb.FindString(-1, "x"));
    EXPECT_EQ(LB_ERR, lb.FindStringExact(-1, "x"));
    EXPECT_EQ(LB_ERR, lb.SelectString(-1, "x"));
    EXPECT_EQ(0, lb.GetSelItems(0, nullptr));
    lb.SetColumnWidth(10);
    lb.SetHorizontalExtent(10);
    EXPECT_EQ(0, lb.GetHorizontalExtent());
    lb.Dir(0, "*");

    CComboBox cb;
    EXPECT_EQ(CB_ERR, cb.InsertString(0, "x"));
    EXPECT_EQ(CB_ERR, cb.DeleteString(0));
    EXPECT_EQ(CB_ERR, cb.GetLBText(0, (LPTSTR)nullptr));
    EXPECT_EQ(CB_ERR, cb.GetLBTextLen(0));
    EXPECT_EQ(0, cb.GetWindowTextLength());
    cb.SetItemData(0, 1);
    EXPECT_EQ((DWORD_PTR)0, cb.GetItemData(0));   // unbound control: hooks see no widget
    cb.ShowDropDown();
    cb.GetDroppedControlRect(nullptr);
    EXPECT_FALSE(cb.GetDroppedState());
    cb.LimitText(10);
    EXPECT_EQ(CB_ERR, cb.SetEditSel(0, 1));
    EXPECT_FALSE(cb.GetEditSel(nullptr, nullptr));

    CEdit e;
    EXPECT_EQ(0, e.GetLine(0, nullptr, 0));
    e.Clear(); e.Copy(); e.Cut(); e.Paste(); e.Undo();
    EXPECT_FALSE(e.CanUndo());
    e.EmptyUndoBuffer();
    e.SetMargins(1, 1);
    EXPECT_EQ((DWORD)0, e.GetMargins());
    e.SetFont(nullptr);
    e.SetTabStops(4);
    int stops[2] = { 4, 8 };
    e.SetTabStops(2, stops);
    EXPECT_FALSE(e.GetModify());
    e.SetModify();
    EXPECT_FALSE(e.LineScroll(1));
    EXPECT_EQ(0, e.GetFirstVisibleLine());
    CPoint pc = e.PosFromChar(0);
    EXPECT_EQ(0, pc.x);
    EXPECT_EQ((HLOCAL)nullptr, e.GetHandle());
    e.SetHandle(nullptr);
    e.SetReadOnly();
    e.LimitText(80);

    CButton b;
    b.SetState(TRUE);
    EXPECT_EQ(0u, b.GetState());
    b.SetIcon(nullptr);
    EXPECT_EQ((HICON)nullptr, b.GetIcon());
    b.SetBitmap(nullptr);
    b.SetButtonStyle(0);
    RECT r = {};
    EXPECT_FALSE(b.Create("x", 0, r, nullptr, 1));
    DRAWITEMSTRUCT dis = {};
    b.DrawItem(&dis);   // base owner-draw: no custom paint

    CStatic st;
    st.SetBitmap(nullptr);
    EXPECT_EQ((HBITMAP)nullptr, st.GetBitmap());
    st.SetIcon(nullptr);
    EXPECT_EQ((HICON)nullptr, st.GetIcon());
    EXPECT_FALSE(st.Create("x", 0, r, nullptr));

    CListCtrl lc;
    LVITEM item = {};
    EXPECT_FALSE(lc.SetItem(&item));
    EXPECT_TRUE(lc.GetItemText(0, 0).IsEmpty());
    LVCOLUMN col = {};
    EXPECT_EQ(-1, lc.InsertColumn(0, &col));
    EXPECT_FALSE(lc.GetColumn(0, &col));
    EXPECT_FALSE(lc.SetColumn(0, &col));
    EXPECT_EQ((CEdit*)nullptr, lc.EditLabel(0));
    EXPECT_FALSE(lc.GetItemRect(0, &r, 0));
    CRect cr;
    EXPECT_FALSE(lc.GetSubItemRect(0, 0, 0, cr));
    EXPECT_FALSE(lc.GetSubItemRect(0, 0, 0, r));
    EXPECT_EQ(-1, lc.HitTest(CPoint(0, 0)));
    EXPECT_FALSE(lc.Scroll(CSize(0, 1)));
    EXPECT_EQ(0, lc.GetTopIndex());
    EXPECT_EQ(0, lc.GetCountPerPage());
    EXPECT_TRUE(lc.RedrawItems(0, 1));
    lc.SetBkColor(0);
    lc.SetTextColor(0);
    lc.SetTextBkColor(0);
    EXPECT_FALSE(lc.SetImageList(nullptr, 0));
    EXPECT_EQ(0, lc.GetColumnWidth(0));
    lc.SetExtendedStyle(0);
    EXPECT_EQ((DWORD)0, lc.GetExtendedStyle());
}

TEST(StubSweep, TabTreeAndSmallControls) {
    CTabCtrl tab;
    EXPECT_EQ(-1, tab.SetCurSel(0));
    TCITEM tci;
    EXPECT_FALSE(tab.InsertItem(0, &tci));
    EXPECT_FALSE(tab.DeleteAllItems());
    EXPECT_FALSE(tab.DeleteItem(0));
    RECT r = {};
    EXPECT_FALSE(tab.GetItemRect(0, &r));
    tab.AdjustRect(TRUE, &r);
    EXPECT_FALSE(tab.GetItem(0, &tci));
    EXPECT_FALSE(tab.SetItem(0, &tci));
    TCHITTESTINFO ht;
    EXPECT_EQ(-1, tab.HitTest(&ht));
    EXPECT_EQ((CImageList*)nullptr, tab.SetImageList(nullptr));
    EXPECT_EQ((CImageList*)nullptr, tab.GetImageList());
    EXPECT_FALSE(tab.Create(0, r, nullptr, 1));
    tab.RemoveImage(0);
    EXPECT_TRUE(tab.RedrawWindow());

    CTreeCtrl tree;
    TVINSERTSTRUCT tvi = {};
    EXPECT_EQ((HTREEITEM)nullptr, tree.InsertItem(&tvi));
    EXPECT_FALSE(tree.SelectItem(nullptr));
    TVITEM tv = {};
    EXPECT_FALSE(tree.GetItem(&tv));
    EXPECT_FALSE(tree.SetItem(&tv));
    EXPECT_FALSE(tree.Expand(nullptr, 0));
    EXPECT_EQ((HTREEITEM)nullptr, tree.GetChildItem(nullptr));
    EXPECT_EQ((HTREEITEM)nullptr, tree.GetNextSiblingItem(nullptr));
    EXPECT_EQ((HTREEITEM)nullptr, tree.GetPrevSiblingItem(nullptr));
    EXPECT_EQ((HTREEITEM)nullptr, tree.GetParentItem(nullptr));
    EXPECT_EQ((HTREEITEM)nullptr, tree.GetRootItem());
    EXPECT_TRUE(tree.GetItemText(nullptr).IsEmpty());
    EXPECT_EQ((DWORD_PTR)0, tree.GetItemData(nullptr));
    EXPECT_FALSE(tree.SetItemData(nullptr, 0));
    EXPECT_FALSE(tree.SetItemText(nullptr, "x"));
    EXPECT_EQ(0u, tree.GetItemState(nullptr, 0));
    EXPECT_FALSE(tree.SetItemState(nullptr, 0, 0));
    EXPECT_FALSE(tree.SortChildren(nullptr));
    EXPECT_FALSE(tree.SortChildrenCB(nullptr));
    tree.SetImageList(nullptr, 0);
    EXPECT_EQ((HTREEITEM)nullptr, tree.HitTest(CPoint(0, 0)));
    EXPECT_FALSE(tree.GetItemRect(nullptr, &r, TRUE));

    CProgressCtrl prog;
    prog.SetRange32(0, 10);
    EXPECT_EQ(0, prog.SetPos(1));
    EXPECT_EQ(0, prog.GetPos());
    EXPECT_EQ(0, prog.OffsetPos(1));
    EXPECT_EQ(0, prog.StepIt());
    EXPECT_EQ(0, prog.SetStep(2));
    prog.SetBarColor(0);

    // Slider / spin route through hooks; record with fakes
    static int s_lo, s_hi, s_pos;
    g_wxhook_slider_set_range = [](void*, int lo, int hi) { s_lo = lo; s_hi = hi; };
    g_wxhook_slider_set_pos   = [](void*, int pos) { s_pos = pos; };
    g_wxhook_slider_get_pos   = [](void*) -> int { return 42; };
    g_wxhook_slider_set_page_size = [](void*, int) {};
    CSliderCtrl slider;
    slider.SetRange(0, 255);
    EXPECT_EQ(0, s_lo);
    EXPECT_EQ(255, s_hi);
    slider.SetPos(7);
    EXPECT_EQ(7, s_pos);
    EXPECT_EQ(42, slider.GetPos());
    slider.SetPageSize(16);
    EXPECT_EQ(0, slider.GetPageSize());
    g_wxhook_slider_set_range = nullptr;
    g_wxhook_slider_set_pos = nullptr;
    g_wxhook_slider_get_pos = nullptr;
    g_wxhook_slider_set_page_size = nullptr;
    CSliderCtrl bareSlider;
    bareSlider.SetRange(0, 1);
    EXPECT_EQ(0, bareSlider.GetPos());

    g_wxhook_spin_set_range = [](void*, int lo, int hi) { s_lo = lo; s_hi = hi; };
    CSpinButtonCtrl spin;
    spin.SetRange(1, 31);
    EXPECT_EQ(1, s_lo);
    EXPECT_EQ(31, s_hi);
    spin.SetRange32(0, 60);
    EXPECT_EQ(60, s_hi);
    g_wxhook_spin_set_range = nullptr;
    spin.SetRange(0, 1);      // null-hook arm
    spin.SetPos(1);
    EXPECT_EQ(0, spin.GetPos());
    spin.SetBuddy(nullptr);
    EXPECT_EQ((CWnd*)nullptr, spin.GetBuddy());
    spin.SetBase(10);

    CIPAddressCtrl ip;
    ip.ClearAddress();
    DWORD addr = 1;
    EXPECT_EQ(0, ip.GetAddress(addr));
    ip.SetAddress(0);

    static unsigned short s_vk, s_mod;
    g_wxhook_hotkey_get = [](void*, unsigned short* vk, unsigned short* mod) {
        *vk = 0x42; *mod = 0x05;
    };
    CHotKeyCtrl hot;
    WORD vk = 0, mod = 0;
    hot.GetHotKey(vk, mod);
    EXPECT_EQ(0x42, (int)vk);
    EXPECT_EQ(0x05, (int)mod);
    EXPECT_EQ((DWORD)0x0542, hot.GetHotKey());   // LOBYTE=vk, HIBYTE=mods
    g_wxhook_hotkey_get = nullptr;
    hot.GetHotKey(vk, mod);
    EXPECT_EQ(0, (int)vk);
    hot.SetHotKey(1, 2);
    hot.SetRules(0, 0);
    (void)s_vk; (void)s_mod;

    CRichEditCtrl rich;
    EXPECT_EQ(0, rich.GetTextLength());
    rich.SetReadOnly();
    rich.SetFont(nullptr);
    EXPECT_EQ(0L, rich.StreamIn(0, nullptr));
    EXPECT_EQ(0L, rich.StreamOut(0, nullptr));
    CHARFORMAT cf;
    EXPECT_FALSE(rich.GetDefaultCharFormat(cf));
    EXPECT_FALSE(rich.SetDefaultCharFormat(cf));
    EXPECT_FALSE(rich.SetSelectionCharFormat(cf));
    rich.ReplaceSel("x");
    EXPECT_EQ((COLORREF)0, rich.SetBackgroundColor(TRUE, 0));
    rich.SetSel(0L, 1L);
    long a = 0, z = 0;
    rich.GetSel(a, z);
    EXPECT_EQ(0, rich.GetSel());

    CScrollBar sb;
    int lo = 1, hi = 1;
    sb.GetScrollRange(&lo, &hi);
    EXPECT_EQ(0, sb.SetScrollPos(1));
    EXPECT_EQ(0, sb.GetScrollPos());
    EXPECT_FALSE(sb.EnableScrollBar());

    CToolTipCtrl tip;
    tip.Activate(TRUE);
    EXPECT_TRUE(tip.AddTool((CWnd*)nullptr, "tip"));
    EXPECT_TRUE(tip.AddTool((CWnd*)nullptr, 1u));
    tip.DelTool(nullptr);
    tip.UpdateTipText("x", nullptr);
    tip.SetMaxTipWidth(100);
    MSG msg = {};
    tip.RelayEvent(&msg);
    tip.SendMessage(0);
    EXPECT_EQ(0, tip.GetDelayTime(0));
    tip.SetDelayTime(0, 1);

    CHeaderCtrl hdr;
    EXPECT_FALSE(hdr.GetItem(0, nullptr));
    EXPECT_FALSE(hdr.SetItem(0, nullptr));
    EXPECT_EQ(-1, hdr.InsertItem(0, nullptr));
    EXPECT_FALSE(hdr.DeleteItem(0));
    hdr.Layout(nullptr);
    EXPECT_EQ(-1, hdr.OrderToIndex(0));

    CStatusBarCtrl sbc;
    sbc.SetText("x");
    EXPECT_FALSE(sbc.GetRect(0, &r));
    CRect crr;
    EXPECT_FALSE(sbc.GetRect(0, crr));
}

TEST(StubSweep, FramesMenusAppRemainder) {
    CFrameWnd fw;
    fw.OnContextHelp(); fw.OnHelp(); fw.OnHelpIndex(); fw.OnHelpFinder(); fw.OnHelpUsing();
    fw.SetActiveView(nullptr);
    EXPECT_EQ((CFrameWnd*)nullptr, fw.GetActiveFrame());
    fw.InitialUpdateFrame(nullptr);

    CControlBar cbar;
    cbar.EnableDocking(0);
    EXPECT_EQ((DWORD)0, cbar.GetBarStyle());
    cbar.SetBarStyle(0);

    CMDIChildWnd child;
    child.SetHandles(0, nullptr);
    child.OnUpdateFrameMenu(TRUE, nullptr, 0);
    CREATESTRUCT cs = {};
    EXPECT_TRUE(child.PreCreateWindow(cs));
    EXPECT_TRUE(child.OnCreateClient(&cs, nullptr));
    child.OnClose();
    child.OnDestroy();
    child.OnSize(0, 1, 1);
    child.OnActivate(0, nullptr, FALSE);

    CMDIFrameWnd frame;
    frame.MDIPrev();
    frame.MDIIconArrange();
    EXPECT_EQ((HMENU)0, frame.GetWindowMenuPopup(0));
    frame.DockControlBar(nullptr);
    frame.FloatControlBar(nullptr, CPoint(0, 0));
    frame.SaveBarState("x");
    frame.LoadBarState("x");
    EXPECT_FALSE(frame.ShowControlBar(nullptr, TRUE, FALSE));
    EXPECT_TRUE(frame.PreCreateWindow(cs));
    EXPECT_EQ((LRESULT)0, frame.WindowProc(0, 0, 0));
    frame.OnUpdateFrameMenu(0);
    frame.OnClose();
    frame.OnDestroy();
    frame.OnSize(0, 1, 1);
    frame.OnActivate(0, nullptr, FALSE);
    frame.OnTimer(0);
    frame.OnSysCommand(0, 0);
    frame.OnUpdateControlBarMenu(nullptr);
    EXPECT_FALSE(frame.OnBarCheck(0));
    frame.OnContextHelp(); frame.OnHelpFinder(); frame.OnHelpUsing(); frame.OnHelpIndex();

    CToolBarCtrl tbc;
    EXPECT_FALSE(tbc.SetButtonInfo(0, nullptr));
    EXPECT_EQ(0, tbc.GetButtonCount());
    RECT r = {};
    EXPECT_FALSE(tbc.GetItemRect(0, &r));

    CToolBar tb;
    EXPECT_FALSE(tb.CreateEx(nullptr));
    EXPECT_FALSE(tb.LoadBitmap(1u));
    EXPECT_FALSE(tb.SetButtons(nullptr, 0));
    tb.SetSizes(CSize(1, 1), CSize(1, 1));
    tb.SetHeight(20);
    EXPECT_EQ(-1, tb.CommandToIndex(1));
    tb.DoPaint(nullptr);

    CStatusBar sbar;
    sbar.SetPaneInfo(0, 0, 0, 0);
    UINT id = 0, style = 0;
    int width = 0;
    sbar.GetPaneInfo(0, id, style, width);
    EXPECT_TRUE(sbar.GetPaneText(0).IsEmpty());
    EXPECT_EQ(-1, sbar.CommandToIndex(1));

    CReBar rebar;
    EXPECT_FALSE(rebar.AddBar(nullptr));

    CSplitterWnd sw;
    EXPECT_FALSE(sw.CreateView(0, 0, nullptr, CSize(1, 1), nullptr));
    EXPECT_EQ((CWnd*)nullptr, sw.GetPane(0, 0));
    sw.SetColumnInfo(0, 1, 1);
    sw.SetRowInfo(0, 1, 1);
    EXPECT_EQ(0, sw.GetColumnCount());
    int cur = 0, mn = 0;
    sw.GetRowInfo(0, cur, mn);
    sw.GetColumnInfo(0, cur, mn);
    sw.RecalcLayout();
    sw.OnLButtonUp(0, CPoint(0, 0));

    CMenu m;
    EXPECT_FALSE(m.AppendMenu(0, 0, (const CBitmap*)nullptr));
    EXPECT_FALSE(m.InsertMenu(0, 0));
    EXPECT_FALSE(m.ModifyMenu(0, 0));
    EXPECT_FALSE(m.DeleteMenu(0, 0));
    EXPECT_FALSE(m.CheckMenuItem(0, 0));
    EXPECT_FALSE(m.EnableMenuItem(0, 0));
    EXPECT_EQ(0u, m.GetMenuState(0, 0));
    CString ms;
    EXPECT_EQ(0, m.GetMenuString(0, ms, 0));
    // GetSubMenu no longer returns nullptr - it materialises the submenu on
    // demand.  Returning nullptr crashed every canonical LoadMenu/GetSubMenu
    // chain; see the CMenu::GetSubMenu comment and the tests below.
    EXPECT_TRUE(m.GetSubMenu(0) != nullptr);
    EXPECT_EQ(0u, m.GetMenuItemID(0));
    EXPECT_FALSE(m.LoadMenu(1u));
    EXPECT_EQ((CMenu*)nullptr, CMenu::FromHandle(0));

    CImageList il;
    EXPECT_FALSE(il.Create(1u, 1, 1, 0));
    EXPECT_FALSE(il.DeleteImageList());
    EXPECT_EQ(-1, il.Add((CBitmap*)nullptr, (COLORREF)0));
    EXPECT_EQ(-1, il.Add((CBitmap*)nullptr, (CBitmap*)nullptr));
    EXPECT_FALSE(il.Replace(0, nullptr));
    EXPECT_FALSE(il.Remove(0));
    EXPECT_FALSE(il.SetImageCount(1));
    EXPECT_FALSE(il.Draw(nullptr, 0, CPoint(0, 0), 0));
    EXPECT_EQ((HIMAGELIST)nullptr, il.GetSafeHandle());
    EXPECT_EQ((HIMAGELIST)nullptr, il.Detach());
    EXPECT_FALSE(il.Attach(nullptr));
    EXPECT_EQ((CImageList*)nullptr, CImageList::FromHandle(nullptr));

    CWinApp* saved = g_pWinApp;
    {
        CWinApp app;
        CCommandLineInfo cli;
        app.ParseCommandLine(cli);
        app.ParseCommandLine((void*)nullptr);
        EXPECT_FALSE(app.DoPrintDialog(nullptr));
        app.HelpHelper(0, 0);
        app.RegisterShellFileTypes();
        app.EnableShellOpen();
        app.LoadFrame(1);
        app.OnHelp();
        app.WinHelp(0);
        CString prompt;
        EXPECT_FALSE(app.DoPromptFileName(prompt, 0, 0, TRUE, nullptr));
        app.AddDocTemplate(nullptr);
        MSG msg = {};
        EXPECT_FALSE(app.PreTranslateMessage(&msg));
        app.OnFileNew(); app.OnFileOpen(); app.OnFilePrintSetup();
    }
    g_pWinApp = saved;

    CWinThread th;
    EXPECT_FALSE(th.OnIdle(0));
    EXPECT_FALSE(th.PumpMessage());
    MSG msg2 = {};
    EXPECT_FALSE(th.IsIdleMessage(&msg2));
    th.SetThreadPriority(0);
    EXPECT_FALSE(th.PostThreadMessage(0, 0, 0));
    EXPECT_TRUE(AfxGetThread() != nullptr);
}

TEST(StubSweep, GlobalApiRemainder) {
    // 638-647 family
    EXPECT_FALSE(LockWindowUpdate((HWND)nullptr));
    UnlockWindowUpdate();
    EXPECT_FALSE(SetMenuDefaultItem((HMENU)0, 0, 0));
    EXPECT_TRUE(InvalidateRgn((HWND)nullptr, nullptr));
    EXPECT_TRUE(ScrollWindow((HWND)nullptr, 0, 0, nullptr, nullptr));
    EXPECT_FALSE(SystemParametersInfo(0, 0, nullptr, 0));
    SIZE sz;
    EXPECT_FALSE(GetTextExtentPoint32W(nullptr, nullptr, 0, &sz));
    CSize csz;
    EXPECT_FALSE(GetTextExtentPoint32W(nullptr, nullptr, 0, &csz));
    EXPECT_FALSE(ExtTextOutW(nullptr, 0, 0, 0, nullptr, nullptr, 0, nullptr));
    EXPECT_FALSE(GetTextExtentPoint32(nullptr, "x", 1, &sz));

    // CDC leftovers
    CDC dc;
    EXPECT_FALSE(dc.Pie(0, 0, 1, 1, 0, 0, 1, 1));
    EXPECT_FALSE(dc.Pie(CRect(0, 0, 1, 1), CPoint(0, 0), CPoint(1, 1)));
    RECT rr = {};
    EXPECT_FALSE(dc.InvertRect(&rr));
    CRect crr(0, 0, 1, 1);
    EXPECT_FALSE(dc.InvertRect(&crr));
    EXPECT_FALSE(dc.ExtFloodFill(0, 0, 0, 0));
    EXPECT_FALSE(dc.SetPixelV(0, 0, 0));
    EXPECT_EQ((COLORREF)0, dc.GetPixel(0, 0));
    EXPECT_EQ(0, dc.SetPolyFillMode(1));
    EXPECT_EQ(0, dc.GetPolyFillMode());
    CString face;
    EXPECT_EQ(0, dc.GetTextFace(face));
    EXPECT_EQ(0, dc.SetGraphicsMode(1));
    EXPECT_FALSE(dc.SetWorldTransform(nullptr));
    EXPECT_FALSE(dc.ModifyWorldTransform(nullptr, 0));
    EXPECT_FALSE(dc.GradientFill(nullptr, 0, nullptr, 0, 0));
    EXPECT_EQ((CBrush*)nullptr, dc.GetCurrentBrush());
    EXPECT_EQ((CPen*)nullptr, dc.GetCurrentPen());
    EXPECT_EQ((CBitmap*)nullptr, dc.GetCurrentBitmap());
    EXPECT_EQ(0, dc.SetStretchBltMode(0));
    EXPECT_EQ(0, dc.GetStretchBltMode());
    EXPECT_EQ(0, dc.SetTextCharacterExtra(0));
    EXPECT_EQ(0u, dc.GetTextAlign());
    EXPECT_EQ(0, dc.GetBkMode());
    EXPECT_EQ(0, dc.GetROP2());
    EXPECT_EQ((COLORREF)0, dc.GetTextColor());
    EXPECT_EQ((COLORREF)0, dc.GetBkColor());
    EXPECT_FALSE(dc.GetTextFace(0, nullptr));
    EXPECT_FALSE(dc.GetClipBox(&rr));
    EXPECT_EQ(0, dc.ExcludeClipRect(0, 0, 1, 1));
    EXPECT_EQ(0, dc.IntersectClipRect(0, 0, 1, 1));
    EXPECT_EQ(0, dc.IntersectClipRect(crr));
    EXPECT_EQ(0, dc.ExcludeClipRect((LPCRECT)&rr));
    EXPECT_EQ((CPalette*)nullptr, dc.SelectPalette(nullptr, FALSE));
    EXPECT_EQ(0u, dc.RealizePalette());

    // clipboard / global mem leftovers
    EXPECT_FALSE(EmptyClipboard());
    EXPECT_EQ((HANDLE)nullptr, GetClipboardData(CF_TEXT));
    EXPECT_EQ((HANDLE)nullptr, SetClipboardData(CF_TEXT, nullptr));
    EXPECT_EQ((SIZE_T)0, GlobalSize(nullptr));
    DragAcceptFiles((HWND)nullptr, TRUE);

    // sync-object family (inert)
    EXPECT_EQ((HANDLE)nullptr, CreateMutex(nullptr, FALSE, "m"));
    EXPECT_EQ((DWORD)0, WaitForSingleObject(nullptr, 0));
    EXPECT_FALSE(ReleaseMutex(nullptr));
    EXPECT_FALSE(SetEvent(nullptr));
    EXPECT_FALSE(ResetEvent(nullptr));
    EXPECT_EQ((HANDLE)nullptr, CreateEvent(nullptr, FALSE, FALSE, "e"));
    EXPECT_EQ((HANDLE)nullptr, CreateThread(nullptr, 0, nullptr, nullptr, 0, nullptr));
    EXPECT_FALSE(TerminateThread(nullptr, 0));
    EXPECT_EQ((DWORD)0, ResumeThread(nullptr));
    EXPECT_EQ((DWORD)0, SuspendThread(nullptr));

    // window-info family (6639-6657)
    EXPECT_FALSE(BringWindowToTop((HWND)nullptr));
    EXPECT_EQ(0L, GetClassLong((HWND)nullptr, 0));
    char cls[16];
    EXPECT_EQ(0, GetClassName((HWND)nullptr, cls, sizeof cls));
    EXPECT_EQ((HWND)nullptr, GetNextWindow((HWND)nullptr, 0));
    EXPECT_EQ((HWND)nullptr, GetParent((HWND)nullptr));
    EXPECT_EQ((HWND)nullptr, GetTopWindow((HWND)nullptr));
    EXPECT_EQ((HDC)nullptr, GetWindowDC((HWND)nullptr));
    EXPECT_EQ(0, ReleaseDC((HWND)nullptr, (HDC)nullptr));
    EXPECT_FALSE(TrackPopupMenu((HMENU)0, 0, 0, 0, 0, (HWND)nullptr, nullptr));
    char wtxt[8];
    EXPECT_EQ(0, GetWindowText((HWND)nullptr, wtxt, sizeof wtxt));
    EXPECT_FALSE(SetWindowText((HWND)nullptr, "x"));
    EXPECT_EQ(0, GetWindowTextLength((HWND)nullptr));
    RECT er = {};
    EXPECT_FALSE(DrawEdge((HDC)nullptr, &er, 0, 0));
    EXPECT_EQ((HWND)nullptr, GetWindow((HWND)nullptr, 0));
    char wdir[8];
    EXPECT_FALSE(GetWindowsDirectory(wdir, sizeof wdir));
    EXPECT_EQ((HCURSOR)(void*)0x1, CopyCursor((HCURSOR)(void*)0x1));
    EXPECT_TRUE(InitCommonControls());
    EXPECT_EQ(S_OK, CoInitialize(nullptr));
    CoUninitialize();
    EXPECT_EQ(0L, GetWindowLong((HWND)nullptr, 0));
    EXPECT_EQ((LONG_PTR)0, GetWindowLongPtr((HWND)nullptr, 0));
    EXPECT_EQ((LONG_PTR)0, SetWindowLongPtr((HWND)nullptr, 0, 0));

    // misc singles
    EXPECT_FALSE(IsZoomed((HWND)nullptr));
    EXPECT_FALSE(IsWindowVisible((HWND)nullptr));
    PostQuitMessage(0);
    EXPECT_EQ(0u, RegisterWindowMessage("m"));
    CPoint cp;
    EXPECT_FALSE(ClientToScreen((HWND)nullptr, &cp));
    EXPECT_FALSE(ScreenToClient((HWND)nullptr, &cp));
    EXPECT_FALSE(GetCursorPos(&cp));
    EXPECT_FALSE(SetCursorPos(0, 0));
    EXPECT_EQ((HCURSOR)nullptr, SetCursor((HCURSOR)nullptr));
    EXPECT_FALSE(DeleteMenu((HMENU)0, 0, 0));
    EXPECT_EQ(0, StartDoc((HDC)nullptr, nullptr));
    EXPECT_EQ((HCERTSTORE)nullptr, CertOpenSystemStore(nullptr, "MY"));
    EXPECT_FALSE(SetBrushOrgEx(nullptr, 0, 0, nullptr));
    EXPECT_FALSE(SetGraphicsMode(nullptr, 0));
    EXPECT_FALSE(SetWorldTransform(nullptr, nullptr));
    EXPECT_FALSE(ModifyWorldTransform(nullptr, nullptr, 0));
    EXPECT_EQ((HPEN)nullptr, ExtCreatePen(0, 0, nullptr, 0, nullptr));
    EXPECT_EQ((HBITMAP)nullptr, CreateDIBSection(nullptr, nullptr, 0, nullptr, nullptr, 0));
    EXPECT_EQ((HDWP)nullptr, BeginDeferWindowPos(0));
    EXPECT_EQ((HACCEL)nullptr, CreateAcceleratorTable(nullptr, 0));
    EXPECT_EQ(0, CopyAcceleratorTable(nullptr, nullptr, 0));
    EXPECT_EQ((HACCEL)nullptr, MFC_GetDefaultAccelerator());
    gRectType gr = { 1, 2, 11, 22 };
    EXPECT_EQ(10, gRect_width(gr));
    EXPECT_EQ(20, gRect_height(gr));
    EXPECT_EQ(1252u, GetACP());
    EXPECT_EQ(437u, GetOEMCP());
    EXPECT_EQ(0, GetLocaleInfo(0, 0, nullptr, 0));
    EXPECT_EQ((LCID)0, GetUserDefaultLCID());
    char loadbuf[8];
    EXPECT_FALSE(AfxLoadString(1, loadbuf, sizeof loadbuf));
    CString loadstr;
    EXPECT_FALSE(AfxLoadString(1, loadstr));
    CString fmt2;
    AfxFormatString2(fmt2, 0, "a", "b");
    AfxOleLockApp();
    AfxOleUnlockApp();
    EXPECT_FALSE(SetRegistryKey("k"));
    EXPECT_EQ(0u, AfxGetApp_nID());
    bool threw = false;
    try { AfxThrowResourceException(); } catch (const std::exception&) { threw = true; }
    EXPECT_TRUE(threw);
}

TEST(StubSweep, DialogsAndDatesRemainder) {
    CFindReplaceDialog fr;
    EXPECT_TRUE(fr.GetFindString().IsEmpty());
    EXPECT_TRUE(fr.GetReplaceString().IsEmpty());
    EXPECT_FALSE(fr.FindNext());
    EXPECT_FALSE(fr.ReplaceCurrent());
    EXPECT_FALSE(fr.ReplaceAll());
    EXPECT_FALSE(fr.MatchCase());
    EXPECT_FALSE(fr.MatchWholeWord());
    EXPECT_FALSE(fr.SearchDown());

    CPropertyPage page(2);
    page.OnReset();
    page.OnOK();
    page.OnCancel();
    page.CancelToClose();
    RECT r = {};
    page.MapWindowPoints(nullptr, &r);
    page.OnRButtonDown(0, CPoint(0, 0));
    page.OnContextMenu(nullptr, CPoint(0, 0));
    EXPECT_FALSE(page.OnHelpInfo(nullptr));

    CPrintDialog pd(FALSE);
    EXPECT_EQ((HDC)nullptr, pd.GetPrinterDC());
    EXPECT_EQ((HDC)nullptr, pd.GetHDC());
    EXPECT_FALSE(pd.PrintSelection());
    EXPECT_FALSE(pd.PrintRange());
    EXPECT_EQ(1, pd.GetToPage());
    pd.SetFromPage(1);
    pd.SetToPage(2);
    EXPECT_FALSE(pd.GetDefaults());
    EXPECT_TRUE(pd.GetDeviceName().IsEmpty());
    EXPECT_TRUE(pd.GetPortName().IsEmpty());

    COleDateTime full(2024, 1, 2, 3, 4, 5);   // inert extended ctor
    EXPECT_NEAR(0.0, full.m_dt, 1e-12);
    COleDateTime now = COleDateTime::GetCurrentTime();
    EXPECT_NEAR(0.0, now.m_dt, 1e-12);        // stub: epoch
    COleDateTime d;
    EXPECT_FALSE(d.SetDateTime(2024, 1, 2, 3, 4, 5));
    EXPECT_FALSE(d.SetDate(2024, 1, 2));
    EXPECT_EQ(0, d.GetYear());
    EXPECT_EQ(0, d.GetMonth());
    EXPECT_EQ(0, d.GetDay());
    EXPECT_EQ(0, d.GetHour());
    EXPECT_EQ(0, d.GetMinute());
    EXPECT_EQ(0, d.GetSecond());
    EXPECT_TRUE(d.Format("%Y").IsEmpty());
    EXPECT_TRUE(d.Format(0u).IsEmpty());

    // CFile / CStdioFile leftovers
    CFile f;
    f.SetLength(0);
    f.Abort();
    CFileStatus st;
    EXPECT_FALSE(CFile::GetStatus("/tmp", (void*)nullptr));
    (void)st;
    FILE* raw = tmpfile();
    ASSERT_TRUE(raw != nullptr);
    fputs("attached\n", raw);
    rewind(raw);
    CStdioFile attached(raw);   // attach ctor takes ownership of the FILE*
    CString line;
    EXPECT_TRUE(attached.ReadString(line));
    EXPECT_STREQ("attached", (LPCTSTR)line);

    // COleTemplateServer + misc COM residue
    COleTemplateServer srv;
    (void)srv;
    CDumpContext dump;
    dump << 1UL << (const void*)nullptr;
}

// ═══ CWnd core (platform/compat.h) ═════════════════════════════════════════

TEST(Wnd, MessageAndGeometryHooksRoute) {
    static long s_sendMsg; static int s_postMsg, s_invalidate, s_update,
        s_show, s_focus, s_moveX, s_moveW, s_destroyed, s_flash;
    s_sendMsg = s_postMsg = s_invalidate = s_update = s_show = s_focus =
        s_moveX = s_moveW = s_destroyed = s_flash = 0;
    g_wxhook_send_message = [](void*, unsigned m, uintptr_t, intptr_t) -> long {
        s_sendMsg = (long)m; return 7;
    };
    g_wxhook_post_message = [](void*, unsigned m, uintptr_t, intptr_t) -> int {
        s_postMsg = (int)m; return 1;
    };
    g_wxhook_invalidate = [](void*, bool) { ++s_invalidate; };
    g_wxhook_update_window = [](void*) { ++s_update; };
    g_wxhook_show_window = [](void*, int cmd) { s_show = cmd; };
    g_wxhook_set_focus = [](void*) { ++s_focus; };
    g_wxhook_move_window = [](void*, int x, int, int w, int, bool) {
        s_moveX = x; s_moveW = w;
    };
    g_wxhook_destroy_window = [](void*) { ++s_destroyed; };
    g_wxhook_flash_icon = []() { ++s_flash; };

    CWnd w;
    w.m_hWnd = (HWND)(void*)0x77;
    EXPECT_EQ((LRESULT)7, w.SendMessage(0x111));
    EXPECT_EQ(0x111L, s_sendMsg);
    EXPECT_TRUE(w.PostMessage(0x222));
    EXPECT_EQ(0x222, s_postMsg);
    w.Invalidate();
    w.InvalidateRect((const RECT*)nullptr);
    w.RedrawWindow();            // routes to invalidate too
    EXPECT_EQ(3, s_invalidate);
    w.UpdateWindow();
    EXPECT_EQ(1, s_update);
    w.ShowWindow(5);
    EXPECT_EQ(5, s_show);
    w.SetFocus();
    EXPECT_EQ(1, s_focus);
    w.MoveWindow(10, 20, 30, 40);
    EXPECT_EQ(10, s_moveX);
    EXPECT_EQ(30, s_moveW);
    RECT r; r.left = 1; r.top = 2; r.right = 7; r.bottom = 12;
    w.MoveWindow(&r);            // RECT* form converts to x/y/w/h
    EXPECT_EQ(1, s_moveX);
    EXPECT_EQ(6, s_moveW);
    w.FlashWindow(TRUE);
    EXPECT_EQ(1, s_flash);
    w.FlashWindow(FALSE);        // stop is a no-op (one-shot request)
    EXPECT_EQ(1, s_flash);
    w.DestroyWindow();
    EXPECT_EQ(1, s_destroyed);
    EXPECT_EQ((HWND)nullptr, w.m_hWnd);

    g_wxhook_send_message = nullptr;
    g_wxhook_post_message = nullptr;
    g_wxhook_invalidate = nullptr;
    g_wxhook_update_window = nullptr;
    g_wxhook_show_window = nullptr;
    g_wxhook_set_focus = nullptr;
    g_wxhook_move_window = nullptr;
    g_wxhook_destroy_window = nullptr;
    g_wxhook_flash_icon = nullptr;
    // null-hook arms
    CWnd bare;
    EXPECT_EQ((LRESULT)0, bare.SendMessage(1));
    EXPECT_FALSE(bare.PostMessage(1));
    bare.Invalidate();
    bare.UpdateWindow();
    bare.ShowWindow(1);
    bare.SetFocus();
    bare.MoveWindow(0, 0, 1, 1);
    bare.DestroyWindow();
    bare.FlashWindow(TRUE);
}

// CWnd::SetWindowPlacement was a permanent FALSE, so even once
// CWindowPlacement::Restore got past its screen-intersect test it had nowhere to
// put the geometry it had just read out of the database: every dialog and world
// window opened at its default position, however carefully it had been placed.
TEST(Wnd, SetWindowPlacementMovesTheWindow) {
    static int s_x, s_y, s_w, s_h, s_calls;
    static bool s_repaint;
    s_calls = 0;
    void (*const saved)(void*, int, int, int, int, bool) = g_wxhook_move_window;
    g_wxhook_move_window = [](void*, int x, int y, int w, int h, bool rp) {
        s_x = x; s_y = y; s_w = w; s_h = h; s_repaint = rp; ++s_calls;
    };

    CWnd w;
    WINDOWPLACEMENT wp = {};
    wp.rcNormalPosition.left   = 40;
    wp.rcNormalPosition.top    = 50;
    wp.rcNormalPosition.right  = 340;
    wp.rcNormalPosition.bottom = 250;
    EXPECT_TRUE(w.SetWindowPlacement(&wp));
    EXPECT_EQ(1, s_calls);
    EXPECT_EQ(40, s_x);
    EXPECT_EQ(50, s_y);
    // Width/height, NOT right/bottom — the two differ here on purpose, so a
    // straight-through copy of the rect fails.
    EXPECT_EQ(300, s_w);
    EXPECT_EQ(200, s_h);
    EXPECT_TRUE(s_repaint);

    EXPECT_FALSE(w.SetWindowPlacement(nullptr));   // and a null placement is refused
    EXPECT_EQ(1, s_calls);

    g_wxhook_move_window = saved;
}

// Every CWnd geometry-mapping overload was a no-op for anything that is not a
// CView -- CView carries its own real copies, which is why the gap survived in
// the classes that actually hit it (CDialog, CButton, CEdit, every control).
// 5cfd0e5 fixed GetClientRect's pointer forms; the CRect& spelling, which is
// what the standard MFC idiom `CRect r; wnd.GetClientRect(r);` binds -- it is
// the sole candidate, an unadorned name cannot bind a CRect* -- still zeroed
// the rect, and ClientToScreen/ScreenToClient were untouched on every overload.
//
// The structs are pre-poisoned, so "the call did nothing" cannot pass.
static int s_c2sDx, s_c2sDy;

TEST(Wnd, GeometryMappingOverloadsAllAgree) {
    HookGuard<decltype(g_wxhook_get_client_rect)> g1(g_wxhook_get_client_rect);
    HookGuard<decltype(g_wxhook_get_window_rect)> g2(g_wxhook_get_window_rect);
    HookGuard<decltype(g_wxhook_client_to_screen)> g3(g_wxhook_client_to_screen);
    HookGuard<decltype(g_wxhook_screen_to_client)> g4(g_wxhook_screen_to_client);

    g_wxhook_get_client_rect = [](const void*, RECT* r) {
        r->left = 0; r->top = 0; r->right = 300; r->bottom = 200;
    };
    g_wxhook_get_window_rect = [](const void*, RECT* r) {
        r->left = 7; r->top = 9; r->right = 307; r->bottom = 209;
    };
    // A translation, so client and screen coordinates genuinely differ -- an
    // identity mapping would make every assertion below vacuous.
    s_c2sDx = 100; s_c2sDy = 40;
    g_wxhook_client_to_screen = [](const void*, int* x, int* y) {
        *x += s_c2sDx; *y += s_c2sDy;
    };
    g_wxhook_screen_to_client = [](const void*, int* x, int* y) {
        *x -= s_c2sDx; *y -= s_c2sDy;
    };

    CWnd w;
    w.m_hWnd = (HWND)(void*)1;

    // GetClientRect: by-name and address-of must give the same real answer.
    CRect byName(-1, -1, -1, -1);
    CRect byAddr(-2, -2, -2, -2);
    w.GetClientRect(byName);
    w.GetClientRect(&byAddr);
    EXPECT_EQ(0L,   (long)byName.left);
    EXPECT_EQ(300L, (long)byName.right);
    EXPECT_EQ(200L, (long)byName.bottom);
    EXPECT_EQ((long)byAddr.left,   (long)byName.left);
    EXPECT_EQ((long)byAddr.right,  (long)byName.right);
    EXPECT_EQ((long)byAddr.bottom, (long)byName.bottom);

    // GetWindowRect gained the same reference spelling.
    CRect win(-1, -1, -1, -1);
    w.GetWindowRect(win);
    EXPECT_EQ(7L,   (long)win.left);
    EXPECT_EQ(307L, (long)win.right);

    // ClientToScreen maps BOTH corners of a rect, by name and by address.
    CRect rName(10, 20, 110, 220);
    CRect rAddr(10, 20, 110, 220);
    w.ClientToScreen(rName);
    w.ClientToScreen(&rAddr);
    EXPECT_EQ(110L, (long)rName.left);     // 10 + 100
    EXPECT_EQ(60L,  (long)rName.top);      // 20 + 40
    EXPECT_EQ(210L, (long)rName.right);    // 110 + 100
    EXPECT_EQ(260L, (long)rName.bottom);   // 220 + 40
    EXPECT_EQ((long)rAddr.left,   (long)rName.left);
    EXPECT_EQ((long)rAddr.bottom, (long)rName.bottom);

    // ...and ScreenToClient undoes it, so the round trip returns the original.
    w.ScreenToClient(rName);
    EXPECT_EQ(10L,  (long)rName.left);
    EXPECT_EQ(220L, (long)rName.bottom);

    // The point overloads: CPoint (which is NOT a POINT here) and POINT.
    CPoint cp(10, 20);
    w.ClientToScreen(&cp);
    EXPECT_EQ(110, cp.x);
    EXPECT_EQ(60,  cp.y);
    w.ScreenToClient(&cp);
    EXPECT_EQ(10, cp.x);
    EXPECT_EQ(20, cp.y);

    POINT pt = { 10, 20 };
    w.ClientToScreen(&pt);
    EXPECT_EQ(110L, (long)pt.x);
    EXPECT_EQ(60L,  (long)pt.y);
    w.ScreenToClient(&pt);
    EXPECT_EQ(10L, (long)pt.x);
    EXPECT_EQ(20L, (long)pt.y);

    RECT raw = { 10, 20, 110, 220 };
    w.ClientToScreen(&raw);
    EXPECT_EQ(110L, (long)raw.left);
    EXPECT_EQ(260L, (long)raw.bottom);

    // A null pointer and a hookless CWnd are both refusals, not crashes.
    w.ClientToScreen((POINT*)nullptr);
    w.ScreenToClient((CRect*)nullptr);
    g_wxhook_client_to_screen = nullptr;
    CPoint untouched(3, 4);
    w.ClientToScreen(&untouched);
    EXPECT_EQ(3, untouched.x);
    EXPECT_EQ(4, untouched.y);
}

// The Complete Word popup is anchored at the caret by a three-step chain --
// CEdit::PosFromChar -> CWnd::ClientToScreen -> CWnd::SetWindowPos -- and every
// step used to discard its input.  ClientToScreen is pinned by the test above;
// these are the other two.
static int s_pfcChar, s_swpX, s_swpY, s_swpCx, s_swpCy, s_swpCalls;
static bool s_swpMove, s_swpSize;

TEST(Edit, PosFromCharAsksTheControlAndFallsBackToTheOrigin) {
    HookGuard<decltype(g_wxhook_edit_pos_from_char)> g(g_wxhook_edit_pos_from_char);

    // No hook (headless / mushcore): the historical (0,0), unchanged.
    g_wxhook_edit_pos_from_char = nullptr;
    CEdit bare;
    CPoint none = bare.PosFromChar(17);
    EXPECT_EQ(0, none.x);
    EXPECT_EQ(0, none.y);

    s_pfcChar = -1;
    g_wxhook_edit_pos_from_char = [](const void*, int nChar, int* x, int* y) {
        s_pfcChar = nChar; *x = 64; *y = 26;
    };
    CEdit e;
    CPoint pt = e.PosFromChar(17);
    EXPECT_EQ(17, s_pfcChar);        // the index really travels
    EXPECT_EQ(64, pt.x);
    EXPECT_EQ(26, pt.y);

    // A hook that answers nothing leaves the caller's (0,0) alone rather than
    // writing junk -- PositionToCoords can decline.
    g_wxhook_edit_pos_from_char = [](const void*, int, int*, int*) {};
    CPoint silent = e.PosFromChar(3);
    EXPECT_EQ(0, silent.x);
    EXPECT_EQ(0, silent.y);
}

TEST(Wnd, SetWindowPosHonoursTheMoveAndSizeFlagsSeparately) {
    HookGuard<decltype(g_wxhook_set_window_pos)> g(g_wxhook_set_window_pos);
    s_swpCalls = 0;
    g_wxhook_set_window_pos = [](void*, int x, int y, int cx, int cy,
                                 bool bMove, bool bSize) {
        s_swpX = x; s_swpY = y; s_swpCx = cx; s_swpCy = cy;
        s_swpMove = bMove; s_swpSize = bSize; ++s_swpCalls;
    };

    CWnd w;

    // The Complete Word popup's own call: move, keep the size.
    w.SetWindowPos(nullptr, 120, 340, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    EXPECT_EQ(1, s_swpCalls);
    EXPECT_EQ(120, s_swpX);
    EXPECT_EQ(340, s_swpY);
    EXPECT_TRUE(s_swpMove);
    EXPECT_FALSE(s_swpSize);

    // Resize in place (mainfrm's info-bar rich edit).
    w.SetWindowPos(nullptr, 0, 0, 3000, 20, SWP_NOMOVE | SWP_NOZORDER);
    EXPECT_EQ(2, s_swpCalls);
    EXPECT_FALSE(s_swpMove);
    EXPECT_TRUE(s_swpSize);
    EXPECT_EQ(3000, s_swpCx);
    EXPECT_EQ(20, s_swpCy);

    // Both at once.
    w.SetWindowPos(nullptr, 5, 6, 7, 8, 0);
    EXPECT_EQ(3, s_swpCalls);
    EXPECT_TRUE(s_swpMove);
    EXPECT_TRUE(s_swpSize);

    // The always-on-top spelling -- SetWindowPos(&wndTopMost, 0,0,0,0,
    // SWP_NOACTIVATE|SWP_NOMOVE|SWP_NOSIZE) at mainfrm.cpp:1529 and
    // MUSHclient.cpp:768 -- asks only for a Z-order change, and its coordinates
    // are dummies.  Acting on them would slam every world window to the origin,
    // so this MUST NOT reach the hook at all.
    w.SetWindowPos(&CWnd::wndTopMost, 0, 0, 0, 0,
                   SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    EXPECT_EQ(3, s_swpCalls);
}

TEST(Wnd, InertStubSurface) {
    CWnd w;
    EXPECT_EQ((void*)nullptr, w.GetSafeHwnd());
    EXPECT_FALSE(w.IsWindow());
    EXPECT_TRUE(w.IsWindowEnabled());
    EXPECT_FALSE(w.IsWindowVisible());       // null m_hWnd
    w.m_hWnd = (HWND)(void*)1;
    EXPECT_TRUE(w.IsWindowVisible());
    w.m_hWnd = nullptr;
    w.SetWindowPos(nullptr, 0, 0, 1, 1, 0);
    w.BringWindowToTop();
    w.SetRedraw();
    EXPECT_EQ((CWnd*)nullptr, w.GetParent());
    EXPECT_EQ((CWnd*)nullptr, w.GetOwner());
    EXPECT_EQ((CWnd*)nullptr, w.SetParent(nullptr));
    RECT r = {};
    w.MapWindowPoints(nullptr, &r);
    POINT pts[1] = {};
    w.MapWindowPoints(nullptr, pts, 1);
    EXPECT_EQ(0u, w.IsDlgButtonChecked(1));
    EXPECT_FALSE(w.CheckDlgButton(1, 1));
    EXPECT_EQ((CWnd*)nullptr, w.SetCapture());
    EXPECT_EQ((CWnd*)nullptr, CWnd::ReleaseCapture());
    EXPECT_EQ((CWnd*)nullptr, w.GetFocus());
    EXPECT_EQ((CWnd*)nullptr, w.GetWindow(0));
    EXPECT_EQ(0, w.GetDlgCtrlID());
    EXPECT_EQ((UINT_PTR)0, w.SetTimer(1, 100));
    EXPECT_FALSE(w.KillTimer(1));
    w.SetScrollPos(0, 1);
    EXPECT_EQ(0, w.GetScrollPos(0));
    w.SetScrollRange(0, 0, 10);
    w.EnableScrollBar(0);
    w.ShowScrollBar(0);
    w.ScrollWindow(1, 1);
    EXPECT_EQ((CDC*)nullptr, w.GetDC());
    w.ReleaseDC(nullptr);
    EXPECT_EQ((CMenu*)nullptr, w.GetMenu());
    EXPECT_FALSE(w.SetMenu(0));
    EXPECT_EQ((CMenu*)nullptr, w.GetSystemMenu(FALSE));
    w.DrawMenuBar();
    EXPECT_EQ((HCURSOR)nullptr, CWnd::SetCursor(nullptr));
    EXPECT_FALSE(w.LockWindowUpdate());
    w.UnlockWindowUpdate();
    w.ModifyStyle(0, 0);
    w.ModifyStyleEx(0, 0);
    EXPECT_EQ((DWORD)0, w.GetStyle());
    EXPECT_EQ((DWORD)0, w.GetExStyle());
    POINT p = {};
    w.ScreenToClient(&p);
    w.ClientToScreen(&p);
    w.ScreenToClient(&r);
    w.ClientToScreen(&r);
    CPoint cp;
    w.ScreenToClient(&cp);
    w.ClientToScreen(&cp);
    CRect cr;
    w.ScreenToClient(&cr);
    w.ClientToScreen(&cr);
    EXPECT_FALSE(w.CreateEx(0, "cls", "title", 0, 0, 0, 1, 1, nullptr, (HMENU)0));
    w.CenterWindow();
    w.SetFont(nullptr);
    EXPECT_EQ((CFont*)nullptr, w.GetFont());
    w.DragAcceptFiles();
    EXPECT_FALSE(w.SetForegroundWindow());
    WINDOWPLACEMENT wp = {};
    EXPECT_FALSE(w.SetWindowPlacement(&wp));       // no move hook installed here
    EXPECT_FALSE(w.SetWindowPlacement(nullptr));
    w.MessageBox("msg");
    EXPECT_FALSE(w.IsIconic());
    EXPECT_FALSE(w.IsZoomed());
    EXPECT_FALSE(w.SubclassDlgItem(1, nullptr));
    EXPECT_FALSE(w.SubclassWindow(nullptr));
    EXPECT_EQ((LRESULT)0, w.Default());
    EXPECT_EQ(0, w.OnCreate(nullptr));
    w.OnDestroy();
    EXPECT_FALSE(w.OnEraseBkgnd(nullptr));
    EXPECT_EQ((CWnd*)nullptr, CWnd::GetDesktopWindow());
    EXPECT_EQ((CWnd*)nullptr, CWnd::GetForegroundWindow());
    MSG msg = {};
    EXPECT_FALSE(w.IsDialogMessage(&msg));
    EXPECT_EQ(0, w.GetWindowTextLength());
    RECT cr2 = { 5, 5, 6, 6 };
    w.GetClientRect(&cr2);      // no hook: left untouched by CWnd base
    // CCmdTarget + CEvent residue
    CCmdTarget tgt;
    tgt.EnableAutomation();
    tgt.BeginWaitCursor();
    tgt.EndWaitCursor();
    tgt.RestoreWaitCursor();
    CEvent ev;
    ev.SetEvent();
    ev.ResetEvent();
}

// ═══ Round-4 coverage: command-UI dispatch, CDocument base serialize,
//     SafeArray LONG shims, FindResource, misc real-logic arms ══════════════

TEST(MessageMap, UpdateUiDispatchRunsHandler) {
    MapTarget t;
    CCmdUI ui;
    ui.m_nID = 400;
    EXPECT_TRUE(AfxDispatchUpdateUI(&t, &ui));    // handler disables + checks

    CCmdUI miss;
    miss.m_nID = 999;
    EXPECT_FALSE(AfxDispatchUpdateUI(&t, &miss)); // no matching entry

    EXPECT_FALSE(AfxDispatchUpdateUI(nullptr, &ui));
    EXPECT_FALSE(AfxDispatchUpdateUI(&t, nullptr));

    // a derived target walks up to the base map for the entry
    DerivedTarget d;
    CCmdUI ui2;
    ui2.m_nID = 400;
    EXPECT_TRUE(AfxDispatchUpdateUI(&d, &ui2));
}

namespace {
// Minimal document with real Serialize both ways.
class SerDoc : public CDocument {
public:
    CString m_payload;
    void Serialize(CArchive& ar) override {
        if (ar.IsStoring())
            ar.WriteString(m_payload + "\n");
        else
            ar.ReadString(m_payload);
    }
};
} // namespace

TEST(DocBase, OnSaveThenOnOpenRoundTrip) {
    char path[128];
    snprintf(path, sizeof path, "/tmp/mc_test_mfc_%d_doc.txt", (int)getpid());

    SerDoc out;
    out.m_payload = "round trip payload";
    EXPECT_TRUE(out.OnSaveDocument(path));

    SerDoc in;
    EXPECT_TRUE(in.OnOpenDocument(path));
    EXPECT_STREQ("round trip payload", (LPCTSTR)in.m_payload);
    unlink(path);

    // missing file: the open-failure arm
    SerDoc miss;
    EXPECT_FALSE(miss.OnOpenDocument("/nonexistent-r4/doc.txt"));
    // unwritable path: the save-failure arm
    SerDoc bad;
    EXPECT_FALSE(bad.OnSaveDocument("/nonexistent-r4/doc.txt"));
}

// The one-element index array below is load-bearing, and only a sanitizer can
// see it: the LONG* overload used to copy index[1] as well, reading off the
// end of it.  An out-of-bounds READ has no observable effect, so this test
// passes either way in a normal build -- under -DMUSHCLIENT_SANITIZE=address
// the unfixed shim reports a stack-buffer-overflow here.  Do not "tidy" the
// array to two elements.  See docs/SANITIZERS.md.
TEST(MiscShims, SafeArrayLongPointerOverloads) {
    COleSafeArray sa;
    sa.CreateOneDim(VT_VARIANT, 3, NULL, 0);

    LONG lo = 99, hi = -99;
    EXPECT_EQ(S_OK, SafeArrayGetLBound(sa.m_psa, 1, &lo));
    EXPECT_EQ(S_OK, SafeArrayGetUBound(sa.m_psa, 1, &hi));
    EXPECT_EQ(0L, lo);
    EXPECT_EQ(2L, hi);

    long putIx = 1;
    COleVariant v("x");
    sa.PutElement(&putIx, &v);
    LONG getIx[1] = { 1 };
    VARIANT out;
    VariantInit(&out);
    EXPECT_EQ(S_OK, SafeArrayGetElement(sa.m_psa, getIx, &out));
    EXPECT_EQ((int)VT_BSTR, (int)out.vt);
    VariantClear(&out);
}

TEST(MiscShims, FindResourceTextByNumericId) {
    // TEXT + numeric id → the id itself is the handle
    HRSRC h = FindResource(nullptr, (LPCTSTR)(uintptr_t)42, "TEXT");
    EXPECT_EQ((void*)(uintptr_t)42, (void*)h);
    // non-TEXT type → null
    EXPECT_EQ((void*)nullptr, (void*)FindResource(nullptr, (LPCTSTR)(uintptr_t)42, "BITMAP"));
}

TEST(MiscShims, GlobalMemoryStatusZeroesAndSmallStubs) {
    MEMORYSTATUS ms;
    ms.dwMemoryLoad = 55;
    GlobalMemoryStatus(&ms);
    EXPECT_EQ(0u, (unsigned)ms.dwMemoryLoad);
    GlobalMemoryStatus(nullptr);   // null-safe

    CDC dc;
    EXPECT_FALSE(dc.TextOut(0, 0, CString("s")));   // CString overload
}

// dwTotalPhys is REAL, not zero.  It has one consumer and that consumer is a
// warning: the world's output-buffer page refuses sizes whose
// 16 MB + 60 bytes/line exceeds physical RAM.  Zero made that true for every
// buffer over 1000 lines, so changing the size at all popped "you have only
// 0 Mb of physical RAM" -- on a machine with gigabytes.
//
// The bound is deliberately loose at both ends rather than an equality: this
// has to hold on any machine the suite runs on, and the defect it guards
// against is a value of ZERO, not a value that is slightly off.  The upper
// bound keeps it from passing on a garbage read (an uninitialised or
// byte-swapped sysctl result would blow past it).
TEST(MiscShims, GlobalMemoryStatusReportsRealPhysicalMemory) {
    MEMORYSTATUS ms;
    ms.dwTotalPhys = 12345;          // poison: the call must overwrite it
    GlobalMemoryStatus(&ms);

    const unsigned long long mb = (unsigned long long) ms.dwTotalPhys / (1024ULL * 1024ULL);
    EXPECT_GT(mb, 256ULL);               // not zero, and not implausibly small
    EXPECT_LT(mb, 8ULL * 1024 * 1024);   // < 8 TB: a sane machine, not garbage

    // The consumer's actual arithmetic, at the shipped default buffer size:
    // this must NOT trip the warning.
    const unsigned long long needed = 16ULL * 1024 * 1024 + 5000ULL * 60;
    EXPECT_LT(needed, (unsigned long long) ms.dwTotalPhys);
}

// ── round 5: document open/save shims, stdio strings, shell, clipboard ────

// B-3 atomic flip: real headers (portable stub deleted; tests are FULL_BUILD).
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "platform/wx_mfc_hooks.h"

// OnSaveDocument/OnOpenDocument: the CDocument shim opens the file, wraps
// it in a CArchive, and round-trips through Serialize (mushcore's doc is
// not CDocument-derived, so a small serialising document stands in).
namespace {
class SerialDoc : public CDocument {
public:
    CString payload;      // stored on save, replaced on load
    bool    boom = false; // throw from Serialize to hit the catch arms
    void Serialize(CArchive& ar) override {
        if (boom) ThrowErrorException("serialize boom");
        if (ar.IsStoring()) {
            ar.WriteString(payload);
        } else {
            CString line;
            payload.Empty();
            while (ar.ReadString(line)) payload += line;
        }
    }
};
} // namespace

TEST(DocFileR5, SaveAndReopenRoundTrip)
{
    char path[128];
    snprintf(path, sizeof path, "/tmp/mc_r5_docfile_%d.txt", (int)getpid());

    {
        SerialDoc doc;
        doc.payload = "hello shim";
        EXPECT_TRUE(doc.OnSaveDocument(path));
    }
    {
        SerialDoc doc;
        EXPECT_TRUE(doc.OnOpenDocument(path));
        EXPECT_STREQ("hello shim", (LPCTSTR)doc.payload);
    }
    {   // failure arms: unopenable paths
        SerialDoc doc;
        EXPECT_FALSE(doc.OnOpenDocument("/no/such/dir/x.txt"));
        EXPECT_FALSE(doc.OnSaveDocument("/no/such/dir/x.txt"));
    }
    {   // Serialize throwing exercises the CException catch arms
        SerialDoc doc;
        doc.boom = true;
        EXPECT_FALSE(doc.OnSaveDocument(path));
        EXPECT_FALSE(doc.OnOpenDocument(path));
    }
    unlink(path);
}

TEST(StdioFileR5, ReadStringBufferVariant)
{
    char path[128];
    snprintf(path, sizeof path, "/tmp/mc_r5_stdio_%d.txt", (int)getpid());
    FILE* f = fopen(path, "wb");
    ASSERT_NE(nullptr, f);
    fputs("first line\nsecond\n", f);
    fclose(f);

    f = fopen(path, "rb");
    ASSERT_NE(nullptr, f);
    CStdioFile sf(f);
    char buf[64];
    EXPECT_TRUE(sf.ReadString(buf, sizeof buf));
    EXPECT_EQ(0, strncmp(buf, "first line", 10));
    CString s;
    EXPECT_TRUE(sf.ReadString(s));
    EXPECT_STREQ("second", (LPCTSTR)s);
    EXPECT_FALSE(sf.ReadString(buf, sizeof buf));   // EOF
    sf.Close();
    unlink(path);

    CStdioFile closed;
    EXPECT_FALSE(closed.ReadString(buf, sizeof buf));
}

// ShellExecute's editor-launch form runs the quoted command; exercised with
// a harmless no-op binary.  The quote-escaping arm is exercised with a
// nonexistent name containing a quote (the command fails, nothing runs).
TEST(ShellR5, ShellExecuteEditorFormAndQuoting)
{
    EXPECT_EQ(nullptr, ShellExecute(nullptr, "open", "", nullptr, nullptr, 0));

    HINSTANCE h = ShellExecute(nullptr, "open", "/usr/bin/true", "--noop",
                               nullptr, 0);
    EXPECT_NE(nullptr, h);   // command ran (exit 0)

    // embedded quote goes through the escape arm; the file does not exist,
    // so the shell reports failure and nothing is executed
    ShellExecute(nullptr, "open", "/nonexistent/it's-not-here", "--x",
                 nullptr, 0);
}

TEST(ClipboardR5, FormatAvailabilityViaHook)
{
    char* (*saved)() = g_wxhook_get_clipboard_text;

    g_wxhook_get_clipboard_text = []() -> char* { return strdup("clip"); };
    EXPECT_TRUE(IsClipboardFormatAvailable(1));    // CF_TEXT
    EXPECT_TRUE(IsClipboardFormatAvailable(13));   // CF_UNICODETEXT
    EXPECT_FALSE(IsClipboardFormatAvailable(2));   // unbridged format

    g_wxhook_get_clipboard_text = []() -> char* { return nullptr; };
    EXPECT_FALSE(IsClipboardFormatAvailable(1));   // no text on clipboard

    g_wxhook_get_clipboard_text = saved;
    EXPECT_FALSE(IsClipboardFormatAvailable(1));   // no hook at all
}


// ── ShellExecute argument handling ────────────────────
// mfc_ui.h BuildShellExecuteCommand: "`op` and `dir` used to be accepted and
// thrown away here, which made utils.shellexecute(file, params, defdir,
// operation) quietly ignore two of its four arguments."  The verb and the
// working directory are part of a documented scripting API
// (scripting/lua_utils.cpp shell_execute).

TEST(ShellExecuteCmd, DefaultVerbOpensTheFile)
{
    std::string c = BuildShellExecuteCommand(nullptr, "/tmp/a.txt", nullptr, nullptr);
    EXPECT_TRUE(c.find("'/tmp/a.txt'") != std::string::npos);
    EXPECT_TRUE(c.find("cd ") == std::string::npos);   // no defdir -> no cd
    // backgrounded so a launch cannot block the MUD connection
    EXPECT_TRUE(c.size() > 2 && c.compare(c.size() - 2, 2, " &") == 0);
}

TEST(ShellExecuteCmd, VerbSelectsADifferentOpenerAndIsCaseInsensitive)
{
    std::string open_  = BuildShellExecuteCommand("open",  "/tmp/a.txt", nullptr, nullptr);
    std::string print_ = BuildShellExecuteCommand("print", "/tmp/a.txt", nullptr, nullptr);
    std::string edit_  = BuildShellExecuteCommand("edit",  "/tmp/a.txt", nullptr, nullptr);
    // The whole point: a different verb must not produce the same command.
    EXPECT_TRUE(print_ != open_);
    EXPECT_TRUE(print_.find("lpr") == 0);
    EXPECT_TRUE(edit_ != print_);
    // Windows verbs are case-insensitive.
    EXPECT_EQ(print_, BuildShellExecuteCommand("PRINT", "/tmp/a.txt", nullptr, nullptr));
}

TEST(ShellExecuteCmd, UnmappedVerbIsRefusedRatherThanSilentlyOpening)
{
    // An unmapped verb must NOT fall back to "open" -- that is the silent
    // wrong-behaviour this change exists to remove.  Empty => the caller
    // returns SE_ERR_NOASSOC and the script gets nil + an error message.
    EXPECT_TRUE(BuildShellExecuteCommand("runas", "/tmp/a.txt", nullptr, nullptr).empty());
    EXPECT_TRUE(BuildShellExecuteCommand("frobnicate", "/tmp/a.txt", nullptr, nullptr).empty());
}

TEST(ShellExecuteCmd, WorkingDirectoryIsHonoured)
{
    std::string c = BuildShellExecuteCommand("open", "a.txt", nullptr, "/tmp/some dir");
    EXPECT_TRUE(c.find("cd '/tmp/some dir' && ") == 0);
}

TEST(ShellExecuteCmd, EditorLaunchFormRunsTheExecutableWithItsArguments)
{
    // doc.cpp's script-editor launch: `file` is the editor, `params` its
    // already-quoted argument string.  Must not be routed through "open".
    std::string c = BuildShellExecuteCommand("open", "/usr/bin/vi", "'/tmp/s.lua'", nullptr);
    EXPECT_TRUE(c.find("'/usr/bin/vi' '/tmp/s.lua'") == 0);
    EXPECT_TRUE(c.find("open") == std::string::npos);
}

TEST(ShellExecuteCmd, SingleQuotesInPathsAreEscaped)
{
    // A file name containing a quote must not break out of the quoting.
    std::string c = BuildShellExecuteCommand("open", "/tmp/it's here.txt", nullptr, nullptr);
    EXPECT_TRUE(c.find("'/tmp/it'\\''s here.txt'") != std::string::npos);
}

TEST(ShellExecuteCmd, EmptyFileYieldsNoCommand)
{
    EXPECT_TRUE(BuildShellExecuteCommand("open", nullptr, nullptr, nullptr).empty());
    EXPECT_TRUE(BuildShellExecuteCommand("open", "",      nullptr, nullptr).empty());
}


// mfc_ui.h CMenu::GetSubMenu: "This used to `return nullptr` unconditionally,
// which CRASHED the app."  The canonical idiom is LoadMenu -> GetSubMenu(0) ->
// ASSERT(pPopup) -> pPopup->GetSubMenu(n)->AppendMenu(...), and the port has no
// menu-resource loader, so the whole chain used to walk into a null CMenu*.
// Live repro: Configure > Triggers > Add > "..." > "->" killed the process.

TEST(MenuSubMenus, CanonicalLoadMenuChainDoesNotYieldNull)
{
    CMenu menu;
    EXPECT_FALSE(menu.LoadMenu(1u));        // still no resource loader...
    CMenu* pPopup = menu.GetSubMenu(0);     // ...but the chain must survive it
    ASSERT_TRUE(pPopup != nullptr);
    // dialogs/EditDlg.cpp walks six submenus and appends into each.
    for (int i = 0; i < 6; ++i) {
        CMenu* sub = pPopup->GetSubMenu(i);
        ASSERT_TRUE(sub != nullptr);
        sub->AppendMenu(MF_STRING | MF_ENABLED, 5000 + i, "item");
    }
    EXPECT_EQ(6u, (unsigned)pPopup->m_subs.size());
}

TEST(MenuSubMenus, RepeatedGetIsStableAndItemsLandInTheRightSubmenu)
{
    CMenu menu;
    CMenu* a1  = menu.GetSubMenu(1);
    CMenu* a1b = menu.GetSubMenu(1);
    EXPECT_TRUE(a1 == a1b);              // same object, not a fresh one
    // Asking for index 1 first must have created index 0 too, so the MF_POPUP
    // entries stay parallel to m_subs.
    EXPECT_EQ(2u, (unsigned)menu.m_subs.size());
    EXPECT_EQ(2u, (unsigned)menu.m_items.size());
    EXPECT_TRUE((menu.m_items[0].flags & MF_POPUP) != 0);
    EXPECT_TRUE(menu.m_items[0].text.empty());   // no resource = no title

    menu.GetSubMenu(0)->AppendMenu(MF_STRING, 11, "zero");
    a1->AppendMenu(MF_STRING, 22, "one");
    EXPECT_EQ(1u, (unsigned)menu.GetSubMenu(0)->m_items.size());
    EXPECT_EQ(1u, (unsigned)a1->m_items.size());
    EXPECT_STREQ("zero", menu.GetSubMenu(0)->m_items[0].text.c_str());
    EXPECT_STREQ("one",  a1->m_items[0].text.c_str());
}

TEST(MenuSubMenus, NegativeIndexStillYieldsNull)
{
    CMenu menu;
    EXPECT_EQ((CMenu*)nullptr, menu.GetSubMenu(-1));
}


// mfc_ui.h CDocument::GetDocTemplate used to `return nullptr`, and
// CWinApp::DoPromptFileName used to `return FALSE`.  Together they made
// CMUSHclientDoc::DoSave's empty-path arm (doc.cpp:2181-2211) abort in a
// debug build (ASSERT(pTemplate != NULL)) and, in the shipped Release .app,
// SILENTLY NOT SAVE -- `if (!bResult) return FALSE;`.  Reachable from
// world.Save() with no argument and from auto-save-on-close at quit for a
// never-saved File > New World.

TEST(DocTemplate, AddDocumentRecordsTheOwningTemplate)
{
    CMultiDocTemplate tmpl(0, nullptr, nullptr, nullptr);
    CDocument doc;
    EXPECT_EQ((CDocTemplate*)nullptr, doc.GetDocTemplate());  // before
    tmpl.AddDocument(&doc);
    // The ASSERT in DoSave's empty-path arm needs this to be non-null.
    EXPECT_TRUE(doc.GetDocTemplate() != nullptr);
    EXPECT_TRUE(doc.GetDocTemplate() == (CDocTemplate*)&tmpl);
    tmpl.RemoveDocument(&doc);
}

TEST(DocPrompt, DoPromptFileNameAsksAndReportsTheChosenPath)
{
    // Drive the file-dialog hook the same way the rest of this file drives
    // the wx seam: install a fake, assert on what the shim asked for.
    auto saved = g_wxhook_file_dialog;
    static std::string s_seenLeaf, s_seenFilter;
    static bool s_seenOpen = true;
    g_wxhook_file_dialog = [](bool bOpen, const char* /*title*/,
                              const char* /*dir*/, const char* leaf,
                              const char* filter, bool /*overwrite*/,
                              bool) -> char* {
        s_seenOpen   = bOpen;
        s_seenLeaf   = leaf   ? leaf   : "";
        s_seenFilter = filter ? filter : "";
        return strdup("/tmp/chosen.mcl");
    };
    CString name = "/worlds/Suggested.mcl";
    BOOL ok = App.DoPromptFileName(name, 0, 0, FALSE /*save*/, nullptr);
    EXPECT_TRUE(ok);
    EXPECT_STREQ("/tmp/chosen.mcl", (LPCTSTR)name);   // caller gets the choice
    EXPECT_FALSE(s_seenOpen);                         // a SAVE dialog
    EXPECT_STREQ("Suggested.mcl", s_seenLeaf.c_str());// seeded with the suggestion
    EXPECT_TRUE(s_seenFilter.find(".mcl") != std::string::npos);
    g_wxhook_file_dialog = saved;
}

TEST(DocPrompt, CancellingLeavesTheNameAloneAndReportsFailure)
{
    auto saved = g_wxhook_file_dialog;
    g_wxhook_file_dialog = [](bool, const char*, const char*, const char*,
                              const char*, bool, bool) -> char* { return nullptr; };
    CString name = "/worlds/Suggested.mcl";
    EXPECT_FALSE(App.DoPromptFileName(name, 0, 0, FALSE, nullptr));
    EXPECT_STREQ("/worlds/Suggested.mcl", (LPCTSTR)name);  // untouched
    g_wxhook_file_dialog = saved;
}


// mfc_ui.h CWinApp::OpenDocumentFile used to `return nullptr`, silently
// killing "File > Open Worlds In Startup List" and "Connection > Connect To
// Worlds In Startup List" (mainfrm.cpp:1844/1848 null-check it, so no crash --
// just two dead menu commands).  It is a NON-VIRTUAL CWinApp member, so it
// never reached CMultiDocTemplate's working override.  Dispatch is narrow on
// purpose: the other callers want a CTextDocument, so sending everything to
// the world hook would try to open a plugin's XML as a world.

TEST(AppOpenDocument, WorldFileRoutesToTheWorldHook)
{
    auto saved = g_wxhook_open_world_file;
    static std::string s_asked;
    static CDocument s_doc;
    g_wxhook_open_world_file = [](const char* path) -> void* {
        s_asked = path ? path : "";
        return (void*)&s_doc;
    };
    s_asked.clear();
    EXPECT_TRUE(App.OpenDocumentFile("/w/Discworld.mcl") == (CDocument*)&s_doc);
    EXPECT_STREQ("/w/Discworld.mcl", s_asked.c_str());
    // extension match is case-insensitive, as on Windows
    s_asked.clear();
    EXPECT_TRUE(App.OpenDocumentFile("/w/Other.MCL") != nullptr);
    EXPECT_STREQ("/w/Other.MCL", s_asked.c_str());
    g_wxhook_open_world_file = saved;
}

TEST(AppOpenDocument, NonWorldFileGoesToTheNotepadNotTheWorldOpener)
{
    auto savedW = g_wxhook_open_world_file;
    auto savedT = g_wxhook_open_text_file;
    static bool s_worldCalled = false;
    static std::string s_textAsked;
    static CDocument s_textDoc;
    g_wxhook_open_world_file = [](const char*) -> void* {
        s_worldCalled = true; return nullptr;
    };
    g_wxhook_open_text_file = [](const char* p) -> void* {
        s_textAsked = p ? p : ""; return (void*)&s_textDoc;
    };

    // A plugin/script file must NOT be handed to the world opener -- those
    // callers cast the result to CTextDocument*.  It goes to the notepad.
    s_worldCalled = false; s_textAsked.clear();
    EXPECT_TRUE(App.OpenDocumentFile("/p/MyPlugin.xml") == (CDocument*)&s_textDoc);
    EXPECT_STREQ("/p/MyPlugin.xml", s_textAsked.c_str());
    EXPECT_FALSE(s_worldCalled);

    s_textAsked.clear();
    EXPECT_TRUE(App.OpenDocumentFile("/p/script.lua") != nullptr);
    EXPECT_STREQ("/p/script.lua", s_textAsked.c_str());
    s_textAsked.clear();
    EXPECT_TRUE(App.OpenDocumentFile("/p/noextension") != nullptr);
    EXPECT_FALSE(s_worldCalled);

    // An empty/null path reaches neither hook.
    s_textAsked.clear();
    EXPECT_EQ((CDocument*)nullptr, App.OpenDocumentFile(nullptr));
    EXPECT_EQ((CDocument*)nullptr, App.OpenDocumentFile(""));
    EXPECT_TRUE(s_textAsked.empty());
    EXPECT_FALSE(s_worldCalled);

    g_wxhook_open_world_file = savedW;
    g_wxhook_open_text_file  = savedT;
}

// ── CMultiDocTemplate must keep its resource id ─────────────────────────────
// App creates three templates; the shim discarded the id, so they were
// indistinguishable and OpenDocumentFile(NULL) always meant "new world".
// Ticking "Open activity window at startup" therefore popped Quick Connect
// during startup and, on OK, made a junk untitled world.
TEST(DocTemplate, ActivityTemplateOpensTheActivityWindowNotANewWorld)
{
    auto savedNew  = g_wxhook_new_world;
    auto savedAct  = g_wxhook_show_activity_window;
    static int s_newWorlds, s_activities;
    s_newWorlds = 0; s_activities = 0;
    g_wxhook_new_world = []() -> void* { ++s_newWorlds; return nullptr; };
    g_wxhook_show_activity_window = []() { ++s_activities; };

    // How both MUSHclient.cpp (the startup preference) and mainfrm.cpp (the
    // Activity List command) ask for the activity window.
    CMultiDocTemplate activity(131 /*IDR_ACTIVITYTYPE*/, nullptr, nullptr, nullptr);
    EXPECT_EQ((CDocument*)nullptr, activity.OpenDocumentFile(nullptr));
    EXPECT_EQ(1, s_activities);
    EXPECT_EQ(0, s_newWorlds);

    // A path does not turn it into a world open either.
    activity.OpenDocumentFile("/w/Discworld.mcl");
    EXPECT_EQ(2, s_activities);
    EXPECT_EQ(0, s_newWorlds);

    // The world template is unaffected: NULL still means "new world".
    CMultiDocTemplate world(128 /*IDR_MUSHCLTYPE*/, nullptr, nullptr, nullptr);
    world.OpenDocumentFile(nullptr);
    EXPECT_EQ(1, s_newWorlds);
    EXPECT_EQ(2, s_activities);

    g_wxhook_new_world = savedNew;
    g_wxhook_show_activity_window = savedAct;
}

TEST(DocTemplate, TheActivityResourceIdLiteralStillMatchesResourceH)
{
    // platform/mfc_ui.h cannot include resource.h, so it hardcodes 131.  If
    // resource.h ever renumbers IDR_ACTIVITYTYPE, this fails instead of the
    // activity window quietly turning back into Quick Connect.
    EXPECT_EQ(131, IDR_ACTIVITYTYPE);
}

// ── CMainFrame::PostMessage must not shadow CWnd::PostMessage ───────────────
// platform/mainfrm_stub.h is included by mainfrm.h for EVERY non-Windows build,
// mushgui included, so an in-class PostMessage there name-hid the real
// CWnd::PostMessage in the shipping GUI.  Both engine call sites went nowhere:
// world.DoCommand (methods_commands.cpp, ~200 commands) and the TLS-failure
// consent prompt (doc_shared.cpp).  This is the P6 signature/shadowing trap --
// it compiles, links and reads as wired.  These tests pin the dispatch.
TEST(FramePostMessage, ReachesTheHookRatherThanAStub)
{
    auto saved = g_wxhook_post_message;
    static unsigned s_msg; static uintptr_t s_wp; static intptr_t s_lp;
    static int s_calls;
    s_msg = 0; s_wp = 0; s_lp = 0; s_calls = 0;
    g_wxhook_post_message = [](void*, unsigned m, uintptr_t w, intptr_t l) -> int {
        s_msg = m; s_wp = w; s_lp = l; ++s_calls; return TRUE;
    };

    // What world.DoCommand posts: WM_COMMAND carrying the command id.
    EXPECT_TRUE(Frame.PostMessage(WM_COMMAND, 32332, 0));
    EXPECT_EQ(1, s_calls);
    EXPECT_EQ((unsigned) WM_COMMAND, s_msg);
    EXPECT_EQ((uintptr_t) 32332, s_wp);

    // What OnTlsFailed posts: the doc pointer as wParam, cert-vs-handshake as lParam.
    EXPECT_TRUE(Frame.PostMessage(WM_USER_SSL_FALLBACK_PROMPT, (WPARAM) 0x1234, (LPARAM) 1));
    EXPECT_EQ(2, s_calls);
    EXPECT_EQ((unsigned) WM_USER_SSL_FALLBACK_PROMPT, s_msg);
    EXPECT_EQ((uintptr_t) 0x1234, s_wp);
    EXPECT_EQ((intptr_t) 1, s_lp);

    g_wxhook_post_message = saved;
}

TEST(FramePostMessage, HeadlessStillFailsClosedWhenNoPumpIsInstalled)
{
    // The stub existed to make headless posts inert.  Inheriting CWnd::
    // PostMessage preserves that by construction: a null hook means no pump,
    // so the post reports failure and the TLS prompt never fires -- which is
    // what "TLS failures fail closed" means for the headless build.
    auto saved = g_wxhook_post_message;
    g_wxhook_post_message = nullptr;
    EXPECT_FALSE(Frame.PostMessage(WM_COMMAND, 32332, 0));
    EXPECT_FALSE(Frame.PostMessage(WM_USER_SSL_FALLBACK_PROMPT, 0, 0));
    g_wxhook_post_message = saved;
}

// ── CMyToolBar::GetButtonInfo must assign its outputs ────────────────
//
// mainfrm.cpp's OnUpdateBtnWorlds declares nID/nStyle/iImage, hands them to
// GetButtonInfo and then compares iImage against a freshly computed image
// index -- so the stub returning FALSE without touching them left a live path
// reading uninitialised locals (and handing them straight back to SetButtonInfo).
TEST(MainFrameStub, GetButtonInfoAssignsOutputsEvenWithNoToolbar)
{
    UINT nID = 0xDEAD, nStyle = 0xDEAD;
    int  iImage = -12345;
    // the very toolbar OnUpdateBtnWorlds asks
    EXPECT_FALSE(Frame.m_wndActivityToolBar.GetButtonInfo(0, nID, nStyle, iImage));
    EXPECT_EQ((UINT) 0, nID);
    EXPECT_EQ((UINT) 0, nStyle);
    EXPECT_EQ(0, iImage);
}

// ── DDV_MinMax* actually validate ─────────────────────────────────────────
//
// These were no-ops, so every numeric range in every property page was
// advisory: a value outside it was accepted, written to the doc member and
// SAVED to the world file, with nothing downstream re-checking.  MFC shows a
// message and calls pDX->Fail(), which throws and aborts the exchange -- so the
// dialog stays open and the member keeps its old value.
TEST(Ddv, MinMaxIntRejectsOutOfRangeAndAcceptsInRange) {
    CDataExchange dx;
    dx.m_bSaveAndValidate = TRUE;

    // In range: no throw.
    DDV_MinMaxInt(&dx, 5, 0, 10);
    DDV_MinMaxInt(&dx, 0, 0, 10);      // inclusive at both ends
    DDV_MinMaxInt(&dx, 10, 0, 10);

    bool threw = false;
    try { DDV_MinMaxInt(&dx, 11, 0, 10); } catch (const CDdxValidationFail&) { threw = true; }
    EXPECT_TRUE(threw);

    threw = false;
    try { DDV_MinMaxInt(&dx, -1, 0, 10); } catch (const CDdxValidationFail&) { threw = true; }
    EXPECT_TRUE(threw);
}

// The LOAD direction (members -> controls) must never validate: MFC only checks
// when m_bSaveAndValidate is TRUE, so a doc that already holds an out-of-range
// value can still open its property page.  Validating here would make such a
// world impossible to edit at all.
TEST(Ddv, MinMaxDoesNotValidateOnTheLoadDirection) {
    CDataExchange dx;
    dx.m_bSaveAndValidate = FALSE;

    bool threw = false;
    try {
        DDV_MinMaxInt(&dx, 9999, 0, 10);
        DDV_MinMaxLong(&dx, 9999, 0, 10);
        DDV_MinMaxUInt(&dx, 9999u, 0u, 10u);
        DDV_MinMaxDouble(&dx, 9999.0, 0.0, 10.0);
        DDV_MinMaxFloat(&dx, 9999.0f, 0.0f, 10.0f);
    } catch (const CDdxValidationFail&) { threw = true; }
    EXPECT_FALSE(threw);
}

TEST(Ddv, EveryMinMaxOverloadValidates) {
    CDataExchange dx;
    dx.m_bSaveAndValidate = TRUE;

    int n = 0;
    try { DDV_MinMaxLong(&dx, 11L, 0L, 10L); }        catch (const CDdxValidationFail&) { n++; }
    try { DDV_MinMaxUInt(&dx, 11u, 0u, 10u); }        catch (const CDdxValidationFail&) { n++; }
    try { DDV_MinMaxDouble(&dx, 10.5, 0.0, 10.0); }   catch (const CDdxValidationFail&) { n++; }
    try { DDV_MinMaxFloat(&dx, 10.5f, 0.0f, 10.0f); } catch (const CDdxValidationFail&) { n++; }
    EXPECT_EQ(4, n);
}

// ── CInsertUnicodeDlg: the code-value guard is not vacuous on LP64 ─────────
//
// The dialog checked `i > LONG_MAX`, which on Windows (LONG is 32 bits) is
// exactly the int range _pcre_ord2utf accepts, but on this LP64 platform LONG
// is 64 bits, so the check let a 9-10 digit hex value between 0x80000000 and
// LONG_MAX through -- then truncated it to the `int` parameter at the sole
// call site, producing garbage UTF-8 instead of the rejection Windows gives.
//
// No fakewx hooks are installed, so DDX_Text/DDX_Check are no-ops (guarded,
// per the NullDlgItemsLeaveEveryMemberAlone contract above) and the members
// set directly below are what DoDataExchange's own validation code sees.
TEST(MfcUi, InsertUnicodeRejectsACodeAboveIntMaxOnSave) {
    CInsertUnicodeDlg dlg;
    dlg.m_bHex = TRUE;
    dlg.m_strCharacter = "80000001";   // 0x80000001 -- above INT32_MAX, below LONG_MAX on LP64
    EXPECT_FALSE(dlg.UpdateData(TRUE));   // pDX->Fail() -> FALSE, no throw escape
}

TEST(MfcUi, InsertUnicodeAcceptsTheTopOfTheValidRange) {
    CInsertUnicodeDlg dlg;
    dlg.m_bHex = TRUE;
    dlg.m_strCharacter = "7FFFFFFF";   // INT32_MAX -- still valid on both platforms
    EXPECT_TRUE(dlg.UpdateData(TRUE));
    EXPECT_EQ((__int64) 0x7FFFFFFF, dlg.m_iCode);
}

// The other half of the chain -- that a Fail() makes UpdateData return FALSE and
// vetoes OnOK, which is what keeps the dialog open -- is already pinned by
// Ddx.ValidationFailVetoesUpdateAndOnOK above.  Together with the throws
// asserted here, an out-of-range entry now cannot reach the doc member.

// ═══════════════════════════════════════════════════════════════════════════
// Closing a world: which questions get asked
// ═══════════════════════════════════════════════════════════════════════════
//
// platform/world_close_decision.h, lifted out of CWorldChildFrame::OnClose.
// It lives in this executable for the same reason the rest of this file does:
// it is the port's own frontend logic, pure, and free of wx types -- so it can
// be checked here rather than in the app, where every selftest exits before the
// event loop and no modal could ever complete.
//
// The assertions are properties, not a re-implementation of the function: a
// table that simply repeats the code would agree with any mutation of it.

TEST(WorldClose, AForcedCloseAsksNothingAtAll)
{
    // Cmd+Q, app shutdown, Close(true): the event cannot be vetoed, so a
    // question nobody can answer would only wedge the teardown.  Every
    // combination of the other five flags, because this must hold for all of
    // them -- it is the property the whole teardown path depends on.
    for (int bits = 0; bits < 32; ++bits) {
        WorldClosePlan p = DecideWorldClose(/*canVeto*/ false,
                                            (bits &  1) != 0, (bits &  2) != 0,
                                            (bits &  4) != 0, (bits &  8) != 0,
                                            (bits & 16) != 0);
        EXPECT_FALSE(p.askEndSession);
        EXPECT_FALSE(p.askSaveVariables);
        EXPECT_FALSE(p.askSaveModified);
    }
}

// ── The matrix ─────────────────────────────────────────────────────────────
// {connected, disconnected} x {modified, unmodified} x {confirm-before-closing
// on, off}, with the questions written out per cell as literal text rather than
// derived, so the table pins behaviour instead of restating the formula -- and
// so a failure names the cell.
//
// Three of the eight cells changed when the connection dimension arrived, and
// each is marked.  Before, Cmd+W could not see the connection at all and spent
// m_bConfirmBeforeClosingWorld on a general "Close <world>?" prompt, which is
// the one question CMUSHclientDoc::SaveModified never asks in any state.
TEST(WorldClose, TheFullConnectionByModifiedByPreferenceMatrix)
{
    auto cell = [](bool connected, bool modified, bool confirmClosing) {
        // Variables are held steady (none changed) so the cells speak about the
        // other two questions only; the variables arm has its own tests below.
        const WorldClosePlan p =
            DecideWorldClose(/*canVeto*/ true, connected, modified,
                             /*variablesChanged*/ false,
                             /*confirmSavingVariables*/ true, confirmClosing);
        char buf[160];
        snprintf(buf, sizeof buf,
                 "connected=%d modified=%d confirmClosing=%d -> "
                 "endSession=%d saveModified=%d saveVariables=%d",
                 (int) connected, (int) modified, (int) confirmClosing,
                 (int) p.askEndSession, (int) p.askSaveModified,
                 (int) p.askSaveVariables);
        return std::string(buf);
    };

    // CHANGED: was the save prompt alone; now warned about the session too.
    EXPECT_EQ(std::string("connected=1 modified=1 confirmClosing=1 -> "
                          "endSession=1 saveModified=1 saveVariables=0"),
              cell(true, true, true));
    // unchanged: the preference is off, so only the save prompt.
    EXPECT_EQ(std::string("connected=1 modified=1 confirmClosing=0 -> "
                          "endSession=0 saveModified=1 saveVariables=0"),
              cell(true, true, false));
    // CHANGED: was the port's invented "Close <world>?"; now Windows' warning.
    EXPECT_EQ(std::string("connected=1 modified=0 confirmClosing=1 -> "
                          "endSession=1 saveModified=0 saveVariables=0"),
              cell(true, false, true));
    // unchanged: nothing to save and no warning asked for.
    EXPECT_EQ(std::string("connected=1 modified=0 confirmClosing=0 -> "
                          "endSession=0 saveModified=0 saveVariables=0"),
              cell(true, false, false));
    // unchanged: no session to end, so just the save prompt.
    EXPECT_EQ(std::string("connected=0 modified=1 confirmClosing=1 -> "
                          "endSession=0 saveModified=1 saveVariables=0"),
              cell(false, true, true));
    // unchanged.
    EXPECT_EQ(std::string("connected=0 modified=1 confirmClosing=0 -> "
                          "endSession=0 saveModified=1 saveVariables=0"),
              cell(false, true, false));
    // CHANGED: an idle, saved, disconnected world used to demand an answer
    // before it would close -- on a stock installation, every single time.
    EXPECT_EQ(std::string("connected=0 modified=0 confirmClosing=1 -> "
                          "endSession=0 saveModified=0 saveVariables=0"),
              cell(false, false, true));
    // unchanged: closes without a word.
    EXPECT_EQ(std::string("connected=0 modified=0 confirmClosing=0 -> "
                          "endSession=0 saveModified=0 saveVariables=0"),
              cell(false, false, false));
}

TEST(WorldClose, OnlyALiveSessionIsWarnedAbout)
{
    // doc.cpp:2103 gates the warning on the connection, and Cmd+W could not see
    // one: closing a connected world dropped the session without a word, while
    // Cmd+Q (which goes through SaveModified) always asked.
    //
    // It depends on the connection and the preference and NOTHING else, so
    // sweep the other three flags -- a mutation folding `modified` or the
    // variables state into this arm has to survive all eight.
    for (int bits = 0; bits < 8; ++bits) {
        const bool modified   = (bits & 1) != 0;
        const bool varsChgd   = (bits & 2) != 0;
        const bool confirmVar = (bits & 4) != 0;

        EXPECT_TRUE(DecideWorldClose(true, /*connected*/ true, modified,
                                     varsChgd, confirmVar,
                                     /*confirmClosing*/ true).askEndSession);
        // The preference gates it...
        EXPECT_FALSE(DecideWorldClose(true, /*connected*/ true, modified,
                                      varsChgd, confirmVar,
                                      /*confirmClosing*/ false).askEndSession);
        // ...and so does there actually being a session to lose.
        EXPECT_FALSE(DecideWorldClose(true, /*connected*/ false, modified,
                                      varsChgd, confirmVar,
                                      /*confirmClosing*/ true).askEndSession);
    }
}

TEST(WorldClose, AModifiedWorldIsAskedAboutSavingOnceAndOnlyOnce)
{
    // The unsaved-changes question is asked exactly when there are unsaved
    // changes, and the variables question -- which exists only to cover the
    // case where there are NOT -- may never join it, whatever the preferences
    // say and whether or not script variables changed as well.
    for (int bits = 0; bits < 16; ++bits) {
        WorldClosePlan p = DecideWorldClose(/*canVeto*/ true,
                                            (bits & 1) != 0, /*modified*/ true,
                                            (bits & 2) != 0, (bits & 4) != 0,
                                            (bits & 8) != 0);
        EXPECT_TRUE(p.askSaveModified);
        EXPECT_FALSE(p.askSaveVariables);
    }
}

TEST(WorldClose, AnIdleDisconnectedWorldClosesWithoutBeingQuestioned)
{
    // The prompt the port invented, now gone.  m_bConfirmBeforeClosingWorld is
    // ON by default, so a stock installation had to answer "Close <world>?"
    // every time it dismissed a world that was saved and not connected.
    WorldClosePlan p = DecideWorldClose(/*canVeto*/ true, /*connected*/ false,
                                        /*modified*/ false,
                                        /*variablesChanged*/ false,
                                        /*confirmSavingVariables*/ true,
                                        /*confirmClosingWorld*/ true);
    EXPECT_FALSE(p.askEndSession);
    EXPECT_FALSE(p.askSaveModified);
    EXPECT_FALSE(p.askSaveVariables);
}

TEST(WorldClose, VariablesOnlyChangesGetTheirOwnQuestion)
{
    // Changing a script variable does not mark the document modified, so
    // without this question the world would close and lose them silently.
    WorldClosePlan p = DecideWorldClose(true, false, false, true, true, false);
    EXPECT_TRUE(p.askSaveVariables);
    EXPECT_FALSE(p.askSaveModified);
    EXPECT_FALSE(p.askEndSession);

    // The global preference gates it...
    p = DecideWorldClose(true, false, false, true, false, false);
    EXPECT_FALSE(p.askSaveVariables);
    // ...and so does something actually having changed.
    p = DecideWorldClose(true, false, false, false, true, false);
    EXPECT_FALSE(p.askSaveVariables);
}

TEST(WorldClose, TheTwoPreferencesBuyDifferentQuestions)
{
    // They used to overlap: confirm-before-closing produced a second prompt
    // after the variables one, so a stock installation (both default ON) was
    // asked twice about a world it had nothing to warn about.  Now each
    // preference gates exactly one question and neither touches the other's.
    WorldClosePlan p = DecideWorldClose(true, /*connected*/ false,
                                        /*modified*/ false,
                                        /*variablesChanged*/ true,
                                        /*confirmSavingVariables*/ true,
                                        /*confirmClosingWorld*/ true);
    EXPECT_TRUE(p.askSaveVariables);
    EXPECT_FALSE(p.askEndSession);      // disconnected: the pref buys nothing

    // Connected, the same world is warned AND asked about its variables --
    // both of SaveModified's first two questions, in that order.
    p = DecideWorldClose(true, /*connected*/ true, false, true, true, true);
    EXPECT_TRUE(p.askEndSession);
    EXPECT_TRUE(p.askSaveVariables);

    // The variables preference has no say in the session warning...
    p = DecideWorldClose(true, /*connected*/ true, false, true,
                         /*confirmSavingVariables*/ false, true);
    EXPECT_TRUE(p.askEndSession);
    EXPECT_FALSE(p.askSaveVariables);
    // ...and the closing preference none in the variables question.
    p = DecideWorldClose(true, /*connected*/ true, false, true, true,
                         /*confirmClosingWorld*/ false);
    EXPECT_FALSE(p.askEndSession);
    EXPECT_TRUE(p.askSaveVariables);
}

// ═══════════════════════════════════════════════════════════════════════════
// Quitting with several worlds open: ask them all, THEN close them
// ═══════════════════════════════════════════════════════════════════════════
//
// QueryThenCloseAllWorlds (platform/world_close_decision.h) is what the macOS
// quit hook walks the world list with.  It used to be a single loop calling
// CMUSHclientDoc::SaveModified, which asks the questions AND ends the world, so
// cancelling at the third world left the first two with their on_world_close
// script already run, MXP shut down and m_bWorldClosing stuck true for the rest
// of the session -- which silently suppressed those worlds'
// ON_PLUGIN_GETFOCUS / ON_PLUGIN_LOSEFOCUS for as long as the app stayed up.
//
// The fake records the same three effects the real CommitCloseWorld has, plus a
// shared transcript, so the ORDER of the two passes is observable and not just
// their totals -- the old interleaved loop agrees with every total.

namespace {

struct FakeWorldDoc
{
    FakeWorldDoc(const char* n, std::string* log, bool willAgree)
        : name(n), transcript(log), agrees(willAgree) {}

    const char*  name;
    std::string* transcript;
    bool         agrees;

    // What CommitCloseWorld does to a real world, mirrored so a test can see it.
    bool worldClosing   = false;   // m_bWorldClosing
    bool mxpUp          = true;    // m_bMXP
    bool closeScriptRan = false;   // on_world_close

    int  timesAsked = 0;

    bool QueryCloseWorld()
    {
        ++timesAsked;
        *transcript += std::string("ask ") + name + "; ";
        return agrees;
    }

    void CommitCloseWorld()
    {
        *transcript += std::string("close ") + name + "; ";
        worldClosing   = true;
        mxpUp          = false;
        closeScriptRan = true;
    }

    // "Untouched" in the sense that matters: nothing observable happened to it.
    bool StillOpen() const
    {
        return !worldClosing && mxpUp && !closeScriptRan;
    }
};

// SaveModified is protected (ClassWizard's AFX_VIRTUAL block); the two halves
// it now delegates to are public, so only the whole-thing check needs this.
struct CloseWorldDoc : public CMUSHclientDoc
{
    using CMUSHclientDoc::SaveModified;
};

}  // namespace

TEST(QuitCloseAllWorlds, ACancelLeavesEveryWorldOpenAndUntouched)
{
    std::string transcript;
    FakeWorldDoc a("a", &transcript, true);
    FakeWorldDoc b("b", &transcript, true);
    FakeWorldDoc c("c", &transcript, false);   // the user cancels here
    FakeWorldDoc d("d", &transcript, true);
    std::vector<FakeWorldDoc*> worlds { &a, &b, &c, &d };

    EXPECT_FALSE(QueryThenCloseAllWorlds(worlds));

    // The worlds BEFORE the cancelled one are the whole point: they had already
    // said yes, and under the old single loop they were already gone.
    EXPECT_TRUE(a.StillOpen());
    EXPECT_TRUE(b.StillOpen());
    EXPECT_FALSE(a.worldClosing);        // spelled out: this is the flag that
    EXPECT_FALSE(b.worldClosing);        // used to stick true all session
    EXPECT_FALSE(a.closeScriptRan);
    EXPECT_FALSE(b.closeScriptRan);
    EXPECT_TRUE(a.mxpUp);
    EXPECT_TRUE(b.mxpUp);

    EXPECT_TRUE(c.StillOpen());          // and the one that refused, obviously
    EXPECT_TRUE(d.StillOpen());
    EXPECT_EQ(0, d.timesAsked);          // MFC stops at the first refusal

    // No close of any kind happened, in any order.
    EXPECT_EQ(std::string("ask a; ask b; ask c; "), transcript);
}

TEST(QuitCloseAllWorlds, AQuitThatIsNotCancelledStillClosesEverything)
{
    // The control.  Without it "we never close anything" passes the test above.
    std::string transcript;
    FakeWorldDoc a("a", &transcript, true);
    FakeWorldDoc b("b", &transcript, true);
    FakeWorldDoc c("c", &transcript, true);
    std::vector<FakeWorldDoc*> worlds { &a, &b, &c };

    EXPECT_TRUE(QueryThenCloseAllWorlds(worlds));

    for (FakeWorldDoc* w : worlds) {
        EXPECT_TRUE(w->worldClosing);
        EXPECT_TRUE(w->closeScriptRan);
        EXPECT_FALSE(w->mxpUp);
        EXPECT_EQ(1, w->timesAsked);
    }

    // ...and every question came before every close.  Totals alone would be
    // satisfied by the old interleaved loop, which is the bug.
    EXPECT_EQ(std::string("ask a; ask b; ask c; close a; close b; close c; "),
              transcript);
}

TEST(QuitCloseAllWorlds, NoWorldsOpenIsNotACancel)
{
    std::vector<FakeWorldDoc*> none;
    EXPECT_TRUE(QueryThenCloseAllWorlds(none));
}

// ── The real halves of SaveModified ────────────────────────────────────────
// The loop above is only correct if CMUSHclientDoc::QueryCloseWorld really does
// keep its hands off the closing side effects, so drive the shipping bodies.
TEST(QuitCloseAllWorlds, QueryingAWorldDoesNotStartClosingIt)
{
    CMUSHclientDoc doc;
    doc.m_bMXP = true;               // a live MXP session...
    doc.SetModifiedFlag(FALSE);      // ...with nothing to ask about
    doc.m_bVariablesChanged = false;

    EXPECT_TRUE(doc.QueryCloseWorld());

    // Neither of the two effects a bare document can show.  (on_world_close
    // needs a script engine, which a bare document has none of; the flag is the
    // one that used to stick true for the rest of the session.)
    EXPECT_FALSE(doc.m_bWorldClosing);
    EXPECT_TRUE(doc.m_bMXP);

    // Committing is what does them -- so the query pass above is a genuine
    // skip, not a document that had nothing to do either way.
    doc.CommitCloseWorld();
    EXPECT_TRUE(doc.m_bWorldClosing);
    EXPECT_FALSE(doc.m_bMXP);
}

TEST(QuitCloseAllWorlds, SaveModifiedStillDoesBothHalves)
{
    // Windows' caller of SaveModified must be unaffected by the split: it is
    // CanCloseFrame's "may I, and if so goodbye" in a single call.
    CloseWorldDoc doc;
    doc.m_bMXP = true;
    doc.SetModifiedFlag(FALSE);
    doc.m_bVariablesChanged = false;

    EXPECT_TRUE(doc.SaveModified());
    EXPECT_TRUE(doc.m_bWorldClosing);
    EXPECT_FALSE(doc.m_bMXP);
}

// ── UMessageBox's headless fallback answers the RIGHT button ────────────────
// With no hook installed there is no user to ask, so it must answer the least
// destructive button of whichever set was requested -- decline, never proceed.
//
// It used to be `(nType & MB_YESNO) ? IDNO : IDOK`, treating the button set as
// a bit field.  It is the LOW NIBBLE and an ENUMERATION: MB_YESNOCANCEL is 3
// and MB_YESNO is 4, so 3 & 4 == 0 and a three-way "save your changes?" prompt
// was answered IDOK -- which a caller testing for IDYES and IDCANCEL falls
// straight through, closing the world and discarding the work.  Exactly the
// mis-reading hook_message_box's own comment records being fixed there.
TEST(MfcUi, UMessageBoxHeadlessFallbackDeclines)
{
    struct { unsigned type; int want; const char* what; } kCases[] = {
        { MB_OK,                IDOK,     "MB_OK has nothing to decline" },
        { MB_OKCANCEL,          IDCANCEL, "MB_OKCANCEL declines" },
        { MB_ABORTRETRYIGNORE,  IDABORT,  "MB_ABORTRETRYIGNORE aborts" },
        { MB_YESNOCANCEL,       IDCANCEL, "MB_YESNOCANCEL cancels, NOT IDOK" },
        { MB_YESNO,             IDNO,     "MB_YESNO says no" },
        { MB_RETRYCANCEL,       IDCANCEL, "MB_RETRYCANCEL cancels" },
    };
    for (const auto& c : kCases) {
        const int got = UMessageBox("headless", c.type, "test");
        if (got != c.want)
            printf("  %s: got %d, want %d\n", c.what, got, c.want);
        EXPECT_EQ(c.want, got);
    }

    // The icon and default-button fields must not disturb the button decode --
    // they are the other two nibbles, and the real call sites always set them.
    EXPECT_EQ(IDCANCEL, UMessageBox("x", MB_YESNOCANCEL | MB_ICONQUESTION, "t"));
    EXPECT_EQ(IDNO,     UMessageBox("x", MB_YESNO | MB_ICONEXCLAMATION | MB_DEFBUTTON2, "t"));

    // And the answer is never one the caller cannot act on: every value above
    // is a real ID*, not zero.
    for (const auto& c : kCases)
        EXPECT_NE(0, UMessageBox("x", c.type, "t"));
}

// ── UMessageBox fills in a default icon, matching Windows ──────────────────
// twin-drift#2: the port's two UMessageBox twins (platform/windows_stubs.cpp
// for headless test builds, platform/mushapp_stubs.cpp for mushgui) used to
// forward nType straight through to the hook, so an icon-less caller reached
// wxMessageDialog with the icon nibble still zero.  Windows' UMessageBox
// (Utilities.cpp, #ifdef _WIN32) ORs in MB_ICONEXCLAMATION for OK / OKCANCEL /
// YESNO / YESNOCANCEL when the caller specified none -- reproducing MFC's
// AfxMessageBox default -- and about 60 of the tree's ~103 UMessageBox /
// TMessageBox call sites rely on exactly that fill.  ApplyDefaultMessageBoxIcon
// (platform/wx_mfc_hooks.h) is the shared decision both twins now call before
// touching the hook.
TEST(MfcUi, ApplyDefaultMessageBoxIconFillsInTheWindowsDefault)
{
    // The four button sets Windows fills, each with no icon bit set.
    EXPECT_EQ((unsigned)(MB_OK | MB_ICONEXCLAMATION),
              ApplyDefaultMessageBoxIcon(MB_OK));
    EXPECT_EQ((unsigned)(MB_OKCANCEL | MB_ICONEXCLAMATION),
              ApplyDefaultMessageBoxIcon(MB_OKCANCEL));
    EXPECT_EQ((unsigned)(MB_YESNO | MB_ICONEXCLAMATION),
              ApplyDefaultMessageBoxIcon(MB_YESNO));
    EXPECT_EQ((unsigned)(MB_YESNOCANCEL | MB_ICONEXCLAMATION),
              ApplyDefaultMessageBoxIcon(MB_YESNOCANCEL));

    // Windows' own comment calls these two "rarely used... caller should
    // specify the icon" and fills neither -- a fix that filled every button
    // set would wrongly paint these too.
    EXPECT_EQ((unsigned)MB_ABORTRETRYIGNORE,
              ApplyDefaultMessageBoxIcon(MB_ABORTRETRYIGNORE));
    EXPECT_EQ((unsigned)MB_RETRYCANCEL,
              ApplyDefaultMessageBoxIcon(MB_RETRYCANCEL));

    // A caller that already picked an icon is never overridden -- so a fix
    // that unconditionally ORs the exclamation bit in fails this.
    EXPECT_EQ((unsigned)(MB_OK | MB_ICONINFORMATION),
              ApplyDefaultMessageBoxIcon(MB_OK | MB_ICONINFORMATION));
    EXPECT_EQ((unsigned)(MB_YESNO | MB_ICONSTOP),
              ApplyDefaultMessageBoxIcon(MB_YESNO | MB_ICONSTOP));

    // Through the real UMessageBox path, via the hook: the flags the hook
    // actually receives are the ones a real prompt reaches wxMessageDialog
    // with, so this pins the wiring, not just the helper in isolation.
    // g_wxhook_message_box is a plain function pointer, so the capture has to
    // be static -- same pattern the mxp-charset block above (s_modals) uses.
    static unsigned s_capturedFlags = 0;
    s_capturedFlags = 0;
    int (*savedHook)(const char*, const char*, unsigned) = g_wxhook_message_box;
    g_wxhook_message_box = [](const char*, const char*, unsigned flags) -> int {
        s_capturedFlags = flags;
        return IDOK;
    };
    UMessageBox("Cannot connect. World name not specified", MB_OK, "MUSHclient");
    g_wxhook_message_box = savedHook;
    EXPECT_EQ((unsigned)(MB_OK | MB_ICONEXCLAMATION), s_capturedFlags);
}

// ── CProgressDlg: the shipping bodies, not a twin ───────────────────────────
//
// dialogs/ProgDlg.cpp is in mushcore as of 2026-08-07; fourteen hand-written
// stubs in platform/doc_flip_gui_stubs.cpp went with it.  CProgressDlg is the
// app's only modeless dialog, so this one class serves Recall, Send-to-world,
// Find/Global Change, the world-prefs memory calculation and the Lua `progress`
// library.
//
// This test exists for two reasons.  First, PROOF OF LIVENESS: adopting a TU
// without deleting its stub in the same commit builds clean and passes the
// whole suite, because both archives define the symbols and the linker silently
// picks one (docs/STUB-ADOPTION-2026-08.md §5a).  A green suite is what that
// hazard produces, not what rules it out — so the discriminator has to be an
// assertion the stub would FAIL.  SetStep is the strongest available: it had no
// stub at all, so this test could not even LINK before the adoption.
//
// Second, the relative movers are what ccd094b fixed, and until now their only
// coverage was the in-app selftest.  Their port arms are expressed in terms of
// SetPos, which is the one driver that is really wired, so the position
// arithmetic below is the whole of what the fix did.
// The port arm talks to wxProgressDialog through five function pointers, so a
// fake set of them drives the REAL chain rather than a friend class poking at
// members — the same technique tests/test_chat_sockets.cpp uses for the accept
// prompt.  It also lets the test see the value actually handed to the dialog,
// which is the only way to check the m_nLower rebase.
namespace {

struct ProgressSpy {
    static int         opens, updates, closes, ranges;
    static int         lastPos, lastRange;
    static std::string lastStatus, lastTitle, openCaption;
    static bool        answer;          // what _update reports (false = cancelled)
    static int         handleTarget;    // its address is the fake handle

    static void reset()
    {
        opens = updates = closes = ranges = 0;
        lastPos = lastRange = -999;
        lastStatus.clear(); lastTitle.clear(); openCaption.clear();
        answer = true;
    }
};
int ProgressSpy::opens, ProgressSpy::updates, ProgressSpy::closes, ProgressSpy::ranges;
int ProgressSpy::lastPos, ProgressSpy::lastRange;
std::string ProgressSpy::lastStatus, ProgressSpy::lastTitle, ProgressSpy::openCaption;
bool ProgressSpy::answer;
int ProgressSpy::handleTarget;

void* SpyOpen(const char* caption, const char*, int maximum)
{
    ++ProgressSpy::opens;
    ProgressSpy::openCaption = caption ? caption : "";
    ProgressSpy::lastRange   = maximum;
    return &ProgressSpy::handleTarget;
}
void SpySetRange(void*, int maximum) { ++ProgressSpy::ranges; ProgressSpy::lastRange = maximum; }
bool SpyUpdate(void*, int pos, const char* status)
{
    ++ProgressSpy::updates;
    ProgressSpy::lastPos    = pos;
    ProgressSpy::lastStatus = status ? status : "";
    return ProgressSpy::answer;
}
void SpyClose(void*) { ++ProgressSpy::closes; }
void SpySetTitle(void*, const char* title) { ProgressSpy::lastTitle = title ? title : ""; }

struct ProgressHooks {
    void* (*sOpen)(const char*, const char*, int);
    void  (*sRange)(void*, int);
    bool  (*sUpdate)(void*, int, const char*);
    void  (*sClose)(void*);
    void  (*sTitle)(void*, const char*);
    ProgressHooks()
      : sOpen(g_wxhook_progress_open), sRange(g_wxhook_progress_set_range),
        sUpdate(g_wxhook_progress_update), sClose(g_wxhook_progress_close),
        sTitle(g_wxhook_progress_set_title)
    {
        ProgressSpy::reset();
        g_wxhook_progress_open      = SpyOpen;
        g_wxhook_progress_set_range = SpySetRange;
        g_wxhook_progress_update    = SpyUpdate;
        g_wxhook_progress_close     = SpyClose;
        g_wxhook_progress_set_title = SpySetTitle;
    }
    ~ProgressHooks()
    {
        g_wxhook_progress_open      = sOpen;
        g_wxhook_progress_set_range = sRange;
        g_wxhook_progress_update    = sUpdate;
        g_wxhook_progress_close     = sClose;
        g_wxhook_progress_set_title = sTitle;
    }
};

} // namespace

TEST(MfcUi, ProgressDialogDriversRunTheShippingBodies)
{
    ProgressHooks hooks;
    CProgressDlg dlg;

    // Create reports whether it really got a dialog.  The stub returned TRUE
    // unconditionally, so every caller believed it had one.
    ASSERT_TRUE(dlg.Create(NULL));
    EXPECT_EQ(1, ProgressSpy::opens);

    // The real ctor seeds a state no stub did: range 0..100, step 10.  Create
    // passes that range straight through as the dialog's maximum.
    EXPECT_EQ(100, ProgressSpy::lastRange);

    // Callers do Create() then SetRange(), so the true maximum is only known
    // afterwards and the port re-sizes the bar.  The bound handed over is the
    // EXTENT, not the upper bound — so the lower bound must be non-zero here or
    // the subtraction under test is an identity and any bug survives.
    dlg.SetRange(20, 220);
    EXPECT_EQ(1, ProgressSpy::ranges);
    EXPECT_EQ(200, ProgressSpy::lastRange);
    dlg.SetRange(0, 200);                    // back to a zero base for the rest
    EXPECT_EQ(2, ProgressSpy::ranges);

    EXPECT_EQ(40, dlg.SetPos(40));
    EXPECT_EQ(40, ProgressSpy::lastPos);
    EXPECT_EQ(10, dlg.StepIt() - 40);        // the ctor's default step

    // SetStep's RETURN is meaningless off Windows — it forwards to the
    // CProgressCtrl shim, which is a `return 0` (platform/mfc_ui.h:4121).  Only
    // the m_nStep it stores matters, and StepIt is the only way to observe it.
    EXPECT_EQ(0, dlg.SetStep(5));
    EXPECT_EQ(55, dlg.StepIt());             // 50 + 5, so the STORE took effect
    EXPECT_EQ(55, ProgressSpy::lastPos);

    // OffsetPos is relative to where StepIt left the bar — the coupling through
    // m_iPortPos that ccd094b introduced.  Before it, both drove an inert
    // CProgressCtrl and a pump that returns on a null m_hWnd, so the bar never
    // moved at all.
    EXPECT_EQ(58, dlg.OffsetPos(3));
    EXPECT_EQ(63, dlg.StepIt());
    EXPECT_EQ(60, dlg.OffsetPos(-3));        // and it really is signed
    EXPECT_EQ(60, ProgressSpy::lastPos);

    // Every one of those reached the dialog: 1 (SetPos) + 5 (the movers).
    EXPECT_EQ(6, ProgressSpy::updates);

    // The value CALLERS see is absolute; the lower bound is subtracted only on
    // the way to the dialog, which is a wxProgressDialog counting from 0.
    dlg.SetRange(100, 300);
    EXPECT_EQ(120, dlg.SetPos(120));
    EXPECT_EQ(20, ProgressSpy::lastPos);     // 120 - 100
    EXPECT_EQ(125, dlg.StepIt());
    EXPECT_EQ(25, ProgressSpy::lastPos);

    // SetWindowText retitles the live dialog.  It used to drop every caption,
    // so all five callers read a generic "Progress".
    dlg.SetWindowText("Recalling...");
    EXPECT_EQ(std::string("Recalling..."), ProgressSpy::lastTitle);

    // SetStatus is DEFERRED, and the difference is observable: wxProgressDialog
    // carries the message on its Update() call, so the text lands with the next
    // position change rather than immediately.  Asserting only the end state
    // would pass against an implementation that showed it at once, which is
    // what the Windows arm does.
    const std::string before = ProgressSpy::lastStatus;
    dlg.SetStatus("line 3 of 9");
    EXPECT_EQ(before, ProgressSpy::lastStatus);        // not yet
    dlg.StepIt();
    EXPECT_EQ(std::string("line 3 of 9"), ProgressSpy::lastStatus);   // now
}

// CheckCancelButton is one-shot BY DESIGN: it reports the flag and clears it, so
// a caller can poll it in a loop and still continue.  The stub returned FALSE
// unconditionally — indistinguishable from "nobody cancelled" — so a test that
// only covered the un-cancelled case would have passed against it too.
TEST(MfcUi, ProgressDialogCancelIsLatchedThenCleared)
{
    ProgressHooks hooks;
    CProgressDlg dlg;
    ASSERT_TRUE(dlg.Create(NULL));

    EXPECT_FALSE(dlg.CheckCancelButton());   // nothing has happened yet
    EXPECT_EQ(10, dlg.SetPos(10));
    EXPECT_FALSE(dlg.CheckCancelButton());   // ...and an ordinary update is not a cancel

    // The user hits Cancel: wxProgressDialog::Update reports false, and that is
    // the ONLY way the flag gets set on this platform.
    ProgressSpy::answer = false;
    dlg.SetPos(20);

    EXPECT_TRUE(dlg.CheckCancelButton());    // latched — the stub could never say this

    // MFC's read-and-clear only SHOWS once the dialog stops reporting a cancel:
    // CheckCancelButton is now a pump too, so while wxProgressDialog::Update
    // keeps saying "cancelled" (which a real one does — the flag is sticky) the
    // poll keeps saying so, rather than answering "carry on" to a user who has
    // already pressed the button.
    EXPECT_TRUE(dlg.CheckCancelButton());
    ProgressSpy::answer = true;              // ...and once it stops reporting it
    EXPECT_FALSE(dlg.CheckCancelButton());   // the flag is cleared, so a loop may continue

    // It latches again rather than being once-per-lifetime.
    ProgressSpy::answer = false;
    dlg.SetPos(30);
    EXPECT_TRUE(dlg.CheckCancelButton());
}

// A caller that polls ONLY checkcancel -- which the Lua progress library offers
// as a standalone method -- used to get a frozen dialog: PumpMessages returns at
// once on a null m_hWnd, so nothing repainted and the Cancel click could never
// be seen.  wxProgressDialog::Update is this platform's pump, so CheckCancelButton
// now re-sends the current position.
TEST(MfcUi, CheckCancelButtonPumpsWithoutASetPos)
{
    ProgressHooks hooks;
    CProgressDlg dlg;
    ASSERT_TRUE(dlg.Create(NULL));
    dlg.SetRange(0, 100);
    dlg.SetStatus("working");
    EXPECT_EQ(40, dlg.SetPos(40));

    const int before = ProgressSpy::updates;
    ProgressSpy::answer = false;             // the user hits Cancel

    EXPECT_TRUE(dlg.CheckCancelButton());    // seen with NO intervening SetPos
    EXPECT_EQ(before + 1, ProgressSpy::updates);
    EXPECT_EQ(40, ProgressSpy::lastPos);     // and it did not move the bar
    EXPECT_EQ(std::string("working"), ProgressSpy::lastStatus);

    // The lower bound is still subtracted on the way to the dialog, exactly as
    // SetPos does it — so a non-zero base cannot make this an identity.
    dlg.SetRange(20, 120);
    dlg.SetPos(50);
    EXPECT_EQ(30, ProgressSpy::lastPos);
    dlg.CheckCancelButton();
    EXPECT_EQ(30, ProgressSpy::lastPos);
}

// With no hooks installed — which is every other headless test in this binary —
// the drivers must stay safe rather than dereferencing a null handle.  This is
// what the old stubs bought, and adopting the real bodies must not lose it.
TEST(MfcUi, ProgressDialogIsInertWithNoDialogBehindIt)
{
    CProgressDlg dlg;
    EXPECT_FALSE(dlg.Create(NULL));          // honest: there is no dialog

    dlg.SetRange(0, 50);
    EXPECT_EQ(7, dlg.SetPos(7));             // the position is still tracked...
    EXPECT_EQ(17, dlg.StepIt());             // ...so the arithmetic still works
    EXPECT_EQ(20, dlg.OffsetPos(3));
    dlg.SetWindowText("x");
    dlg.SetStatus("y");
    EXPECT_FALSE(dlg.CheckCancelButton());   // nothing can cancel it
}

// Plugin Wizard language page (2026-08-12).  USE_PYTHON/USE_TCL/USE_RUBY are
// defined only in stdafx.h's _WIN32 arm, so the wizard's language switch was
// compiled WITHOUT those cases here: choosing Python/Tcl/Ruby (all fully
// supported engines on the port) produced a plugin whose language attribute
// was "", and re-entering the page never preselected them.  The gates now
// admit the port.  PHP stays Windows-only — the port refuses it by design.
TEST(MfcUi, PluginWizardLanguagePageSpeaksAllPortLanguages)
{
    fakewx::Install hooks;
    // OnInitDialog is protected; re-expose it (members and OnOK are public).
    struct Probe : CPluginWizardPage7 { using CPluginWizardPage7::OnInitDialog; };
    Probe page;

    // Re-entering the page preselects a port language (used to map to -1).
    page.m_strLanguage = "python";
    page.OnInitDialog();
    EXPECT_EQ(3, fakewx::widget(IDC_LANGUAGE).cursel);

    struct Case { int idx; const char* want; };
    static const Case kCases[] = {
        { 3, "Python" }, { 4, "TclScript" }, { 7, "RubyScript" },
        { 5, "Lua" },                       // control: has always worked
    };
    for (const Case& c : kCases) {
        fakewx::widget(IDC_LANGUAGE).cursel = c.idx;
        page.OnOK();
        EXPECT_STREQ(c.want, (LPCTSTR) page.m_strLanguage);
    }

    // PHP is unsupported on the port: still the documented empty refusal.
    fakewx::widget(IDC_LANGUAGE).cursel = 6;
    page.OnOK();
    EXPECT_STREQ("", (LPCTSTR) page.m_strLanguage);
}

// CPropertySheet::GetActivePage() was a permanent nullptr, and every sheet's
// OnKickIdle handler dereferences it with no null check (GlobalPrefsSheet.cpp:59,
// PluginWizardSheet.cpp:90, PrefsPropertySheet.cpp:45).  Nothing on the port
// routes WM_KICKIDLE to a sheet today, so it was a landmine rather than a live
// crash — the fix is to make the accessor answer, not to touch the canonical
// dialog code, which cannot see a null GetActivePage on Windows.
//
// (The shim's CWnd::WindowProc does not consult message maps, so there is no way
// to reach the handler through ON_MESSAGE here; it is called directly.)
TEST(PropertySheet, ActivePageIsARealPageSoKickIdleCannotCrash)
{
    struct Probe : CPluginWizardSheet { using CPluginWizardSheet::OnKickIdle; };
    Probe sheet;

    // The ctor adds eight pages, and MFC opens on the first.
    EXPECT_EQ(8, sheet.GetPageCount());
    EXPECT_EQ((CPropertyPage*) &sheet.m_Page1, sheet.GetActivePage());
    EXPECT_EQ(0, sheet.GetActiveIndex());
    EXPECT_EQ(3, sheet.GetPageIndex(&sheet.m_Page4));
    EXPECT_EQ((CPropertyPage*) &sheet.m_Page4, sheet.GetPage(3));
    EXPECT_EQ((CPropertyPage*) nullptr, sheet.GetPage(99));

    sheet.SetActivePage(3);
    EXPECT_EQ((CPropertyPage*) &sheet.m_Page4, sheet.GetActivePage());
    sheet.SetActivePage(&sheet.m_Page7);
    EXPECT_EQ(6, sheet.GetActiveIndex());
    sheet.SetActivePage(99);                  // out of range: ignored, not stored
    EXPECT_EQ(6, sheet.GetActiveIndex());

    // The handler itself.  It cannot crash HERE even with a null active page —
    // CWnd::SendMessage returns before touching `this` when no send hook is
    // installed, which is every headless test — so this line is not what
    // discriminates; the accessor assertions above are.  It is here because in
    // the GUI, where the hook IS installed, the same call would dereference.
    EXPECT_EQ((LRESULT) 0, sheet.OnKickIdle(0, 0));
}

// ═══════════════════════════════════════════════════════════════════════════
// World-properties Output page: the three sums that used to be buried in
// dialogs nothing tests (2026-08-18)
// ═══════════════════════════════════════════════════════════════════════════

// "Adjust width to size" (World Properties ▸ Appearance ▸ Output).  Its whole
// body used to sit inside a walk looking for a CMUSHView, which the port never
// constructs — so the button did nothing at all here.  The document answers for
// both frontends now, from whatever last measured the pane.
TEST(AdjustWidth, TheWrapColumnComesFromTheMeasuredOutputWindow)
{
    CMUSHclientDoc doc;
    doc.m_gui_cols       = 100;    // what the wx pane last measured...
    doc.m_gui_font_width = 8;      // ...i.e. 800 pixels of text
    doc.m_iPixelOffset   = 0;

    EXPECT_EQ(800, doc.OutputWindowWidthPixels());
    EXPECT_EQ(100, doc.WrapColumnForCharWidth(8));

    // A wider font fits fewer columns: the division is real, not a pass-through
    // of the column count the pane happened to report.
    EXPECT_EQ(50, doc.WrapColumnForCharWidth(16));

    // ...and the world's pixel offset insets the text before the division, as
    // Windows' one wrap formula does (mushview.cpp:3132).
    doc.m_iPixelOffset = 80;
    EXPECT_EQ(90, doc.WrapColumnForCharWidth(8));
}

TEST(AdjustWidth, TheAnswerStaysInsideTheWrapColumnOptionsRange)
{
    CMUSHclientDoc doc;
    doc.m_iPixelOffset   = 0;
    doc.m_gui_font_width = 8;

    doc.m_gui_cols = 4000;                                   // 32,000 pixels
    EXPECT_EQ(MAX_LINE_WIDTH, doc.WrapColumnForCharWidth(8));
    doc.m_gui_cols = 10;                                     // 80 pixels
    EXPECT_EQ(20, doc.WrapColumnForCharWidth(8));            // the floor

    // 0 = "cannot tell, keep the figure you have" — the button's old behaviour
    // when the view walk found nothing, and the only safe answer.
    EXPECT_EQ(0, doc.WrapColumnForCharWidth(0));             // no font metric
    doc.m_gui_cols = 0;
    EXPECT_EQ(0, doc.WrapColumnForCharWidth(8));             // nothing measured
}

// The physical-RAM warning on the output-buffer size.  The port raised the
// ceiling from Windows' 500,000 lines to 100,000,000, at which point the DWORD
// the byte count accumulated into wrapped — so the largest, and only genuinely
// dangerous, settings were the ones that warned least.
TEST(OutputBufferSize, TheMemoryWarningSurvivesTheRaisedLineCeiling)
{
    // Windows' old ceiling: comfortably inside 32 bits either way.
    EXPECT_EQ(16ULL * 1024 * 1024 + 500000ULL * 60,
              OutputBufferBytesNeeded(500000L));

    // The port's ceiling is ~6.02 Gb.  The wrapped figure was ~1.72 Gb, so a
    // 4 Gb machine was told the biggest possible buffer would fit.
    const unsigned long long need = OutputBufferBytesNeeded(100000000L);
    EXPECT_EQ(16ULL * 1024 * 1024 + 100000000ULL * 60, need);
    EXPECT_TRUE(need > 4ULL * 1024 * 1024 * 1024);
    EXPECT_TRUE(need > 6000000000ULL);

    // 0 lines is "unlimited" and has its own warning; it costs no per-line
    // bytes here.
    EXPECT_EQ(16ULL * 1024 * 1024, OutputBufferBytesNeeded(0L));
}

// The alias / timer / variable pages' "fix up last column to exactly fit the
// list view" figure.  It used to be stored in m_iColWidth after the base class
// had already inserted the columns from it, so it reached nothing.
TEST(PrefsListColumns, TheLastColumnIsFittedOnlyWhenTheUserHasNoWidthOfTheirOwn)
{
    // Seed still in place = the user has never resized this column, so fit it:
    // 600 wide, less a 15-pixel scrollbar, less 160 of other columns.
    EXPECT_EQ(425, FittedLastColumnWidth(150, 150, 600, 15, 160));

    // A restored width that differs is the user's own: leave it alone.
    EXPECT_EQ(0, FittedLastColumnWidth(150, 320, 600, 15, 160));

    // A list too narrow for the other columns is not a reason to set a silly
    // width — 0 means "leave the column alone" here too.
    EXPECT_EQ(0, FittedLastColumnWidth(150, 150, 100, 15, 160));
    EXPECT_EQ(0, FittedLastColumnWidth(150, 150, 175, 15, 160));
}

MC_TEST_MAIN()

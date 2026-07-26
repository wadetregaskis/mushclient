// platform/mainfrm_stub.h — Non-Windows stub for CMainFrame / CMyToolBar.
// Included by mainfrm.h on non-Windows builds.
#pragma once
#ifndef _WIN32

#include "platform/compat.h"

// ── Macros defined in mainfrm.h Windows block ────────────────────────────
#ifndef TRAY_FIRST_MENU
#  define TRAY_FIRST_MENU         11000
#  define TRAY_MENU_COUNT         50
#  define ACCELERATOR_FIRST_COMMAND 12000
#  define ACCELERATOR_COUNT       1000
#endif

// ── NOTIFYICONDATA ────────────────────────────────────────────────────────
#ifndef NOTIFYICONDATA_defined
#define NOTIFYICONDATA_defined
struct NOTIFYICONDATA {
    DWORD cbSize = sizeof(NOTIFYICONDATA);
    HWND  hWnd   = nullptr;
    UINT  uID    = 0;
    UINT  uFlags = 0;
    UINT  uCallbackMessage = 0;
    HICON hIcon  = nullptr;
    char  szTip[128] = {};
};
#endif
#ifndef NIF_MESSAGE
#  define NIF_MESSAGE   0x01
#  define NIF_ICON      0x02
#  define NIF_TIP       0x04
#  define NIM_ADD       0x00000000
#  define NIM_MODIFY    0x00000001
#  define NIM_DELETE    0x00000002
#endif
inline BOOL Shell_NotifyIcon(DWORD, NOTIFYICONDATA*) { return FALSE; }

// ── DRAWITEMSTRUCT / LPDRAWITEMSTRUCT ─────────────────────────────────────
#ifndef DRAWITEMSTRUCT_defined
#define DRAWITEMSTRUCT_defined
struct DRAWITEMSTRUCT {
    UINT  CtlType = 0; UINT CtlID = 0; UINT itemID = 0;
    UINT  itemAction = 0; UINT itemState = 0;
    HWND  hwndItem = nullptr; HDC hDC = nullptr;
    RECT  rcItem = {}; DWORD itemData = 0;
};
typedef DRAWITEMSTRUCT* LPDRAWITEMSTRUCT;
#endif

// ── CHARFORMAT ────────────────────────────────────────────────────────────
#ifndef CHARFORMAT_defined
#define CHARFORMAT_defined
#define CFM_BOLD      0x00000001
#define CFM_ITALIC    0x00000002
#define CFM_UNDERLINE 0x00000004
#define CFM_STRIKEOUT 0x00000008
#define CFM_FACE      0x20000000
#define CFM_SIZE      0x80000000
struct CHARFORMAT {
    UINT cbSize = sizeof(CHARFORMAT);
    DWORD dwMask = 0;
    DWORD dwEffects = 0;
    LONG  yHeight = 0;
    LONG  yOffset = 0;
    COLORREF crTextColor = 0;
    BYTE bCharSet = 0; BYTE bPitchAndFamily = 0;
    char szFaceName[32] = {};
};
#endif

// ── CMDIClientWnd ─────────────────────────────────────────────────────────
// Use the real declaration from MDIClientWnd.h (CWnd already has SubclassWindow).
#include "MDIClientWnd.h"

// ── CMyToolBar ────────────────────────────────────────────────────────────
#ifndef CMyToolBar_defined
#define CMyToolBar_defined
class CMyToolBar : public CToolBar {
public:
    void DrawGripper(CDC* pDC, const CRect& rect);
    void EraseNonClient();
    virtual void DoPaint(CDC* pDC);
    void OnNcPaint();
    void EnableDocking(DWORD) {}
    DWORD GetBarStyle() const { return 0; }
    void  SetBarStyle(DWORD) {}
    BOOL  GetButtonInfo(int, UINT&, UINT&, int&) const { return FALSE; }
    BOOL  SetButtonInfo(int, UINT, UINT, int) { return FALSE; }
    DECLARE_MESSAGE_MAP()
};
#endif

// ── CMDITabs ─────────────────────────────────────────────────────────────
#ifndef CMDITabs_defined
#define CMDITabs_defined
class CMDITabs : public CWnd {
public:
    bool InUse()  const { return false; }
    void Update()       {}
    BOOL Create(CWnd*, UINT=0) { return FALSE; }
};
#endif

// ── EMDITabStyles — from MDITabs.h (included via mainfrm.h on Windows) ────
// Defined here so mainfrm.cpp can use MT_TOP/MT_BOTTOM without MDITabs.h.
#ifndef MT_BOTTOM
enum EMDITabStyles {
    MT_BOTTOM        = 0x0000,
    MT_TOP           = 0x0001,
    MT_IMAGES        = 0x0002,
    MT_HIDEWLT2VIEWS = 0x0004,
    MT_TOOLTIPS      = 0x0008,
    MT_BUTTONS       = 0x0010,
    MT_AUTOSIZE      = 0x0020,
    MT_TASKBAR       = 0x0038
};
#endif

// ── idleMenu flag used in mainfrm.cpp ─────────────────────────────────────
#ifndef idleMenu
#  define idleMenu 1
#endif

// ── CMainFrame ────────────────────────────────────────────────────────────
#ifndef CMainFrame_defined
#define CMainFrame_defined
class CMainFrame : public CMDIFrameWnd {
    DECLARE_DYNAMIC(CMainFrame)
public:
    CMainFrame();
    virtual ~CMainFrame();

    // Data members
    int             m_nPauseItem          = 0;
    UINT            m_wDeviceID           = 0;
    UINT            m_timer               = 0;
    UINT            m_ticktimer           = 0;
    bool            m_bFullScreen         = false;
    WINDOWPLACEMENT m_wpPrev              = {};
    BOOL            m_bMaximized          = FALSE;
    bool            m_bFlashingWindow     = false;
    bool            m_bActive             = false;
    NOTIFYICONDATA  m_niData;
    CMDITabs        m_wndMDITabs;
    CTime           m_timeFlashed;
    int             m_iTabsCount          = 0;
    CMyToolBar      m_wndToolBar;
    CMyToolBar      m_wndGameToolBar;
    CMyToolBar      m_wndActivityToolBar;
    CDialogBar      m_wndInfoBar;
    CStatusBar      m_wndStatusBar;
    BOOL            m_bAutoMenuEnable     = FALSE;
    COLORREF        m_backgroundColour    = 0xFFFFFFFF;
    CMDIClientWnd   m_wndMDIClient;
    CHARFORMAT      m_defaultInfoBarFormat;
    int             m_nIdleFlags          = 0;

    // Methods
    BOOL OnDynamicTipText(UINT, NMHDR*, LRESULT*);
    void SetStatusMessage(const CString&);
    void SetStatusMessageNow(const CString&);
    void SetStatusNormal();
    void ReturnStatusRect(CRect&);
    void ReturnToolbarRect(CRect&);
    void DelayDebugStatus(const CString&);
    void FixUpTitleBar();
    void OnUpdateBtnWorlds(int, CCmdUI*);
    void CancelSound();
    bool PlaySoundFile(const CString&);
    void ShowMCIError(DWORD, const CString&);
    bool IsFullScreen() { return m_bFullScreen; }
    void AddTrayIcon();
    void LeftTrayClick();
    void RightTrayClick();
    void OpenAndConnectToWorldsInStartupList(const bool);
    virtual void OnUpdateFrameMenu(HMENU);
    virtual void OnUpdateFrameTitle(BOOL);
    void DoFileOpen();
    void DoFixMenus(CCmdUI*);
    void CheckTimerFallback();
    void ProcessTimers();
    // NO PostMessage OVERRIDE HERE.  This header is included by mainfrm.h for
    // every non-Windows build — mushgui as well as headless — so an in-class
    // PostMessage name-hides the real CWnd::PostMessage (platform/compat.h:253)
    // in the GUI too, and the two Frame.PostMessage call sites in the engine
    // both went nowhere: world.DoCommand (scripting/methods/methods_commands.cpp)
    // for its whole ~200-command surface, and the TLS-failure consent prompt
    // (doc_shared.cpp), whose handler in wx_mfc_impl.cpp had therefore never run
    // — a failed handshake gave up silently instead of asking the user.
    // Inheriting CWnd::PostMessage keeps the headless behaviour this stub was
    // written for BY CONSTRUCTION: it forwards to g_wxhook_post_message and
    // returns FALSE when that pointer is null, which is exactly the headless
    // case (no message pump, so TLS failures still fail closed).  Neither call
    // site inspects the return value.

    // MFC overridables
    virtual BOOL PreTranslateMessage(MSG*);
    virtual BOOL PreCreateWindow(CREATESTRUCT&);
    virtual LRESULT WindowProc(UINT, WPARAM, LPARAM);
    virtual void AssertValid() const {}
    virtual void Dump(CDumpContext&) const {}

    // Message handlers
    int  OnCreate(LPCREATESTRUCT);
    void OnClose();
    void OnDestroy();
    void OnContextMenu(CWnd*, CPoint);
    void OnSize(UINT, int, int);
    void OnActivate(UINT, CWnd*, BOOL);
    void OnTimer(UINT);
    void OnSysCommand(UINT, LPARAM);
    void OnDrawItem(int, LPDRAWITEMSTRUCT);
    BOOL OnWorldSwitch(UINT);
    LRESULT OnMCINotify(WPARAM, LPARAM);
    void OnDisplayActivitylist();
    void OnUpdateDisplayActivitylist(CCmdUI*);
    void OnFilePreferences();
    void OnFileOpen();
    void OnFileOpenworldsinstartuplist();
    void OnUpdateFileOpenworldsinstartuplist(CCmdUI*);
    void OnConnectionAutoconnect();
    void OnUpdateConnectionAutoconnect(CCmdUI*);
    void OnConnectionReconnectondisconnect();
    void OnUpdateConnectionReconnectondisconnect(CCmdUI*);
    void OnConnectionConnecttoallopenworlds();
    void OnConnectionConnecttoworldsinstartuplist();
    void OnUpdateConnectionConnecttoworldsinstartuplist(CCmdUI*);
    void OnHelpContents();
    void OnHelpTipoftheday();
    void OnHelpBugreportsuggestion();
    void OnHelpForum();
    void OnHelpFunctionswebpage();
    void OnHelpDocumentationwebpage();
    void OnHelpRegularexpressionswebpage();
    void OnHelpPluginslist();
    void OnHelpMudlists();
    void OnWebPage();
    void OnUpdateBtnWorldsWorld0(CCmdUI*);
    void OnUpdateBtnWorldsWorld1(CCmdUI*);
    void OnUpdateBtnWorldsWorld2(CCmdUI*);
    void OnUpdateBtnWorldsWorld3(CCmdUI*);
    void OnUpdateBtnWorldsWorld4(CCmdUI*);
    void OnUpdateBtnWorldsWorld5(CCmdUI*);
    void OnUpdateBtnWorldsWorld6(CCmdUI*);
    void OnUpdateBtnWorldsWorld7(CCmdUI*);
    void OnUpdateBtnWorldsWorld8(CCmdUI*);
    void OnUpdateBtnWorldsWorld9(CCmdUI*);
    void OnViewResetToolbars();
    void OnViewAlwaysontop();
    void OnUpdateViewAlwaysontop(CCmdUI*);
    void OnViewFullscreenmode();
    void OnUpdateViewFullscreenmode(CCmdUI*);
    void OnGameSendtoallworlds();
    void OnEditReloadnamesfile();
    void OnEditNotesworkarea();
    void OnEditGenerateuniqueid();
    void OnEditConvertclipboardforumcodes();
    void OnWindowCloseallnotepadwindows();
    void OnDisplayStopsoundplaying();
    void OnUpdateDisplayStopsoundplaying(CCmdUI*);
    void OnFileWinsock();
    void OnGameFunctionslist();
    void OnF1Test();
    void OnUpdateStatuslineFreeze(CCmdUI*);
    void OnUpdateStatuslineMushname(CCmdUI*);
    void OnUpdateStatuslineLines(CCmdUI*);
    void OnUpdateStatuslineLog(CCmdUI*);
    void OnUpdateStatuslineTime(CCmdUI*);
    void OnUpdateInfoBar(CCmdUI*);
    void OnTrayMenu(UINT);
    void OnFixMenus(CCmdUI*);

    operator HWND() const { return m_hWnd; }
    // Additional methods called on Frame in non-Windows code
    CFrameWnd* GetActiveFrame() { return nullptr; }
    // LoadFrame is NOT redefined here; CMDIFrameWnd::LoadFrame in
    // mushapp_stubs.cpp provides the real implementation (calls the wx hook).

    DECLARE_MESSAGE_MAP()
};
extern CMainFrame Frame;
#endif // CMainFrame_defined

// FindWorld — used by many .cpp files
class CMUSHclientDoc;
CMUSHclientDoc* FindWorld(const UINT nIDC);

#endif // !_WIN32

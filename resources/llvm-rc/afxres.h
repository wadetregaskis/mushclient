// afxres.h for llvm-rc: what resources/en.rc needs from the Windows SDK and
// MFC headers, so that the macOS and Linux builds can compile it with no
// Windows SDK installed.
//
// On Windows, rc.exe never sees this file: en.vcxproj compiles en.rc against
// the real afxres.h, which pulls in winres.h and through it most of the SDK's
// resource headers.  The port's build (resources/CMakeLists.txt) runs llvm-rc
// with this directory first on the include path instead.  These are the SDK's
// published values for every identifier en.rc uses that src/resource.h does
// not define, and nothing else.  An identifier en.rc gains later fails the
// port's build with llvm-rc's "undefined" error; add its value here.
//
// tests/check_port_resources.py compiles en.rc both ways on Windows and
// fails if the port would see anything rc.exe's build does not.

#ifndef MC_LLVM_RC_AFXRES_H
#define MC_LLVM_RC_AFXRES_H

// MFC's TOOLBAR statement is rc.exe's alone (en.rc guards its toolbars).
#define MC_RC_NO_TOOLBARS

// rc.exe knows DLGINIT as resource type 240 (RT_DLGINIT); to llvm-rc it is
// any other user-defined type, so give it its number.
#define DLGINIT 240

// Window and dialog styles (winuser.h)
#define DS_3DLOOK            0x0004
#define DS_MODALFRAME        0x80
#define WS_BORDER            0x00800000
#define WS_CAPTION           0x00C00000
#define WS_CHILD             0x40000000
#define WS_DISABLED          0x08000000
#define WS_EX_DLGMODALFRAME  0x00000001
#define WS_GROUP             0x00020000
#define WS_HSCROLL           0x00100000
#define WS_MAXIMIZEBOX       0x00010000
#define WS_MINIMIZEBOX       0x00020000
#define WS_POPUP             0x80000000
#define WS_SYSMENU           0x00080000
#define WS_TABSTOP           0x00010000
#define WS_THICKFRAME        0x00040000
#define WS_VISIBLE           0x10000000
#define WS_VSCROLL           0x00200000

// Control styles (winuser.h, commctrl.h)
#define BS_AUTO3STATE         0x00000006
#define BS_AUTOCHECKBOX       0x00000003
#define BS_AUTORADIOBUTTON    0x00000009
#define BS_FLAT               0x00008000
#define BS_OWNERDRAW          0x0000000B
#define CBS_DROPDOWN          0x0002
#define CBS_DROPDOWNLIST      0x0003
#define CBS_HASSTRINGS        0x0200
#define CBS_OWNERDRAWFIXED    0x0010
#define CBS_SORT              0x0100
#define ES_AUTOHSCROLL        0x0080
#define ES_AUTOVSCROLL        0x0040
#define ES_MULTILINE          0x0004
#define ES_NOHIDESEL          0x0100
#define ES_NUMBER             0x2000
#define ES_PASSWORD           0x0020
#define ES_READONLY           0x0800
#define ES_RIGHT              0x0002
#define ES_UPPERCASE          0x0008
#define ES_WANTRETURN         0x1000
#define LBS_MULTIPLESEL       0x0008
#define LBS_NOINTEGRALHEIGHT  0x0100
#define LBS_SORT              0x0002
#define LVS_LIST              0x0003
#define LVS_NOCOLUMNHEADER    0x4000
#define LVS_REPORT            0x0001
#define LVS_SHOWSELALWAYS     0x0008
#define LVS_SINGLESEL         0x0004
#define LVS_SORTASCENDING     0x0010
#define SS_BITMAP             0x0000000E
#define SS_BLACKFRAME         0x00000007
#define TBS_BOTH              0x0008
#define TBS_NOTICKS           0x0010
#define UDS_ALIGNRIGHT        0x0004
#define UDS_ARROWKEYS         0x0020
#define UDS_AUTOBUDDY         0x0010
#define UDS_NOTHOUSANDS       0x0080
#define UDS_SETBUDDYINT       0x0002
#define UDS_WRAP              0x0001

// Virtual keys, for ACCELERATORS (winuser.h)
#define VK_ADD       0x6B
#define VK_BACK      0x08
#define VK_DECIMAL   0x6E
#define VK_DELETE    0x2E
#define VK_DIVIDE    0x6F
#define VK_DOWN      0x28
#define VK_END       0x23
#define VK_ESCAPE    0x1B
#define VK_F1        0x70
#define VK_F10       0x79
#define VK_F11       0x7A
#define VK_F12       0x7B
#define VK_F2        0x71
#define VK_F3        0x72
#define VK_F4        0x73
#define VK_F5        0x74
#define VK_F6        0x75
#define VK_F7        0x76
#define VK_F8        0x77
#define VK_F9        0x78
#define VK_HOME      0x24
#define VK_INSERT    0x2D
#define VK_MULTIPLY  0x6A
#define VK_NEXT      0x22
#define VK_NUMPAD0   0x60
#define VK_NUMPAD1   0x61
#define VK_NUMPAD2   0x62
#define VK_NUMPAD3   0x63
#define VK_NUMPAD4   0x64
#define VK_NUMPAD5   0x65
#define VK_NUMPAD6   0x66
#define VK_NUMPAD7   0x67
#define VK_NUMPAD8   0x68
#define VK_NUMPAD9   0x69
#define VK_PRIOR     0x21
#define VK_RETURN    0x0D
#define VK_SPACE     0x20
#define VK_SUBTRACT  0x6D
#define VK_TAB       0x09
#define VK_UP        0x26

// Dialog-box command ids (winuser.h) and IDC_STATIC (afxres.h)
#define IDCANCEL    2
#define IDC_STATIC  -1
#define IDOK        1

// Language ids (winnt.h)
#define LANG_ENGLISH         0x09
#define SUBLANG_ENGLISH_AUS  0x03

// MFC's command ids (afxres.h)
#define ID_APP_ABOUT           0xE140
#define ID_APP_EXIT            0xE141
#define ID_CONTEXT_HELP        0xE145
#define ID_EDIT_CLEAR          0xE120
#define ID_EDIT_CLEAR_ALL      0xE121
#define ID_EDIT_COPY           0xE122
#define ID_EDIT_CUT            0xE123
#define ID_EDIT_FIND           0xE124
#define ID_EDIT_PASTE          0xE125
#define ID_EDIT_REDO           0xE12C
#define ID_EDIT_REPEAT         0xE128
#define ID_EDIT_REPLACE        0xE129
#define ID_EDIT_SELECT_ALL     0xE12A
#define ID_EDIT_UNDO           0xE12B
#define ID_FILE_CLOSE          0xE102
#define ID_FILE_MRU_FILE1      0xE110
#define ID_FILE_MRU_FILE2      0xE111
#define ID_FILE_MRU_FILE3      0xE112
#define ID_FILE_MRU_FILE4      0xE113
#define ID_FILE_NEW            0xE100
#define ID_FILE_OPEN           0xE101
#define ID_FILE_PRINT          0xE107
#define ID_FILE_PRINT_PREVIEW  0xE109
#define ID_FILE_PRINT_SETUP    0xE106
#define ID_FILE_SAVE           0xE103
#define ID_FILE_SAVE_AS        0xE104
#define ID_HELP                0xE146
#define ID_HELP_INDEX          0xE142
#define ID_HELP_USING          0xE144
#define ID_INDICATOR_CAPS      0xE701
#define ID_INDICATOR_EXT       0xE700
#define ID_INDICATOR_NUM       0xE702
#define ID_INDICATOR_OVR       0xE704
#define ID_INDICATOR_REC       0xE705
#define ID_INDICATOR_SCRL      0xE703
#define ID_NEXT_PANE           0xE150
#define ID_PREV_PANE           0xE151
#define ID_VIEW_STATUS_BAR     0xE801
#define ID_VIEW_TOOLBAR        0xE800
#define ID_WINDOW_ARRANGE      0xE131
#define ID_WINDOW_CASCADE      0xE132
#define ID_WINDOW_NEW          0xE130
#define ID_WINDOW_SPLIT        0xE135
#define ID_WINDOW_TILE_HORZ    0xE133
#define ID_WINDOW_TILE_VERT    0xE134

// MFC's own resources: dialogs, controls, cursors, bitmaps (afxres.h)
#define AFX_IDB_CHECKLISTBOX_95    30996
#define AFX_IDB_MINIFRAME_MENU     30994
#define AFX_IDC_CONTEXTHELP        30977
#define AFX_IDC_HSPLITBAR          30980
#define AFX_IDC_LISTBOX            100
#define AFX_IDC_MAGNIFY            30978
#define AFX_IDC_MOVE4WAY           30988
#define AFX_IDC_NODROPCRSR         30982
#define AFX_IDC_PRINT_DOCNAME      201
#define AFX_IDC_PRINT_PAGENUM      204
#define AFX_IDC_PRINT_PORTNAME     203
#define AFX_IDC_PRINT_PRINTERNAME  202
#define AFX_IDC_SMALLARROWS        30979
#define AFX_IDC_TRACK4WAY          30987
#define AFX_IDC_TRACKNESW          30984
#define AFX_IDC_TRACKNS            30985
#define AFX_IDC_TRACKNWSE          30983
#define AFX_IDC_TRACKWE            30986
#define AFX_IDC_VSPLITBAR          30981
#define AFX_IDD_NEWTYPEDLG         30721
#define AFX_IDD_PREVIEW_TOOLBAR    30723
#define AFX_IDD_PRINTDLG           30722
#define AFX_IDR_PREVIEW_ACCEL      30997
#define AFX_ID_PREVIEW_CLOSE       0xE300
#define AFX_ID_PREVIEW_NEXT        0xE302
#define AFX_ID_PREVIEW_NUMPAGE     0xE301
#define AFX_ID_PREVIEW_PREV        0xE303
#define AFX_ID_PREVIEW_PRINT       0xE304
#define AFX_ID_PREVIEW_ZOOMIN      0xE305
#define AFX_ID_PREVIEW_ZOOMOUT     0xE306

// MFC's prompt strings (afxres.h)
#define AFX_IDP_ARCH_BADCLASS            0xF1B6
#define AFX_IDP_ARCH_BADINDEX            0xF1B5
#define AFX_IDP_ARCH_BADSCHEMA           0xF1B7
#define AFX_IDP_ARCH_ENDOFFILE           0xF1B3
#define AFX_IDP_ARCH_GENERIC             0xF1B1
#define AFX_IDP_ARCH_NONE                0xF1B0
#define AFX_IDP_ARCH_READONLY            0xF1B2
#define AFX_IDP_ARCH_WRITEONLY           0xF1B4
#define AFX_IDP_ASK_TO_SAVE              0xF103
#define AFX_IDP_ASK_TO_UPDATE            0xF187
#define AFX_IDP_COMMAND_FAILURE          0xF109
#define AFX_IDP_DLL_BAD_VERSION          0xF10E
#define AFX_IDP_DLL_LOAD_FAILED          0xF10D
#define AFX_IDP_FAILED_ACCESS_READ       0xF123
#define AFX_IDP_FAILED_ACCESS_WRITE      0xF124
#define AFX_IDP_FAILED_DISK_FULL         0xF122
#define AFX_IDP_FAILED_INVALID_FORMAT    0xF120
#define AFX_IDP_FAILED_INVALID_PATH      0xF121
#define AFX_IDP_FAILED_IO_ERROR_READ     0xF125
#define AFX_IDP_FAILED_IO_ERROR_WRITE    0xF126
#define AFX_IDP_FAILED_MAPI_LOAD         0xF190
#define AFX_IDP_FAILED_MAPI_SEND         0xF192
#define AFX_IDP_FAILED_MEMORY_ALLOC      0xF10A
#define AFX_IDP_FAILED_TO_AUTO_REGISTER  0xF18A
#define AFX_IDP_FAILED_TO_CREATE_DOC     0xF104
#define AFX_IDP_FAILED_TO_LAUNCH_HELP    0xF107
#define AFX_IDP_FAILED_TO_NOTIFY         0xF185
#define AFX_IDP_FAILED_TO_OPEN_DOC       0xF101
#define AFX_IDP_FAILED_TO_REGISTER       0xF189
#define AFX_IDP_FAILED_TO_SAVE_DOC       0xF102
#define AFX_IDP_FAILED_TO_START_PRINT    0xF106
#define AFX_IDP_FAILED_TO_UPDATE         0xF188
#define AFX_IDP_FILE_ACCESS_DENIED       0xF1A5
#define AFX_IDP_FILE_BAD_PATH            0xF1A3
#define AFX_IDP_FILE_BAD_SEEK            0xF1A9
#define AFX_IDP_FILE_DIR_FULL            0xF1A8
#define AFX_IDP_FILE_DISKFULL            0xF1AD
#define AFX_IDP_FILE_EOF                 0xF1AE
#define AFX_IDP_FILE_GENERIC             0xF1A1
#define AFX_IDP_FILE_HARD_IO             0xF1AA
#define AFX_IDP_FILE_INVALID_FILE        0xF1A6
#define AFX_IDP_FILE_LOCKING             0xF1AC
#define AFX_IDP_FILE_NONE                0xF1A0
#define AFX_IDP_FILE_NOT_FOUND           0xF1A2
#define AFX_IDP_FILE_REMOVE_CURRENT      0xF1A7
#define AFX_IDP_FILE_SHARING             0xF1AB
#define AFX_IDP_FILE_TOO_LARGE           0xF105
#define AFX_IDP_FILE_TOO_MANY_OPEN       0xF1A4
#define AFX_IDP_GET_NOT_SUPPORTED        0xF18C
#define AFX_IDP_INTERNAL_FAILURE         0xF108
#define AFX_IDP_INVALID_FILENAME         0xF100
#define AFX_IDP_INVALID_MAPI_DLL         0xF191
#define AFX_IDP_NO_ERROR_AVAILABLE       0xF020
#define AFX_IDP_PARSE_BYTE               0xF116
#define AFX_IDP_PARSE_CURRENCY           0xF119
#define AFX_IDP_PARSE_DATETIME           0xF118
#define AFX_IDP_PARSE_INT                0xF110
#define AFX_IDP_PARSE_INT_RANGE          0xF112
#define AFX_IDP_PARSE_RADIO_BUTTON       0xF115
#define AFX_IDP_PARSE_REAL               0xF111
#define AFX_IDP_PARSE_REAL_RANGE         0xF113
#define AFX_IDP_PARSE_STRING_SIZE        0xF114
#define AFX_IDP_PARSE_UINT               0xF117
#define AFX_IDP_SET_NOT_SUPPORTED        0xF18D
#define AFX_IDP_UNREG_DONE               0xF10B
#define AFX_IDP_UNREG_FAILURE            0xF10C

// MFC's strings (afxres.h)
#define AFX_IDS_ALLFILTER                0xF002
#define AFX_IDS_APP_TITLE                0xE000
#define AFX_IDS_APP_TITLE_EMBEDDING      0xE003
#define AFX_IDS_EXIT_MENU                0xF08C
#define AFX_IDS_HELPMODEMESSAGE          0xE002
#define AFX_IDS_HIDE                     0xF011
#define AFX_IDS_IDLEMESSAGE              0xE001
#define AFX_IDS_MDICHILD                 0xEF1F
#define AFX_IDS_MEMORY_EXCEPTION         0xF023
#define AFX_IDS_NOT_DOCOBJECT            0xF184
#define AFX_IDS_NOT_SUPPORTED_EXCEPTION  0xF021
#define AFX_IDS_OBJ_TITLE_INPLACE        0xE005
#define AFX_IDS_OCC_SCALEUNITS_PIXELS    0xF1C0
#define AFX_IDS_ONEPAGE                  0xF041
#define AFX_IDS_OPENFILE                 0xF000
#define AFX_IDS_PASTELINKEDTYPE          0xF094
#define AFX_IDS_PREVIEWPAGEDESC          0xF044
#define AFX_IDS_PRINTCAPTION             0xF048
#define AFX_IDS_PRINTDEFAULT             0xF046
#define AFX_IDS_PRINTDEFAULTEXT          0xF045
#define AFX_IDS_PRINTFILTER              0xF047
#define AFX_IDS_PRINTONPORT              0xF040
#define AFX_IDS_PRINTPAGENUM             0xF043
#define AFX_IDS_PRINTTOFILE              0xF049
#define AFX_IDS_RESOURCE_EXCEPTION       0xF022
#define AFX_IDS_SAVEFILE                 0xF001
#define AFX_IDS_SAVEFILECOPY             0xF004
#define AFX_IDS_SCCLOSE                  0xEF06
#define AFX_IDS_SCMAXIMIZE               0xEF03
#define AFX_IDS_SCMINIMIZE               0xEF02
#define AFX_IDS_SCMOVE                   0xEF01
#define AFX_IDS_SCNEXTWINDOW             0xEF04
#define AFX_IDS_SCPREVWINDOW             0xEF05
#define AFX_IDS_SCRESTORE                0xEF12
#define AFX_IDS_SCSIZE                   0xEF00
#define AFX_IDS_SCTASKLIST               0xEF13
#define AFX_IDS_TWOPAGE                  0xF042
#define AFX_IDS_UNKNOWNTYPE              0xF095
#define AFX_IDS_UNNAMED_FILE             0xF006
#define AFX_IDS_UNTITLED                 0xF003
#define AFX_IDS_UPDATE_MENU              0xF089
#define AFX_IDS_USER_EXCEPTION           0xF024

#endif // MC_LLVM_RC_AFXRES_H

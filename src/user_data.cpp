// user_data.cpp -- where MUSHclient 6 keeps the files it writes, on Windows.
// See user_data.h.

#include "stdafx.h"
#include "MUSHclient.h"
#include "user_data.h"
#include "legacy_import.h"
#include "bundled_files.h"
#include "version.h"     // VERSION_STRING: the seeded-version marker

#ifdef _WIN32

#include <shlobj.h>
#include <fstream>
#include <sstream>

namespace {

CString s_strProgramDir;
CString s_strDataDir;
bool    s_bPortable = true;     // until UserData_SetUp says otherwise: beside the exe

CString WithSlash (CString s)
  {
  if (!s.IsEmpty () && s.Right (1) != "\\" && s.Right (1) != "/")
    s += "\\";
  return s;
  }

bool FileExists (const CString & s)
  {
  const DWORD a = GetFileAttributes (s);
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
  }

void MakeDirs (CString strDir)
  {
  while (strDir.GetLength () > 3 && (strDir.Right (1) == "\\" || strDir.Right (1) == "/"))
    strDir = strDir.Left (strDir.GetLength () - 1);
  if (GetFileAttributes (strDir) == INVALID_FILE_ATTRIBUTES)
    SHCreateDirectoryEx (NULL, strDir, NULL);
  }

CString KnownFolder (int csidl)
  {
  char path [MAX_PATH];
  if (FAILED (SHGetFolderPath (NULL, csidl | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, path)))
    return "";
  return WithSlash (path);
  }

// The program's folder playing a 5.x's: the defaults, and the bundled files
// copied out of it, are written as 5.x's were, relative to its folder.
legacy::Install ProgramAsInstall ()
  {
  legacy::Install program;
  program.folder = program.windowsFolder = (LPCTSTR) s_strProgramDir;
  return program;
  }

std::wstring Wide (const std::string & s)
  {
  const int n = MultiByteToWideChar (CP_ACP, 0, s.c_str (), -1, NULL, 0);
  std::wstring w (n > 0 ? n - 1 : 0, L'\0');
  if (n > 1)
    MultiByteToWideChar (CP_ACP, 0, s.c_str (), -1, &w [0], n);
  return w;
  }

// comctl32's task dialog, as commctrl.h declares it for Vista and later --
// which it does not here, the project being built for _WIN32_WINNT 0x0501.
// Same layout, packed as commctrl.h packs it; the function is looked up at
// run time, so nothing links against what XP lacked.
#include <pshpack1.h>
struct McTaskDialogButton
  {
  int     nButtonID;
  PCWSTR  pszButtonText;
  };
struct McTaskDialogConfig
  {
  UINT      cbSize;
  HWND      hwndParent;
  HINSTANCE hInstance;
  int       dwFlags;
  int       dwCommonButtons;
  PCWSTR    pszWindowTitle;
  union { HICON hMainIcon; PCWSTR pszMainIcon; };
  PCWSTR    pszMainInstruction;
  PCWSTR    pszContent;
  UINT      cButtons;
  const McTaskDialogButton * pButtons;
  int       nDefaultButton;
  UINT      cRadioButtons;
  const McTaskDialogButton * pRadioButtons;
  int       nDefaultRadioButton;
  PCWSTR    pszVerificationText;
  PCWSTR    pszExpandedInformation;
  PCWSTR    pszExpandedControlText;
  PCWSTR    pszCollapsedControlText;
  union { HICON hFooterIcon; PCWSTR pszFooterIcon; };
  PCWSTR    pszFooter;
  void *    pfCallback;
  LONG_PTR  lpCallbackData;
  UINT      cxWidth;
  };
#include <poppack.h>
const int    kTdfUseCommandLinks = 0x0010;                 // TDF_USE_COMMAND_LINKS
const PCWSTR kTdInformationIcon  = MAKEINTRESOURCEW (-3);  // TD_INFORMATION_ICON

// A bundled file the user changed, and this version has a new one of: ask.
// A task dialog, for its "don't ask again" box; comctl32's, looked up rather
// than linked, with a plain Yes/No for want of it.  Before the main window
// exists, so owned by nothing.
bundled::Answer AskAboutChangedFile (const std::string & relative, const std::string & name)
  {
  const bool plugin = relative.find ("plugins\\") != std::string::npos &&
                      relative.size () > 4 &&
                      _stricmp (relative.c_str () + relative.size () - 4, ".xml") == 0;
  const std::string file = relative.substr (relative.find_last_of ('\\') + 1);
  const std::string what = plugin ? "plugin" : "file";
  const std::string content =
    "MUSHclient " VERSION_STRING " comes with a new version of " +
    (name == relative ? relative : "\"" + name + "\" (" + relative + ")") +
    ", but your copy has been changed since MUSHclient installed it.\n\n"
    "If you upgrade it, your copy is kept beside it, as " + file + ".backup.";

  bundled::Answer answer;
  typedef HRESULT (WINAPI * TaskDialogIndirectFn) (const McTaskDialogConfig *, int *, int *, BOOL *);
  HMODULE comctl = GetModuleHandle ("comctl32.dll");
  if (!comctl)
    comctl = LoadLibrary ("comctl32.dll");
  const TaskDialogIndirectFn taskDialog = comctl
    ? (TaskDialogIndirectFn) GetProcAddress (comctl, "TaskDialogIndirect") : NULL;
  if (!taskDialog)
    {
    answer.upgrade = ::MessageBox (NULL, (content + "\n\nUpgrade it?").c_str (), "MUSHclient",
                                   MB_YESNO | MB_ICONQUESTION) == IDYES;
    return answer;
    }

  const std::wstring title       = L"MUSHclient";
  const std::wstring instruction = Wide ("A " + what + " you changed has a new version");
  const std::wstring body        = Wide (content);
  const std::wstring upgrade     = L"Upgrade to the new version";
  const std::wstring keep        = L"Keep my copy";
  const std::wstring dontAsk     = Wide ("Don't ask again about this " + what);
  const McTaskDialogButton buttons [] = { { IDYES, upgrade.c_str () }, { IDNO, keep.c_str () } };

  McTaskDialogConfig config = {};
  config.cbSize             = sizeof config;
  config.dwFlags            = kTdfUseCommandLinks;
  config.pszWindowTitle     = title.c_str ();
  config.pszMainIcon        = kTdInformationIcon;
  config.pszMainInstruction = instruction.c_str ();
  config.pszContent         = body.c_str ();
  config.cButtons           = sizeof buttons / sizeof buttons [0];
  config.pButtons           = buttons;
  config.nDefaultButton     = IDNO;
  config.pszVerificationText = dontAsk.c_str ();
  int button = IDNO;
  BOOL remember = FALSE;
  if (SUCCEEDED (taskDialog (&config, &button, NULL, &remember)))
    {
    answer.upgrade  = button == IDYES;
    answer.remember = remember != FALSE;
    }
  return answer;
  }

// The bundled plugins and worlds, and the Discworld edition's mapper and its
// maps, are the user's to change -- the mapper even writes its own database
// beside its maps -- so they go into the data folder, and are kept up to date
// there (bundled_files.h).  Once per version: that is when they can change.
void Seed ()
  {
  const CString strMarker = s_strDataDir + "seeded-version.txt";
  std::string seeded;
    {
    std::ifstream f ((LPCTSTR) strMarker, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf ();
    seeded = ss.str ();
    }
  if (seeded == VERSION_STRING)
    return;
  // World files are translated on the way, as an import's are.  The bundled
  // ones hold only relative paths, which this leaves alone -- but 6.00
  // installed into a 5.x's own folder finds that 5.x's worlds here too, and
  // they hold paths into this folder that belong in the data folder now.
  const legacy::Install program = ProgramAsInstall ();
  const std::string data = (LPCTSTR) s_strDataDir, prog = (LPCTSTR) s_strProgramDir;
  bundled::Sync (prog, data, { "worlds", "quow_plugins" }, data + "bundled-files.txt",
                 AskAboutChangedFile,
                 [&] (const std::string & text) {
                   return legacy::TranslateText (text, program, data, prog);
                 });
  MakeDirs (s_strDataDir + "worlds\\plugins\\state");
  MakeDirs (s_strDataDir + "logs");
  MakeDirs (s_strDataDir + "sounds");
  std::ofstream ((LPCTSTR) strMarker, std::ios::binary | std::ios::trunc) << VERSION_STRING;
  }

}   // namespace

void UserData_SetUp (const CString & strProgramDirectory)
  {
  s_strProgramDir = WithSlash (strProgramDirectory);
  s_bPortable = FileExists (s_strProgramDir + "portable.txt");
  if (!s_bPortable)
    {
    const CString strAppData = KnownFolder (CSIDL_APPDATA);
    if (strAppData.IsEmpty ())
      s_bPortable = true;   // nowhere else to go
    else
      s_strDataDir = strAppData + "MUSHclient\\";
    }
  if (s_bPortable)
    s_strDataDir = s_strProgramDir;
  else
    {
    MakeDirs (s_strDataDir);
    Seed ();
    }
  SetCurrentDirectory (s_strDataDir);
  }

bool UserData_IsPortable ()
  {
  return s_bPortable;
  }

const CString & UserData_Directory ()
  {
  return s_strDataDir;
  }

const CString & UserData_ProgramDirectory ()
  {
  return s_strProgramDir;
  }

CString UserData_DefaultPreference (const char * pName, const CString & strDefault)
  {
  if (!legacy::IsPathPreference (pName) || s_strProgramDir.IsEmpty ())
    return strDefault;
  return legacy::TranslatePath ((LPCTSTR) strDefault, ProgramAsInstall (),
                                (LPCTSTR) s_strDataDir, (LPCTSTR) s_strProgramDir).c_str ();
  }

#endif  // _WIN32

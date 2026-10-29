// user_data.cpp -- where MUSHclient 6 keeps the files it writes, on Windows.
// See user_data.h.

#include "stdafx.h"
#include "MUSHclient.h"
#include "user_data.h"
#include "legacy_import.h"
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

// The bundled plugins and worlds, and the Discworld edition's mapper and its
// maps, are the user's to change -- the mapper even writes its own database
// beside its maps -- so they go into the data folder.  Once per version, so a
// file the user deleted stays deleted until an upgrade; and never over a file
// already there, so their changes survive one.
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
  legacy::CopyTree (prog + "worlds",       data + "worlds",       &program, data, prog);
  legacy::CopyTree (prog + "quow_plugins", data + "quow_plugins", &program, data, prog);
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

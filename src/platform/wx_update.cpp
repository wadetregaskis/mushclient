// wx_update.cpp -- see wx_update.h.
//
// How a copy replaces itself here, unlike Windows' portable swap: user data isn't
// beside the program (~/Library/Application Support/MUSHclient, ~/.local/share/MUSHclient),
// so the whole of it can go at once.  A running program's files can be renamed from under
// it on both systems, so the old .app bundle or tarball folder is moved aside, the new one
// moved into its place, and a small script waits for this process to end, removes the old
// one and starts the new one.  A .deb, or a copy somewhere it can't write, doesn't update
// itself and goes to the release page.

#include <wx/wx.h>
#include <wx/stdpaths.h>
#include <wx/gauge.h>
#include <wx/filename.h>

#include "../stdafx.h"
#include "../MUSHclient.h"

#include "wx_update.h"
#include "compat.h"
#include "../update_key.h"
#include "../update_service.h"
#include "../version.h"

#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include <unistd.h>

namespace fs = std::filesystem;

namespace {

// Where the copy is.  On macOS, the .app; on Linux, the folder the tarball unpacked to.
fs::path InstallRoot ()
  {
  const fs::path exe = fs::u8path (std::string (wxStandardPaths::Get ().GetExecutablePath ().utf8_str ()));
#ifdef __APPLE__
  for (fs::path p = exe; p.has_parent_path () && p != p.parent_path (); p = p.parent_path ())
    if (p.extension () == ".app")
      return p;
  return fs::path ();
#else
  return exe.parent_path ();
#endif
  }

update::ThisCopy DescribeThisCopy ()
  {
  update::ThisCopy copy;
#ifdef __APPLE__
  copy.platform = update::Platform::MacOS;
  copy.arch = "universal";
#else
  copy.platform = update::Platform::Linux;
#if defined(__x86_64__)
  copy.arch = "x86_64";
#elif defined(__aarch64__)
  copy.arch = "arm64";
#endif
#endif

  const fs::path root = InstallRoot ();
  std::error_code ec;
#ifdef __APPLE__
  copy.discworld = fs::exists (root / "Contents" / "Resources" / update::kEditionMarker, ec);
#else
  copy.discworld = fs::exists (root / update::kEditionMarker, ec);
#endif

  // A package manager's, or somewhere the folder can't be replaced from.
  const std::string where = root.u8string ();
  copy.systemManaged = root.empty () || where.compare (0, 5, "/usr/") == 0 || where.compare (0, 5, "/opt/") == 0 ||
                       access (root.parent_path ().u8string ().c_str (), W_OK) != 0;

  // The whole folder is replaced, and the old one removed: so it has to be one of ours,
  // as the package unpacked it, and not wherever the program was copied to (~/bin).
  const char * home = getenv ("HOME");
  const bool bHome = home && *home && fs::equivalent (root, fs::u8path (home), ec);
  copy.foreignFolder = !copy.systemManaged &&
                       (bHome || root == root.root_path () ||
                        !update::LooksLikeOurFolder (where, copy.platform));
  return copy;
  }

std::string UpdatesFolder ()
  {
  return PortableUserDataDir () + "/updates";
  }

// For putting a path in a shell command.
std::string Quoted (const std::string & text)
  {
  std::string out = "'";
  for (const char c : text)
    {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
    }
  return out + "'";
  }

// On a worker.  True if the new copy is in place, and will start when this one ends;
// *notice, if not empty, is something they should be told even so.
bool ReplaceThisCopy (const std::string & package, const std::string & sha256, std::string * error,
                      std::string * notice)
  {
  const fs::path root = InstallRoot ();
#ifdef __APPLE__
  const update::Platform platform = update::Platform::MacOS;
#else
  const update::Platform platform = update::Platform::Linux;
#endif
  if (!update::LooksLikeOurFolder (root.u8string (), platform))
    {
    *error = "the folder this copy is in doesn't look like one of MUSHclient's own";
    return false;
    }

  // Once more, just before it's unpacked: the file has sat in a folder since it was checked.
  std::string actual;
  if (!update::HashFile (package, &actual, error) || actual != sha256)
    {
    *error = "the update changed after it was checked";
    return false;
    }

  const fs::path stage = fs::u8path (UpdatesFolder ()) / "staging";
  std::error_code ec;
  fs::remove_all (stage, ec);
  fs::create_directories (stage, ec);

#ifdef __APPLE__
  // ditto rather than unzip: frameworks are full of symlinks.
  const std::string unpack = "/usr/bin/ditto -x -k " + Quoted (package) + " " + Quoted (stage.u8string ());
#else
  const std::string unpack = "tar -xJf " + Quoted (package) + " -C " + Quoted (stage.u8string ());
#endif
  if (system (unpack.c_str ()) != 0)
    {
    *error = "could not unpack the update";
    return false;
    }

  // The one thing that came out of it: MUSHclient.app, or mushclient/.
  fs::path unpacked;
  for (fs::directory_iterator it (stage, ec), end; !ec && it != end; it.increment (ec))
    if (it->is_directory (ec) && !unpacked.empty ())
      {
      *error = "the update doesn't look as it should";
      return false;
      }
    else if (it->is_directory (ec))
      unpacked = it->path ();
  if (unpacked.empty () || !update::LooksLikeOurFolder (unpacked.u8string (), platform))
    {
    *error = "the update doesn't look as it should";
    return false;
    }

  // What the last update left, if its script didn't get to it.  Only if it's ours.
  fs::path old = root;
  old += ".mcold";
  if (fs::exists (fs::symlink_status (old, ec)))
    {
    if (!update::LooksLikeOurFolder (old.u8string (), platform))
      {
      *error = "there is already something called " + old.u8string () + ", in the way";
      return false;
      }
    fs::remove_all (old, ec);
    }
  fs::rename (root, old, ec);
  if (ec)
    {
    *error = "could not move this copy aside: " + ec.message ();
    return false;
    }
  fs::rename (unpacked, root, ec);
  if (ec)
    {
    std::error_code ignore;
    fs::rename (old, root, ignore);    // put it back
    *error = "could not put the update in place: " + ec.message ();
    return false;
    }
  fs::remove_all (stage, ec);
  fs::remove (fs::u8path (package), ec);

  // Whatever they added to the tarball's folder comes too, as on Windows.  (Not into a
  // .app, where anything added would break its signature.)  If any of it couldn't be
  // moved, the old folder stays, with it: renamed out of the way of the next update,
  // which removes <root>.mcold as its own leftover, and would take their files with it.
  bool bRemoveOld = true;
#ifndef __APPLE__
  std::string why;
  if (!update::CarryOverUserFiles (old.u8string (), root.u8string (), &why))
    {
    bRemoveOld = false;
    char stamp [32] = "";
    const time_t now = time (NULL);
    struct tm local;
    if (localtime_r (&now, &local))
      strftime (stamp, sizeof stamp, "%Y%m%d-%H%M%S", &local);
    fs::path kept = root;
    kept += std::string (".mcold-kept-") + stamp;
    std::error_code renameError;
    fs::rename (old, kept, renameError);
    const std::string start = "Some of what was in the old folder couldn't be moved to the new one (" + why + "), ";
    if (renameError)
      *notice = start + "so the old one has been kept, as " + old.u8string () +
                ".  Move it somewhere else before the next update, which would remove it.";
    else
      *notice = start + "so the old one has been kept, as " + kept.u8string () +
                ".  Updates leave it alone: remove it yourself once you have what you need from it.";
    }
#endif

  // When this process has gone, clear up and start the new one.
  const fs::path script = fs::u8path (UpdatesFolder ()) / "finish-update.sh";
    {
    std::ofstream out (script);
    out << "#!/bin/sh\n"
        << "while kill -0 " << (long) getpid () << " 2>/dev/null; do sleep 1; done\n";
    if (bRemoveOld)
      out << "rm -rf " << Quoted (old.u8string ()) << "\n";
#ifdef __APPLE__
    out << "open " << Quoted (root.u8string ()) << "\n";
#else
    out << "exec " << Quoted ((root / "mushclient.sh").u8string ()) << "\n";
#endif
    }
  const std::string finish = "nohup /bin/sh " + Quoted (script.u8string ()) + " >/dev/null 2>&1 &";
  if (system (finish.c_str ()) != 0)
    {
    *error = "the update is in place, but MUSHclient will have to be started again by hand";
    return false;
    }
  return true;
  }

class CUpdateDialog : public wxDialog
  {
  public:
  explicit CUpdateDialog (wxWindow * parent)
      : wxDialog (parent, wxID_ANY, wxT ("Update MUSHclient"), wxDefaultPosition, wxSize (640, 520),
                  wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
        m_pAlive (std::make_shared<bool> (true))
    {
    update::Controller & controller = update::Controller::Get ();
    const update::Offer offer = update::ChooseOffer (controller.Newer (), DescribeThisCopy (), update::kUpdatePublicKey);
    m_latest = offer.release;
    m_plan = offer.plan;

    wxBoxSizer * sizer = new wxBoxSizer (wxVERTICAL);
    sizer->Add (new wxStaticText (this, wxID_ANY, wxString::FromUTF8 (
                    ("You have version " + controller.InstalledVersion () + ".  The latest is " +
                     controller.LatestVersion () + ".").c_str ())), 0, wxALL, 10);
    sizer->Add (new wxStaticText (this, wxID_ANY, wxT ("What's new:")), 0, wxLEFT | wxRIGHT, 10);
    sizer->Add (new wxTextCtrl (this, wxID_ANY,
                                wxString::FromUTF8 (update::ConcatenateNotes (controller.Newer ()).c_str ()),
                                wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY),
                1, wxEXPAND | wxALL, 10);
    m_status = new wxStaticText (this, wxID_ANY, wxEmptyString);
    sizer->Add (m_status, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
    m_gauge = new wxGauge (this, wxID_ANY, 100);
    m_gauge->Hide ();
    sizer->Add (m_gauge, 0, wxEXPAND | wxALL, 10);

    wxBoxSizer * row = new wxBoxSizer (wxHORIZONTAL);
    m_dontAsk = new wxCheckBox (this, wxID_ANY, wxT ("Don't ask me again"));
    m_dontAsk->SetValue (App.m_bCheckForUpdates == 0);
    row->Add (m_dontAsk, 0, wxALIGN_CENTER_VERTICAL);
    row->AddStretchSpacer ();
    m_install = new wxButton (this, wxID_ANY, m_plan.canInstall ? wxT ("Download and Install") : wxT ("Open Release Page"));
    m_install->SetDefault ();
    row->Add (m_install, 0, wxRIGHT, 8);
    m_later = new wxButton (this, wxID_CANCEL, wxT ("Remind Me Later"));
    row->Add (m_later);
    sizer->Add (row, 0, wxEXPAND | wxALL, 10);
    SetSizer (sizer);

    if (!m_plan.canInstall)
      SetStatus (m_plan.why + "  The release page has it.");
    else if (!offer.note.empty ())
      SetStatus (offer.note);

    m_install->Bind (wxEVT_BUTTON, &CUpdateDialog::OnInstall, this);
    m_later->Bind (wxEVT_BUTTON, &CUpdateDialog::OnLater, this);
    Bind (wxEVT_CLOSE_WINDOW, &CUpdateDialog::OnCloseWindow, this);
    }

  private:
  void SetStatus (const std::string & text)
    {
    m_status->SetLabel (wxString::FromUTF8 (text.c_str ()));
    Layout ();
    }

  void Busy (const bool bBusy)
    {
    m_bBusy = bBusy;
    m_install->Enable (!bBusy);
    m_dontAsk->Enable (!bBusy);
    m_gauge->Show (bBusy);
    m_later->SetLabel (bBusy ? wxT ("Cancel") : wxT ("Remind Me Later"));
    Layout ();
    }

  void Finish (const int result)
    {
    const bool bOff = m_dontAsk->GetValue ();
    if ((App.m_bCheckForUpdates == 0) != bOff)
      {
      App.m_bCheckForUpdates = bOff ? 0 : 1;
      App.SaveGlobalsToDatabase ();
      }
    update::Controller::Get ().DismissBadge ();
    *m_pAlive = false;
    EndModal (result);
    }

  void OnLater (wxCommandEvent &)
    {
    Cancel ();
    }

  void OnCloseWindow (wxCloseEvent &)
    {
    Cancel ();
    }

  void Cancel ()
    {
    if (m_bBusy)
      {
      m_bCancelling = true;
      update::Controller::Get ().CancelDownload ();
      SetStatus ("Cancelling...");
      m_later->Enable (false);
      return;
      }
    Finish (wxID_CANCEL);
    }

  void OnInstall (wxCommandEvent &)
    {
    if (!m_plan.canInstall)
      {
      wxLaunchDefaultBrowser (wxString::FromUTF8 (update::ReleasePageFor (m_latest).c_str ()));
      Finish (wxID_OK);
      return;
      }

    Busy (true);
    SetStatus ("Downloading...");
    std::shared_ptr<bool> alive = m_pAlive;
    update::Controller::Get ().StartDownload (
        m_latest, DescribeThisCopy (), update::kUpdatePublicKey, UpdatesFolder (),
        [this, alive] (uint64_t done, uint64_t total)
          {
          if (total)
            {
            const int percent = (int) (done * 100 / total);
            wxTheApp->CallAfter ([this, alive, percent]
              {
              if (*alive)
                m_gauge->SetValue (percent);
              });
            }
          return true;
          },
        [this, alive] (bool bOK, const std::string & pathOrError, const std::string & sha256)
          {
          if (*alive)
            DownloadFinished (bOK, pathOrError, sha256);
          });
    }

  void DownloadFinished (const bool bOK, const std::string & pathOrError, const std::string & sha256)
    {
    Busy (false);
    m_later->Enable (true);
    if (m_bCancelling)
      {
      Finish (wxID_CANCEL);
      return;
      }
    if (!bOK)
      {
      SetStatus ("Could not download it: " + pathOrError);
      return;
      }

    Busy (true);
    m_later->Enable (false);
    SetStatus ("Installing...");
    const std::string package = pathOrError;
    std::shared_ptr<bool> alive = m_pAlive;
    std::thread ([this, alive, package, sha256]
      {
      std::string error, notice;
      const bool bDone = ReplaceThisCopy (package, sha256, &error, &notice);
      wxTheApp->CallAfter ([this, alive, bDone, error, notice]
        {
        if (*alive)
          InstallFinished (bDone, error, notice);
        });
      }).detach ();
    }

  void InstallFinished (const bool bDone, const std::string & error, const std::string & notice)
    {
    Busy (false);
    m_later->Enable (true);
    if (!bDone)
      {
      SetStatus ("Could not install it: " + error);
      return;
      }
    wxMessageBox (wxString::FromUTF8 (("MUSHclient " + m_latest.version +
                                       " is ready, and will start when this one has closed." +
                                       (notice.empty () ? std::string () : "\n\n" + notice)).c_str ()),
                  wxT ("MUSHclient"), wxOK | wxICON_INFORMATION, this);
    Finish (wxID_OK);
    // The ordinary way, questions included.
    if (wxWindow * top = wxTheApp->GetTopWindow ())
      top->Close ();
    }

  update::Release m_latest;
  update::InstallPlan m_plan;
  wxStaticText * m_status = nullptr;
  wxGauge * m_gauge = nullptr;
  wxCheckBox * m_dontAsk = nullptr;
  wxButton * m_install = nullptr;
  wxButton * m_later = nullptr;
  bool m_bBusy = false;
  bool m_bCancelling = false;
  std::shared_ptr<bool> m_pAlive;
  };

class CUpdateTimer : public wxTimer
  {
  public:
  void Notify () override
    {
    update::Controller::Get ().Tick (time (NULL), App.m_bCheckForUpdates != 0);
    }
  };

// The one that's up, if one is: there is only ever one.
CUpdateDialog * g_pOpenDialog = nullptr;

void Checked (const bool bUserAsked, const update::CheckResult & result)
  {
  if (!bUserAsked)
    return;    // the status bar says it, if there's anything to say

  if (g_pOpenDialog)
    {
    g_pOpenDialog->Raise ();     // it says what there is to say
    return;
    }

  if (!result.ok)
    wxMessageBox (wxString::FromUTF8 (("Could not check for a new version of MUSHclient: " + result.error).c_str ()),
                  wxT ("MUSHclient"), wxOK | wxICON_WARNING);
  else if (result.newer.empty ())
    wxMessageBox (wxString::FromUTF8 ((std::string ("You have version ") + VERSION_STRING +
                                       " of MUSHclient, which is the latest.").c_str ()),
                  wxT ("MUSHclient"), wxOK | wxICON_INFORMATION);
  else
    WxUpdate_OpenDialog (wxTheApp->GetTopWindow ());
  }

} // anonymous namespace

void WxUpdate_Start ()
  {
  update::Controller & controller = update::Controller::Get ();
  controller.Configure ([] (std::function<void ()> work)
    {
    if (!wxTheApp)
      return false;
    wxTheApp->CallAfter ([work] { work (); });
    return true;
    }, VERSION_STRING);
  controller.SetOnChecked (Checked);

  static CUpdateTimer timer;
  timer.Start (1000);
  }

void WxUpdate_CheckNow ()
  {
  update::Controller::Get ().CheckNow (true);
  }

void WxUpdate_OpenDialog (wxWindow * parent)
  {
  if (g_pOpenDialog)
    {
    g_pOpenDialog->Raise ();
    return;
    }
  CUpdateDialog dialog (parent);
  g_pOpenDialog = &dialog;
  dialog.ShowModal ();
  g_pOpenDialog = nullptr;
  }

std::string WxUpdate_BadgeText ()
  {
  return update::Controller::Get ().BadgeText ();
  }

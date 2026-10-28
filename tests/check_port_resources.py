#!/usr/bin/env python3
"""Guard: the macOS/Linux port sees exactly the resources Windows does.

Why this exists
----------------
resources/en.rc is compiled twice, by two different compilers: rc.exe builds
it into locale/en.dll for Windows (resources/en.vcxproj), and llvm-rc builds
it for the port (resources/CMakeLists.txt), against resources/llvm-rc/afxres.h
in place of the Windows SDK's headers.  tools/port_resources.py then turns the
port's .res into the dialog text and art the wx build bundles.

The two compilers can disagree without either failing: a constant in
afxres.h with the wrong value, an identifier the shim lacks that happens to
be guarded out, a statement llvm-rc reads differently.  The port would build
and run with a dialog that is not the one Windows shows.  So, on Windows,
where both compilers are available, this compiles en.rc with llvm-rc exactly
as the port's build does, runs tools/port_resources.py on that and on rc.exe's
own en.res, and requires the two results to be identical, file for file and
byte for byte.

Usage: check_port_resources.py <rc.exe's en.res> [llvm-rc]
  e.g. check_port_resources.py WinRel-x64/en_dll/en.res
llvm-rc is found on PATH, else in Visual Studio's LLVM component (vswhere),
else in C:\\Program Files\\LLVM\\bin.
Exit status 0 = identical, 1 = different (the differing files are printed).
"""

import filecmp
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def find_llvm_rc():
    found = shutil.which("llvm-rc")
    if found:
        return found
    candidates = []
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                           "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if os.path.exists(vswhere):
        out = subprocess.run([vswhere, "-latest", "-products", "*", "-property", "installationPath"],
                             capture_output=True, text=True).stdout.strip()
        for vs in out.splitlines():
            for arch in ("ARM64", "x64"):
                candidates.append(os.path.join(vs, "VC", "Tools", "Llvm", arch, "bin", "llvm-rc.exe"))
    candidates.append(os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"),
                                   "LLVM", "bin", "llvm-rc.exe"))
    for c in candidates:
        if os.path.exists(c):
            return c
    return None


def tree(d):
    return sorted(os.path.relpath(os.path.join(p, f), d)
                  for p, _dirs, files in os.walk(d) for f in files)


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__.split("Usage: ", 1)[1].split("\n", 1)[0])
    rcexe_res = sys.argv[1]
    llvm_rc = sys.argv[2] if len(sys.argv) == 3 else find_llvm_rc()
    if not llvm_rc:
        sys.exit("llvm-rc not found (Visual Studio's 'C++ Clang tools' component has it)")
    resh = os.path.join(ROOT, "src", "resource.h")
    tool = os.path.join(ROOT, "tools", "port_resources.py")

    with tempfile.TemporaryDirectory() as tmp:
        llvm_res = os.path.join(tmp, "en.res")
        # The same command resources/CMakeLists.txt runs; INCLUDE emptied so
        # that nothing of the SDK can stand in for the shim.
        env = dict(os.environ, INCLUDE="")
        subprocess.run([llvm_rc, "-nologo", "-C", "1252", "-D_WIN32", "-DNDEBUG",
                        "-I", os.path.join(ROOT, "resources", "llvm-rc"),
                        "-I", os.path.join(ROOT, "src"),
                        "-fo", llvm_res, os.path.join(ROOT, "resources", "en.rc")],
                       cwd=os.path.join(ROOT, "resources"), env=env, check=True)
        outs = {}
        for name, res in (("rc.exe", rcexe_res), ("llvm-rc", llvm_res)):
            outs[name] = os.path.join(tmp, name)
            subprocess.run([sys.executable, tool, res, resh, outs[name]], check=True)

        a, b = outs["rc.exe"], outs["llvm-rc"]
        ta, tb = tree(a), tree(b)
        bad = sorted(set(ta) ^ set(tb))
        bad += [f for f in ta if f in tb and not filecmp.cmp(os.path.join(a, f), os.path.join(b, f), shallow=False)]
        if bad:
            print("rc.exe and llvm-rc builds of resources/en.rc differ for the port:")
            for f in sorted(bad):
                print("  " + f)
            return 1
        print(f"port resources identical from rc.exe and llvm-rc ({len(ta)} files)")
        return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Guard: every platform ships the same set of bundled plugins.

Why this exists
---------------
The Windows installer (install/mushclient.nsi) installs a curated list of
plugins with explicit `File "..\\plugins\\X"` lines, and removes them again with
matching `Delete "$INSTDIR\\worlds\\plugins\\X"` lines.  The macOS and Linux
bundles stage the plugins listed in packaging/bundled-plugins.txt.  Three lists
of the same thing drift -- and plugins/ also holds test and sample plugins
(test.xml, error_plugin.xml, ...) that must never reach end users, so "just
ship the whole directory" is not an option.

This fails if:
  * the manifest and the installer's File lines disagree;
  * the installer's uninstaller does not Delete exactly what it installed;
  * a manifest entry does not exist in plugins/.

Usage: check_bundled_plugins.py [repo-root]
       check_bundled_plugins.py --selftest
Exit status 0 = consistent, 1 = not (differences printed).
"""

import os
import re
import sys
import tempfile

FILE_RE = re.compile(r'^\s*File\s+"\.\.\\plugins\\([^"]+)"', re.M)
DEL_RE = re.compile(r'^\s*Delete\s+"\$INSTDIR\\worlds\\plugins\\([^"\\]+)"', re.M)


def read_manifest(path):
    names = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line:
                names.append(line)
    return names


def check(root):
    errors = []
    manifest = read_manifest(os.path.join(root, "packaging", "bundled-plugins.txt"))
    with open(os.path.join(root, "install", "mushclient.nsi"), encoding="utf-8",
              errors="replace") as f:
        nsi = f.read()
    installed = FILE_RE.findall(nsi)
    deleted = DEL_RE.findall(nsi)

    def diff(a_name, a, b_name, b):
        for n in sorted(set(a) - set(b)):
            errors.append(f"{n}: in {a_name} but not in {b_name}")

    diff("bundled-plugins.txt", manifest, "mushclient.nsi File lines", installed)
    diff("mushclient.nsi File lines", installed, "bundled-plugins.txt", manifest)
    diff("mushclient.nsi File lines", installed, "mushclient.nsi uninstaller", deleted)
    diff("mushclient.nsi uninstaller", deleted, "mushclient.nsi File lines", installed)
    for n in manifest:
        if not os.path.isfile(os.path.join(root, "plugins", n)):
            errors.append(f"{n}: listed in bundled-plugins.txt but missing from plugins/")
    if len(set(manifest)) != len(manifest):
        errors.append("bundled-plugins.txt contains duplicate entries")
    return manifest, errors


def selftest():
    def make(tmp, manifest, files, deletes, on_disk):
        os.makedirs(os.path.join(tmp, "packaging"))
        os.makedirs(os.path.join(tmp, "install"))
        os.makedirs(os.path.join(tmp, "plugins"))
        with open(os.path.join(tmp, "packaging", "bundled-plugins.txt"), "w") as f:
            f.write("# comment\n\n" + "\n".join(manifest) + "\n")
        with open(os.path.join(tmp, "install", "mushclient.nsi"), "w") as f:
            for n in files:
                f.write(f'  File "..\\plugins\\{n}"\n')
            for n in deletes:
                f.write(f'  Delete "$INSTDIR\\worlds\\plugins\\{n}"\n')
        for n in on_disk:
            open(os.path.join(tmp, "plugins", n), "w").close()

    cases = [
        (["a.xml", "b.xml"], ["a.xml", "b.xml"], ["a.xml", "b.xml"], ["a.xml", "b.xml", "test.xml"], True),
        (["a.xml"], ["a.xml", "b.xml"], ["a.xml", "b.xml"], ["a.xml", "b.xml"], False),  # manifest short
        (["a.xml", "b.xml"], ["a.xml", "b.xml"], ["a.xml"], ["a.xml", "b.xml"], False),  # uninstall short
        (["a.xml", "c.xml"], ["a.xml", "c.xml"], ["a.xml", "c.xml"], ["a.xml"], False),  # missing on disk
    ]
    for i, (m, fl, dl, disk, want_ok) in enumerate(cases):
        with tempfile.TemporaryDirectory() as tmp:
            make(tmp, m, fl, dl, disk)
            _, errs = check(tmp)
            if (not errs) != want_ok:
                print(f"selftest case {i} FAILED: expected ok={want_ok}, got {errs}")
                return 1
    print("selftest passed")
    return 0


def main(argv):
    if len(argv) > 1 and argv[1] == "--selftest":
        return selftest()
    root = argv[1] if len(argv) > 1 else os.path.join(os.path.dirname(__file__), "..")
    manifest, errors = check(root)
    if errors:
        for e in errors:
            print(e)
        return 1
    print(f"bundled plugins agree across manifest, installer and uninstaller: {len(manifest)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

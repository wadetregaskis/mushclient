#!/usr/bin/env python3
"""Guard: the Windows test project compiles the same engine sources as the app.

Why this exists
---------------
`MUSHclient_tests.vcxproj` builds the unit-test suite on Windows.  It cannot
reuse `MUSHclient.vcxproj`'s object files, because it defines
`MUSHCLIENT_TESTS`, and that changes `CMUSHclientDoc`'s constructor from
protected to public -- which is part of the C++ mangled name.  A half-applied
define makes `doc.obj` reference `??0CMUSHclientDoc@@IAE@XZ` while
`doc_construct.obj` defines `??0CMUSHclientDoc@@QAE@XZ`, and the link fails on
a symbol that looks identical in both.  So the test project compiles its own
copy of every engine TU, and the two `<ClCompile>` lists have to agree.

Two lists of the same thing drift.  Drift between two definitions of "how this
builds on Windows" is precisely the defect class `docs/WINDOWS-VALIDATION.md`
exists to catch -- §10a is one: a Windows-only file used a macro that only
`doc.h` defines, and nothing off Windows could notice for eleven days.  A file
added to the app and not to the test project would produce an unresolved
external at some future Windows session, a long way from the commit that caused
it.

So: this fails if the two projects' engine source lists differ.  It is the real
guard; `tests/win/gen_test_vcxproj.ps1` is just the convenient way to fix it.

The test project additionally compiles `tests/win/all_tests.cpp` and the
`tests/test_*.cpp` files, which the app does not -- those are ignored here.
Test files are opted *out* by name in the generator, never opted in, so a new
test is built on Windows by default.

Usage: check_win_test_sources.py [repo-root]
       check_win_test_sources.py --selftest
Exit status 0 = the lists agree, 1 = they do not (differences printed).

The `--selftest` mode builds a matching pair and a diverging pair in a temp
directory and asserts the verdict on each, so that this checker cannot rot into
a vacuous pass -- the same fixture discipline the three lints beside it use.
"""

import os
import re
import shutil
import sys
import tempfile

CLCOMPILE = re.compile(r'<ClCompile\s+Include="([^"]+)"')


def sources(path):
    """Every <ClCompile Include="..."> in a vcxproj, normalised."""
    with open(path, "r", encoding="utf-8-sig", errors="replace") as f:
        text = f.read()
    out = set()
    for hit in CLCOMPILE.finditer(text):
        name = hit.group(1).replace("/", "\\")
        out.add(name)
    return out


def _write_proj(path, entries):
    with open(path, "w", encoding="utf-8") as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n<Project>\n')
        for name in entries:
            f.write('    <ClCompile Include="%s" />\n' % name)
        f.write("</Project>\n")


def selftest():
    """Prove the checker can both pass and fail."""
    tmp = tempfile.mkdtemp(prefix="winsrc")
    try:
        engine = ["doc.cpp", "doc_shared.cpp", "pal\\win_transport.cpp"]

        # 1. agreeing pair (the test project also has its own test files)
        _write_proj(os.path.join(tmp, "MUSHclient.vcxproj"), engine)
        _write_proj(os.path.join(tmp, "MUSHclient_tests.vcxproj"),
                    engine + ["tests\\test_telnet.cpp", "tests\\win\\all_tests.cpp"])
        if main(["", tmp]) != 0:
            sys.stderr.write("SELFTEST FAIL: agreeing projects were rejected\n")
            return 1

        # 2. a source added to the app and not to the test target
        _write_proj(os.path.join(tmp, "MUSHclient.vcxproj"),
                    engine + ["telnet_phases.cpp"])
        if main(["", tmp]) == 0:
            sys.stderr.write("SELFTEST FAIL: a missing engine source was accepted\n")
            return 1

        # 3. a stale source left in the test target
        _write_proj(os.path.join(tmp, "MUSHclient.vcxproj"), engine)
        _write_proj(os.path.join(tmp, "MUSHclient_tests.vcxproj"),
                    engine + ["deleted_file.cpp", "tests\\test_telnet.cpp"])
        if main(["", tmp]) == 0:
            sys.stderr.write("SELFTEST FAIL: a stale engine source was accepted\n")
            return 1

        # 4. absent test project is not an error
        os.remove(os.path.join(tmp, "MUSHclient_tests.vcxproj"))
        if main(["", tmp]) != 0:
            sys.stderr.write("SELFTEST FAIL: absent test project was rejected\n")
            return 1

        print("check_win_test_sources selftest: 4/4 OK")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main(argv):
    if len(argv) > 1 and argv[1] == "--selftest":
        return selftest()
    root = argv[1] if len(argv) > 1 else os.path.dirname(os.path.dirname(
        os.path.abspath(__file__)))
    app_path = os.path.join(root, "MUSHclient.vcxproj")
    test_path = os.path.join(root, "MUSHclient_tests.vcxproj")

    if not os.path.exists(app_path):
        sys.stderr.write("no MUSHclient.vcxproj at %s\n" % root)
        return 1
    if not os.path.exists(test_path):
        # The Windows test target is optional; if it is not in the tree there
        # is nothing to keep in step.
        print("MUSHclient_tests.vcxproj absent - nothing to check")
        return 0

    app = sources(app_path)
    test = sources(test_path)

    # The test project compiles the suite as well; that is expected.  So are a
    # very small number of test-SUPPORT sources: engine-shaped TUs the app does
    # not link but the suite needs, the Windows echo of CMake's
    # MUSHCORE_TESTSUPPORT_SOURCES.  Mirrored in tests/win/gen_test_vcxproj.ps1;
    # keep both tiny, because every entry here is a file whose Windows build is
    # covered by nothing else.
    TEST_SUPPORT = set()   # currently empty; gen_test_vcxproj.ps1 says why

    test_only_ok = set(n for n in test
                       if n.startswith("tests\\") or n.startswith("tests/"))
    test_engine = test - test_only_ok - TEST_SUPPORT

    missing = sorted(app - test_engine)     # in the app, not in the tests
    extra = sorted(test_engine - app)       # in the tests, not in the app

    if not missing and not extra:
        print("engine sources agree: %d files" % len(app))
        return 0

    sys.stdout.write(
        "MUSHclient_tests.vcxproj is out of step with MUSHclient.vcxproj.\n"
        "Regenerate it:  pwsh tests/win/gen_test_vcxproj.ps1\n\n")
    for name in missing:
        sys.stdout.write("  only in the app        : %s\n" % name)
    for name in extra:
        sys.stdout.write("  only in the test target: %s\n" % name)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))

#!/usr/bin/env python3
"""Run a CTest suite with every test forced onto one CPU architecture.

    tools/ctest-arch.py BUILD_DIR x86_64 [-R REGEX] [-E REGEX] [-j N]

A universal macOS build's test executables contain both architectures, and
ctest always launches the native one.  This reads the suite from
`ctest --show-only=json-v1` and runs each test's command as `arch -<arch> ...`,
honouring the properties ctest itself would: WORKING_DIRECTORY, ENVIRONMENT,
TIMEOUT, SKIP_RETURN_CODE, WILL_FAIL, DISABLED and RUN_SERIAL.  On Apple Silicon,
`x86_64` runs the Intel half under Rosetta 2.

Tests whose command is not a Mach-O executable (Python lint scripts, shell
wrappers) are run unchanged: they exercise no architecture-specific code.
Exit status is 0 only if every test passed or was skipped.
"""
import argparse
import concurrent.futures as cf
import json
import os
import re
import subprocess
import sys
import time


def is_macho(path):
    try:
        with open(path, "rb") as f:
            magic = f.read(4)
    except OSError:
        return False
    return magic in (b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xfe\xed\xfa\xcf", b"\xbe\xba\xfe\xca")


def has_arch(path, arch):
    r = subprocess.run(["lipo", "-archs", path], capture_output=True, text=True)
    return arch in r.stdout.split()


def with_arch(cmd, arch):
    """Prefix the program the test actually exercises with `arch -<arch>`.

    Usually that is cmd[0].  A test may instead go through a launcher such as
    `cmake -E env VAR=value program args` (Guard Malloc's DYLD_INSERT_LIBRARIES,
    for one), whose own binary need not contain <arch> at all: then the arch
    prefix goes in front of the wrapped program, not the launcher."""
    if not cmd:
        return cmd
    if is_macho(cmd[0]) and has_arch(cmd[0], arch):
        return ["arch", "-" + arch] + cmd
    if os.path.basename(cmd[0]) == "cmake" and cmd[1:3] == ["-E", "env"]:
        i = 3
        while i < len(cmd) and (cmd[i].startswith("-") or re.match(r"^[A-Za-z_][A-Za-z0-9_]*=", cmd[i])):
            i += 1
        if i < len(cmd) and is_macho(cmd[i]) and has_arch(cmd[i], arch):
            return cmd[:i] + ["arch", "-" + arch] + cmd[i:]
    return cmd


def props(test):
    return {p["name"]: p["value"] for p in test.get("properties", [])}


def run_one(test, arch):
    name = test["name"]
    p = props(test)
    if p.get("DISABLED"):
        return name, "skipped", 0.0, "disabled"
    cmd = with_arch(list(test["command"]), arch)
    env = dict(os.environ)
    for kv in p.get("ENVIRONMENT", []):
        k, _, v = kv.partition("=")
        env[k] = v
    timeout = float(p.get("TIMEOUT", 1500))
    start = time.time()
    try:
        r = subprocess.run(cmd, cwd=p.get("WORKING_DIRECTORY"), env=env,
                           capture_output=True, text=True, errors="replace", timeout=timeout)
        rc, out = r.returncode, (r.stdout + r.stderr)
    except subprocess.TimeoutExpired as e:
        return name, "timeout", time.time() - start, (e.stdout or "")[-4000:] if isinstance(e.stdout, str) else ""
    elapsed = time.time() - start
    skip = p.get("SKIP_RETURN_CODE")
    if skip is not None and rc == int(skip):
        return name, "skipped", elapsed, out[-2000:]
    passed = (rc == 0)
    if p.get("WILL_FAIL"):
        passed = not passed
    for rx in p.get("PASS_REGULAR_EXPRESSION", []):
        passed = passed and re.search(rx, out) is not None
    for rx in p.get("FAIL_REGULAR_EXPRESSION", []):
        if re.search(rx, out):
            passed = False
    return name, ("passed" if passed else "FAILED (exit %d)" % rc), elapsed, out[-4000:]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("build_dir")
    ap.add_argument("arch")
    ap.add_argument("-R", dest="include")
    ap.add_argument("-E", dest="exclude")
    ap.add_argument("-j", dest="jobs", type=int, default=1)
    a = ap.parse_args()

    q = subprocess.run(["ctest", "--test-dir", a.build_dir, "--show-only=json-v1"],
                       capture_output=True, text=True, check=True)
    tests = json.loads(q.stdout)["tests"]
    if a.include:
        tests = [t for t in tests if re.search(a.include, t["name"])]
    if a.exclude:
        tests = [t for t in tests if not re.search(a.exclude, t["name"])]
    # RUN_SERIAL / RESOURCE_LOCK tests (the in-app GUI selftests) run one at a
    # time after the rest, as ctest would.
    def serial(t):
        p = props(t)
        return bool(p.get("RUN_SERIAL") or p.get("RESOURCE_LOCK"))
    locked = [t for t in tests if serial(t)]
    free = [t for t in tests if not serial(t)]

    results = []
    with cf.ThreadPoolExecutor(max(1, a.jobs)) as ex:
        results += list(ex.map(lambda t: run_one(t, a.arch), free))
    results += [run_one(t, a.arch) for t in locked]

    failed = [r for r in results if r[1] not in ("passed", "skipped")]
    for name, status, el, out in sorted(results):
        print("%-50s %-18s %6.1fs" % (name, status, el))
    for name, status, el, out in failed:
        print("\n----- %s: %s -----\n%s" % (name, status, out.rstrip()))
    print("\n%d tests under %s: %d passed, %d skipped, %d failed" % (
        len(results), a.arch,
        sum(r[1] == "passed" for r in results),
        sum(r[1] == "skipped" for r in results), len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

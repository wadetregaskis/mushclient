#!/usr/bin/env python3
"""tests/mud_stub.py — the repo's one loopback test MUD.

Python 3, standard library only, no build step, runs on macOS, Linux and
Windows.  It exists because this has now been written from scratch at least
three times (an inline copy used to sit in docs/GUI-DRIVE-PLAN.md §3.1, and the
2026-08-20 Windows round-two pass grew a PowerShell one); if you need a server
to point a client at, extend this rather than starting again.

**Reach for a unit test first.**  Nearly everything about the telnet phase
machine, MCCP, NAWS escaping and the subnegotiation cap is reachable headless
from tests/test_telnet.cpp against the real CMUSHclientDoc, and a unit test is
faster, deterministic, and runs in ctest.  This file is for what a unit test
structurally cannot see: a whole live client, its window, its modal dialogs,
and the Windows-only arms (SendWindowSizes derives its dimensions from
CMUSHView, which no headless build constructs).  Two of the three round-two
findings it produced were of exactly that kind — a modal-dialog storm and text
reaching the output window — and neither is visible from a doc-level test.

Two modes, because the two jobs are different:

  interactive   A deterministic MUD you can type at.  Fixed banner, and a small
                command vocabulary (ansi, x256, rgb, long, utf8, tab, url, mxp,
                stream N, prompt, gag, quit) chosen so that the answer to "did
                the client render this right?" is decidable from a screenshot.
                This is what docs/GUI-DRIVE-PLAN.md drives.

  scenario      A scripted byte sequence, for protocol work: telnet option
                negotiation, MCCP, and the two hostile streams the 2026-08-19
                security fixes are about.  The client's side of the
                conversation is written to the log as decoded telnet, so
                "did it double the 255?" is answerable from a file.

Everything received is logged either way.  The log is the instrument: for the
protocol scenarios the finding is a diff of two clients' logs, which is what
makes "is this a regression?" a comparison rather than a judgement.

Usage:

    python3 tests/mud_stub.py --port 4000
    python3 tests/mud_stub.py --scenario naws --log /tmp/srv.log --seconds 25
    python3 tests/mud_stub.py --list

A NOTE ON TRUST.  docs/WINDOWS-VALIDATION.md §3 records the rule the hard way:
verify any loopback stub against a *stock* client before believing what it says
about a new one.  A PowerShell predecessor of this file once produced a
connection that established and carried no data, which looked alarming until
stock 5.06 behaved identically against it — the harness was wrong, not the
client.  Run every scenario against both clients and diff.
"""

import argparse
import datetime
import socket
import sys
import threading
import time
import zlib

# ── telnet ──────────────────────────────────────────────────────────────────
IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240
TELOPT_ECHO, TELOPT_SGA = 1, 3
TELOPT_TTYPE, TELOPT_NAWS = 24, 31
TELOPT_COMPRESS, TELOPT_COMPRESS2 = 85, 86
TELOPT_GMCP = 201
IS, SEND = 0, 1

_CMD_NAMES = {IAC: "IAC", DONT: "DONT", DO: "DO", WONT: "WONT",
              WILL: "WILL", SB: "SB", SE: "SE"}
_OPT_NAMES = {TELOPT_ECHO: "ECHO", TELOPT_SGA: "SGA", TELOPT_TTYPE: "TTYPE",
              TELOPT_NAWS: "NAWS", TELOPT_COMPRESS: "COMPRESS",
              TELOPT_COMPRESS2: "COMPRESS2", TELOPT_GMCP: "GMCP"}

# telnet_phases.cpp caps an unterminated subnegotiation here and drops the
# connection (71c6afd).  Kept in step deliberately: the scenarios below sit
# either side of it.
MAX_SUBNEGOTIATION_LENGTH = 256 * 1024


def decode_telnet(data):
    """Render a byte string as a readable telnet trace."""
    out, i, n = [], 0, len(data)
    while i < n:
        if data[i] == IAC and i + 1 < n:
            cmd = data[i + 1]
            name = _CMD_NAMES.get(cmd, str(cmd))
            if cmd in (WILL, WONT, DO, DONT) and i + 2 < n:
                opt = data[i + 2]
                out.append("IAC %s %s" % (name, _OPT_NAMES.get(opt, str(opt))))
                i += 3
                continue
            if cmd == SB:
                j, payload = i + 2, []
                while j < n:
                    if data[j] == IAC and j + 1 < n and data[j + 1] == SE:
                        break
                    payload.append(data[j])
                    j += 1
                opt = payload[0] if payload else -1
                out.append("IAC SB %s [%s] IAC SE"
                           % (_OPT_NAMES.get(opt, str(opt)),
                              " ".join(str(b) for b in payload[1:])))
                i = j + 2
                continue
            out.append("IAC " + name)
            i += 2
            continue
        start = i
        while i < n and data[i] != IAC:
            i += 1
        text = data[start:i].decode("latin-1")
        out.append("TEXT %r" % text)
    return " | ".join(out)


class Log(object):
    def __init__(self, path):
        self.path = path
        self.lock = threading.Lock()
        self.lines = []

    def __call__(self, line):
        stamp = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
        with self.lock:
            self.lines.append("%s %s" % (stamp, line))
            if self.path:
                with open(self.path, "a") as f:
                    f.write("%s %s\n" % (stamp, line))
            else:
                sys.stdout.write("%s %s\n" % (stamp, line))
                sys.stdout.flush()


# ── the deterministic interactive MUD ───────────────────────────────────────

def ansi_block():
    out = []
    for base, label in ((30, "FG"), (90, "FG-BRIGHT")):
        out.append(label + ": " + " ".join(
            "\x1b[%dm#%d\x1b[0m" % (base + c, c) for c in range(8)))
    out.append("BG: " + "".join("\x1b[%dm %d \x1b[0m" % (40 + c, c) for c in range(8)))
    out.append("BG-BRIGHT: " + "".join("\x1b[%dm %d \x1b[0m" % (100 + c, c) for c in range(8)))
    out.append("STYLES: \x1b[1mbold\x1b[0m \x1b[4munderline\x1b[0m "
               "\x1b[7minverse\x1b[0m \x1b[5mblink\x1b[0m")
    return "\n".join(out) + "\n"


def interactive_commands(line):
    """Return the reply for one typed line, or None to echo it."""
    if line == "ansi":
        return ansi_block()
    if line == "x256":
        return ("X256: " + " ".join("\x1b[38;5;%dm%d\x1b[0m" % (c, c)
                                    for c in (1, 9, 21, 46, 196)) + "\n")
    if line == "rgb":
        return ("RGB: \x1b[38;2;128;0;0mmaroon\x1b[0m "
                "\x1b[38;2;170;0;0mfirebrick\x1b[0m "
                "\x1b[38;2;0;0;0mblack\x1b[0m\n")
    if line == "long":
        return "[wraptest]" + "abcdefghij" * 50 + "\n"
    if line == "utf8":
        return "UTF8: café naïve 世界 —dash— tail\n"
    if line == "tab":
        return "col1\tX\nlonger-col1\tX\ncafé\tX\n"
    if line == "url":
        return "Link: http://example.invalid/mudstub\n"
    if line == "mxp":
        return ("\x1b[1z<p>paragraph one</p>rest of line\n"
                "\x1b[1z<a href=\"https://example.invalid/\">a link</a>\n")
    if line == "osc":
        return "\x1b]0;a window title\x07after BEL\n\x1b]0;another\x1b\\after ST\n"
    if line == "gag":
        return "GAGME " + ("wrapme " * 60) + "\n"
    if line == "prompt":
        return "HP:100 MP:50 > "
    if line.startswith("stream"):
        try:
            count = int(line.split()[1])
        except (IndexError, ValueError):
            count = 20
        return ["stream-line-%03d\n" % i for i in range(count)]
    if line == "quit":
        return None
    return "echo: %s\n" % line


def serve_interactive(conn, log, banner):
    conn.sendall(banner.replace("\n", "\r\n").encode("utf-8"))
    buf = b""
    while True:
        data = conn.recv(4096)
        if not data:
            return
        log("RECV %d bytes: %s" % (len(data), decode_telnet(data)))
        buf += data
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            line = raw.rstrip(b"\r").decode("utf-8", "replace")
            reply = interactive_commands(line)
            if reply is None:
                conn.sendall(b"BYE\r\n")
                return
            if isinstance(reply, list):
                for chunk in reply:
                    conn.sendall(chunk.replace("\n", "\r\n").encode("utf-8"))
                    time.sleep(0.2)
            else:
                conn.sendall(reply.replace("\n", "\r\n").encode("utf-8"))


# ── the scripted protocol scenarios ─────────────────────────────────────────

def zlib_stream(payload, fdict=False):
    """A zlib stream around `payload`.

    With fdict=True the FDICT bit is set and a DICTID follows the header, which
    makes inflate answer Z_NEED_DICT — consuming nothing and producing nothing.
    A decompression loop that only tests for `< 0` spins on that forever; that
    is the first of the three defects 759d778 fixed.
    """
    if not fdict:
        return zlib.compress(payload)
    cmf = 0x78
    flg = next(f for f in range(0x20, 0x40) if (cmf * 256 + f) % 31 == 0)
    body = zlib.compressobj(9, zlib.DEFLATED, -zlib.MAX_WBITS)
    raw = body.compress(payload) + body.flush()
    return (bytes([cmf, flg]) + b"\x01\x02\x03\x04" + raw
            + zlib.adler32(payload).to_bytes(4, "big"))


def _send(conn, log, data, what=None):
    conn.sendall(bytes(data))
    log("SENT %s: %s" % (what or "%d bytes" % len(data),
                         " ".join(str(b) for b in data[:32])
                         + (" ..." if len(data) > 32 else "")))


def _text(conn, log, s):
    conn.sendall(s.encode("latin-1"))
    log("SENT TEXT: %r" % s)


SCENARIOS = {}


def scenario(name, doc):
    def register(fn):
        fn.__doc__ = doc
        SCENARIOS[name] = fn
        return fn
    return register


@scenario("plain", "MOTD, then a prompt with no newline.  The control: proves "
                   "the stub itself works before any of the others is believed.")
def s_plain(conn, log):
    _text(conn, log, "MOTD line one\r\nMOTD line two\r\n")
    time.sleep(1.0)
    _text(conn, log, "prompt> ")


@scenario("ttype", "IAC DO TTYPE, then four IAC SB TTYPE SEND cycles.  The "
                   "client's three answers are the terminal name, ANSI, and "
                   "the MTTS bitvector (00f634e).")
def s_ttype(conn, log):
    _send(conn, log, [IAC, DO, TELOPT_TTYPE], "IAC DO TTYPE")
    time.sleep(0.7)
    for _ in range(4):
        _send(conn, log, [IAC, SB, TELOPT_TTYPE, SEND, IAC, SE], "IAC SB TTYPE SEND")
        time.sleep(0.5)


@scenario("naws", "IAC DO NAWS.  Set the world's wrap column to 255 first: a "
                  "data 255 must be doubled per RFC 854, which is f878ff5.")
def s_naws(conn, log):
    _send(conn, log, [IAC, DO, TELOPT_NAWS], "IAC DO NAWS")
    time.sleep(2.0)
    _text(conn, log, "NAWS probe done\r\n")


@scenario("mccp_normal", "A well-formed MCCP2 stream.  The control for the two "
                         "MCCP scenarios below.")
def s_mccp_normal(conn, log):
    _send(conn, log, [IAC, WILL, TELOPT_COMPRESS2], "IAC WILL COMPRESS2")
    time.sleep(0.7)
    _send(conn, log, [IAC, SB, TELOPT_COMPRESS2, IAC, SE], "IAC SB COMPRESS2 IAC SE")
    conn.sendall(zlib_stream(b"MCCPHELLO one\r\nMCCPHELLO two\r\n"))
    log("SENT a well-formed zlib stream")


@scenario("mccp_fdict", "An MCCP2 stream whose zlib header sets FDICT.  inflate "
                        "answers Z_NEED_DICT, consuming and producing nothing; "
                        "stock 5.06 spins on a full core and stops responding.")
def s_mccp_fdict(conn, log):
    _send(conn, log, [IAC, WILL, TELOPT_COMPRESS2], "IAC WILL COMPRESS2")
    time.sleep(0.7)
    _send(conn, log, [IAC, SB, TELOPT_COMPRESS2, IAC, SE], "IAC SB COMPRESS2 IAC SE")
    conn.sendall(zlib_stream(b"this should never be seen\r\n", fdict=True))
    log("SENT an FDICT zlib stream")
    time.sleep(1.5)
    conn.sendall(zlib_stream(b"second packet\r\n", fdict=True))


@scenario("subneg_unbounded", "IAC SB <200> and then megabytes, never IAC SE.  "
                              "Unbounded heap growth before 71c6afd.")
def s_subneg_unbounded(conn, log):
    _text(conn, log, "before subneg\r\n")
    _send(conn, log, [IAC, SB, 200], "IAC SB 200")
    chunk = b"A" * 60000
    sent = 0
    for _ in range(200):
        try:
            conn.sendall(chunk)
        except OSError as exc:
            log("write failed after %d bytes: %s" % (sent, exc))
            break
        sent += len(chunk)
    log("sent %d subnegotiation bytes with no IAC SE" % sent)


@scenario("subneg_overflow_inject",
          "Exactly past the cap, then a marker.  Answers how many modal boxes "
          "one hostile stream raises, and whether post-cap bytes reach the "
          "output window as ordinary text.")
def s_subneg_overflow_inject(conn, log):
    _text(conn, log, "before subneg\r\n")
    _send(conn, log, [IAC, SB, 200], "IAC SB 200")
    conn.sendall(b"A" * (MAX_SUBNEGOTIATION_LENGTH + 16))
    time.sleep(0.3)
    _text(conn, log, "\r\nINJECTED-AFTER-CAP\r\n")


@scenario("subneg_big_ok",
          "A LEGITIMATE 200 KB subnegotiation, properly terminated — just under "
          "the cap.  UPSTREAM-BUGS 74 asks whether the ceiling clears real "
          "traffic; this is that check.")
def s_subneg_big_ok(conn, log):
    _text(conn, log, "before big subneg\r\n")
    _send(conn, log, [IAC, SB, TELOPT_GMCP], "IAC SB GMCP")
    conn.sendall(b"B" * 200000)
    log("sent 200000 payload bytes")
    _send(conn, log, [IAC, SE], "IAC SE")
    time.sleep(0.5)
    _text(conn, log, "after big subneg\r\n")


def drain(conn, log, until):
    conn.settimeout(0.5)
    while time.time() < until:
        try:
            data = conn.recv(65536)
        except socket.timeout:
            continue
        except OSError as exc:
            log("read error: %s" % exc)
            return
        if not data:
            log("peer closed")
            return
        log("RECV %d bytes: %s" % (len(data), " ".join(str(b) for b in data)))
        log("RECV decoded: %s" % decode_telnet(data))


# ── main ────────────────────────────────────────────────────────────────────

BANNER = ("WELCOME-MUDSTUB\n"
          "Type a command; anything unrecognised is echoed back.\n"
          "Known: ansi x256 rgb long utf8 tab url mxp osc gag prompt stream N quit\n")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4000)
    ap.add_argument("--log", default=None,
                    help="write the transcript here (default: stdout)")
    ap.add_argument("--scenario", default=None,
                    help="run one scripted protocol scenario instead of the "
                         "interactive MUD")
    ap.add_argument("--seconds", type=float, default=25.0,
                    help="scenario mode: how long to wait for a connection and "
                         "then keep draining (default 25)")
    ap.add_argument("--once", action="store_true",
                    help="interactive mode: serve one connection and exit")
    ap.add_argument("--list", action="store_true", help="list the scenarios")
    args = ap.parse_args(argv)

    if args.list:
        for name in sorted(SCENARIOS):
            print("%-24s %s" % (name, " ".join(SCENARIOS[name].__doc__.split())))
        return 0

    if args.scenario and args.scenario not in SCENARIOS:
        sys.stderr.write("unknown scenario %r; --list to see them\n" % args.scenario)
        return 2

    log = Log(args.log)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(4)
    log("listening on %s:%d scenario=%s" % (args.host, args.port, args.scenario or "-"))

    if args.scenario:
        srv.settimeout(args.seconds)
        try:
            conn, addr = srv.accept()
        except socket.timeout:
            log("NO CONNECTION")
            return 1
        deadline = time.time() + args.seconds
        log("connected from %s" % (addr,))
        try:
            SCENARIOS[args.scenario](conn, log)
            drain(conn, log, deadline)
        finally:
            log("done")
            conn.close()
            srv.close()
        return 0

    while True:
        conn, addr = srv.accept()
        log("CONNECT %s" % (addr,))
        try:
            serve_interactive(conn, log, BANNER)
        except OSError as exc:
            log("connection error: %s" % exc)
        finally:
            log("DISCONNECT %s" % (addr,))
            conn.close()
        if args.once:
            srv.close()
            return 0


if __name__ == "__main__":
    sys.exit(main())

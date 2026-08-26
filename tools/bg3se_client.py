#!/usr/bin/env python3
"""
bg3se_client.py - External client for the native macOS BG3 Script Extender console socket.

The Script Extender (bg3se-macos) listens on a Unix domain socket (default
/tmp/bg3se.sock). Lines written to it are evaluated as Lua in SERVER context
(Ext.IsServer() == true, Osi live). Output printed with Ext.Print() comes back
on the same socket. See src/console/console.c for the server side.

This module provides three things:

  1. A library core  -> class BG3SE (connect / eval / command / close, context mgr)
  2. A one-shot CLI   -> python3 bg3se_client.py 'Ext.Utils.GameVersion()'
  3. A REPL           -> python3 bg3se_client.py --repl

Reliability: the raw protocol has no per-command end marker, so instead of
racing a fixed sleep we wrap every request as a single physical line that
runs the user's code via load() and then prints a unique sentinel. We read
until the sentinel appears, so completion is deterministic.

The socket only ACCEPTS connections while BG3 is running with a save loaded
(a live server session). If the game is at the menu or closed you'll get
"connection refused" -- that's expected, load a save first.
"""

import argparse
import os
import re
import secrets
import socket
import sys

DEFAULT_SOCKET = "/tmp/bg3se.sock"
DEFAULT_TIMEOUT = 5.0

_ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
_ERR_MARK = "__BG3SE_ERR__"

# Lines the server emits that are framing noise, not results.
_NOISE_SUBSTRINGS = ("=== BG3SE Console ===", "Type Lua commands", "Multi-line:")
_PROMPTS = ("> ", "... ", ">", "...")


class BG3SEError(Exception):
    """A Lua compile or runtime error reported by the Script Extender."""


class BG3SETimeout(Exception):
    """The sentinel did not arrive before the timeout. `.partial` holds what we got."""

    def __init__(self, msg, partial=""):
        super().__init__(msg)
        self.partial = partial


def lua_escape(s: str) -> str:
    """Escape a Python string into the body of a Lua double-quoted string literal.
    Newlines become \\n so the whole literal stays on ONE physical line."""
    out = []
    for ch in s:
        o = ord(ch)
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\t":
            out.append("\\t")
        elif o < 0x20 or o == 0x7F:
            out.append("\\%03d" % o)  # zero-padded so a following digit can't extend it
        else:
            out.append(ch)
    return "".join(out)


def _strip_ansi(s: str) -> str:
    return _ANSI_RE.sub("", s)


class BG3SE:
    def __init__(self, path: str = DEFAULT_SOCKET, timeout: float = DEFAULT_TIMEOUT):
        self.path = path
        self.timeout = timeout
        self.sock = None

    # -- connection -------------------------------------------------------
    def connect(self):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(self.timeout)
        try:
            s.connect(self.path)
        except ConnectionRefusedError:
            s.close()
            raise BG3SEError(
                "connection refused at %s -- BG3 is not in a live session. "
                "Launch the game and load a save, then retry." % self.path
            )
        except FileNotFoundError:
            s.close()
            raise BG3SEError(
                "socket %s does not exist -- the Script Extender is not loaded." % self.path
            )
        self.sock = s
        self._drain_banner()
        return self

    def _drain_banner(self):
        """Consume the connect banner the server sends, if any (best effort)."""
        self.sock.settimeout(0.25)
        try:
            while True:
                if not self.sock.recv(4096):
                    break
        except socket.timeout:
            pass
        except OSError:
            pass
        finally:
            self.sock.settimeout(self.timeout)

    def close(self):
        if self.sock is not None:
            try:
                self.sock.close()
            finally:
                self.sock = None

    def __enter__(self):
        if self.sock is None:
            self.connect()
        return self

    def __exit__(self, *exc):
        self.close()

    # -- low level --------------------------------------------------------
    def _send_line(self, line: str):
        if self.sock is None:
            raise BG3SEError("not connected")
        self.sock.sendall(line.encode("utf-8", "surrogatepass") + b"\n")

    def _read_until(self, needle: bytes, timeout: float) -> bytes:
        self.sock.settimeout(timeout)
        buf = b""
        while True:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                raise BG3SETimeout(
                    "no sentinel within %.1fs" % timeout,
                    partial=_strip_ansi(buf.decode("utf-8", "replace")),
                )
            if not chunk:
                raise BG3SEError("server closed the connection")
            buf += chunk
            if needle in buf:
                return buf

    # -- high level -------------------------------------------------------
    def eval(self, code: str, timeout: float = None) -> str:
        """Run Lua `code` in server context and return its output.

        If `code` is an expression, its value(s) are returned (tab-joined),
        matching REPL behavior. Ext.Print output during the run is included.
        Raises BG3SEError on a Lua compile/runtime error.
        """
        timeout = self.timeout if timeout is None else timeout
        nonce = "__BG3SE_%s__" % secrets.token_hex(8)
        esc = lua_escape(code)
        # One physical line. Try `return <code>` first (expression -> auto-print),
        # fall back to running it as statements. pcall so runtime errors are caught
        # and the sentinel always prints.
        cmd = (
            'local __s="%(esc)s" '
            'local __f=load("return "..__s,"=bg3se") '
            "if not __f then __f=load(__s,\"=bg3se\") end "
            "if not __f then local _,__e=load(__s,\"=bg3se\") "
            'Ext.Print("%(err)scompile: "..tostring(__e)) '
            "else local __r={pcall(__f)} "
            "if not __r[1] then "
            'Ext.Print("%(err)sruntime: "..tostring(__r[2])) '
            "else local __o={} for __i=2,#__r do __o[#__o+1]=tostring(__r[__i]) end "
            'if #__o>0 then Ext.Print(table.concat(__o,"\\t")) end end end '
            'Ext.Print("%(nonce)s")'
        ) % {"esc": esc, "err": _ERR_MARK, "nonce": nonce}

        self._send_line(cmd)
        raw = self._read_until(nonce.encode(), timeout)
        text = _strip_ansi(raw.decode("utf-8", "replace"))
        # Keep only what came before the sentinel.
        text = text.split(nonce, 1)[0]
        return self._clean(text)

    def _clean(self, text: str) -> str:
        lines = []
        for ln in text.splitlines():
            stripped = ln.strip()
            if not stripped:
                continue
            if stripped in _PROMPTS:
                continue
            if any(sub in ln for sub in _NOISE_SUBSTRINGS):
                continue
            if stripped.startswith(_ERR_MARK):
                raise BG3SEError(stripped[len(_ERR_MARK):].strip())
            lines.append(ln.rstrip())
        return "\n".join(lines)

    def command(self, line: str, timeout: float = None) -> str:
        """Send a raw server command (e.g. '!identity', '!status') and return its reply.

        These builtin ! commands don't run our sentinel wrapper, so we read
        whatever arrives within a short window.
        """
        timeout = self.timeout if timeout is None else timeout
        if not line.startswith("!"):
            line = "!" + line
        self._send_line(line)
        self.sock.settimeout(timeout)
        buf = b""
        try:
            while True:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                buf += chunk
        except socket.timeout:
            pass
        return _strip_ansi(buf.decode("utf-8", "replace")).strip()

    def identity(self) -> str:
        """Machine-readable identity/readiness JSON (pid, session_init, stats_ready)."""
        return self.command("!identity")


# ------------------------------------------------------------------------
# CLI + REPL
# ------------------------------------------------------------------------
def _run_repl(client: BG3SE, color: bool):
    green = "\033[32m" if color else ""
    red = "\033[31m" if color else ""
    reset = "\033[0m" if color else ""
    print("bg3se REPL -- connected to %s (server context). Ctrl-D to exit." % client.path)
    print("End a line with \\ to continue on the next line.")
    buffer = ""
    try:
        import readline  # noqa: F401  (enables line editing/history)
    except Exception:
        pass
    while True:
        prompt = ("%s...%s " % (green, reset)) if buffer else ("%sbg3se>%s " % (green, reset))
        try:
            line = input(prompt)
        except EOFError:
            print()
            break
        except KeyboardInterrupt:
            print()
            buffer = ""
            continue
        if line.endswith("\\"):
            buffer += line[:-1] + "\n"
            continue
        code = buffer + line
        buffer = ""
        if not code.strip():
            continue
        try:
            out = client.eval(code)
            if out:
                print(out)
        except BG3SEError as e:
            print("%s%s%s" % (red, e, reset))
        except BG3SETimeout as e:
            print("%stimeout%s%s" % (red, reset, ("\n" + e.partial if e.partial else "")))


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="bg3se_client",
        description="Send Lua to the native macOS BG3 Script Extender and read the result.",
    )
    ap.add_argument("code", nargs="?", help="Lua to run. Omit to read from --file, stdin, or start --repl.")
    ap.add_argument("-s", "--socket", default=DEFAULT_SOCKET, help="socket path (default %s)" % DEFAULT_SOCKET)
    ap.add_argument("-t", "--timeout", type=float, default=DEFAULT_TIMEOUT, help="seconds to wait for a reply")
    ap.add_argument("-f", "--file", help="read Lua from this file")
    ap.add_argument("-c", "--command", help="send a raw ! command (e.g. identity, status) and print the reply")
    ap.add_argument("--repl", action="store_true", help="start an interactive REPL")
    ap.add_argument("--no-color", action="store_true", help="disable ANSI color in the REPL")
    args = ap.parse_args(argv)

    client = BG3SE(args.socket, args.timeout)
    try:
        client.connect()
    except BG3SEError as e:
        print("error: %s" % e, file=sys.stderr)
        return 2

    try:
        if args.command:
            print(client.command(args.command))
            return 0
        if args.repl:
            _run_repl(client, color=not args.no_color and sys.stdout.isatty())
            return 0

        if args.code is not None:
            code = args.code
        elif args.file:
            with open(args.file, "r") as fh:
                code = fh.read()
        elif not sys.stdin.isatty():
            code = sys.stdin.read()
        else:
            ap.print_help()
            return 1

        try:
            out = client.eval(code)
            if out:
                print(out)
            return 0
        except BG3SEError as e:
            print("lua error: %s" % e, file=sys.stderr)
            return 1
        except BG3SETimeout as e:
            print("timeout: %s" % e, file=sys.stderr)
            if e.partial:
                print(e.partial, file=sys.stderr)
            return 3
    finally:
        client.close()


if __name__ == "__main__":
    sys.exit(main())

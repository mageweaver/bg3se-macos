#!/usr/bin/env python3
"""
bg3se_win_client.py - External client for Norbyte's Windows BG3SE via the
ConsoleBridge mod.

Norbyte's extender exposes no IPC socket (its console reads stdin only, and it
freopen()s stdin to its own CONIN$, so a redirected stdin pipe can't reach it).
The ConsoleBridge mod instead offers a file-based request/response channel in the
SE user directory. This client drives that channel and mirrors the API of the
macOS bg3se_client.BG3SE class (eval / BG3SEError / BG3SETimeout).

Files (in the SE user dir):
    bridge_req.json   this client writes  {"id":N,"code":"..."}
    bridge_resp.json  the mod writes       {"id":N,"ok":bool,"output":"...","error":...}

The mod processes a request when its id is greater than the last one it handled,
then writes the matching response. We use a strictly-increasing millisecond id so
requests are never mistaken for stale ones (the mod's counter resets to 0 on
reload; our ids are always large and positive).

Usage:
    python bg3se_win_client.py 'Osi.GetHostCharacter()'
    python bg3se_win_client.py -f probe.lua
    echo "return 1+1" | python bg3se_win_client.py
    python bg3se_win_client.py --repl
    python bg3se_win_client.py --dir "D:\\path\\to\\Script Extender" 'Ext.Utils.Version()'
"""

import argparse
import json
import os
import sys
import time

DEFAULT_TIMEOUT = 8.0
REQ_NAME = "bridge_req.json"
RESP_NAME = "bridge_resp.json"


def default_se_dir():
    # Windows SE user directory (Ext.IO UserProfile root).
    local = os.environ.get("LOCALAPPDATA")
    if local:
        return os.path.join(local, "Larian Studios", "Baldur's Gate 3", "Script Extender")
    # Fallback for testing on non-Windows hosts.
    return os.path.join(os.path.expanduser("~"), "bg3se_bridge")


class BG3SEError(Exception):
    """A Lua compile or runtime error reported by the bridge mod."""


class BG3SETimeout(Exception):
    """No matching response within the timeout (mod not running, or too slow)."""


class BG3SEWin:
    def __init__(self, se_dir=None, timeout=DEFAULT_TIMEOUT, poll_interval=0.05):
        self.se_dir = se_dir or default_se_dir()
        self.timeout = timeout
        self.poll_interval = poll_interval
        self._last_id = 0
        self.req_path = os.path.join(self.se_dir, REQ_NAME)
        self.resp_path = os.path.join(self.se_dir, RESP_NAME)

    def _next_id(self):
        nid = max(self._last_id + 1, int(time.time() * 1000))
        self._last_id = nid
        return nid

    def _write_request(self, req_id, code):
        if not os.path.isdir(self.se_dir):
            raise BG3SEError(
                "SE directory not found: %s -- is the Script Extender installed?" % self.se_dir
            )
        payload = json.dumps({"id": req_id, "code": code})
        tmp = self.req_path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as fh:
            fh.write(payload)
        os.replace(tmp, self.req_path)  # atomic swap

    def _await_response(self, req_id, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                with open(self.resp_path, "r", encoding="utf-8") as fh:
                    data = fh.read()
                resp = json.loads(data)
            except (FileNotFoundError, ValueError):
                time.sleep(self.poll_interval)
                continue
            if isinstance(resp, dict) and resp.get("id") == req_id:
                return resp
            time.sleep(self.poll_interval)
        raise BG3SETimeout(
            "no response for id %d within %.1fs -- is BG3 running with the "
            "ConsoleBridge mod enabled and a save loaded?" % (req_id, timeout)
        )

    def eval(self, code, timeout=None):
        """Run Lua `code` in server context; return output (Ext.Print + return values).
        Raises BG3SEError on a Lua compile/runtime error."""
        timeout = self.timeout if timeout is None else timeout
        req_id = self._next_id()
        self._write_request(req_id, code)
        resp = self._await_response(req_id, timeout)
        if not resp.get("ok", False):
            raise BG3SEError(resp.get("error") or "unknown error")
        return resp.get("output", "") or ""

    # Parity with the Mac client; connection is implicit for the file channel.
    def connect(self):
        return self

    def close(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def _run_repl(client, color):
    green = "\033[32m" if color else ""
    red = "\033[31m" if color else ""
    reset = "\033[0m" if color else ""
    print("bg3se (Windows/ConsoleBridge) REPL -- dir: %s" % client.se_dir)
    print("End a line with \\ to continue on the next line. Ctrl-D / Ctrl-Z to exit.")
    try:
        import readline  # noqa: F401
    except Exception:
        pass
    buffer = ""
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
            print("%s%s%s" % (red, e, reset))


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="bg3se_win_client",
        description="Send Lua to Norbyte's Windows BG3SE via the ConsoleBridge mod.",
    )
    ap.add_argument("code", nargs="?", help="Lua to run. Omit for --file, stdin, or --repl.")
    ap.add_argument("-d", "--dir", help="SE user directory (default: %%LOCALAPPDATA%%\\...\\Script Extender)")
    ap.add_argument("-t", "--timeout", type=float, default=DEFAULT_TIMEOUT, help="seconds to wait for a reply")
    ap.add_argument("-f", "--file", help="read Lua from this file")
    ap.add_argument("--repl", action="store_true", help="start an interactive REPL")
    ap.add_argument("--no-color", action="store_true", help="disable ANSI color in the REPL")
    args = ap.parse_args(argv)

    client = BG3SEWin(se_dir=args.dir, timeout=args.timeout)

    if args.repl:
        _run_repl(client, color=not args.no_color and sys.stdout.isatty())
        return 0

    if args.code is not None:
        code = args.code
    elif args.file:
        with open(args.file, "r", encoding="utf-8") as fh:
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
        return 3


if __name__ == "__main__":
    sys.exit(main())

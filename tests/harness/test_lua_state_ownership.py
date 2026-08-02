"""E2.0 static gate: no unowned global lua_State references.

The dual-VM refactor (Wave 7 Phase 1, docs/dual-vm/E2.0-state-ownership-audit.md)
requires every raw `lua_State*` cache to be owned by a LuaRuntime. This test
walks src/ for static lua_State declarations and fails on any not in the
allowlist. Category 1 is fully migrated: the allowlist is empty and must stay
empty — new global Lua state belongs in the LuaRuntime registry.
"""

import re
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent.parent / "src"

# (file relative to src/, declaration substring). All 8 Category-1 items from
# the E2.0 audit have migrated onto LuaRuntime — the gate is closed; no
# unowned declarations are allowed.
ALLOWLIST = set()

# Files where owned lua_State storage is legitimate by design.
OWNED_FILES = {"lua/lua_runtime.c", "lua/lua_runtime.h"}

DECL_RE = re.compile(
    r"^\s*(?:static|_Atomic|\w+\s+)*.*\b(?:static\s+)?"
    r"(?:_Atomic\s*\(\s*)?lua_State\s*\*", re.MULTILINE
)
STATIC_DECL_RE = re.compile(
    r"^\s*static\s+(?:_Atomic\s*\(\s*)?lua_State\s*\*?[^;(]*;|"
    r"^\s*static\s+_Atomic\s*\(\s*lua_State\s*\*\s*\)[^;]*;",
    re.MULTILINE,
)


def _find_static_lua_state_decls():
    found = []
    for path in sorted(SRC.rglob("*")):
        if path.suffix not in (".c", ".h", ".m", ".mm"):
            continue
        rel = str(path.relative_to(SRC))
        if rel in OWNED_FILES:
            continue
        text = path.read_text(errors="replace")
        for match in STATIC_DECL_RE.finditer(text):
            decl = " ".join(match.group(0).split())
            line = text[: match.start()].count("\n") + 1
            found.append((rel, line, decl))
    return found


def test_no_unowned_global_lua_state_references():
    found = _find_static_lua_state_decls()
    unowned = []
    for rel, line, decl in found:
        allowed = any(rel == f and sub in decl for f, sub in ALLOWLIST)
        if not allowed:
            unowned.append(f"{rel}:{line}: {decl}")
    assert not unowned, (
        "New unowned global lua_State reference(s) — register ownership via "
        "LuaRuntime (docs/dual-vm/E2.0-state-ownership-audit.md) instead:\n"
        + "\n".join(unowned)
    )


def test_allowlist_entries_still_exist():
    """A migrated (removed) declaration must also leave the allowlist."""
    found = _find_static_lua_state_decls()
    stale = []
    for f, sub in sorted(ALLOWLIST):
        if not any(rel == f and sub in decl for rel, line, decl in found):
            stale.append(f"{f} :: {sub}")
    assert not stale, (
        "Allowlist entries no longer match a declaration — remove them so the "
        "E2.0 gate keeps shrinking:\n" + "\n".join(stale)
    )

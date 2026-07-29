#!/usr/bin/env python3
"""
port_offsets.py — resolve BG3SE-macOS per-version addresses against a BG3 binary.

The macOS BG3 binary keeps its symbol table, so almost every per-version address
is a `nm` lookup. This tool reads tools/offset_manifest.json (the recipe) and, for
a target binary, resolves every address and emits ready-to-paste offset_table.c
content. Anything it can't resolve is flagged — that's your signal that a struct
changed and you need Ghidra/runtime probing for that one item.

Usage:
  python3 tools/port_offsets.py resolve [--binary PATH] [--emit]
  python3 tools/port_offsets.py verify  [--binary PATH]   # diff vs offset_table.c

See docs/PORTING.md.
"""
import argparse, json, os, re, subprocess, sys, tempfile, plistlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
MANIFEST = os.path.join(HERE, "offset_manifest.json")
OFFSET_TABLE_C = os.path.join(REPO, "src", "core", "offset_table.c")
GHIDRA_BASE = 0x100000000

DEFAULT_BINARY = os.path.expanduser(
    "~/Library/Application Support/Steam/steamapps/common/"
    "Baldurs Gate 3/Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3"
)

# ----------------------------------------------------------------------------
# Binary + symbol handling
# ----------------------------------------------------------------------------

def thin_arm64(binary):
    """Return a path to an arm64 thin slice (extracting if the binary is fat)."""
    archs = subprocess.run(["lipo", "-archs", binary], capture_output=True, text=True)
    if archs.returncode != 0:
        # not a fat binary / lipo failed — assume it's already thin
        return binary
    if "arm64" not in archs.stdout.split():
        sys.exit(f"error: {binary} has no arm64 slice (archs: {archs.stdout.strip()})")
    if archs.stdout.split() == ["arm64"]:
        return binary
    out = os.path.join(tempfile.gettempdir(), "bg3_arm64_thin_port")
    subprocess.run(["lipo", "-thin", "arm64", binary, "-output", out], check=True)
    return out

def norm(s):
    """Normalize a C++ signature for matching (whitespace-insensitive)."""
    return re.sub(r"\s+", "", s)

def build_symbol_map(thin):
    """{normalized_demangled_name: set(addresses)} from nm + c++filt."""
    raw = subprocess.run(["nm", thin], capture_output=True, text=True).stdout
    dem = subprocess.run(["c++filt"], input=raw, capture_output=True, text=True).stdout
    table = {}
    for line in dem.splitlines():
        parts = line.split(" ", 2)
        if len(parts) < 3:
            continue
        addr, typ, name = parts
        if not re.fullmatch(r"[0-9a-fA-F]+", addr):
            continue  # undefined symbol (no address)
        table.setdefault(norm(name), set()).add(int(addr, 16))
    return table

def lookup(symtab, symbol):
    """Return (addr, note). note flags ambiguity/missing."""
    addrs = symtab.get(norm(symbol))
    if not addrs:
        return None, "NOT FOUND"
    if len(addrs) > 1:
        return sorted(addrs)[0], f"AMBIGUOUS ({len(addrs)} matches; took lowest)"
    return next(iter(addrs)), ""

def detect_version(binary):
    info = os.path.join(os.path.dirname(os.path.dirname(binary)), "Info.plist")
    try:
        with open(info, "rb") as f:
            return plistlib.load(f).get("CFBundleShortVersionString")
    except Exception:
        return None

def hx(v):
    return f"0x{v:08x}"

# ----------------------------------------------------------------------------
# Resolution
# ----------------------------------------------------------------------------

def got_slots(thin):
    """{symbol_mangled: slot_va} from otool -Iv (indirect-symbol table)."""
    out = {}
    res = subprocess.run(["otool", "-arch", "arm64", "-Iv", thin],
                         capture_output=True, text=True)
    for line in res.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[0].startswith("0x"):
            out.setdefault(parts[2], int(parts[0], 16))
    return out


def resolve(manifest, symtab, thin):
    """Return a dict of resolved values + a list of (level, message) issues."""
    issues = []
    out = {"fn": {}, "data": {}, "remap": [], "exported": {}, "constants": [], "struct": []}

    # 1. derive component_data_shift from the anchor.
    # NOTE: the anchor baseline is the 7209685 address (the vintage of the
    # compiled-in constants), so the result IS component_data_shift for the
    # target version: 0 on 7209685 itself, -0x8000 on 6995620.
    # It is used ONLY for TypeId globals (proven-uniform family), never for
    # the data singletons below — those resolve per-entry.
    anchor = manifest["data_shift_anchor"]
    a_addr, a_note = lookup(symtab, anchor["symbol"])
    if a_addr is None:
        issues.append(("FATAL", f"data_shift anchor '{anchor['symbol']}' not found — "
                                "cannot derive component_data_shift. Pick another anchor."))
        return out, issues
    shift = a_addr - int(anchor["baseline"], 16)
    out["data_shift"] = shift
    issues.append(("INFO", f"component_data_shift (delta from {manifest.get('constants_vintage','7209685')} "
                           f"constants): {shift:#x} "
                           f"(anchor {anchor['symbol'].split('(')[0]} {hx(int(anchor['baseline'],16))} -> 0x{a_addr:x})"))

    # 2. offset_table functions (symbol)
    for e in manifest["offset_table_functions"]:
        addr, note = lookup(symtab, e["symbol"])
        if addr is None:
            issues.append(("ERROR", f"offset_table.{e['field']}: symbol not found ({e['symbol'][:60]}...)"))
        else:
            out["fn"][e["field"]] = addr - GHIDRA_BASE
            if note:
                issues.append(("WARN", f"offset_table.{e['field']}: {note}"))

    # 3. data singletons — per-entry method; NO uniform-shift assumption
    # (global_switches_ptr moved -0x24000 while its neighbors moved +0x8000).
    gots = None
    for e in manifest["data_singletons"]:
        method = e.get("method", "symbol")
        if method == "symbol":
            addr, note = lookup(symtab, e["symbol"])
            if addr is None:
                issues.append(("ERROR", f"offset_table.{e['field']}: symbol not found "
                                        f"({e.get('symbol','?')[:60]})"))
                continue
            out["data"][e["field"]] = addr - GHIDRA_BASE
            if note:
                issues.append(("WARN", f"offset_table.{e['field']}: {note}"))
        elif method == "got":
            if gots is None:
                gots = got_slots(thin)
            # match by mangled name fragment of the target symbol
            # (exclude __ZGV* guard variables — same suffix, wrong slot)
            frag = "m_StateE"
            cands = {s: va for s, va in gots.items()
                     if "ImmutableDataHeadmaster" in s and s.endswith(frag)
                     and not s.startswith("__ZGV")}
            if len(cands) == 1:
                out["data"][e["field"]] = next(iter(cands.values())) - GHIDRA_BASE
            else:
                issues.append(("ERROR", f"offset_table.{e['field']}: __got slot for "
                                        f"'{e['symbol']}' not uniquely found "
                                        f"({len(cands)} candidates) — resolve via otool -Iv manually"))
        elif method == "disasm":
            issues.append(("WARN", f"offset_table.{e['field']}: MANUAL — anonymous slot, "
                                   f"no symbol. {e.get('note','')}"))
        else:
            issues.append(("ERROR", f"offset_table.{e['field']}: unknown method '{method}'"))

    # 4. exported data (symbol)
    for e in manifest["exported_data"]:
        addr, note = lookup(symtab, e["symbol"])
        if addr is None:
            issues.append(("ERROR", f"exported_data {e['name']}: symbol not found"))
        else:
            out["exported"][e["name"]] = addr

    # 5. remap functions (symbol). ABI CAUTION: resolving a symbol proves the
    # ADDRESS, not the SIGNATURE the C wrapper expects — entries flagged
    # abi_changed emit 0 until a matching wrapper exists.
    for e in manifest["remap_functions"]:
        addr, note = lookup(symtab, e["symbol"])
        if addr is None:
            issues.append(("ERROR", f"remap {e['name']}: symbol not found ({e['symbol'][:60]}...)"))
        else:
            if e.get("abi_changed"):
                issues.append(("WARN", f"remap {e['name']}: ABI CHANGED — emitting 0 "
                                       f"(found at 0x{addr:x}, but the wrapper signature "
                                       f"does not fit; see manifest _note)"))
                addr = 0
            out["remap"].append((int(e["baseline"], 16), addr, e["name"]))
            if note:
                issues.append(("WARN", f"remap {e['name']}: {note}"))

    # 6. constant functions (verify unchanged)
    for e in manifest.get("constant_functions", []):
        addr, note = lookup(symtab, e["symbol"])
        base = int(e["baseline"], 16)
        if addr is None:
            issues.append(("WARN", f"constant {e['name']}: symbol not found — cannot verify it is unchanged"))
        elif addr != base:
            issues.append(("ERROR", f"constant {e['name']}: CHANGED {hx(base)} -> 0x{addr:x} "
                                    "— it is NOT constant for this version; promote it to remap_functions"))
        out["constants"].append((e["name"], base, addr))

    # 7. struct offsets — carried, not re-resolved (documented)
    out["struct"] = manifest.get("struct_offsets", [])
    return out, issues

# ----------------------------------------------------------------------------
# Output
# ----------------------------------------------------------------------------

def emit_c(out, version):
    L = []
    L.append(f"    /* ---- generated by tools/port_offsets.py for {version} ---- */")
    L.append("    {")
    L.append(f'        .version                 = "{version}",')
    L.append("")
    L.append("        /* Singleton pointer globals (per-entry symbol/got resolution) */")
    for field, val in out["data"].items():
        L.append(f"        .{field:<24} = {hx(val)},")
    L.append("")
    L.append("        /* Function offsets (symbol-resolved) */")
    for field, val in out["fn"].items():
        L.append(f"        .{field:<24} = {hx(val)},")
    shift = out["data_shift"]
    L.append(f"        .component_data_shift    = {'-' if shift < 0 else ''}{hx(abs(shift))},")
    L.append("    },")
    L.append("")
    L.append(f"    /* g_fn_remap: {version} column values (row key = 6995620 baseline). */")
    L.append("    /* NOTE: g_fn_remap is a fixed two-column {6995620, 7209685} table; a  */")
    L.append("    /* THIRD version needs a schema change (add a column), not new rows.   */")
    for base, new, name in out["remap"]:
        L.append(f"    {{ 0x{base:09x}, 0x{new:09x} }},  // {name}")
    return "\n".join(L)

# ----------------------------------------------------------------------------
# verify: compare generated values against what's in offset_table.c
# ----------------------------------------------------------------------------

def parse_offset_table_c(version):
    """Extract the {.field = 0x..} block for `version` and the remap pairs."""
    txt = open(OFFSET_TABLE_C).read()
    fields = {}
    # find the struct entry for this version
    m = re.search(r'\.version\s*=\s*"' + re.escape(version) + r'"(.*?)\n\s*\},', txt, re.S)
    if m:
        for fm in re.finditer(r'\.(\w+)\s*=\s*(-?0x[0-9a-fA-F]+|\d+)', m.group(1)):
            v = fm.group(2)
            fields[fm.group(1)] = int(v, 16) if "0x" in v else int(v)
    remap = []
    rm = re.search(r'g_fn_remap\[\]\s*=\s*\{(.*?)\n\};', txt, re.S)
    if rm:
        for pm in re.finditer(r'\{\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+|0)\s*\}', rm.group(1)):
            remap.append((int(pm.group(1), 16), int(pm.group(2), 16)))
    return fields, remap

def do_verify(out, version):
    fields, remap = parse_offset_table_c(version)
    if not fields:
        print(f"  (no '{version}' entry in offset_table.c to verify against)")
        return 0
    mism = 0
    gen = {**out["data"], **out["fn"], "component_data_shift": out["data_shift"]}
    for f, v in gen.items():
        cur = fields.get(f)
        if cur is None:
            print(f"  MISSING in offset_table.c: .{f} (generated {hx(v)})"); mism += 1
        elif cur != v:
            print(f"  MISMATCH .{f}: table={hx(cur)} generated={hx(v)}"); mism += 1
    gen_remap = {b: n for b, n, _ in out["remap"]}
    cur_remap = dict(remap)
    for b, n in gen_remap.items():
        if cur_remap.get(b) != n:
            print(f"  REMAP MISMATCH 0x{b:x}: table={cur_remap.get(b) and hex(cur_remap[b])} generated=0x{n:x}"); mism += 1
    if mism == 0:
        print(f"  ✓ all {len(gen)} fields + {len(gen_remap)} remap entries match offset_table.c")
    return mism

# ----------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["resolve", "verify"])
    ap.add_argument("--binary", default=DEFAULT_BINARY, help="path to the BG3 Mach-O binary")
    ap.add_argument("--version", help="override detected version label")
    ap.add_argument("--emit", action="store_true", help="(resolve) print copy-pasteable offset_table.c content")
    args = ap.parse_args()

    if not os.path.exists(args.binary):
        sys.exit(f"error: binary not found: {args.binary}\n  pass --binary PATH")
    manifest = json.load(open(MANIFEST))
    version = args.version or detect_version(args.binary) or "UNKNOWN"

    print(f"binary : {args.binary}")
    print(f"version: {version}")
    print("indexing symbols (nm + c++filt)...")
    thin = thin_arm64(args.binary)
    symtab = build_symbol_map(thin)
    print(f"  {len(symtab)} symbols\n")

    out, issues = resolve(manifest, symtab, thin)

    rank = {"FATAL": 0, "ERROR": 1, "WARN": 2, "INFO": 3}
    for lvl, msg in sorted(issues, key=lambda i: rank.get(i[0], 9)):
        print(f"[{lvl}] {msg}")
    errs = sum(1 for l, _ in issues if l in ("FATAL", "ERROR"))
    print()

    if args.cmd == "verify":
        mism = do_verify(out, version)
        sys.exit(1 if (mism or errs) else 0)

    # resolve
    nres = len(out["fn"]) + len(out["data"]) + len(out["remap"]) + len(out["exported"])
    print(f"resolved {nres} addresses; {len(out['struct'])} struct offsets carried (stable).")
    if args.emit:
        print("\n" + "=" * 70)
        print(emit_c(out, version))
        print("=" * 70)
    else:
        print("re-run with --emit to print copy-pasteable offset_table.c content.")
    sys.exit(1 if errs else 0)

if __name__ == "__main__":
    main()

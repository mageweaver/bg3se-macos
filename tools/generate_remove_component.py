#!/usr/bin/env python3
"""generate_remove_component.py -- regenerate src/gen/<store>/generated_remove_component.h

macOS emits one ImmediateWorldCache::RemoveComponent<T> specialization per
component type, each with its TypeId<T> baked in, and exposes no generic
runtime-TypeIndex entry point. The specializations carry symbols, so the
name -> address mapping is recoverable per build.

Previously built by hand from `nm` output, which stopped being reasonable with a
second store to support.

Emits only components this port registers: the binary carries more
specializations (734 on 4.1.1.7398727) than the port can name, and an unnamed
entry is unusable.

Also excludes proxy components, whose ECS slot holds a pointer to the object
rather than the object, so RemoveComponent<T> does not apply -- esv::Item is the
only one with a specialization. The proxy flag is read from the curated table in
src/entity/component_typeid.c so the two stay in step.

Usage:
  python3 tools/generate_remove_component.py --binary PATH --build-id ID \
      [--typeids src/gen/<store>/generated_typeids.h] [--out PATH]
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent

# void ecs::legacy::ImmediateWorldCache::RemoveComponent<T>(ls::ID<...>)
_DEMANGLED = re.compile(
    r"^(?P<addr>[0-9a-f]+)\s+\S+\s+"
    r"void ecs::legacy::ImmediateWorldCache::RemoveComponent<(?P<type>.+)>"
    r"\(ls::ID<.*>\)$"
)

# Registry rows look like:
#   { "eoc::FooComponent", "__ZN2ls6TypeId...", "ecs::...Context", "<build>", 0x...ULL },
_REGISTRY_NAME = re.compile(r'^\s*\{\s*"(?P<name>[^"]+)"\s*,', re.MULTILINE)

# Curated rows in component_typeid.c:
#   { "esv::Item", 0, true, "ecs::ComponentTypeIdContext", false },
#                     ^^^^ isProxy
_CURATED_ROW = re.compile(
    r'^\s*\{\s*"(?P<name>[^"]+)"\s*,\s*\d+\s*,\s*(?P<proxy>true|false)\s*,',
    re.MULTILINE)


def specializations(binary: Path) -> dict[str, int]:
    """component type name -> function VA, from the binary's own symbols."""
    nm = subprocess.run(
        ["nm", "-arch", "arm64", "-a", str(binary)],
        capture_output=True, text=True, check=True,
    )
    raw = [l for l in nm.stdout.splitlines()
           if "ImmediateWorldCache15RemoveComponent" in l]
    if not raw:
        sys.exit(f"error: no RemoveComponent specializations found in {binary}\n"
                 "  (is this the real game binary, not the launcher stub?)")

    demangled = subprocess.run(
        ["c++filt"], input="\n".join(raw), capture_output=True, text=True, check=True,
    ).stdout

    found: dict[str, int] = {}
    for line in demangled.splitlines():
        m = _DEMANGLED.match(line.strip())
        if m:
            found[m.group("type")] = int(m.group("addr"), 16)
    return found


def registered_names(typeids_header: Path) -> set[str]:
    """Component names this port registers, read from the generated authority."""
    text = typeids_header.read_text(encoding="utf-8")
    # The registry .c beside the header carries the exact names; the header
    # carries them in trailing comments. Prefer the registry when present.
    registry = typeids_header.parent / "generated_component_registry.c"
    if registry.is_file():
        return set(_REGISTRY_NAME.findall(registry.read_text(encoding="utf-8")))
    return set(re.findall(r'/\*\s*([A-Za-z_][\w:]*::[\w:]+)\s*\*/', text))


def proxy_components(curated: Path) -> set[str]:
    """Components whose ECS slot holds a pointer, not the object.

    RemoveComponent<T> does not apply, so they stay out of the dispatch table
    even when the binary carries a specialization.
    """
    rows = _CURATED_ROW.findall(curated.read_text(encoding="utf-8"))
    if not rows:
        sys.exit(f"error: no curated component rows parsed from {curated}\n"
                 "  the table's shape changed; update _CURATED_ROW")
    return {name for name, is_proxy in rows if is_proxy == "true"}


def emit(mapping: dict[str, int], build_id: str, total_specializations: int) -> str:
    lines = [
        "/* GENERATED - do not edit by hand.",
        " *",
        " * Per-build dispatch table for",
        " *   ecs::legacy::ImmediateWorldCache::RemoveComponent<T>(ls::ID<EntityHandleTraits>)",
        " *",
        " * macOS emits one specialization per component type, each with its TypeId<T>",
        " * baked in, and exposes no generic runtime-TypeIndex entry point -- which is",
        " * why entity:RemoveComponent was deferred. The specializations are exported",
        " * symbols, so the mapping is recoverable per build:",
        " *",
        " *   python3 tools/generate_remove_component.py --binary <game> --build-id <id>",
        " *",
        f" * Build {build_id}: {total_specializations} specializations exist; "
        f"{len(mapping)} of them correspond to a",
        " * component name registered by this port. Call as",
        " *   fn(*(void **)(EntityWorld + ENTITYWORLD_CACHE_OFFSET), entityHandle)",
        " */",
        "",
        "#ifndef GENERATED_REMOVE_COMPONENT_H",
        "#define GENERATED_REMOVE_COMPONENT_H",
        "",
        "/* X(componentName, removeFnVA) */",
        "#define GENERATED_REMOVE_COMPONENT_ENTRIES(X) \\",
    ]
    ordered = sorted(mapping.items())
    for i, (name, addr) in enumerate(ordered):
        cont = "" if i == len(ordered) - 1 else " \\"
        lines.append(f'    X("{name}", 0x{addr:x}ULL){cont}')
    lines += [
        "",
        f"#define GENERATED_REMOVE_COMPONENT_COUNT {len(mapping)}",
        "",
        "#endif /* GENERATED_REMOVE_COMPONENT_H */",
        "",
    ]
    return "\n".join(lines)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", required=True,
                    help="the real game binary (NOT the launcher stub)")
    ap.add_argument("--build-id", required=True, help='e.g. "4.1.1.7398727-gog"')
    ap.add_argument("--typeids", type=Path,
                    help="generated_typeids.h for this build "
                         "(default: src/gen/<store>/ inferred from --build-id)")
    ap.add_argument("--out", type=Path, help="write here instead of stdout")
    args = ap.parse_args(argv)

    binary = Path(args.binary).expanduser()
    if not binary.is_file():
        ap.error(f"binary not found: {binary}")

    typeids = args.typeids
    if typeids is None:
        store = "gog" if args.build_id.endswith("-gog") else "steam"
        typeids = PROJECT_ROOT / "src/gen" / store / "generated_typeids.h"
    if not typeids.is_file():
        ap.error(f"typeid authority not found: {typeids}\n"
                 "  generate it first with tools/extract_typeids.py")

    found = specializations(binary)
    known = registered_names(typeids)
    proxies = proxy_components(PROJECT_ROOT / "src/entity/component_typeid.c")
    mapping = {name: addr for name, addr in found.items()
               if name in known and name not in proxies}

    skipped = sorted(n for n in found if n in known and n in proxies)
    print(f"{len(found)} specializations in binary; "
          f"{len(known)} components registered; "
          f"{len(skipped)} proxy skipped; {len(mapping)} usable",
          file=sys.stderr)
    if skipped:
        print(f"  proxies skipped: {', '.join(skipped)}", file=sys.stderr)
    if not mapping:
        sys.exit("error: no specialization matched a registered component name")

    text = emit(mapping, args.build_id, len(found))
    if args.out:
        args.out.write_text(text, encoding="utf-8")
        print(f"wrote {args.out}", file=sys.stderr)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

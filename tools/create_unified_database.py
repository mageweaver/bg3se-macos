#!/usr/bin/env python3
"""
Create a unified component database from all sources.

Priority:
1. Ghidra ARM64 sizes (most accurate for macOS)
2. Windows BG3SE estimated sizes (fallback)
3. TypeIds without sizes (just addresses)

Output: ghidra/offsets/COMPONENT_DATABASE.md
"""

import os
import re
import json
from pathlib import Path
from collections import defaultdict
from dataclasses import dataclass
from typing import Dict, Optional

# File paths
WINDOWS_SIZES_JSON = Path("ghidra/offsets/windows_reference_sizes.json")
GHIDRA_SIZES_DIR = Path("ghidra/offsets/components")
# Generated tables are per store (src/gen/<store>/): one dylib build serves
# one game build. Defaults to the Steam tables; set BG3SE_GEN_STORE=gog to
# analyse the GOG set instead.
_GEN_STORE = os.environ.get("BG3SE_GEN_STORE", "steam")
TYPEIDS_HEADER = Path(f"src/gen/{_GEN_STORE}/generated_typeids.h")
OUTPUT_FILE = Path("ghidra/offsets/COMPONENT_DATABASE.md")


@dataclass
class ComponentEntry:
    name: str
    namespace: str
    ghidra_size: Optional[int] = None
    windows_size: Optional[int] = None
    typeid_address: Optional[str] = None
    is_tag: bool = False
    source: str = ""  # 'ghidra', 'windows', 'typeid'


def parse_ghidra_sizes() -> Dict[str, int]:
    """Parse Ghidra-extracted sizes from markdown files."""
    sizes = {}

    for md_file in GHIDRA_SIZES_DIR.glob("COMPONENT_SIZES*.md"):
        with open(md_file, 'r') as f:
            for line in f:
                # Skip header/separator lines
                if '---' in line or not line.strip().startswith('|'):
                    continue
                if '::' not in line:
                    continue

                parts = line.split('|')
                if len(parts) < 3:
                    continue

                name = parts[1].strip()
                size_field = parts[2].strip()

                # Skip headers and invalid names
                if name in ('Component', 'Name', 'Type') or '::' not in name:
                    continue

                # Skip malformed entries (namespace-only, "Not found", etc.)
                if size_field.startswith('~') or 'Not found' in size_field:
                    continue
                if size_field.startswith('ecl::') or size_field.startswith('esv::'):
                    continue

                # Extract numeric size from various formats:
                # - "0x28" -> 40 (hex)
                # - "40" -> 40 (decimal)
                # - "`0x1b0`" -> 432 (hex in backticks)
                # - "8 (ptr)" -> 8
                # - "2 (0x2)" -> 2

                # Remove backticks
                size_field = size_field.replace('`', '')

                # Remove parenthetical suffixes like "(ptr)" or "(0x2)"
                size_field = re.sub(r'\s*\([^)]*\)', '', size_field).strip()

                # Try to parse
                size = None
                if size_field.startswith('0x'):
                    try:
                        size = int(size_field, 16)
                    except ValueError:
                        pass
                else:
                    # Try decimal
                    match = re.match(r'^(\d+)', size_field)
                    if match:
                        try:
                            size = int(match.group(1))
                        except ValueError:
                            pass

                # Validate and store
                if size is not None and 0 < size <= 10000:
                    # Keep larger size if duplicate
                    if name not in sizes or size > sizes[name]:
                        sizes[name] = size

    return sizes


def load_windows_sizes() -> Dict[str, dict]:
    """Load Windows estimated sizes from JSON."""
    if not WINDOWS_SIZES_JSON.exists():
        return {}
    with open(WINDOWS_SIZES_JSON) as f:
        return json.load(f)


def parse_typeids() -> Dict[str, str]:
    """Parse TypeId addresses from generated_typeids.h."""
    typeids = {}

    if not TYPEIDS_HEADER.exists():
        return typeids

    pattern = re.compile(r'#define\s+TYPEID_(\w+)\s+(0x[0-9a-fA-F]+)')

    with open(TYPEIDS_HEADER, 'r') as f:
        for line in f:
            match = pattern.match(line.strip())
            if match:
                raw_name = match.group(1)
                addr = match.group(2)

                # Convert TYPEID_EOC_HEALTHCOMPONENT to eoc::HealthComponent
                parts = raw_name.split('_', 1)
                if len(parts) == 2:
                    ns = parts[0].lower()
                    comp = parts[1]
                    camel = ''.join(part.title() for part in comp.split('_'))
                    full_name = f"{ns}::{camel}"
                    typeids[full_name] = addr

    return typeids


def normalize_name(name: str) -> str:
    """Normalize component name for matching.

    Handles different naming conventions:
    - ecl::camera::CombatTargetComponent (Ghidra, multi-namespace)
    - ecl::CameraCombattargetcomponent (TypeId, flattened)
    - eoc::HealthComponent (common format)
    """
    # Remove template parameters
    name = re.sub(r'<[^>]+>', '', name).strip()
    # Lowercase everything for comparison
    name = name.lower()
    # Collapse multiple :: into single (for multi-namespace matching)
    # e.g., "ecl::camera::combattargetcomponent" -> still unique
    return name


def fuzzy_match_name(name1: str, name2: str) -> bool:
    """Check if two names match, handling namespace flattening."""
    n1 = normalize_name(name1)
    n2 = normalize_name(name2)

    if n1 == n2:
        return True

    # Try matching without middle namespaces
    # e.g., "ecl::camera::combattarget" vs "ecl::cameracombattarget"
    parts1 = n1.split('::')
    parts2 = n2.split('::')

    # Compare first namespace and collapsed rest
    if len(parts1) >= 1 and len(parts2) >= 1:
        if parts1[0] == parts2[0]:
            rest1 = ''.join(parts1[1:])
            rest2 = ''.join(parts2[1:])
            if rest1 == rest2:
                return True

    return False


def create_unified_database():
    """Create unified database merging all sources."""
    print("Loading sources...")

    ghidra = parse_ghidra_sizes()
    print(f"  Ghidra ARM64: {len(ghidra)} components")

    windows = load_windows_sizes()
    print(f"  Windows BG3SE: {len(windows)} components")

    typeids = parse_typeids()
    print(f"  TypeIds: {len(typeids)} components")

    # Build unified database
    database: Dict[str, ComponentEntry] = {}

    # First, add all TypeIds (base layer)
    for name, addr in typeids.items():
        ns = name.split('::')[0] if '::' in name else 'unknown'
        database[name] = ComponentEntry(
            name=name,
            namespace=ns,
            typeid_address=addr,
            source='typeid'
        )

    # Next, add/update from Windows (fallback sizes)
    for name, info in windows.items():
        # Find matching entry using fuzzy matching
        matched_key = None
        for key in database:
            if fuzzy_match_name(key, name):
                matched_key = key
                break

        if matched_key:
            database[matched_key].windows_size = info['estimated_size']
            database[matched_key].is_tag = info.get('is_tag', False)
            if database[matched_key].source == 'typeid':
                database[matched_key].source = 'windows'
        else:
            ns = name.split('::')[0] if '::' in name else 'unknown'
            database[name] = ComponentEntry(
                name=name,
                namespace=ns,
                windows_size=info['estimated_size'],
                is_tag=info.get('is_tag', False),
                source='windows'
            )

    # Finally, add/update from Ghidra (highest priority)
    for name, size in ghidra.items():
        # Find matching entry using fuzzy matching
        matched_key = None
        for key in database:
            if fuzzy_match_name(key, name):
                matched_key = key
                break

        if matched_key:
            database[matched_key].ghidra_size = size
            database[matched_key].source = 'ghidra'
        else:
            ns = name.split('::')[0] if '::' in name else 'unknown'
            database[name] = ComponentEntry(
                name=name,
                namespace=ns,
                ghidra_size=size,
                source='ghidra'
            )

    return database


def generate_markdown(database: Dict[str, ComponentEntry]) -> str:
    """Generate markdown documentation."""
    lines = [
        "# Unified Component Database",
        "",
        "Merged from all available sources:",
        "- **Ghidra ARM64**: Direct decompilation of macOS BG3 binary (highest priority)",
        "- **Windows BG3SE**: Estimated from C++ struct definitions (fallback)",
        "- **TypeIds**: Registered component addresses (no size info)",
        "",
    ]

    # Statistics
    total = len(database)
    with_ghidra = sum(1 for e in database.values() if e.ghidra_size is not None)
    with_windows = sum(1 for e in database.values() if e.windows_size is not None)
    with_any_size = sum(1 for e in database.values()
                       if e.ghidra_size is not None or e.windows_size is not None)
    tags = sum(1 for e in database.values() if e.is_tag)

    lines.extend([
        "## Statistics",
        "",
        f"| Metric | Count |",
        f"|--------|-------|",
        f"| Total components | {total} |",
        f"| With Ghidra ARM64 size | {with_ghidra} ({100*with_ghidra/total:.1f}%) |",
        f"| With Windows estimate | {with_windows} ({100*with_windows/total:.1f}%) |",
        f"| With any size info | {with_any_size} ({100*with_any_size/total:.1f}%) |",
        f"| Missing sizes | {total - with_any_size} |",
        f"| Tag components | {tags} |",
        "",
    ])

    # Group by namespace
    by_namespace = defaultdict(list)
    for entry in database.values():
        by_namespace[entry.namespace].append(entry)

    lines.extend([
        "## Coverage by Namespace",
        "",
        "| Namespace | Total | Ghidra | Windows | Missing |",
        "|-----------|-------|--------|---------|---------|",
    ])

    for ns in sorted(by_namespace.keys()):
        entries = by_namespace[ns]
        ns_total = len(entries)
        ns_ghidra = sum(1 for e in entries if e.ghidra_size is not None)
        ns_windows = sum(1 for e in entries if e.windows_size is not None)
        ns_missing = sum(1 for e in entries
                        if e.ghidra_size is None and e.windows_size is None)
        lines.append(f"| {ns} | {ns_total} | {ns_ghidra} | {ns_windows} | {ns_missing} |")

    lines.extend(["", "---", ""])

    # Detailed tables by namespace
    for ns in sorted(by_namespace.keys()):
        entries = sorted(by_namespace[ns], key=lambda e: e.name)

        lines.extend([
            f"## {ns}:: Components",
            "",
            "| Component | Ghidra | Windows | TypeId | Source |",
            "|-----------|--------|---------|--------|--------|",
        ])

        for entry in entries:
            ghidra_str = str(entry.ghidra_size) if entry.ghidra_size else "-"
            windows_str = str(entry.windows_size) if entry.windows_size else "-"
            typeid_str = entry.typeid_address if entry.typeid_address else "-"
            tag_marker = " (tag)" if entry.is_tag else ""

            # Truncate typeid for display
            if len(typeid_str) > 14:
                typeid_str = typeid_str[:14]

            short_name = entry.name.split('::')[-1] if '::' in entry.name else entry.name
            lines.append(f"| {short_name}{tag_marker} | {ghidra_str} | {windows_str} | {typeid_str} | {entry.source} |")

        lines.extend(["", "---", ""])

    return '\n'.join(lines)


def main():
    os.chdir(Path(__file__).parent.parent)

    database = create_unified_database()

    print(f"\nTotal unified entries: {len(database)}")

    # Generate and save markdown
    markdown = generate_markdown(database)

    with open(OUTPUT_FILE, 'w') as f:
        f.write(markdown)

    print(f"\nOutput written to: {OUTPUT_FILE}")

    # Summary
    with_ghidra = sum(1 for e in database.values() if e.ghidra_size is not None)
    with_any = sum(1 for e in database.values()
                  if e.ghidra_size is not None or e.windows_size is not None)

    print(f"\nCoverage:")
    print(f"  Ghidra ARM64 sizes: {with_ghidra} ({100*with_ghidra/len(database):.1f}%)")
    print(f"  Any size info: {with_any} ({100*with_any/len(database):.1f}%)")
    print(f"  Missing: {len(database) - with_any}")


if __name__ == "__main__":
    main()

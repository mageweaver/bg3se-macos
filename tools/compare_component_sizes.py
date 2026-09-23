#!/usr/bin/env python3
"""
Compare Windows BG3SE estimated sizes with Ghidra-extracted ARM64 sizes.

This script identifies:
1. Matches - Windows and ARM64 sizes agree
2. Discrepancies - Sizes differ (alignment differences between platforms)
3. Gaps - Components missing from one source or the other

Usage:
    python3 tools/compare_component_sizes.py
"""

import os
import re
import json
from pathlib import Path
from collections import defaultdict

# File paths
WINDOWS_SIZES_JSON = Path("ghidra/offsets/windows_reference_sizes.json")
GHIDRA_SIZES_DIR = Path("ghidra/offsets/components")
# Generated tables are per store (src/gen/<store>/): one dylib build serves
# one game build. Defaults to the Steam tables; set BG3SE_GEN_STORE=gog to
# analyse the GOG set instead.
_GEN_STORE = os.environ.get("BG3SE_GEN_STORE", "steam")
TYPEIDS_HEADER = Path(f"src/gen/{_GEN_STORE}/generated_typeids.h")


def load_windows_sizes() -> dict:
    """Load Windows estimated sizes from JSON."""
    if not WINDOWS_SIZES_JSON.exists():
        print(f"Error: {WINDOWS_SIZES_JSON} not found")
        return {}

    with open(WINDOWS_SIZES_JSON) as f:
        return json.load(f)


def parse_ghidra_sizes() -> dict:
    """Parse Ghidra-extracted sizes from markdown files."""
    sizes = {}

    # Pattern 1: Table format with hex and decimal
    # | eoc::HealthComponent | 0x28 | 40 | notes |
    table_pattern1 = re.compile(r'\|\s*([a-zA-Z0-9_:]+(?:::[a-zA-Z0-9_]+)+)\s*\|\s*0x[0-9a-fA-F]+\s*\|\s*(\d+)\s*\|')

    # Pattern 2: Table format with just decimal bytes
    # | Component | SIZE bytes |
    table_pattern2 = re.compile(r'\|\s*([a-zA-Z0-9_:]+(?:::[a-zA-Z0-9_]+)+)\s*\|\s*(\d+)\s+(?:bytes?)?\s*\|')

    # Pattern 3: Table with bytes in second column and notes
    # | Component | 8 | GetComponent |
    table_pattern3 = re.compile(r'\|\s*([a-zA-Z0-9_:]+(?:::[a-zA-Z0-9_]+)+)\s*\|\s*(\d+)\s*\|[^|]*\|')

    # Pattern 4: Table with hex in parens
    # | eoc::ObjectSizeComponent | 2 (0x2) | notes |
    table_pattern4 = re.compile(r'\|\s*([a-zA-Z0-9_:]+(?:::[a-zA-Z0-9_]+)+)\s*\|\s*(\d+)\s*\(0x[0-9a-fA-F]+\)\s*\|')

    # Pattern 5: List format
    # - Component: SIZE bytes
    list_pattern = re.compile(r'[-*]\s*`?([a-zA-Z0-9_:]+(?:::[a-zA-Z0-9_]+)+)`?\s*[:=]\s*(\d+)\s*(?:bytes?)?')

    # Pattern 6: GetComponent< format in notes
    # GetComponent<eoc::Foo,false> | SIZE bytes
    getcomp_pattern = re.compile(r'GetComponent<([^,>]+)')

    # Pattern 7: More lenient table format
    # Captures component name followed by a number somewhere on the same line
    lenient_pattern = re.compile(r'\|\s*([a-zA-Z0-9_:]+(?:::[a-zA-Z0-9_]+)+)\s*\|[^|]*?(\d{1,4})\s*(?:bytes|0x[0-9a-fA-F]+|\|)')

    for md_file in GHIDRA_SIZES_DIR.glob("*.md"):
        with open(md_file, 'r') as f:
            content = f.read()

        # Try each pattern
        for pattern in [table_pattern1, table_pattern2, table_pattern3,
                        table_pattern4, list_pattern, lenient_pattern]:
            for match in pattern.finditer(content):
                component_name = match.group(1).strip()
                try:
                    size = int(match.group(2))
                except (ValueError, IndexError):
                    continue

                # Skip obviously wrong entries
                if size == 0 or size > 10000:
                    continue

                # Skip header entries
                if component_name in ('Component', 'Name', 'Type'):
                    continue

                # Normalize component name
                base_name = re.sub(r'<[^>]+>', '', component_name)
                base_name = base_name.strip()

                if base_name and '::' in base_name:
                    # Keep the larger size if we see duplicates
                    if base_name not in sizes or size > sizes[base_name]:
                        sizes[base_name] = size

    return sizes


def parse_typeids() -> dict:
    """Parse TypeId addresses from generated_typeids.h."""
    typeids = {}

    if not TYPEIDS_HEADER.exists():
        print(f"Warning: {TYPEIDS_HEADER} not found")
        return typeids

    # Pattern: #define TYPEID_EOC_HEALTHCOMPONENT 0x108907888ULL
    pattern = re.compile(r'#define\s+TYPEID_(\w+)\s+(0x[0-9a-fA-F]+)')

    with open(TYPEIDS_HEADER, 'r') as f:
        for line in f:
            match = pattern.match(line.strip())
            if match:
                name = match.group(1)
                addr = match.group(2)
                # Convert TYPEID_EOC_HEALTHCOMPONENT to eoc::HealthComponent
                # Split by underscore, lowercase first part (namespace), titlecase rest
                parts = name.split('_', 1)
                if len(parts) == 2:
                    ns = parts[0].lower()
                    comp = parts[1]
                    # Convert underscore-separated to CamelCase
                    # E.g., HEALTH_COMPONENT -> HealthComponent
                    camel = ''.join(part.title() for part in comp.split('_'))
                    full_name = f"{ns}::{camel}"
                    typeids[full_name] = addr

    return typeids


def normalize_component_name(name: str) -> str:
    """Normalize component name for matching."""
    # Remove template parameters
    name = re.sub(r'<[^>]+>', '', name)
    # Remove leading/trailing whitespace
    name = name.strip()
    # Lowercase for comparison
    return name.lower()


def match_components(windows: dict, ghidra: dict, typeids: dict):
    """Match components across all sources."""
    results = {
        'matches': [],        # Windows and Ghidra agree
        'discrepancies': [],  # Different sizes
        'windows_only': [],   # In Windows but not Ghidra
        'ghidra_only': [],    # In Ghidra but not Windows
        'typeids_without_size': []  # TypeIds without any size info
    }

    # Create normalized name mappings
    windows_normalized = {}
    for name, info in windows.items():
        norm = normalize_component_name(name)
        windows_normalized[norm] = (name, info)

    ghidra_normalized = {}
    for name, size in ghidra.items():
        norm = normalize_component_name(name)
        ghidra_normalized[norm] = (name, size)

    # Find matches and discrepancies
    for norm_name, (win_name, win_info) in windows_normalized.items():
        if norm_name in ghidra_normalized:
            ghidra_name, ghidra_size = ghidra_normalized[norm_name]
            win_size = win_info['estimated_size']

            if win_size == ghidra_size:
                results['matches'].append({
                    'name': win_name,
                    'size': win_size,
                    'source': 'both'
                })
            else:
                results['discrepancies'].append({
                    'name': win_name,
                    'windows_size': win_size,
                    'ghidra_size': ghidra_size,
                    'diff': ghidra_size - win_size
                })
        else:
            results['windows_only'].append({
                'name': win_name,
                'size': win_info['estimated_size'],
                'is_tag': win_info.get('is_tag', False)
            })

    # Find Ghidra-only entries
    for norm_name, (ghidra_name, ghidra_size) in ghidra_normalized.items():
        if norm_name not in windows_normalized:
            results['ghidra_only'].append({
                'name': ghidra_name,
                'size': ghidra_size
            })

    # Find TypeIds without any size info
    all_sizes = set(normalize_component_name(n) for n in windows.keys())
    all_sizes.update(normalize_component_name(n) for n in ghidra.keys())

    for typeid_name, addr in typeids.items():
        norm = normalize_component_name(typeid_name)
        if norm not in all_sizes:
            results['typeids_without_size'].append({
                'name': typeid_name,
                'address': addr
            })

    return results


def print_report(results: dict, windows: dict, ghidra: dict, typeids: dict):
    """Print a summary report."""
    print("=" * 70)
    print("COMPONENT SIZE COMPARISON REPORT")
    print("=" * 70)
    print()

    print(f"Sources:")
    print(f"  Windows BG3SE estimated sizes: {len(windows)} components")
    print(f"  Ghidra ARM64 extracted sizes:  {len(ghidra)} components")
    print(f"  TypeId registrations:          {len(typeids)} components")
    print()

    print("-" * 70)
    print("SUMMARY")
    print("-" * 70)
    print(f"  Matches (same size):           {len(results['matches'])}")
    print(f"  Discrepancies (different):     {len(results['discrepancies'])}")
    print(f"  Windows only (not in Ghidra):  {len(results['windows_only'])}")
    print(f"  Ghidra only (not in Windows):  {len(results['ghidra_only'])}")
    print(f"  TypeIds without any size:      {len(results['typeids_without_size'])}")
    print()

    # Show some discrepancies
    if results['discrepancies']:
        print("-" * 70)
        print("SAMPLE DISCREPANCIES (Top 20 by absolute difference)")
        print("-" * 70)
        sorted_disc = sorted(results['discrepancies'],
                            key=lambda x: abs(x['diff']), reverse=True)
        for item in sorted_disc[:20]:
            print(f"  {item['name']}: Windows={item['windows_size']}, "
                  f"Ghidra={item['ghidra_size']} (diff={item['diff']:+d})")
        print()

    # Show Windows-only components (potential gaps in our extraction)
    if results['windows_only']:
        print("-" * 70)
        print("WINDOWS-ONLY COMPONENTS (gaps in Ghidra extraction)")
        print("-" * 70)

        # Group by namespace
        by_ns = defaultdict(list)
        for item in results['windows_only']:
            ns = item['name'].split('::')[0] if '::' in item['name'] else 'unknown'
            by_ns[ns].append(item)

        for ns, items in sorted(by_ns.items()):
            tag_count = sum(1 for i in items if i.get('is_tag', False))
            print(f"  {ns}: {len(items)} components ({tag_count} tag components)")

        print()

    # Show TypeIds without sizes
    if results['typeids_without_size']:
        print("-" * 70)
        print(f"TYPEIDS WITHOUT SIZES ({len(results['typeids_without_size'])} components)")
        print("-" * 70)

        # Group by namespace
        by_ns = defaultdict(list)
        for item in results['typeids_without_size']:
            ns = item['name'].split('::')[0] if '::' in item['name'] else 'unknown'
            by_ns[ns].append(item)

        for ns, items in sorted(by_ns.items()):
            print(f"  {ns}: {len(items)} components")

        print()
        print("  Sample (first 10):")
        for item in results['typeids_without_size'][:10]:
            print(f"    {item['name']} @ {item['address']}")


def main():
    os.chdir(Path(__file__).parent.parent)

    print("Loading Windows BG3SE sizes...")
    windows = load_windows_sizes()

    print("Parsing Ghidra-extracted ARM64 sizes...")
    ghidra = parse_ghidra_sizes()

    print("Parsing TypeId registrations...")
    typeids = parse_typeids()

    print("Matching components...")
    results = match_components(windows, ghidra, typeids)

    print()
    print_report(results, windows, ghidra, typeids)


if __name__ == "__main__":
    main()

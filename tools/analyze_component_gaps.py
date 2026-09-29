#!/usr/bin/env python3
"""
analyze_component_gaps.py - Compare TypeIds with extracted sizes

Shows which components have sizes and which are missing.
"""

import re
from collections import defaultdict
import os
from pathlib import Path

BASE_DIR = Path(__file__).parent.parent
# Generated tables are per store (src/gen/<store>/): one dylib build serves
# one game build. Defaults to the Steam tables; set BG3SE_GEN_STORE=gog to
# analyse the GOG set instead.
_GEN_STORE = os.environ.get("BG3SE_GEN_STORE", "steam")
TYPEIDS_FILE = BASE_DIR / f"src/gen/{_GEN_STORE}/generated_typeids.h"
COMPONENTS_DIR = BASE_DIR / "ghidra/offsets/components"

def extract_typeids():
    """Extract component names from generated_typeids.h"""
    components = {}
    pattern = re.compile(r'#define TYPEID_(\w+) (0x[0-9a-f]+)ULL')

    with open(TYPEIDS_FILE) as f:
        for line in f:
            match = pattern.match(line)
            if match:
                macro_name = match.group(1)
                addr = match.group(2)
                # Convert MACRO_NAME to namespace::ComponentName
                # e.g., EOC_HEALTHCOMPONENT -> eoc::HealthComponent
                parts = macro_name.split('_')
                ns = parts[0].lower()
                rest = '_'.join(parts[1:])
                # Try to reconstruct the name
                components[macro_name] = {
                    'namespace': ns,
                    'macro': macro_name,
                    'address': addr
                }
    return components

def extract_sizes():
    """Extract component names from size documentation"""
    components = set()
    pattern = re.compile(r'\|\s*(eoc|esv|ecl|ls|navcloud|gui|ecs)::[^\|]+\|')

    for filepath in COMPONENTS_DIR.glob("COMPONENT_SIZES_*.md"):
        with open(filepath) as f:
            for line in f:
                match = pattern.search(line)
                if match:
                    name = line.split('|')[1].strip()
                    if '::' in name and 'Component' in name:
                        # Normalize to uppercase macro style
                        macro = name.replace('::', '_').upper()
                        components.add(macro)
    return components

def main():
    print("=" * 60)
    print("Component Gap Analysis")
    print("=" * 60)

    typeids = extract_typeids()
    sizes = extract_sizes()

    print(f"\nTotal TypeIds: {len(typeids)}")
    print(f"Total sizes extracted: {len(sizes)}")

    # Find missing
    typeid_set = set(typeids.keys())
    missing = typeid_set - sizes
    extra = sizes - typeid_set

    print(f"Missing sizes: {len(missing)}")
    print(f"Extra (in sizes but not TypeIds): {len(extra)}")

    # Group by namespace
    by_ns = defaultdict(lambda: {'total': 0, 'have': 0, 'missing': []})

    for macro in typeids:
        ns = typeids[macro]['namespace']
        by_ns[ns]['total'] += 1
        if macro in sizes:
            by_ns[ns]['have'] += 1
        else:
            by_ns[ns]['missing'].append(macro)

    print("\n" + "=" * 60)
    print("Coverage by Namespace")
    print("=" * 60)
    print(f"{'Namespace':<12} {'Have':<8} {'Total':<8} {'Missing':<8} {'%':<8}")
    print("-" * 60)

    total_have = 0
    total_all = 0
    for ns in sorted(by_ns.keys(), key=lambda x: by_ns[x]['total'], reverse=True):
        data = by_ns[ns]
        pct = data['have'] / data['total'] * 100 if data['total'] > 0 else 0
        print(f"{ns}::{' '*(10-len(ns))} {data['have']:<8} {data['total']:<8} {len(data['missing']):<8} {pct:.1f}%")
        total_have += data['have']
        total_all += data['total']

    print("-" * 60)
    print(f"{'TOTAL':<12} {total_have:<8} {total_all:<8} {total_all-total_have:<8} {total_have/total_all*100:.1f}%")

    # Output missing by namespace for targeted extraction
    print("\n" + "=" * 60)
    print("Missing Components by Namespace (first 20 each)")
    print("=" * 60)

    for ns in sorted(by_ns.keys(), key=lambda x: len(by_ns[x]['missing']), reverse=True):
        missing_list = by_ns[ns]['missing']
        if not missing_list:
            continue
        print(f"\n{ns}:: ({len(missing_list)} missing)")
        for macro in sorted(missing_list)[:20]:
            print(f"  - {macro}")
        if len(missing_list) > 20:
            print(f"  ... and {len(missing_list) - 20} more")

if __name__ == "__main__":
    main()

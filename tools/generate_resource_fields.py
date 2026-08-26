#!/usr/bin/env python3
"""
Generate the GuidResource field layout table from upstream's GuidResources.h.

Every resource type is a plain struct laid out by declaration order, so the
offsets can be computed from the field types alone -- but only if *every* field
is sized correctly, including ones we never expose: one wrong size shifts
everything after it. So the size table below is the load-bearing part, and any
type missing from it truncates that struct rather than guessing (see TRUNCATION
below).

Sizes are for the macOS build (libc++, LLVM), which differs from upstream's
MSVC in one place that matters: std::string is 24 bytes, not 32.

Usage: tools/generate_resource_fields.py [upstream_root] > generated_resource_fields.c
"""

import re
import sys
import os

# (size, align, lua_kind) -- lua_kind None means "sized, but not exposed to Lua".
# Anything absent from this table is unsized and truncates its struct.
TYPES = {
    "bool":             (1, 1, "BOOL"),
    "uint8_t":          (1, 1, "U8"),
    "uint16_t":         (2, 2, "U16"),
    "uint32_t":         (4, 4, "U32"),
    "int32_t":          (4, 4, "I32"),
    "int":              (4, 4, "I32"),
    "float":            (4, 4, "F32"),
    "double":           (8, 8, "F64"),
    "FixedString":      (4, 4, "FIXEDSTRING"),
    "Guid":             (16, 8, "GUID"),
    "STDString":        (24, 8, "STDSTRING"),      # libc++ std::string, not MSVC's 32
    "TranslatedString": (16, 4, "TRANSLATEDSTRING"),
    "glm::vec3":        (12, 4, None),
    "glm::vec4":        (16, 4, None),
    "HashSet<FixedString>": (48, 8, "HASHSET_FIXEDSTRING"),  # StaticArray + 2x Array
    "std::optional<Guid>":  (24, 8, None),
    "std::optional<STDString>": (32, 8, None),
    # Enums -- underlying type from Enumerations/*.inl
    "AbilityId":             (1, 1, "U8"),
    "DiceSizeId":            (1, 1, "U8"),
    "SpellPrepareType":      (1, 1, "U8"),
    "SpellCooldownType":     (1, 1, "U8"),
    "SpellLearningStrategy": (1, 1, "U8"),
    "ProgressionType":       (1, 1, "U8"),
    "SpellMetaConditionType": (1, 1, "U8"),
    "ResourceReplenishType": (1, 1, "U8"),          # BITMASK(uint8_t)
    "AppearanceMaterialType": (4, 4, "U32"),
    "__int64":               (8, 8, None),
    "TypedInt":              (8, 4, None),          # uint8 + int32
    "TypedFloat":            (8, 4, None),          # uint8 + float
}

# Every Array<T> is the same 16-byte {buf, capacity, size} regardless of T, so
# element type only decides whether Lua can read it.
ARRAY_SIZE, ARRAY_ALIGN = 16, 8
ARRAY_ELEMS = {"Guid": "GUID", "FixedString": "FIXEDSTRING", "int32_t": "I32", "uint8_t": "U8"}

# HashMap<K,V> is HashSet<K> plus an Array<V>.
HASHMAP_RE = re.compile(r"^HashMap<")
VARIANT_RE = re.compile(r"^std::variant<")


def size_of(ctype):
    """(size, align, lua_kind) or None if we cannot size it."""
    if ctype in TYPES:
        return TYPES[ctype]
    m = re.match(r"^Array<(.+)>$", ctype)
    if m:
        elem = ARRAY_ELEMS.get(m.group(1).strip())
        return (ARRAY_SIZE, ARRAY_ALIGN, ("ARRAY_" + elem) if elem else None)
    if HASHMAP_RE.match(ctype):
        return (64, 8, None)
    # std::variant sizes depend on alternative alignment; not worth guessing.
    return None


FIELD_RE = re.compile(
    r"^((?:\[\[[^\]]*\]\]\s*)?)"          # attributes, e.g. [[bg3::hidden]]
    r"([A-Za-z_][\w:]*(?:\s*<[^;]*?>)?)"  # type
    r"\s+(\w+)\s*(\{[^;]*\})?\s*;")       # name, initialiser


def strip_nested_structs(body):
    """
    Remove nested `struct X { ... };` / enum / union definitions from a struct
    body. Their members are not fields of the outer struct -- Progression
    declares Spell, AddedSpell, Ability and friends before its real fields, and
    treating those as outer members put every offset in the wrong place (and
    produced duplicate names, which is how it was spotted).
    """
    out, i = [], 0
    while i < len(body):
        m = re.compile(r"\b(struct|union|enum(?:\s+class)?)\s+\w+[^;{]*\{").search(body, i)
        if not m:
            out.append(body[i:])
            break
        out.append(body[i:m.start()])
        depth, j = 0, m.end() - 1
        while j < len(body):
            if body[j] == "{":
                depth += 1
            elif body[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        # Skip past the closing brace and its trailing declarator/semicolon.
        k = body.find(";", j)
        i = (k + 1) if k != -1 else len(body)
    return "".join(out)


def parse(header):
    src = open(header).read()
    blocks = re.findall(
        r"struct\s+(\w+)\s*:\s*public\s+resource::GuidResource\s*\{(.*?)\n\};", src, re.S)
    out = []
    for name, body in blocks:
        body = strip_nested_structs(body)
        fields, offset, truncated = [], 0x18, None  # VMT(8) + ResourceUUID(16)
        for line in body.splitlines():
            l = line.strip()
            if not l or l.startswith("//") or l.startswith("static constexpr"):
                continue
            m = FIELD_RE.match(l)
            if not m:
                continue
            ctype, fname = m.group(2).strip(), m.group(3)
            info = size_of(ctype)
            if info is None:
                truncated = "%s %s" % (ctype, fname)
                break
            size, align, kind = info
            offset = (offset + align - 1) & ~(align - 1)
            if kind:
                fields.append((fname, offset, kind, ctype))
            offset += size
        out.append((name, fields, offset, truncated))
    return out


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "../upstream"
    header = os.path.join(root, "BG3Extender/GameDefinitions/GuidResources.h")
    structs = parse(header)

    print("// Generated by tools/generate_resource_fields.py - do not edit.")
    print("// Field offsets computed from upstream GuidResources.h declaration order,")
    print("// using macOS (libc++) type sizes. See the generator for the size table.")
    print('#include "staticdata_fields.h"')
    print("#include <stddef.h>")
    print()

    for name, fields, size, _ in structs:
        if not fields:
            continue
        print("static const ResourceField fields_%s[] = {" % name)
        for fname, off, kind, ctype in fields:
            print('    { "%s", 0x%02x, RF_%s },  // %s' % (fname, off, kind, ctype))
        print("};")
    print()

    print("const ResourceLayout g_resource_layouts[] = {")
    for name, fields, size, trunc in structs:
        if not fields:
            print('    { "%s", NULL, 0, 0x%02x },  // no readable fields' % (name, size))
            continue
        note = ("  // truncated at %s" % trunc) if trunc else ""
        print('    { "%s", fields_%s, %d, 0x%02x },%s'
              % (name, name, len(fields), size, note))
    print("};")
    print()
    print("const int g_resource_layout_count = %d;" % len(structs))

    ntrunc = sum(1 for _, _, _, t in structs if t)
    sys.stderr.write("%d structs, %d truncated, %d fields\n"
                     % (len(structs), ntrunc,
                        sum(len(f) for _, f, _, _ in structs)))
    for n, f, _, t in structs:
        if t:
            sys.stderr.write("  truncated %-40s after %d fields, at %s\n" % (n, len(f), t))


if __name__ == "__main__":
    main()

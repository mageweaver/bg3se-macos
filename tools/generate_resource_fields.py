#!/usr/bin/env python3
"""
Generate the GuidResource field layout table from upstream's GuidResources.h.

Every resource type is a plain struct laid out by declaration order, so the
offsets can be computed from the field types alone -- but only if *every* field
is sized correctly, including ones we never expose: one wrong size shifts
everything after it. So the size table below is the load-bearing part, and any
type missing from it truncates that struct rather than guessing (see TRUNCATION
below).

Sizes are for the macOS build. The one that matters most is STDString: upstream
declares it as a std::basic_string, but the shipped struct stores it in 16
bytes -- a pointer, a 32-bit size and a 32-bit capacity whose top bit marks the
long form, with short strings inline and their length in the last byte. That
was read off a live Progression (see plans/), not assumed.

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
    "STDString":        (16, 8, "STDSTRING"),      # Larian's compact string, not std::string
    "TranslatedString": (16, 4, "TRANSLATEDSTRING"),
    "glm::vec3":        (12, 4, "VEC3"),
    "glm::vec4":        (16, 4, "VEC4"),
    "HashSet<FixedString>": (48, 8, "HASHSET_FIXEDSTRING"),  # StaticArray + 2x Array
    "HashMap<uint8_t, FixedString>": (64, 8, "HASHMAP_U8_FIXEDSTRING"),
    # std::optional<T> is T followed by an engaged flag, rounded to T's alignment.
    "std::optional<Guid>":      (24, 8, "OPT_GUID"),
    "std::optional<STDString>": (24, 8, "OPT_STDSTRING"),
    "std::optional<uint64_t>":  (16, 8, None),
    "std::optional<int>":       (8, 4, "OPT_I32"),
    "std::optional<AbilityId>": (2, 1, "OPT_U8"),
    # libc++ lays a variant out as the union of its alternatives followed by a
    # one-byte index, rounded to the widest alternative's alignment.
    "std::variant<NoValue, float, int, FixedString, bool>": (8, 4, "VARIANT_SCALAR"),
    "std::variant<int32_t, DiceRoll>":                      (8, 4, None),
    "std::variant<float, glm::vec3, glm::vec4, FixedString>": (20, 4, None),
    "NoValue":          (1, 1, None),
    "DiceRoll":         (2, 1, None),
    "MultiEffectFlags": (4, 4, "U32"),
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
ARRAY_ELEMS = {"Guid": "GUID", "FixedString": "FIXEDSTRING", "int32_t": "I32", "uint8_t": "U8",
               "AbilityId": "U8", "SkillId": "U8"}

# HashMap<K,V> is HashSet<K> plus an Array<V>.
HASHMAP_RE = re.compile(r"^HashMap<")
VARIANT_RE = re.compile(r"^std::variant<")


def size_of(ctype, nested=None):
    """(size, align, lua_kind) or None if we cannot size it."""
    if ctype in TYPES:
        return TYPES[ctype]
    m = re.match(r"^Array<(.+)>$", ctype)
    if m:
        inner = m.group(1).strip()
        elem = ARRAY_ELEMS.get(inner)
        if elem:
            return (ARRAY_SIZE, ARRAY_ALIGN, "ARRAY_" + elem)
        # Array of a struct: readable as an array of field proxies.
        if nested:
            for candidate in (inner, inner.split("::")[-1]):
                if candidate in nested:
                    return (ARRAY_SIZE, ARRAY_ALIGN, "ARRAY_STRUCT:" + candidate)
        return (ARRAY_SIZE, ARRAY_ALIGN, None)
    if HASHMAP_RE.match(ctype):
        return (64, 8, None)
    # std::variant sizes depend on alternative alignment; not worth guessing.
    return None


FIELD_RE = re.compile(
    r"^((?:\[\[[^\]]*\]\]\s*)?)"          # attributes, e.g. [[bg3::hidden]]
    r"([A-Za-z_][\w:]*(?:\s*<[^;]*?>)?)"  # type
    r"\s+(\w+)\s*(\{[^;]*\})?\s*;")       # name, initialiser


def collect_nested_structs(body):
    """
    Return {name: body} for each `struct X { ... };` nested in a struct body.

    These are element types, not members: Progression declares Spell, Skill,
    Equipment and friends and then holds Array<Spell>, Array<Skill> and so on.
    Laying them out is what lets those arrays be read.
    """
    found = {}
    i = 0
    while i < len(body):
        m = re.compile(r"\bstruct\s+(\w+)[^;{]*\{").search(body, i)
        if not m:
            break
        depth, j = 0, m.end() - 1
        while j < len(body):
            if body[j] == "{":
                depth += 1
            elif body[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        found[m.group(1)] = body[m.end():j]
        k = body.find(";", j)
        i = (k + 1) if k != -1 else len(body)
    return found


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


def layout_fields(body, start_offset, nested_sizes):
    """Walk a struct body assigning offsets. Returns (fields, size, truncated)."""
    fields, offset, truncated, max_align = [], start_offset, None, 1
    for line in body.splitlines():
        l = line.strip()
        if not l or l.startswith("//") or l.startswith("static constexpr"):
            continue
        m = FIELD_RE.match(l)
        if not m:
            continue
        ctype, fname = m.group(2).strip(), m.group(3)
        info = size_of(ctype, nested_sizes)
        if info is None:
            truncated = "%s %s" % (ctype, fname)
            break
        size, align, kind = info
        offset = (offset + align - 1) & ~(align - 1)
        max_align = max(max_align, align)
        if kind:
            fields.append((fname, offset, kind, ctype))
        offset += size
    # Round the struct out to its own alignment; this is the array stride.
    offset = (offset + max_align - 1) & ~(max_align - 1)
    return fields, offset, truncated


def collect_element_types(src, blocks):
    """
    Every struct that can appear as an Array element, by the names a field might
    use for it.

    Element types come from two places: helper structs declared at file scope
    (EffectInfo, FeatRequirement) and structs nested inside a resource. A nested
    one is reachable from other resources by its qualified name -- several
    fields say Array<Progression::Spell> from outside Progression -- so it is
    registered both ways.
    """
    bodies = {}

    resource_names = {n for n, _ in blocks}
    for m in re.finditer(r"struct\s+(\w+)\s*\{(.*?)\n\};", src, re.S):
        name = m.group(1)
        if name in resource_names:
            continue
        bodies.setdefault(name, m.group(2))

    for name, body in blocks:
        for nname, nbody in collect_nested_structs(body).items():
            bodies["%s::%s" % (name, nname)] = nbody
            bodies.setdefault(nname, nbody)

    return bodies


def parse(header):
    src = open(header).read()
    blocks = re.findall(
        r"struct\s+(\w+)\s*:\s*public\s+resource::GuidResource\s*\{(.*?)\n\};", src, re.S)
    element_bodies = collect_element_types(src, blocks)

    # Size every element type first; a resource's Array<X> needs X's stride.
    element_sizes = {}
    for ename, ebody in element_bodies.items():
        _, esize, _ = layout_fields(strip_nested_structs(ebody), 0, None)
        element_sizes[ename] = esize

    element_layouts = {}
    for ename, ebody in element_bodies.items():
        efields, esize, _ = layout_fields(strip_nested_structs(ebody), 0, element_sizes)
        element_layouts[ename] = (efields, esize)

    out = []
    for name, body in blocks:
        fields, size, truncated = layout_fields(strip_nested_structs(body), 0x18, element_sizes)
        used = sorted({k.split(":", 1)[1] for _, _, k, _ in fields if k.startswith("ARRAY_STRUCT:")})
        out.append((name, fields, size, truncated,
                    [(u, element_layouts[u][0], element_layouts[u][1]) for u in used
                     if element_layouts.get(u, ([], 0))[0]]))
    return out


def c_ident(name):
    """A qualified element name like Progression::Spell is not a C identifier."""
    return name.replace("::", "_")


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

    # Nested element layouts first: an outer field points at one of these.
    for name, fields, size, _, nested in structs:
        for nname, nfields, nsize in nested:
            if not nfields:
                continue
            print("static const ResourceField fields_%s_%s[] = {" % (name, c_ident(nname)))
            for fname, off, kind, ctype in nfields:
                if kind.startswith("ARRAY_STRUCT:"):
                    # An element type holding its own struct arrays: sized so the
                    # fields after it are right, but not followed further.
                    print('    { "%s", 0x%02x, RF_ARRAY_STRUCT, NULL, 0 },  // %s (not followed)'
                          % (fname, off, ctype))
                    continue
                print('    { "%s", 0x%02x, RF_%s, NULL, 0 },  // %s' % (fname, off, kind, ctype))
            print("};")
            print("static const ResourceLayout layout_%s_%s = "
                  '{ "%s", fields_%s_%s, %d, 0x%02x, true };'
                  % (name, c_ident(nname), nname, name, c_ident(nname), len(nfields), nsize))
    print()

    for name, fields, size, _, nested in structs:
        if not fields:
            continue
        have = {n for n, nf, _ in nested if nf}
        print("static const ResourceField fields_%s[] = {" % name)
        for fname, off, kind, ctype in fields:
            if kind.startswith("ARRAY_STRUCT:"):
                elem = kind.split(":", 1)[1]
                if elem in have:
                    esize = next(ns for n, nf, ns in nested if n == elem)
                    print('    { "%s", 0x%02x, RF_ARRAY_STRUCT, &layout_%s_%s, 0x%02x },  // %s'
                          % (fname, off, name, c_ident(elem), esize, ctype))
                    continue
                print('    { "%s", 0x%02x, RF_ARRAY_STRUCT, NULL, 0 },  // %s (no readable fields)'
                      % (fname, off, ctype))
                continue
            print('    { "%s", 0x%02x, RF_%s, NULL, 0 },  // %s' % (fname, off, kind, ctype))
        print("};")
    print()

    print("const ResourceLayout g_resource_layouts[] = {")
    for name, fields, size, trunc, _ in structs:
        if not fields:
            print('    { "%s", NULL, 0, 0x%02x, false },  // no readable fields' % (name, size))
            continue
        note = ("  // truncated at %s" % trunc) if trunc else ""
        print('    { "%s", fields_%s, %d, 0x%02x, false },%s'
              % (name, name, len(fields), size, note))
    print("};")
    print()
    print("const int g_resource_layout_count = %d;" % len(structs))

    ntrunc = sum(1 for _, _, _, t, _ in structs if t)
    sys.stderr.write("%d structs, %d truncated, %d fields\n"
                     % (len(structs), ntrunc,
                        sum(len(f) for _, f, _, _, _ in structs)))
    for n, f, _, t, _ in structs:
        if t:
            sys.stderr.write("  truncated %-40s after %d fields, at %s\n" % (n, len(f), t))


if __name__ == "__main__":
    main()

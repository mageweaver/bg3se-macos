#!/usr/bin/env python3
"""Derive BG3 anonymous globals by named ARM64 reference roles.

This is a report-only migration aid.  It never edits source or emits an
applyable patch.  The two supported recipes are intentionally narrow:

* global_switches_ptr: the sole ADRP+STR pointer slot in the named
  App::CreateGlobalSwitches function, corroborated by named load consumers.
* osiris_interface_ptr: the sole ADRP+LDR pointer slot in the named
  osi::OsirisInterface::OsirisQuery prologue whose loaded register is followed
  by CBZ, corroborated by direct references and named callers.

The old binary is a mandatory self-test.  A result is reported as resolved
only when its role candidate is unique and has independent corroboration.
No address delta is calculated or applied.
"""

from __future__ import annotations

import argparse
import bisect
import mmap
import re
import struct
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional

# Reuse the repository's relocation masking rules rather than introducing a
# second masked ARM64 signature implementation.
from sig_scan_functors import mask_for


FAT_MAGIC = 0xCAFEBABE
MH_MAGIC_64 = 0xFEEDFACF
CPU_TYPE_ARM64 = 0x0100000C
LC_SEGMENT_64 = 0x19

SYMBOLS = {
    "global_switches_ptr": "__ZN3App20CreateGlobalSwitchesEv",
    "osiris_interface_ptr": (
        "__ZN3osi15OsirisInterface11OsirisQueryEjP16COsiArgumentDesc"
    ),
}
EXPECTED_OLD = {
    "global_switches_ptr": 0x108AF4F30,
    "osiris_interface_ptr": 0x108A86128,
}
ROLE_WINDOW_INSTRUCTIONS = {
    "global_switches_ptr": 43,  # entry through the normal RET
    "osiris_interface_ptr": 23,  # entry through the first query-table load
}


def sign_extend(value: int, bits: int) -> int:
    sign = 1 << (bits - 1)
    return (value ^ sign) - sign


@dataclass(frozen=True)
class Symbol:
    address: int
    kind: str
    name: str


@dataclass(frozen=True)
class Segment:
    name: str
    vmaddr: int
    vmsize: int
    fileoff: int
    filesize: int


@dataclass(frozen=True)
class Adrp:
    pc: int
    word: int
    rd: int
    immhi: int
    immlo: int
    signed_imm21: int
    page: int


@dataclass(frozen=True)
class MemoryRef:
    pc: int
    word: int
    operation: str
    rt: int
    rn: int
    imm12: int
    scale: int
    byte_offset: int


@dataclass(frozen=True)
class Pair:
    adrp: Adrp
    memory: MemoryRef
    target: int


@dataclass
class Resolution:
    target_name: str
    anchor: Symbol
    role_candidates: list[Pair]
    selected: Optional[Pair]
    references: list[Pair]
    callers: list[int]
    corroboration: list[str]
    unresolved_reason: Optional[str] = None


class MachOImage:
    def __init__(self, path: Path, label: str):
        self.path = path
        self.label = label
        self._file = path.open("rb")
        self.data = mmap.mmap(self._file.fileno(), 0, access=mmap.ACCESS_READ)
        self.slice_offset, self.slice_size = self._find_arm64_slice()
        self.segments, self.text_vmaddr, self.text_size = self._load_segments()
        self.symbols = self._load_symbols()
        self._symbol_addresses = [symbol.address for symbol in self.symbols]
        self._demangled: dict[str, str] = {}

    def close(self) -> None:
        self.data.close()
        self._file.close()

    def __enter__(self) -> "MachOImage":
        return self

    def __exit__(self, *_args: object) -> None:
        self.close()

    def _find_arm64_slice(self) -> tuple[int, int]:
        if len(self.data) < 8:
            raise ValueError(f"{self.path}: file is too short")
        magic_be = struct.unpack_from(">I", self.data, 0)[0]
        if magic_be == FAT_MAGIC:
            nfat = struct.unpack_from(">I", self.data, 4)[0]
            for index in range(nfat):
                offset = 8 + index * 20
                cputype, _subtype, fileoff, size, _align = struct.unpack_from(
                    ">iiIII", self.data, offset
                )
                if cputype & 0xFFFFFFFF == CPU_TYPE_ARM64:
                    return fileoff, size
            raise ValueError(f"{self.path}: fat binary has no arm64 slice")
        if struct.unpack_from("<I", self.data, 0)[0] == MH_MAGIC_64:
            return 0, len(self.data)
        raise ValueError(f"{self.path}: unsupported Mach-O format")

    def _load_segments(self) -> tuple[list[Segment], int, int]:
        start = self.slice_offset
        if struct.unpack_from("<I", self.data, start)[0] != MH_MAGIC_64:
            raise ValueError(f"{self.path}: arm64 slice is not MH_MAGIC_64")
        ncmds = struct.unpack_from("<I", self.data, start + 16)[0]
        cursor = start + 32
        segments: list[Segment] = []
        text_vmaddr = text_size = None
        for _ in range(ncmds):
            cmd, cmdsize = struct.unpack_from("<II", self.data, cursor)
            if cmdsize < 8:
                raise ValueError(f"{self.path}: malformed load command")
            if cmd == LC_SEGMENT_64:
                name = bytes(self.data[cursor + 8 : cursor + 24]).rstrip(b"\0").decode()
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from(
                    "<QQQQ", self.data, cursor + 24
                )
                segments.append(Segment(name, vmaddr, vmsize, fileoff, filesize))
                nsects = struct.unpack_from("<I", self.data, cursor + 64)[0]
                section_cursor = cursor + 72
                for _section in range(nsects):
                    section_name = bytes(
                        self.data[section_cursor : section_cursor + 16]
                    ).rstrip(b"\0").decode()
                    segment_name = bytes(
                        self.data[section_cursor + 16 : section_cursor + 32]
                    ).rstrip(b"\0").decode()
                    if section_name == "__text" and segment_name == "__TEXT":
                        text_vmaddr, text_size = struct.unpack_from(
                            "<QQ", self.data, section_cursor + 32
                        )
                    section_cursor += 80
            cursor += cmdsize
        if text_vmaddr is None or text_size is None:
            raise ValueError(f"{self.path}: __TEXT,__text not found")
        return segments, text_vmaddr, text_size

    def _load_symbols(self) -> list[Symbol]:
        proc = subprocess.run(
            ["nm", "-arch", "arm64", "-n", str(self.path)],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if proc.returncode:
            raise RuntimeError(f"nm failed for {self.path}: {proc.stderr.strip()}")
        pattern = re.compile(r"^([0-9A-Fa-f]{8,16})\s+(\S)\s+(.+)$")
        symbols = []
        for line in proc.stdout.splitlines():
            match = pattern.match(line)
            if match:
                symbols.append(
                    Symbol(int(match.group(1), 16), match.group(2), match.group(3))
                )
        symbols.sort(key=lambda symbol: symbol.address)
        return symbols

    def exact_symbol(self, name: str) -> Symbol:
        matches = [symbol for symbol in self.symbols if symbol.name == name]
        if len(matches) != 1:
            raise ValueError(
                f"{self.path}: expected exactly one nm symbol {name!r}, "
                f"found {len(matches)}"
            )
        return matches[0]

    def symbol_after(self, address: int) -> Optional[Symbol]:
        index = bisect.bisect_right(self._symbol_addresses, address)
        return self.symbols[index] if index < len(self.symbols) else None

    def owner(self, address: int) -> Optional[Symbol]:
        index = bisect.bisect_right(self._symbol_addresses, address) - 1
        return self.symbols[index] if index >= 0 else None

    def demangle(self, name: str) -> str:
        if name in self._demangled:
            return self._demangled[name]
        proc = subprocess.run(
            ["c++filt"],
            input=name + "\n",
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        result = proc.stdout.strip() if proc.returncode == 0 else name
        self._demangled[name] = result or name
        return self._demangled[name]

    def file_offset_for_va(self, va: int) -> int:
        for segment in self.segments:
            if segment.vmaddr <= va < segment.vmaddr + segment.filesize:
                return self.slice_offset + segment.fileoff + (va - segment.vmaddr)
        raise ValueError(f"{self.path}: VA 0x{va:x} is not file-backed")

    def word(self, va: int) -> int:
        return struct.unpack_from("<I", self.data, self.file_offset_for_va(va))[0]

    def words(self, va: int, count: int) -> list[int]:
        return [self.word(va + index * 4) for index in range(count)]

    def text_words(self) -> Iterable[tuple[int, int]]:
        file_offset = self.file_offset_for_va(self.text_vmaddr)
        for relative in range(0, self.text_size - 3, 4):
            yield self.text_vmaddr + relative, struct.unpack_from(
                "<I", self.data, file_offset + relative
            )[0]


def decode_adrp(word: int, pc: int) -> Optional[Adrp]:
    if word & 0x9F000000 != 0x90000000:
        return None
    immlo = (word >> 29) & 0x3
    immhi = (word >> 5) & 0x7FFFF
    signed_imm21 = sign_extend((immhi << 2) | immlo, 21)
    return Adrp(
        pc=pc,
        word=word,
        rd=word & 0x1F,
        immhi=immhi,
        immlo=immlo,
        signed_imm21=signed_imm21,
        page=(pc & ~0xFFF) + (signed_imm21 << 12),
    )


def decode_memory64_unsigned(word: int, pc: int) -> Optional[MemoryRef]:
    opcode = word & 0xFFC00000
    if opcode == 0xF9400000:
        operation = "LDR"
    elif opcode == 0xF9000000:
        operation = "STR"
    else:
        return None
    imm12 = (word >> 10) & 0xFFF
    return MemoryRef(
        pc=pc,
        word=word,
        operation=operation,
        rt=word & 0x1F,
        rn=(word >> 5) & 0x1F,
        imm12=imm12,
        scale=8,
        byte_offset=imm12 * 8,
    )


def decode_pair(image: MachOImage, pc: int) -> Optional[Pair]:
    adrp = decode_adrp(image.word(pc), pc)
    memory = decode_memory64_unsigned(image.word(pc + 4), pc + 4)
    if adrp is None or memory is None or memory.rn != adrp.rd:
        return None
    return Pair(adrp, memory, adrp.page + memory.byte_offset)


def is_cbz_on_register(word: int, register: int) -> bool:
    # CBZ (32- or 64-bit); exclude CBNZ because the role in OsirisQuery is the
    # null failure guard, while a later generic allocator guard uses CBNZ.
    return word & 0x7F000000 == 0x34000000 and word & 0x1F == register


def function_pairs(image: MachOImage, anchor: Symbol, limit: int) -> list[Pair]:
    following = image.symbol_after(anchor.address)
    end = min(anchor.address + limit, following.address if following else anchor.address + limit)
    pairs = []
    for pc in range(anchor.address, end - 4, 4):
        pair = decode_pair(image, pc)
        if pair is not None:
            pairs.append(pair)
    return pairs


def direct_references(image: MachOImage, target: int) -> list[Pair]:
    references = []
    previous_pc = previous_word = None
    for pc, word in image.text_words():
        if previous_pc is not None:
            adrp = decode_adrp(previous_word, previous_pc)
            memory = decode_memory64_unsigned(word, pc)
            if adrp is not None and memory is not None and memory.rn == adrp.rd:
                pair = Pair(adrp, memory, adrp.page + memory.byte_offset)
                if pair.target == target:
                    references.append(pair)
        previous_pc, previous_word = pc, word
    return references


def bl_target(word: int, pc: int) -> Optional[int]:
    if word & 0xFC000000 != 0x94000000:
        return None
    immediate = sign_extend(word & 0x03FFFFFF, 26)
    return pc + (immediate << 2)


def direct_callers(image: MachOImage, target: int) -> list[int]:
    return [
        pc
        for pc, word in image.text_words()
        if bl_target(word, pc) == target
    ]


def resolve_global_switches(image: MachOImage) -> Resolution:
    anchor = image.exact_symbol(SYMBOLS["global_switches_ptr"])
    pairs = function_pairs(image, anchor, 0x200)
    candidates = [pair for pair in pairs if pair.memory.operation == "STR"]
    selected = candidates[0] if len(candidates) == 1 else None
    references: list[Pair] = []
    callers = direct_callers(image, anchor.address)
    corroboration: list[str] = []
    reason = None
    if selected is None:
        reason = f"writer role has {len(candidates)} candidates (requires exactly 1)"
    else:
        references = direct_references(image, selected.target)
        load_owners = {
            owner.name
            for pair in references
            if pair.memory.operation == "LDR"
            for owner in [image.owner(pair.adrp.pc)]
            if owner is not None and owner.name != anchor.name
        }
        if not load_owners:
            reason = "unique writer lacks an independent named load consumer"
            selected = None
        else:
            corroboration.append(
                f"{len(references)} adjacent direct references include "
                f"{sum(pair.memory.operation == 'LDR' for pair in references)} loads"
            )
            corroboration.append(
                f"{len(load_owners)} distinct named load consumers differ from the writer"
            )
            if callers:
                corroboration.append(
                    f"the named writer has {len(callers)} direct BL caller(s)"
                )
    return Resolution(
        "global_switches_ptr",
        anchor,
        candidates,
        selected,
        references,
        callers,
        corroboration,
        reason,
    )


def resolve_osiris_interface(image: MachOImage) -> Resolution:
    anchor = image.exact_symbol(SYMBOLS["osiris_interface_ptr"])
    pairs = function_pairs(image, anchor, 0x100)
    candidates = [
        pair
        for pair in pairs
        if pair.memory.operation == "LDR"
        and is_cbz_on_register(image.word(pair.memory.pc + 4), pair.memory.rt)
    ]
    selected = candidates[0] if len(candidates) == 1 else None
    references: list[Pair] = []
    callers = direct_callers(image, anchor.address)
    corroboration: list[str] = []
    reason = None
    if selected is None:
        reason = f"OsirisQuery null-guard role has {len(candidates)} candidates (requires exactly 1)"
    else:
        references = direct_references(image, selected.target)
        reference_owners = {
            owner.name
            for pair in references
            for owner in [image.owner(pair.adrp.pc)]
            if owner is not None
        }
        independent_owners = reference_owners - {anchor.name}
        writer_owners = {
            owner.name
            for pair in references
            if pair.memory.operation == "STR"
            for owner in [image.owner(pair.adrp.pc)]
            if owner is not None
        }
        if not independent_owners or not writer_owners:
            reason = (
                "unique prologue slot lacks independent named reference and "
                "writer corroboration"
            )
            selected = None
        else:
            corroboration.append(
                f"{len(references)} adjacent direct reference(s) from "
                f"{len(reference_owners)} named function(s)"
            )
            corroboration.append(
                f"{len(independent_owners)} named functions independently use the slot; "
                f"{len(writer_owners)} named function(s) write it"
            )
            corroboration.append(
                f"direct BL callers={len(callers)} (informational; interface dispatch "
                "does not require a direct call edge)"
            )
    return Resolution(
        "osiris_interface_ptr",
        anchor,
        candidates,
        selected,
        references,
        callers,
        corroboration,
        reason,
    )


def masked_compare(
    old: MachOImage, new: MachOImage, old_anchor: Symbol, new_anchor: Symbol, count: int
) -> tuple[int, list[int]]:
    old_words = old.words(old_anchor.address, count)
    new_words = new.words(new_anchor.address, count)
    mismatches = []
    old_previous = new_previous = None
    for index, (old_word, new_word) in enumerate(zip(old_words, new_words)):
        old_mask, old_previous = mask_for(old_word, old_previous)
        new_mask, new_previous = mask_for(new_word, new_previous)
        mask = old_mask & new_mask
        if old_word & mask != new_word & mask:
            mismatches.append(index)
    return count - len(mismatches), mismatches


def owner_label(image: MachOImage, address: int) -> str:
    owner = image.owner(address)
    if owner is None:
        return "<no nm owner>"
    return f"{image.demangle(owner.name)}+0x{address - owner.address:x}"


def format_pair(pair: Pair) -> list[str]:
    return [
        (
            f"    0x{pair.adrp.pc:x}: word=0x{pair.adrp.word:08x} "
            f"ADRP x{pair.adrp.rd}, immhi=0x{pair.adrp.immhi:x}, "
            f"immlo={pair.adrp.immlo}, signed_imm21={pair.adrp.signed_imm21} "
            f"=> page 0x{pair.adrp.page:x}"
        ),
        (
            f"    0x{pair.memory.pc:x}: word=0x{pair.memory.word:08x} "
            f"{pair.memory.operation} x{pair.memory.rt}, "
            f"[x{pair.memory.rn}, #0x{pair.memory.byte_offset:x}] "
            f"(imm12=0x{pair.memory.imm12:x}, scale={pair.memory.scale}) "
            f"=> slot 0x{pair.target:x}"
        ),
    ]


def print_resolution(image: MachOImage, resolution: Resolution) -> None:
    print(f"target: {resolution.target_name}")
    print(
        f"  named_anchor: 0x{resolution.anchor.address:x} "
        f"{image.demangle(resolution.anchor.name)} [{resolution.anchor.kind}]"
    )
    print(f"  role_candidate_count: {len(resolution.role_candidates)}")
    for index, pair in enumerate(resolution.role_candidates, 1):
        print(f"  role_candidate_{index}:")
        for line in format_pair(pair):
            print(line)
    if resolution.selected is None:
        print("  status: UNRESOLVED")
        print(f"  reason: {resolution.unresolved_reason}")
    else:
        print(f"  candidate_va: 0x{resolution.selected.target:x}")
        loads = sum(pair.memory.operation == "LDR" for pair in resolution.references)
        stores = sum(pair.memory.operation == "STR" for pair in resolution.references)
        print(
            f"  direct_adjacent_refs: total={len(resolution.references)} "
            f"loads={loads} stores={stores}"
        )
        owners: dict[str, int] = {}
        for pair in resolution.references:
            label = owner_label(image, pair.adrp.pc)
            owners[label] = owners.get(label, 0) + 1
        for label, count in sorted(owners.items())[:12]:
            print(f"    ref_owner: {label} ({count})")
        if len(owners) > 12:
            print(f"    ref_owner: ... {len(owners) - 12} more")
        nearby_named_refs = []
        seen_ref_owners = set()
        for pair in sorted(
            resolution.references,
            key=lambda item: abs(item.adrp.pc - resolution.anchor.address),
        ):
            owner = image.owner(pair.adrp.pc)
            if owner is None or owner.name == resolution.anchor.name:
                continue
            if owner.name in seen_ref_owners:
                continue
            seen_ref_owners.add(owner.name)
            nearby_named_refs.append((pair, owner))
            if len(nearby_named_refs) == 8:
                break
        for pair, owner in nearby_named_refs:
            print(
                f"    named_ref: 0x{pair.adrp.pc:x} {pair.memory.operation} "
                f"{image.demangle(owner.name)}+0x{pair.adrp.pc - owner.address:x}"
            )
        print(f"  direct_anchor_callers: {len(resolution.callers)}")
        for address in resolution.callers[:12]:
            print(f"    caller: 0x{address:x} {owner_label(image, address)}")
        if len(resolution.callers) > 12:
            print(f"    caller: ... {len(resolution.callers) - 12} more")
        for item in resolution.corroboration:
            print(f"  corroboration: {item}")
        print("  status: RESOLVED_UNIQUE")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Report uniquely derived anonymous BG3 globals; never edit source."
    )
    parser.add_argument("--old", required=True, type=Path, help="old fat/thin Mach-O")
    parser.add_argument("--new", required=True, type=Path, help="new fat/thin Mach-O")
    parser.add_argument(
        "--target",
        action="append",
        choices=sorted(SYMBOLS),
        help="target to resolve (repeatable; default: both)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    targets = args.target or sorted(SYMBOLS)
    resolvers = {
        "global_switches_ptr": resolve_global_switches,
        "osiris_interface_ptr": resolve_osiris_interface,
    }
    with MachOImage(args.old, "old") as old, MachOImage(args.new, "new") as new:
        print("anonymous-global migration report (report-only; no source writes)")
        print(
            f"old: {old.path} arm64_slice_offset=0x{old.slice_offset:x} "
            f"size=0x{old.slice_size:x}"
        )
        print(
            f"new: {new.path} arm64_slice_offset=0x{new.slice_offset:x} "
            f"size=0x{new.slice_size:x}"
        )

        old_results: dict[str, Resolution] = {}
        new_results: dict[str, Resolution] = {}
        for target in targets:
            print(f"\n=== OLD {target} ===")
            old_result = resolvers[target](old)
            old_results[target] = old_result
            print_resolution(old, old_result)
            actual = old_result.selected.target if old_result.selected else None
            expected = EXPECTED_OLD[target]
            status = "PASS" if actual == expected else "FAIL"
            actual_text = f"0x{actual:x}" if actual is not None else "unresolved"
            print(
                f"  self_test: {status} expected=0x{expected:x} actual={actual_text}"
            )

            print(f"\n=== NEW {target} ===")
            new_result = resolvers[target](new)
            new_results[target] = new_result
            print_resolution(new, new_result)

            count = ROLE_WINDOW_INSTRUCTIONS[target]
            matched, mismatches = masked_compare(
                old, new, old_result.anchor, new_result.anchor, count
            )
            if old_result.selected is not None and new_result.selected is not None:
                pair_matched, pair_mismatches = masked_compare(
                    old,
                    new,
                    Symbol(old_result.selected.adrp.pc, "t", "role_pair"),
                    Symbol(new_result.selected.adrp.pc, "t", "role_pair"),
                    2,
                )
                print(
                    f"  masked_old_new_role_pair: {pair_matched}/2 instructions match"
                )
                if pair_mismatches:
                    print(
                        "  masked_role_pair_mismatch_offsets: "
                        + ", ".join(f"+0x{index * 4:x}" for index in pair_mismatches)
                    )
            print(
                f"  masked_old_new_anchor_window: {matched}/{count} instructions match"
            )
            if mismatches:
                print(
                    "  masked_mismatch_offsets: "
                    + ", ".join(f"+0x{index * 4:x}" for index in mismatches)
                )

        self_test_pass = all(
            old_results[target].selected is not None
            and old_results[target].selected.target == EXPECTED_OLD[target]
            for target in targets
        )
        all_new_resolved = all(new_results[target].selected is not None for target in targets)
        print("\n=== SUMMARY ===")
        for target in targets:
            selected = new_results[target].selected
            value = f"0x{selected.target:x}" if selected else "unresolved"
            print(f"{target}={value}")
        for target in targets:
            selected = old_results[target].selected
            actual = f"0x{selected.target:x}" if selected else "unresolved"
            expected = EXPECTED_OLD[target]
            status = "PASS" if selected and selected.target == expected else "FAIL"
            print(
                f"self_test {target}: {status} expected=0x{expected:x} actual={actual}"
            )
        print(f"SELF_TEST={'PASS' if self_test_pass else 'FAIL'}")
        print(f"NEW_RESOLUTION={'PASS' if all_new_resolved else 'UNRESOLVED'}")
        return 0 if self_test_pass and all_new_resolved else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)

#!/usr/bin/env python3
"""sig_scan_functors.py — locate old-build functions in a new BG3 binary.

Takes the byte dump produced by ghidra/scripts/dump_functor_bytes.py
(4.1.1.6995620 vintage) and scans the ARM64 slice of the new binary
(4.1.1.7209685) with PC-relative/address-bearing instruction fields masked,
so that code that merely slid keeps matching while genuinely changed code
does not.

Masked (immediate bits zeroed in both pattern and candidate):
  ADRP/ADR          — page addresses always differ across builds
  B/BL              — branch targets slide
  B.cond/CBZ/CBNZ/TBZ/TBNZ — ditto
  LDR (literal)     — PC-relative pools slide
  ADD/SUB imm12 following an ADRP to the same register — page-offset fixups

Usage:
  python3 sig_scan_functors.py <dump.json> <new_binary> [-o out.json]
          [--min-instrs 24] [--max-instrs 128]

Output JSON: name -> {old_addr, candidates: [va...], instrs_used, status}
status: unique | ambiguous | not_found | no_bytes
"""
import argparse
import json
import struct
import sys

MH_MAGIC_64 = 0xFEEDFACF
FAT_MAGIC = 0xCAFEBABE
CPU_TYPE_ARM64 = 0x0100000C


def find_arm64_slice(data):
    """Return (offset, size) of the ARM64 slice in a fat or thin Mach-O."""
    magic = struct.unpack(">I", data[:4])[0]
    if magic == FAT_MAGIC:
        nfat = struct.unpack(">I", data[4:8])[0]
        for i in range(nfat):
            off = 8 + i * 20
            cputype, _sub, f_off, f_size, _al = struct.unpack(
                ">iiIII", data[off:off + 20])
            if cputype & 0xFFFFFFFF == CPU_TYPE_ARM64:
                return f_off, f_size
        raise SystemExit("no ARM64 slice in fat binary")
    if struct.unpack("<I", data[:4])[0] == MH_MAGIC_64:
        return 0, len(data)
    raise SystemExit("unrecognized binary format")


def text_segment(data, slice_off):
    """Parse load commands; return (file_off, vmaddr, size) of __TEXT __text."""
    ncmds = struct.unpack("<I", data[slice_off + 16:slice_off + 20])[0]
    cur = slice_off + 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack("<II", data[cur:cur + 8])
        if cmd == 0x19:  # LC_SEGMENT_64
            segname = data[cur + 8:cur + 24].rstrip(b"\0").decode()
            if segname == "__TEXT":
                nsects = struct.unpack("<I", data[cur + 64:cur + 68])[0]
                sc = cur + 72
                for _s in range(nsects):
                    sectname = data[sc:sc + 16].rstrip(b"\0").decode()
                    if sectname == "__text":
                        vmaddr, size = struct.unpack("<QQ", data[sc + 32:sc + 48])
                        f_off = struct.unpack("<I", data[sc + 48:sc + 52])[0]
                        return slice_off + f_off, vmaddr, size
                    sc += 80
        cur += cmdsize
    raise SystemExit("__TEXT,__text not found")


def mask_for(instr, prev_adrp_reg):
    """Return (mask, new_prev_adrp_reg). mask has 0 bits where field is volatile."""
    full = 0xFFFFFFFF
    rd = instr & 0x1F
    # ADRP / ADR: op 0b?0010000 in bits 24-28,31 -> (instr >> 24) & 0x9F in {0x90 (ADRP), 0x10 (ADR)}
    top = (instr >> 24) & 0x9F
    if top in (0x90, 0x10):
        # mask immlo (30:29) + immhi (23:5); keep op bits + Rd
        return full & ~((0x3 << 29) | (0x7FFFF << 5)), rd
    op26 = (instr >> 26) & 0x3F
    if op26 in (0x05, 0x25):  # B (0b000101) / BL (0b100101): mask imm26
        return full & ~0x03FFFFFF, None
    op24 = (instr >> 24) & 0xFF
    if op24 == 0x54:  # B.cond: mask imm19
        return full & ~(0x7FFFF << 5), None
    if op24 in (0x34, 0x35, 0xB4, 0xB5):  # CBZ/CBNZ 32/64: mask imm19
        return full & ~(0x7FFFF << 5), None
    if op24 in (0x36, 0x37, 0xB6, 0xB7):  # TBZ/TBNZ: mask imm14
        return full & ~(0x3FFF << 5), None
    # LDR literal (C4.4.5): opc0x011000 -> (instr >> 24) & 0xBF == 0x18
    if (instr >> 24) & 0xBF == 0x18:
        return full & ~(0x7FFFF << 5), None
    # ADD/SUB imm12 (64-bit) with Rn == last ADRP Rd: page-offset fixup
    if prev_adrp_reg is not None:
        rn = (instr >> 5) & 0x1F
        if (instr >> 23) & 0xFF in (0x122, 0x122 & 0xFF,):
            pass  # unreachable; kept simple below
        # ADD (imm): sf=1 op=0 S=0 -> bits 31-23 = 0b100100010 (0x122)
        if ((instr >> 23) & 0x1FF) in (0x122, 0x1A2) and rn == prev_adrp_reg:
            return full & ~(0xFFF << 10), None
        # LDR/STR unsigned imm with base == ADRP reg (GOT loads)
        if ((instr >> 24) & 0x3F) == 0x39 and rn == prev_adrp_reg:
            return full & ~(0xFFF << 10), None
    return full, None


def build_masked(pattern_bytes, n_instrs):
    words, masks = [], []
    prev_adrp = None
    for i in range(n_instrs):
        instr = struct.unpack("<I", pattern_bytes[i * 4:i * 4 + 4])[0]
        m, prev_adrp = mask_for(instr, prev_adrp)
        words.append(instr & m)
        masks.append(m)
    return words, masks


def scan(text, words, masks):
    """Return byte offsets in text where the masked pattern matches (4-aligned)."""
    hits = []
    n = len(words)
    first_w, first_m = words[0], masks[0]
    limit = len(text) - n * 4
    unpack = struct.unpack_from
    for off in range(0, limit + 1, 4):
        w0 = unpack("<I", text, off)[0]
        if w0 & first_m != first_w:
            continue
        ok = True
        for i in range(1, n):
            wi = unpack("<I", text, off + i * 4)[0]
            if wi & masks[i] != words[i]:
                ok = False
                break
        if ok:
            hits.append(off)
            if len(hits) > 8:
                break
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("binary")
    ap.add_argument("-o", "--out", default=None)
    ap.add_argument("--min-instrs", type=int, default=24)
    ap.add_argument("--max-instrs", type=int, default=128)
    args = ap.parse_args()

    with open(args.dump) as f:
        dump = json.load(f)
    with open(args.binary, "rb") as f:
        data = f.read()

    s_off, _s_size = find_arm64_slice(data)
    t_off, t_vmaddr, t_size = text_segment(data, s_off)
    text = data[t_off:t_off + t_size]
    sys.stderr.write("ARM64 slice @0x%x, __text vm 0x%x size 0x%x\n"
                     % (s_off, t_vmaddr, t_size))

    results = {}
    for name, rec in sorted(dump["targets"].items()):
        if "bytes" not in rec:
            results[name] = {"old_addr": rec.get("addr"), "status": "no_bytes",
                             "error": rec.get("error")}
            continue
        pat = bytes.fromhex(rec["bytes"])
        n = args.max_instrs
        hits, used = [], None
        while n >= args.min_instrs:
            words, masks = build_masked(pat, min(n, len(pat) // 4))
            hits = scan(text, words, masks)
            if len(hits) == 1:
                used = n
                break
            if len(hits) > 1:
                # more context should disambiguate, but we're already at max
                used = n
                break
            n //= 2  # not found: maybe fn shorter than window; shrink
        vas = ["0x%x" % (t_vmaddr + h) for h in hits]
        status = ("unique" if len(hits) == 1 else
                  "ambiguous" if hits else "not_found")
        results[name] = {"old_addr": rec["addr"], "candidates": vas,
                         "instrs_used": used, "status": status}
        sys.stderr.write("%-34s %-9s %s\n" % (name, status, " ".join(vas)))

    out = {"binary": args.binary, "text_vmaddr": "0x%x" % t_vmaddr,
           "results": results}
    if args.out:
        with open(args.out, "w") as f:
            json.dump(out, f, indent=1, sort_keys=True)
        sys.stderr.write("WROTE %s\n" % args.out)
    else:
        json.dump(out, sys.stdout, indent=1, sort_keys=True)


if __name__ == "__main__":
    main()

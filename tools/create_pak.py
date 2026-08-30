#!/usr/bin/env python3
"""Create an LSPK v18 pak from a directory tree (files stored uncompressed).

Usage: create_pak.py <source_dir> <output.pak>

Counterpart to extract_pak.py / repack_pak.py. Entries are stored with
compression flag 0; BG3 accepts stored entries, and mod-sized content doesn't
need the LZ4 pass. The file list itself is LZ4-block compressed as the format
requires. Part of the BG3SE-macOS project.
"""
import os, struct, sys
import lz4.block

def create(src_dir, out_path):
    files = []
    for root, _, names in os.walk(src_dir):
        for n in sorted(names):
            if n == '.DS_Store': continue
            full = os.path.join(root, n)
            rel = os.path.relpath(full, src_dir).replace(os.sep, '/')
            files.append((rel, full))
    files.sort()
    if not files:
        raise SystemExit("no files under " + src_dir)

    entries = b''
    with open(out_path, 'wb') as out:
        out.write(b'\x00' * 40)
        for rel, full in files:
            data = open(full, 'rb').read()
            offset = out.tell()
            out.write(data)
            name = rel.encode('utf-8')
            if len(name) > 255: raise SystemExit("path too long: " + rel)
            e = bytearray(272)
            e[0:len(name)] = name
            struct.pack_into('<I', e, 256, offset & 0xFFFFFFFF)
            struct.pack_into('<H', e, 260, (offset >> 32) & 0xFFFF)
            e[262] = 0          # archive part
            e[263] = 0          # flags: stored
            struct.pack_into('<I', e, 264, len(data))   # size on disk
            struct.pack_into('<I', e, 268, len(data))   # uncompressed
            entries += bytes(e)
        flo = out.tell()
        comp = lz4.block.compress(entries, store_size=False)
        out.write(struct.pack('<I', len(files)))
        out.write(struct.pack('<I', len(comp)))
        out.write(comp)
        hdr = bytearray(40)
        hdr[0:4] = b'LSPK'
        struct.pack_into('<I', hdr, 4, 18)
        struct.pack_into('<Q', hdr, 8, flo)
        struct.pack_into('<I', hdr, 16, 8 + len(comp))
        struct.pack_into('<H', hdr, 38, 1)
        out.seek(0); out.write(hdr)
    return len(files)

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(__doc__); sys.exit(1)
    n = create(sys.argv[1], sys.argv[2])
    print(f"packed {n} files -> {sys.argv[2]} ({os.path.getsize(sys.argv[2])} bytes)")

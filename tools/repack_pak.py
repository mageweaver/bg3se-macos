#!/usr/bin/env python3
"""Rebuild an LSPK v18 pak, dropping a set of files. Copies kept files' bytes
verbatim (compression preserved); only offsets are re-patched."""
import struct, sys, os
import lz4.block

def repack(src, dst, drop_predicate):
    with open(src,'rb') as f:
        hdr = f.read(40)
        assert hdr[0:4]==b'LSPK' and struct.unpack('<I',hdr[4:8])[0]==18
        flo = struct.unpack('<Q',hdr[8:16])[0]
        f.seek(flo)
        num_files = struct.unpack('<I', f.read(4))[0]
        comp_size = struct.unpack('<I', f.read(4))[0]
        comp = f.read(comp_size)
        raw = lz4.block.decompress(comp, uncompressed_size=num_files*272)
        entries = [bytearray(raw[i*272:(i+1)*272]) for i in range(num_files)]

        kept=[]; dropped=[]
        for e in entries:
            name = e[0:256].split(b'\x00')[0].decode('utf-8','replace')
            oldoff = struct.unpack('<I',e[256:260])[0] | (struct.unpack('<H',e[260:262])[0]<<32)
            disk_size = struct.unpack('<I',e[264:268])[0]
            if drop_predicate(name):
                dropped.append(name); continue
            kept.append((e, oldoff, disk_size, name))

        # Write new pak
        with open(dst,'wb') as out:
            out.write(b'\x00'*40)  # header placeholder
            for e, oldoff, disk_size, name in kept:
                f.seek(oldoff); blob = f.read(disk_size)
                assert len(blob)==disk_size, f"short read {name}"
                newoff = out.tell()
                out.write(blob)
                # patch offset field (48-bit): lo u32 @256, hi u16 @260
                e[256:260] = struct.pack('<I', newoff & 0xFFFFFFFF)
                e[260:262] = struct.pack('<H', (newoff>>32) & 0xFFFF)
            new_flo = out.tell()
            new_list = b''.join(bytes(e) for e,_,_,_ in kept)
            new_comp = lz4.block.compress(new_list, store_size=False)
            out.write(struct.pack('<I', len(kept)))
            out.write(struct.pack('<I', len(new_comp)))
            out.write(new_comp)
            file_list_size = 8 + len(new_comp)
            # write header
            out.seek(0)
            h = bytearray(40)
            h[0:4]=b'LSPK'
            struct.pack_into('<I',h,4,18)
            struct.pack_into('<Q',h,8,new_flo)
            struct.pack_into('<I',h,16,file_list_size)
            h[20]=0; h[21]=0  # flags, priority
            # md5 stays zero (22:38)
            struct.pack_into('<H',h,38,1)  # num_parts
            out.write(h)
    return len(kept), dropped

if __name__=='__main__':
    src, dst = sys.argv[1], sys.argv[2]
    def drop(name):
        b=name.split('/')[-1]
        if b in ('CHAR_Hair.lsf','CHAR_Skin_Head_v3.lsf') and '/Materials/Characters/' in name: return True
        if b.endswith('.bshd') and '/Materials/Characters/' in name: return True
        return False
    kept, dropped = repack(src, dst, drop)
    print(f"kept {kept} files, dropped {len(dropped)}")
    for d in dropped[:5]: print("  dropped:", d.split('/')[-1])
    print("  ...")

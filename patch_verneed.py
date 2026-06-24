#!/usr/bin/env python3
"""
patch_verneed.py — strip Android Bionic version requirements from .so files.

Two-step patch so the ELF loads on Linux with glibc:

  1. In .gnu.version_r (VERNEED): collect the version index (vna_other) for
     every version named LIBC / LIBM / LIBDL.

  2. In .gnu.version (VERSYM): every per-symbol entry whose index matches one
     of those Bionic version indices is reset to VER_NDX_GLOBAL (1), which
     means "unversioned global". glibc always exports symbols both versioned
     (GLIBC_2.x) and as an unversioned default, so they resolve cleanly.

Usage:
    python3 patch_verneed.py lib1.so [lib2.so ...]   # patches in-place
"""

import struct
import sys
import os

BIONIC_VERSIONS  = {'LIBC', 'LIBM', 'LIBDL'}
VER_NDX_GLOBAL   = 1          # resolves to unversioned symbol
SHT_GNU_verneed  = 0x6ffffffe
SHT_GNU_versym   = 0x6fffffff


def get_cstr(data: bytes, offset: int) -> str:
    end = data.index(b'\x00', offset)
    return data[offset:end].decode('latin-1')


def patch_file(path: str) -> int:
    with open(path, 'rb') as f:
        data = bytearray(f.read())

    assert data[:4] == b'\x7fELF', f"{path}: not an ELF"
    assert data[4] == 2, f"{path}: not 64-bit"
    assert data[5] == 1, f"{path}: not little-endian"

    e_shoff     = struct.unpack_from('<Q', data, 0x28)[0]
    e_shentsize = struct.unpack_from('<H', data, 0x3a)[0]
    e_shnum     = struct.unpack_from('<H', data, 0x3c)[0]
    e_shstrndx  = struct.unpack_from('<H', data, 0x3e)[0]

    def sh(i):
        o = e_shoff + i * e_shentsize
        return (struct.unpack_from('<I', data, o)[0],      # sh_name
                struct.unpack_from('<I', data, o + 4)[0],  # sh_type
                struct.unpack_from('<Q', data, o + 0x18)[0],  # sh_offset
                struct.unpack_from('<Q', data, o + 0x20)[0])  # sh_size

    shstr_off = sh(e_shstrndx)[2]

    verneed_off = verneed_sz = None
    versym_off  = versym_sz  = None
    dynstr_off  = None

    for i in range(e_shnum):
        sh_name, sh_type, sh_offset, sh_size = sh(i)
        name = get_cstr(data, shstr_off + sh_name)
        if sh_type == SHT_GNU_verneed:
            verneed_off, verneed_sz = sh_offset, sh_size
        elif sh_type == SHT_GNU_versym:
            versym_off, versym_sz = sh_offset, sh_size
        elif name == '.dynstr':
            dynstr_off = sh_offset

    if verneed_off is None or dynstr_off is None:
        print(f"  {os.path.basename(path)}: no VERNEED/.dynstr — skip")
        return 0

    # ── Step 1: collect Bionic version indices from VERNEED & set WEAK ───
    bionic_indices = set()
    VER_FLG_WEAK = 0x2
    off = verneed_off
    end = verneed_off + verneed_sz
    while off < end:
        vn_cnt  = struct.unpack_from('<H', data, off + 2)[0]
        vn_aux  = struct.unpack_from('<I', data, off + 8)[0]
        vn_next = struct.unpack_from('<I', data, off + 12)[0]
        aux_off = off + vn_aux
        for _ in range(vn_cnt):
            vna_flags = struct.unpack_from('<H', data, aux_off + 4)[0]
            vna_other = struct.unpack_from('<H', data, aux_off + 6)[0]
            vna_name  = struct.unpack_from('<I', data, aux_off + 8)[0]
            vna_next  = struct.unpack_from('<I', data, aux_off + 12)[0]
            ver_name = get_cstr(data, dynstr_off + vna_name)
            if ver_name in BIONIC_VERSIONS:
                bionic_indices.add(vna_other)
                # Set VER_FLG_WEAK so the library-level version check is non-fatal
                new_flags = vna_flags | VER_FLG_WEAK
                struct.pack_into('<H', data, aux_off + 4, new_flags)
                print(f"  {os.path.basename(path)}: version {ver_name!r} "
                      f"index={vna_other} flags {vna_flags:#04x}→{new_flags:#04x} (WEAK)")
            if vna_next == 0:
                break
            aux_off += vna_next
        if vn_next == 0:
            break
        off += vn_next

    if not bionic_indices:
        print(f"  {os.path.basename(path)}: no Bionic versions found")
        return 0

    # ── Step 2: zero out per-symbol version indices in .gnu.version ──────
    patched = 0
    if versym_off is None:
        print(f"  {os.path.basename(path)}: no .gnu.version section — "
              "VERNEED-only patch applied")
    else:
        n_entries = versym_sz // 2   # each entry is uint16
        for i in range(n_entries):
            idx = struct.unpack_from('<H', data, versym_off + i * 2)[0]
            if idx in bionic_indices:
                struct.pack_into('<H', data, versym_off + i * 2, VER_NDX_GLOBAL)
                patched += 1

        print(f"  {os.path.basename(path)}: reset {patched} symbol version "
              f"entries to VER_NDX_GLOBAL")

    with open(path, 'wb') as f:
        f.write(data)
    return patched


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    total = 0
    for path in sys.argv[1:]:
        print(f"Patching {path} ...")
        total += patch_file(path)

    print(f"\nDone — {total} symbol entries patched.")


if __name__ == '__main__':
    main()

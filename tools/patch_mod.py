#!/usr/bin/env python3
"""Patch the NIER_DLAA 1.1 d3d11.dll so its NGX loader loads NIER_FSR4.dll.

The mod statically links NVIDIA's nvsdk_ngx_s.lib (NGX core rel 310.9).
NGXLoadCoreLibrary (RVA 0x229a0) finds _nvngx.dll in the NVIDIA driver store,
checks for an NVIDIA adapter and verifies NVIDIA's signature. Its only caller
(0x18002018b) stores the result in the core-module global and then resolves
the NVSDK_NGX_D3D11_* entry points from it with GetProcAddress.

The patch replaces the body of NGXLoadCoreLibrary with

    lea  rcx, [rip + name]          ; L"NIER_FSR4.dll"
    jmp  qword ptr [rip + IAT]      ; LoadLibraryW (IAT slot RVA 0x4f020)
  name: dw 'NIER_FSR4.dll', 0

so the NGX entry points resolve to NIER_FSR4.dll's exports. Nothing else in
the mod changes.

Usage: patch_mod.py <original d3d11.dll> <output d3d11.dll>
Only uses the Python standard library.
"""
import hashlib
import struct
import sys

ORIGINAL_SHA256 = "f1c77c58fdd265b25dfa262c8660d49a4463a05bb0ee88daf732727f15e717ce"
RVA_LOAD_CORE = 0x229A0
RVA_IAT_LOADLIBRARYW = 0x4F020
# mov rax, rsp / push rbp / push r14 / push r15 / lea rbp, [rax - 0x198]
EXPECTED_PROLOGUE = bytes.fromhex("488bc45541564157488da868feffff")
DLL_NAME = "NIER_FSR4.dll"


def rva_to_offset(data, rva):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    sec = pe + 24 + opt_size
    for i in range(nsec):
        s = sec + i * 40
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, s + 8)
        if va <= rva < va + max(vsize, rsize):
            return rva - va + raw
    raise ValueError(f"RVA {rva:#x} is not in any section")


def build_stub():
    name = DLL_NAME.encode("utf-16le") + b"\0\0"
    lea = b"\x48\x8d\x0d" + struct.pack("<i", 6)  # string starts after the 6-byte jmp
    jmp_end = RVA_LOAD_CORE + 7 + 6
    jmp = b"\xff\x25" + struct.pack("<i", RVA_IAT_LOADLIBRARYW - jmp_end)
    return lea + jmp + name


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    data = bytearray(open(sys.argv[1], "rb").read())
    digest = hashlib.sha256(data).hexdigest()
    if digest != ORIGINAL_SHA256:
        print(f"error: {sys.argv[1]} is not the NIER_DLAA 1.1 d3d11.dll (sha256 {digest})")
        print("       the patch offsets only fit that exact build")
        return 1

    off = rva_to_offset(data, RVA_LOAD_CORE)
    if data[off:off + len(EXPECTED_PROLOGUE)] != EXPECTED_PROLOGUE:
        print("error: NGXLoadCoreLibrary prologue does not match")
        return 1
    # The IAT slot must be KERNEL32!LoadLibraryW: check its import name.
    iat_off = rva_to_offset(data, RVA_IAT_LOADLIBRARYW)
    hint_rva = struct.unpack_from("<Q", data, iat_off)[0]
    name_off = rva_to_offset(data, hint_rva) + 2
    if data[name_off:name_off + 13] != b"LoadLibraryW\0":
        print("error: IAT slot is not LoadLibraryW")
        return 1

    stub = build_stub()
    data[off:off + len(stub)] = stub
    open(sys.argv[2], "wb").write(data)
    print(f"patched NGXLoadCoreLibrary at RVA {RVA_LOAD_CORE:#x} ({len(stub)} bytes): loads {DLL_NAME}")
    print(f"output sha256 {hashlib.sha256(data).hexdigest()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

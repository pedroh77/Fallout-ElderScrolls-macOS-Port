#!/usr/bin/env python3
"""Build and run PE DLL loader tests (synthetic CI mode + optional --nvse local mode)."""
import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "upstream/recomp/host"
RECOMP = ROOT / "upstream/recomp"
MACOS = ROOT / "macos"

VCLIBS_URL = "https://download.microsoft.com/download/4/7/c/47c6134b-d61f-4024-83bd-b9c9ea951c25/Microsoft.VCLibs.x86.14.00.Desktop.appx"
VCLIBS_SHA256 = "a7fb9d76e07b36d868179eb53ffd13740c25242176fa363f154798cf34edd4a9"
UCRT_CAB_URL = "https://download.visualstudio.microsoft.com/download/pr/616f25d4-4e58-48a3-a8f1-f31e7c172e75/d1927d5466caa917782df8054568731e/fef2cfedd6135e0ed85290b83f3682c3.cab"
UCRT_CAB_SHA256 = "4e9a69b016d75b2520b975eee411c7049e0dcc96acd683e9a570834baf4a2192"


def find_unicorn():
    for cand in ["/opt/homebrew/opt/unicorn", "/opt/homebrew", "/usr/local/opt/unicorn", "/usr/local"]:
        p = Path(cand)
        if (p / "include/unicorn/unicorn.h").is_file() and (p / "lib/libunicorn.a").is_file():
            return p
    raise SystemExit("Unicorn not found")


def build_pe_dll(sections, base=0x10000000, ep_rva=0x1000, exp_dir=(0, 0), imp_dir=(0, 0), reloc_dir=(0, 0), tls_dir=(0, 0)):
    """Assemble a minimal valid 32-bit x86 PE DLL with arbitrary sections & data directories."""
    hdr_sz = 0x400
    max_va = max(va + ((len(data) + 0xFFF) & ~0xFFF or 0x1000) for _, va, data, _ in sections)
    raw = bytearray(hdr_sz + sum((len(data) + 0x1FF) & ~0x1FF for _, _, data, _ in sections))
    raw[0:2] = b"MZ"
    struct.pack_into("<I", raw, 0x3C, 0x80)
    pe = 0x80
    raw[pe:pe + 4] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", raw, pe + 4, 0x014C, len(sections), 0, 0, 0, 224, 0x2102)
    opt = pe + 24
    struct.pack_into("<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII",
                     raw, opt,
                     0x010B, 14, 0, 0x1000, 0x1000, 0, ep_rva, 0x1000, 0x2000,
                     base, 0x1000, 0x200, 6, 0, 0, 0, 6, 0, 0,
                     max_va, hdr_sz, 0, 3, 0, 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
    dd = opt + 96
    for idx, (rva, sz) in [(0, exp_dir), (1, imp_dir), (5, reloc_dir), (9, tls_dir)]:
        struct.pack_into("<II", raw, dd + 8 * idx, rva, sz)

    rp = hdr_sz
    sec_off = opt + 224
    for name, va, data, chars in sections:
        rs = (len(data) + 0x1FF) & ~0x1FF
        vs = max(len(data), 1)
        raw[sec_off:sec_off + 8] = name.encode("latin1").ljust(8, b"\0")[:8]
        struct.pack_into("<IIIIIIHHI", raw, sec_off + 8, vs, va, rs, rp, 0, 0, 0, 0, chars)
        raw[rp:rp + len(data)] = data
        rp += rs
        sec_off += 40
    return bytes(raw)


def make_synthetic_dlls(out_dir: Path):
    # 1. dep.dll: exports dep_add(a, b) = a + b at 0x1010, DllMain at 0x1000
    dep_text = bytearray(0x100)
    dep_text[0:8] = b"\xB8\x01\x00\x00\x00\xC2\x0C\x00"  # mov eax,1; ret 12
    dep_text[0x10:0x19] = b"\x8B\x44\x24\x04\x03\x44\x24\x08\xC3"  # mov eax,[esp+4]; add eax,[esp+8]; ret

    dep_rdata = bytearray(0x200)
    # Export dir at 0x2000: 1 fn, 1 name
    struct.pack_into("<IIIIIIIIIII", dep_rdata, 0, 0, 0, 0, 0x2050, 1, 1, 1, 0x2028, 0x202C, 0x2030, 0)
    struct.pack_into("<I", dep_rdata, 0x28, 0x1010)  # AddressOfFunctions
    struct.pack_into("<I", dep_rdata, 0x2C, 0x2040)  # AddressOfNames
    struct.pack_into("<H", dep_rdata, 0x30, 0)       # AddressOfNameOrdinals
    dep_rdata[0x40:0x48] = b"dep_add\0"
    dep_rdata[0x50:0x58] = b"dep.dll\0"
    dep_bytes = build_pe_dll(
        [(".text", 0x1000, dep_text, 0x60000020), (".rdata", 0x2000, dep_rdata, 0x40000040)],
        base=0x10000000, ep_rva=0x1000, exp_dir=(0x2000, 0x60),
    )
    (out_dir / "dep.dll").write_bytes(dep_bytes)

    # 2. main.dll: same preferred base 0x10000000 -> forces relocation!
    # .text (0x1000), .rdata (0x2000), .data (0x3000), .tls (0x4000), .reloc (0x5000)
    base = 0x10000000
    # .data VAs: tls_index=0x10003000, old_prot=0x10003004, tls_out=0x10003008
    # .rdata VAs: IAT at 0x2100 (VirtualProtect slot = 0x10002100)
    main_text = bytearray(0x100)
    # DllMain @ 0x1000:
    #   64 A1 2C 00 00 00       mov eax, fs:[0x2C]
    #   8B 0D [0x10003000]      mov ecx, [tls_index]       (reloc @ +8)
    #   8B 04 88                mov eax, [eax + ecx*4]
    #   8B 00                   mov eax, [eax]
    #   A3 [0x10003008]         mov [tls_out], eax         (reloc @ +18)
    #   68 [0x10003004]         push &old_prot             (reloc @ +23)
    #   6A 40                   push 0x40
    #   6A 10                   push 16
    #   68 [0x10001000]         push 0x10001000            (reloc @ +32)
    #   FF 15 [0x10002100]      call [VirtualProtect]      (reloc @ +38)
    #   B8 01 00 00 00          mov eax, 1
    #   C2 0C 00                ret 12
    code = bytearray()
    code += b"\x64\xA1\x2C\x00\x00\x00"
    reloc_rvas = []
    code += b"\x8B\x0D"; reloc_rvas.append(0x1000 + len(code)); code += struct.pack("<I", base + 0x3000)
    code += b"\x8B\x04\x88\x8B\x00"
    code += b"\xA3"; reloc_rvas.append(0x1000 + len(code)); code += struct.pack("<I", base + 0x3008)
    code += b"\x68"; reloc_rvas.append(0x1000 + len(code)); code += struct.pack("<I", base + 0x3004)
    code += b"\x6A\x40\x6A\x10"
    code += b"\x68"; reloc_rvas.append(0x1000 + len(code)); code += struct.pack("<I", base + 0x1000)
    code += b"\xFF\x15"; reloc_rvas.append(0x1000 + len(code)); code += struct.pack("<I", base + 0x2100)
    code += b"\xB8\x01\x00\x00\x00\xC2\x0C\x00"
    main_text[0:len(code)] = code

    # get_tls_val @ 0x1040: mov eax, [tls_out]; ret
    gt = bytearray(b"\xA1")
    reloc_rvas.append(0x1040 + len(gt))
    gt += struct.pack("<I", base + 0x3008) + b"\xC3"
    main_text[0x40:0x40 + len(gt)] = gt

    # .rdata @ 0x2000:
    # Exports @ 0x2000..0x20A0 (forward_add, get_tls_val)
    main_rdata = bytearray(0x300)
    struct.pack_into("<IIIIIIIIII", main_rdata, 0, 0, 0, 0, 0x2080, 1, 2, 2, 0x2028, 0x2030, 0x2038)
    struct.pack_into("<II", main_rdata, 0x28, 0x2060, 0x1040)   # forward_add -> RVA inside exp_dir; get_tls_val -> 0x1040
    struct.pack_into("<II", main_rdata, 0x30, 0x2040, 0x2050)   # names sorted
    struct.pack_into("<HH", main_rdata, 0x38, 0, 1)
    main_rdata[0x40:0x4C] = b"forward_add\0"
    main_rdata[0x50:0x5C] = b"get_tls_val\0"
    main_rdata[0x60:0x6C] = b"dep.dep_add\0"
    main_rdata[0x80:0x89] = b"main.dll\0"

    # Imports @ 0x20B0: KERNEL32.dll!VirtualProtect (IAT @ 0x2100)
    struct.pack_into("<IIIII", main_rdata, 0xB0, 0x2110, 0, 0, 0x2130, 0x2100)
    struct.pack_into("<II", main_rdata, 0x110, 0x2140, 0)  # ILT
    struct.pack_into("<II", main_rdata, 0x100, 0x2140, 0)  # IAT
    main_rdata[0x130:0x13D] = b"KERNEL32.dll\0"
    struct.pack_into("<H", main_rdata, 0x140, 0)
    main_rdata[0x142:0x151] = b"VirtualProtect\0"

    main_data = bytearray(0x100)
    main_tls = bytearray(0x100)
    struct.pack_into("<I", main_tls, 0, 0xCAFEBABE)  # template @ 0x4000
    # IMAGE_TLS_DIRECTORY32 @ 0x4010
    struct.pack_into("<IIIII", main_tls, 0x10, base + 0x4000, base + 0x4004, base + 0x3000, 0, 4)
    reloc_rvas.extend([0x4010, 0x4014, 0x4018])

    # Build .reloc grouped by 4KB page
    by_page = {}
    for rva in reloc_rvas:
        by_page.setdefault(rva & ~0xFFF, []).append(rva & 0xFFF)
    main_reloc = bytearray()
    for page, offs in sorted(by_page.items()):
        entries = [(3 << 12) | o for o in offs]
        if len(entries) % 2:
            entries.append(0)
        blk_sz = 8 + 2 * len(entries)
        main_reloc += struct.pack("<II", page, blk_sz)
        for e in entries:
            main_reloc += struct.pack("<H", e)

    main_bytes = build_pe_dll(
        [
            (".text", 0x1000, main_text, 0x60000020),
            (".rdata", 0x2000, main_rdata, 0x40000040),
            (".data", 0x3000, main_data, 0xC0000040),
            (".tls", 0x4000, main_tls, 0xC0000040),
            (".reloc", 0x5000, main_reloc, 0x42000040),
        ],
        base=base, ep_rva=0x1000,
        exp_dir=(0x2000, 0xA0), imp_dir=(0x20B0, 40),
        reloc_dir=(0x5000, len(main_reloc)), tls_dir=(0x4010, 24),
    )
    (out_dir / "main.dll").write_bytes(main_bytes)


def fetch_verified(url: str, dst: Path, sha256: str):
    if dst.is_file() and hashlib.sha256(dst.read_bytes()).hexdigest() == sha256:
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    tmp = dst.with_suffix(".tmp")
    print(f"Downloading {url} -> {dst.name}...")
    urllib.request.urlretrieve(url, tmp)
    got = hashlib.sha256(tmp.read_bytes()).hexdigest()
    if got != sha256:
        tmp.unlink(missing_ok=True)
        raise SystemExit(f"SHA-256 mismatch for {url}: got {got}, expected {sha256}")
    tmp.replace(dst)


def ensure_ms_crt(crt_dir: Path):
    if (crt_dir / "ucrtbase.dll").is_file() and (crt_dir / "msvcp140.dll").is_file() and (crt_dir / "vcruntime140.dll").is_file():
        return
    crt_dir.mkdir(parents=True, exist_ok=True)
    appx = crt_dir / "Microsoft.VCLibs.x86.14.00.Desktop.appx"
    cab = crt_dir / "ucrt_x86.cab"
    fetch_verified(VCLIBS_URL, appx, VCLIBS_SHA256)
    fetch_verified(UCRT_CAB_URL, cab, UCRT_CAB_SHA256)
    with tempfile.TemporaryDirectory() as td:
        subprocess.run(["/usr/bin/unzip", "-oq", str(appx), "*.dll", "-d", td], check=True)
        for p in Path(td).glob("*.dll"):
            shutil.copy2(p, crt_dir / p.name)
        cab_out = Path(td) / "cab"
        cab_out.mkdir()
        subprocess.run(["/usr/bin/tar", "-xf", str(cab), "-C", str(cab_out)], check=True)
        ucrt_src = cab_out / "fila4b850e6d3b1997cb1f47f07278644f0"
        if not ucrt_src.is_file():
            raise SystemExit("ucrtbase.dll payload not found in CAB")
        shutil.copy2(ucrt_src, crt_dir / "ucrtbase.dll")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nvse", type=Path, help="directory containing nvse_1_4.dll")
    ap.add_argument("--exe", type=Path, help="FalloutNV.exe path (for --nvse)")
    args = ap.parse_args()

    unicorn = find_unicorn()
    with tempfile.TemporaryDirectory() as td:
        td_path = Path(td)
        bin_path = td_path / "pe_loader_test"
        sources = [
            ROOT / "tests/pe_loader_test.c",
            HOST / "pe_loader.c",
            HOST / "fallback.c",
            HOST / "host.c",
            RECOMP / "rt_dispatch.c",
            HOST / "gheap.c",
            HOST / "shims_kernel32.c",
            HOST / "shims_sync.c",
            HOST / "shims_files.c",
            HOST / "shims_text.c",
            HOST / "shims_imagehlp.c",
        ]
        cmd = [
            "xcrun", "clang", "-O1", "-g",
            "-I", str(MACOS), "-I", str(HOST), "-I", str(RECOMP),
            "-I", str(unicorn / "include"),
            *map(str, sources),
            str(unicorn / "lib/libunicorn.a"),
            "-pthread", "-lm",
            "-o", str(bin_path),
        ]
        subprocess.run(cmd, check=True)

        dll_dir = td_path / "dlls"
        dll_dir.mkdir()
        make_synthetic_dlls(dll_dir)
        subprocess.run([str(bin_path), "synthetic", str(dll_dir)], cwd=dll_dir, check=True)

        if args.nvse:
            exe = args.exe or Path.home() / "Applications/Fallout New Vegas.app/Contents/Resources/GameData/FalloutNV.exe"
            if not exe.is_file():
                raise SystemExit(f"FalloutNV.exe not found at {exe}")
            crt_dir = Path.home() / "Library/Caches/fnv-macos/nvse-crt"
            ensure_ms_crt(crt_dir)
            nvse_dll = args.nvse / "nvse_1_4.dll"
            if not nvse_dll.is_file():
                raise SystemExit(f"nvse_1_4.dll not found in {args.nvse}")
            game_root = td_path / "game_root"
            (game_root / "Data/NVSE/Plugins").mkdir(parents=True)
            if (args.nvse / "Data/NVSE/nvse_config.ini").is_file():
                shutil.copy2(args.nvse / "Data/NVSE/nvse_config.ini", game_root / "Data/NVSE/nvse_config.ini")
            env = dict(os.environ, FNV_ROOT=str(game_root))
            subprocess.run([str(bin_path), "nvse", str(exe), str(crt_dir), str(nvse_dll)],
                           cwd=game_root, env=env, check=True)
            log_file = game_root / "nvse.log"
            if not log_file.is_file():
                raise SystemExit("nvse.log was not created!")
            log_txt = log_file.read_text(errors="replace")
            print("=== nvse.log ===")
            print(log_txt)
            assert "NVSE runtime: initialize" in log_txt, "missing initialize header in nvse.log"
            assert "init complete" in log_txt, "missing 'init complete' in nvse.log"
            print("PASS: real nvse_1_4.dll initialized cleanly!")


if __name__ == "__main__":
    main()

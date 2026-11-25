"""Verify MSVC 2019+ CRT non-returning INT 0x29 (__fastfail) and AVX-512 traps emit cleanly."""
from pathlib import Path
import json
import subprocess
import sys
import tempfile
from native_test_toolchain import COMPILER, PLATFORM_FLAGS

ROOT = Path(__file__).resolve().parents[1]
EMIT = ROOT / "tools/translate/emit.py"

fns = [
    {
        "entry": 0x00401000,
        "name": "__security_check_cookie",
        "insns": [
            {
                "a": 0x00401000,
                "len": 2,
                "m": "JNZ 0x00401004",
                "ops": [["CBRANCH", None, [["ram", 0x00401004, 4], ["register", 518, 1]]]],
            },
            {
                "a": 0x00401002,
                "len": 1,
                "m": "RET",
                "ops": [
                    ["LOAD", ["register", 644, 4], [["const", 417, 8], ["register", 16, 4]]],
                    ["INT_ADD", ["register", 16, 4], [["register", 16, 4], ["const", 4, 4]]],
                    ["RETURN", None, [["register", 644, 4]]],
                ],
            },
            {
                "a": 0x00401004,
                "len": 2,
                "m": "INT 0x29",
                "ops": [
                    ["CALLOTHER", ["unique", 0x100, 4], [["const", 0, 4], ["const", 0x29, 1]]],
                    ["CALLIND", None, [["unique", 0x100, 4]]],
                ],
            },
        ],
    },
    {
        "entry": 0x00402000,
        "name": "_ftoui3_wrapper",
        "insns": [
            {
                "a": 0x00402000,
                "len": 2,
                "m": "JMP 0x00402010",
                "ops": [["BRANCH", None, [["ram", 0x00402010, 4]]]],
            },
            {
                "a": 0x00402010,
                "len": 6,
                "m": "VCVTTSS2USI EAX,XMM0",
                "ops": [["CALLOTHER", ["register", 0, 4], [["const", 1, 4], ["register", 4608, 16]]]],
            },
            {
                "a": 0x00402016,
                "len": 1,
                "m": "RET",
                "ops": [
                    ["LOAD", ["register", 644, 4], [["const", 417, 8], ["register", 16, 4]]],
                    ["INT_ADD", ["register", 16, 4], [["register", 16, 4], ["const", 4, 4]]],
                    ["RETURN", None, [["register", 644, 4]]],
                ],
            },
        ],
    },
]

with tempfile.TemporaryDirectory(prefix="fnv-emit-traps-") as tmp:
    tmp_path = Path(tmp)
    jsonl = tmp_path / "test.jsonl"
    jsonl.write_text("\n".join(json.dumps(f) for f in fns) + "\n")
    (tmp_path / "test.jsonl.userops.json").write_text(json.dumps(["swi", "vcvttss2usi_avx512f"]))
    extra = tmp_path / "extra.json"
    extra.write_text(json.dumps({str(0x00402000): [0x00402010]}))
    outdir = tmp_path / "out"

    subprocess.run([sys.executable, str(EMIT), str(jsonl), str(outdir), "--extra", str(extra)], check=True)

    report = json.loads((outdir / "emit_report.json").read_text())
    assert report["skipped"] == 0, f"expected 0 skipped, got {report}"
    assert report["emitted"] == 3, f"expected 3 emitted (2 primary + 1 extra), got {report}"

    stubs = (outdir / "stubs.c").read_text()
    assert "rt_fallback" not in stubs, f"unexpected fallback stub in stubs.c:\n{stubs}"

    tus = sorted(outdir.glob("tu_*.c"))
    assert tus, "expected generated translation unit(s)"
    tu_text = "\n".join(p.read_text() for p in tus)
    assert "rt_trap(c,TRAP_INT3,0x401004u);" in tu_text, "missing TRAP_INT3 for INT 0x29"
    assert "rt_trap(c,TRAP_UNSUPPORTED,0x402010u);" in tu_text, "missing TRAP_UNSUPPORTED for AVX-512"

    table_text = (outdir / "table.c").read_text()
    for addr in ("0x401000u", "0x402000u", "0x402010u"):
        assert addr in table_text, f"missing {addr} in table.c"

    for c_file in [*tus, outdir / "table.c", outdir / "stubs.c"]:
        subprocess.run([*COMPILER, *PLATFORM_FLAGS, "-O1", "-I", str(ROOT / "upstream/recomp"), "-fsyntax-only", str(c_file)], check=True)

print("PASS: INT 0x29 (__fastfail) and AVX-512 userops emit natively with zero fallbacks and valid C syntax", flush=True)

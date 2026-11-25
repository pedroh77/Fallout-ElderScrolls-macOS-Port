#!/usr/bin/env python3
"""End-to-end unit test for patch-aware deoptimization, range invalidation, and boot hooks."""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "upstream/recomp/host"
RECOMP = ROOT / "upstream/recomp"
MACOS = ROOT / "macos"


def find_unicorn():
    for cand in ["/opt/homebrew/opt/unicorn", "/opt/homebrew", "/usr/local/opt/unicorn", "/usr/local"]:
        p = Path(cand)
        if (p / "include/unicorn/unicorn.h").is_file() and (p / "lib/libunicorn.a").is_file():
            return p
    raise SystemExit("Unicorn not found")


def make_synthetic_pcode(work_dir: Path):
    # Register offsets: EAX=0, ECX=4, EDX=8, EBX=12, ESP=16
    def mov_eax_imm(addr, val):
        return {
            "a": addr, "len": 5, "m": f"MOV EAX,{hex(val)}",
            "ops": [["COPY", ["register", 0, 4], [["const", val, 4]]]],
        }

    def ret_insn(addr):
        return {
            "a": addr, "len": 1, "m": "RET",
            "ops": [
                ["INT_ADD", ["register", 16, 4], [["register", 16, 4], ["const", 4, 4]]],
                ["RETURN", None, []],
            ],
        }

    # 1. 0x401000: single-entry (MOV EAX, 111; RET)
    fn1 = {
        "entry": 0x401000,
        "name": "fn_single",
        "insns": [mov_eax_imm(0x401000, 111), ret_insn(0x401005)],
    }

    # 2. 0x401100 (primary) + 0x401110 (extra entry in gap layout)
    fn2 = {
        "entry": 0x401100,
        "name": "fn_multi",
        "insns": [
            mov_eax_imm(0x401100, 222), ret_insn(0x401105),
            mov_eax_imm(0x401110, 223), ret_insn(0x401115),
        ],
    }

    # 3. 0x401200 (2 non-contiguous chunks: [0x401200..0x401205) and [0x401280..0x401286))
    fn3 = {
        "entry": 0x401200,
        "name": "fn_split",
        "insns": [
            {"a": 0x401200, "len": 5, "m": "JMP 0x401280",
             "ops": [["BRANCH", None, [["ram", 0x401280, 4]]]]},
            mov_eax_imm(0x401280, 333),
            ret_insn(0x401285),
        ],
    }

    # 4. 0x401240 (control function sitting inside the gap between fn_split's two chunks)
    fn_gap = {
        "entry": 0x401240,
        "name": "fn_gap_ctrl",
        "insns": [mov_eax_imm(0x401240, 555), ret_insn(0x401245)],
    }

    # 5. 0x401300 (boot-hook target: MOV EAX, [ESP+4]; ADD EAX, EBX; RET)
    fn_boot = {
        "entry": 0x401300,
        "name": "fn_boot",
        "insns": [
            {
                "a": 0x401300, "len": 4, "m": "MOV EAX,dword ptr [ESP + 0x4]",
                "ops": [
                    ["INT_ADD", ["unique", 0x100, 4], [["register", 16, 4], ["const", 4, 4]]],
                    ["LOAD", ["register", 0, 4], [["const", 0, 4], ["unique", 0x100, 4]]],
                ],
            },
            {
                "a": 0x401304, "len": 2, "m": "ADD EAX,EBX",
                "ops": [["INT_ADD", ["register", 0, 4], [["register", 0, 4], ["register", 12, 4]]]],
            },
            ret_insn(0x401306),
        ],
    }

    # 6. 0x401380 (unpatched control function)
    fn_ctrl = {
        "entry": 0x401380,
        "name": "fn_ctrl",
        "insns": [mov_eax_imm(0x401380, 444), ret_insn(0x401385)],
    }

    # 7. Dynamic CALL/PUSH targets & functions (0x401400..0x4014b0)
    fn_tgt_a = {"entry": 0x401400, "name": "fn_tgt_a", "insns": [mov_eax_imm(0x401400, 600), ret_insn(0x401405)]}
    fn_tgt_b = {"entry": 0x401420, "name": "fn_tgt_b", "insns": [mov_eax_imm(0x401420, 700), ret_insn(0x401425)]}
    fn_dyn_call = {
        "entry": 0x401440,
        "name": "fn_dyn_call",
        "insns": [
            {
                "a": 0x401440, "len": 5, "m": "CALL 0x401400",
                "ops": [
                    ["INT_SUB", ["register", 16, 4], [["register", 16, 4], ["const", 4, 4]]],
                    ["STORE", None, [["const", 0, 4], ["register", 16, 4], ["const", 0x401445, 4]]],
                    ["CALL", None, [["ram", 0x401400, 4]]],
                ],
            },
            ret_insn(0x401445),
        ],
    }

    def push_imm_insn(addr, length, val, uoff):
        return {
            "a": addr, "len": length, "m": f"PUSH {hex(val)}",
            "ops": [
                ["COPY", ["unique", uoff, 4], [["const", val, 4]]],
                ["INT_SUB", ["register", 16, 4], [["register", 16, 4], ["const", 4, 4]]],
                ["STORE", None, [["const", 0, 4], ["register", 16, 4], ["unique", uoff, 4]]],
            ],
        }

    def pop_eax_insn(addr):
        return {
            "a": addr, "len": 1, "m": "POP EAX",
            "ops": [
                ["LOAD", ["register", 0, 4], [["const", 0, 4], ["register", 16, 4]]],
                ["INT_ADD", ["register", 16, 4], [["register", 16, 4], ["const", 4, 4]]],
            ],
        }

    fn_dyn_push8 = {
        "entry": 0x401480,
        "name": "fn_dyn_push8",
        "insns": [push_imm_insn(0x401480, 2, 0x50, 0x200), pop_eax_insn(0x401482), ret_insn(0x401483)],
    }
    fn_dyn_push32 = {
        "entry": 0x4014a0,
        "name": "fn_dyn_push32",
        "insns": [push_imm_insn(0x4014a0, 5, 0xC8, 0x210), pop_eax_insn(0x4014a5), ret_insn(0x4014a6)],
    }

    jsonl = work_dir / "test.jsonl"
    with open(jsonl, "w") as f:
        for fn in (fn1, fn2, fn3, fn_gap, fn_boot, fn_ctrl, fn_tgt_a, fn_tgt_b, fn_dyn_call, fn_dyn_push8, fn_dyn_push32):
            f.write(json.dumps(fn) + "\n")
    (work_dir / "test.jsonl.userops.json").write_text("[]")
    extra = work_dir / "extra.json"
    extra.write_text(json.dumps({str(0x401100): [0x401110]}))
    return jsonl, extra


HARNESS_C = r"""
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "FNVGuestMemory.h"
#include "host.h"
#include "rt.h"
#include "shims.h"

#define EMPTY_TABLE(name) const ShimTable name = {NULL, 0}
EMPTY_TABLE(win_shims);
EMPTY_TABLE(steam_shims);
EMPTY_TABLE(xlive_shims);
EMPTY_TABLE(wsock_shims);
EMPTY_TABLE(dinput_shims);
EMPTY_TABLE(dsound_shims);
EMPTY_TABLE(d3d9_shims);
EMPTY_TABLE(d3dx_shims);
EMPTY_TABLE(xinput_shims);
EMPTY_TABLE(media_shims);
EMPTY_TABLE(mmio_shims);

void host_pump_events(void) {}
int rt_has_fn(uint32_t a);
int host_run_guest(CPU *c, uint32_t entry);
extern uint64_t rt_fast_leaf_hits(void);

void g_401000(CPU *c);
void g_401100(CPU *c);
void g_401110(CPU *c);
void g_401200(CPU *c);
void g_401240(CPU *c);
void g_401300(CPU *c);
void g_401380(CPU *c);
void g_401400(CPU *c);
void g_401420(CPU *c);
void g_401440(CPU *c);
void g_401480(CPU *c);
void g_4014a0(CPU *c);

static void put32(uint32_t a, uint32_t v) { memcpy(MEM + a, &v, 4); }
static void write_mov_ret(uint32_t a, uint32_t val) {
    MEM[a] = 0xB8;
    put32(a + 1, val);
    MEM[a + 5] = 0xC3;
}

static void vp_patch_bytes(CPU *c, uint32_t addr, const void *data, uint32_t sz) {
    uint32_t vp = host_bind_import("kernel32", "VirtualProtect");
    uint32_t oldp = gmalloc(4);
    uint32_t esp = REG(c, R_ESP);
    uint32_t sp = esp - 32;
    P32(sp, HOST_EXIT_THUNK);
    P32(sp + 4, addr);
    P32(sp + 8, sz);
    P32(sp + 12, 0x40);
    P32(sp + 16, oldp);
    SETREG(c, R_ESP, sp);
    rt_import_call(c, vp);
    assert(REG(c, R_EAX) == 1);
    memcpy(MEM + addr, data, sz);
    sp = esp - 32;
    P32(sp, HOST_EXIT_THUNK);
    P32(sp + 4, addr);
    P32(sp + 8, sz);
    P32(sp + 12, G32(oldp));
    P32(sp + 16, oldp);
    SETREG(c, R_ESP, sp);
    rt_import_call(c, vp);
    assert(REG(c, R_EAX) == 1);
    SETREG(c, R_ESP, esp);
}

static void vp_patch_u32(CPU *c, uint32_t addr, uint32_t val) {
    vp_patch_bytes(c, addr, &val, 4);
}

static uint32_t call_direct(CPU *c, uint32_t base_esp, GuestFn fn, uint32_t arg0) {
    uint32_t sp = base_esp - 16;
    P32(sp, HOST_EXIT_THUNK);
    P32(sp + 4, arg0);
    SETREG(c, R_ESP, sp);
    SETREG(c, R_EAX, 0xDEADu);
    fn(c);
    uint32_t r = REG(c, R_EAX);
    SETREG(c, R_ESP, base_esp);
    return r;
}

static uint32_t call_indirect(CPU *c, uint32_t base_esp, uint32_t addr, uint32_t arg0) {
    uint32_t sp = base_esp - 16;
    P32(sp, HOST_EXIT_THUNK);
    P32(sp + 4, arg0);
    SETREG(c, R_ESP, sp);
    SETREG(c, R_EAX, 0xDEADu);
    assert(host_run_guest(c, addr) == 0);
    uint32_t r = REG(c, R_EAX);
    SETREG(c, R_ESP, base_esp);
    return r;
}

static int hook_calls = 0;
static void test_boot_hook(CPU *c, uint32_t addr) {
    assert(addr == 0x401300u);
    assert(REG(c, 12) == 23u);
    assert(G32(REG(c, R_ESP) + 4) == 100u);
    hook_calls++;
    CPU saved = *c;
    // Clobber registers and stack to verify caller/boot-hook preservation.
    SETREG(c, R_EAX, 0xBAD00001u);
    SETREG(c, 12, 0xBAD00002u);
    SETREG(c, R_ESP, REG(c, R_ESP) - 128u);
    *c = saved;
}

static uint32_t va_alloc_exec(CPU *c, uint32_t sz) {
    uint32_t va = host_bind_import("kernel32", "VirtualAlloc");
    uint32_t esp = REG(c, R_ESP);
    uint32_t sp = esp - 32u;
    P32(sp, HOST_EXIT_THUNK);
    P32(sp + 4u, 0u);
    P32(sp + 8u, sz);
    P32(sp + 12u, 0x3000u);
    P32(sp + 16u, 0x40u);
    SETREG(c, R_ESP, sp);
    rt_import_call(c, va);
    uint32_t addr = REG(c, R_EAX);
    SETREG(c, R_ESP, esp);
    assert(addr != 0u && host_addr_is_executable(addr));
    return addr;
}

typedef struct {
    uint32_t entry, lock_va, cnt_va, esi_val, iters;
} AtomicWorkerArg;

static void *atomic_worker(void *p) {
    AtomicWorkerArg *a = (AtomicWorkerArg *)p;
    CPU wcpu = {0};
    host_thread_init(&wcpu, 65536u);
    ST(wcpu.r + 4256, 0x027Fu, 2);
    uint32_t sp = REG(&wcpu, R_ESP) - 16u;
    P32(sp, HOST_EXIT_THUNK);
    SETREG(&wcpu, R_ESP, sp);
    SETREG(&wcpu, R_ECX, a->lock_va);
    SETREG(&wcpu, R_EDX, a->cnt_va);
    SETREG(&wcpu, 24u, a->esi_val);
    SETREG(&wcpu, 12u, a->iters);
    SETREG(&wcpu, R_EAX, 1u);
    assert(host_run_guest(&wcpu, a->entry) == 0);
    return NULL;
}

int main(void) {
    void *mem = fnv_reserve_guest();
    assert(mem != MAP_FAILED);
    MEM = mem;

    char *fake_argv[] = {"patch_deopt_test.exe"};
    host_init_args(1, fake_argv);
    host_image_base = 0x00400000u;
    host_text_start = 0x00401000u;
    host_text_end = 0x00402000u;
    P32(host_image_base + 0x3C, 0x80);
    P32(host_image_base + 0x80 + 24 + 56, 0x00100000u);

    // Populate guest x86 machine code for each function.
    write_mov_ret(0x401000, 111);
    write_mov_ret(0x401100, 222);
    write_mov_ret(0x401110, 223);
    MEM[0x401200] = 0xE9; put32(0x401201, 0x401280u - 0x401205u); // JMP 0x401280
    write_mov_ret(0x401280, 333);
    write_mov_ret(0x401240, 555);
    // 0x401300: mov eax,[esp+4]; add eax,ebx; ret
    memcpy(MEM + 0x401300, "\x8B\x44\x24\x04\x01\xD8\xC3", 7);
    write_mov_ret(0x401380, 444);
    write_mov_ret(0x401400, 600);
    write_mov_ret(0x401420, 700);
    MEM[0x401440] = 0xE8; put32(0x401441, 0x401400u - 0x401445u); MEM[0x401445] = 0xC3;
    MEM[0x401480] = 0x6A; MEM[0x401481] = 0x50; MEM[0x401482] = 0x58; MEM[0x401483] = 0xC3;
    MEM[0x4014a0] = 0x68; put32(0x4014a1, 0xC8u); MEM[0x4014a5] = 0x58; MEM[0x4014a6] = 0xC3;

    static CPU cpu;
    CPU *c = &cpu;
    host_thread_init(c, 1 << 20);
    ST(c->r + 4256, 0x027F, 2);
    uint32_t base_esp = REG(c, R_ESP);

    // 1. Before patching: all native, all rt_has_fn == 1.
    assert(rt_has_fn(0x401000) == 1);
    assert(rt_has_fn(0x401100) == 1);
    assert(rt_has_fn(0x401110) == 1);
    assert(rt_has_fn(0x401200) == 1);
    assert(rt_has_fn(0x401240) == 1);
    assert(rt_has_fn(0x401300) == 1);
    assert(rt_has_fn(0x401380) == 1);
    assert(call_direct(c, base_esp, g_401000, 0) == 111u);
    assert(call_direct(c, base_esp, g_401100, 0) == 222u);
    assert(call_direct(c, base_esp, g_401110, 0) == 223u);
    assert(call_direct(c, base_esp, g_401200, 0) == 333u);
    assert(call_direct(c, base_esp, g_401240, 0) == 555u);
    assert(call_direct(c, base_esp, g_401380, 0) == 444u);

    // 2. Patch single-entry fn at 0x401001 -> 999 via VirtualProtect.
    vp_patch_u32(c, 0x401001, 999u);
    assert(rt_has_fn(0x401000) == 0);
    assert(call_direct(c, base_esp, g_401000, 0) == 999u);
    assert(call_indirect(c, base_esp, 0x401000, 0) == 999u);

    // 3. Patch ONLY inside extra entry 0x401111 -> 889 (and write 888 at 0x401101 directly).
    // Both 0x401100 and 0x401110 belong to the same group, so both must deoptimize!
    put32(0x401101, 888u);
    vp_patch_u32(c, 0x401111, 889u);
    assert(rt_has_fn(0x401100) == 0);
    assert(rt_has_fn(0x401110) == 0);
    assert(call_direct(c, base_esp, g_401100, 0) == 888u);
    assert(call_direct(c, base_esp, g_401110, 0) == 889u);

    // 4. Patch non-contiguous 2nd chunk 0x401281 -> 777; 0x401200 deoptimizes, gap fn 0x401240 stays native!
    vp_patch_u32(c, 0x401281, 777u);
    assert(rt_has_fn(0x401200) == 0);
    assert(rt_has_fn(0x401240) == 1);
    assert(call_direct(c, base_esp, g_401200, 0) == 777u);
    assert(call_direct(c, base_esp, g_401240, 0) == 555u);

    // 5. FlushInstructionCache(proc, NULL, 0) bumps epoch without deoptimizing 0x401380.
    uint64_t ep_before = rt_patch_epoch();
    uint32_t gcount_before = rt_patched_group_count();
    uint32_t fic = host_bind_import("kernel32", "FlushInstructionCache");
    uint32_t sp = base_esp - 32;
    P32(sp, HOST_EXIT_THUNK); P32(sp + 4, 0xFFFFFFFFu); P32(sp + 8, 0); P32(sp + 12, 0);
    SETREG(c, R_ESP, sp);
    rt_import_call(c, fic);
    SETREG(c, R_ESP, base_esp);
    assert(rt_patch_epoch() > ep_before);
    assert(rt_patched_group_count() == gcount_before);
    assert(rt_has_fn(0x401380) == 1);
    assert(call_direct(c, base_esp, g_401380, 0) == 444u);

    // 6. Boot hook fires once on first g_401300(c), preserves stack args and EBX, then disarms.
    rt_set_boot_hook(0x401300u, test_boot_hook);
    assert(rt_has_fn(0x401300) == 0);
    SETREG(c, 12, 23u);
    assert(call_direct(c, base_esp, g_401300, 100u) == 123u);
    assert(hook_calls == 1);
    assert(REG(c, 12) == 23u);
    SETREG(c, 12, 23u);
    assert(call_direct(c, base_esp, g_401300, 200u) == 223u);
    assert(hook_calls == 1);

    // 7. Dynamic CALL rel32 patch keeps function native (state 2), redirects natively.
    assert(rt_has_fn(0x401440) == 1);
    assert(call_direct(c, base_esp, g_401440, 0) == 600u);
    uint32_t dyn_before = rt_dyn_group_count();
    vp_patch_u32(c, 0x401441, 0x401420u - 0x401445u);
    assert(rt_has_fn(0x401440) == 1);
    assert(rt_dyn_group_count() == dyn_before + 1);
    assert(call_direct(c, base_esp, g_401440, 0) == 700u);

    // 8a. Flag-preserving 5B in-place CALL replacements keep function native (state 2).
    uint32_t scratch = gpages(4096);
    uint32_t obj = scratch + 0x100u;
    uint32_t gvar = scratch + 0x500u;
    uint32_t pat_before = rt_patched_group_count();

    uint8_t mov_imm[5] = {0xB8, 0xE7, 0x03, 0x00, 0x00};
    vp_patch_bytes(c, 0x401440, mov_imm, sizeof mov_imm);
    assert(rt_has_fn(0x401440) == 1);
    assert(rt_dyn_group_count() == dyn_before + 1);
    assert(rt_patched_group_count() == pat_before);
    assert(call_direct(c, base_esp, g_401440, 0) == 999u);

    uint8_t mov_rr_nop3[5] = {0x89, 0xC8, 0x0F, 0x1F, 0x00};
    vp_patch_bytes(c, 0x401440, mov_rr_nop3, sizeof mov_rr_nop3);
    assert(rt_has_fn(0x401440) == 1);
    SETREG(c, R_ECX, 0x55AAu);
    assert(call_direct(c, base_esp, g_401440, 0) == 0x55AAu);

    uint8_t mov_mem8_nop2[5] = {0x8B, 0x41, 0x08, 0x66, 0x90};
    vp_patch_bytes(c, 0x401440, mov_mem8_nop2, sizeof mov_mem8_nop2);
    assert(rt_has_fn(0x401440) == 1);
    P32(scratch + 8u, 0x778899AAu);
    SETREG(c, R_ECX, scratch);
    assert(call_direct(c, base_esp, g_401440, 0) == 0x778899AAu);

    uint8_t nop5[5] = {0x90, 0x90, 0x90, 0x90, 0x90};
    vp_patch_bytes(c, 0x401440, nop5, sizeof nop5);
    assert(rt_has_fn(0x401440) == 1);
    assert(call_direct(c, base_esp, g_401440, 0) == 0xDEADu);

    // 8b. Unsupported 5B replacement (XOR EAX, EAX modifies flags) upgrades group 2 -> 1 cleanly.
    uint8_t xor_nop3[5] = {0x31, 0xC0, 0x90, 0x90, 0x90};
    vp_patch_bytes(c, 0x401440, xor_nop3, sizeof xor_nop3);
    assert(rt_has_fn(0x401440) == 0);
    assert(rt_dyn_group_count() == dyn_before);
    assert(rt_patched_group_count() == pat_before + 1);
    assert(call_direct(c, base_esp, g_401440, 0) == 0u);

    // 9. Dynamic PUSH imm8 (0x401480) and PUSH imm32 (0x4014a0) stay native with updated immediates.
    assert(call_direct(c, base_esp, g_401480, 0) == 0x50u);
    uint8_t new_imm8 = 0x54u;
    vp_patch_bytes(c, 0x401481, &new_imm8, 1);
    assert(rt_has_fn(0x401480) == 1);
    assert(call_direct(c, base_esp, g_401480, 0) == 0x54u);

    assert(call_direct(c, base_esp, g_4014a0, 0) == 0xC8u);
    vp_patch_u32(c, 0x4014a1, 0xD0u);
    assert(rt_has_fn(0x4014a0) == 1);
    assert(call_direct(c, base_esp, g_4014a0, 0) == 0xD0u);

    // 10. Differential fast-leaf interpreter tests (real patched leaf patterns).
    int expect_fast = (getenv("FNV_NO_FAST_LEAF") == NULL);

    // 10a. 0x61a310: mov eax, [esp+4]; mov eax, [eax+0x50]; ret
    memcpy(MEM + 0x401800, "\x8B\x44\x24\x04\x8B\x40\x50\xC3", 8);
    P32(obj + 0x50u, 0x12345678u);
    uint64_t hits0 = rt_fast_leaf_hits();
    assert(call_indirect(c, base_esp, 0x401800, obj) == 0x12345678u);
    assert(expect_fast ? (rt_fast_leaf_hits() == hits0 + 1u) : (rt_fast_leaf_hits() == 0u));

    // 10b. 0x4720d0: xorps xmm0, xmm0; movq [ecx+0x258], xmm0; ret
    memcpy(MEM + 0x401820, "\x0F\x57\xC0\x66\x0F\xD6\x81\x58\x02\x00\x00\xC3", 12);
    memset(c->r + 4608, 0xAA, 16);
    P32(obj + 0x258u, 0xAAAAAAAAu);
    P32(obj + 0x25Cu, 0xBBBBBBBBu);
    SETREG(c, R_ECX, obj);
    hits0 = rt_fast_leaf_hits();
    (void)call_indirect(c, base_esp, 0x401820, 0);
    assert(expect_fast ? (rt_fast_leaf_hits() == hits0 + 1u) : (rt_fast_leaf_hits() == 0u));
    assert(G32(obj + 0x258u) == 0u && G32(obj + 0x25Cu) == 0u);
    for (int zi = 0; zi < 16; zi++) assert(c->r[4608 + zi] == 0u);

    // 10c. 0x472100: mov eax,[ecx+0x240]; cmp eax,[gvar]; je +10; test [ecx+0x248],0x40000; setne al; ret
    uint8_t p472100[28] = {
        0x8B, 0x81, 0x40, 0x02, 0x00, 0x00,
        0x3B, 0x05, 0, 0, 0, 0,
        0x74, 0x0A,
        0xF7, 0x81, 0x48, 0x02, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
        0x0F, 0x95, 0xC0,
        0xC3
    };
    memcpy(p472100 + 8, &gvar, 4);
    memcpy(MEM + 0x401840, p472100, sizeof p472100);
    P32(gvar, 0xABCDEF42u);
    P32(obj + 0x240u, 0xABCDEF42u);
    P32(obj + 0x248u, 0x40000u);
    SETREG(c, R_ECX, obj);
    assert((call_indirect(c, base_esp, 0x401840, 0) & 0xFFu) == 0u); // equal -> je -> ZF=1 -> AL=0
    P32(obj + 0x240u, 0xABCDEF00u);
    SETREG(c, R_ECX, obj);
    assert((call_indirect(c, base_esp, 0x401840, 0) & 0xFFu) == 1u); // unequal, bit set -> AL=1
    P32(obj + 0x248u, 0u);
    SETREG(c, R_ECX, obj);
    assert((call_indirect(c, base_esp, 0x401840, 0) & 0xFFu) == 0u); // unequal, bit clear -> AL=0

    // 10d. 0x618660: mov eax,[ecx+0x40]; test eax,eax; je +1; ret; mov eax,0x01011584; ret
    memcpy(MEM + 0x401870, "\x8B\x41\x40\x85\xC0\x74\x01\xC3\xB8\x84\x15\x01\x01\xC3", 14);
    P32(obj + 0x40u, 0x77u);
    SETREG(c, R_ECX, obj);
    assert(call_indirect(c, base_esp, 0x401870, 0) == 0x77u);
    P32(obj + 0x40u, 0u);
    SETREG(c, R_ECX, obj);
    assert(call_indirect(c, base_esp, 0x401870, 0) == 0x01011584u);

    // 10e. 0x659210 (RET 8): mov eax,[esp+8]; movsx ecx,byte [esp+4]; shl eax,5; add eax,[esp+8]; add eax,ecx; ret 8
    memcpy(MEM + 0x401890, "\x8B\x44\x24\x08\x0F\xBE\x4C\x24\x04\xC1\xE0\x05\x03\x44\x24\x08\x03\xC1\xC2\x08\x00", 21);
    {
        uint32_t sp2 = base_esp - 24u;
        P32(sp2, HOST_EXIT_THUNK);
        P32(sp2 + 4u, 0xFFFFFFFEu); // signed byte -2
        P32(sp2 + 8u, 10u);
        SETREG(c, R_ESP, sp2);
        hits0 = rt_fast_leaf_hits();
        assert(host_run_guest(c, 0x401890u) == 0);
        assert(expect_fast ? (rt_fast_leaf_hits() == hits0 + 1u) : (rt_fast_leaf_hits() == 0u));
        assert(REG(c, R_EAX) == 328u);
        assert(REG(c, R_ESP) == sp2 + 12u);
        SETREG(c, R_ESP, base_esp);
    }

    // 10f. 0x601c30: CMOVNE + SUB + TEST
    memcpy(MEM + 0x4018B0,
           "\x8B\x41\x04\x85\xC0\x74\x01\xC3\x81\xE9\xD8\x00\x00\x00\xF7\x01\x01\x00\x00\x00"
           "\xB8\x11\x11\x11\x11\xB9\x22\x22\x22\x22\x0F\x45\xC1\xC3", 34);
    uint32_t node = scratch + 0x700u;
    P32(node + 0xD8u + 4u, 0u);
    P32(node, 0u);
    SETREG(c, R_ECX, node + 0xD8u);
    assert(call_indirect(c, base_esp, 0x4018B0u, 0) == 0x11111111u);
    P32(node, 1u);
    SETREG(c, R_ECX, node + 0xD8u);
    assert(call_indirect(c, base_esp, 0x4018B0u, 0) == 0x22222222u);

    // 10g. JMP rel32 trampoline + PUSH/POP + INC/DEC preserving CF
    MEM[0x4018E0] = 0xE9; put32(0x4018E1, 0x401900u - 0x4018E5u);
    memcpy(MEM + 0x401900, "\x50\xB8\x00\x00\x00\x00\x58\x40\x4A\xC3", 10);
    {
        uint32_t sp2 = base_esp - 16u;
        P32(sp2, HOST_EXIT_THUNK);
        SETREG(c, R_ESP, sp2);
        SETREG(c, R_EAX, 10u);
        SETREG(c, R_EDX, 5u);
        c->r[512] = 1u; // CF = 1
        hits0 = rt_fast_leaf_hits();
        assert(host_run_guest(c, 0x4018E0u) == 0);
        assert(expect_fast ? (rt_fast_leaf_hits() == hits0 + 1u) : (rt_fast_leaf_hits() == 0u));
        assert(REG(c, R_EAX) == 11u);
        assert(REG(c, R_EDX) == 4u);
        assert((c->r[512] & 1u) == 1u);
        assert((c->r[518] & 1u) == 0u && (c->r[519] & 1u) == 0u);
        SETREG(c, R_ESP, base_esp);
    }

    // 10h. E9 tail-jump to native g_401400
    MEM[0x401980] = 0xE9; put32(0x401981, 0x401400u - 0x401985u);
    {
        uint32_t sp2 = base_esp - 16u;
        P32(sp2, HOST_EXIT_THUNK);
        SETREG(c, R_ESP, sp2);
        hits0 = rt_fast_leaf_hits();
        assert(host_run_guest(c, 0x401980u) == 0);
        assert(expect_fast ? (rt_fast_leaf_hits() == hits0 + 1u) : (rt_fast_leaf_hits() == 0u));
        assert(REG(c, R_EAX) == 600u);
        assert(REG(c, R_ESP) == sp2 + 4u);
        SETREG(c, R_ESP, base_esp);
    }

    // 10i. 0x401000 CALL rel32 wrapper thunk: push [esp+4]; mov ecx, 0x11f6238; call 0x401400; add esp, 4; ret
    memcpy(MEM + 0x401990, "\xFF\x74\x24\x04\xB9\x38\x62\x1F\x01\xE8", 10);
    put32(0x40199A, 0x401400u - 0x40199Eu);
    memcpy(MEM + 0x40199E, "\x83\xC4\x04\xC3", 4);
    {
        uint32_t sp2 = base_esp - 16u;
        P32(sp2, HOST_EXIT_THUNK);
        P32(sp2 + 4u, 0x123u);
        SETREG(c, R_ESP, sp2);
        hits0 = rt_fast_leaf_hits();
        assert(host_run_guest(c, 0x401990u) == 0);
        assert(expect_fast ? (rt_fast_leaf_hits() == hits0 + 1u) : (rt_fast_leaf_hits() == 0u));
        assert(REG(c, R_EAX) == 600u);
        assert(REG(c, R_ECX) == 0x11F6238u);
        assert(REG(c, R_ESP) == sp2 + 4u);
        SETREG(c, R_ESP, base_esp);
    }

    // 10j. Atomic LOCK XADD / LOCK DEC + indirect JMP + LOCK CMPXCHG (0x4019a0 & 0x401970 patterns)
    // 10j-1: mov ecx,[esp+4]; or eax,-1; lock xadd [ecx],eax; dec eax; ret
    memcpy(MEM + 0x4019D0, "\x8B\x4C\x24\x04\x83\xC8\xFF\xF0\x0F\xC1\x01\x48\xC3", 13);
    P32(scratch + 0x900u, 5u);
    {
        hits0 = rt_fast_leaf_hits();
        assert(call_indirect(c, base_esp, 0x4019D0u, scratch + 0x900u) == 4u);
        assert(expect_fast ? (rt_fast_leaf_hits() == hits0 + 1u) : (rt_fast_leaf_hits() == 0u));
        assert(G32(scratch + 0x900u) == 4u);
    }

    // 10j-2: lock dec [ecx+4]; jne +5; mov eax,[ecx]; jmp [eax+4]; ret
    memcpy(MEM + 0x4019E0, "\xF0\xFF\x49\x04\x75\x05\x8B\x01\xFF\x60\x04\xC3", 12);
    uint32_t vtbl = scratch + 0x910u;
    uint32_t rcobj = scratch + 0x930u;
    P32(vtbl + 4u, 0x401400u);
    P32(rcobj + 0u, vtbl);
    P32(rcobj + 4u, 2u);
    {
        uint32_t sp2 = base_esp - 16u;
        P32(sp2, HOST_EXIT_THUNK);
        SETREG(c, R_ESP, sp2);
        SETREG(c, R_ECX, rcobj);
        SETREG(c, R_EAX, 0x77u);
        c->r[512] = 1u; // CF = 1 preserved across lock dec
        hits0 = rt_fast_leaf_hits();
        assert(host_run_guest(c, 0x4019E0u) == 0);
        assert(expect_fast ? (rt_fast_leaf_hits() == hits0 + 1u) : (rt_fast_leaf_hits() == 0u));
        assert(G32(rcobj + 4u) == 1u);
        assert(REG(c, R_EAX) == 0x77u);
        assert((c->r[512] & 1u) == 1u);

        P32(sp2, HOST_EXIT_THUNK);
        SETREG(c, R_ESP, sp2);
        SETREG(c, R_ECX, rcobj);
        hits0 = rt_fast_leaf_hits();
        assert(host_run_guest(c, 0x4019E0u) == 0);
        assert(expect_fast ? (rt_fast_leaf_hits() == hits0 + 1u) : (rt_fast_leaf_hits() == 0u));
        assert(G32(rcobj + 4u) == 0u);
        assert(REG(c, R_EAX) == 600u); // tail-jumped to g_401400 via [eax+4]!
        SETREG(c, R_ESP, base_esp);
    }

    // 10j-3: lock cmpxchg [ecx], edx (hit & miss)
    memcpy(MEM + 0x4019F0, "\xF0\x0F\xB1\x11\xC3", 5);
    P32(scratch + 0x940u, 10u);
    {
        uint32_t sp2 = base_esp - 16u;
        P32(sp2, HOST_EXIT_THUNK);
        SETREG(c, R_ESP, sp2);
        SETREG(c, R_ECX, scratch + 0x940u);
        SETREG(c, R_EAX, 10u);
        SETREG(c, R_EDX, 42u);
        assert(host_run_guest(c, 0x4019F0u) == 0);
        assert(G32(scratch + 0x940u) == 42u && REG(c, R_EAX) == 10u && (c->r[518] & 1u) == 1u);

        P32(sp2, HOST_EXIT_THUNK);
        SETREG(c, R_ESP, sp2);
        SETREG(c, R_ECX, scratch + 0x940u);
        SETREG(c, R_EAX, 99u);
        SETREG(c, R_EDX, 77u);
        assert(host_run_guest(c, 0x4019F0u) == 0);
        assert(G32(scratch + 0x940u) == 42u && REG(c, R_EAX) == 42u && (c->r[518] & 1u) == 0u);
        SETREG(c, R_ESP, base_esp);
    }

    // 11. Bailout atomicity: unsupported opcode & unbalanced ESP RET fall through cleanly to Unicorn.
    // 11a. mov dword ptr [ecx], 0x11223344; imul eax, eax, 3 (0x6B C0 03); ret
    memcpy(MEM + 0x401940, "\xC7\x01\x44\x33\x22\x11\x6B\xC0\x03\xC3", 10);
    P32(scratch + 0x800u, 0xDEADBEEFu);
    {
        uint32_t sp2 = base_esp - 16u;
        P32(sp2, HOST_EXIT_THUNK);
        SETREG(c, R_ESP, sp2);
        SETREG(c, R_ECX, scratch + 0x800u);
        SETREG(c, R_EAX, 7u);
        hits0 = rt_fast_leaf_hits();
        assert(host_run_guest(c, 0x401940u) == 0);
        assert(rt_fast_leaf_hits() == hits0); // fast_leaf must have bailed out!
        assert(REG(c, R_EAX) == 21u);
        assert(G32(scratch + 0x800u) == 0x11223344u);
        SETREG(c, R_ESP, base_esp);
    }

    // 11b. mov dword ptr [ecx], 0x12345678; sub esp, 4; mov eax, [esp+4]; mov [esp], eax; ret
    memcpy(MEM + 0x401960, "\xC7\x01\x78\x56\x34\x12\x83\xEC\x04\x8B\x44\x24\x04\x89\x04\x24\xC3", 17);
    P32(scratch + 0x804u, 0xCAFEBABEu);
    {
        uint32_t sp2 = base_esp - 16u;
        P32(sp2, HOST_EXIT_THUNK);
        SETREG(c, R_ESP, sp2);
        SETREG(c, R_ECX, scratch + 0x804u);
        hits0 = rt_fast_leaf_hits();
        assert(host_run_guest(c, 0x401960u) == 0);
        assert(rt_fast_leaf_hits() == hits0); // unbalanced ESP at RET -> bailed out to Unicorn!
        assert(REG(c, R_ESP) == sp2);
        assert(G32(scratch + 0x804u) == 0x12345678u);
        SETREG(c, R_ESP, base_esp);
    }

    // 11c. Bailout AFTER native CALL rel32: call 0x401400; imul eax, eax, 3; ret
    MEM[0x4019B0] = 0xE8; put32(0x4019B1, 0x401400u - 0x4019B5u);
    memcpy(MEM + 0x4019B5, "\x6B\xC0\x03\xC3", 4);
    {
        uint32_t sp2 = base_esp - 16u;
        P32(sp2, HOST_EXIT_THUNK);
        SETREG(c, R_ESP, sp2);
        hits0 = rt_fast_leaf_hits();
        assert(host_run_guest(c, 0x4019B0u) == 0);
        assert(rt_fast_leaf_hits() == hits0); // bailed on imul -> Unicorn resumed at 0x4019B5!
        assert(REG(c, R_EAX) == 1800u);
        assert(REG(c, R_ESP) == sp2 + 4u);
        SETREG(c, R_ESP, base_esp);
    }

    // 11d. Bailout AFTER atomic LOCK INC: lock inc [ecx]; imul eax, eax, 3; ret (must execute LOCK INC once!)
    memcpy(MEM + 0x4019C0, "\xF0\xFF\x01\x6B\xC0\x03\xC3", 7);
    P32(scratch + 0x950u, 10u);
    {
        uint32_t sp2 = base_esp - 16u;
        P32(sp2, HOST_EXIT_THUNK);
        SETREG(c, R_ESP, sp2);
        SETREG(c, R_ECX, scratch + 0x950u);
        SETREG(c, R_EAX, 5u);
        hits0 = rt_fast_leaf_hits();
        assert(host_run_guest(c, 0x4019C0u) == 0);
        assert(rt_fast_leaf_hits() == hits0);
        assert(G32(scratch + 0x950u) == 11u); // NOT 12!
        assert(REG(c, R_EAX) == 15u);
        SETREG(c, R_ESP, base_esp);
    }

    // 12. Multi-threaded Unicorn LOCK / XCHG stress + atomic opcode coverage (non-.text PE-like page)
    uint32_t exec_page = va_alloc_exec(c, 0x1000u);

    // 12a. Exact JIP 0x10005540 spinlock + non-LOCK unlock across 2 concurrent Unicorn threads:
    //   imul eax,eax,1; loop: xor eax,eax; lock cmpxchg [ecx],esi; jne loop;
    //   inc [edx]; and dword ptr [ecx],0; dec ebx; jne loop (has 0xF0 disp8!); ret
    uint32_t fn_spin = exec_page;
    memcpy(MEM + fn_spin,
           "\x6B\xC0\x01\x33\xC0\xF0\x0F\xB1\x31\x75\xF8\xFF\x02\x83\x21\x00\x4B\x75\xF0\xC3", 20);
    P32(scratch + 0xA00u, 0u);
    P32(scratch + 0xA04u, 0u);
    {
        pthread_t th[2];
        AtomicWorkerArg wa[2] = {
            {fn_spin, scratch + 0xA00u, scratch + 0xA04u, 0x1111u, 25000u},
            {fn_spin, scratch + 0xA00u, scratch + 0xA04u, 0x2222u, 25000u},
        };
        assert(pthread_create(&th[0], NULL, atomic_worker, &wa[0]) == 0);
        assert(pthread_create(&th[1], NULL, atomic_worker, &wa[1]) == 0);
        pthread_join(th[0], NULL);
        pthread_join(th[1], NULL);
        assert(G32(scratch + 0xA04u) == 50000u);
        assert(G32(scratch + 0xA00u) == 0u);
    }

    // 12a-edge. Same spinlock reached across a TB edge (imul + jmp +2 -> loop) so UC_HOOK_EDGE_GENERATED path is stressed
    uint32_t fn_spin_edge = exec_page + 0x20u;
    memcpy(MEM + fn_spin_edge,
           "\x6B\xC0\x01\xEB\x02\x90\x90\x33\xC0\xF0\x0F\xB1\x31\x75\xF8\xFF\x02\x83\x21\x00\x4B\x75\xF0\xC3", 24);
    P32(scratch + 0xA08u, 0u);
    P32(scratch + 0xA0Cu, 0u);
    {
        pthread_t th[2];
        AtomicWorkerArg wa[2] = {
            {fn_spin_edge, scratch + 0xA08u, scratch + 0xA0Cu, 0x3333u, 25000u},
            {fn_spin_edge, scratch + 0xA08u, scratch + 0xA0Cu, 0x4444u, 25000u},
        };
        assert(pthread_create(&th[0], NULL, atomic_worker, &wa[0]) == 0);
        assert(pthread_create(&th[1], NULL, atomic_worker, &wa[1]) == 0);
        pthread_join(th[0], NULL);
        pthread_join(th[1], NULL);
        assert(G32(scratch + 0xA0Cu) == 50000u);
        assert(G32(scratch + 0xA08u) == 0u);
    }

    // 12b. Implicit-LOCK XCHG [ecx], al (0x86) spinlock across 2 concurrent Unicorn threads
    uint32_t fn_xchg = exec_page + 0x40u;
    memcpy(MEM + fn_xchg,
           "\x6B\xC0\x01\xB0\x01\x86\x01\x84\xC0\x75\xF8\xFF\x02\xC6\x01\x00\x4B\x75\xF0\xC3", 20);
    P32(scratch + 0xA10u, 0u);
    P32(scratch + 0xA14u, 0u);
    {
        pthread_t th[2];
        AtomicWorkerArg wa[2] = {
            {fn_xchg, scratch + 0xA10u, scratch + 0xA14u, 1u, 20000u},
            {fn_xchg, scratch + 0xA10u, scratch + 0xA14u, 2u, 20000u},
        };
        assert(pthread_create(&th[0], NULL, atomic_worker, &wa[0]) == 0);
        assert(pthread_create(&th[1], NULL, atomic_worker, &wa[1]) == 0);
        pthread_join(th[0], NULL);
        pthread_join(th[1], NULL);
        assert(G32(scratch + 0xA14u) == 40000u);
        assert(G32(scratch + 0xA10u) == 0u);
    }

    // 12c. Single-thread Unicorn atomic opcode coverage (each begins with `imul eax,eax,1` -> forces Unicorn)
    {
        uint32_t sp2 = base_esp - 16u;
        uint32_t target = scratch + 0xA20u;

        // LOCK OR byte [ecx], al; LOCK OR dword [ecx+4], eax; LOCK AND [ecx+8], eax; LOCK XOR [ecx+12], eax
        uint32_t f_alu = exec_page + 0x100u;
        memcpy(MEM + f_alu,
               "\x6B\xC0\x01\xF0\x08\x01\xF0\x09\x41\x04\xF0\x21\x41\x08\xF0\x31\x41\x0C\xC3", 19);
        P32(target + 0u, 0x12u);
        P32(target + 4u, 0x100u);
        P32(target + 8u, 0xFFu);
        P32(target + 12u, 0xAAu);
        P32(sp2, HOST_EXIT_THUNK);
        SETREG(c, R_ESP, sp2);
        SETREG(c, R_ECX, target);
        SETREG(c, R_EAX, 0x41u);
        assert(host_run_guest(c, f_alu) == 0);
        assert((G32(target + 0u) & 0xFFu) == 0x53u);
        assert(G32(target + 4u) == 0x141u);
        assert(G32(target + 8u) == 0x41u);
        assert(G32(target + 12u) == 0xEBu);

        // LOCK ADD / ADC (with STC) / SUB imm8 / SBB (with STC) / NOT / NEG
        uint32_t f_rmw = exec_page + 0x140u;
        // imul eax,eax,1; stc; lock adc [ecx], eax; stc; lock sbb [ecx+4], eax; lock not [ecx+8]; stc; lock neg [ecx+12]; ret
        memcpy(MEM + f_rmw,
               "\x6B\xC0\x01\xF9\xF0\x11\x01\xF9\xF0\x19\x41\x04\xF0\xF7\x51\x08\xF9\xF0\xF7\x59\x0C\xC3", 22);
        P32(target + 0u, 10u);
        P32(target + 4u, 20u);
        P32(target + 8u, 0x0F0F0F0Fu);
        P32(target + 12u, 5u);
        P32(sp2, HOST_EXIT_THUNK);
        SETREG(c, R_ESP, sp2);
        SETREG(c, R_ECX, target);
        SETREG(c, R_EAX, 3u);
        assert(host_run_guest(c, f_rmw) == 0);
        assert(G32(target + 0u) == 14u);         // 10 + 3 + CF(1)
        assert(G32(target + 4u) == 16u);         // 20 - 3 - CF(1)
        assert(G32(target + 8u) == 0xF0F0F0F0u);
        assert(G32(target + 12u) == (uint32_t)-5);
        assert((c->r[512] & 1u) == 1u);          // NEG(5) sets CF=1

        // LOCK BTS [ecx], 5 then LOCK BTC [ecx], 5
        uint32_t f_bits = exec_page + 0x180u;
        memcpy(MEM + f_bits,
               "\x6B\xC0\x01\xF0\x0F\xBA\x29\x05\xF0\x0F\xBA\x39\x05\xC3", 14);
        P32(target + 0u, 0u);
        P32(sp2, HOST_EXIT_THUNK);
        SETREG(c, R_ESP, sp2);
        SETREG(c, R_ECX, target);
        SETREG(c, R_EAX, 1u);
        assert(host_run_guest(c, f_bits) == 0);
        assert(G32(target + 0u) == 0u);          // bit 5 set then cleared by BTC
    }

    // 12d. Split bit-test & CMPXCHG8B + XCHG dword checks cleanly
    {
        uint32_t sp2 = base_esp - 16u;
        uint32_t target = scratch + 0xA60u;

        uint32_t f_bt = exec_page + 0x1C0u;
        // imul eax,eax,1; lock btr [ecx], 2; lock bts [ecx], edx; ret
        memcpy(MEM + f_bt, "\x6B\xC0\x01\xF0\x0F\xBA\x31\x02\xF0\x0F\xAB\x11\xC3", 13);
        P32(target + 0u, 4u);
        P32(target + 4u, 0u);
        P32(sp2, HOST_EXIT_THUNK);
        SETREG(c, R_ESP, sp2);
        SETREG(c, R_ECX, target);
        SETREG(c, R_EDX, 35u);
        SETREG(c, R_EAX, 1u);
        assert(host_run_guest(c, f_bt) == 0);
        assert(G32(target + 0u) == 0u);
        assert(G32(target + 4u) == 8u);
        assert((c->r[512] & 1u) == 0u);          // prior bit 35 was 0

        // 16-bit LOCK INC [ecx] wraps 0xFFFF -> 0x0000, sets ZF=1 and preserves CF=1; XCHG [ecx+4], edx
        uint32_t f_inc16 = exec_page + 0x1F0u;
        memcpy(MEM + f_inc16, "\x6B\xC0\x01\xF9\x66\xF0\xFF\x01\x87\x51\x04\xC3", 12);
        P32(target + 0u, 0xAABBFFFFu);
        P32(target + 4u, 0xCAFE1234u);
        P32(sp2, HOST_EXIT_THUNK);
        SETREG(c, R_ESP, sp2);
        SETREG(c, R_ECX, target);
        SETREG(c, R_EDX, 0x55667788u);
        SETREG(c, R_EAX, 1u);
        assert(host_run_guest(c, f_inc16) == 0);
        assert(G32(target + 0u) == 0xAABB0000u);
        assert((c->r[518] & 1u) == 1u);          // ZF=1
        assert((c->r[512] & 1u) == 1u);          // CF=1 preserved across INC16 & XCHG
        assert(G32(target + 4u) == 0x55667788u && REG(c, R_EDX) == 0xCAFE1234u);

        // LOCK CMPXCHG8B [ecx] match & miss (preserving CF=1!)
        uint32_t f_c8b = exec_page + 0x220u;
        memcpy(MEM + f_c8b, "\x6B\xC0\x01\xF9\xF0\x0F\xC7\x09\xC3", 9);
        P32(target + 0u, 0x11223344u);
        P32(target + 4u, 0x55667788u);
        P32(sp2, HOST_EXIT_THUNK);
        SETREG(c, R_ESP, sp2);
        SETREG(c, R_ECX, target);
        SETREG(c, R_EAX, 0x11223344u);
        SETREG(c, R_EDX, 0x55667788u);
        SETREG(c, 12u, 0xAAAABBBBu); // EBX
        // ECX holds pointer, wait -> don't overwrite ECX! Use target+8 or make ECX != mem value
        SETREG(c, R_ECX, target);
        assert(host_run_guest(c, f_c8b) == 0);
        assert((c->r[518] & 1u) == 1u); // ZF=1 on match
        assert((c->r[512] & 1u) == 1u); // CF=1 preserved
        assert(G32(target + 0u) == 0xAAAABBBBu && G32(target + 4u) == target);

        P32(sp2, HOST_EXIT_THUNK);
        SETREG(c, R_ESP, sp2);
        SETREG(c, R_ECX, target);
        SETREG(c, R_EAX, 0xDEAD0000u);
        SETREG(c, R_EDX, 0xBEEF0000u);
        SETREG(c, 12u, 0x99999999u);
        assert(host_run_guest(c, f_c8b) == 0);
        assert((c->r[518] & 1u) == 0u); // ZF=0 on mismatch
        assert((c->r[512] & 1u) == 1u); // CF=1 preserved
        assert(REG(c, R_EAX) == 0xAAAABBBBu && REG(c, R_EDX) == target);
        SETREG(c, R_ESP, base_esp);
    }

    printf("PASS: patch-aware deoptimization, range invalidation & boot hook suite\n");
    return 0;
}
"""


def main():
    unicorn = find_unicorn()
    with tempfile.TemporaryDirectory() as td:
        td_path = Path(td)
        jsonl, extra = make_synthetic_pcode(td_path)
        gen_dir = td_path / "gen"
        subprocess.run(
            [sys.executable, str(ROOT / "tools/translate/emit.py"), str(jsonl), str(gen_dir), "--extra", str(extra)],
            check=True,
        )
        table_txt = (gen_dir / "table.c").read_text()
        for sym in ("rt_fn_group", "rt_fn_patched", "rt_fn_ranges", "rt_fn_range_max_span", "rt_dyn_sites", "rt_dyn_site_count"):
            assert sym in table_txt, f"missing {sym} in generated table.c"

        harness = td_path / "harness.c"
        harness.write_text(HARNESS_C)

        bin_path = td_path / "patch_deopt_test"
        sources = [
            harness,
            gen_dir / "table.c",
            gen_dir / "stubs.c",
            *sorted(gen_dir.glob("tu_*.c")),
            RECOMP / "rt_dispatch.c",
            HOST / "host.c",
            HOST / "fallback.c",
            HOST / "pe_loader.c",
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
        subprocess.run([str(bin_path)], check=True)
        env_no_fast = os.environ.copy()
        env_no_fast["FNV_NO_FAST_LEAF"] = "1"
        subprocess.run([str(bin_path)], check=True, env=env_no_fast)


if __name__ == "__main__":
    main()

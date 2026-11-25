// x86 fallback interpreter (from thetheoryofR/Fallout-NV-MacOS, MIT).
// When dispatch finds no native function for a guest address, runs in Unicorn on the SAME guest memory.
// All Unicorn calls run under a single non-reentrant mutex: native calls (rt_call / rt_import_call)
// stop Unicorn via uc_emu_stop, drop the lock, execute natively, and resume Unicorn at the return address.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unicorn/unicorn.h>
#include "host.h"

extern int rt_has_fn(uint32_t a);
int rt_has_fallback(void) { return 1; }
#define MAXDEPTH 128
static _Thread_local int depth;
static pthread_mutex_t uc_open_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    uint32_t cur_ip;
    uint32_t root_ret;
    int done;
    int has_pending;
    int pending_is_import;
    uint32_t pending_target;
    uint32_t pending_ret;
    int pending_edge;
    uint32_t edge_pc;
    uint32_t edge_sz;
    int pending_atomic;
    int suppress_hooks;
    uint32_t atomic_ip;
    uint32_t atomic_end;
} FallbackRun;

#define ATOMIC_HOOK_CAP 2048u
#define ATOMIC_SET_CAP  4096u
#define ENTRY_SCAN_CAP  2048u

typedef struct {
    uc_engine *uc;
    FallbackRun *active_run;
    uc_hook text_hook;
    int text_hooked;
    uint32_t text_start, text_end;
    uint64_t epoch;
    uc_hook atomic_hooks[ATOMIC_HOOK_CAP];
    uint32_t n_atomic_hooks;
    uint32_t hooked_set[ATOMIC_SET_CAP];
    uint32_t scanned_entry[ENTRY_SCAN_CAP];
} ThreadUC;

static _Thread_local ThreadUC *tl_tuc;

static const int uc_atomic_regs[9] = {
    UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
    UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI,
    UC_X86_REG_EFLAGS,
};

// Ghidra register-file offsets
#define O_EFL_CF 512
#define O_ST0 4352
#define O_XMM0 4608
#define O_FPUCW 4256
#define O_C0 4240
static const int uc_w_regs[28] = {
    UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
    UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI,
    UC_X86_REG_EFLAGS, UC_X86_REG_FPSW, UC_X86_REG_FPCW, UC_X86_REG_FPTAG,
    UC_X86_REG_FP0, UC_X86_REG_FP1, UC_X86_REG_FP2, UC_X86_REG_FP3,
    UC_X86_REG_FP4, UC_X86_REG_FP5, UC_X86_REG_FP6, UC_X86_REG_FP7,
    UC_X86_REG_XMM0, UC_X86_REG_XMM1, UC_X86_REG_XMM2, UC_X86_REG_XMM3,
    UC_X86_REG_XMM4, UC_X86_REG_XMM5, UC_X86_REG_XMM6, UC_X86_REG_XMM7,
};
static const int uc_r_regs[26] = {
    UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
    UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI,
    UC_X86_REG_EFLAGS, UC_X86_REG_FPSW,
    UC_X86_REG_FP0, UC_X86_REG_FP1, UC_X86_REG_FP2, UC_X86_REG_FP3,
    UC_X86_REG_FP4, UC_X86_REG_FP5, UC_X86_REG_FP6, UC_X86_REG_FP7,
    UC_X86_REG_XMM0, UC_X86_REG_XMM1, UC_X86_REG_XMM2, UC_X86_REG_XMM3,
    UC_X86_REG_XMM4, UC_X86_REG_XMM5, UC_X86_REG_XMM6, UC_X86_REG_XMM7,
};
static const struct { int off, bit; } flag_bits[] = {{512, 0}, {514, 2}, {516, 4}, {518, 6}, {519, 7}, {522, 10}, {523, 11}};

static void cpu_to_uc(CPU *c, uc_engine *uc) {
    uint32_t g[12];
    memcpy(g, c->r, 32);
    uint32_t efl = 2;
    for (size_t i = 0; i < sizeof flag_bits / sizeof *flag_bits; i++) efl |= (uint32_t)(c->r[flag_bits[i].off] & 1) << flag_bits[i].bit;
    g[8] = efl;
    uint32_t top = c->r[REG_TOP] & 7;
    g[9] = (top << 11) | ((uint32_t)(c->r[O_C0] & 1) << 8) | ((uint32_t)(c->r[O_C0 + 1] & 1) << 9) |
           ((uint32_t)(c->r[O_C0 + 2] & 1) << 10) | ((uint32_t)(c->r[O_C0 + 3] & 1) << 14);
    g[10] = (uint32_t)LD(c->r + O_FPUCW, 2);
    g[11] = 0;

    uint8_t fp[8][16];
    for (int i = 0; i < 8; i++) {
        uint32_t slot = (top + (uint32_t)i) & 7;
        uint64_t bits;
        memcpy(&bits, c->r + O_ST0 + 16 * i, 8);
        memset(fp[slot], 0, 16);
        if (bits != 0) {
            double d;
            memcpy(&d, &bits, 8);
            f80_put(fp[slot], d);
        }
    }

    uint64_t xmm[8][2];
    for (int i = 0; i < 8; i++) memcpy(xmm[i], c->r + O_XMM0 + 64 * i, 16);

    void *ptrs[28];
    for (int i = 0; i < 12; i++) ptrs[i] = &g[i];
    for (int i = 0; i < 8; i++) ptrs[12 + i] = fp[i];
    for (int i = 0; i < 8; i++) ptrs[20 + i] = xmm[i];
    uc_reg_write_batch(uc, uc_w_regs, ptrs, 28);
}

static void uc_to_cpu(uc_engine *uc, CPU *c) {
    uint32_t g[10];
    uint8_t fp[8][16] = {{0}};
    uint64_t xmm[8][2];
    void *ptrs[26];
    for (int i = 0; i < 10; i++) ptrs[i] = &g[i];
    for (int i = 0; i < 8; i++) ptrs[10 + i] = fp[i];
    for (int i = 0; i < 8; i++) ptrs[18 + i] = xmm[i];
    uc_reg_read_batch(uc, uc_r_regs, ptrs, 26);

    memcpy(c->r, g, 32);
    uint32_t efl = g[8];
    for (size_t i = 0; i < sizeof flag_bits / sizeof *flag_bits; i++) c->r[flag_bits[i].off] = (efl >> flag_bits[i].bit) & 1;
    uint32_t sw = g[9];
    uint32_t top = (sw >> 11) & 7;
    c->r[REG_TOP] = (uint8_t)top;
    c->r[O_C0] = (sw >> 8) & 1; c->r[O_C0 + 1] = (sw >> 9) & 1; c->r[O_C0 + 2] = (sw >> 10) & 1; c->r[O_C0 + 3] = (sw >> 14) & 1;
    for (int i = 0; i < 8; i++) {
        uint32_t slot = (top + (uint32_t)i) & 7;
        uint64_t m; uint16_t se;
        memcpy(&m, fp[slot], 8);
        memcpy(&se, fp[slot] + 8, 2);
        if (m == 0 && se == 0) {
            uint64_t zero = 0;
            memcpy(c->r + O_ST0 + 16 * i, &zero, 8);
        } else {
            double d = f80_get(fp[slot]);
            memcpy(c->r + O_ST0 + 16 * i, &d, 8);
        }
    }
    for (int i = 0; i < 8; i++) memcpy(c->r + O_XMM0 + 64 * i, xmm[i], 16);
}

static inline int is_call_return_site(uint32_t ret, uint32_t root_ret) {
    if (ret == root_ret) return 1;
    if (ret < 0x10000u || !host_addr_is_executable(ret)) return 0;
    if (MEM[ret - 5] == 0xE8) return 1;
    static const uint8_t offs[5] = {2, 3, 4, 6, 7};
    for (int i = 0; i < 5; i++) {
        uint32_t p = ret - offs[i];
        if (MEM[p] == 0xFF && (MEM[p + 1] & 0x38u) == 0x10u) return 1;
    }
    return 0;
}

// Exact x86-32 instruction-length decoder for identifying LOCK prefixes and memory XCHG in QEMU blocks.
static inline int x86_skip_modrm(const uint8_t *p, uint32_t off, uint32_t max_len, int addr16,
                                 uint8_t *mod_out, uint8_t *reg_out) {
    if (off >= max_len) return -1;
    uint8_t b = p[off++];
    uint8_t mod = (b >> 6) & 3u, reg = (b >> 3) & 7u, rm = b & 7u;
    *mod_out = mod;
    *reg_out = reg;
    if (mod == 3u) return (int)off;
    if (addr16) {
        if (mod == 0u && rm == 6u) off += 2u;
        else if (mod == 1u) off += 1u;
        else if (mod == 2u) off += 2u;
    } else {
        if (rm == 4u) {
            if (off >= max_len) return -1;
            uint8_t base = p[off++] & 7u;
            if (mod == 0u && base == 5u) off += 4u;
        } else if (mod == 0u && rm == 5u) {
            off += 4u;
        }
        if (mod == 1u) off += 1u;
        else if (mod == 2u) off += 4u;
    }
    return (off <= max_len) ? (int)off : -1;
}

static int x86_decode_insn(const uint8_t *p, uint32_t max_len, int *out_is_atomic, int *out_is_ctrl) {
    uint32_t off = 0;
    int has_lock = 0, op16 = 0, addr16 = 0;
    while (off < max_len && off < 15u) {
        uint8_t b = p[off];
        if (b == 0xF0u) has_lock = 1;
        else if (b == 0x66u) op16 = 1;
        else if (b == 0x67u) addr16 = 1;
        else if (b != 0xF2u && b != 0xF3u && b != 0x26u && b != 0x2Eu &&
                 b != 0x36u && b != 0x3Eu && b != 0x64u && b != 0x65u) break;
        off++;
    }
    if (off >= max_len) return -1;
    uint8_t op = p[off++];
    uint32_t imm_w = op16 ? 2u : 4u;
    uint32_t addr_w = addr16 ? 2u : 4u;
    int has_modrm = 0, modrm_implicit_xchg = 0, is_ctrl = 0, modrm_ctrl_ff = 0;
    uint32_t imm = 0;

    if (op == 0x0Fu) {
        if (off >= max_len) return -1;
        uint8_t op2 = p[off++];
        if (op2 == 0x38u || op2 == 0x3Au) {
            if (off >= max_len) return -1;
            off++;
            has_modrm = 1;
            if (op2 == 0x3Au) imm = 1u;
        } else if (op2 >= 0x80u && op2 <= 0x8Fu) {
            imm = imm_w;
            is_ctrl = 1;
        } else if ((op2 >= 0x70u && op2 <= 0x73u) || op2 == 0xA4u || op2 == 0xACu ||
                   op2 == 0xBAu || op2 == 0xC2u || (op2 >= 0xC4u && op2 <= 0xC6u)) {
            has_modrm = 1;
            imm = 1u;
        } else if ((op2 >= 0x05u && op2 <= 0x0Bu) || (op2 >= 0x30u && op2 <= 0x35u) ||
                   op2 == 0x37u || op2 == 0x77u || (op2 >= 0xA0u && op2 <= 0xA2u) ||
                   (op2 >= 0xA8u && op2 <= 0xAAu) || (op2 >= 0xC8u && op2 <= 0xCFu)) {
            if (op2 == 0x05u || op2 == 0x07u || op2 == 0x0Bu || op2 == 0x34u || op2 == 0x35u) is_ctrl = 1;
        } else {
            has_modrm = 1;
        }
    } else {
        switch (op) {
        case 0x00 ... 0x03: case 0x08 ... 0x0B: case 0x10 ... 0x13: case 0x18 ... 0x1B:
        case 0x20 ... 0x23: case 0x28 ... 0x2B: case 0x30 ... 0x33: case 0x38 ... 0x3B:
        case 0x62: case 0x63: case 0x84: case 0x85: case 0x88 ... 0x8F:
        case 0xD0 ... 0xD3: case 0xD8 ... 0xDF: case 0xFE:
            has_modrm = 1; break;
        case 0xFF:
            has_modrm = 1; modrm_ctrl_ff = 1; break;
        case 0x86: case 0x87:
            has_modrm = 1; modrm_implicit_xchg = 1; break;
        case 0x69: case 0x81: case 0xC7:
            has_modrm = 1; imm = imm_w; break;
        case 0x6B: case 0x80: case 0x82: case 0x83: case 0xC0: case 0xC1: case 0xC6:
            has_modrm = 1; imm = 1u; break;
        case 0xF6: case 0xF7: {
            uint8_t mod = 0, reg = 0;
            int noff = x86_skip_modrm(p, off, max_len, addr16, &mod, &reg);
            if (noff < 0) return -1;
            off = (uint32_t)noff;
            if (reg <= 1u) off += (op == 0xF7u) ? imm_w : 1u;
            *out_is_atomic = has_lock;
            *out_is_ctrl = 0;
            return (off <= max_len) ? (int)off : -1;
        }
        case 0x70 ... 0x7F: case 0xE0 ... 0xE3: case 0xEB: case 0xCD:
            imm = 1u; is_ctrl = 1; break;
        case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C:
        case 0x6A: case 0xA8: case 0xB0 ... 0xB7: case 0xD4: case 0xD5: case 0xE4 ... 0xE7:
            imm = 1u; break;
        case 0xE8: case 0xE9:
            imm = imm_w; is_ctrl = 1; break;
        case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D:
        case 0x68: case 0xA9: case 0xB8 ... 0xBF:
            imm = imm_w; break;
        case 0xA0 ... 0xA3:
            imm = addr_w; break;
        case 0xC2: case 0xCA:
            imm = 2u; is_ctrl = 1; break;
        case 0xC8:
            imm = 3u; break;
        case 0x9A: case 0xEA:
            imm = imm_w + 2u; is_ctrl = 1; break;
        case 0xC3: case 0xCB: case 0xCC: case 0xCE: case 0xCF: case 0xF4:
            is_ctrl = 1; break;
        default:
            break;
        }
    }

    uint8_t mod = 3u, reg = 0u;
    if (has_modrm) {
        int noff = x86_skip_modrm(p, off, max_len, addr16, &mod, &reg);
        if (noff < 0) return -1;
        off = (uint32_t)noff;
        if (modrm_ctrl_ff && reg >= 2u && reg <= 5u) is_ctrl = 1;
    }
    off += imm;
    if (off > max_len) return -1;
    *out_is_atomic = has_lock || (modrm_implicit_xchg && mod != 3u);
    *out_is_ctrl = is_ctrl;
    return (int)off;
}

static void on_atomic_code(uc_engine *uc, uint64_t address, uint32_t size, void *user) {
    ThreadUC *tu = (ThreadUC *)user;
    FallbackRun *r = tu->active_run;
    if (!r || r->done || r->has_pending || r->pending_edge || r->pending_atomic || r->suppress_hooks) return;
    uint32_t a = (uint32_t)address;
    if (a == r->root_ret) {
        r->done = 1;
        uc_emu_stop(uc);
        return;
    }
    r->pending_atomic = 1;
    r->atomic_ip = a;
    r->atomic_end = a + size;
    uc_emu_stop(uc);
}

static int tuc_hook_atomic_ip(ThreadUC *tu, uint32_t aip) {
    uint32_t h = (aip ^ (aip >> 10) ^ (aip >> 19)) & (ATOMIC_SET_CAP - 1u);
    while (tu->hooked_set[h] != 0u) {
        if (tu->hooked_set[h] == aip) return 0;
        h = (h + 1u) & (ATOMIC_SET_CAP - 1u);
    }
    if (tu->n_atomic_hooks >= ATOMIC_HOOK_CAP) return 0;
    tu->hooked_set[h] = aip;
    uc_hook_add(tu->uc, &tu->atomic_hooks[tu->n_atomic_hooks++], UC_HOOK_CODE,
                (void *)on_atomic_code, tu, (uint64_t)aip, (uint64_t)aip);
    return 1;
}

static int tuc_scan_tb_atomics(ThreadUC *tu, uint32_t a, uint32_t size) {
    const uint8_t *p = MEM + a;
    int maybe = 0;
    for (uint32_t i = 0; i < size; i++) {
        uint8_t b = p[i];
        if (b == 0xF0u || b == 0x86u || b == 0x87u) { maybe = 1; break; }
    }
    if (!maybe) return 0;
    int added = 0;
    uint32_t off = 0;
    while (off < size) {
        int is_atomic = 0, is_ctrl = 0;
        int len = x86_decode_insn(p + off, size - off, &is_atomic, &is_ctrl);
        if (len <= 0) break;
        if (is_atomic && tuc_hook_atomic_ip(tu, a + off)) added = 1;
        off += (uint32_t)len;
    }
    return added;
}

static void tuc_ensure_entry_atomics(ThreadUC *tu, uint32_t start_ip) {
    if (start_ip < 0x10000u || start_ip >= HOST_EXIT_THUNK) return;
    uint32_t h = (start_ip ^ (start_ip >> 10) ^ (start_ip >> 19)) & (ENTRY_SCAN_CAP - 1u);
    if (__builtin_expect(tu->scanned_entry[h] == start_ip, 1)) return;
    tu->scanned_entry[h] = start_ip;
    uint32_t off = 0, added = 0;
    uint32_t checked_page = 0xFFFFFFFFu;
    while (off < 256u) {
        uint32_t ip = start_ip + off;
        if ((ip & ~0xFFFu) != checked_page) {
            if (!host_addr_is_executable(ip)) break;
            checked_page = ip & ~0xFFFu;
        }
        uint32_t max_len = 256u - off;
        uint32_t page_rem = 0x1000u - (ip & 0xFFFu);
        if (page_rem < max_len) max_len = page_rem;
        if (max_len == 0u) break;
        int is_atomic = 0, is_ctrl = 0;
        int len = x86_decode_insn(MEM + ip, max_len, &is_atomic, &is_ctrl);
        if (len <= 0) break;
        if (is_atomic && tuc_hook_atomic_ip(tu, ip)) added = 1;
        off += (uint32_t)len;
        if (is_ctrl) break;
    }
    if (added && off > 0u) uc_ctl_remove_cache(tu->uc, start_ip, (uint64_t)start_ip + off);
}

// Addresses on_block sees (import thunks, .text); elsewhere uc_emu_start's `until` stops at root_ret.
static inline int addr_is_hooked(uint32_t addr) {
    return (addr >= HOST_EXIT_THUNK && addr < IMPORT_THUNK_BASE + 0x10000u) ||
           (host_text_end > host_text_start && addr >= host_text_start && addr < host_text_end);
}

static void on_block(uc_engine *uc, uint64_t address, uint32_t size, void *user) {
    (void)size;
    ThreadUC *tu = (ThreadUC *)user;
    FallbackRun *r = tu->active_run;
    if (!r || r->done || r->has_pending || r->pending_edge || r->pending_atomic) return;
    uint32_t a = (uint32_t)address;
    if (a == r->root_ret) {
        r->done = 1;
        uc_emu_stop(uc);
        return;
    }
    if (r->suppress_hooks) return;
    int is_import = a >= IMPORT_THUNK_BASE && a < IMPORT_THUNK_BASE + 0x10000u;
    if (!is_import && (a == r->cur_ip || !rt_has_fn(a))) return;
    uint32_t esp; uc_reg_read(uc, UC_X86_REG_ESP, &esp);
    uint32_t ret = G32(esp);
    if (!is_import && !is_call_return_site(ret, r->root_ret)) return;
    r->has_pending = 1;
    r->pending_is_import = is_import;
    r->pending_target = a;
    r->pending_ret = ret;
    uc_emu_stop(uc);
}

static void on_edge(uc_engine *uc, uc_tb *cur_tb, uc_tb *prev_tb, void *user) {
    (void)prev_tb;
    ThreadUC *tu = (ThreadUC *)user;
    FallbackRun *r = tu->active_run;
    if (!r || r->done || r->has_pending || r->pending_edge || r->pending_atomic || r->suppress_hooks) return;
    uint32_t a = (uint32_t)cur_tb->pc;
    uint32_t sz = cur_tb->size;
    if (!sz || a == r->root_ret || a >= HOST_EXIT_THUNK) return;
    if (host_text_end > host_text_start && a >= host_text_start && a < host_text_end &&
        a != r->cur_ip && rt_has_fn(a)) return;
    if (tuc_scan_tb_atomics(tu, a, sz)) {
        r->pending_edge = 1;
        r->edge_pc = a;
        r->edge_sz = sz;
        uc_emu_stop(uc);
    }
}

static ThreadUC *engine(CPU *c) {
    if (tl_tuc) return tl_tuc;
    uc_engine *uc;
    pthread_mutex_lock(&uc_open_lock);
    if (uc_open(UC_ARCH_X86, UC_MODE_32, &uc) != UC_ERR_OK) {
        pthread_mutex_unlock(&uc_open_lock);
        fprintf(stderr, "[fallback] uc_open failed\n");
        abort();
    }
    uc_err e = uc_mem_map_ptr(uc, 0, (size_t)1 << 32, UC_PROT_ALL, MEM);
    pthread_mutex_unlock(&uc_open_lock);
    if (e != UC_ERR_OK) { fprintf(stderr, "[fallback] map: %s\n", uc_strerror(e)); abort(); }
    uint32_t teb = REG(c, 272), gdt = teb + 0x1F00;
    uint8_t table[32] = {0};
    uint8_t code[8] = {0xFF, 0xFF, 0, 0, 0, 0x9B, 0xCF, 0}, data[8] = {0xFF, 0xFF, 0, 0, 0, 0x93, 0xCF, 0};
    uint8_t fs[8] = {0xFF, 0x0F, (uint8_t)teb, (uint8_t)(teb >> 8), (uint8_t)(teb >> 16), 0x93, 0x40, (uint8_t)(teb >> 24)};
    memcpy(table + 8, code, 8); memcpy(table + 16, data, 8); memcpy(table + 24, fs, 8);
    memcpy(MEM + gdt, table, 32);
    uc_x86_mmr g = {0, gdt, 31, 0};
    uc_reg_write(uc, UC_X86_REG_GDTR, &g);
    uint32_t sel = 1 << 3; uc_reg_write(uc, UC_X86_REG_CS, &sel);
    sel = 2 << 3; uc_reg_write(uc, UC_X86_REG_SS, &sel); uc_reg_write(uc, UC_X86_REG_DS, &sel); uc_reg_write(uc, UC_X86_REG_ES, &sel);
    sel = 3 << 3; uc_reg_write(uc, UC_X86_REG_FS, &sel);
    ThreadUC *tu = calloc(1, sizeof(*tu));
    if (!tu) abort();
    tu->uc = uc;
    uc_hook h;
    uc_hook_add(uc, &h, UC_HOOK_BLOCK, (void *)on_block, tu,
                HOST_EXIT_THUNK, IMPORT_THUNK_BASE + 0xFFFFu);
    uc_hook eh;
    uc_hook_add(uc, &eh, UC_HOOK_EDGE_GENERATED, (void *)on_edge, tu,
                0x00010000ull, (uint64_t)HOST_EXIT_THUNK - 1ull);
    tl_tuc = tu;
    return tu;
}

static int ensure_text_hook(ThreadUC *tu) {
    if (__builtin_expect(tu->text_hooked && tu->text_start == host_text_start && tu->text_end == host_text_end, 1)) {
        return 0;
    }
    int replaced = 0;
    if (tu->text_hooked) {
        uc_hook_del(tu->uc, tu->text_hook);
        tu->text_hooked = 0;
        replaced = 1;
    }
    if (host_text_end > host_text_start) {
        uc_hook_add(tu->uc, &tu->text_hook, UC_HOOK_BLOCK, (void *)on_block, tu,
                    host_text_start, host_text_end - 1);
        tu->text_hooked = 1;
    }
    tu->text_start = host_text_start;
    tu->text_end = host_text_end;
    return replaced;
}

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static void log_root(uint32_t a) {
    static int log_on = -1;
    if (__builtin_expect(log_on < 0, 0)) {
        const char *p = getenv("FNV_FALLBACK_LOG");
        log_on = (p && p[0]) || getenv("FNV_TRACE") != NULL;
    }
    if (__builtin_expect(!log_on, 1)) return;
    static uint32_t fast_seen[4096];
    uint32_t h = (a ^ (a >> 12)) & 4095u;
    if (__builtin_expect(a != 0 && __atomic_load_n(&fast_seen[h], __ATOMIC_RELAXED) == a, 1)) return;
    static uint32_t seen[65536]; static int n;
    pthread_mutex_lock(&log_lock);
    for (int i = 0; i < n; i++) {
        if (seen[i] == a) {
            __atomic_store_n(&fast_seen[h], a, __ATOMIC_RELAXED);
            pthread_mutex_unlock(&log_lock);
            return;
        }
    }
    if (n < 65536) seen[n++] = a;
    if (a != 0) __atomic_store_n(&fast_seen[h], a, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&log_lock);
    const char *path = getenv("FNV_FALLBACK_LOG");
    if (path && path[0]) {
        FILE *f = fopen(path, "a");
        if (f) { fprintf(f, "%#x\n", a); fclose(f); }
    }
    if (getenv("FNV_TRACE")) fprintf(stderr, "[fallback] interpreting %#x\n", a);
}

static uint64_t fast_leaf_hits_total;
uint64_t rt_fast_leaf_hits(void) {
    return __atomic_load_n(&fast_leaf_hits_total, __ATOMIC_RELAXED);
}

#define FAST_LEAF_MAX_STEPS 48
#define FAST_LEAF_MAX_WRITES 8

typedef struct {
    uint32_t addr;
    uint64_t val;
    uint8_t size;
} FastWrite;

typedef struct {
    uint8_t mod, reg, rm, is_mem;
    uint32_t ea;
} FastModRM;

typedef struct {
    uint8_t cf, pf, af, zf, sf, of;
    int loaded, dirty;
} FastFlags;

static inline int fast_addr_ok(uint32_t addr, uint32_t len) {
    return len > 0u && len <= 16u && addr >= 0x10000u && addr <= 0xEFFFFFF0u && (addr + len) >= addr;
}

static inline int fast_ip_ok(uint32_t ip, uint32_t *checked_page) {
    if (ip < 0x10000u || ip > 0xEFFFFF00u) return 0;
    uint32_t pg = ip & ~0xFFFu;
    if (__builtin_expect(pg != *checked_page, 0)) {
        if (!host_addr_is_executable(ip)) return 0;
        *checked_page = pg;
    }
    return 1;
}

static inline int fast_read(uint32_t addr, uint8_t sz, const FastWrite *wbuf, int nw, uint64_t *out) {
    if (!fast_addr_ok(addr, sz)) return 0;
    for (int i = nw - 1; i >= 0; i--) {
        if (wbuf[i].addr == addr && wbuf[i].size == sz) {
            *out = wbuf[i].val;
            return 1;
        }
        if (addr < wbuf[i].addr + wbuf[i].size && wbuf[i].addr < addr + sz) return 0;
    }
    if (sz != 1u && sz != 2u && sz != 4u && sz != 8u) return 0;
    *out = (uint64_t)LDM(addr, sz);
    return 1;
}

static inline int fast_write(uint32_t addr, uint64_t val, uint8_t sz, FastWrite *wbuf, int *nw) {
    if (!fast_addr_ok(addr, sz) || *nw >= FAST_LEAF_MAX_WRITES) return 0;
    if (sz != 1u && sz != 2u && sz != 4u && sz != 8u) return 0;
    wbuf[*nw].addr = addr;
    wbuf[*nw].val = val;
    wbuf[*nw].size = sz;
    (*nw)++;
    return 1;
}

static inline uint8_t fast_get_r8(const uint32_t g[8], uint8_t r) {
    return (r < 4u) ? (uint8_t)g[r] : (uint8_t)(g[r - 4u] >> 8);
}

static inline void fast_set_r8(uint32_t g[8], uint8_t r, uint8_t v) {
    if (r < 4u) g[r] = (g[r] & ~0xFFu) | (uint32_t)v;
    else g[r - 4u] = (g[r - 4u] & ~0xFF00u) | ((uint32_t)v << 8);
}

static inline int fast_decode_modrm(uint32_t *ip, uint32_t *checked_page, const uint32_t g[8], FastModRM *m) {
    if (!fast_ip_ok(*ip, checked_page)) return 0;
    uint8_t b = MEM[(*ip)++];
    m->mod = (b >> 6) & 3u;
    m->reg = (b >> 3) & 7u;
    m->rm = b & 7u;
    if (m->mod == 3u) {
        m->is_mem = 0;
        m->ea = 0;
        return 1;
    }
    m->is_mem = 1;
    if (m->mod == 0u && m->rm == 5u) {
        if (!fast_ip_ok(*ip + 3u, checked_page)) return 0;
        m->ea = (uint32_t)LDV(MEM + *ip, 4);
        *ip += 4u;
        return 1;
    }
    uint32_t ea;
    if (m->rm == 4u) {
        if (!fast_ip_ok(*ip, checked_page)) return 0;
        uint8_t sib = MEM[(*ip)++];
        uint8_t scale = (sib >> 6) & 3u, idx = (sib >> 3) & 7u, base = sib & 7u;
        if (base == 5u && m->mod == 0u) {
            if (!fast_ip_ok(*ip + 3u, checked_page)) return 0;
            ea = (uint32_t)LDV(MEM + *ip, 4);
            *ip += 4u;
        } else {
            ea = g[base];
        }
        if (idx != 4u) ea += g[idx] << scale;
    } else {
        ea = g[m->rm];
    }
    if (m->mod == 1u) {
        if (!fast_ip_ok(*ip, checked_page)) return 0;
        ea += (uint32_t)(int32_t)(int8_t)MEM[(*ip)++];
    } else if (m->mod == 2u) {
        if (!fast_ip_ok(*ip + 3u, checked_page)) return 0;
        ea += (uint32_t)LDV(MEM + *ip, 4);
        *ip += 4u;
    }
    m->ea = ea;
    return 1;
}

static inline int fast_read_rm(const FastModRM *m, uint8_t w, const uint32_t g[8], const FastWrite *wbuf, int nw, uint32_t *out) {
    if (!m->is_mem) {
        if (w == 1u) *out = fast_get_r8(g, m->rm);
        else if (w == 2u) *out = g[m->rm] & 0xFFFFu;
        else *out = g[m->rm];
        return 1;
    }
    uint64_t v = 0;
    if (!fast_read(m->ea, w, wbuf, nw, &v)) return 0;
    *out = (uint32_t)v;
    return 1;
}

static inline int fast_write_rm(const FastModRM *m, uint8_t w, uint32_t val, uint32_t g[8], FastWrite *wbuf, int *nw) {
    if (!m->is_mem) {
        if (w == 1u) fast_set_r8(g, m->rm, (uint8_t)val);
        else if (w == 2u) g[m->rm] = (g[m->rm] & ~0xFFFFu) | (val & 0xFFFFu);
        else g[m->rm] = val;
        return 1;
    }
    uint64_t mask = (w == 1u) ? 0xFFu : (w == 2u) ? 0xFFFFu : 0xFFFFFFFFu;
    return fast_write(m->ea, (uint64_t)val & mask, w, wbuf, nw);
}

static inline void fast_load_flags(FastFlags *f, const CPU *c) {
    if (__builtin_expect(!f->loaded, 0)) {
        f->cf = c->r[512] & 1u;
        f->pf = c->r[514] & 1u;
        f->af = c->r[516] & 1u;
        f->zf = c->r[518] & 1u;
        f->sf = c->r[519] & 1u;
        f->of = c->r[523] & 1u;
        f->loaded = 1;
    }
}

static inline int fast_eval_cc(uint8_t cc, const FastFlags *f) {
    switch (cc & 15u) {
    case 0x0: return f->of;
    case 0x1: return !f->of;
    case 0x2: return f->cf;
    case 0x3: return !f->cf;
    case 0x4: return f->zf;
    case 0x5: return !f->zf;
    case 0x6: return f->cf || f->zf;
    case 0x7: return !f->cf && !f->zf;
    case 0x8: return f->sf;
    case 0x9: return !f->sf;
    case 0xA: return f->pf;
    case 0xB: return !f->pf;
    case 0xC: return f->sf != f->of;
    case 0xD: return f->sf == f->of;
    case 0xE: return f->zf || (f->sf != f->of);
    default:  return !f->zf && (f->sf == f->of);
    }
}

static inline uint32_t fast_alu_cf(uint8_t kind, uint32_t a, uint32_t b, uint8_t w, uint8_t cf_in, FastFlags *f) {
    uint64_t mask = (w == 1u) ? 0xFFu : (w == 2u) ? 0xFFFFu : 0xFFFFFFFFu;
    uint64_t sign = (w == 1u) ? 0x80u : (w == 2u) ? 0x8000u : 0x80000000u;
    uint64_t ua = (uint64_t)a & mask, ub = (uint64_t)b & mask, res;
    if (kind == 0u || kind == 2u) {
        uint64_t c = (kind == 2u) ? (uint64_t)(cf_in & 1u) : 0u;
        uint64_t sum = ua + ub + c;
        res = sum & mask;
        f->cf = (sum > mask) ? 1u : 0u;
        f->of = (((~(ua ^ ub) & (ua ^ res)) & sign) != 0) ? 1u : 0u;
        f->af = (((ua ^ ub ^ res) & 0x10u) != 0) ? 1u : 0u;
    } else if (kind == 3u || kind == 5u || kind == 7u) {
        uint64_t c = (kind == 3u) ? (uint64_t)(cf_in & 1u) : 0u;
        res = (ua - ub - c) & mask;
        f->cf = (ua < (ub + c)) ? 1u : 0u;
        f->of = ((((ua ^ ub) & (ua ^ res)) & sign) != 0) ? 1u : 0u;
        f->af = (((ua ^ ub ^ res) & 0x10u) != 0) ? 1u : 0u;
    } else {
        res = (kind == 1u) ? (ua | ub) : (kind == 4u) ? (ua & ub) : (ua ^ ub);
        f->cf = 0u;
        f->of = 0u;
        f->af = 0u;
    }
    f->zf = (res == 0u) ? 1u : 0u;
    f->sf = ((res & sign) != 0u) ? 1u : 0u;
    f->pf = (uint8_t)(__builtin_parity((unsigned)(res & 0xFFu)) ^ 1);
    f->loaded = 1;
    f->dirty = 1;
    return (uint32_t)res;
}

static inline uint32_t fast_alu(uint8_t kind, uint32_t a, uint32_t b, uint8_t w, FastFlags *f) {
    return fast_alu_cf(kind, a, b, w, 0u, f);
}

static inline uint32_t fast_inc_dec(uint32_t val, int is_dec, FastFlags *f, const CPU *c) {
    fast_load_flags(f, c);
    uint8_t saved_cf = f->cf;
    uint32_t res = fast_alu(is_dec ? 5u : 0u, val, 1u, 4u, f);
    f->cf = saved_cf;
    return res;
}

static inline uint32_t fast_shift32(uint8_t kind, uint32_t val, uint32_t cnt, FastFlags *f, const CPU *c) {
    cnt &= 31u;
    if (cnt == 0u) return val;
    fast_load_flags(f, c);
    uint32_t res;
    if (kind == 4u) {
        f->cf = (uint8_t)((val >> (32u - cnt)) & 1u);
        res = val << cnt;
        f->of = (cnt == 1u) ? (uint8_t)(((res >> 31) ^ f->cf) & 1u) : 0u;
    } else if (kind == 5u) {
        f->cf = (uint8_t)((val >> (cnt - 1u)) & 1u);
        res = val >> cnt;
        f->of = (cnt == 1u) ? (uint8_t)((val >> 31) & 1u) : 0u;
    } else {
        f->cf = (uint8_t)(((uint32_t)((int32_t)val >> (cnt - 1u))) & 1u);
        res = (uint32_t)((int32_t)val >> cnt);
        f->of = 0u;
    }
    f->af = 0u;
    f->zf = (res == 0u) ? 1u : 0u;
    f->sf = (uint8_t)((res >> 31) & 1u);
    f->pf = (uint8_t)(__builtin_parity((unsigned)(res & 0xFFu)) ^ 1);
    f->dirty = 1;
    return res;
}

static inline void fast_ensure_xmm(int r, uint64_t xmm[8][2], uint8_t *xmm_loaded, const CPU *c) {
    if (!(*xmm_loaded & (1u << r))) {
        memcpy(xmm[r], c->r + O_XMM0 + 64 * r, 16);
        *xmm_loaded |= (uint8_t)(1u << r);
    }
}

static inline void fast_commit_state(CPU *c, const uint32_t g[8], const FastFlags *flags,
                                     const uint64_t xmm[8][2], uint8_t xmm_dirty,
                                     const FastWrite *wbuf, int nw) {
    for (int i = 0; i < nw; i++) STM(wbuf[i].addr, (V)wbuf[i].val, wbuf[i].size);
    memcpy(c->r, g, 32);
    if (flags->dirty) {
        c->r[512] = flags->cf;
        c->r[514] = flags->pf;
        c->r[516] = flags->af;
        c->r[518] = flags->zf;
        c->r[519] = flags->sf;
        c->r[523] = flags->of;
    }
    if (xmm_dirty) {
        for (int r = 0; r < 8; r++) {
            if (xmm_dirty & (1u << r)) memcpy(c->r + O_XMM0 + 64 * r, xmm[r], 16);
        }
    }
}

enum {
    ATOMIC_OP_ADD = 0, ATOMIC_OP_OR = 1, ATOMIC_OP_ADC = 2, ATOMIC_OP_SBB = 3,
    ATOMIC_OP_AND = 4, ATOMIC_OP_SUB = 5, ATOMIC_OP_XOR = 6, ATOMIC_OP_XCHG = 8,
    ATOMIC_OP_NEG = 9
};

static inline uint32_t atomic_width_mask(uint8_t w) {
    return (w == 1u) ? 0xFFu : (w == 2u) ? 0xFFFFu : 0xFFFFFFFFu;
}

static inline uint32_t atomic_eval_rmw(uint8_t kind, uint32_t old, uint32_t rhs, uint8_t cf_in) {
    switch (kind) {
    case ATOMIC_OP_ADD:  return old + rhs;
    case ATOMIC_OP_OR:   return old | rhs;
    case ATOMIC_OP_ADC:  return old + rhs + (uint32_t)(cf_in & 1u);
    case ATOMIC_OP_SBB:  return old - rhs - (uint32_t)(cf_in & 1u);
    case ATOMIC_OP_AND:  return old & rhs;
    case ATOMIC_OP_SUB:  return old - rhs;
    case ATOMIC_OP_XOR:  return old ^ rhs;
    case ATOMIC_OP_NEG:  return 0u - old;
    default:             return rhs;
    }
}

#define ATOMIC_ALIGNED_RMW(T, ptr, op, v) ( \
    (op) == ATOMIC_OP_ADD ? (uint32_t)__atomic_fetch_add((T *)(ptr), (T)(v), __ATOMIC_SEQ_CST) : \
    (op) == ATOMIC_OP_SUB ? (uint32_t)__atomic_fetch_sub((T *)(ptr), (T)(v), __ATOMIC_SEQ_CST) : \
    (op) == ATOMIC_OP_OR  ? (uint32_t)__atomic_fetch_or((T *)(ptr), (T)(v), __ATOMIC_SEQ_CST) : \
    (op) == ATOMIC_OP_AND ? (uint32_t)__atomic_fetch_and((T *)(ptr), (T)(v), __ATOMIC_SEQ_CST) : \
    (op) == ATOMIC_OP_XOR ? (uint32_t)__atomic_fetch_xor((T *)(ptr), (T)(v), __ATOMIC_SEQ_CST) : \
                            (uint32_t)__atomic_exchange_n((T *)(ptr), (T)(v), __ATOMIC_SEQ_CST))

#define ATOMIC_ALIGNED_CAS(T, ptr, op, rhs, cf_in) ({ \
    T _cur = __atomic_load_n((T *)(ptr), __ATOMIC_SEQ_CST), _nv; \
    do { _nv = (T)atomic_eval_rmw((op), (uint32_t)_cur, (rhs), (cf_in)); } \
    while (!__atomic_compare_exchange_n((T *)(ptr), &_cur, _nv, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)); \
    (uint32_t)_cur; \
})

#define ATOMIC_ALIGNED_CMPXCHG(T, ptr, exp_io, des) ({ \
    T _e = (T)(*(exp_io)); \
    int _ok = __atomic_compare_exchange_n((T *)(ptr), &_e, (T)(des), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
    *(exp_io) = (uint32_t)_e; \
    _ok; \
})

static uint32_t atomic_rmw_w(uint32_t ea, uint8_t w, uint8_t kind, uint32_t rhs, uint8_t cf_in) {
    uint32_t mask = atomic_width_mask(w);
    uint32_t old = 0;
    rt_lock();
    if (!(ea & (uint32_t)(w - 1u))) {
        void *ptr = MEM + ea;
        if (kind == ATOMIC_OP_ADC || kind == ATOMIC_OP_SBB || kind == ATOMIC_OP_NEG) {
            if (w == 1u) old = ATOMIC_ALIGNED_CAS(uint8_t, ptr, kind, rhs, cf_in);
            else if (w == 2u) old = ATOMIC_ALIGNED_CAS(uint16_t, ptr, kind, rhs, cf_in);
            else old = ATOMIC_ALIGNED_CAS(uint32_t, ptr, kind, rhs, cf_in);
        } else {
            if (w == 1u) old = ATOMIC_ALIGNED_RMW(uint8_t, ptr, kind, rhs);
            else if (w == 2u) old = ATOMIC_ALIGNED_RMW(uint16_t, ptr, kind, rhs);
            else old = ATOMIC_ALIGNED_RMW(uint32_t, ptr, kind, rhs);
        }
    } else {
        memcpy(&old, MEM + ea, w);
        uint32_t nv = atomic_eval_rmw(kind, old, rhs, cf_in) & mask;
        memcpy(MEM + ea, &nv, w);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }
    rt_unlock();
    return old & mask;
}

static int atomic_cmpxchg_w(uint32_t ea, uint8_t w, uint32_t *exp_io, uint32_t des) {
    uint32_t mask = atomic_width_mask(w);
    uint32_t exp = *exp_io & mask;
    int ok;
    rt_lock();
    if (!(ea & (uint32_t)(w - 1u))) {
        void *ptr = MEM + ea;
        if (w == 1u) ok = ATOMIC_ALIGNED_CMPXCHG(uint8_t, ptr, &exp, des);
        else if (w == 2u) ok = ATOMIC_ALIGNED_CMPXCHG(uint16_t, ptr, &exp, des);
        else ok = ATOMIC_ALIGNED_CMPXCHG(uint32_t, ptr, &exp, des);
    } else {
        uint32_t cur = 0;
        memcpy(&cur, MEM + ea, w);
        cur &= mask;
        if (cur == exp) {
            uint32_t dv = des & mask;
            memcpy(MEM + ea, &dv, w);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            ok = 1;
        } else {
            exp = cur;
            ok = 0;
        }
    }
    rt_unlock();
    *exp_io = exp & mask;
    return ok;
}

static int atomic_cmpxchg64(uint32_t ea, uint64_t *exp_io, uint64_t des) {
    int ok;
    rt_lock();
    if (!(ea & 7u)) {
        ok = __atomic_compare_exchange_n((uint64_t *)(void *)(MEM + ea), exp_io, des, 0,
                                         __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    } else {
        uint64_t cur = 0;
        memcpy(&cur, MEM + ea, 8);
        if (cur == *exp_io) {
            memcpy(MEM + ea, &des, 8);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            ok = 1;
        } else {
            *exp_io = cur;
            ok = 0;
        }
    }
    rt_unlock();
    return ok;
}

static inline void efl_apply_arith(uint32_t *efl, const FastFlags *f, int keep_cf) {
    uint32_t val = ((uint32_t)(f->pf & 1u) << 2) | ((uint32_t)(f->af & 1u) << 4) |
                   ((uint32_t)(f->zf & 1u) << 6) | ((uint32_t)(f->sf & 1u) << 7) |
                   ((uint32_t)(f->of & 1u) << 11);
    if (keep_cf) {
        *efl = (*efl & ~0x8D4u) | val;
    } else {
        *efl = (*efl & ~0x8D5u) | val | (uint32_t)(f->cf & 1u);
    }
}

static inline uint32_t exec_read_reg_w(const uint32_t g[8], uint8_t reg, uint8_t w) {
    if (w == 1u) return fast_get_r8(g, reg);
    if (w == 2u) return g[reg] & 0xFFFFu;
    return g[reg];
}

static inline void exec_write_reg_w(uint32_t g[8], uint8_t reg, uint8_t w, uint32_t val) {
    if (w == 1u) fast_set_r8(g, reg, (uint8_t)val);
    else if (w == 2u) g[reg] = (g[reg] & ~0xFFFFu) | (val & 0xFFFFu);
    else g[reg] = val;
}

// Execute a single LOCK-prefixed instruction or memory XCHG atomically in host C.
// Returns 1 if handled (updating g, *efl_io, *next_ip_out), 0 to fall back to serialized Unicorn.
static int exec_atomic_insn(uint32_t g[8], uint32_t *efl_io, uint32_t teb_base,
                            uint32_t ip, uint32_t *next_ip_out) {
    uint32_t checked_page = 0xFFFFFFFFu;
    int has_lock = 0, op16 = 0;
    uint32_t seg_base = 0;
    for (int p = 0; p < 15; p++) {
        if (!fast_ip_ok(ip, &checked_page)) return 0;
        uint8_t b = MEM[ip];
        if (b == 0xF0u) has_lock = 1;
        else if (b == 0x66u) op16 = 1;
        else if (b == 0x64u) seg_base = teb_base;
        else if (b == 0x26u || b == 0x2Eu || b == 0x36u || b == 0x3Eu) seg_base = 0u;
        else if (b == 0x67u || b == 0x65u || b == 0xF2u || b == 0xF3u) return 0;
        else break;
        ip++;
    }
    if (!fast_ip_ok(ip, &checked_page)) return 0;
    uint8_t op = MEM[ip++];
    uint8_t w_word = op16 ? 2u : 4u;
    uint8_t cf_in = (uint8_t)(*efl_io & 1u);
    FastFlags f = {0, 0, 0, 0, 0, 0, 0, 0};

    if (op == 0x86u || op == 0x87u) { // XCHG r/m, r (always atomic on memory)
        uint8_t w = (op == 0x86u) ? 1u : w_word;
        FastModRM m;
        if (!fast_decode_modrm(&ip, &checked_page, g, &m) || !m.is_mem) return 0;
        uint32_t ea = m.ea + seg_base;
        if (!fast_addr_ok(ea, w)) return 0;
        uint32_t rhs = exec_read_reg_w(g, m.reg, w);
        uint32_t old = atomic_rmw_w(ea, w, ATOMIC_OP_XCHG, rhs, 0u);
        exec_write_reg_w(g, m.reg, w, old);
        *next_ip_out = ip;
        return 1;
    }
    if (!has_lock) return 0;

    if (op == 0x0Fu) {
        if (!fast_ip_ok(ip, &checked_page)) return 0;
        uint8_t op2 = MEM[ip++];
        FastModRM m;
        if (!fast_decode_modrm(&ip, &checked_page, g, &m) || !m.is_mem) return 0;
        uint32_t ea = m.ea + seg_base;
        if (op2 == 0xC7u && m.reg == 1u && !op16) { // CMPXCHG8B m64
            if (!fast_addr_ok(ea, 8u)) return 0;
            uint64_t exp = ((uint64_t)g[2] << 32) | (uint64_t)g[0];
            uint64_t des = ((uint64_t)g[1] << 32) | (uint64_t)g[3];
            if (atomic_cmpxchg64(ea, &exp, des)) {
                *efl_io |= 0x40u;
            } else {
                g[0] = (uint32_t)exp;
                g[2] = (uint32_t)(exp >> 32);
                *efl_io &= ~0x40u;
            }
            *next_ip_out = ip;
            return 1;
        }
        uint8_t w = (op2 == 0xC0u || op2 == 0xB0u) ? 1u : w_word;
        if (op2 == 0xC0u || op2 == 0xC1u) { // LOCK XADD m, r
            if (!fast_addr_ok(ea, w)) return 0;
            uint32_t rhs = exec_read_reg_w(g, m.reg, w);
            uint32_t old = atomic_rmw_w(ea, w, ATOMIC_OP_ADD, rhs, 0u);
            exec_write_reg_w(g, m.reg, w, old);
            (void)fast_alu(0u, old, rhs, w, &f);
            efl_apply_arith(efl_io, &f, 0);
            *next_ip_out = ip;
            return 1;
        }
        if (op2 == 0xB0u || op2 == 0xB1u) { // LOCK CMPXCHG m, r
            if (!fast_addr_ok(ea, w)) return 0;
            uint32_t acc = exec_read_reg_w(g, 0u, w);
            uint32_t rhs = exec_read_reg_w(g, m.reg, w);
            uint32_t exp = acc;
            int ok = atomic_cmpxchg_w(ea, w, &exp, rhs);
            (void)fast_alu(7u, acc, exp, w, &f);
            efl_apply_arith(efl_io, &f, 0);
            if (!ok) exec_write_reg_w(g, 0u, w, exp);
            *next_ip_out = ip;
            return 1;
        }
        if (op2 == 0xABu || op2 == 0xB3u || op2 == 0xBBu || (op2 == 0xBAu && m.reg >= 5u)) {
            uint32_t bit_in_unit;
            uint32_t target_ea = ea;
            uint8_t bt_op;
            if (op2 == 0xBAu) {
                if (!fast_ip_ok(ip, &checked_page)) return 0;
                uint8_t imm8 = MEM[ip++];
                bit_in_unit = imm8 & (uint32_t)(w * 8u - 1u);
                bt_op = m.reg; // 5=BTS, 6=BTR, 7=BTC
            } else {
                int32_t sbit = op16 ? (int32_t)(int16_t)g[m.reg] : (int32_t)g[m.reg];
                int32_t bpu = (int32_t)(w * 8u);
                int32_t word_off = (sbit >= 0) ? (sbit / bpu) : ((sbit - (bpu - 1)) / bpu);
                bit_in_unit = (uint32_t)(sbit - word_off * bpu);
                target_ea = ea + (uint32_t)(word_off * (int32_t)w);
                bt_op = (op2 == 0xABu) ? 5u : (op2 == 0xB3u) ? 6u : 7u;
            }
            if (!fast_addr_ok(target_ea, w)) return 0;
            uint32_t bitmask = 1u << bit_in_unit;
            uint32_t old = (bt_op == 5u) ? atomic_rmw_w(target_ea, w, ATOMIC_OP_OR, bitmask, 0u) :
                           (bt_op == 6u) ? atomic_rmw_w(target_ea, w, ATOMIC_OP_AND, ~bitmask, 0u) :
                                           atomic_rmw_w(target_ea, w, ATOMIC_OP_XOR, bitmask, 0u);
            *efl_io = (*efl_io & ~1u) | ((old >> bit_in_unit) & 1u);
            *next_ip_out = ip;
            return 1;
        }
        return 0;
    }

    FastModRM m;
    if (!fast_decode_modrm(&ip, &checked_page, g, &m) || !m.is_mem) return 0;
    uint32_t ea = m.ea + seg_base;

    if (op == 0xFEu || op == 0xFFu) { // LOCK INC/DEC
        if (m.reg > 1u) return 0;
        uint8_t w = (op == 0xFEu) ? 1u : w_word;
        if (!fast_addr_ok(ea, w)) return 0;
        uint8_t is_dec = (m.reg == 1u);
        uint32_t old = atomic_rmw_w(ea, w, is_dec ? ATOMIC_OP_SUB : ATOMIC_OP_ADD, 1u, 0u);
        (void)fast_alu(is_dec ? 5u : 0u, old, 1u, w, &f);
        efl_apply_arith(efl_io, &f, 1 /* keep CF */);
        *next_ip_out = ip;
        return 1;
    }

    if ((op & 0xC6u) == 0x00u && op <= 0x31u) { // LOCK ADD/OR/ADC/SBB/AND/SUB/XOR m, r
        uint8_t kind = (op >> 3) & 7u;
        if (kind == 7u) return 0;
        uint8_t w = (op & 1u) ? w_word : 1u;
        if (!fast_addr_ok(ea, w)) return 0;
        uint32_t rhs = exec_read_reg_w(g, m.reg, w);
        uint32_t old = atomic_rmw_w(ea, w, kind, rhs, cf_in);
        (void)fast_alu_cf(kind, old, rhs, w, cf_in, &f);
        efl_apply_arith(efl_io, &f, 0);
        *next_ip_out = ip;
        return 1;
    }

    if (op == 0x80u || op == 0x81u || op == 0x83u) { // LOCK ALU m, imm
        if (m.reg == 7u) return 0;
        uint8_t w = (op == 0x80u) ? 1u : w_word;
        if (!fast_addr_ok(ea, w)) return 0;
        uint32_t rhs;
        if (op == 0x81u) {
            if (!fast_ip_ok(ip + (uint32_t)(w - 1u), &checked_page)) return 0;
            rhs = (uint32_t)LDV(MEM + ip, w);
            ip += w;
        } else {
            if (!fast_ip_ok(ip, &checked_page)) return 0;
            rhs = (op == 0x83u) ? (uint32_t)(int32_t)(int8_t)MEM[ip++] : (uint32_t)MEM[ip++];
        }
        uint32_t old = atomic_rmw_w(ea, w, m.reg, rhs, cf_in);
        (void)fast_alu_cf(m.reg, old, rhs, w, cf_in, &f);
        efl_apply_arith(efl_io, &f, 0);
        *next_ip_out = ip;
        return 1;
    }

    if (op == 0xF6u || op == 0xF7u) { // LOCK NOT (/2) / LOCK NEG (/3)
        if (m.reg != 2u && m.reg != 3u) return 0;
        uint8_t w = (op == 0xF6u) ? 1u : w_word;
        if (!fast_addr_ok(ea, w)) return 0;
        if (m.reg == 2u) {
            (void)atomic_rmw_w(ea, w, ATOMIC_OP_XOR, 0xFFFFFFFFu, 0u);
        } else {
            uint32_t old = atomic_rmw_w(ea, w, ATOMIC_OP_NEG, 0u, 0u);
            (void)fast_alu(5u, 0u, old, w, &f);
            efl_apply_arith(efl_io, &f, 0);
        }
        *next_ip_out = ip;
        return 1;
    }

    return 0;
}

static int fast_try_tail_jump(CPU *c, uint32_t target, uint32_t root_esp, uint32_t root_ret,
                              const uint32_t g[8], const FastFlags *flags,
                              const uint64_t xmm[8][2], uint8_t xmm_dirty,
                              const FastWrite *wbuf, int nw) {
    int is_imp = (target >= IMPORT_THUNK_BASE && target < IMPORT_THUNK_BASE + 0x10000u);
    if (g[4] != root_esp || depth + 1 >= MAXDEPTH || (!is_imp && !rt_has_fn(target))) return 0;
    uint64_t ret_val = 0;
    if (!fast_read(root_esp, 4u, wbuf, nw, &ret_val) || (uint32_t)ret_val != root_ret) return 0;
    fast_commit_state(c, g, flags, xmm, xmm_dirty, wbuf, nw);
    __atomic_fetch_add(&fast_leaf_hits_total, 1, __ATOMIC_RELAXED);
    depth++;
    if (is_imp) rt_import_call(c, target);
    else rt_call(c, target);
    depth--;
    return 1;
}

static int fast_exec_call(CPU *c, uint32_t target, uint32_t ret_ip, uint32_t root_esp, uint32_t root_ret,
                          uint32_t *cur_ip_io, uint32_t g[8], FastFlags *flags,
                          uint64_t xmm[8][2], uint8_t *xmm_loaded, uint8_t *xmm_dirty,
                          FastWrite *wbuf, int *nw) {
    int is_imp = (target >= IMPORT_THUNK_BASE && target < IMPORT_THUNK_BASE + 0x10000u);
    if (depth + 1 >= MAXDEPTH || (!is_imp && !rt_has_fn(target))) return 0;
    if (g[4] > root_esp || !fast_addr_ok(g[4] - 4u, 4u)) return 0;
    uint64_t rv = 0;
    if (!fast_read(root_esp, 4u, wbuf, *nw, &rv) || (uint32_t)rv != root_ret) return 0;
    fast_commit_state(c, g, flags, xmm, *xmm_dirty, wbuf, *nw);
    g[4] -= 4u;
    STM(g[4], (V)ret_ip, 4u);
    STV(c->r + 16, (V)g[4], 4);
    *cur_ip_io = ret_ip;
    *nw = 0;
    flags->loaded = 0;
    flags->dirty = 0;
    *xmm_loaded = 0;
    *xmm_dirty = 0;
    depth++;
    if (is_imp) rt_import_call(c, target);
    else rt_call(c, target);
    depth--;
    memcpy(g, c->r, 32);
    if (ret_ip == root_ret && g[4] > root_esp) {
        __atomic_fetch_add(&fast_leaf_hits_total, 1, __ATOMIC_RELAXED);
        return 1;
    }
    return 2;
}

// Atomic fast-path interpreter for tiny patched leaf functions (never mutates CPU or MEM until RET/CALL commit).
static int try_fast_leaf(CPU *c, uint32_t *cur_ip_io, uint32_t root_esp, uint32_t root_ret) {
    static int fast_on = -1;
    if (__builtin_expect(fast_on < 0, 0)) fast_on = getenv("FNV_NO_FAST_LEAF") == NULL;
    if (__builtin_expect(!fast_on, 0)) return 0;

    uint32_t g[8];
    memcpy(g, c->r, 32);
    FastFlags flags = {0, 0, 0, 0, 0, 0, 0, 0};
    uint64_t xmm[8][2];
    uint8_t xmm_loaded = 0, xmm_dirty = 0;
    FastWrite wbuf[FAST_LEAF_MAX_WRITES];
    int nw = 0;

    uint32_t ip = *cur_ip_io;
    uint32_t checked_page = ip & ~0xFFFu; // entry address was already checked by caller

    for (int step = 0; step < FAST_LEAF_MAX_STEPS; step++) {
        if (!fast_ip_ok(ip, &checked_page)) return 0;
        uint8_t op = MEM[ip++];
        switch (op) {
        case 0x90: // NOP
            break;

        case 0xC3: // RET
        case 0xC2: { // RET imm16
            uint32_t adj = 4u;
            if (op == 0xC2) {
                if (!fast_ip_ok(ip + 1u, &checked_page)) return 0;
                adj += (uint32_t)(uint16_t)LDV(MEM + ip, 2);
                ip += 2u;
            }
            if (g[4] != root_esp) return 0;
            uint64_t ret_val = 0;
            if (!fast_read(root_esp, 4u, wbuf, nw, &ret_val) || (uint32_t)ret_val != root_ret) return 0;
            g[4] += adj;
            fast_commit_state(c, g, &flags, xmm, xmm_dirty, wbuf, nw);
            __atomic_fetch_add(&fast_leaf_hits_total, 1, __ATOMIC_RELAXED);
            return 1;
        }

        case 0xEB: { // JMP rel8
            if (!fast_ip_ok(ip, &checked_page)) return 0;
            int8_t rel = (int8_t)MEM[ip++];
            ip += (uint32_t)(int32_t)rel;
            break;
        }

        case 0xE9: { // JMP rel32 (trampoline or native tail-jump)
            if (!fast_ip_ok(ip + 3u, &checked_page)) return 0;
            int32_t rel = (int32_t)(uint32_t)LDV(MEM + ip, 4);
            uint32_t target = ip + 4u + (uint32_t)rel;
            if (fast_try_tail_jump(c, target, root_esp, root_ret, g, &flags, xmm, xmm_dirty, wbuf, nw)) return 1;
            ip = target;
            break;
        }

        case 0xE8: { // CALL rel32 -> dispatch to native/import function
            if (!fast_ip_ok(ip + 3u, &checked_page)) return 0;
            int32_t rel = (int32_t)(uint32_t)LDV(MEM + ip, 4);
            ip += 4u;
            uint32_t ret_ip = ip;
            uint32_t target = ret_ip + (uint32_t)rel;
            int rc = fast_exec_call(c, target, ret_ip, root_esp, root_ret, cur_ip_io, g, &flags,
                                    xmm, &xmm_loaded, &xmm_dirty, wbuf, &nw);
            if (rc == 0) return 0;
            if (rc == 1) return 1;
            ip = ret_ip;
            break;
        }

        case 0x68: { // PUSH imm32
            if (!fast_ip_ok(ip + 3u, &checked_page)) return 0;
            uint32_t val = (uint32_t)LDV(MEM + ip, 4);
            ip += 4u;
            g[4] -= 4u;
            if (!fast_write(g[4], val, 4u, wbuf, &nw)) return 0;
            break;
        }

        case 0x6A: { // PUSH imm8
            if (!fast_ip_ok(ip, &checked_page)) return 0;
            uint32_t val = (uint32_t)(int32_t)(int8_t)MEM[ip++];
            g[4] -= 4u;
            if (!fast_write(g[4], val, 4u, wbuf, &nw)) return 0;
            break;
        }

        case 0x70 ... 0x7F: { // Jcc rel8
            if (!fast_ip_ok(ip, &checked_page)) return 0;
            int8_t rel = (int8_t)MEM[ip++];
            fast_load_flags(&flags, c);
            if (fast_eval_cc(op & 0xFu, &flags)) ip += (uint32_t)(int32_t)rel;
            break;
        }

        case 0x88: case 0x89: case 0x8A: case 0x8B: { // MOV
            FastModRM m;
            if (!fast_decode_modrm(&ip, &checked_page, g, &m)) return 0;
            uint8_t w = (op & 1u) ? 4u : 1u;
            if (op & 2u) { // reg <- rm
                uint32_t v = 0;
                if (!fast_read_rm(&m, w, g, wbuf, nw, &v)) return 0;
                if (w == 1u) fast_set_r8(g, m.reg, (uint8_t)v);
                else g[m.reg] = v;
            } else { // rm <- reg
                uint32_t v = (w == 1u) ? fast_get_r8(g, m.reg) : g[m.reg];
                if (!fast_write_rm(&m, w, v, g, wbuf, &nw)) return 0;
            }
            break;
        }

        case 0x8D: { // LEA r32, m
            FastModRM m;
            if (!fast_decode_modrm(&ip, &checked_page, g, &m) || !m.is_mem) return 0;
            g[m.reg] = m.ea;
            break;
        }

        case 0xA0: case 0xA1: { // MOV AL/EAX, [imm32]
            if (!fast_ip_ok(ip + 3u, &checked_page)) return 0;
            uint32_t addr = (uint32_t)LDV(MEM + ip, 4);
            ip += 4u;
            uint8_t w = (op == 0xA1) ? 4u : 1u;
            uint64_t v = 0;
            if (!fast_read(addr, w, wbuf, nw, &v)) return 0;
            if (w == 1u) fast_set_r8(g, 0u, (uint8_t)v);
            else g[0] = (uint32_t)v;
            break;
        }

        case 0xA2: case 0xA3: { // MOV [imm32], AL/EAX
            if (!fast_ip_ok(ip + 3u, &checked_page)) return 0;
            uint32_t addr = (uint32_t)LDV(MEM + ip, 4);
            ip += 4u;
            uint8_t w = (op == 0xA3) ? 4u : 1u;
            uint64_t v = (w == 1u) ? (uint64_t)fast_get_r8(g, 0u) : (uint64_t)g[0];
            if (!fast_write(addr, v, w, wbuf, &nw)) return 0;
            break;
        }

        case 0xB0 ... 0xB7: { // MOV r8, imm8
            if (!fast_ip_ok(ip, &checked_page)) return 0;
            fast_set_r8(g, op - 0xB0u, MEM[ip++]);
            break;
        }

        case 0xB8 ... 0xBF: { // MOV r32, imm32
            if (!fast_ip_ok(ip + 3u, &checked_page)) return 0;
            g[op - 0xB8u] = (uint32_t)LDV(MEM + ip, 4);
            ip += 4u;
            break;
        }

        case 0xC6: case 0xC7: { // MOV r/m8, imm8 ; MOV r/m32, imm32
            FastModRM m;
            if (!fast_decode_modrm(&ip, &checked_page, g, &m) || m.reg != 0u) return 0;
            uint8_t w = (op == 0xC7) ? 4u : 1u;
            if (!fast_ip_ok(ip + (uint32_t)(w - 1u), &checked_page)) return 0;
            uint32_t imm = (w == 1u) ? MEM[ip++] : (uint32_t)LDV(MEM + ip, 4);
            if (w == 4u) ip += 4u;
            if (!fast_write_rm(&m, w, imm, g, wbuf, &nw)) return 0;
            break;
        }

        case 0x50 ... 0x57: { // PUSH r32
            uint32_t val = g[op - 0x50u];
            g[4] -= 4u;
            if (!fast_write(g[4], val, 4u, wbuf, &nw)) return 0;
            break;
        }

        case 0x58 ... 0x5F: { // POP r32
            uint64_t val = 0;
            if (!fast_read(g[4], 4u, wbuf, nw, &val)) return 0;
            g[4] += 4u;
            g[op - 0x58u] = (uint32_t)val;
            break;
        }

        case 0x99: // CDQ
            g[2] = ((int32_t)g[0] < 0) ? 0xFFFFFFFFu : 0u;
            break;

        case 0x00 ... 0x05:
        case 0x08 ... 0x0D:
        case 0x20 ... 0x25:
        case 0x28 ... 0x2D:
        case 0x30 ... 0x35:
        case 0x38 ... 0x3D: { // ADD/OR/AND/SUB/XOR/CMP
            uint8_t kind = (op >> 3) & 7u;
            uint8_t form = op & 7u;
            uint8_t w = (form & 1u) ? 4u : 1u;
            if (form >= 4u) {
                if (!fast_ip_ok(ip + (uint32_t)(w - 1u), &checked_page)) return 0;
                uint32_t imm = (w == 1u) ? MEM[ip++] : (uint32_t)LDV(MEM + ip, 4);
                if (w == 4u) ip += 4u;
                uint32_t lhs = (w == 1u) ? fast_get_r8(g, 0u) : g[0];
                uint32_t res = fast_alu(kind, lhs, imm, w, &flags);
                if (kind != 7u) {
                    if (w == 1u) fast_set_r8(g, 0u, (uint8_t)res);
                    else g[0] = res;
                }
            } else {
                FastModRM m;
                if (!fast_decode_modrm(&ip, &checked_page, g, &m)) return 0;
                uint32_t rm_val = 0;
                if (!fast_read_rm(&m, w, g, wbuf, nw, &rm_val)) return 0;
                uint32_t reg_val = (w == 1u) ? fast_get_r8(g, m.reg) : g[m.reg];
                if (form & 2u) {
                    uint32_t res = fast_alu(kind, reg_val, rm_val, w, &flags);
                    if (kind != 7u) {
                        if (w == 1u) fast_set_r8(g, m.reg, (uint8_t)res);
                        else g[m.reg] = res;
                    }
                } else {
                    uint32_t res = fast_alu(kind, rm_val, reg_val, w, &flags);
                    if (kind != 7u && !fast_write_rm(&m, w, res, g, wbuf, &nw)) return 0;
                }
            }
            break;
        }

        case 0x80: case 0x81: case 0x83: { // ALU r/m, imm
            FastModRM m;
            if (!fast_decode_modrm(&ip, &checked_page, g, &m)) return 0;
            if (m.reg == 2u || m.reg == 3u) return 0;
            uint8_t w = (op == 0x80) ? 1u : 4u;
            uint8_t imm_sz = (op == 0x81) ? 4u : 1u;
            if (!fast_ip_ok(ip + (uint32_t)(imm_sz - 1u), &checked_page)) return 0;
            uint32_t imm;
            if (op == 0x80) imm = MEM[ip++];
            else if (op == 0x83) imm = (uint32_t)(int32_t)(int8_t)MEM[ip++];
            else { imm = (uint32_t)LDV(MEM + ip, 4); ip += 4u; }
            uint32_t lhs = 0;
            if (!fast_read_rm(&m, w, g, wbuf, nw, &lhs)) return 0;
            uint32_t res = fast_alu(m.reg, lhs, imm, w, &flags);
            if (m.reg != 7u && !fast_write_rm(&m, w, res, g, wbuf, &nw)) return 0;
            break;
        }

        case 0x84: case 0x85: { // TEST r/m, r
            FastModRM m;
            if (!fast_decode_modrm(&ip, &checked_page, g, &m)) return 0;
            uint8_t w = (op == 0x85) ? 4u : 1u;
            uint32_t lhs = 0;
            if (!fast_read_rm(&m, w, g, wbuf, nw, &lhs)) return 0;
            uint32_t rhs = (w == 1u) ? fast_get_r8(g, m.reg) : g[m.reg];
            fast_alu(4u, lhs, rhs, w, &flags);
            break;
        }

        case 0xA8: case 0xA9: { // TEST AL/EAX, imm
            uint8_t w = (op == 0xA9) ? 4u : 1u;
            if (!fast_ip_ok(ip + (uint32_t)(w - 1u), &checked_page)) return 0;
            uint32_t imm = (w == 1u) ? MEM[ip++] : (uint32_t)LDV(MEM + ip, 4);
            if (w == 4u) ip += 4u;
            uint32_t lhs = (w == 1u) ? fast_get_r8(g, 0u) : g[0];
            fast_alu(4u, lhs, imm, w, &flags);
            break;
        }

        case 0xF6: case 0xF7: { // TEST/NOT/NEG r/m
            FastModRM m;
            if (!fast_decode_modrm(&ip, &checked_page, g, &m) || m.reg > 3u) return 0;
            uint8_t w = (op == 0xF7) ? 4u : 1u;
            uint32_t lhs = 0;
            if (!fast_read_rm(&m, w, g, wbuf, nw, &lhs)) return 0;
            if (m.reg <= 1u) {
                if (!fast_ip_ok(ip + (uint32_t)(w - 1u), &checked_page)) return 0;
                uint32_t imm = (w == 1u) ? MEM[ip++] : (uint32_t)LDV(MEM + ip, 4);
                if (w == 4u) ip += 4u;
                fast_alu(4u, lhs, imm, w, &flags);
            } else if (m.reg == 2u) {
                uint32_t mask = (w == 1u) ? 0xFFu : 0xFFFFFFFFu;
                if (!fast_write_rm(&m, w, (~lhs) & mask, g, wbuf, &nw)) return 0;
            } else {
                uint32_t res = fast_alu(5u, 0u, lhs, w, &flags);
                if (!fast_write_rm(&m, w, res, g, wbuf, &nw)) return 0;
            }
            break;
        }

        case 0x40 ... 0x47: // INC r32
            g[op - 0x40u] = fast_inc_dec(g[op - 0x40u], 0, &flags, c);
            break;

        case 0x48 ... 0x4F: // DEC r32
            g[op - 0x48u] = fast_inc_dec(g[op - 0x48u], 1, &flags, c);
            break;

        case 0xFF: { // INC/DEC/CALL/JMP/PUSH r/m32
            FastModRM m;
            if (!fast_decode_modrm(&ip, &checked_page, g, &m)) return 0;
            uint32_t val = 0;
            if (!fast_read_rm(&m, 4u, g, wbuf, nw, &val)) return 0;
            if (m.reg == 6u) {
                g[4] -= 4u;
                if (!fast_write(g[4], val, 4u, wbuf, &nw)) return 0;
            } else if (m.reg <= 1u) {
                uint32_t res = fast_inc_dec(val, m.reg == 1u, &flags, c);
                if (!fast_write_rm(&m, 4u, res, g, wbuf, &nw)) return 0;
            } else if (m.reg == 2u) {
                uint32_t ret_ip = ip;
                int rc = fast_exec_call(c, val, ret_ip, root_esp, root_ret, cur_ip_io, g, &flags,
                                        xmm, &xmm_loaded, &xmm_dirty, wbuf, &nw);
                if (rc == 0) return 0;
                if (rc == 1) return 1;
                ip = ret_ip;
            } else if (m.reg == 4u) {
                if (fast_try_tail_jump(c, val, root_esp, root_ret, g, &flags, xmm, xmm_dirty, wbuf, nw)) return 1;
                if (!host_addr_is_executable(val)) return 0;
                ip = val;
            } else {
                return 0;
            }
            break;
        }

        case 0xF0: { // LOCK prefix -> hardware-atomic 32-bit RMW serialized with rt_lock()/rt_unlock()
            if (!fast_ip_ok(ip, &checked_page)) return 0;
            uint8_t lop = MEM[ip++];
            FastModRM m;
            uint32_t rhs = 0;
            uint8_t subop = 0; // 1=INC, 2=DEC, 3=XADD, 4=CMPXCHG, 5=ADD, 6=SUB, 7=XCHG
            if (lop == 0xFF) {
                if (!fast_decode_modrm(&ip, &checked_page, g, &m) || m.reg > 1u) return 0;
                subop = (m.reg == 1u) ? 2u : 1u;
            } else if (lop == 0x0F) {
                if (!fast_ip_ok(ip, &checked_page)) return 0;
                uint8_t lop2 = MEM[ip++];
                if (lop2 == 0xC1) subop = 3u;
                else if (lop2 == 0xB1) subop = 4u;
                else return 0;
                if (!fast_decode_modrm(&ip, &checked_page, g, &m)) return 0;
                rhs = g[m.reg];
            } else if (lop == 0x01 || lop == 0x29 || lop == 0x87) {
                if (!fast_decode_modrm(&ip, &checked_page, g, &m)) return 0;
                subop = (lop == 0x01) ? 5u : (lop == 0x29) ? 6u : 7u;
                rhs = g[m.reg];
            } else if (lop == 0x83) {
                if (!fast_decode_modrm(&ip, &checked_page, g, &m) || (m.reg != 0u && m.reg != 5u)) return 0;
                if (!fast_ip_ok(ip, &checked_page)) return 0;
                rhs = (uint32_t)(int32_t)(int8_t)MEM[ip++];
                subop = (m.reg == 5u) ? 6u : 5u;
            } else {
                return 0;
            }
            if (!m.is_mem || !fast_addr_ok(m.ea, 4u)) return 0;
            if (subop <= 2u) fast_load_flags(&flags, c);
            fast_commit_state(c, g, &flags, xmm, xmm_dirty, wbuf, nw);
            if (subop <= 2u) {
                uint32_t old = atomic_rmw_w(m.ea, 4u, (subop == 2u) ? ATOMIC_OP_SUB : ATOMIC_OP_ADD, 1u, 0u);
                (void)fast_inc_dec(old, subop == 2u, &flags, c);
            } else if (subop == 3u) {
                uint32_t old = atomic_rmw_w(m.ea, 4u, ATOMIC_OP_ADD, rhs, 0u);
                g[m.reg] = old;
                (void)fast_alu(0u, old, rhs, 4u, &flags);
            } else if (subop == 4u) {
                uint32_t orig_eax = g[0], exp = orig_eax;
                int ok = atomic_cmpxchg_w(m.ea, 4u, &exp, rhs);
                (void)fast_alu(7u, orig_eax, exp, 4u, &flags);
                if (!ok) g[0] = exp;
            } else if (subop == 5u || subop == 6u) {
                uint32_t old = atomic_rmw_w(m.ea, 4u, (subop == 6u) ? ATOMIC_OP_SUB : ATOMIC_OP_ADD, rhs, 0u);
                (void)fast_alu((subop == 6u) ? 5u : 0u, old, rhs, 4u, &flags);
            } else {
                g[m.reg] = atomic_rmw_w(m.ea, 4u, ATOMIC_OP_XCHG, rhs, 0u);
            }
            fast_commit_state(c, g, &flags, xmm, 0u, wbuf, 0);
            *cur_ip_io = ip;
            nw = 0;
            flags.dirty = 0;
            xmm_dirty = 0;
            break;
        }

        case 0xC1: case 0xD1: { // SHL/SHR/SAR r/m32, imm8/1
            FastModRM m;
            if (!fast_decode_modrm(&ip, &checked_page, g, &m)) return 0;
            if (m.reg != 4u && m.reg != 5u && m.reg != 7u) return 0;
            uint32_t cnt = 1u;
            if (op == 0xC1) {
                if (!fast_ip_ok(ip, &checked_page)) return 0;
                cnt = MEM[ip++];
            }
            uint32_t val = 0;
            if (!fast_read_rm(&m, 4u, g, wbuf, nw, &val)) return 0;
            uint32_t res = fast_shift32(m.reg, val, cnt, &flags, c);
            if (!fast_write_rm(&m, 4u, res, g, wbuf, &nw)) return 0;
            break;
        }

        case 0x0F: {
            if (!fast_ip_ok(ip, &checked_page)) return 0;
            uint8_t op2 = MEM[ip++];
            if (op2 == 0x1F) { // multi-byte NOP
                FastModRM m;
                if (!fast_decode_modrm(&ip, &checked_page, g, &m)) return 0;
            } else if (op2 >= 0x80u && op2 <= 0x8Fu) { // Jcc rel32
                if (!fast_ip_ok(ip + 3u, &checked_page)) return 0;
                int32_t rel = (int32_t)(uint32_t)LDV(MEM + ip, 4);
                ip += 4u;
                fast_load_flags(&flags, c);
                if (fast_eval_cc(op2 & 0xFu, &flags)) ip += (uint32_t)rel;
            } else if (op2 >= 0x90u && op2 <= 0x9Fu) { // SETcc r/m8
                FastModRM m;
                if (!fast_decode_modrm(&ip, &checked_page, g, &m)) return 0;
                fast_load_flags(&flags, c);
                uint32_t val = fast_eval_cc(op2 & 0xFu, &flags) ? 1u : 0u;
                if (!fast_write_rm(&m, 1u, val, g, wbuf, &nw)) return 0;
            } else if (op2 >= 0x40u && op2 <= 0x4Fu) { // CMOVcc r32, r/m32
                FastModRM m;
                if (!fast_decode_modrm(&ip, &checked_page, g, &m)) return 0;
                uint32_t rhs = 0;
                if (!fast_read_rm(&m, 4u, g, wbuf, nw, &rhs)) return 0;
                fast_load_flags(&flags, c);
                if (fast_eval_cc(op2 & 0xFu, &flags)) g[m.reg] = rhs;
            } else if (op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF) { // MOVZX/MOVSX
                FastModRM m;
                if (!fast_decode_modrm(&ip, &checked_page, g, &m)) return 0;
                uint8_t w = (op2 == 0xB7 || op2 == 0xBF) ? 2u : 1u;
                uint32_t v = 0;
                if (!fast_read_rm(&m, w, g, wbuf, nw, &v)) return 0;
                if (op2 == 0xBE) v = (uint32_t)(int32_t)(int8_t)v;
                else if (op2 == 0xBF) v = (uint32_t)(int32_t)(int16_t)v;
                g[m.reg] = v;
            } else if (op2 == 0x57) { // XORPS xmm, xmm
                FastModRM m;
                if (!fast_decode_modrm(&ip, &checked_page, g, &m) || m.is_mem) return 0;
                if (m.reg == m.rm) {
                    xmm[m.reg][0] = 0u;
                    xmm[m.reg][1] = 0u;
                    xmm_loaded |= (uint8_t)(1u << m.reg);
                } else {
                    fast_ensure_xmm(m.reg, xmm, &xmm_loaded, c);
                    fast_ensure_xmm(m.rm, xmm, &xmm_loaded, c);
                    xmm[m.reg][0] ^= xmm[m.rm][0];
                    xmm[m.reg][1] ^= xmm[m.rm][1];
                }
                xmm_dirty |= (uint8_t)(1u << m.reg);
            } else {
                return 0;
            }
            break;
        }

        case 0x66: { // strictly whitelisted 16-bit / SSE2 prefix instructions
            if (!fast_ip_ok(ip, &checked_page)) return 0;
            uint8_t op2 = MEM[ip++];
            if (op2 == 0x90) { // 2-byte NOP
            } else if (op2 == 0x0F) {
                if (!fast_ip_ok(ip, &checked_page)) return 0;
                uint8_t op3 = MEM[ip++];
                FastModRM m;
                if (!fast_decode_modrm(&ip, &checked_page, g, &m)) return 0;
                if (op3 == 0x1F) { // multi-byte NOP
                } else if (op3 == 0xD6) { // MOVQ r/m64, xmm
                    fast_ensure_xmm(m.reg, xmm, &xmm_loaded, c);
                    if (m.is_mem) {
                        if (!fast_write(m.ea, xmm[m.reg][0], 8u, wbuf, &nw)) return 0;
                    } else {
                        xmm[m.rm][0] = xmm[m.reg][0];
                        xmm[m.rm][1] = 0u;
                        xmm_loaded |= (uint8_t)(1u << m.rm);
                        xmm_dirty |= (uint8_t)(1u << m.rm);
                    }
                } else if (op3 == 0xEF && !m.is_mem) { // PXOR xmm, xmm
                    if (m.reg == m.rm) {
                        xmm[m.reg][0] = 0u;
                        xmm[m.reg][1] = 0u;
                        xmm_loaded |= (uint8_t)(1u << m.reg);
                    } else {
                        fast_ensure_xmm(m.reg, xmm, &xmm_loaded, c);
                        fast_ensure_xmm(m.rm, xmm, &xmm_loaded, c);
                        xmm[m.reg][0] ^= xmm[m.rm][0];
                        xmm[m.reg][1] ^= xmm[m.rm][1];
                    }
                    xmm_dirty |= (uint8_t)(1u << m.reg);
                } else {
                    return 0;
                }
            } else if (op2 == 0x89 || op2 == 0x8B) { // MOV r/m16, r16 ; MOV r16, r/m16
                FastModRM m;
                if (!fast_decode_modrm(&ip, &checked_page, g, &m)) return 0;
                if (op2 == 0x8B) {
                    uint32_t v = 0;
                    if (!fast_read_rm(&m, 2u, g, wbuf, nw, &v)) return 0;
                    g[m.reg] = (g[m.reg] & ~0xFFFFu) | (v & 0xFFFFu);
                } else {
                    if (!fast_write_rm(&m, 2u, g[m.reg] & 0xFFFFu, g, wbuf, &nw)) return 0;
                }
            } else if (op2 == 0xC7) { // MOV r/m16, imm16
                FastModRM m;
                if (!fast_decode_modrm(&ip, &checked_page, g, &m) || m.reg != 0u) return 0;
                if (!fast_ip_ok(ip + 1u, &checked_page)) return 0;
                uint32_t imm = (uint32_t)(uint16_t)LDV(MEM + ip, 2);
                ip += 2u;
                if (!fast_write_rm(&m, 2u, imm, g, wbuf, &nw)) return 0;
            } else {
                return 0;
            }
            break;
        }

        default:
            return 0;
        }
    }
    return 0;
}

void rt_fallback(CPU *c, uint32_t a) {
    if (!host_addr_is_executable(a) || depth >= MAXDEPTH) rt_trap(c, TRAP_NOFN, a);
    rt_run_boot_hook_if_armed(c, a);
    log_root(a);
    uint32_t root_esp = REG(c, R_ESP);
    uint32_t root_ret = G32(root_esp);
    uint32_t cur_ip = a;
    if (cur_ip == root_ret) return;
    if (try_fast_leaf(c, &cur_ip, root_esp, root_ret)) return;
    ThreadUC *tu = engine(c);
    uc_engine *uc = tu->uc;
    uint32_t teb_base = REG(c, 272);
    for (;;) {
        int need_flush = ensure_text_hook(tu);
        uint64_t ep = rt_patch_epoch();
        if (tu->epoch != ep) {
            for (uint32_t i = 0; i < tu->n_atomic_hooks; i++) uc_hook_del(uc, tu->atomic_hooks[i]);
            tu->n_atomic_hooks = 0;
            memset(tu->hooked_set, 0, sizeof(tu->hooked_set));
            memset(tu->scanned_entry, 0, sizeof(tu->scanned_entry));
            tu->epoch = ep;
            need_flush = 1;
        }
        if (need_flush) uc_ctl_flush_tb(uc);
        FallbackRun *prev_run = tu->active_run;
        cpu_to_uc(c, uc);
        uint64_t until = addr_is_hooked(root_ret) ? 0 : (uint64_t)root_ret;
        FallbackRun run;
        uc_err e;
        for (;;) {
            run = (FallbackRun){cur_ip, root_ret, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            tu->active_run = &run;
            tuc_ensure_entry_atomics(tu, cur_ip);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            e = uc_emu_start(uc, cur_ip, until, 0, 0);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            if (e != UC_ERR_OK || (!run.pending_edge && !run.pending_atomic)) {
                uc_to_cpu(uc, c);
                tu->active_run = prev_run;
                break;
            }
            if (run.pending_edge) {
                uc_ctl_remove_cache(uc, run.edge_pc, (uint64_t)run.edge_pc + run.edge_sz);
                cur_ip = run.edge_pc;
                continue;
            }
            uint32_t g[9];
            void *ptrs[9];
            for (int i = 0; i < 9; i++) ptrs[i] = &g[i];
            uc_reg_read_batch(uc, uc_atomic_regs, ptrs, 9);
            uint32_t next_ip = 0;
            if (exec_atomic_insn(g, &g[8], teb_base, run.atomic_ip, &next_ip)) {
                uc_reg_write_batch(uc, uc_atomic_regs, ptrs, 9);
                cur_ip = next_ip;
                if (cur_ip == root_ret) {
                    uc_to_cpu(uc, c);
                    tu->active_run = prev_run;
                    return;
                }
                continue;
            }
            uc_ctl_remove_cache(uc, run.atomic_ip, (uint64_t)run.atomic_end);
            FallbackRun step = { .cur_ip = run.atomic_ip, .root_ret = root_ret, .suppress_hooks = 1 };
            tu->active_run = &step;
            rt_lock();
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            e = uc_emu_start(uc, run.atomic_ip, run.atomic_end, 0, 0);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            rt_unlock();
            if (e != UC_ERR_OK) {
                uint32_t eip = 0;
                uc_reg_read(uc, UC_X86_REG_EIP, &eip);
                uc_to_cpu(uc, c);
                tu->active_run = prev_run;
                fprintf(stderr, "[fallback] %s at %#x (root %#x)\n", uc_strerror(e), eip, a);
                rt_trap(c, TRAP_NOFN, eip);
            }
            if (step.done) {
                uc_to_cpu(uc, c);
                tu->active_run = prev_run;
                return;
            }
            uc_reg_read(uc, UC_X86_REG_EIP, &cur_ip);
            if (cur_ip == root_ret) {
                uc_to_cpu(uc, c);
                tu->active_run = prev_run;
                return;
            }
        }

        if (run.done) return;
        if (run.has_pending) {
            depth++;
            if (run.pending_is_import) rt_import_call(c, run.pending_target);
            else rt_call(c, run.pending_target);
            depth--;
            if (run.pending_ret == root_ret && REG(c, R_ESP) > root_esp) return;
            cur_ip = run.pending_ret;
            if (cur_ip == root_ret) return;
            continue;
        }
        if (e != UC_ERR_OK) {
            uint32_t eip = 0;
            uc_reg_read(uc, UC_X86_REG_EIP, &eip);
            fprintf(stderr, "[fallback] %s at %#x (root %#x)\n", uc_strerror(e), eip, a);
            rt_trap(c, TRAP_NOFN, eip);
        }
        return;
    }
}

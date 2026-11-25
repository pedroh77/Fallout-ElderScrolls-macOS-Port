// fnv-re recomp runtime — shared by every generated translation unit.
//
// Value model (M0, correctness first): every P-code value is an unsigned __int128 holding the
// varnode's little-endian bytes; ops mask results to the output size. Floats are converted from /
// to their storage format (4 = float, 8 = double, 10 = x87 extended) around each operation.
// x87 arithmetic runs in double — the MSVC CRT sets precision control to 53 bits, so this matches
// the hardware as the game actually runs it.
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef unsigned __int128 U;
typedef __int128 S;

#define REGFILE_SIZE 8960
typedef struct CPU { uint8_t r[REGFILE_SIZE]; } CPU;
// Ghidra's x86 model shifts ST0..ST7 instead of tracking the x87 TOP field. The emitter keeps TOP
// in this spare register-file byte and merges it into FPUStatusWord before anything reads it.
#define REG_TOP 8900
#define REG_FPUSW 4258
#define TOPADJ(R, d) ((R)[REG_TOP] = (uint8_t)(((R)[REG_TOP] + (d)) & 7))
#define SYNC_FPUSW(R) ST((R) + REG_FPUSW, (LD((R) + REG_FPUSW, 2) & ~(U)0x3800) | ((U)(R)[REG_TOP] << 11), 2)

// Base of the 4 GB guest address space. The host maps it at RT_MEM_BASE; generated code built with
// -DRT_FIXED_MEM uses that constant instead of loading the pointer (a byte store could alias the
// global, so the compiler would otherwise reload it before nearly every guest access).
#define RT_MEM_BASE 0x300000000000ULL
#ifdef RT_FIXED_MEM
#define MEM ((uint8_t *)RT_MEM_BASE)
#else
extern uint8_t *MEM;
#endif
extern int64_t rt_budget;     // loop guard: decremented on every taken branch
typedef void (*GuestFn)(CPU *);

void rt_trap(CPU *c, int code, uint32_t addr) __attribute__((noreturn));
enum { TRAP_BUDGET = 1, TRAP_BRANCHIND = 2, TRAP_DIV0 = 3, TRAP_FALLOUT = 4, TRAP_UNSUPPORTED = 5 };

static inline U MASK(int n) { return n >= 16 ? ~(U)0 : (((U)1 << (8 * n)) - 1); }
static inline U LD(const uint8_t *p, int n) { U v = 0; memcpy(&v, p, n); return v; }
static inline void ST(uint8_t *p, U v, int n) { memcpy(p, &v, n); }
static inline uint8_t *GM(U addr) { return MEM + (uint32_t)addr; }
static inline S SX(U v, int n) { int s = 128 - 8 * n; return (S)(v << s) >> s; }

static inline double f80_get(const uint8_t *p) {
    uint64_t m; uint16_t se; memcpy(&m, p, 8); memcpy(&se, p + 8, 2);
    int e = se & 0x7fff;
    uint64_t sign = (uint64_t)(se >> 15) << 63;
    if (e == 0x7fff) {                         // inf / NaN (payload kept: top fraction bits map 1:1)
        uint64_t b = sign | (0x7ffULL << 52) | ((m >> 11) & ((1ULL << 52) - 1));
        if (m << 1) b |= 1ULL << 51;
        double r; memcpy(&r, &b, 8); return r;
    }
    if (m == 0) { double r; memcpy(&r, &sign, 8); return r; }
    int de = e - 16383 + 1023;
    if (de > 0 && de < 0x7ff && (m >> 63)) {   // normal fast path: round the 64-bit mantissa to 53 bits
        uint64_t frac = m << 1;                // drop the explicit integer bit
        uint64_t keep = frac >> 12, rest = frac & 0xfff;
        if (rest > 0x800 || (rest == 0x800 && (keep & 1))) keep++;   // round to nearest even
        uint64_t b = sign + (((uint64_t)de << 52) + keep);           // a carry rolls into the exponent
        double r; memcpy(&r, &b, 8); return r;
    }
    double r = ldexp((double)m, e - 16383 - 63);   // denormal / overflow / unnormal: slow but exact
    return sign ? -r : r;
}
static inline void f80_put(uint8_t *p, double d) {
    uint64_t b; memcpy(&b, &d, 8);
    uint16_t se = (uint16_t)((b >> 63) << 15);
    uint64_t e = (b >> 52) & 0x7ff, f = b & ((1ULL << 52) - 1), m;
    if (e == 0) {
        if (f == 0) m = 0;
        else { int msb = 63 - __builtin_clzll(f); m = f << (63 - msb); se |= (uint16_t)(msb - 1074 + 16383); }
    } else if (e == 0x7ff) { se |= 0x7fff; m = f ? (0xC000000000000000ULL | (f << 11)) : 0x8000000000000000ULL; }
    else { se |= (uint16_t)(e - 1023 + 16383); m = (1ULL << 63) | (f << 11); }
    memcpy(p, &m, 8); memcpy(p + 8, &se, 2);
}
static inline double FG(U v, int n) {
    uint8_t b[16]; memcpy(b, &v, 16);
    if (n == 4) { float f; memcpy(&f, b, 4); return f; }
    if (n == 8) { double d; memcpy(&d, b, 8); return d; }
    return f80_get(b);
}
static inline U FP(double d, int n) {
    U v = 0;
    if (n == 4) { float f = (float)d; memcpy(&v, &f, 4); }
    else if (n == 8) memcpy(&v, &d, 8);
    else { uint8_t b[16] = {0}; f80_put(b, d); memcpy(&v, b, 16); }
    return v;
}
// x86 float->int truncation: out-of-range and NaN give the "integer indefinite" (MIN_INT).
static inline U FTRUNC(double d, int n) {
    double lim = ldexp(1.0, 8 * n - 1);
    if (!(d >= -lim && d < lim)) return ((U)1 << (8 * n - 1)) & MASK(n);
    return (U)(S)(int64_t)d & MASK(n);
}
// x87 FPUControlWord rounding control (bits 10..11 at R+4256) and SSE MXCSR (bits 13..14 at R+4244).
static inline double FRND(const uint8_t *R, double d) {
    switch ((R[4257] >> 2) & 3) {
    case 0: return nearbyint(d);
    case 1: return floor(d);
    case 2: return ceil(d);
    default: return trunc(d);
    }
}
static inline double MXRND(const uint8_t *R, double d) {
    switch ((R[4245] >> 5) & 3) {
    case 0: return nearbyint(d);
    case 1: return floor(d);
    case 2: return ceil(d);
    default: return trunc(d);
    }
}
// x86 NaN propagation: a NaN operand is returned (quieted) instead of the host's default NaN;
// the first operand wins when both are NaN (SSE rule).
// A NaN *created* by an operation (inf-inf, 0*inf, ...) is x86's "real indefinite": negative quiet
// NaN. ARM's default NaN is positive, so it is re-signed.
static inline double XNAN(double r) { return isnan(r) ? -fabs(r) : r; }
#define FBIN(OP, a, na, b, nb, n) ({ double _x = FG(a, na), _y = FG(b, nb); \
    isnan(_x) ? FP(_x, n) : isnan(_y) ? FP(_y, n) : FP(XNAN(_x OP _y), n); })
static inline U POPC(U v) { return (U)(__builtin_popcountll((uint64_t)v) + __builtin_popcountll((uint64_t)(v >> 64))); }
static inline U SHL(U a, U b, int n) { return b >= (U)(8 * n) ? 0 : (a << (int)b) & MASK(n); }
static inline U SHR(U a, U b, int n) { return b >= (U)(8 * n) ? 0 : (a & MASK(n)) >> (int)b; }
static inline U SAR(U a, U b, int n) { int s = b >= (U)(8 * n) ? 8 * n - 1 : (int)b; return (U)(SX(a, n) >> s) & MASK(n); }
static inline U SCARRY(U a, U b, int n) { S r = SX(a, n) + SX(b, n); return r != SX((U)r, n); }
static inline U SBORROW(U a, U b, int n) { S r = SX(a, n) - SX(b, n); return r != SX((U)r, n); }

// ---- call trace (debug builds: -DRT_CALLTRACE) ------------------------------------------------
#ifdef RT_CALLTRACE
extern _Thread_local uint32_t rt_ct_ring[256];
extern _Thread_local uint32_t rt_ct_pos;
#define RT_ENTER(a) (rt_ct_ring[rt_ct_pos++ & 255] = (a))
#else
#define RT_ENTER(a) ((void)0)
#endif
void rt_calltrace_dump(void);

// ---- whole-program support (M1) ------------------------------------------------------------
enum { TRAP_NOFN = 6, TRAP_INT3 = 7 };
void rt_call(CPU *c, uint32_t guest_addr);   // dispatch an indirect call / tail jump
void rt_fallback(CPU *c, uint32_t guest_addr);   // no native code: interpret (host) or trap (tests)
// Packed dynamic-patch site entry: bits 0..29 = VA (< 0x40000000),
// bits 30..31 = kind: 0 = CALL rel32 (5B, 0xE8), 1 = PUSH imm8 (2B, 0x6A), 2 = PUSH imm32 (5B, 0x68).
#define RT_SITE_ADDR(x) ((x) & 0x3FFFFFFFu)
#define RT_SITE_KIND(x) ((x) >> 30)
enum { RT_SITE_CALL5 = 0u, RT_SITE_PUSH2 = 1u, RT_SITE_PUSH5 = 2u };
typedef struct { uint32_t lo, hi, group; } RtFnRange;
extern uint8_t rt_fn_patched[];
static inline uint8_t rt_group_state(uint32_t gid) {
    return __atomic_load_n(&rt_fn_patched[gid], __ATOMIC_RELAXED);
}
static inline int rt_is_patched_group(uint32_t gid) {
    return __builtin_expect(rt_group_state(gid) == 1u, 0);
}
void rt_notify_code_patch(uint32_t addr, uint32_t size);
void rt_notify_dyn_patch(uint32_t addr, uint32_t size);
int rt_dyn_site_covers(uint32_t addr, uint32_t *out_site, uint32_t *out_kind);
uint64_t rt_patch_epoch(void);
uint32_t rt_patched_group_count(void);
uint32_t rt_dyn_group_count(void);
int rt_collect_patched_ranges(uint32_t text_lo, uint32_t text_hi, uint32_t *out_lo, uint32_t *out_hi, int cap);
typedef void (*RtBootHookFn)(CPU *c, uint32_t addr);
void rt_set_boot_hook(uint32_t addr, RtBootHookFn fn);
void rt_run_boot_hook_if_armed(CPU *c, uint32_t addr);
int rt_has_fallback(void);
uint64_t rt_rdtsc(void);
void rt_cpuid(CPU *c);

// SSE lane helpers. Lanes are little-endian 4 x f32 / 8 x u16 inside a U.
static inline float LANEF(U v, int i) { uint32_t b = (uint32_t)(v >> (32 * i)); float f; memcpy(&f, &b, 4); return f; }
static inline U SETF(U v, int i, float f) { uint32_t b; memcpy(&b, &f, 4); U m = (U)0xffffffffu << (32 * i); return (v & ~m) | ((U)b << (32 * i)); }
static inline U MOVMSKPS(U x) { U r = 0; for (int i = 0; i < 4; i++) r |= ((x >> (32 * i + 31)) & 1) << i; return r; }
// x86 MINPS/MAXPS: dest = (a OP b) ? a : b — returns the SECOND operand on NaN or equal.
static inline U MINPS(U a, U b) { U r = a; for (int i = 0; i < 4; i++) { float x = LANEF(a, i), y = LANEF(b, i); r = SETF(r, i, x < y ? x : y); } return r; }
static inline U MAXPS(U a, U b) { U r = a; for (int i = 0; i < 4; i++) { float x = LANEF(a, i), y = LANEF(b, i); r = SETF(r, i, x > y ? x : y); } return r; }
static inline U DIVPS(U a, U b) { U r = a; for (int i = 0; i < 4; i++) r = SETF(r, i, (float)FG(FBIN(/, (U)(uint32_t)(a >> (32 * i)), 4, (U)(uint32_t)(b >> (32 * i)), 4, 4), 4)); return r; }
// RSQRT/RCP: hardware gives a ~12-bit approximation that differs between CPU vendors; the exact
// value is a legal result and is what the reference emulator produces.
static inline U RSQRTPS(U s) { U r = s; for (int i = 0; i < 4; i++) r = SETF(r, i, 1.0f / sqrtf(LANEF(s, i))); return r; }
static inline U RCPPS(U s) { U r = s; for (int i = 0; i < 4; i++) r = SETF(r, i, 1.0f / LANEF(s, i)); return r; }
static inline U RSQRTSS(U d, U s) { return SETF(d, 0, 1.0f / sqrtf(LANEF(s, 0))); }
static inline U PADDUSW(U a, U b) { U r = 0; for (int i = 0; i < 8; i++) { uint32_t x = (uint32_t)(a >> (16 * i)) & 0xffff, y = (uint32_t)(b >> (16 * i)) & 0xffff, z = x + y; r |= (U)(z > 0xffff ? 0xffff : z) << (16 * i); } return r; }

// ---- fast path (narrow values, cached registers) ----------------------------------------------
typedef uint64_t V;
typedef int64_t SV;
static inline V MASKV(int n) { return n >= 8 ? ~(V)0 : (((V)1 << (8 * n)) - 1); }
static inline V LDV(const uint8_t *p, int n) { V v = 0; memcpy(&v, p, n); return v; }
static inline void STV(uint8_t *p, V v, int n) { memcpy(p, &v, n); }
static inline int rt_is_nop2(const uint8_t *p) {
    return (p[0] == 0x66u && p[1] == 0x90u) || (p[0] == 0x90u && p[1] == 0x90u);
}
static inline int rt_is_nop3(const uint8_t *p) {
    return (p[0] == 0x0Fu && p[1] == 0x1Fu && p[2] == 0x00u) ||
           (p[0] == 0x90u && p[1] == 0x90u && p[2] == 0x90u) ||
           (p[0] == 0x66u && p[1] == 0x90u && p[2] == 0x90u) ||
           (p[0] == 0x90u && p[1] == 0x66u && p[2] == 0x90u);
}
static inline int rt_is_valid_call5_bytes(const uint8_t *p) {
    uint8_t op = p[0];
    if (op == 0xE8u) return 1;
    if (op >= 0xB8u && op <= 0xBFu && op != 0xBCu) return 1; // MOV r32, imm32 (not ESP)
    if ((op == 0x90u && p[1] == 0x90u && p[2] == 0x90u && p[3] == 0x90u && p[4] == 0x90u) ||
        (op == 0x0Fu && p[1] == 0x1Fu && p[2] == 0x44u && p[3] == 0x00u && p[4] == 0x00u) ||
        (op == 0x66u && p[1] == 0x0Fu && p[2] == 0x1Fu && p[3] == 0x40u && p[4] == 0x00u)) {
        return 1; // 5-byte NOP
    }
    if ((op == 0x89u || op == 0x8Bu) && p[1] >= 0xC0u) {
        uint8_t dst = (op == 0x8Bu) ? ((p[1] >> 3) & 7u) : (p[1] & 7u);
        return dst != 4u && rt_is_nop3(p + 2);
    }
    if (op == 0x8Bu && (p[1] & 0xC0u) == 0x40u) {
        uint8_t reg = (p[1] >> 3) & 7u, rm = p[1] & 7u;
        return rm != 4u && reg != 4u && rt_is_nop2(p + 3);
    }
    if (op == 0x8Bu && (p[1] & 0xC0u) == 0x00u) {
        uint8_t reg = (p[1] >> 3) & 7u, rm = p[1] & 7u;
        return rm != 4u && rm != 5u && reg != 4u && rt_is_nop3(p + 2);
    }
    return 0;
}
static inline V rt_push_imm8(uint32_t site) {
    return (V)(uint32_t)(int32_t)(int8_t)MEM[site + 1u];
}
static inline V rt_push_imm32(uint32_t site) {
    return (V)(uint32_t)LDV(MEM + site + 1u, 4);
}
static inline SV SXV(V v, int n) { int s = 64 - 8 * n; return (SV)(v << s) >> s; }
static inline V SHLV(V a, V b, int n) { return b >= (V)(8 * n) ? 0 : (a << b) & MASKV(n); }
static inline V SHRV(V a, V b, int n) { return b >= (V)(8 * n) ? 0 : (a & MASKV(n)) >> b; }
static inline V SARV(V a, V b, int n) { int s = b >= (V)(8 * n) ? 8 * n - 1 : (int)b; return (V)(SXV(a, n) >> s) & MASKV(n); }
static inline V SCARRYV(V a, V b, int n) { if (n == 8) { int64_t r; return __builtin_add_overflow((int64_t)a, (int64_t)b, &r); } SV r = SXV(a, n) + SXV(b, n); return r != SXV((V)r, n); }
static inline V SBORROWV(V a, V b, int n) { if (n == 8) { int64_t r; return __builtin_sub_overflow((int64_t)a, (int64_t)b, &r); } SV r = SXV(a, n) - SXV(b, n); return r != SXV((V)r, n); }
static inline double FGV(V v, int n) { if (n == 4) { float f; uint32_t b = (uint32_t)v; memcpy(&f, &b, 4); return f; } double d; memcpy(&d, &v, 8); return d; }
static inline V FPV(double d, int n) { if (n == 4) { float f = (float)d; uint32_t b; memcpy(&b, &f, 4); return b; } V v; memcpy(&v, &d, 8); return v; }
static inline V FTRUNCV(double d, int n) {
    double lim = n == 8 ? 9223372036854775808.0 : n == 4 ? 2147483648.0 : 32768.0;
    if (!(d >= -lim && d < lim)) return ((V)1 << (8 * n - 1)) & MASKV(n);
    return (V)(int64_t)d & MASKV(n);
}
#define FBINV(OP, a, na, b, nb, n) ({ double _x = FGV(a, na), _y = FGV(b, nb); \
    isnan(_x) ? FPV(_x, n) : isnan(_y) ? FPV(_y, n) : FPV(XNAN(_x OP _y), n); })
#ifdef RT_TEST
#define RT_BRANCH(a) do { if (--rt_budget < 0) rt_trap(c, TRAP_BUDGET, (a)); } while (0)
#else
#define RT_BRANCH(a) ((void)0)
#endif

// ---- guest memory with x86 (TSO) ordering ------------------------------------------------------
// Every guest load is an acquire, every guest store a release (LDAPR / STLR on Apple Silicon), so
// the game's lock-free code sees stores in program order like on x86, and spin-loops re-read
// memory. Misaligned and wide accesses use a plain copy bracketed by full fences.
#ifdef RT_NO_TSO   // measurement only: plain accesses (unsafe for the game's lock-free code)
static inline V LDM(uint32_t a, int n) { V v = 0; memcpy(&v, MEM + a, n); return v; }
static inline void STM(uint32_t a, V v, int n) { memcpy(MEM + a, &v, n); }
#else
#if defined(RT_TSO_RELAXED_LOADS)   // measurement: loads plain (still re-read: relaxed atomics)
static inline V LDM(uint32_t a, int n) {
    uint8_t *p = MEM + a;
    switch (n) {
    case 1: return __atomic_load_n(p, __ATOMIC_RELAXED);
    case 2: return __atomic_load_n((uint16_t *)(void *)p, __ATOMIC_RELAXED);
    case 4: return __atomic_load_n((uint32_t *)(void *)p, __ATOMIC_RELAXED);
    case 8: return __atomic_load_n((uint64_t *)(void *)p, __ATOMIC_RELAXED);
    }
    V v = 0; memcpy(&v, p, n); return v;
}
#else
static inline V LDM(uint32_t a, int n) {
    uint8_t *p = MEM + a;
    switch (n) {
    case 1: return __atomic_load_n(p, __ATOMIC_ACQUIRE);
    case 2: if (!(a & 1)) return __atomic_load_n((uint16_t *)(void *)p, __ATOMIC_ACQUIRE); break;
    case 4: if (!(a & 3)) return __atomic_load_n((uint32_t *)(void *)p, __ATOMIC_ACQUIRE); break;
    case 8: if (!(a & 7)) return __atomic_load_n((uint64_t *)(void *)p, __ATOMIC_ACQUIRE); break;
    }
    V v = 0; memcpy(&v, p, n); __atomic_thread_fence(__ATOMIC_ACQUIRE); return v;
}
#endif
#if defined(RT_TSO_RELAXED_STORES)   // measurement: stores plain
static inline void STM(uint32_t a, V v, int n) {
    uint8_t *p = MEM + a;
    switch (n) {
    case 1: __atomic_store_n(p, (uint8_t)v, __ATOMIC_RELAXED); return;
    case 2: __atomic_store_n((uint16_t *)(void *)p, (uint16_t)v, __ATOMIC_RELAXED); return;
    case 4: __atomic_store_n((uint32_t *)(void *)p, (uint32_t)v, __ATOMIC_RELAXED); return;
    case 8: __atomic_store_n((uint64_t *)(void *)p, (uint64_t)v, __ATOMIC_RELAXED); return;
    }
    memcpy(p, &v, n);
}
#else
static inline void STM(uint32_t a, V v, int n) {
    uint8_t *p = MEM + a;
    switch (n) {
    case 1: __atomic_store_n(p, (uint8_t)v, __ATOMIC_RELEASE); return;
    case 2: if (!(a & 1)) { __atomic_store_n((uint16_t *)(void *)p, (uint16_t)v, __ATOMIC_RELEASE); return; } break;
    case 4: if (!(a & 3)) { __atomic_store_n((uint32_t *)(void *)p, (uint32_t)v, __ATOMIC_RELEASE); return; } break;
    case 8: if (!(a & 7)) { __atomic_store_n((uint64_t *)(void *)p, (uint64_t)v, __ATOMIC_RELEASE); return; } break;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE); memcpy(p, &v, n);
}
#endif
#endif
static inline U LDMW(uint32_t a, int n) { U v = 0; memcpy(&v, MEM + a, n); __atomic_thread_fence(__ATOMIC_ACQUIRE); return v; }
static inline void STMW(uint32_t a, U v, int n) { __atomic_thread_fence(__ATOMIC_RELEASE); memcpy(MEM + a, &v, n); }
static inline void rt_call_rel32(CPU *c, uint32_t site, uint32_t orig_target, GuestFn orig_fn) {
    const uint8_t *p = MEM + site;
    uint8_t op = p[0];
    if (__builtin_expect(op == 0xE8u, 1)) {
        int32_t rel = (int32_t)(uint32_t)LDV(p + 1u, 4);
        uint32_t target = site + 5u + (uint32_t)rel;
        if (__builtin_expect(target == orig_target, 1)) orig_fn(c);
        else rt_call(c, target);
        return;
    }
    uint32_t esp = (uint32_t)LDV(c->r + 16, 4);
    STV(c->r + 16, (V)(esp + 4u), 4);
    if (op >= 0xB8u && op <= 0xBFu && op != 0xBCu) {
        STV(c->r + 4u * (op - 0xB8u), LDV(p + 1u, 4), 4);
    } else if ((op == 0x89u || op == 0x8Bu) && p[1] >= 0xC0u) {
        uint8_t reg = (p[1] >> 3) & 7u, rm = p[1] & 7u;
        uint8_t dst = (op == 0x8Bu) ? reg : rm;
        uint8_t src = (op == 0x8Bu) ? rm : reg;
        STV(c->r + 4u * dst, LDV(c->r + 4u * src, 4), 4);
    } else if (op == 0x8Bu && (p[1] & 0xC0u) == 0x40u && (p[1] & 7u) != 4u) {
        uint8_t reg = (p[1] >> 3) & 7u, rm = p[1] & 7u;
        uint32_t ea = (uint32_t)LDV(c->r + 4u * rm, 4) + (uint32_t)(int32_t)(int8_t)p[2];
        STV(c->r + 4u * reg, LDM(ea, 4), 4);
    } else if (op == 0x8Bu && (p[1] & 0xC0u) == 0x00u && (p[1] & 7u) != 4u && (p[1] & 7u) != 5u) {
        uint8_t reg = (p[1] >> 3) & 7u, rm = p[1] & 7u;
        uint32_t ea = (uint32_t)LDV(c->r + 4u * rm, 4);
        STV(c->r + 4u * reg, LDM(ea, 4), 4);
    }
}
void rt_lock(void);
// RT_X87CHECK: report calls / returns made with more x87 values live than the ABI allows
#ifdef RT_X87CHECK
void rt_x87_violation(uint32_t at, int depth, int allowed);
#define X87CHK(at, allowed) do { int _d = (8 - R[REG_TOP]) & 7; if (_d > (allowed)) rt_x87_violation((at), _d, (allowed)); } while (0)
#else
#define X87CHK(at, allowed) ((void)0)
#endif
void rt_unlock(void);

// ---- x87 registers as doubles ----------------------------------------------------------------
// ST0..ST7 live in the register file as 8-byte doubles (offsets 4352 + 16*i). 80-bit extended
// format exists only where x87 data meets guest memory (FLD/FSTP tbyte, FSAVE...).
typedef double F;
static inline F LDF(const uint8_t *p) { F d; memcpy(&d, p, 8); return d; }
static inline void STF(uint8_t *p, F d) { memcpy(p, &d, 8); }
static inline F LDMF(uint32_t a) { F d = f80_get(MEM + a); __atomic_thread_fence(__ATOMIC_ACQUIRE); return d; }
static inline void STMF(uint32_t a, F d) { __atomic_thread_fence(__ATOMIC_RELEASE); f80_put(MEM + a, d); }
static inline U F2U(F d) { U v = 0; uint8_t b[16] = {0}; f80_put(b, d); memcpy(&v, b, 16); return v; }
static inline F U2F(U v) { uint8_t b[16]; memcpy(b, &v, 16); return f80_get(b); }
#define FBIND(OP, x, y) ({ F _x = (x), _y = (y); isnan(_x) ? _x : isnan(_y) ? _y : XNAN(_x OP _y); })

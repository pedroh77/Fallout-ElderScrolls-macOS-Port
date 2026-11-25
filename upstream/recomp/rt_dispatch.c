// Guest-address -> host-function dispatch, plus the platform pieces generated code calls.
#include <mach/mach_time.h>
#include <os/lock.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include "rt.h"

extern const uint32_t rt_fn_addr[];
extern const GuestFn rt_fn_ptr[];
extern const int rt_fn_count;

__attribute__((weak)) const uint32_t rt_fn_group[1] = {0};
__attribute__((weak)) const int rt_fn_group_count = 0;
__attribute__((weak)) uint8_t rt_fn_patched[1] = {0};
__attribute__((weak)) const RtFnRange rt_fn_ranges[1] = {{0, 0, 0}};
__attribute__((weak)) const int rt_fn_range_count = 0;
__attribute__((weak)) const uint32_t rt_fn_range_max_span = 0;
__attribute__((weak)) const uint32_t rt_dyn_sites[1] = {0};
__attribute__((weak)) const int rt_dyn_site_count = 0;
__attribute__((weak)) int rt_has_fallback(void) { return 0; }

// Import thunks (0xFEE00000 + 4*i) belong to the host; the test library has no host, so its
// weak default just traps like any other unknown address.
__attribute__((weak)) void rt_import_call(CPU *c, uint32_t a) { rt_trap(c, TRAP_NOFN, a); }
// No native function for an address: the host interprets it (fallback.c); the test library traps.
__attribute__((weak)) void rt_fallback(CPU *c, uint32_t a) { rt_trap(c, TRAP_NOFN, a); }

static inline int fn_is_patched(int i) {
    if (rt_fn_group_count <= 0) return 0;
    uint32_t gid = rt_fn_group[i];
    return gid < (uint32_t)rt_fn_group_count &&
           __atomic_load_n(&rt_fn_patched[gid], __ATOMIC_RELAXED) == 1u;
}

// O(1) dispatch: one 32-bit slot per byte of the code range holds (function index + 1).
#define DISP_LO 0x00400000u
#define DISP_HI 0x01080000u
static uint32_t *disp;
static void disp_build(void) {
    uint32_t *t = calloc(DISP_HI - DISP_LO, sizeof *t);
    for (int i = 0; i < rt_fn_count; i++) {
        uint32_t a = rt_fn_addr[i];
        if (a >= DISP_LO && a < DISP_HI) t[a - DISP_LO] = (uint32_t)i + 1;
    }
    if (!__sync_bool_compare_and_swap(&disp, NULL, t)) free(t);
}
static inline int fn_index(uint32_t a) {
    if (a >= DISP_LO && a < DISP_HI) {
        if (__builtin_expect(!disp, 0)) disp_build();
        return (int)disp[a - DISP_LO] - 1;
    }
    int lo = 0, hi = rt_fn_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t v = rt_fn_addr[mid];
        if (v == a) return mid;
        if (v < a) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

#ifndef FNV_GAME_SKYRIM
#include <stdio.h>
#include <time.h>
static int rt_trace_voice(void) { static int on = -1; if (on < 0) on = getenv("FNV_TRACE_VOICE") != NULL; return on; }
static uint64_t rt_trace_ms(void) { static uint64_t t0; uint64_t t = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); if (!t0) t0 = t; return (t - t0) / 1000000ull; }
#endif

void rt_call(CPU *c, uint32_t a) {
    if (a >= 0xFEE00000u && a < 0xFEE10000u) { rt_import_call(c, a); return; }
#ifndef FNV_GAME_SKYRIM
    if (__builtin_expect(a == 0x7664d0u || a == 0x766430u || a == 0x7664a0u || a == 0x766510u || a == 0x767660u, 0) && rt_trace_voice()) {
        uint32_t esp = (uint32_t)LD(c->r + 16, 4);
        if (a == 0x767660u) {
            uint32_t actor = (uint32_t)LD(c->r + 4, 4);
            uint32_t fid = actor ? (uint32_t)LDM(actor + 0xc, 4) : 0;
            if (fid == 0x290a7u || fid == 0x5ede0u) {
                uint8_t b7b = MEM[actor + 0x7b], b7c = MEM[actor + 0x7c], b7d = MEM[actor + 0x7d];
                static uint32_t last_dad = 0xffffffffu, last_mom = 0xffffffffu;
                uint32_t *lp = (fid == 0x290a7u) ? &last_dad : &last_mom;
                uint32_t cur = ((uint32_t)b7b << 16) | ((uint32_t)b7c << 8) | b7d;
                if (*lp != cur) {
                    *lp = cur;
                    fprintf(stderr, "[voice-cb +%llums] UpdateDialogue fid=%#x actor=%#x 7b=%u 7c=%#x 7d=%u\n",
                            (unsigned long long)rt_trace_ms(), fid, actor, b7b, b7c, b7d);
                }
            }
        } else {
            uint32_t ra = (uint32_t)LDM(esp, 4), arg0 = (uint32_t)LDM(esp + 4, 4), arg1 = (uint32_t)LDM(esp + 8, 4);
            fprintf(stderr, "[voice-cb +%llums] call %#x ra=%#x arg0=%#x arg1=%#x\n",
                    (unsigned long long)rt_trace_ms(), a, ra, arg0, arg1);
        }
    }
#endif
    int i = fn_index(a);
    if (i >= 0 && !fn_is_patched(i)) { rt_fn_ptr[i](c); return; }
    rt_fallback(c, a);
}

int rt_has_fn(uint32_t a) {
    int i = fn_index(a);
    return i >= 0 && !fn_is_patched(i);
}

static _Atomic uint64_t code_patch_epoch;
static _Atomic uint32_t patched_groups_total;
static _Atomic uint32_t dyn_groups_total;
static _Atomic uint32_t boot_hook_addr;
static _Atomic(RtBootHookFn) boot_hook_fn;

int rt_dyn_site_covers(uint32_t addr, uint32_t *out_site, uint32_t *out_kind) {
    if (rt_dyn_site_count <= 0) return 0;
    int lo = 0, hi = rt_dyn_site_count - 1, found = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint32_t s = RT_SITE_ADDR(rt_dyn_sites[mid]);
        if (s <= addr) { found = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (found < 0) return 0;
    uint32_t entry = rt_dyn_sites[found];
    uint32_t site = RT_SITE_ADDR(entry);
    uint32_t kind = RT_SITE_KIND(entry);
    uint32_t len = (kind == RT_SITE_PUSH2) ? 2u : 5u;
    if (addr >= site && addr < site + len) {
        if (out_site) *out_site = site;
        if (out_kind) *out_kind = kind;
        return 1;
    }
    return 0;
}

void rt_notify_code_patch(uint32_t addr, uint32_t size) {
    if (size > 0 && rt_fn_range_count > 0 && rt_fn_group_count > 0) {
        static int trace_patch = -1;
        if (trace_patch < 0) trace_patch = getenv("FNV_TRACE_PATCH") != NULL;
        uint64_t end = (uint64_t)addr + (uint64_t)size;
        uint32_t lo_bound = addr > rt_fn_range_max_span ? addr - rt_fn_range_max_span : 0u;
        int lo = 0, hi = rt_fn_range_count;
        while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            if (rt_fn_ranges[mid].lo < lo_bound) lo = mid + 1;
            else hi = mid;
        }
        for (int k = lo; k < rt_fn_range_count && (uint64_t)rt_fn_ranges[k].lo < end; k++) {
            if (rt_fn_ranges[k].hi > addr) {
                uint32_t gid = rt_fn_ranges[k].group;
                if (gid < (uint32_t)rt_fn_group_count) {
                    uint8_t prev = __atomic_exchange_n(&rt_fn_patched[gid], 1u, __ATOMIC_RELEASE);
                    if (prev == 0u) {
                        atomic_fetch_add_explicit(&patched_groups_total, 1, memory_order_relaxed);
                        if (trace_patch) {
                            fprintf(stderr, "[patch] deopt group %u ([%#x..%#x) hit by %#x+%#x)\n",
                                    gid, rt_fn_ranges[k].lo, rt_fn_ranges[k].hi, addr, size);
                        }
                    } else if (prev == 2u) {
                        atomic_fetch_add_explicit(&patched_groups_total, 1, memory_order_relaxed);
                        atomic_fetch_sub_explicit(&dyn_groups_total, 1, memory_order_relaxed);
                        if (trace_patch) {
                            fprintf(stderr, "[patch] upgrade group %u (2->1) ([%#x..%#x) hit by %#x+%#x)\n",
                                    gid, rt_fn_ranges[k].lo, rt_fn_ranges[k].hi, addr, size);
                        }
                    }
                }
            }
        }
    }
    atomic_fetch_add_explicit(&code_patch_epoch, 1, memory_order_release);
}
void rt_notify_dyn_patch(uint32_t addr, uint32_t size) {
    if (size > 0 && rt_fn_range_count > 0 && rt_fn_group_count > 0) {
        static int trace_patch = -1;
        if (trace_patch < 0) trace_patch = getenv("FNV_TRACE_PATCH") != NULL;
        uint64_t end = (uint64_t)addr + (uint64_t)size;
        uint32_t lo_bound = addr > rt_fn_range_max_span ? addr - rt_fn_range_max_span : 0u;
        int lo = 0, hi = rt_fn_range_count;
        while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            if (rt_fn_ranges[mid].lo < lo_bound) lo = mid + 1;
            else hi = mid;
        }
        for (int k = lo; k < rt_fn_range_count && (uint64_t)rt_fn_ranges[k].lo < end; k++) {
            if (rt_fn_ranges[k].hi > addr) {
                uint32_t gid = rt_fn_ranges[k].group;
                if (gid < (uint32_t)rt_fn_group_count) {
                    uint8_t exp = 0u;
                    if (__atomic_compare_exchange_n(&rt_fn_patched[gid], &exp, 2u, 0,
                                                    __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
                        atomic_fetch_add_explicit(&dyn_groups_total, 1, memory_order_relaxed);
                        if (trace_patch) {
                            fprintf(stderr, "[patch] dyn group %u ([%#x..%#x) hit by %#x+%#x)\n",
                                    gid, rt_fn_ranges[k].lo, rt_fn_ranges[k].hi, addr, size);
                        }
                    }
                }
            }
        }
    }
    atomic_fetch_add_explicit(&code_patch_epoch, 1, memory_order_release);
}
uint64_t rt_patch_epoch(void) {
    return atomic_load_explicit(&code_patch_epoch, memory_order_acquire);
}
uint32_t rt_patched_group_count(void) {
    return atomic_load_explicit(&patched_groups_total, memory_order_relaxed);
}
uint32_t rt_dyn_group_count(void) {
    return atomic_load_explicit(&dyn_groups_total, memory_order_relaxed);
}
int rt_collect_patched_ranges(uint32_t text_lo, uint32_t text_hi, uint32_t *out_lo, uint32_t *out_hi, int cap) {
    if (text_hi <= text_lo || rt_fn_range_count <= 0 || rt_fn_group_count <= 0) return 0;
    int n = 0;
    for (int i = 0; i < rt_fn_range_count; i++) {
        uint32_t gid = rt_fn_ranges[i].group;
        if (gid >= (uint32_t)rt_fn_group_count) continue;
        if (__atomic_load_n(&rt_fn_patched[gid], __ATOMIC_ACQUIRE) != 1u) continue;
        uint32_t lo = rt_fn_ranges[i].lo;
        uint32_t hi = rt_fn_ranges[i].hi;
        if (hi <= text_lo || lo >= text_hi) continue;
        if (lo < text_lo) lo = text_lo;
        if (hi > text_hi) hi = text_hi;
        if (n > 0 && lo <= out_hi[n - 1]) {
            if (hi > out_hi[n - 1]) out_hi[n - 1] = hi;
        } else {
            if (n >= cap) return -1;
            out_lo[n] = lo;
            out_hi[n] = hi;
            n++;
        }
    }
    return n;
}
void rt_set_boot_hook(uint32_t addr, RtBootHookFn fn) {
    atomic_store_explicit(&boot_hook_addr, addr, memory_order_relaxed);
    atomic_store_explicit(&boot_hook_fn, fn, memory_order_release);
    if (fn != NULL && addr != 0) rt_notify_code_patch(addr, 1);
}
void rt_run_boot_hook_if_armed(CPU *c, uint32_t addr) {
    if (__builtin_expect(atomic_load_explicit(&boot_hook_addr, memory_order_relaxed) != addr, 1)) return;
    RtBootHookFn fn = atomic_exchange_explicit(&boot_hook_fn, (RtBootHookFn)NULL, memory_order_acq_rel);
    if (fn) {
        atomic_store_explicit(&boot_hook_addr, 0, memory_order_relaxed);
        fn(c, addr);
    }
}

#ifdef RT_CALLTRACE
_Thread_local uint32_t rt_ct_ring[256];
_Thread_local uint32_t rt_ct_pos;
#endif
#include <stdio.h>
void rt_calltrace_dump(void) {
#ifdef RT_CALLTRACE
    fprintf(stderr, "[calltrace] last function entries on this thread (oldest first):\n");
    for (uint32_t i = rt_ct_pos > 64 ? rt_ct_pos - 64 : 0; i < rt_ct_pos; i++) fprintf(stderr, " %#x", rt_ct_ring[i & 255]);
    fprintf(stderr, "\n");
#endif
}

uint64_t rt_rdtsc(void) { return mach_absolute_time(); }

// Report a plain SSE2-capable x86 (what the game's 2010 feature checks look for).
void rt_cpuid(CPU *c) {
    uint32_t leaf = (uint32_t)LD(c->r + 0, 4), a = 0, b = 0, cc = 0, d = 0;
    switch (leaf) {
    case 0: a = 1; b = 0x756e6547; d = 0x49656e69; cc = 0x6c65746e; break;   // "GenuineIntel"
    case 1: a = 0x000006fb; b = 0x00040800; cc = 0x00000001; d = 0x078bfbff; break; // SSE..SSE3
    default: break;
    }
    ST(c->r + 0, a, 4); ST(c->r + 12, b, 4); ST(c->r + 4, cc, 4); ST(c->r + 8, d, 4);
}

// global lock for x86 LOCK-prefixed read-modify-write instructions
static os_unfair_lock lock_prefix = OS_UNFAIR_LOCK_INIT;
void rt_lock(void) { os_unfair_lock_lock(&lock_prefix); }
void rt_unlock(void) { os_unfair_lock_unlock(&lock_prefix); }

#ifdef RT_X87CHECK
#include <stdio.h>
void rt_x87_violation(uint32_t at, int depth, int allowed) {
    static uint32_t seen[4096]; static int n;
    for (int i = 0; i < n; i++) if (seen[i] == at) return;
    if (n < 4096) seen[n++] = at;
    fprintf(stderr, "[x87] %s at %#x with %d values on the stack (ABI allows %d)\n", allowed == 1 ? "RETURN" : "CALL", at, depth, allowed);
}
#endif

// kernel32 (and friends) shims, grown one trap at a time. Each shim reads its arguments from the
// guest stack, does the work natively, sets EAX and pops like the real stdcall export.
#include <mach/mach_time.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "host.h"

__attribute__((weak)) uint32_t pe_load_dll(CPU *c, const char *name, const char *dir) { (void)c; (void)name; (void)dir; return 0; }
__attribute__((weak)) uint32_t pe_find_module_by_name(const char *name) { (void)name; return 0; }
__attribute__((weak)) uint32_t pe_find_module_by_addr(uint32_t addr) { (void)addr; return 0; }
__attribute__((weak)) const char *pe_module_win_path(uint32_t mod_base) { (void)mod_base; return NULL; }
__attribute__((weak)) uint32_t pe_get_proc_address(CPU *c, uint32_t mod_base, const char *sym) { (void)c; (void)mod_base; (void)sym; return 0; }
__attribute__((weak)) uint32_t host_bind_import(const char *dll, const char *name) { (void)dll; (void)name; return 0; }
__attribute__((weak)) void rt_notify_code_patch(uint32_t addr, uint32_t size) { (void)addr; (void)size; }
__attribute__((weak)) void rt_notify_dyn_patch(uint32_t addr, uint32_t size) { (void)addr; (void)size; }
__attribute__((weak)) int rt_dyn_site_covers(uint32_t addr, uint32_t *out_site, uint32_t *out_kind) { (void)addr; (void)out_site; (void)out_kind; return 0; }
__attribute__((weak)) uint32_t host_text_start = 0, host_text_end = 0;

const char *host_saved_cmdline;
_Thread_local uint32_t host_last_error;
#define last_error host_last_error
static uint32_t cmdline_a, cmdline_w;
static uint32_t teb_main;

#ifdef FNV_GAME_SKYRIM
static char g_exe_name[128] = "TESV.exe";
static char g_game_dir[64] = "Skyrim";
#elif defined(FNV_GAME_FALLOUT3)
static char g_exe_name[128] = "Fallout3.exe";
static char g_game_dir[64] = "Fallout3";
#else
static char g_exe_name[128] = "FalloutNV.exe";
static char g_game_dir[64] = "FalloutNV";
#endif

const char *host_exe_name(void) { return g_exe_name; }
const char *host_game_dir(void) { return g_game_dir; }

void host_init_args(int argc, char **argv) {
    if (argc > 0 && argv[0] && argv[0][0]) {
        const char *slash = strrchr(argv[0], '/');
        const char *bslash = strrchr(argv[0], '\\');
        const char *base = slash > bslash ? slash + 1 : (bslash ? bslash + 1 : argv[0]);
        snprintf(g_exe_name, sizeof g_exe_name, "%s", base);
        snprintf(g_game_dir, sizeof g_game_dir, "%s",
                 strcasecmp(g_exe_name, "Fallout3.exe") == 0 ? "Fallout3"
                 : strcasecmp(g_exe_name, "TESV.exe") == 0 ? "Skyrim"
                 : "FalloutNV");
    }
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", g_exe_name);
    for (int i = 1; i < argc; i++) { strncat(buf, " ", sizeof buf - strlen(buf) - 1); strncat(buf, argv[i], sizeof buf - strlen(buf) - 1); }
    // allocated lazily (heap needs MEM); stash in a static until first use
    static char saved[4096];
    memcpy(saved, buf, sizeof saved);
    cmdline_a = 0;
    host_saved_cmdline = saved;
}

// ---- thread / TEB / multi-module static TLS --------------------------------------------------
#define TLS_SLOTS 64
#define MAX_LIVE_TEBS 256
static pthread_mutex_t tls_lk = PTHREAD_MUTEX_INITIALIZER;
static struct { uint8_t *tpl; uint32_t tpl_sz, blk_sz; } tls_mods[MAX_TLS_MODULES];
static int tls_mod_count;
static uint32_t live_tebs[MAX_LIVE_TEBS];
static int live_teb_count;

static void tls_populate_slot_locked(uint32_t teb, int idx) {
    if (idx < 0 || idx >= tls_mod_count) return;
    if (G32(teb + 0x1000 + 4u * (uint32_t)idx)) return;
    uint32_t blk_sz = tls_mods[idx].blk_sz ? tls_mods[idx].blk_sz : 4;
    uint32_t blk = gmalloc(blk_sz);
    if (tls_mods[idx].tpl && tls_mods[idx].tpl_sz) {
        memcpy(MEM + blk, tls_mods[idx].tpl, tls_mods[idx].tpl_sz);
    }
    P32(teb + 0x1000 + 4u * (uint32_t)idx, blk);
}

int host_tls_register(uint32_t template_va, uint32_t template_size, uint32_t zero_fill, uint32_t index_va) {
    pthread_mutex_lock(&tls_lk);
    if (tls_mod_count >= MAX_TLS_MODULES) {
        pthread_mutex_unlock(&tls_lk);
        fprintf(stderr, "[host] MAX_TLS_MODULES (%d) exceeded\n", MAX_TLS_MODULES);
        return -1;
    }
    int idx = tls_mod_count++;
    tls_mods[idx].tpl_sz = template_size;
    tls_mods[idx].blk_sz = template_size + zero_fill;
    if (template_size) {
        tls_mods[idx].tpl = malloc(template_size + 1);
        memcpy(tls_mods[idx].tpl, MEM + template_va, template_size);
    } else {
        tls_mods[idx].tpl = NULL;
    }
    if (index_va) P32(index_va, (uint32_t)idx);
    for (int i = 0; i < live_teb_count; i++) {
        tls_populate_slot_locked(live_tebs[i], idx);
    }
    pthread_mutex_unlock(&tls_lk);
    return idx;
}

void host_tls_populate_thread(uint32_t teb) {
    pthread_mutex_lock(&tls_lk);
    if (live_teb_count < MAX_LIVE_TEBS) live_tebs[live_teb_count++] = teb;
    for (int i = 0; i < tls_mod_count; i++) {
        tls_populate_slot_locked(teb, i);
    }
    if (!tls_mod_count && host_tls_template) {
        uint32_t blk = gmalloc(host_tls_block_size ? host_tls_block_size : 4);
        memcpy(MEM + blk, host_tls_template, host_tls_template_size);
        P32(teb + 0x1000, blk);
    }
    pthread_mutex_unlock(&tls_lk);
}

void host_thread_init(CPU *c, uint32_t stack_size) {
    uint32_t stack = gpages(stack_size);
    uint32_t teb = gpages(0x2000);
    P32(teb + 0x00, 0xFFFFFFFFu);              // SEH chain end
    P32(teb + 0x04, stack + stack_size);       // stack top
    P32(teb + 0x08, stack);                    // stack limit
    P32(teb + 0x18, teb);                      // self
    P32(teb + 0x20, (uint32_t)getpid());       // ClientId.UniqueProcess
    static uint32_t next_tid = 1;
    P32(teb + 0x24, __atomic_fetch_add(&next_tid, 1, __ATOMIC_RELAXED));   // ClientId.UniqueThread
    P32(teb + 0x2C, teb + 0x1000);             // ThreadLocalStoragePointer (static TLS array)
    host_tls_populate_thread(teb);
    P32(teb + 0x30, gpages(0x1000));           // PEB
    SETREG(c, 272, teb);                       // FS_OFFSET
    SETREG(c, R_ESP, stack + stack_size - 64);
    { uint16_t fcw = 0x037Fu; uint32_t mxcsr = 0x1F80u; memcpy(c->r + 4256, &fcw, 2); memcpy(c->r + 4244, &mxcsr, 4); }
    if (!teb_main) teb_main = teb;
}

// ---- shims -----------------------------------------------------------------------------------
#define SHIM(n) static void n(CPU *c)

SHIM(GetLastError_) { RET(c, last_error, 0); }
SHIM(SetLastError_) { last_error = ARG(c, 0); RET(c, 0, 1); }
SHIM(GetVersion_) { RET(c, 0x0A280105u, 0); }            // "Windows XP SP2-ish": 5.1 build 2600
SHIM(GetVersionExA_) {
    uint32_t v = ARG(c, 0);
    P32(v + 4, 5); P32(v + 8, 1); P32(v + 12, 2600); P32(v + 16, 2);
    RET(c, 1, 1);
}
SHIM(GetCommandLineA_) {
    if (!cmdline_a) { uint32_t n = (uint32_t)strlen(host_saved_cmdline) + 1; cmdline_a = gmalloc(n); memcpy(MEM + cmdline_a, host_saved_cmdline, n); }
    RET(c, cmdline_a, 0);
}
SHIM(GetCommandLineW_) {
    if (!cmdline_w) { uint32_t n = (uint32_t)strlen(host_saved_cmdline) + 1; cmdline_w = gmalloc(2 * n);
        for (uint32_t i = 0; i < n; i++) { uint16_t ch = (uint8_t)host_saved_cmdline[i]; memcpy(MEM + cmdline_w + 2 * i, &ch, 2); } }
    RET(c, cmdline_w, 0);
}
// ---- modules: system DLLs get fake handles; GetProcAddress hands out dynamic import thunks ----
#define MOD_BASE 0xFD000000u
#ifdef FNV_GAME_SKYRIM
static const char *known_mods[] = {"kernel32", "user32", "gdi32", "advapi32", "shell32", "ole32",
    "ntdll", "d3d9", "dinput8", "dsound", "xinput1_3", "xaudio2_6", "x3daudio1_7", "winmm", "comctl32",
    "wsock32", "ws2_32", "d3dx9_42", "d3dx9_38", "binkw32", "libvorbisfile", "steam_api", "atimgpud", "shlwapi",
    "version", "mscoree", "msvcrt", "imagehlp"};
#else
static const char *known_mods[] = {"kernel32", "user32", "gdi32", "advapi32", "shell32", "ole32",
    "ntdll", "d3d9", "dinput8", "dsound", "xinput1_3", "winmm", "comctl32", "wsock32", "ws2_32", "d3dx9_38", "d3dx9_42",
    "binkw32", "libvorbisfile", "steam_api", "xlive", "shlwapi", "version", "mscoree", "msvcrt", "imagehlp"};
#endif
#define NMODS (sizeof known_mods / sizeof *known_mods)

static uint32_t mod_handle(const char *name) {
    if (!name) return host_image_base;
    char low[128]; size_t n = 0;
    const char *s1 = strrchr(name, '\\'), *s2 = strrchr(name, '/');
    const char *start = s1 > s2 ? s1 + 1 : (s2 ? s2 + 1 : name);
    for (const char *p = start; *p && n < sizeof low - 1; p++)
        low[n++] = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
    low[n] = 0;
    char *dot = strstr(low, ".dll"); if (dot && dot[4] == 0) *dot = 0;
    if (!strcmp(low, "falloutnv") || !strcmp(low, "fallout3") ||
        !strcmp(low, "tesv") || !strcmp(low, "tesv.exe") || !strcmp(low, "skyrim"))
        return host_image_base;
    if (!strncmp(low, "api-ms-win-core-", 16) || !strncmp(low, "api-ms-win-security-", 20) ||
        !strncmp(low, "api-ms-win-eventing-", 20) || !strncmp(low, "ext-ms-win-", 11) ||
        !strcmp(low, "kernelbase"))
        return MOD_BASE;
    for (size_t i = 0; i < NMODS; i++) if (!strcmp(low, known_mods[i])) return MOD_BASE + 0x10000u * (uint32_t)i;
    return 0;
}
static uint32_t lookup_mod_handle(const char *name) {
    uint32_t h = mod_handle(name);
    if (!h && name) h = pe_find_module_by_name(name);
    return h;
}
static void wide_to_ascii(uint32_t w, char *out, size_t n) {
    size_t i = 0; uint16_t ch;
    for (; i < n - 1; i++) { memcpy(&ch, MEM + w + 2 * i, 2); if (!ch) break; out[i] = (char)ch; }
    out[i] = 0;
}
SHIM(GetModuleHandleA_) { uint32_t h = lookup_mod_handle(GSTR(ARG(c, 0))); if (!h) last_error = 126; RET(c, h, 1); }
SHIM(GetModuleHandleW_) {
    char nm[256]; uint32_t a = ARG(c, 0);
    if (a) wide_to_ascii(a, nm, sizeof nm);
    uint32_t h = lookup_mod_handle(a ? nm : NULL); if (!h) last_error = 126; RET(c, h, 1);
}
SHIM(LoadLibraryA_) {
    const char *nm = GSTR(ARG(c, 0)); uint32_t h = mod_handle(nm);
    if (!h && nm) h = pe_load_dll(c, nm, NULL);
    fprintf(stderr, "[host] LoadLibraryA(%s) -> %#x\n", nm ? nm : "(null)", h);
    if (!h) last_error = 126; RET(c, h, 1);
}
SHIM(LoadLibraryW_) {
    char nm[256]; wide_to_ascii(ARG(c, 0), nm, sizeof nm); uint32_t h = mod_handle(nm);
    if (!h) h = pe_load_dll(c, nm, NULL);
    fprintf(stderr, "[host] LoadLibraryW(%s) -> %#x\n", nm, h);
    if (!h) last_error = 126; RET(c, h, 1);
}
SHIM(LoadLibraryExA_) {
    const char *nm = GSTR(ARG(c, 0)); uint32_t h = mod_handle(nm);
    if (!h && nm) h = pe_load_dll(c, nm, NULL);
    fprintf(stderr, "[host] LoadLibraryExA(%s) -> %#x\n", nm ? nm : "(null)", h);
    if (!h) last_error = 126; RET(c, h, 3);
}
SHIM(LoadLibraryExW_) {
    char nm[256]; wide_to_ascii(ARG(c, 0), nm, sizeof nm); uint32_t h = mod_handle(nm);
    if (!h) h = pe_load_dll(c, nm, NULL);
    fprintf(stderr, "[host] LoadLibraryExW(%s) -> %#x\n", nm, h);
    if (!h) last_error = 126; RET(c, h, 3);
}
SHIM(GetModuleHandleExA_) {
    uint32_t fl = ARG(c, 0), name = ARG(c, 1), out = ARG(c, 2);
    uint32_t h = 0;
    if (fl & 4u) { h = pe_find_module_by_addr(name); if (!h) h = host_image_base; }
    else if (!name) h = host_image_base;
    else h = lookup_mod_handle(GSTR(name));
    if (out) P32(out, h);
    if (!h) last_error = 126;
    RET(c, h != 0, 3);
}
SHIM(GetModuleHandleExW_) {
    uint32_t fl = ARG(c, 0), name = ARG(c, 1), out = ARG(c, 2);
    uint32_t h = 0;
    if (fl & 4u) { h = pe_find_module_by_addr(name); if (!h) h = host_image_base; }
    else if (!name) h = host_image_base;
    else { char nm[256]; wide_to_ascii(name, nm, sizeof nm); h = lookup_mod_handle(nm); }
    if (out) P32(out, h);
    if (!h) last_error = 126;
    RET(c, h != 0, 3);
}
SHIM(FreeLibrary_) { RET(c, 1, 1); }
SHIM(DisableThreadLibraryCalls_) { RET(c, 1, 1); }
SHIM(GetProcAddress_) {
    uint32_t h = ARG(c, 0), pn = ARG(c, 1);
    char name[96];
    if (pn < 0x10000) snprintf(name, sizeof name, "#%u", pn); else snprintf(name, sizeof name, "%s", GSTR(pn));
    if (h && h != host_image_base && h < MOD_BASE) {
        uint32_t pe_fn = pe_get_proc_address(c, h, name);
        if (!pe_fn) { fprintf(stderr, "[host] GetProcAddress(mod=%#x, %s) -> 0\n", h, name); last_error = 127; }
        RET(c, pe_fn, 2);
        return;
    }
    const char *dll = h >= MOD_BASE && (h - MOD_BASE) / 0x10000u < NMODS ? known_mods[(h - MOD_BASE) / 0x10000u] : "?";
    ShimFn fn = host_find_shim(dll, name);
    if (!fn) { fprintf(stderr, "[host] GetProcAddress(%s, %s) -> 0 (no shim)\n", dll, name); last_error = 127; RET(c, 0, 2); return; }
    RET(c, host_bind_import(dll, name), 2);
}
SHIM(GetModuleFileNameA_) {
    uint32_t h = ARG(c, 0), buf = ARG(c, 1), n = ARG(c, 2);
    char name[512];
    const char *mod_wp = (h && h != host_image_base) ? pe_module_win_path(h) : NULL;
    if (mod_wp) snprintf(name, sizeof name, "%s", mod_wp);
    else snprintf(name, sizeof name, "C:\\Games\\%s\\%s", g_game_dir, g_exe_name);
    uint32_t l = (uint32_t)strlen(name);
    if (n) { if (l >= n) l = n - 1; memcpy(MEM + buf, name, l); MEM[buf + l] = 0; }
    RET(c, l, 3);
}
SHIM(GetModuleFileNameW_) {
    uint32_t h = ARG(c, 0), buf = ARG(c, 1), n = ARG(c, 2);
    char name[512];
    const char *mod_wp = (h && h != host_image_base) ? pe_module_win_path(h) : NULL;
    if (mod_wp) snprintf(name, sizeof name, "%s", mod_wp);
    else snprintf(name, sizeof name, "C:\\Games\\%s\\%s", g_game_dir, g_exe_name);
    uint32_t l = (uint32_t)strlen(name);
    if (n) {
        if (l >= n) l = n - 1;
        for (uint32_t i = 0; i <= l; i++) { uint16_t ch = i < l ? (uint8_t)name[i] : 0; memcpy(MEM + buf + 2 * i, &ch, 2); }
    }
    RET(c, l, 3);
}
SHIM(GetSystemDirectoryW_) {
    uint32_t buf = ARG(c, 0), n = ARG(c, 1);
    const char *sys = "C:\\Windows\\System32";
    uint32_t l = (uint32_t)strlen(sys);
    if (n > l) {
        for (uint32_t i = 0; i <= l; i++) { uint16_t ch = i < l ? (uint8_t)sys[i] : 0; memcpy(MEM + buf + 2 * i, &ch, 2); }
        RET(c, l, 2);
    } else RET(c, l + 1, 2);
}
SHIM(LocalAlloc_) {
    uint32_t fl = ARG(c, 0), n = ARG(c, 1);
    uint32_t p = gmalloc(n ? n : 1);
    if (p && (fl & 0x40u)) memset(MEM + p, 0, n ? n : 1);
    RET(c, p, 2);
}
SHIM(VerifyVersionInfoW_) { RET(c, 1, 3); }
SHIM(VerSetConditionMask_) { SETREG(c, R_EDX, 0); RET(c, 0, 4); }
SHIM(CreateProcessA_) { last_error = 2; RET(c, 0, 10); }
SHIM(CreateProcessW_) { last_error = 2; RET(c, 0, 10); }
SHIM(GetCurrentProcess_) { RET(c, 0xFFFFFFFFu, 0); }
SHIM(GetCurrentThread_) { RET(c, 0xFFFFFFFEu, 0); }
SHIM(GetCurrentProcessId_) { RET(c, (uint32_t)getpid(), 0); }
SHIM(GetCurrentThreadId_) { RET(c, G32(REG(c, 272) + 0x24) ? G32(REG(c, 272) + 0x24) : 1, 0); }
SHIM(GetTickCount_) { RET(c, (uint32_t)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000ull), 0); }
static pthread_once_t qpc_once = PTHREAD_ONCE_INIT;
static uint64_t qpc_t0_ns;
static void qpc_init(void) { qpc_t0_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
SHIM(QueryPerformanceCounter_) {
    pthread_once(&qpc_once, qpc_init);
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    /* 10 MHz counter with a 60s startup bias avoids (QPC * 1e6) uint64 overflow after ~5.1h uptime
       and keeps initial delta checks non-zero across all threads. */
    uint64_t t = 600000000ull + (now - qpc_t0_ns) / 100ull;
    memcpy(MEM + ARG(c, 0), &t, 8); RET(c, 1, 1);
}
SHIM(QueryPerformanceFrequency_) { uint64_t f = 10000000ull; memcpy(MEM + ARG(c, 0), &f, 8); RET(c, 1, 1); }
SHIM(GetSystemTimeAsFileTime_) {
    struct timeval tv; gettimeofday(&tv, NULL);
    uint64_t ft = ((uint64_t)tv.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)tv.tv_usec * 10;
    memcpy(MEM + ARG(c, 0), &ft, 8); RET(c, 0, 1);
}
SHIM(IsProcessorFeaturePresent_) { uint32_t f = ARG(c, 0); RET(c, f == 6 || f == 7 || f == 10 || f == 13, 1); } // SSE/SSE2/3DNow!-no/SSE3
SHIM(IsDebuggerPresent_) { RET(c, 0, 0); }

// heaps: one guest heap backs all of them
SHIM(HeapCreate_) { RET(c, 0x00F00000u, 3); }
SHIM(GetProcessHeap_) { RET(c, 0x00F00000u, 0); }
SHIM(HeapDestroy_) { RET(c, 1, 1); }
SHIM(HeapAlloc_) { uint32_t f = ARG(c, 1), n = ARG(c, 2); uint32_t p = gmalloc(n); (void)f; RET(c, p, 3); }
SHIM(HeapFree_) { gfree(ARG(c, 2)); RET(c, 1, 3); }
SHIM(HeapReAlloc_) { RET(c, grealloc(ARG(c, 2), ARG(c, 3)), 4); }
SHIM(HeapSize_) { RET(c, gsize(ARG(c, 2)), 3); }
SHIM(HeapCompact_) { RET(c, 0, 2); }
SHIM(HeapValidate_) { RET(c, 1, 3); }
SHIM(HeapQueryInformation_) { last_error = 120; RET(c, 0, 5); }
SHIM(HeapWalk_) { last_error = 259; RET(c, 0, 2); }
SHIM(RtlAllocateHeap_) { uint32_t f = ARG(c, 1), n = ARG(c, 2); uint32_t p = gmalloc(n); (void)f; RET(c, p, 3); }
SHIM(RtlFreeHeap_) { gfree(ARG(c, 2)); RET(c, 1, 3); }
SHIM(RtlReAllocateHeap_) { RET(c, grealloc(ARG(c, 2), ARG(c, 3)), 4); }
SHIM(RtlSizeHeap_) { RET(c, gsize(ARG(c, 2)), 3); }
// VirtualAlloc keeps a list of reserved ranges so a fixed-address reservation that overlaps the
// image or an earlier reservation fails (ERROR_INVALID_ADDRESS) exactly like on Windows; the game's
// allocator probes fixed addresses and falls back when they are taken.
#define MAXRES 4096
static struct { uint32_t lo, hi; int exec; } res[MAXRES];
static int nres;
static int overlaps(uint32_t lo, uint32_t hi) {
    for (int i = 0; i < nres; i++) if (lo < res[i].hi && res[i].lo < hi) return i;
    return -1;
}
static void reserve_ex(uint32_t lo, uint32_t hi, int exec) {
    if (nres < MAXRES) { res[nres].lo = lo; res[nres].hi = hi; res[nres].exec = exec; nres++; }
}
static void ensure_base_reserved(void) {
    if (!nres) {
        reserve_ex(0, 0x10000, 0);
        uint32_t pe_off = G32(host_image_base + 0x3C);
        uint32_t img_sz = pe_off ? G32(host_image_base + pe_off + 24 + 56) : 0x1000000u;
        reserve_ex(host_image_base, host_image_base + img_sz, 0);
    }
}
#include <pthread.h>
static pthread_mutex_t va_lk = PTHREAD_MUTEX_INITIALIZER;
int gheap_reserve_fixed(uint32_t lo,uint32_t hi);
int host_reserve_guest_range(uint32_t lo, uint32_t hi) {
    if (lo >= hi) return 0;
    pthread_mutex_lock(&va_lk);
    ensure_base_reserved();
    int ok = overlaps(lo, hi) < 0 && nres < MAXRES && gheap_reserve_fixed(lo, hi);
    if (ok) reserve_ex(lo, hi, 0);
    pthread_mutex_unlock(&va_lk);
    return ok;
}
int host_addr_in_exec_va(uint32_t addr) {
    pthread_mutex_lock(&va_lk);
    int hit = 0;
    for (int i = 2; i < nres; i++) {
        if (res[i].exec && addr >= res[i].lo && addr < res[i].hi) { hit = 1; break; }
    }
    pthread_mutex_unlock(&va_lk);
    return hit;
}
static void VirtualAlloc_locked(CPU *c);
SHIM(VirtualAlloc_) { pthread_mutex_lock(&va_lk); VirtualAlloc_locked(c); pthread_mutex_unlock(&va_lk); }
static void VirtualAlloc_locked(CPU *c) {
    uint32_t a = ARG(c, 0), n = ARG(c, 1), type = ARG(c, 2), prot = ARG(c, 3);
    int is_exec = (prot & 0xF0u) != 0;
    if(!n || (uint64_t)a+n>0x100000000ull || !(type&0x3000)){last_error=87;RET(c,0,4);return;}
    ensure_base_reserved();
    uint64_t end=((uint64_t)a+n+4095)&~4095ull;
    if(end>0xffffffffu || n>0xeffff000u){last_error=87;RET(c,0,4);return;}
    if (!a) { if(nres==MAXRES){last_error=8;RET(c,0,4);return;}uint32_t p = gpages(n); reserve_ex(p, p + ((n + 0xFFF) & ~0xFFFu), is_exec); RET(c, p, 4); return; }
    uint32_t lo = a & ((type&0x2000)?~0xffffu:~0xfffu), hi = (uint32_t)end;
    int i = overlaps(lo, hi);
    if (type & 0x2000) {                        // MEM_RESERVE at a fixed address
        int ok=i<0&&nres<MAXRES&&gheap_reserve_fixed(lo,hi);
        fprintf(stderr,"[address-map] fixed request=%#x bytes=%#x span=%#x..%#x result=%#x caller=%#x\n",a,n,lo,hi,ok?lo:0,G32(REG(c,R_ESP)));
        if (!ok) { last_error = 487; RET(c, 0, 4); return; }
        reserve_ex(lo, hi, is_exec); RET(c, lo, 4); return;
    }
    // A commit must be wholly inside an existing reservation, including already committed pages.
    if(i<0 || lo<res[i].lo || hi>res[i].hi){
        fprintf(stderr,"[address-map] rejected unreserved commit %#x..%#x caller=%#x\n",lo,hi,G32(REG(c,R_ESP)));
        last_error=487;RET(c,0,4);return;
    }
    if (is_exec) res[i].exec = 1;
    RET(c, lo, 4);
}
SHIM(VirtualFree_) {
    uint32_t a = ARG(c, 0), n = ARG(c, 1), type = ARG(c, 2);
    if ((type & 0x4000) && a && n && (uint64_t)a + n <= 0x100000000ull) memset(MEM + a, 0, n);
    RET(c, 1, 3);
}
static pthread_mutex_t text_patch_lk = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *orig_text, *synced_text;
static uint32_t text_snap_base, text_snap_len;
static int text_dirty, text_bulk_unlocked;

static int range_overlaps_text(uint32_t addr, uint32_t sz) {
    return host_text_end > host_text_start && sz > 0 &&
           (uint64_t)addr < host_text_end && (uint64_t)addr + sz > host_text_start;
}

static void ensure_text_snapshots_locked(void) {
    if (orig_text || host_text_end <= host_text_start) return;
    text_snap_base = host_text_start;
    text_snap_len = host_text_end - host_text_start;
    orig_text = (uint8_t *)malloc(text_snap_len);
    synced_text = (uint8_t *)malloc(text_snap_len);
    if (orig_text) memcpy(orig_text, MEM + text_snap_base, text_snap_len);
    if (synced_text) memcpy(synced_text, MEM + text_snap_base, text_snap_len);
}

static inline uint8_t expected_site_op(uint32_t kind) {
    if (kind == RT_SITE_PUSH2) return 0x6Au;
    if (kind == RT_SITE_PUSH5) return 0x68u;
    return 0xE8u;
}

static int range_covered_by_dyn_sites_locked(uint32_t addr, uint32_t sz) {
    if (!orig_text || !text_snap_len || sz == 0) return 0;
    uint64_t lo = addr, hi = (uint64_t)addr + sz;
    uint64_t base = text_snap_base, end = (uint64_t)text_snap_base + text_snap_len;
    if (lo < base) lo = base;
    if (hi > end) hi = end;
    if (lo >= hi) return 0;
    for (uint64_t p = lo; p < hi; p++) {
        uint32_t site = 0, kind = 0;
        if (!rt_dyn_site_covers((uint32_t)p, &site, &kind)) return 0;
        if (site < text_snap_base || site - text_snap_base >= text_snap_len) return 0;
        if (orig_text[site - text_snap_base] != expected_site_op(kind)) return 0;
    }
    return 1;
}

static void classify_written_span_locked(uint32_t va_lo, uint32_t va_hi) {
    if (!orig_text || !synced_text || !text_snap_len) return;
    uint64_t lo = va_lo, hi = va_hi;
    uint64_t base = text_snap_base, end = (uint64_t)text_snap_base + text_snap_len;
    if (lo < base) lo = base;
    if (hi > end) hi = end;
    if (lo >= hi) return;
    memcpy(synced_text + (lo - base), MEM + lo, (size_t)(hi - lo));
    for (uint64_t p = lo; p < hi; p++) {
        if (MEM[p] == orig_text[p - base]) continue;
        uint32_t site = 0, kind = 0;
        if (rt_dyn_site_covers((uint32_t)p, &site, &kind) &&
            site >= base && site - base < text_snap_len &&
            orig_text[site - base] == expected_site_op(kind)) {
            if (kind == RT_SITE_CALL5) {
                if ((uint64_t)site + 5u <= end && rt_is_valid_call5_bytes(MEM + site)) {
                    rt_notify_dyn_patch((uint32_t)p, 1u);
                    continue;
                }
            } else if (p > site && MEM[site] == expected_site_op(kind)) {
                rt_notify_dyn_patch((uint32_t)p, 1u);
                continue;
            }
        }
        rt_notify_code_patch((uint32_t)p, 1u);
    }
}

static void sync_text_patches_locked(void) {
    if (!orig_text || !synced_text || !text_snap_len || (!text_dirty && !text_bulk_unlocked)) return;
    const uint8_t *cur = MEM + text_snap_base;
    uint32_t i = 0;
    while (i < text_snap_len) {
        while (i + 8 <= text_snap_len) {
            uint64_t a, b;
            memcpy(&a, cur + i, 8);
            memcpy(&b, synced_text + i, 8);
            if (a != b) break;
            i += 8;
        }
        while (i < text_snap_len && cur[i] == synced_text[i]) i++;
        if (i >= text_snap_len) break;
        uint32_t start = i;
        while (i < text_snap_len) {
            if (cur[i] != synced_text[i]) { i++; continue; }
            uint32_t same = i;
            while (same < text_snap_len && same - i < 8 && cur[same] == synced_text[same]) same++;
            if (same - i >= 8 || same >= text_snap_len) break;
            i = same;
        }
        classify_written_span_locked(text_snap_base + start, text_snap_base + i);
    }
    if (!text_bulk_unlocked) text_dirty = 0;
}

void host_sync_text_patches(void) {
    pthread_mutex_lock(&text_patch_lk);
    sync_text_patches_locked();
    pthread_mutex_unlock(&text_patch_lk);
}

SHIM(VirtualProtect_) {
    uint32_t addr = ARG(c, 0), sz = ARG(c, 1), prot = ARG(c, 2), old_p = ARG(c, 3);
    if (!old_p) { last_error = 87; RET(c, 0, 4); return; }
    P32(old_p, 0x40 /* PAGE_EXECUTE_READWRITE */);
    int writable = (prot & 0xCCu) != 0;
    if (prot & 0xF0u) {
        uint64_t end = (uint64_t)addr + sz;
        pthread_mutex_lock(&va_lk);
        for (int i = 2; i < nres; i++) {
            if ((uint64_t)addr < res[i].hi && res[i].lo < end) res[i].exec = 1;
        }
        pthread_mutex_unlock(&va_lk);
    }
    if (range_overlaps_text(addr, sz)) {
        pthread_mutex_lock(&text_patch_lk);
        ensure_text_snapshots_locked();
        if (sz > 0x10000u) {
            text_bulk_unlocked = 1;
            text_dirty = 1;
            sync_text_patches_locked();
        } else {
            uint64_t hi = (uint64_t)addr + sz;
            uint32_t hi32 = hi > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)hi;
            classify_written_span_locked(addr, hi32);
            if (writable) {
                if (range_covered_by_dyn_sites_locked(addr, sz)) {
                    rt_notify_dyn_patch(addr, sz);
                    text_dirty = 1;
                } else {
                    rt_notify_code_patch(addr, sz);
                }
            }
        }
        pthread_mutex_unlock(&text_patch_lk);
    } else {
        rt_notify_code_patch(addr, sz);
    }
    RET(c, 1, 4);
}
SHIM(FlushInstructionCache_) {
    uint32_t addr = ARG(c, 1), sz = ARG(c, 2);
    if (range_overlaps_text(addr, sz)) {
        pthread_mutex_lock(&text_patch_lk);
        ensure_text_snapshots_locked();
        if (sz > 0x10000u) {
            sync_text_patches_locked();
        } else {
            uint64_t hi = (uint64_t)addr + sz;
            classify_written_span_locked(addr, hi > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)hi);
        }
        pthread_mutex_unlock(&text_patch_lk);
    } else {
        rt_notify_code_patch(addr, sz);
    }
    RET(c, 1, 3);
}
SHIM(VirtualQuery_) {
    uint32_t addr = ARG(c, 0), buf = ARG(c, 1), len = ARG(c, 2);
    if (!buf || len < 28) { last_error = 24; RET(c, 0, 3); return; }
    uint32_t page = addr & ~0xFFFu;
    memset(MEM + buf, 0, 28);
    P32(buf + 0, page);
    uint32_t pe_off = G32(host_image_base + 0x3C);
    uint32_t img_end = host_image_base + (pe_off ? G32(host_image_base + pe_off + 24 + 56) : 0x1000000u);
    if (page >= host_image_base && page < img_end) {
        P32(buf + 4, host_image_base);
        P32(buf + 8, 0x40);
        P32(buf + 12, img_end - page);
        P32(buf + 16, 0x1000);
        P32(buf + 20, 0x40);
        P32(buf + 24, 0x1000000);
        RET(c, 28, 3);
        return;
    }
    uint32_t mod_base = pe_find_module_by_addr(addr);
    if (mod_base) {
        P32(buf + 4, mod_base);
        P32(buf + 8, 0x40);
        P32(buf + 12, 0x1000);
        P32(buf + 16, 0x1000);
        P32(buf + 20, 0x40);
        P32(buf + 24, 0x1000000);
        RET(c, 28, 3);
        return;
    }
    pthread_mutex_lock(&va_lk);
    ensure_base_reserved();
    int i = overlaps(page, page + 0x1000u);
    uint32_t alloc_base = i >= 0 ? res[i].lo : (page & ~0xFFFFu);
    uint32_t reg_sz = i >= 0 ? (res[i].hi - page) : 0x1000u;
    pthread_mutex_unlock(&va_lk);
    P32(buf + 4, alloc_base);
    P32(buf + 8, 0x40);
    P32(buf + 12, reg_sz);
    P32(buf + 16, (i >= 0 || (page >= 0x40000000u && page < 0xF0000000u)) ? 0x1000u : 0x10000u);
    P32(buf + 20, 0x40);
    P32(buf + 24, 0x20000);
    RET(c, 28, 3);
}

// TLS / FLS
static uint32_t tls_next;
SHIM(TlsAlloc_) { RET(c, tls_next < TLS_SLOTS ? tls_next++ : 0xFFFFFFFFu, 0); }
SHIM(TlsFree_) { RET(c, 1, 1); }
SHIM(TlsGetValue_) { uint32_t i = ARG(c, 0); last_error = 0; RET(c, G32(REG(c, 272) + 0xE10 + 4 * i), 1); }
SHIM(TlsSetValue_) { uint32_t i = ARG(c, 0); P32(REG(c, 272) + 0xE10 + 4 * i, ARG(c, 1)); RET(c, 1, 2); }
SHIM(FlsAlloc_) { (void)ARG(c, 0); RET(c, tls_next < TLS_SLOTS ? tls_next++ : 0xFFFFFFFFu, 1); }
SHIM(FlsFree_) { RET(c, 1, 1); }
SHIM(FlsGetValue_) { uint32_t i = ARG(c, 0); last_error = 0; RET(c, G32(REG(c, 272) + 0xE10 + 4 * i), 1); }
SHIM(FlsSetValue_) { uint32_t i = ARG(c, 0); P32(REG(c, 272) + 0xE10 + 4 * i, ARG(c, 1)); RET(c, 1, 2); }

// environment / code pages / startup info
SHIM(GetEnvironmentStringsW_) { uint32_t p = gmalloc(4); RET(c, p, 0); }
SHIM(GetEnvironmentStrings_) { uint32_t p = gmalloc(2); RET(c, p, 0); }
SHIM(FreeEnvironmentStringsW_) { RET(c, 1, 1); }
SHIM(FreeEnvironmentStringsA_) { RET(c, 1, 1); }
SHIM(GetACP_) { RET(c, 1252, 0); }
SHIM(GetOEMCP_) { RET(c, 437, 0); }
SHIM(GetCPInfo_) { uint32_t p = ARG(c, 1); memset(MEM + p, 0, 20); P32(p, 1); MEM[p + 4] = '?'; RET(c, 1, 2); }
SHIM(IsValidCodePage_) { RET(c, 1, 1); }
SHIM(GetStartupInfoA_) { uint32_t p = ARG(c, 0); memset(MEM + p, 0, 68); P32(p, 68); RET(c, 0, 1); }
SHIM(GetStartupInfoW_) { uint32_t p = ARG(c, 0); memset(MEM + p, 0, 68); P32(p, 68); RET(c, 0, 1); }
SHIM(GetStdHandle_) { RET(c, 0x10 + (ARG(c, 0) & 3), 1); }
SHIM(GetFileType_) { RET(c, 2, 1); }                      // FILE_TYPE_CHAR
SHIM(SetHandleCount_) { RET(c, ARG(c, 0), 1); }
SHIM(SetUnhandledExceptionFilter_) { RET(c, 0, 1); }
SHIM(UnhandledExceptionFilter_) { RET(c, 0, 1); }
SHIM(SetConsoleCtrlHandler_) { RET(c, 1, 2); }
SHIM(SetThreadAffinityMask_) { (void)ARG(c, 0); RET(c, 1, 2); }
SHIM(IsBadWritePtr_) { uint32_t p = ARG(c, 0), sz = ARG(c, 1); RET(c, sz != 0 && p == 0, 2); }
SHIM(OutputDebugStringA_) { fprintf(stderr, "[guest] %s", GSTR(ARG(c, 0))); RET(c, 0, 1); }
SHIM(OutputDebugStringW_) { char s[512]; wide_to_ascii(ARG(c, 0), s, sizeof s); fprintf(stderr, "[guest] %s", s); RET(c, 0, 1); }
SHIM(EncodePointer_) { RET(c, ARG(c, 0), 1); }
SHIM(DecodePointer_) { RET(c, ARG(c, 0), 1); }
SHIM(Sleep_) {
    uint32_t ms = ARG(c, 0);
    if (ms == 0xFFFFFFFFu) { fprintf(stderr, "[host] Sleep(INFINITE) — guest waits forever\n"); exit(4); }
    static int trace = -1;
    if (trace < 0) trace = getenv("FNV_TRACE_SLEEP") != NULL;
    if (trace) {                                             // histogram of (caller, ms) every 2000 calls
        static uint64_t n; static uint32_t hist[64][3]; static int nh;
        uint32_t ra = G32(REG(c, R_ESP));
        int i; for (i = 0; i < nh && !(hist[i][0] == ra && hist[i][1] == ms); i++) {}
        if (i == nh && nh < 64) { hist[nh][0] = ra; hist[nh][1] = ms; nh++; }
        if (i < 64) hist[i][2]++;
        if (++n % 2000 == 0) { for (int k = 0; k < nh; k++) fprintf(stderr, "[sleep] ra=%#x ms=%u n=%u\n", hist[k][0], hist[k][1], hist[k][2]); }
    }
    usleep((useconds_t)ms * 1000);
    RET(c, 0, 1);
}
SHIM(GetTickCount64_) {
    uint64_t ms = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000ull;
    SETREG(c, R_EDX, (uint32_t)(ms >> 32));
    RET(c, (uint32_t)ms, 0);
}
SHIM(GetCurrentProcessorNumber_) { RET(c, 0, 0); }
SHIM(FlushProcessWriteBuffers_) { __atomic_thread_fence(__ATOMIC_SEQ_CST); RET(c, 0, 0); }
SHIM(SwitchToThread_) { sched_yield(); RET(c, 1, 0); }
SHIM(SetErrorMode_) {
    static uint32_t err_mode;
    uint32_t prev = __atomic_exchange_n(&err_mode, ARG(c, 0), __ATOMIC_RELAXED);
    RET(c, prev, 1);
}
SHIM(GetExitCodeProcess_) { if (ARG(c, 1)) P32(ARG(c, 1), 0); RET(c, 1, 2); }
SHIM(AreFileApisANSI_) { RET(c, 1, 0); }
SHIM(DuplicateHandle_) {
    uint32_t src = ARG(c, 1), out = ARG(c, 3);
    if (out) P32(out, src);
    RET(c, 1, 7);
}
SHIM(AllocConsole_) { RET(c, 1, 0); }
SHIM(AttachConsole_) { RET(c, 1, 1); }
SHIM(SetConsoleMode_) { RET(c, 1, 2); }
SHIM(Beep_) { RET(c, 1, 2); }
SHIM(ReadConsoleInputA_) { if (ARG(c, 3)) P32(ARG(c, 3), 0); RET(c, 0, 4); }
SHIM(ReadConsoleInputW_) { if (ARG(c, 3)) P32(ARG(c, 3), 0); RET(c, 0, 4); }
SHIM(PeekConsoleInputA_) { if (ARG(c, 3)) P32(ARG(c, 3), 0); RET(c, 0, 4); }
SHIM(GetNumberOfConsoleInputEvents_) { if (ARG(c, 1)) P32(ARG(c, 1), 0); RET(c, 1, 2); }
SHIM(RtlCaptureContext_) {
    uint32_t ctx = ARG(c, 0);
    if (ctx) {
        memset(MEM + ctx, 0, 716);
        P32(ctx + 0x00, 0x10007u);
        P32(ctx + 0x9C, REG(c, 28));
        P32(ctx + 0xA0, REG(c, 24));
        P32(ctx + 0xA4, REG(c, 12));
        P32(ctx + 0xA8, REG(c, 8));
        P32(ctx + 0xAC, REG(c, 4));
        P32(ctx + 0xB0, REG(c, 0));
        P32(ctx + 0xB4, REG(c, 20));
        P32(ctx + 0xB8, G32(REG(c, R_ESP)));
        P32(ctx + 0xC4, REG(c, R_ESP) + 4);
    }
    RET(c, 0, 1);
}
SHIM(RtlCaptureStackBackTrace_) { RET(c, 0, 4); }
SHIM(ExitProcess_) {
    fprintf(stderr, "[host] ExitProcess(%u) — guest stack (code-looking words):\n", ARG(c, 0));
    uint32_t sp = REG(c, R_ESP);
    for (int i = 0, shown = 0; i < 4096 && shown < 40; i++) {
        uint32_t v = G32(sp + 4 * (uint32_t)i);
        if (v >= 0x401000 && v < 0x1200000) { fprintf(stderr, "  [esp+%#x] %#x\n", 4 * i, v); shown++; }
    }
    exit((int)ARG(c, 0));
}

SHIM(GlobalAlloc_) {
    uint32_t flags = ARG(c, 0), sz = ARG(c, 1);
    uint32_t p = gmalloc(sz ? sz : 4);
    if (p && (flags & 0x40u) && sz) memset(MEM + p, 0, sz);
    RET(c, p, 2);
}
SHIM(GlobalFree_) { uint32_t p = ARG(c, 0); if (p) gfree(p); RET(c, 0, 1); }
SHIM(GlobalLock_) { RET(c, ARG(c, 0), 1); }
SHIM(GlobalUnlock_) { RET(c, 0, 1); }
SHIM(GetSystemDirectoryA_) {
    uint32_t buf = ARG(c, 0), sz = ARG(c, 1);
    const char *s = "C:\\Windows\\System32";
    uint32_t len = (uint32_t)strlen(s);
    if (buf && sz) {
        uint32_t copy = len < sz ? len : sz - 1;
        memcpy(MEM + buf, s, copy);
        MEM[buf + copy] = 0;
    }
    RET(c, len, 2);
}
SHIM(SetThreadDescription_) { RET(c, 0, 2); }
SHIM(K32GetModuleInformation_) {
    uint32_t mod = ARG(c, 1), out = ARG(c, 2), cb = ARG(c, 3);
    if (!out || cb < 12) { last_error = 87; RET(c, 0, 4); return; }
    uint32_t base = mod ? mod : host_image_base;
    uint32_t pe = G32(base + 0x3C);
    uint32_t ep_rva = pe ? G32(base + pe + 24 + 16) : 0;
    uint32_t img_sz = pe ? G32(base + pe + 24 + 56) : 0x1000u;
    P32(out + 0, base);
    P32(out + 4, img_sz);
    P32(out + 8, ep_rva ? base + ep_rva : base);
    RET(c, 1, 4);
}

typedef struct { const char *name; ShimFn fn; } Shim;
static const Shim kernel32[] = {
#define E(n) { #n, n##_ },
    E(GetLastError) E(SetLastError) E(GetVersion) E(GetVersionExA) E(GetCommandLineA) E(GetCommandLineW)
    E(GetModuleHandleA) E(GetModuleHandleW) E(GetModuleHandleExA) E(GetModuleHandleExW) E(GetModuleFileNameA) E(GetModuleFileNameW)
    E(GetSystemDirectoryW) E(LoadLibraryA) E(LoadLibraryW) E(LoadLibraryExA) E(LoadLibraryExW)
    E(FreeLibrary) E(DisableThreadLibraryCalls) E(GetProcAddress) E(GetCurrentProcess) E(GetCurrentThread)
    E(GetCurrentProcessId) E(GetCurrentThreadId) E(GetTickCount) E(GetTickCount64) E(QueryPerformanceCounter)
    E(QueryPerformanceFrequency) E(GetSystemTimeAsFileTime) E(IsProcessorFeaturePresent) E(IsDebuggerPresent)
    E(HeapCreate) E(GetProcessHeap) E(HeapDestroy) E(HeapAlloc) E(HeapFree) E(HeapReAlloc) E(HeapSize)
    E(HeapCompact) E(HeapValidate) E(HeapQueryInformation) E(HeapWalk)
    E(RtlAllocateHeap) E(RtlFreeHeap) E(RtlReAllocateHeap) E(RtlSizeHeap)
    E(LocalAlloc)
    E(VirtualAlloc) E(VirtualFree) E(VirtualProtect) E(VirtualQuery) E(FlushInstructionCache)
    E(TlsAlloc) E(TlsFree) E(TlsGetValue) E(TlsSetValue)
    E(FlsAlloc) E(FlsFree) E(FlsGetValue) E(FlsSetValue)
    E(GetEnvironmentStringsW) E(GetEnvironmentStrings) E(FreeEnvironmentStringsW) E(FreeEnvironmentStringsA)
    E(GetACP) E(GetOEMCP) E(GetCPInfo) E(IsValidCodePage) E(GetStartupInfoA) E(GetStartupInfoW)
    E(GetStdHandle) E(GetFileType) E(SetHandleCount) E(SetUnhandledExceptionFilter)
    E(UnhandledExceptionFilter) E(SetConsoleCtrlHandler) E(OutputDebugStringA) E(OutputDebugStringW)
    E(AllocConsole) E(AttachConsole) E(SetConsoleMode) E(Beep)
    E(ReadConsoleInputA) E(ReadConsoleInputW) E(PeekConsoleInputA) E(GetNumberOfConsoleInputEvents)
    E(GetCurrentProcessorNumber) E(FlushProcessWriteBuffers) E(SwitchToThread) E(SetErrorMode)
    E(GetExitCodeProcess) E(AreFileApisANSI) E(DuplicateHandle)
    E(RtlCaptureContext) E(RtlCaptureStackBackTrace)
    E(Sleep) E(ExitProcess) E(EncodePointer) E(DecodePointer)
    E(SetThreadAffinityMask) E(IsBadWritePtr)
    E(GlobalAlloc) E(GlobalFree) E(GlobalLock) E(GlobalUnlock)
    E(GetSystemDirectoryA) E(SetThreadDescription) E(K32GetModuleInformation)
    E(VerifyVersionInfoW) E(VerSetConditionMask) E(CreateProcessA) E(CreateProcessW)
#undef E
};

typedef struct { const Shim *list; int count; } ShimTable;
#ifdef FNV_GAME_SKYRIM
extern const ShimTable sync_shims, file_shims, text_shims, win_shims, steam_shims, wsock_shims, dinput_shims, dsound_shims, d3d9_shims, d3dx_shims, xinput_shims, media_shims, mmio_shims, xaudio_shims, imagehlp_shims;
#else
extern const ShimTable sync_shims, file_shims, text_shims, win_shims, steam_shims, xlive_shims, wsock_shims, dinput_shims, dsound_shims, d3d9_shims, d3dx_shims, xinput_shims, media_shims, mmio_shims, imagehlp_shims;
#endif
ShimFn host_find_shim(const char *dll, const char *name) {
    // ordinal imports collide across DLLs: look them up as "<dll basename, lowercase>#<n>"
    char key[160];
    if (name[0] == '#') {
        size_t k = 0;
        for (const char *p = dll; *p && *p != '.' && k < sizeof key - 1; p++) key[k++] = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
        key[k] = 0;
        strncat(key, name, sizeof key - strlen(key) - 1);
        name = key;
    }
    for (size_t i = 0; i < sizeof kernel32 / sizeof *kernel32; i++)
        if (!strcmp(kernel32[i].name, name)) return kernel32[i].fn;
#ifdef FNV_GAME_SKYRIM
    const ShimTable *tabs[] = {&sync_shims, &file_shims, &text_shims, &win_shims, &steam_shims, &wsock_shims, &dinput_shims, &dsound_shims, &d3d9_shims, &d3dx_shims, &xinput_shims, &media_shims, &mmio_shims, &xaudio_shims, &imagehlp_shims};
#else
    const ShimTable *tabs[] = {&sync_shims, &file_shims, &text_shims, &win_shims, &steam_shims, &xlive_shims, &wsock_shims, &dinput_shims, &dsound_shims, &d3d9_shims, &d3dx_shims, &xinput_shims, &media_shims, &mmio_shims, &imagehlp_shims};
#endif
    for (size_t t = 0; t < sizeof tabs / sizeof *tabs; t++)
        for (int i = 0; i < tabs[t]->count; i++)
            if (!strcmp(tabs[t]->list[i].name, name)) return tabs[t]->list[i].fn;
    return NULL;
}

int host_shimmed_count(void) {
    int n = 0;
    for (int i = 0; i < host_import_count; i++) n += host_imports[i].fn != NULL;
    return n;
}

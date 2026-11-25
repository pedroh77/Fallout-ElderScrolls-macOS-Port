// Generic 32-bit x86 PE DLL loader for guest address space.
// Maps DLLs into [0x10000000, 0x3F000000), applies base relocations, registers multi-module
// static TLS, binds imports recursively (including API-set redirection and export forwarding),
// and executes TLS callbacks + DllMain(DLL_PROCESS_ATTACH) via rt_call (Unicorn fallback).
#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "pe_loader.h"
#include "shims.h"

static LoadedModule modules[MAX_LOADED_MODULES];
static _Atomic int module_count;
static uint32_t next_dll_base = 0x18000000u;
static pthread_mutex_t pe_lock = PTHREAD_MUTEX_INITIALIZER;

#define MAX_SEARCH_DIRS 32
static char extra_search_dirs[MAX_SEARCH_DIRS][1024];
static int extra_search_count;

void pe_add_search_dir(const char *host_dir) {
    if (!host_dir || !host_dir[0]) return;
    pthread_mutex_lock(&pe_lock);
    for (int i = 0; i < extra_search_count; i++) {
        if (!strcmp(extra_search_dirs[i], host_dir)) {
            pthread_mutex_unlock(&pe_lock);
            return;
        }
    }
    if (extra_search_count < MAX_SEARCH_DIRS) {
        snprintf(extra_search_dirs[extra_search_count++], 1024, "%s", host_dir);
    }
    pthread_mutex_unlock(&pe_lock);
}

static void str_lower_copy(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    for (; src && src[i] && i + 1 < cap; i++) {
        dst[i] = (char)tolower((unsigned char)src[i]);
    }
    dst[i] = 0;
}

static const char *path_basename(const char *p) {
    if (!p) return "";
    const char *s1 = strrchr(p, '/');
    const char *s2 = strrchr(p, '\\');
    const char *s = s1 > s2 ? s1 : s2;
    return s ? s + 1 : p;
}

static int trace_pe(void) {
    static int t = -1;
    if (t < 0) t = getenv("FNV_TRACE_PE") != NULL || getenv("FNV_TRACE") != NULL;
    return t;
}

static int is_shimmed_system_dll(const char *low_noext) {
    static const char *const sys_dlls[] = {
        "kernel32", "kernelbase", "user32", "gdi32", "advapi32", "shell32", "ole32",
        "ntdll", "d3d9", "dinput8", "dsound", "xinput1_3", "xaudio2_6", "x3daudio1_7",
        "winmm", "comctl32", "wsock32", "ws2_32", "d3dx9_38", "d3dx9_42", "d3dx9_43",
        "binkw32", "libvorbisfile", "steam_api", "xlive", "shlwapi", "version",
        "mscoree", "imagehlp", "dbghelp", "bcrypt", "crypt32", "rpcrt4", "secur32",
        "oleaut32", "imm32", "setupapi", "winhttp", "wininet", "psapi", "userenv", "gdiplus"
    };
    for (size_t i = 0; i < sizeof sys_dlls / sizeof *sys_dlls; i++) {
        if (!strcmp(low_noext, sys_dlls[i])) return 1;
    }
    return 0;
}

// Returns 0 if routed to host shims (shim_dll receives DLL name without .dll),
// or 1 if routed to a real PE DLL (pe_name receives basename with .dll).
static int pe_route_dll_name(const char *in, char *shim_dll, size_t shim_cap, char *pe_name, size_t pe_cap) {
    char base[128], noext[128];
    str_lower_copy(base, sizeof base, path_basename(in));
    snprintf(noext, sizeof noext, "%s", base);
    char *dot = strstr(noext, ".dll");
    if (dot && dot[4] == 0) *dot = 0;

    if (!strncmp(noext, "api-ms-win-crt-", 15)) {
        snprintf(pe_name, pe_cap, "ucrtbase.dll");
        return 1;
    }
    if (!strncmp(noext, "api-ms-win-core-", 16) ||
        !strncmp(noext, "api-ms-win-security-", 20) ||
        !strncmp(noext, "api-ms-win-eventing-", 20) ||
        !strncmp(noext, "ext-ms-win-", 11) ||
        !strcmp(noext, "kernelbase")) {
        snprintf(shim_dll, shim_cap, "kernel32");
        return 0;
    }
    if (is_shimmed_system_dll(noext)) {
        snprintf(shim_dll, shim_cap, "%s", noext);
        return 0;
    }
    if (!strstr(base, ".dll")) snprintf(pe_name, pe_cap, "%s.dll", base);
    else snprintf(pe_name, pe_cap, "%s", base);
    return 1;
}

static LoadedModule *find_mod_by_name_locked(const char *name) {
    char low[128];
    str_lower_copy(low, sizeof low, path_basename(name));
    if (!strstr(low, ".dll") && strlen(low) + 4 < sizeof low) strcat(low, ".dll");
    int n = atomic_load_explicit(&module_count, memory_order_relaxed);
    for (int i = 0; i < n; i++) {
        if (!strcmp(modules[i].name, low)) return &modules[i];
    }
    return NULL;
}

static LoadedModule *find_mod_by_base_locked(uint32_t base) {
    int n = atomic_load_explicit(&module_count, memory_order_relaxed);
    for (int i = 0; i < n; i++) {
        if (modules[i].base == base) return &modules[i];
    }
    return NULL;
}

uint32_t pe_find_module_by_name(const char *name) {
    if (!name) return 0;
    char shim_dll[128], pe_name[128];
    if (!pe_route_dll_name(name, shim_dll, sizeof shim_dll, pe_name, sizeof pe_name)) return 0;
    pthread_mutex_lock(&pe_lock);
    LoadedModule *m = find_mod_by_name_locked(pe_name);
    uint32_t base = m ? m->base : 0;
    pthread_mutex_unlock(&pe_lock);
    return base;
}

uint32_t pe_find_module_by_addr(uint32_t addr) {
    int n = atomic_load_explicit(&module_count, memory_order_acquire);
    for (int i = 0; i < n; i++) {
        if (addr >= modules[i].base && addr < modules[i].base + modules[i].size) {
            return modules[i].base;
        }
    }
    return 0;
}

int pe_addr_in_module(uint32_t addr) {
    return pe_find_module_by_addr(addr) != 0;
}

const char *pe_module_win_path(uint32_t mod_base) {
    pthread_mutex_lock(&pe_lock);
    LoadedModule *m = find_mod_by_base_locked(mod_base);
    const char *p = m ? m->win_path : NULL;
    pthread_mutex_unlock(&pe_lock);
    return p;
}

static int try_candidate(const char *cand, char *out, size_t n) {
    host_existing_path_case(cand, out, n);
    return access(out, R_OK) == 0;
}

static int locate_dll_file(const char *name_or_path, const char *pe_name, const char *parent_host_dir, char *out, size_t n) {
    if (strchr(name_or_path, '\\') || strchr(name_or_path, '/') || (name_or_path[0] && name_or_path[1] == ':')) {
        host_path(name_or_path, out, n);
        if (access(out, R_OK) == 0) return 1;
    }
    char cand[1200];
    if (parent_host_dir && parent_host_dir[0]) {
        snprintf(cand, sizeof cand, "%s/%s", parent_host_dir, pe_name);
        if (try_candidate(cand, out, n)) return 1;
    }
    for (int i = 0; i < extra_search_count; i++) {
        snprintf(cand, sizeof cand, "%s/%s", extra_search_dirs[i], pe_name);
        if (try_candidate(cand, out, n)) return 1;
    }
    const char *env_dirs = getenv("FNV_DLL_PATH");
    if (env_dirs && env_dirs[0]) {
        char buf[2048];
        snprintf(buf, sizeof buf, "%s", env_dirs);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ":", &save); tok; tok = strtok_r(NULL, ":", &save)) {
            if (!tok[0]) continue;
            snprintf(cand, sizeof cand, "%s/%s", tok, pe_name);
            if (try_candidate(cand, out, n)) return 1;
        }
    }
    const char *root = getenv("FNV_ROOT");
    if (!root || !root[0]) root = ".";
    const char *subdirs[] = {"", "/NVSE/runtime", "/NVSE", "/Data/NVSE/Plugins"};
    for (size_t i = 0; i < sizeof subdirs / sizeof *subdirs; i++) {
        snprintf(cand, sizeof cand, "%s%s/%s", root, subdirs[i], pe_name);
        if (try_candidate(cand, out, n)) return 1;
    }
    return 0;
}

static void build_win_path(const char *host_file, const char *pe_name, char *out, size_t n) {
    const char *pdir = getenv("FNV_NVSE_PLUGINS_DIR");
    size_t plen = pdir && pdir[0] ? strlen(pdir) : 0;
    if (plen && !strncmp(host_file, pdir, plen) && (host_file[plen] == '/' || host_file[plen] == 0)) {
        const char *rel = host_file + plen;
        if (*rel == '/') rel++;
        char tmp[512];
        size_t k = (size_t)snprintf(tmp, sizeof tmp, "C:\\Games\\%s\\Data\\NVSE\\Plugins\\", host_game_dir());
        for (size_t i = 0; rel[i] && k + 1 < sizeof tmp; i++) {
            tmp[k++] = rel[i] == '/' ? '\\' : rel[i];
        }
        tmp[k] = 0;
        snprintf(out, n, "%s", tmp);
        return;
    }
    const char *root = getenv("FNV_ROOT");
    size_t rlen = root && root[0] ? strlen(root) : 0;
    if (rlen && !strncmp(host_file, root, rlen) && (host_file[rlen] == '/' || host_file[rlen] == 0)) {
        const char *rel = host_file + rlen;
        if (*rel == '/') rel++;
        char tmp[512];
        size_t k = (size_t)snprintf(tmp, sizeof tmp, "C:\\Games\\%s\\", host_game_dir());
        for (size_t i = 0; rel[i] && k + 1 < sizeof tmp; i++) {
            tmp[k++] = rel[i] == '/' ? '\\' : rel[i];
        }
        tmp[k] = 0;
        snprintf(out, n, "%s", tmp);
        return;
    }
    snprintf(out, n, "C:\\Games\\%s\\%s", host_game_dir(), pe_name);
}

static uint32_t pe_call_stdcall3(CPU *c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
    if (!c) {
        fprintf(stderr, "[pe] cannot invoke %#x without CPU context\n", fn);
        return 0;
    }
    uint32_t esp = REG(c, R_ESP);
    uint32_t sp = esp - 64;
    P32(sp, HOST_EXIT_THUNK);
    P32(sp + 4, a0);
    P32(sp + 8, a1);
    P32(sp + 12, a2);
    SETREG(c, R_ESP, sp);
    rt_call(c, fn);
    uint32_t eax = REG(c, R_EAX);
    SETREG(c, R_ESP, esp);
    return eax;
}

static uint16_t rd16_buf(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t rd32_buf(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t g16(uint32_t a) { uint16_t v; memcpy(&v, MEM + a, 2); return v; }

static uint32_t resolve_export_internal(CPU *c, LoadedModule *m, const char *name_or_ord, int depth) {
    if (!m || !m->exp_rva || depth > 16) return 0;
    uint32_t base = m->base;
    uint32_t exp = base + m->exp_rva;
    uint32_t ord_base = G32(exp + 0x10);
    uint32_t nfuncs = G32(exp + 0x14);
    uint32_t nnames = G32(exp + 0x18);
    uint32_t addr_funcs = G32(exp + 0x1C);
    uint32_t addr_names = G32(exp + 0x20);
    uint32_t addr_ords = G32(exp + 0x24);

    uint32_t fn_rva = 0;
    if (name_or_ord[0] == '#') {
        uint32_t ord = (uint32_t)strtoul(name_or_ord + 1, NULL, 10);
        if (ord < ord_base || ord - ord_base >= nfuncs) return 0;
        fn_rva = G32(base + addr_funcs + 4u * (ord - ord_base));
    } else {
        uint32_t lo = 0, hi = nnames;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            const char *sym = (const char *)MEM + base + G32(base + addr_names + 4u * mid);
            int cmp = strcmp(sym, name_or_ord);
            if (cmp == 0) {
                uint16_t ord_idx = g16(base + addr_ords + 2u * mid);
                if (ord_idx < nfuncs) fn_rva = G32(base + addr_funcs + 4u * ord_idx);
                break;
            }
            if (cmp < 0) lo = mid + 1;
            else hi = mid;
        }
        if (!fn_rva) {
            for (uint32_t i = 0; i < nnames; i++) {
                const char *sym = (const char *)MEM + base + G32(base + addr_names + 4u * i);
                if (!strcmp(sym, name_or_ord)) {
                    uint16_t ord_idx = g16(base + addr_ords + 2u * i);
                    if (ord_idx < nfuncs) fn_rva = G32(base + addr_funcs + 4u * ord_idx);
                    break;
                }
            }
        }
    }
    if (!fn_rva) return 0;

    if (fn_rva >= m->exp_rva && fn_rva < m->exp_rva + m->exp_sz) {
        const char *fwd = (const char *)MEM + base + fn_rva;
        const char *dot = strrchr(fwd, '.');
        if (!dot || dot == fwd) return 0;
        char fwd_dll[128], fwd_sym[128];
        size_t dlen = (size_t)(dot - fwd);
        if (dlen >= sizeof fwd_dll) dlen = sizeof fwd_dll - 1;
        memcpy(fwd_dll, fwd, dlen);
        fwd_dll[dlen] = 0;
        snprintf(fwd_sym, sizeof fwd_sym, "%s", dot + 1);

        char shim_dll[128], pe_name[128];
        if (!pe_route_dll_name(fwd_dll, shim_dll, sizeof shim_dll, pe_name, sizeof pe_name)) {
            return host_bind_import(shim_dll, fwd_sym);
        }
        uint32_t dep_base = pe_load_dll(c, pe_name, m->host_dir);
        if (!dep_base) return 0;
        pthread_mutex_lock(&pe_lock);
        LoadedModule *dm = find_mod_by_base_locked(dep_base);
        pthread_mutex_unlock(&pe_lock);
        return resolve_export_internal(c, dm, fwd_sym, depth + 1);
    }
    return base + fn_rva;
}

uint32_t pe_get_proc_address(CPU *c, uint32_t mod_base, const char *name_or_ord) {
    if (!mod_base || !name_or_ord) return 0;
    pthread_mutex_lock(&pe_lock);
    LoadedModule *m = find_mod_by_base_locked(mod_base);
    pthread_mutex_unlock(&pe_lock);
    if (!m) return 0;
    return resolve_export_internal(c, m, name_or_ord, 0);
}

static uint32_t alloc_dll_base(uint32_t preferred, uint32_t size) {
    uint32_t aligned_sz = (size + 0xFFFu) & ~0xFFFu;
    if (preferred >= 0x10000000u && preferred <= 0x3E000000u && (uint64_t)preferred + aligned_sz <= 0x3F000000ull) {
        uint32_t lo = preferred & ~0xFFFFu;
        if (lo == preferred && host_reserve_guest_range(lo, lo + aligned_sz)) return lo;
    }
    pthread_mutex_lock(&pe_lock);
    uint32_t cand = next_dll_base;
    while (cand + aligned_sz <= 0x3F000000u) {
        if (host_reserve_guest_range(cand, cand + aligned_sz)) {
            next_dll_base = (cand + aligned_sz + 0xFFFFu) & ~0xFFFFu;
            pthread_mutex_unlock(&pe_lock);
            return cand;
        }
        cand = (cand + 0x10000u) & ~0xFFFFu;
    }
    pthread_mutex_unlock(&pe_lock);
    return 0;
}

int pe_can_locate_dll(const char *name_or_path) {
    if (!name_or_path || !name_or_path[0]) return 0;
    char shim_dll[128], pe_name[128], host_file[1200];
    if (!pe_route_dll_name(name_or_path, shim_dll, sizeof shim_dll, pe_name, sizeof pe_name)) {
        return 0;
    }
    return locate_dll_file(name_or_path, pe_name, NULL, host_file, sizeof host_file);
}

uint32_t pe_load_dll(CPU *c, const char *name_or_path, const char *parent_host_dir) {
    if (!name_or_path || !name_or_path[0]) return 0;
    char shim_dll[128], pe_name[128];
    if (!pe_route_dll_name(name_or_path, shim_dll, sizeof shim_dll, pe_name, sizeof pe_name)) {
        return 0;
    }

    pthread_mutex_lock(&pe_lock);
    LoadedModule *existing = find_mod_by_name_locked(pe_name);
    if (existing) {
        existing->refcount++;
        uint32_t base = existing->base;
        pthread_mutex_unlock(&pe_lock);
        return base;
    }
    pthread_mutex_unlock(&pe_lock);

    char host_file[1200];
    if (!locate_dll_file(name_or_path, pe_name, parent_host_dir, host_file, sizeof host_file)) {
        if (trace_pe()) fprintf(stderr, "[pe] DLL not found: %s (as %s)\n", name_or_path, pe_name);
        return 0;
    }

    FILE *f = fopen(host_file, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsz < 0x100) { fclose(f); return 0; }
    uint8_t *raw = malloc((size_t)fsz);
    if (!raw || fread(raw, 1, (size_t)fsz, f) != (size_t)fsz) {
        fclose(f); free(raw); return 0;
    }
    fclose(f);

    if (raw[0] != 'M' || raw[1] != 'Z') { free(raw); return 0; }
    uint32_t pe = rd32_buf(raw + 0x3C);
    if (pe + 24 + 96 + 16 * 8 > (uint32_t)fsz || memcmp(raw + pe, "PE\0\0", 4) != 0) {
        free(raw); return 0;
    }
    if (rd16_buf(raw + pe + 4) != 0x014Cu) {
        fprintf(stderr, "[pe] %s is not x86-32 (machine=%#x)\n", host_file, rd16_buf(raw + pe + 4));
        free(raw);
        return 0;
    }

    uint16_t nsec = rd16_buf(raw + pe + 6);
    uint16_t osz = rd16_buf(raw + pe + 20);
    uint32_t ep_rva = rd32_buf(raw + pe + 24 + 16);
    uint32_t pref_base = rd32_buf(raw + pe + 24 + 28);
    uint32_t img_sz = rd32_buf(raw + pe + 24 + 56);
    uint32_t hdr_sz = rd32_buf(raw + pe + 24 + 60);
    uint32_t exp_rva = rd32_buf(raw + pe + 24 + 96 + 0);
    uint32_t exp_sz = rd32_buf(raw + pe + 24 + 96 + 4);
    uint32_t imp_rva = rd32_buf(raw + pe + 24 + 96 + 8);
    uint32_t reloc_rva = rd32_buf(raw + pe + 24 + 96 + 5 * 8);
    uint32_t reloc_sz = rd32_buf(raw + pe + 24 + 96 + 5 * 8 + 4);
    uint32_t tls_rva = rd32_buf(raw + pe + 24 + 96 + 9 * 8);
    uint32_t lc_rva = rd32_buf(raw + pe + 24 + 96 + 10 * 8);

    uint32_t base = alloc_dll_base(pref_base, img_sz);
    if (!base) {
        fprintf(stderr, "[pe] out of guest DLL address space loading %s\n", pe_name);
        free(raw);
        return 0;
    }

    memset(MEM + base, 0, img_sz);
    if (hdr_sz > (uint32_t)fsz) hdr_sz = (uint32_t)fsz;
    memcpy(MEM + base, raw, hdr_sz);

    for (uint16_t i = 0; i < nsec; i++) {
        uint8_t *s = raw + pe + 24 + osz + 40 * i;
        uint32_t vs = rd32_buf(s + 8);
        uint32_t va = rd32_buf(s + 12);
        uint32_t rs = rd32_buf(s + 16);
        uint32_t rp = rd32_buf(s + 20);
        uint32_t take = (rs < vs || !vs) ? rs : vs;
        if (take && rp + take <= (uint32_t)fsz && va + take <= img_sz) {
            memcpy(MEM + base + va, raw + rp, take);
        }
    }
    free(raw);

    if (base != pref_base && reloc_rva && reloc_sz) {
        uint32_t delta = base - pref_base;
        uint32_t off = 0;
        while (off + 8 <= reloc_sz) {
            uint32_t page_rva = G32(base + reloc_rva + off);
            uint32_t blk_sz = G32(base + reloc_rva + off + 4);
            if (blk_sz < 8 || off + blk_sz > reloc_sz) break;
            for (uint32_t j = 8; j + 2 <= blk_sz; j += 2) {
                uint16_t e = g16(base + reloc_rva + off + j);
                uint16_t type = e >> 12;
                uint16_t r = e & 0x0FFFu;
                if (type == 0) continue;
                if (type == 3) {
                    uint32_t p = base + page_rva + r;
                    P32(p, G32(p) + delta);
                } else {
                    fprintf(stderr, "[pe] unsupported reloc type %u in %s\n", type, pe_name);
                    return 0;
                }
            }
            off += blk_sz;
        }
    }

    if (lc_rva && lc_rva + 0x40 <= img_sz) {
        uint32_t lc_struct_sz = G32(base + lc_rva);
        if (lc_struct_sz >= 0x40) {
            uint32_t cookie_va = G32(base + lc_rva + 0x3C);
            if (cookie_va >= base && cookie_va + 4 <= base + img_sz) {
                uint32_t cur = G32(cookie_va);
                if (cur == 0xBB40E64Eu || cur == 0) {
                    uint32_t cookie = 0x5A7C1235u ^ base ^ (uint32_t)getpid();
                    if (cookie == 0 || cookie == 0xBB40E64Eu) cookie ^= 0x13579BDFu;
                    P32(cookie_va, cookie);
                    if (cookie_va + 8 <= base + img_sz && G32(cookie_va + 4) == ~0xBB40E64Eu) {
                        P32(cookie_va + 4, ~cookie);
                    }
                }
            }
        }
    }

    int tls_idx = -1;
    uint32_t tls_cb_va = 0;
    if (tls_rva) {
        uint32_t td[5];
        memcpy(td, MEM + base + tls_rva, sizeof td);
        uint32_t tpl_sz = td[1] > td[0] ? td[1] - td[0] : 0;
        tls_idx = host_tls_register(td[0], tpl_sz, td[4], td[2]);
        tls_cb_va = td[3];
    }

    pthread_mutex_lock(&pe_lock);
    int idx = atomic_load_explicit(&module_count, memory_order_relaxed);
    if (idx >= MAX_LOADED_MODULES) {
        pthread_mutex_unlock(&pe_lock);
        fprintf(stderr, "[pe] MAX_LOADED_MODULES (%d) exceeded\n", MAX_LOADED_MODULES);
        return 0;
    }
    LoadedModule *mod = &modules[idx];
    memset(mod, 0, sizeof *mod);
    snprintf(mod->name, sizeof mod->name, "%s", pe_name);
    build_win_path(host_file, pe_name, mod->win_path, sizeof mod->win_path);
    snprintf(mod->host_dir, sizeof mod->host_dir, "%s", host_file);
    char *slash = strrchr(mod->host_dir, '/');
    if (slash) *slash = 0; else snprintf(mod->host_dir, sizeof mod->host_dir, ".");
    mod->base = base;
    mod->size = (img_sz + 0xFFFu) & ~0xFFFu;
    mod->entry = ep_rva ? (base + ep_rva) : 0;
    mod->exp_rva = exp_rva;
    mod->exp_sz = exp_sz;
    mod->tls_index = tls_idx;
    mod->refcount = 1;
    atomic_store_explicit(&module_count, idx + 1, memory_order_release);
    pthread_mutex_unlock(&pe_lock);

    fprintf(stderr, "[pe] mapped %s @%#x..%#x (pref=%#x ep=%#x tls=%d) from %s\n",
            mod->name, mod->base, mod->base + mod->size, pref_base, mod->entry, tls_idx, host_file);

    if (imp_rva) {
        for (uint32_t d = base + imp_rva; G32(d + 12); d += 20) {
            const char *imp_dll = (const char *)MEM + base + G32(d + 12);
            uint32_t ilt = G32(d) ? base + G32(d) : base + G32(d + 16);
            uint32_t iat = base + G32(d + 16);
            char dep_shim[128], dep_pe[128];
            int is_pe = pe_route_dll_name(imp_dll, dep_shim, sizeof dep_shim, dep_pe, sizeof dep_pe);
            uint32_t dep_base = 0;
            LoadedModule *dep_mod = NULL;
            if (is_pe) {
                dep_base = pe_load_dll(c, dep_pe, mod->host_dir);
                if (!dep_base) {
                    fprintf(stderr, "[pe] FAILED dependency %s -> %s\n", mod->name, imp_dll);
                    return 0;
                }
                pthread_mutex_lock(&pe_lock);
                dep_mod = find_mod_by_base_locked(dep_base);
                pthread_mutex_unlock(&pe_lock);
            }
            for (; G32(ilt); ilt += 4, iat += 4) {
                uint32_t e = G32(ilt);
                char sym[160];
                if (e & 0x80000000u) snprintf(sym, sizeof sym, "#%u", e & 0xFFFFu);
                else snprintf(sym, sizeof sym, "%s", (const char *)MEM + base + e + 2);
                uint32_t val = is_pe ? resolve_export_internal(c, dep_mod, sym, 0) : host_bind_import(dep_shim, sym);
                if (!val) {
                    fprintf(stderr, "[pe] FAILED import %s -> %s!%s\n", mod->name, imp_dll, sym);
                    return 0;
                }
                P32(iat, val);
            }
        }
    }

    rt_notify_code_patch(mod->base, mod->size);

    if (tls_cb_va) {
        for (uint32_t p = tls_cb_va; G32(p); p += 4) {
            uint32_t cb = G32(p);
            if (trace_pe()) fprintf(stderr, "[pe] running TLS callback %#x for %s\n", cb, mod->name);
            pe_call_stdcall3(c, cb, mod->base, 1 /* DLL_PROCESS_ATTACH */, 0);
        }
    }

    if (mod->entry && c) {
        if (trace_pe()) fprintf(stderr, "[pe] calling DllMain(%#x, 1, 0) for %s @%#x\n", mod->base, mod->name, mod->entry);
        uint32_t ok = pe_call_stdcall3(c, mod->entry, mod->base, 1 /* DLL_PROCESS_ATTACH */, 0);
        fprintf(stderr, "[pe] DllMain(%s) -> %u\n", mod->name, ok);
        if (!ok) return 0;
    }
    mod->initialized = 1;
    return mod->base;
}

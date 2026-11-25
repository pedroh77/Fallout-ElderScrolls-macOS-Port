// M2 host: loads a 32-bit PE image into the guest address space, binds its imports to native
// shims, builds the main thread (stack + TEB) and runs the recompiled entry point.
//
//   fnvhost <game.exe> [args...]
//
// An import with no shim stops the run and names itself â€” that list is the M2 work queue.
#include <execinfo.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "FNVGuestMemory.h"
#include <pthread.h>
#include <pthread/qos.h>
#include "../rt.h"
#include "host.h"
int host_run_guest(CPU *c, uint32_t entry);

uint8_t *MEM;
int64_t rt_budget = INT64_MAX;
static _Thread_local jmp_buf *trap_jb;
static _Thread_local int trap_code;
static _Thread_local uint32_t trap_addr;
static _Thread_local CPU *import_cpu;
void host_heap_fault(const char *why,uint32_t address,uint32_t expected,uint32_t actual){
    (void)why;(void)expected;(void)actual;
    CPU *c=import_cpu;
    if(c){
        uint32_t esp=REG(c,R_ESP);
        fprintf(stderr,"[heap-context] guest=%#x eip=%#x caller=%#x eax=%#x ecx=%#x edx=%#x edi=%#x\n",address,REG(c,644),esp<=0xfffffffbu?G32(esp):0,REG(c,R_EAX),REG(c,R_ECX),REG(c,R_EDX),REG(c,28));
        if(esp<=0xffffff00u){fprintf(stderr,"[heap-stack] esp=%#x",esp);for(int i=0;i<40;i++)fprintf(stderr," %#x",G32(esp+4u*i));fprintf(stderr,"\n");}
    }
    void *frames[24]; int n = backtrace(frames, 24);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
}

void rt_trap(CPU *c, int code, uint32_t addr) {
    (void)c;
    trap_code = code;
    trap_addr = addr;
    longjmp(*trap_jb, 1);
}

static const char *trap_name(int code) {
    switch (code) {
    case TRAP_BUDGET: return "budget";
    case TRAP_BRANCHIND: return "unresolved indirect branch";
    case TRAP_DIV0: return "divide by zero";
    case TRAP_FALLOUT: return "fell out of function";
    case TRAP_UNSUPPORTED: return "unsupported/not emitted";
    case TRAP_NOFN: return "no function at address";
    case TRAP_INT3: return "int3";
    case TRAP_IMPORT: return "unimplemented import";
    default: return "?";
    }
}

static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, MEM + a, 4); return v; }
static uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, MEM + a, 2); return v; }

// ---- imports ---------------------------------------------------------------------------------
HostImport host_imports[MAX_IMPORTS];
int host_import_count;
static pthread_mutex_t import_lk = PTHREAD_MUTEX_INITIALIZER;
void host_sync_boundary(CPU *c);

uint32_t host_bind_import(const char *dll, const char *name) {
    ShimFn fn = host_find_shim(dll, name);
    pthread_mutex_lock(&import_lk);
    if (fn) {
        for (int i = 0; i < host_import_count; i++) {
            if (host_imports[i].fn == fn) {
                pthread_mutex_unlock(&import_lk);
                return IMPORT_THUNK_BASE + 4u * (uint32_t)i;
            }
        }
    } else {
        for (int i = 0; i < host_import_count; i++) {
            if (!strcasecmp(host_imports[i].dll, dll) && !strcmp(host_imports[i].name, name)) {
                pthread_mutex_unlock(&import_lk);
                return IMPORT_THUNK_BASE + 4u * (uint32_t)i;
            }
        }
    }
    if (host_import_count >= MAX_IMPORTS) {
        pthread_mutex_unlock(&import_lk);
        fprintf(stderr, "[host] import table overflow (%d)\n", MAX_IMPORTS);
        abort();
    }
    int slot = host_import_count++;
    HostImport *hi = &host_imports[slot];
    snprintf(hi->dll, sizeof hi->dll, "%s", dll);
    snprintf(hi->name, sizeof hi->name, "%s", name);
    hi->fn = fn;
    pthread_mutex_unlock(&import_lk);
    return IMPORT_THUNK_BASE + 4u * (uint32_t)slot;
}

void host_bind_pe_imports(uint32_t base) {
    uint32_t pe = base + rd32(base + 0x3C);
    uint32_t imp = rd32(pe + 24 + 96 + 8);
    if (!imp) return;
    for (uint32_t d = base + imp; rd32(d + 12); d += 20) {
        const char *dll = (const char *)MEM + base + rd32(d + 12);
        uint32_t ilt = rd32(d) ? base + rd32(d) : base + rd32(d + 16);
        uint32_t iat = base + rd32(d + 16);
        for (; rd32(ilt); ilt += 4, iat += 4) {
            uint32_t e = rd32(ilt);
            char name_buf[128];
            if (e & 0x80000000u) snprintf(name_buf, sizeof name_buf, "#%u", e & 0xFFFF);
            else snprintf(name_buf, sizeof name_buf, "%s", (const char *)MEM + base + e + 2);
            uint32_t thunk = host_bind_import(dll, name_buf);
            memcpy(MEM + iat, &thunk, 4);
        }
    }
}

// called by rt_call for addresses in the thunk range
void rt_import_call(CPU *c, uint32_t addr) {
    int i = (int)((addr - IMPORT_THUNK_BASE) / 4);
    if (i < 0 || i >= host_import_count) rt_trap(c, TRAP_NOFN, addr);
    HostImport *hi = &host_imports[i];
    host_sync_boundary(c);
    if (!hi->fn) {
        fprintf(stderr, "[host] unimplemented import %s!%s\n", hi->dll, hi->name);
        rt_trap(c, TRAP_IMPORT, addr);
    }
    static int trace = -1;
    if (trace < 0) trace = getenv("FNV_TRACE") != NULL;
    if (trace) fprintf(stderr, "[imp] %s!%s(%#x, %#x, %#x, %#x)\n", hi->dll, hi->name,
                       G32(REG(c, R_ESP) + 4), G32(REG(c, R_ESP) + 8), G32(REG(c, R_ESP) + 12), G32(REG(c, R_ESP) + 16));
    CPU *previous_import=import_cpu;import_cpu=c;
    hi->fn(c);
    import_cpu=previous_import;
    if (trace) fprintf(stderr, "[imp]   -> %#x\n", REG(c, R_EAX));
}

// ---- loader ----------------------------------------------------------------------------------
__attribute__((weak)) int pe_addr_in_module(uint32_t addr) { (void)addr; return 0; }
__attribute__((weak)) int host_addr_in_exec_va(uint32_t addr) { (void)addr; return 0; }
__attribute__((weak)) int pe_can_locate_dll(const char *name_or_path) { (void)name_or_path; return 0; }
__attribute__((weak)) uint32_t pe_load_dll(CPU *c, const char *name_or_path, const char *parent_host_dir) {
    (void)c; (void)name_or_path; (void)parent_host_dir; return 0;
}

int host_addr_is_executable(uint32_t addr) {
    if (addr >= host_text_start && addr < host_text_end) return 1;
    if (pe_addr_in_module(addr)) return 1;
    if (host_addr_in_exec_va(addr)) return 1;
    return 0;
}

uint32_t host_load_pe(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(n);
    if (fread(d, 1, n, f) != (size_t)n) { perror("read"); exit(1); }
    fclose(f);
    uint32_t pe; memcpy(&pe, d + 0x3C, 4);
    uint32_t base, soh, ep; uint16_t nsec, osz;
    memcpy(&nsec, d + pe + 6, 2); memcpy(&osz, d + pe + 20, 2);
    memcpy(&ep, d + pe + 24 + 16, 4); memcpy(&base, d + pe + 24 + 28, 4); memcpy(&soh, d + pe + 24 + 60, 4);
    memcpy(MEM + base, d, soh);
    for (int i = 0; i < nsec; i++) {
        uint8_t *s = d + pe + 24 + osz + 40 * i;
        uint32_t vs, va, rs, rp, sc;
        memcpy(&vs, s + 8, 4); memcpy(&va, s + 12, 4); memcpy(&rs, s + 16, 4); memcpy(&rp, s + 20, 4); memcpy(&sc, s + 36, 4);
        uint32_t take = rs < vs || !vs ? rs : vs;
        memcpy(MEM + base + va, d + rp, take);
        if (!memcmp(s, ".text", 5) || (host_text_start == 0x401000 && (sc & 0x20000000u))) {
            host_text_start = base + va;
            host_text_end = base + va + (vs ? vs : rs);
        }
    }
    // static TLS (__declspec(thread)): snapshot the template; host_thread_init gives every thread its
    // own copy in TLS array[index], as the Windows loader does
    uint32_t tls_rva; memcpy(&tls_rva, d + pe + 24 + 96 + 9 * 8, 4);
    if (tls_rva) {
        uint32_t td[5]; memcpy(td, MEM + base + tls_rva, sizeof td);   // start, end, &index, &callbacks, zerofill
        host_tls_template_size = td[1] - td[0];
        host_tls_block_size = host_tls_template_size + td[4];
        host_tls_template = malloc(host_tls_template_size + 1);
        memcpy(host_tls_template, MEM + td[0], host_tls_template_size);
        int idx = host_tls_register(td[0], host_tls_template_size, td[4], td[2]);
        uint32_t cb = td[3] ? G32(td[3]) : 0;
        fprintf(stderr, "[host] static TLS[%d]: %u-byte template + %u zero-fill%s\n", idx, host_tls_template_size, td[4],
                cb ? " (TLS callbacks present: NOT run)" : "");
    }
    free(d);
    host_image_base = base;
    return base + ep;
}

uint32_t host_image_base;
uint32_t host_text_start = 0x401000, host_text_end = 0x1000000;
uint8_t *host_tls_template;
uint32_t host_tls_template_size, host_tls_block_size;

// Selected generated entry probes are inserted by the diagnostic builder only.
// The register file is read before LOAD_ENTRY; no guest state is changed.
void rt_diag_entry(CPU *c,uint32_t entry) {
    uint32_t sp=REG(c,R_ESP),object=REG(c,4);
    fprintf(stderr,"[entry-state] entry=%#x return_pc=%#x eax=%#x ecx=%#x edx=%#x esp=%#x ebp=%#x arg0=%#x arg1=%#x arg2=%#x\n",
        entry,sp<=0xfffffff0u?G32(sp):0,REG(c,0),object,REG(c,8),sp,REG(c,20),
        sp<=0xfffffff0u?G32(sp+4):0,sp<=0xfffffff0u?G32(sp+8):0,sp<=0xfffffff0u?G32(sp+12):0);
    if(object<=0xfffffff0u)
        fprintf(stderr,"[entry-object] entry=%#x receiver=%#x words=%#x,%#x,%#x,%#x\n",entry,object,G32(object),G32(object+4),G32(object+8),G32(object+12));
}
void rt_diag_return(CPU *c,uint32_t site,uint32_t callee) {
    fprintf(stderr,"[callee-return] site=%#x callee=%#x eax=%#x ecx=%#x esp=%#x ebp=%#x\n",
        site,callee,REG(c,0),REG(c,4),REG(c,16),REG(c,20));
}

static void on_fault(int sig, siginfo_t *si, void *uctx) {
    (void)uctx;
    uintptr_t raw = si ? (uintptr_t)si->si_addr : 0;
    uintptr_t base = (uintptr_t)MEM;
    uint64_t g = (MEM && raw >= base && raw - base < 0x100000000ULL) ? (uint64_t)(raw - base) : UINT64_MAX;
    char buf[192];
    int len = snprintf(buf, sizeof buf, "\n[host] FAULT sig=%d addr=%p guest=%#llx\n",
                       sig, (void *)raw, (unsigned long long)g);
    if (len > 0) write(STDERR_FILENO, buf, (size_t)len);
    void *frames[32];
    int n = backtrace(frames, 32);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    _exit(128 + sig);
}

static void install_fault_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
}

#if !defined(FNV_GAME_FALLOUT3) && !defined(FNV_GAME_SKYRIM)
static void nvse_on_winmain_entry(CPU *c, uint32_t addr) {
    (void)addr;
    CPU saved = *c;
    SETREG(c, R_ESP, REG(c, R_ESP) - 128u);
    uint32_t mod = pe_load_dll(c, "nvse_1_4.dll", NULL);
    *c = saved;
    host_sync_text_patches();
    if (mod) {
        fprintf(stderr, "[nvse] loaded nvse_1_4.dll @%#x (%u function groups deoptimized, %u dynamic call/push-hooked)\n",
                mod, rt_patched_group_count(), rt_dyn_group_count());
    } else {
        fprintf(stderr, "[nvse] failed to load nvse_1_4.dll; continuing vanilla\n");
    }
}

static void host_arm_nvse_boot(void) {
    if (!rt_has_fallback()) return;
    const char *env = getenv("FNV_NVSE");
    if (env && !strcmp(env, "0")) {
        fprintf(stderr, "[nvse] disabled via FNV_NVSE=0\n");
        return;
    }
    if (!pe_can_locate_dll("nvse_1_4.dll")) {
        if (env && !strcmp(env, "1")) {
            fprintf(stderr, "[nvse] FNV_NVSE=1 requested, but nvse_1_4.dll was not found\n");
        }
        return;
    }
    if (host_image_base != 0x00400000u || host_text_end <= 0x00ECC470u || MEM[0x00ECC46Bu] != 0xE8u) {
        fprintf(stderr, "[nvse] unexpected PE layout (base=%#x text_end=%#x); skipping hook\n",
                host_image_base, host_text_end);
        return;
    }
    uint32_t winmain = 0x00ECC46Bu + 5u + G32(0x00ECC46Bu + 1u);
    if (winmain != 0x0086a850u) {
        fprintf(stderr, "[nvse] unexpected WinMain target %#x; skipping hook\n", winmain);
        return;
    }
    rt_set_boot_hook(winmain, nvse_on_winmain_entry);
    fprintf(stderr, "[nvse] armed WinMain boot hook @%#x\n", winmain);
}
#endif

int fnv_run_game(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s game.exe [args]\n", argv[0]); return 2; }
    install_fault_handler();
    void *p = fnv_reserve_guest();
    if (p == MAP_FAILED) { perror("reserve 4 GiB guest address space"); return 1; }
    MEM = p;
    // the game's main thread runs the frame: keep it on performance cores ahead of the game's workers
    // (a CLI process otherwise starts at default QoS and can land on efficiency cores)
    if (!getenv("FNV_NO_QOS")) pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    host_init_args(argc - 1, argv + 1);
    uint32_t entry = host_load_pe(argv[1]);
    host_bind_pe_imports(host_image_base);
#if !defined(FNV_GAME_FALLOUT3) && !defined(FNV_GAME_SKYRIM)
    host_arm_nvse_boot();
#endif
    fprintf(stderr, "[host] image @%#x, entry %#x, %d imports (%d shimmed)\n", host_image_base, entry,
            host_import_count, host_shimmed_count());

    static CPU cpu;
    CPU *c = &cpu;
    host_thread_init(c, 1 << 20);
    ST(c->r + 4256, 0x027F, 2);              // FPU control word as the MSVC CRT expects
    // push a return address that ends the run cleanly
    uint32_t esp = (uint32_t)LD(c->r + 16, 4) - 4;
    memcpy(MEM + esp, &(uint32_t){HOST_EXIT_THUNK}, 4);
    ST(c->r + 16, esp, 4);

    int rc = host_run_guest(c, entry);
    if (rc) return 3;
    fprintf(stderr, "[host] entry returned, EAX=%#x\n", (uint32_t)LD(c->r, 4));
    return 0;
}

// Runs guest code on this host thread with a trap guard; returns 0, or the trap code.
int host_run_guest(CPU *c, uint32_t entry) {
    jmp_buf jb;
    jmp_buf *saved = trap_jb;
    CPU *saved_import=import_cpu;
    trap_jb = &jb;
    if (setjmp(jb)) {
        uint32_t trap_sp=REG(c,16);
        fprintf(stderr, "[host] TRAP (thread %u): %s at %#x (EIP~%#x, stack_return=%#x)\n", G32(REG(c, 272) + 0x24),
                trap_name(trap_code), trap_addr, (uint32_t)LD(c->r + 644, 4),trap_sp<=0xfffffffbu?G32(trap_sp):0);
        rt_calltrace_dump();
        fprintf(stderr,"[guest-state] eax=%#x ecx=%#x edx=%#x ebx=%#x esp=%#x ebp=%#x esi=%#x edi=%#x\n",
            REG(c,0),REG(c,4),REG(c,8),REG(c,12),REG(c,16),REG(c,20),REG(c,24),REG(c,28));
        uint32_t sp=REG(c,16), object=REG(c,4);
        if(sp<=0xffffffbfu) {
            fprintf(stderr,"[guest-stack] @%#x",sp);
            for(int i=0;i<16;i++) fprintf(stderr," %#x",G32(sp+4u*i));
            fprintf(stderr,"\n");
        }
        if(object<=0xffffffdfu) {
            fprintf(stderr,"[guest-ecx-object] @%#x",object);
            for(int i=0;i<8;i++) fprintf(stderr," %#x",G32(object+4u*i));
            uint32_t vt=G32(object);
            fprintf(stderr,"\n");
            if(vt && vt<=0xffffffdfu) {
                fprintf(stderr,"[guest-object-vtable] @%#x",vt);
                for(int i=0;i<8;i++) fprintf(stderr," %#x",G32(vt+4u*i));
                fprintf(stderr,"\n");
            }
        }
        // Register-based indirect COM calls often leave ECX holding a thunk. Inspect
        // the actual first stdcall argument, not only the ECX register.
        if(trap_addr==0 && sp<=0xfffffffbu){
            uint32_t o=G32(sp+4);
            if(o && o<=0xffffffbfu){
                fprintf(stderr,"[guest-arg0-object] @%#x",o);
                for(unsigned i=0;i<16;i++)fprintf(stderr," %#x",G32(o+4*i));
                uint32_t vt=G32(o);fprintf(stderr,"\n");
                if(vt && vt<=0xffffff9fu)for(unsigned i=0;i<24;i++){
                    uint32_t target=G32(vt+4*i),slot=(target-IMPORT_THUNK_BASE)/4;
                    fprintf(stderr,"[guest-arg0-vtable] +%#x target=%#x %s\n",4*i,target,target>=IMPORT_THUNK_BASE&&slot<(uint32_t)host_import_count?host_imports[slot].name:"");
                }
            }
        }
        trap_jb = saved;import_cpu=saved_import;
        return trap_code;
    }
    rt_call(c, entry);
    trap_jb = saved;import_cpu=saved_import;
    return 0;
}

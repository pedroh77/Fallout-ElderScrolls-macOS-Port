// Standalone host test for PE DLL loader, relocations, multi-module static TLS,
// export forwarding, imagehlp shims, and real xNVSE initialization inside Unicorn.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "FNVGuestMemory.h"
#include "host.h"
#include "pe_loader.h"
#include "shims.h"

const uint32_t rt_fn_addr[] = {0};
const GuestFn rt_fn_ptr[] = {0};
const int rt_fn_count = 0;

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
int host_run_guest(CPU *c, uint32_t entry);

static int mode_nvse(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s nvse <FalloutNV.exe> <crt_dir> <nvse_1_4.dll>\n", argv[0]);
        return 2;
    }
    const char *exe_path = argv[2];
    const char *crt_dir = argv[3];
    const char *nvse_path = argv[4];

    void *mem = fnv_reserve_guest();
    if (mem == MAP_FAILED) { perror("fnv_reserve_guest"); return 1; }
    MEM = mem;

    char *fake_argv[] = {(char *)exe_path};
    host_init_args(1, fake_argv);
    (void)host_load_pe(exe_path);
    host_bind_pe_imports(host_image_base);
    pe_add_search_dir(crt_dir);

    static CPU cpu;
    CPU *c = &cpu;
    host_thread_init(c, 1 << 20);
    ST(c->r + 4256, 0x027F, 2);

    // Allocate an executable trampoline page via VirtualAlloc(NULL, 4096, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE)
    uint32_t va_thunk = host_bind_import("kernel32", "VirtualAlloc");
    uint32_t esp = REG(c, R_ESP) - 32;
    P32(esp, HOST_EXIT_THUNK); P32(esp + 4, 0); P32(esp + 8, 4096); P32(esp + 12, 0x3000); P32(esp + 16, 0x40);
    SETREG(c, R_ESP, esp);
    rt_import_call(c, va_thunk);
    uint32_t code = REG(c, R_EAX);

    uint32_t path_va = gmalloc((uint32_t)strlen(nvse_path) + 1);
    strcpy((char *)MEM + path_va, nvse_path);
    uint32_t ll_thunk = host_bind_import("kernel32", "LoadLibraryA");

    // push path_va; mov eax, ll_thunk; call eax; ret
    uint8_t stub[16];
    stub[0] = 0x68; memcpy(stub + 1, &path_va, 4);
    stub[5] = 0xB8; memcpy(stub + 6, &ll_thunk, 4);
    stub[10] = 0xFF; stub[11] = 0xD0; // call eax
    stub[12] = 0xC3;                  // ret
    memcpy(MEM + code, stub, 13);

    esp = REG(c, R_ESP) - 4;
    P32(esp, HOST_EXIT_THUNK);
    SETREG(c, R_ESP, esp);
    int rc = host_run_guest(c, code);
    uint32_t hmod = REG(c, R_EAX);
    printf("NVSE LoadLibraryA rc=%d hmod=%#x\n", rc, hmod);
    if (rc != 0 || hmod == 0) return 1;
    uint32_t start_fn = pe_get_proc_address(c, hmod, "StartNVSE");
    printf("StartNVSE=%#x\n", start_fn);
    return start_fn ? 0 : 1;
}

static int mode_synthetic(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s synthetic <dll_dir>\n", argv[0]);
        return 2;
    }
    const char *dir = argv[2];
    void *mem = fnv_reserve_guest();
    if (mem == MAP_FAILED) { perror("fnv_reserve_guest"); return 1; }
    MEM = mem;

    char *fake_argv[] = {"FalloutNV.exe"};
    host_init_args(1, fake_argv);
    host_image_base = 0x00400000u;
    host_text_start = 0x00401000u;
    host_text_end = 0x00402000u;
    // Minimal PE header for image base reservation
    P32(host_image_base + 0x3C, 0x80);
    P32(host_image_base + 0x80 + 24 + 56, 0x00100000u);
    pe_add_search_dir(dir);

    static CPU cpu;
    CPU *c = &cpu;
    host_thread_init(c, 1 << 20);
    ST(c->r + 4256, 0x027F, 2);

    uint64_t ep0 = rt_patch_epoch();
    uint32_t dep = pe_load_dll(c, "dep.dll", dir);
    uint32_t main_mod = pe_load_dll(c, "main.dll", dir);
    printf("dep=%#x main=%#x epoch_before=%llu epoch_after=%llu\n",
           dep, main_mod, (unsigned long long)ep0, (unsigned long long)rt_patch_epoch());
    if (!dep || !main_mod || dep == main_mod) return 1;
    if (rt_patch_epoch() <= ep0) { fprintf(stderr, "VirtualProtect did not bump patch epoch\n"); return 1; }

    uint32_t get_tls_fn = pe_get_proc_address(c, main_mod, "get_tls_val");
    uint32_t fwd_add_fn = pe_get_proc_address(c, main_mod, "forward_add");
    printf("get_tls_fn=%#x fwd_add_fn=%#x\n", get_tls_fn, fwd_add_fn);
    if (!get_tls_fn || !fwd_add_fn) return 1;

    // Call get_tls_val() (cdecl 0 args)
    uint32_t esp = REG(c, R_ESP);
    uint32_t sp = esp - 32;
    P32(sp, HOST_EXIT_THUNK);
    SETREG(c, R_ESP, sp);
    if (host_run_guest(c, get_tls_fn) != 0) return 1;
    uint32_t tls_val = REG(c, R_EAX);
    SETREG(c, R_ESP, esp);
    printf("tls_val=%#x\n", tls_val);
    if (tls_val != 0xCAFEBABEu) return 1;

    // Call forwarded export forward_add(17, 25) -> 42
    sp = esp - 32;
    P32(sp, HOST_EXIT_THUNK);
    P32(sp + 4, 17);
    P32(sp + 8, 25);
    SETREG(c, R_ESP, sp);
    if (host_run_guest(c, fwd_add_fn) != 0) return 1;
    uint32_t sum = REG(c, R_EAX);
    SETREG(c, R_ESP, esp);
    printf("forward_add(17,25)=%u\n", sum);
    if (sum != 42) return 1;

    // Test imagehlp MapAndLoad / ImageDirectoryEntryToData / ImageRvaToVa / UnMapAndLoad
    uint32_t map_fn = host_bind_import("imagehlp", "MapAndLoad");
    uint32_t dir_fn = host_bind_import("imagehlp", "ImageDirectoryEntryToData");
    uint32_t rva_fn = host_bind_import("imagehlp", "ImageRvaToVa");
    uint32_t unmap_fn = host_bind_import("imagehlp", "UnMapAndLoad");
    uint32_t li = gmalloc(64), sz_out = gmalloc(4);
    uint32_t img_nm = gmalloc(32), dll_dir = gmalloc(128);
    strcpy((char *)MEM + img_nm, "main.dll");
    strcpy((char *)MEM + dll_dir, ".");
    sp = esp - 32;
    P32(sp, HOST_EXIT_THUNK); P32(sp + 4, img_nm); P32(sp + 8, dll_dir); P32(sp + 12, li); P32(sp + 16, 0); P32(sp + 20, 1);
    SETREG(c, R_ESP, sp);
    rt_import_call(c, map_fn);
    if (REG(c, R_EAX) != 1) { fprintf(stderr, "MapAndLoad failed\n"); return 1; }

    uint32_t mapped_base = G32(li + 0x08), file_hdr = G32(li + 0x0C);
    sp = esp - 32;
    P32(sp, HOST_EXIT_THUNK); P32(sp + 4, mapped_base); P32(sp + 8, 0); P32(sp + 12, 0); P32(sp + 16, sz_out);
    SETREG(c, R_ESP, sp);
    rt_import_call(c, dir_fn);
    uint32_t exp_va = REG(c, R_EAX);
    if (!exp_va) { fprintf(stderr, "ImageDirectoryEntryToData failed\n"); return 1; }
    uint32_t names_rva = G32(exp_va + 0x20);
    sp = esp - 32;
    P32(sp, HOST_EXIT_THUNK); P32(sp + 4, file_hdr); P32(sp + 8, mapped_base); P32(sp + 12, names_rva); P32(sp + 16, 0);
    SETREG(c, R_ESP, sp);
    rt_import_call(c, rva_fn);
    uint32_t names_va = REG(c, R_EAX);
    const char *first_exp = (const char *)MEM + mapped_base + (G32(names_va) ? 0 : 0);
    // RvaToVa of first name
    sp = esp - 32;
    P32(sp, HOST_EXIT_THUNK); P32(sp + 4, file_hdr); P32(sp + 8, mapped_base); P32(sp + 12, G32(names_va)); P32(sp + 16, 0);
    SETREG(c, R_ESP, sp);
    rt_import_call(c, rva_fn);
    first_exp = (const char *)MEM + REG(c, R_EAX);
    printf("imagehlp first export=%s\n", first_exp);
    if (strcmp(first_exp, "forward_add") != 0) return 1;

    sp = esp - 32;
    P32(sp, HOST_EXIT_THUNK); P32(sp + 4, li);
    SETREG(c, R_ESP, sp);
    rt_import_call(c, unmap_fn);

    printf("PASS: synthetic PE loader & imagehlp suite\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    if (!strcmp(argv[1], "synthetic")) return mode_synthetic(argc, argv);
    if (!strcmp(argv[1], "nvse")) return mode_nvse(argc, argv);
    return 2;
}

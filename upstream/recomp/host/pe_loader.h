#pragma once
#include <stdint.h>
#include "host.h"

#define MAX_LOADED_MODULES 64

typedef struct {
    char name[128];          // lowercase basename, e.g. "nvse_1_4.dll"
    char win_path[512];      // e.g. "C:\\Games\\FalloutNV\\nvse_1_4.dll"
    char host_dir[1024];     // host directory containing this DLL
    uint32_t base;           // mapped guest ImageBase
    uint32_t size;           // SizeOfImage (page-aligned)
    uint32_t entry;          // base + AddressOfEntryPoint (0 if none)
    uint32_t exp_rva;
    uint32_t exp_sz;
    int tls_index;           // -1 if no static TLS
    int initialized;
    int refcount;
} LoadedModule;

void pe_add_search_dir(const char *host_dir);
int pe_can_locate_dll(const char *name_or_path);
uint32_t pe_load_dll(CPU *c, const char *name_or_path, const char *parent_host_dir);
uint32_t pe_find_module_by_name(const char *name);
uint32_t pe_find_module_by_addr(uint32_t addr);
int pe_addr_in_module(uint32_t addr);
const char *pe_module_win_path(uint32_t mod_base);
uint32_t pe_get_proc_address(CPU *c, uint32_t mod_base, const char *name_or_ord);

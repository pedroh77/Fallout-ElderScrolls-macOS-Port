#pragma once
#include <stdint.h>
#include <string.h>
#include "../rt.h"

#define MAX_IMPORTS 16384
#define IMPORT_THUNK_BASE 0xFEE00000u   // import slot i -> IMPORT_THUNK_BASE + 4*i (shared with difftest)
#define HOST_EXIT_THUNK  0xFEDF0000u    // return address of the entry point: run finished
#define MAX_TLS_MODULES 32
enum { TRAP_IMPORT = 8 };

typedef void (*ShimFn)(CPU *c);
typedef struct { char dll[64]; char name[96]; ShimFn fn; } HostImport;
extern HostImport host_imports[MAX_IMPORTS];
extern int host_import_count;
extern uint32_t host_image_base;
extern uint32_t host_text_start, host_text_end;
const char *host_exe_name(void);
const char *host_game_dir(void);

ShimFn host_find_shim(const char *dll, const char *name);
int host_shimmed_count(void);
uint32_t host_bind_import(const char *dll, const char *name);
uint32_t host_load_pe(const char *path);
void host_bind_pe_imports(uint32_t base);
void host_init_args(int argc, char **argv);
void host_thread_init(CPU *c, uint32_t stack_size);
void rt_import_call(CPU *c, uint32_t addr);
int host_reserve_guest_range(uint32_t lo, uint32_t hi);
int host_tls_register(uint32_t template_va, uint32_t template_size, uint32_t zero_fill, uint32_t index_va);
void host_tls_populate_thread(uint32_t teb);
int host_addr_is_executable(uint32_t addr);
void host_sync_text_patches(void);

// guest memory helpers
extern uint8_t *host_tls_template;                // static TLS template (NULL: the image has none)
extern uint32_t host_tls_template_size, host_tls_block_size;
static inline uint32_t G32(uint32_t a) { uint32_t v; memcpy(&v, MEM + a, 4); return v; }
static inline void P32(uint32_t a, uint32_t v) { memcpy(MEM + a, &v, 4); }
static inline char *GSTR(uint32_t a) { return a ? (char *)MEM + a : NULL; }
static inline uint32_t REG(CPU *c, int off) { return (uint32_t)LD(c->r + off, 4); }
static inline void SETREG(CPU *c, int off, uint32_t v) { ST(c->r + off, v, 4); }
#define R_EAX 0
#define R_ECX 4
#define R_EDX 8
#define R_ESP 16
// stdcall/cdecl argument i (0-based) as seen on entry to the import (return address at [ESP])
static inline uint32_t ARG(CPU *c, int i) { return G32(REG(c, R_ESP) + 4 + 4 * i); }
// return from a shim: set EAX, pop the return address (+ nargs*4 for stdcall callee cleanup)
static inline void RET(CPU *c, uint32_t eax, int stdcall_args) {
    SETREG(c, R_EAX, eax);
    SETREG(c, R_ESP, REG(c, R_ESP) + 4 + 4 * stdcall_args);
}

// guest heap (lives inside the 4 GB guest space)
uint32_t gmalloc(uint32_t size);
void gfree(uint32_t p);
uint32_t gsize(uint32_t p);
uint32_t grealloc(uint32_t p, uint32_t size);
uint32_t gpages(uint32_t size);          // page-aligned, zeroed (VirtualAlloc)

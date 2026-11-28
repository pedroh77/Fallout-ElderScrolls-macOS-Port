#pragma once
#include <stdint.h>
#include "host.h"

// Native callback thunk allocator in [SCRIPT_THUNK_BASE, SCRIPT_THUNK_END) for script extenders.
uint32_t script_thunk_alloc(const char *name, ShimFn fn);
void script_thunk_reset(void);

// Re-entrant host -> guest call helpers preserving the full CPU register/flag/x87 state.
uint32_t guest_call_cdecl(CPU *c, uint32_t fn, int nargs, const uint32_t *args);
uint32_t guest_call_thiscall(CPU *c, uint32_t fn, uint32_t this_ptr, int nargs, const uint32_t *args);

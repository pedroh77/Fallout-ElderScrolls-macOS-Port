#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "script_bridge.h"

typedef struct {
    char name[64];
    ShimFn fn;
} ScriptThunk;

static ScriptThunk script_thunks[MAX_SCRIPT_THUNKS];
static int script_thunk_count;
static pthread_mutex_t script_thunk_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t script_thunk_alloc(const char *name, ShimFn fn) {
    pthread_mutex_lock(&script_thunk_lock);
    if (script_thunk_count >= MAX_SCRIPT_THUNKS) {
        fprintf(stderr, "[script-bridge] out of script thunks (%d)\n", MAX_SCRIPT_THUNKS);
        abort();
    }
    int idx = script_thunk_count++;
    snprintf(script_thunks[idx].name, sizeof script_thunks[idx].name, "%s", name ? name : "?");
    script_thunks[idx].fn = fn;
    uint32_t addr = SCRIPT_THUNK_BASE + 4u * (uint32_t)idx;
    pthread_mutex_unlock(&script_thunk_lock);
    return addr;
}

void script_thunk_reset(void) {
    pthread_mutex_lock(&script_thunk_lock);
    memset(script_thunks, 0, sizeof script_thunks);
    script_thunk_count = 0;
    pthread_mutex_unlock(&script_thunk_lock);
}

void rt_script_call(CPU *c, uint32_t addr) {
    if (addr < SCRIPT_THUNK_BASE || addr >= SCRIPT_THUNK_END || ((addr - SCRIPT_THUNK_BASE) & 3u)) {
        rt_trap(c, TRAP_NOFN, addr);
    }
    int idx = (int)((addr - SCRIPT_THUNK_BASE) / 4u);
    pthread_mutex_lock(&script_thunk_lock);
    int count = script_thunk_count;
    ScriptThunk st = (idx >= 0 && idx < count) ? script_thunks[idx] : (ScriptThunk){{0}, NULL};
    pthread_mutex_unlock(&script_thunk_lock);

    if (idx < 0 || idx >= count || !st.fn) {
        rt_trap(c, TRAP_NOFN, addr);
    }

    static int trace = -1;
    if (trace < 0) trace = getenv("FNV_TRACE_SCRIPT") != NULL;
    if (trace) {
        fprintf(stderr, "[script] %s(%#x, %#x, %#x, %#x)\n",
                st.name, ARG(c, 0), ARG(c, 1), ARG(c, 2), ARG(c, 3));
    }
    st.fn(c);
    if (trace) {
        fprintf(stderr, "[script]   -> eax=%#x\n", REG(c, R_EAX));
    }
}

static uint32_t guest_call_impl(CPU *c, uint32_t fn, int use_this, uint32_t this_ptr, int nargs, const uint32_t *args) {
    CPU saved = *c;
    uint32_t sp = (REG(c, R_ESP) - 64u - 4u * (uint32_t)nargs) & ~15u;
    P32(sp, HOST_EXIT_THUNK);
    for (int i = 0; i < nargs; i++) {
        P32(sp + 4u + 4u * (uint32_t)i, args[i]);
    }
    SETREG(c, R_ESP, sp);
    if (use_this) {
        SETREG(c, R_ECX, this_ptr);
    }
    rt_call(c, fn);
    uint32_t ret = REG(c, R_EAX);
    *c = saved;
    return ret;
}

uint32_t guest_call_cdecl(CPU *c, uint32_t fn, int nargs, const uint32_t *args) {
    return guest_call_impl(c, fn, 0, 0, nargs, args);
}

uint32_t guest_call_thiscall(CPU *c, uint32_t fn, uint32_t this_ptr, int nargs, const uint32_t *args) {
    return guest_call_impl(c, fn, 1, this_ptr, nargs, args);
}

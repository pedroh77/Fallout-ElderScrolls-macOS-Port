// Handles, threads and synchronisation: events, semaphores, mutexes, critical sections, waits.
// Each guest thread is a host pthread running recompiled code on its own CPU + stack + TEB.
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "shims.h"

__attribute__((weak)) void rt_call(CPU *c, uint32_t a) { (void)c; (void)a; }
static pthread_mutex_t slist_lk = PTHREAD_MUTEX_INITIALIZER;

// ---- handle table ----------------------------------------------------------------------------
#define MAXH 65536
static struct { int type; void *obj; } htab[MAXH];
static pthread_mutex_t hlock = PTHREAD_MUTEX_INITIALIZER;
uint32_t handle_new(int type, void *obj) {
    pthread_mutex_lock(&hlock);
    for (uint32_t i = 64; i < MAXH; i++)
        if (!htab[i].type) { htab[i].type = type; htab[i].obj = obj; pthread_mutex_unlock(&hlock); return i * 4; }
    pthread_mutex_unlock(&hlock);
    return 0;
}
void *handle_get(uint32_t h, int type) {
    uint32_t i = h / 4;
    return i < MAXH && htab[i].type == type ? htab[i].obj : NULL;
}
int handle_type(uint32_t h) { uint32_t i = h / 4; return i < MAXH ? htab[i].type : 0; }
void file_close_obj(void *obj) __attribute__((weak));
void file_close_obj(void *obj) { (void)obj; }
void mapping_close_obj(void *obj) __attribute__((weak));
void mapping_close_obj(void *obj) { (void)obj; }

static void *handle_detach(uint32_t h, int *type_out) {
    uint32_t i = h / 4;
    void *obj = NULL;
    int type = 0;
    pthread_mutex_lock(&hlock);
    if (i < MAXH && htab[i].type) {
        type = htab[i].type;
        obj = htab[i].obj;
        htab[i].type = 0;
        htab[i].obj = NULL;
    }
    pthread_mutex_unlock(&hlock);
    if (type_out) *type_out = type;
    return obj;
}

void handle_close(uint32_t h) {
    int type = 0;
    void *obj = handle_detach(h, &type);
    if (type == H_FILE) file_close_obj(obj);
    else if (type == H_MAPPING) mapping_close_obj(obj);
}

// ---- waitable objects ------------------------------------------------------------------------
typedef struct {
    pthread_mutex_t m; pthread_cond_t cv;
    int manual, signaled;          // event
    int count, max;                // semaphore
    pthread_t owner; int recursion; int owned;  // mutex
    int done; uint32_t exit_code;  // thread
    int suspend;                   // thread: >0 while suspended (CREATE_SUSPENDED / SuspendThread)
} Obj;

// Snapshots are written by the owning thread at shim boundaries. Never sample another
// thread's live CPU/register file or unsynchronised RT_CALLTRACE ring.
typedef struct { uint32_t tid, caller, object, trace[8]; uint64_t transitions; const char *state; } SyncDiag;
static SyncDiag diagnostics[64];
static unsigned diagnostic_count;
static pthread_mutex_t diagnostic_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local SyncDiag *my_diagnostic;
static _Thread_local unsigned diagnostic_calls;
static int diagnostic_enabled(void) { return getenv("FNV_TRACE_SYNC") != NULL; }
static void diagnostic_state(CPU *c, const char *state, uint32_t object) {
    if (!diagnostic_enabled()) return;
    pthread_mutex_lock(&diagnostic_lock);
    if (!my_diagnostic && diagnostic_count < 64) {
        my_diagnostic = &diagnostics[diagnostic_count++];
        my_diagnostic->tid = G32(REG(c,272) + 0x24);
    }
    if (my_diagnostic) {
        my_diagnostic->state=state; my_diagnostic->object=object;
        uint32_t sp=REG(c,R_ESP);
        my_diagnostic->caller=sp<=0xfffffffbu?G32(sp):0;
        my_diagnostic->transitions++;
#ifdef RT_CALLTRACE
        for(unsigned i=0;i<8;i++) my_diagnostic->trace[i]=rt_ct_pos>i?rt_ct_ring[(rt_ct_pos-1-i)&255]:0;
#endif
    }
    pthread_mutex_unlock(&diagnostic_lock);
}
void host_sync_boundary(CPU *c) {
    if (!(++diagnostic_calls & 8191)) diagnostic_state(c,"api-progress",0);
}
void host_sync_diagnostics(CPU *c) {
    static uint64_t last;
    if (!diagnostic_enabled()) return;
    uint64_t now=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (now-last < 10000000000ull) return;
    last=now;
    diagnostic_state(c,"present",0);
    pthread_mutex_lock(&diagnostic_lock);
    for(unsigned i=0;i<diagnostic_count;i++) {
        SyncDiag *d=&diagnostics[i];
        fprintf(stderr,"[sync-snapshot] tid=%u state=%s object=%#x caller=%#x transitions=%llu recent_entries=",
            d->tid,d->state,d->object,d->caller,(unsigned long long)d->transitions);
        for(unsigned j=0;j<8;j++)fprintf(stderr,"%#x%s",d->trace[j],j==7?"\n":",");
    }
    pthread_mutex_unlock(&diagnostic_lock);
    rt_calltrace_dump();
}

static Obj *obj_new(void) {
    Obj *o = calloc(1, sizeof *o);
    pthread_mutex_init(&o->m, NULL);
    pthread_cond_init(&o->cv, NULL);
    return o;
}

// is the object signaled for the calling thread? (called with o->m held); consumes if it can
static int try_acquire(int type, Obj *o) {
    switch (type) {
    case H_EVENT: if (o->signaled) { if (!o->manual) o->signaled = 0; return 1; } return 0;
    case H_SEMA: if (o->count > 0) { o->count--; return 1; } return 0;
    case H_MUTEX:
        if (!o->owned || pthread_equal(o->owner, pthread_self())) { o->owned = 1; o->owner = pthread_self(); o->recursion++; return 1; }
        return 0;
    case H_THREAD: return o->done;
    }
    return 0;
}

static int can_acquire(int type, Obj *o) {
    switch(type) {
    case H_EVENT: return o->signaled;
    case H_SEMA: return o->count>0;
    case H_MUTEX: return !o->owned || pthread_equal(o->owner,pthread_self());
    case H_THREAD: return o->done;
    default: return 0;
    }
}

static void deadline(struct timespec *ts, uint32_t ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

// 0 = WAIT_OBJECT_0, 0x102 = WAIT_TIMEOUT
static uint32_t wait_one(uint32_t h, uint32_t ms) {
    int t = handle_type(h);
    Obj *o = t>=H_EVENT && t<=H_THREAD ? handle_get(h, t) : NULL;
    if (!o) { SETERR(6); return 0xFFFFFFFFu; }
    struct timespec ts; deadline(&ts, ms);
    pthread_mutex_lock(&o->m);
    while (!try_acquire(t, o)) {
        if (ms == 0) { pthread_mutex_unlock(&o->m); return 0x102; }
        int r = ms == 0xFFFFFFFFu ? pthread_cond_wait(&o->cv, &o->m) : pthread_cond_timedwait(&o->cv, &o->m, &ts);
        if (r == ETIMEDOUT) { pthread_mutex_unlock(&o->m); return 0x102; }
    }
    pthread_mutex_unlock(&o->m);
    return 0;
}

// ---- named events / mutexes & thread-local I/O APC queue -------------------------------------
#define MAX_NAMED 128
static struct { int type; char name[128]; uint32_t handle; } named_objs[MAX_NAMED];
static int n_named;
static pthread_mutex_t named_lk = PTHREAD_MUTEX_INITIALIZER;

static uint32_t find_named(int type, const char *name) {
    if (!name || !name[0]) return 0;
    pthread_mutex_lock(&named_lk);
    uint32_t r = 0;
    for (int i = 0; i < n_named; i++)
        if (named_objs[i].type == type && !strcasecmp(named_objs[i].name, name)) { r = named_objs[i].handle; break; }
    pthread_mutex_unlock(&named_lk);
    return r;
}
static void register_named(int type, const char *name, uint32_t handle) {
    if (!name || !name[0]) return;
    pthread_mutex_lock(&named_lk);
    if (n_named < MAX_NAMED) {
        named_objs[n_named].type = type;
        snprintf(named_objs[n_named].name, sizeof named_objs[n_named].name, "%s", name);
        named_objs[n_named].handle = handle;
        n_named++;
    }
    pthread_mutex_unlock(&named_lk);
}

#define MAX_APCS 64
typedef struct { uint32_t fn, err, nbytes, ov; } ApcItem;
static _Thread_local ApcItem apc_q[MAX_APCS];
static _Thread_local int apc_h, apc_t;

void host_queue_io_apc(uint32_t fn, uint32_t err, uint32_t nbytes, uint32_t ov) {
    if (!fn) return;
    int next = (apc_t + 1) % MAX_APCS;
    if (next == apc_h) return;
    apc_q[apc_t] = (ApcItem){fn, err, nbytes, ov};
    apc_t = next;
}
static int drain_io_apcs(CPU *c) {
    int ran = 0;
    while (apc_h != apc_t) {
        ApcItem item = apc_q[apc_h];
        apc_h = (apc_h + 1) % MAX_APCS;
        uint32_t esp = REG(c, R_ESP);
        uint32_t sp = esp - 64;
        P32(sp, HOST_EXIT_THUNK);
        P32(sp + 4, item.err);
        P32(sp + 8, item.nbytes);
        P32(sp + 12, item.ov);
        SETREG(c, R_ESP, sp);
        rt_call(c, item.fn);
        SETREG(c, R_ESP, esp);
        ran++;
    }
    return ran;
}

SHIM(CreateEventA) {
    const char *nm = GSTR(ARG(c, 3));
    uint32_t ex = find_named(H_EVENT, nm);
    if (ex) { SETERR(183 /* ERROR_ALREADY_EXISTS */); RET(c, ex, 4); return; }
    Obj *o = obj_new(); o->manual = (int)ARG(c, 1); o->signaled = (int)ARG(c, 2);
    uint32_t h = handle_new(H_EVENT, o);
    register_named(H_EVENT, nm, h);
    SETERR(0);
    RET(c, h, 4);
}
SHIM(CreateEventW) {
    Obj *o = obj_new(); o->manual = (int)ARG(c, 1); o->signaled = (int)ARG(c, 2);
    RET(c, handle_new(H_EVENT, o), 4);
}
SHIM(CreateEventExW) {
    uint32_t fl = ARG(c, 2);
    Obj *o = obj_new(); o->manual = (fl & 1) != 0; o->signaled = (fl & 2) != 0;
    RET(c, handle_new(H_EVENT, o), 4);
}
SHIM(OpenEventA) {
    uint32_t h = find_named(H_EVENT, GSTR(ARG(c, 2)));
    if (!h) SETERR(2);
    RET(c, h, 3);
}
SHIM(SetEvent) {
    Obj *o = handle_get(ARG(c, 0), H_EVENT);
    if (o) { pthread_mutex_lock(&o->m); o->signaled = 1; pthread_cond_broadcast(&o->cv); pthread_mutex_unlock(&o->m); }
    RET(c, o != NULL, 1);
}
SHIM(ResetEvent) {
    Obj *o = handle_get(ARG(c, 0), H_EVENT);
    if (o) { pthread_mutex_lock(&o->m); o->signaled = 0; pthread_mutex_unlock(&o->m); }
    RET(c, o != NULL, 1);
}
SHIM(CreateSemaphoreA) { Obj *o = obj_new(); o->count = (int)ARG(c, 1); o->max = (int)ARG(c, 2); RET(c, handle_new(H_SEMA, o), 4); }
SHIM(CreateSemaphoreExW) { Obj *o = obj_new(); o->count = (int)ARG(c, 1); o->max = (int)ARG(c, 2); RET(c, handle_new(H_SEMA, o), 6); }
SHIM(ReleaseSemaphore) {
    Obj *o = handle_get(ARG(c, 0), H_SEMA); uint32_t prev = ARG(c, 2);
    if (!o) { RET(c, 0, 3); return; }
    pthread_mutex_lock(&o->m);
    if (prev) P32(prev, (uint32_t)o->count);
    o->count += (int)ARG(c, 1);
    pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->m);
    RET(c, 1, 3);
}
SHIM(CreateMutexA) {
    const char *nm = GSTR(ARG(c, 2));
    uint32_t ex = find_named(H_MUTEX, nm);
    if (ex) { SETERR(183 /* ERROR_ALREADY_EXISTS */); RET(c, ex, 3); return; }
    Obj *o = obj_new();
    if (ARG(c, 1)) { o->owned = 1; o->owner = pthread_self(); o->recursion = 1; }
    uint32_t h = handle_new(H_MUTEX, o);
    register_named(H_MUTEX, nm, h);
    SETERR(0);
    RET(c, h, 3);
}
SHIM(OpenMutexA) {
    uint32_t h = find_named(H_MUTEX, GSTR(ARG(c, 2)));
    if (!h) SETERR(2);
    RET(c, h, 3);
}
SHIM(ReleaseMutex) {
    Obj *o = handle_get(ARG(c, 0), H_MUTEX);
    if (!o) { SETERR(6); RET(c, 0, 1); return; }
    pthread_mutex_lock(&o->m);
    if (!o->owned || !pthread_equal(o->owner, pthread_self())) {
        pthread_mutex_unlock(&o->m); SETERR(288); RET(c, 0, 1); return;
    }
    if (--o->recursion <= 0) { o->owned = 0; o->recursion = 0; pthread_cond_broadcast(&o->cv); }
    pthread_mutex_unlock(&o->m);
    RET(c, 1, 1);
}
SHIM(SleepEx) {
    uint32_t ms = ARG(c, 0), alertable = ARG(c, 1);
    if (alertable && drain_io_apcs(c) > 0) { RET(c, 0xC0u /* WAIT_IO_COMPLETION */, 2); return; }
    if (ms && ms != 0xFFFFFFFFu) usleep((useconds_t)ms * 1000u);
    if (alertable && drain_io_apcs(c) > 0) { RET(c, 0xC0u, 2); return; }
    RET(c, 0, 2);
}
SHIM(WaitForSingleObject) {
    uint32_t h=ARG(c,0),ms=ARG(c,1);
    if(ms)diagnostic_state(c,"wait-one",h);
    uint32_t result=wait_one(h,ms);
    if(ms)diagnostic_state(c,"wait-returned",h);
    RET(c,result,2);
}
SHIM(WaitForSingleObjectEx) {
    uint32_t h=ARG(c,0),ms=ARG(c,1);
    if(ms)diagnostic_state(c,"wait-one",h);
    uint32_t result=wait_one(h,ms);
    if(ms)diagnostic_state(c,"wait-returned",h);
    RET(c,result,3);
}
SHIM(WaitForMultipleObjects) {
    uint32_t n = ARG(c, 0), arr = ARG(c, 1), all = ARG(c, 2), ms = ARG(c, 3);
    Obj *objects[64]; int types[64]; unsigned order[64];
    if(!n || n>64 || !arr || arr>0xffffffffu-4*n) { SETERR(87); RET(c,0xffffffffu,4); return; }
    for(unsigned i=0;i<n;i++) {
        uint32_t h=G32(arr+4*i);types[i]=handle_type(h);order[i]=i;
        objects[i]=types[i]>=H_EVENT&&types[i]<=H_THREAD?handle_get(h,types[i]):NULL;
        if(!objects[i]) { SETERR(6); RET(c,0xffffffffu,4); return; }
        for(unsigned j=0;j<i;j++)if(objects[j]==objects[i]) { SETERR(87); RET(c,0xffffffffu,4); return; }
    }
    for(unsigned i=1;i<n;i++)for(unsigned j=i;j>0 && (uintptr_t)objects[order[j]]<(uintptr_t)objects[order[j-1]];j--) {
        unsigned swap=order[j];order[j]=order[j-1];order[j-1]=swap;
    }
    if(ms)diagnostic_state(c,all?"wait-all":"wait-any",arr);
    struct timespec start; clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {      // simple polling implementation: correct, not fast; revisit if it shows up in profiles
        if (all) {
            for(unsigned i=0;i<n;i++)pthread_mutex_lock(&objects[order[i]]->m);
            int ok=1;
            for(unsigned i=0;i<n;i++)if(!can_acquire(types[i],objects[i]))ok=0;
            if(ok)for(unsigned i=0;i<n;i++)try_acquire(types[i],objects[i]);
            for(unsigned i=n;i>0;i--)pthread_mutex_unlock(&objects[order[i-1]]->m);
            if (ok) { if(ms)diagnostic_state(c,"wait-returned",arr); RET(c, 0, 4); return; }
        } else {
            for (uint32_t i = 0; i < n; i++) if (wait_one(G32(arr + 4 * i), 0) == 0) { if(ms)diagnostic_state(c,"wait-returned",arr); RET(c, i, 4); return; }
        }
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t ns=(int64_t)(now.tv_sec-start.tv_sec)*1000000000ll+now.tv_nsec-start.tv_nsec;
        if (ms != 0xFFFFFFFFu && ns/1000000 >= ms) { if(ms)diagnostic_state(c,"wait-timeout",arr); RET(c, 0x102, 4); return; }
        usleep(200);
    }
}

// ---- critical sections: guest address -> host recursive mutex ---------------------------------
#define CS_BUCKETS 4096
typedef struct CSNode { uint32_t addr; pthread_mutex_t m; struct CSNode *next; } CSNode;
static CSNode *cs_tab[CS_BUCKETS];
static pthread_mutex_t cs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t *cs_get(uint32_t a) {
    uint32_t b = (a >> 3) % CS_BUCKETS;
    pthread_mutex_lock(&cs_lock);
    CSNode *n = cs_tab[b];
    while (n && n->addr != a) n = n->next;
    if (!n) {
        n = calloc(1, sizeof *n); n->addr = a;
        pthread_mutexattr_t at; pthread_mutexattr_init(&at); pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&n->m, &at);
        n->next = cs_tab[b]; cs_tab[b] = n;
    }
    pthread_mutex_unlock(&cs_lock);
    return &n->m;
}
SHIM(InitializeCriticalSection) { memset(MEM + ARG(c, 0), 0, 24); cs_get(ARG(c, 0)); RET(c, 0, 1); }
SHIM(InitializeCriticalSectionAndSpinCount) { memset(MEM + ARG(c, 0), 0, 24); cs_get(ARG(c, 0)); RET(c, 1, 2); }
SHIM(InitializeCriticalSectionEx) { memset(MEM + ARG(c, 0), 0, 24); cs_get(ARG(c, 0)); RET(c, 1, 3); }
SHIM(InitializeSListHead) { memset(MEM + ARG(c, 0), 0, 8); RET(c, 0, 1); }
SHIM(DeleteCriticalSection) { RET(c, 0, 1); }
SHIM(EnterCriticalSection) {
    uint32_t address=ARG(c,0);pthread_mutex_t *mutex=cs_get(address);
    if(pthread_mutex_trylock(mutex)) {
        diagnostic_state(c,"critical-section",address);
        pthread_mutex_lock(mutex);
        diagnostic_state(c,"critical-acquired",address);
    }
    RET(c,0,1);
}
SHIM(LeaveCriticalSection) { pthread_mutex_unlock(cs_get(ARG(c, 0))); RET(c, 0, 1); }
SHIM(TryEnterCriticalSection) { RET(c, pthread_mutex_trylock(cs_get(ARG(c, 0))) == 0, 1); }
SHIM(RtlInitializeCriticalSection) { memset(MEM + ARG(c, 0), 0, 24); cs_get(ARG(c, 0)); RET(c, 0, 1); }
SHIM(RtlEnterCriticalSection) { EnterCriticalSection_(c); }
SHIM(RtlLeaveCriticalSection) { LeaveCriticalSection_(c); }
SHIM(RtlDeleteCriticalSection) { RET(c, 0, 1); }
SHIM(RtlInitializeSListHead) { memset(MEM + ARG(c, 0), 0, 8); RET(c, 0, 1); }

SHIM(InterlockedPushEntrySList) {
    uint32_t head = ARG(c, 0), entry = ARG(c, 1);
    pthread_mutex_lock(&slist_lk);
    uint32_t old_next = G32(head);
    uint16_t depth = 0, seq = 0;
    memcpy(&depth, MEM + head + 4, 2);
    memcpy(&seq, MEM + head + 6, 2);
    P32(entry, old_next);
    P32(head, entry);
    depth++; seq++;
    memcpy(MEM + head + 4, &depth, 2);
    memcpy(MEM + head + 6, &seq, 2);
    pthread_mutex_unlock(&slist_lk);
    RET(c, old_next, 2);
}
SHIM(InterlockedFlushSList) {
    uint32_t head = ARG(c, 0);
    pthread_mutex_lock(&slist_lk);
    uint32_t old_next = G32(head);
    uint16_t zero = 0, seq = 0;
    memcpy(&seq, MEM + head + 6, 2);
    seq++;
    P32(head, 0);
    memcpy(MEM + head + 4, &zero, 2);
    memcpy(MEM + head + 6, &seq, 2);
    pthread_mutex_unlock(&slist_lk);
    RET(c, old_next, 1);
}

// ---- SRWLock, ConditionVariable, InitOnce, Threadpool stubs -----------------------------------
typedef struct SRWNode {
    uint32_t addr;
    pthread_mutex_t m;
    pthread_cond_t cv;
    int readers, writer, waiting_writers;
    struct SRWNode *next;
} SRWNode;
static SRWNode *srw_tab[1024];
static pthread_mutex_t srw_lk = PTHREAD_MUTEX_INITIALIZER;
static SRWNode *srw_get(uint32_t a) {
    uint32_t b = (a >> 2) % 1024;
    pthread_mutex_lock(&srw_lk);
    SRWNode *n = srw_tab[b];
    while (n && n->addr != a) n = n->next;
    if (!n) {
        n = calloc(1, sizeof *n);
        n->addr = a;
        pthread_mutex_init(&n->m, NULL);
        pthread_cond_init(&n->cv, NULL);
        n->next = srw_tab[b];
        srw_tab[b] = n;
    }
    pthread_mutex_unlock(&srw_lk);
    return n;
}
static void srw_acq_excl(SRWNode *s) {
    pthread_mutex_lock(&s->m);
    s->waiting_writers++;
    while (s->writer || s->readers > 0) pthread_cond_wait(&s->cv, &s->m);
    s->waiting_writers--;
    s->writer = 1;
    pthread_mutex_unlock(&s->m);
}
static void srw_rel_excl(SRWNode *s) {
    pthread_mutex_lock(&s->m);
    s->writer = 0;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->m);
}
static void srw_acq_shared(SRWNode *s) {
    pthread_mutex_lock(&s->m);
    while (s->writer || s->waiting_writers > 0) pthread_cond_wait(&s->cv, &s->m);
    s->readers++;
    pthread_mutex_unlock(&s->m);
}
static void srw_rel_shared(SRWNode *s) {
    pthread_mutex_lock(&s->m);
    if (--s->readers == 0) pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->m);
}
SHIM(InitializeSRWLock) { P32(ARG(c, 0), 0); srw_get(ARG(c, 0)); RET(c, 0, 1); }
SHIM(AcquireSRWLockExclusive) { srw_acq_excl(srw_get(ARG(c, 0))); RET(c, 0, 1); }
SHIM(ReleaseSRWLockExclusive) { srw_rel_excl(srw_get(ARG(c, 0))); RET(c, 0, 1); }
SHIM(AcquireSRWLockShared) { srw_acq_shared(srw_get(ARG(c, 0))); RET(c, 0, 1); }
SHIM(ReleaseSRWLockShared) { srw_rel_shared(srw_get(ARG(c, 0))); RET(c, 0, 1); }
SHIM(TryAcquireSRWLockExclusive) {
    SRWNode *s = srw_get(ARG(c, 0)); int ok = 0;
    pthread_mutex_lock(&s->m);
    if (!s->writer && s->readers == 0) { s->writer = 1; ok = 1; }
    pthread_mutex_unlock(&s->m);
    RET(c, ok, 1);
}
SHIM(TryAcquireSRWLockShared) {
    SRWNode *s = srw_get(ARG(c, 0)); int ok = 0;
    pthread_mutex_lock(&s->m);
    if (!s->writer && s->waiting_writers == 0) { s->readers++; ok = 1; }
    pthread_mutex_unlock(&s->m);
    RET(c, ok, 1);
}

typedef struct CVNode {
    uint32_t addr;
    pthread_mutex_t m;
    pthread_cond_t cv;
    struct CVNode *next;
} CVNode;
static CVNode *cv_tab[1024];
static pthread_mutex_t cv_lk = PTHREAD_MUTEX_INITIALIZER;
static CVNode *cv_get(uint32_t a) {
    uint32_t b = (a >> 2) % 1024;
    pthread_mutex_lock(&cv_lk);
    CVNode *n = cv_tab[b];
    while (n && n->addr != a) n = n->next;
    if (!n) {
        n = calloc(1, sizeof *n);
        n->addr = a;
        pthread_mutex_init(&n->m, NULL);
        pthread_cond_init(&n->cv, NULL);
        n->next = cv_tab[b];
        cv_tab[b] = n;
    }
    pthread_mutex_unlock(&cv_lk);
    return n;
}
SHIM(InitializeConditionVariable) { P32(ARG(c, 0), 0); cv_get(ARG(c, 0)); RET(c, 0, 1); }
SHIM(WakeConditionVariable) {
    CVNode *cv = cv_get(ARG(c, 0));
    pthread_mutex_lock(&cv->m); pthread_cond_signal(&cv->cv); pthread_mutex_unlock(&cv->m);
    RET(c, 0, 1);
}
SHIM(WakeAllConditionVariable) {
    CVNode *cv = cv_get(ARG(c, 0));
    pthread_mutex_lock(&cv->m); pthread_cond_broadcast(&cv->cv); pthread_mutex_unlock(&cv->m);
    RET(c, 0, 1);
}
SHIM(SleepConditionVariableSRW) {
    CVNode *cv = cv_get(ARG(c, 0));
    SRWNode *s = srw_get(ARG(c, 1));
    uint32_t ms = ARG(c, 2), flags = ARG(c, 3);
    struct timespec ts; deadline(&ts, ms);
    pthread_mutex_lock(&cv->m);
    if (flags & 1) srw_rel_shared(s); else srw_rel_excl(s);
    int r = (ms == 0xFFFFFFFFu) ? pthread_cond_wait(&cv->cv, &cv->m)
            : (ms == 0) ? ETIMEDOUT : pthread_cond_timedwait(&cv->cv, &cv->m, &ts);
    pthread_mutex_unlock(&cv->m);
    if (flags & 1) srw_acq_shared(s); else srw_acq_excl(s);
    if (r == ETIMEDOUT) { SETERR(1460); RET(c, 0, 4); return; }
    RET(c, 1, 4);
}
SHIM(SleepConditionVariableCS) {
    CVNode *cv = cv_get(ARG(c, 0));
    pthread_mutex_t *cs = cs_get(ARG(c, 1));
    uint32_t ms = ARG(c, 2);
    struct timespec ts; deadline(&ts, ms);
    pthread_mutex_lock(&cv->m);
    pthread_mutex_unlock(cs);
    int r = (ms == 0xFFFFFFFFu) ? pthread_cond_wait(&cv->cv, &cv->m)
            : (ms == 0) ? ETIMEDOUT : pthread_cond_timedwait(&cv->cv, &cv->m, &ts);
    pthread_mutex_unlock(&cv->m);
    pthread_mutex_lock(cs);
    if (r == ETIMEDOUT) { SETERR(1460); RET(c, 0, 3); return; }
    RET(c, 1, 3);
}

typedef struct { uint32_t addr, done, ctx; } InitOnceEntry;
static InitOnceEntry init_once_tab[256];
static int init_once_count;
static pthread_mutex_t init_once_lk = PTHREAD_MUTEX_INITIALIZER;
SHIM(InitOnceExecuteOnce) {
    uint32_t once = ARG(c, 0), fn = ARG(c, 1), param = ARG(c, 2), ctx = ARG(c, 3);
    pthread_mutex_lock(&init_once_lk);
    InitOnceEntry *ent = NULL;
    for (int i = 0; i < init_once_count; i++) {
        if (init_once_tab[i].addr == once) { ent = &init_once_tab[i]; break; }
    }
    if (!ent && init_once_count < 256) {
        ent = &init_once_tab[init_once_count++];
        ent->addr = once;
    }
    if (ent && ent->done) {
        if (ctx) P32(ctx, ent->ctx);
        pthread_mutex_unlock(&init_once_lk);
        RET(c, 1, 4);
        return;
    }
    pthread_mutex_unlock(&init_once_lk);
    uint32_t esp = REG(c, R_ESP), sp = esp - 64;
    uint32_t tmp_ctx = sp + 32;
    P32(tmp_ctx, ctx ? G32(ctx) : 0);
    P32(sp, HOST_EXIT_THUNK); P32(sp + 4, once); P32(sp + 8, param); P32(sp + 12, tmp_ctx);
    SETREG(c, R_ESP, sp);
    rt_call(c, fn);
    uint32_t ok = REG(c, R_EAX);
    SETREG(c, R_ESP, esp);
    uint32_t out_ctx = G32(tmp_ctx);
    if (ctx) P32(ctx, out_ctx);
    if (ok && ent) {
        pthread_mutex_lock(&init_once_lk);
        ent->done = 1; ent->ctx = out_ctx;
        P32(once, 2);
        pthread_mutex_unlock(&init_once_lk);
    }
    RET(c, ok != 0, 4);
}

static struct { uint32_t fn, ctx; int active; } tp_works[256];
static pthread_mutex_t tp_lk = PTHREAD_MUTEX_INITIALIZER;
SHIM(CreateThreadpoolWork) {
    uint32_t fn = ARG(c, 0), ctx = ARG(c, 1);
    uint32_t h = 0;
    pthread_mutex_lock(&tp_lk);
    for (int i = 0; i < 256; i++) {
        if (!tp_works[i].active) {
            tp_works[i].fn = fn;
            tp_works[i].ctx = ctx;
            tp_works[i].active = 1;
            h = 0x00071000u + (uint32_t)i;
            break;
        }
    }
    pthread_mutex_unlock(&tp_lk);
    RET(c, h ? h : 0x00070001u, 3);
}
SHIM(SubmitThreadpoolWork) {
    uint32_t h = ARG(c, 0), fn = 0, ctx = 0;
    if (h >= 0x00071000u && h < 0x00071100u) {
        pthread_mutex_lock(&tp_lk);
        uint32_t idx = h - 0x00071000u;
        if (tp_works[idx].active) { fn = tp_works[idx].fn; ctx = tp_works[idx].ctx; }
        pthread_mutex_unlock(&tp_lk);
    }
    if (fn) {
        uint32_t esp = REG(c, R_ESP);
        uint32_t sp = esp - 64;
        P32(sp, HOST_EXIT_THUNK);
        P32(sp + 4, 0);
        P32(sp + 8, ctx);
        P32(sp + 12, h);
        SETREG(c, R_ESP, sp);
        rt_call(c, fn);
        SETREG(c, R_ESP, esp);
    }
    RET(c, 0, 1);
}
SHIM(WaitForThreadpoolWorkCallbacks) { RET(c, 0, 2); }
SHIM(CloseThreadpoolWork) {
    uint32_t h = ARG(c, 0);
    if (h >= 0x00071000u && h < 0x00071100u) {
        pthread_mutex_lock(&tp_lk);
        tp_works[h - 0x00071000u].active = 0;
        pthread_mutex_unlock(&tp_lk);
    }
    RET(c, 0, 1);
}
SHIM(CreateThreadpoolTimer) { RET(c, 0x00070002u, 3); }
SHIM(SetThreadpoolTimer) { RET(c, 0, 4); }
SHIM(WaitForThreadpoolTimerCallbacks) { RET(c, 0, 2); }
SHIM(CloseThreadpoolTimer) { RET(c, 0, 1); }
SHIM(CreateThreadpoolWait) { RET(c, 0x00070003u, 3); }
SHIM(SetThreadpoolWait) { RET(c, 0, 3); }
SHIM(CloseThreadpoolWait) { RET(c, 0, 1); }
SHIM(FreeLibraryWhenCallbackReturns) { RET(c, 0, 2); }

// interlocked: real atomics on guest memory (with mutex fallback for unaligned guest addresses on ARM64)
static pthread_mutex_t unaligned_ilock = PTHREAD_MUTEX_INITIALIZER;
enum { IL_ADD, IL_XCHG, IL_CMPXCHG };
static uint32_t il_unaligned(uint32_t a, int op, uint32_t val, uint32_t exp) {
    pthread_mutex_lock(&unaligned_ilock);
    uint32_t cur = 0, next = val, ret;
    memcpy(&cur, MEM + a, 4);
    if (op == IL_ADD) { next = cur + val; ret = next; }
    else if (op == IL_XCHG) { ret = cur; }
    else { ret = cur; if (cur != exp) next = cur; }
    memcpy(MEM + a, &next, 4);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&unaligned_ilock);
    return ret;
}
SHIM(InterlockedIncrement) {
    uint32_t a = ARG(c, 0);
    RET(c, !(a & 3) ? __atomic_add_fetch((uint32_t *)(MEM + a), 1, __ATOMIC_SEQ_CST)
                    : il_unaligned(a, IL_ADD, 1, 0), 1);
}
SHIM(InterlockedDecrement) {
    uint32_t a = ARG(c, 0);
    RET(c, !(a & 3) ? __atomic_sub_fetch((uint32_t *)(MEM + a), 1, __ATOMIC_SEQ_CST)
                    : il_unaligned(a, IL_ADD, (uint32_t)-1, 0), 1);
}
SHIM(InterlockedExchange) {
    uint32_t a = ARG(c, 0), v = ARG(c, 1);
    RET(c, !(a & 3) ? __atomic_exchange_n((uint32_t *)(MEM + a), v, __ATOMIC_SEQ_CST)
                    : il_unaligned(a, IL_XCHG, v, 0), 2);
}
SHIM(InterlockedExchangeAdd) {
    uint32_t a = ARG(c, 0), v = ARG(c, 1);
    RET(c, !(a & 3) ? __atomic_fetch_add((uint32_t *)(MEM + a), v, __ATOMIC_SEQ_CST)
                    : il_unaligned(a, IL_ADD, v, 0) - v, 2);
}
SHIM(InterlockedCompareExchange) {
    uint32_t a = ARG(c, 0), v = ARG(c, 1), exp = ARG(c, 2);
    if (!(a & 3)) {
        __atomic_compare_exchange_n((uint32_t *)(MEM + a), &exp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    } else {
        exp = il_unaligned(a, IL_CMPXCHG, v, exp);
    }
    RET(c, exp, 3);
}

// ---- threads ---------------------------------------------------------------------------------
typedef struct { uint32_t start, param; Obj *obj; uint32_t tid; CPU *cpu; uint32_t exit_code; } ThreadStart;
static _Thread_local ThreadStart *current_thread;
static uint32_t next_tid = 2;
extern int host_run_guest(CPU *c, uint32_t entry);   // host.c: trap-guarded call

static void thread_finished(void *p) {
    ThreadStart *ts=p;
    pthread_mutex_lock(&ts->obj->m);
    ts->obj->done=1;ts->obj->exit_code=ts->exit_code;
    pthread_cond_broadcast(&ts->obj->cv);
    pthread_mutex_unlock(&ts->obj->m);
    fprintf(stderr,"[thread] completed tid=%u start=%#x exit=%#x\n",ts->tid,ts->start,ts->exit_code);
    if(ts->cpu)diagnostic_state(ts->cpu,"completed",ts->exit_code);
    current_thread=NULL;free(ts->cpu);free(ts);
}
static void *thread_main(void *p) {
    ThreadStart *ts = p;
    current_thread=ts;
    pthread_cleanup_push(thread_finished,ts);
    if (!getenv("FNV_NO_QOS")) pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);   // game workers
    pthread_mutex_lock(&ts->obj->m);                      // CREATE_SUSPENDED: wait for ResumeThread
    while (ts->obj->suspend > 0) pthread_cond_wait(&ts->obj->cv, &ts->obj->m);
    pthread_mutex_unlock(&ts->obj->m);
    CPU *c = calloc(1, sizeof *c);
    ts->cpu=c;
    host_thread_init(c, 1 << 20);
    P32(REG(c, 272) + 0x24, ts->tid);
    ST(c->r + 4256, 0x027F, 2);
    uint32_t esp = REG(c, R_ESP) - 8;
    P32(esp, HOST_EXIT_THUNK); P32(esp + 4, ts->param);
    SETREG(c, R_ESP, esp);
    diagnostic_state(c,"started",ts->start);
    int result=host_run_guest(c, ts->start);
    ts->exit_code=result?1067:REG(c,R_EAX);
    pthread_cleanup_pop(1);
    return NULL;
}
SHIM(CreateThread) {
    ThreadStart *ts = calloc(1, sizeof *ts);
    ts->start = ARG(c, 2); ts->param = ARG(c, 3); ts->obj = obj_new();
    ts->tid = __atomic_fetch_add(&next_tid, 1, __ATOMIC_SEQ_CST);
    if (ARG(c, 4) & 4) ts->obj->suspend = 1;
    uint32_t h = handle_new(H_THREAD, ts->obj);
    uint32_t tid=ts->tid,start=ts->start,param=ts->param;
    pthread_attr_t at; pthread_attr_init(&at);
    int error=pthread_attr_setstacksize(&at,64u<<20);
    pthread_t th;
    if(!error)error=pthread_create(&th,&at,thread_main,ts);
    pthread_attr_destroy(&at);
    if(error) {
        fprintf(stderr,"[thread] CreateThread FAILED start=%#x pthread_error=%d (%s)\n",start,error,strerror(error));
        handle_close(h);pthread_cond_destroy(&ts->obj->cv);pthread_mutex_destroy(&ts->obj->m);free(ts->obj);free(ts);
        SETERR(error==EINVAL?87:8);RET(c,0,6);return;
    }
    pthread_detach(th);
    if (ARG(c, 5)) P32(ARG(c, 5), tid);
    fprintf(stderr, "[thread] CreateThread start %#x param %#x flags %#x -> handle %#x tid %u\n",start,param,ARG(c,4),h,tid);
    RET(c, h, 6);
}
SHIM(ResumeThread) {
    Obj *o = handle_get(ARG(c, 0), H_THREAD);
    fprintf(stderr, "[thread] ResumeThread(%#x) %s\n", ARG(c, 0), o ? "" : "-> invalid handle");
    if (!o) { RET(c, 0xFFFFFFFFu, 1); return; }
    pthread_mutex_lock(&o->m);
    int prev = o->suspend;
    if (o->suspend > 0) o->suspend--;
    pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->m);
    RET(c, (uint32_t)prev, 1);
}
// Suspending a running thread asynchronously is not supported; only the count is tracked.
SHIM(SuspendThread) { Obj *o = handle_get(ARG(c, 0), H_THREAD); RET(c, o ? (uint32_t)o->suspend : 0xFFFFFFFFu, 1); }
SHIM(SetThreadPriority) { RET(c, 1, 2); }
SHIM(SetThreadIdealProcessor) { RET(c, 0, 2); }
SHIM(GetExitCodeThread) {
    Obj *o = handle_get(ARG(c, 0), H_THREAD);
    if (o) P32(ARG(c, 1), o->done ? o->exit_code : 259 /* STILL_ACTIVE */);
    RET(c, o != NULL, 2);
}
SHIM(ExitThread) {
    uint32_t code=ARG(c,0);
    fprintf(stderr,"[host] ExitThread(%u)\n",code);
    if(current_thread)current_thread->exit_code=code;
    pthread_exit(NULL);
}
SHIM(FreeLibraryAndExitThread) {
    uint32_t code=ARG(c,1);
    if(current_thread)current_thread->exit_code=code;
    pthread_exit(NULL);
}
SHIM(CloseHandle) {
    uint32_t h = ARG(c, 0);
    if (handle_type(h) == H_THREAD) {
        fprintf(stderr, "[thread] CloseHandle(%#x) thread, caller %#x\n", h, G32(REG(c, R_ESP)));
        static int dumped;
        if (!dumped++) {
            uint32_t sp = REG(c, R_ESP);
            fprintf(stderr, "[thread]   stack:");
            for (int i = 0, n = 0; i < 2048 && n < 24; i++) { uint32_t v = G32(sp + 4 * (uint32_t)i); if (v >= 0x401000 && v < 0x1080000) { fprintf(stderr, " %#x", v); n++; } }
            fprintf(stderr, "\n");
        }
    }
    handle_close(h); RET(c, 1, 1);
}

SHIM(WaitOnAddress) {
    uint32_t addr = ARG(c, 0), cmp = ARG(c, 1), sz = ARG(c, 2), ms = ARG(c, 3);
    if (!addr || !cmp || (sz != 1 && sz != 2 && sz != 4 && sz != 8)) {
        SETERR(87);
        RET(c, 0, 4);
        return;
    }
    struct timespec start;
    if (ms != 0xFFFFFFFFu) clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        uint64_t cur = 0, want = 0;
        memcpy(&cur, MEM + addr, sz);
        memcpy(&want, MEM + cmp, sz);
        if (cur != want) { RET(c, 1, 4); return; }
        if (ms == 0) { SETERR(1460); RET(c, 0, 4); return; }
        if (ms != 0xFFFFFFFFu) {
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            int64_t el_ms = ((int64_t)(now.tv_sec - start.tv_sec) * 1000ll) + ((now.tv_nsec - start.tv_nsec) / 1000000ll);
            if (el_ms >= (int64_t)ms) { SETERR(1460); RET(c, 0, 4); return; }
        }
        usleep(100);
    }
}
SHIM(WakeByAddressSingle) { (void)ARG(c, 0); RET(c, 0, 1); }
SHIM(WakeByAddressAll) { (void)ARG(c, 0); RET(c, 0, 1); }

SHIM_TABLE(sync_shims,
    E(CreateEventA) E(CreateEventW) E(CreateEventExW) E(OpenEventA) E(SetEvent) E(ResetEvent)
    E(CreateSemaphoreA) E(CreateSemaphoreExW) E(ReleaseSemaphore)
    E(CreateMutexA) E(OpenMutexA) E(ReleaseMutex) E(SleepEx)
    E(WaitForSingleObject) E(WaitForSingleObjectEx) E(WaitForMultipleObjects) E(InitializeCriticalSection)
    E(InitializeCriticalSectionAndSpinCount) E(InitializeCriticalSectionEx) E(InitializeSListHead)
    E(DeleteCriticalSection) E(EnterCriticalSection)
    E(LeaveCriticalSection) E(TryEnterCriticalSection)
    E(RtlInitializeCriticalSection) E(RtlEnterCriticalSection) E(RtlLeaveCriticalSection) E(RtlDeleteCriticalSection)
    E(RtlInitializeSListHead) E(InterlockedPushEntrySList) E(InterlockedFlushSList)
    E(InitializeSRWLock) E(AcquireSRWLockExclusive) E(ReleaseSRWLockExclusive)
    E(AcquireSRWLockShared) E(ReleaseSRWLockShared) E(TryAcquireSRWLockExclusive) E(TryAcquireSRWLockShared)
    E(InitializeConditionVariable) E(WakeConditionVariable) E(WakeAllConditionVariable)
    E(SleepConditionVariableSRW) E(SleepConditionVariableCS) E(InitOnceExecuteOnce)
    E(CreateThreadpoolWork) E(SubmitThreadpoolWork) E(WaitForThreadpoolWorkCallbacks) E(CloseThreadpoolWork)
    E(CreateThreadpoolTimer) E(SetThreadpoolTimer) E(WaitForThreadpoolTimerCallbacks) E(CloseThreadpoolTimer)
    E(CreateThreadpoolWait) E(SetThreadpoolWait) E(CloseThreadpoolWait) E(FreeLibraryWhenCallbackReturns)
    E(WaitOnAddress) E(WakeByAddressSingle) E(WakeByAddressAll)
    E(InterlockedIncrement) E(InterlockedDecrement)
    E(InterlockedExchange) E(InterlockedExchangeAdd) E(InterlockedCompareExchange) E(CreateThread)
    E(ResumeThread) E(SuspendThread) E(SetThreadPriority) E(SetThreadIdealProcessor) E(GetExitCodeThread)
    E(ExitThread) E(FreeLibraryAndExitThread) E(CloseHandle))

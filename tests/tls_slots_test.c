// TlsAlloc/TlsFree slot reuse. The engine frees and reallocates 8 dynamic TLS slots per save load;
// before slots were reused, the 5th load exhausted all 64 and the task queue's hazard-pointer
// context broke (TRAP at 0xc3dd68).
#include "FNVGuestMemory.h"
#include "../upstream/recomp/host/gheap.c"
#include "../upstream/recomp/host/shims_kernel32.c"
#include <assert.h>
uint8_t *MEM;uint32_t host_image_base=0x400000;
uint8_t *host_tls_template;uint32_t host_tls_template_size,host_tls_block_size;

// stdcall into a shim with n dword arguments, returning EAX; checks the shim popped its arguments
static uint32_t call(CPU *c,void(*fn)(CPU*),int n,uint32_t a0,uint32_t a1){
    uint32_t sp=REG(c,R_ESP)-16;SETREG(c,R_ESP,sp);P32(sp,0x123456);P32(sp+4,a0);P32(sp+8,a1);
    fn(c);assert(REG(c,R_ESP)==sp+4+4*(uint32_t)n);SETREG(c,R_ESP,sp+16);return REG(c,R_EAX);
}
#define ALLOC(c) call(c,TlsAlloc_,0,0,0)
#define FREE(c,i) call(c,TlsFree_,1,i,0)
#define GET(c,i) call(c,TlsGetValue_,1,i,0)
#define SET(c,i,v) call(c,TlsSetValue_,2,i,v)

int main(void){
    MEM=fnv_reserve_guest();assert(MEM!=MAP_FAILED);
    static CPU a,b;host_thread_init(&a,1<<16);host_thread_init(&b,1<<16);
    // repeated free + realloc, as on every save load, never runs out
    uint32_t base[30];for(int i=0;i<30;i++){base[i]=ALLOC(&a);assert(base[i]<64);}
    uint32_t live[8];for(int i=0;i<8;i++)live[i]=ALLOC(&a);
    for(int load=0;load<1000;load++)for(int i=0;i<8;i++){assert(FREE(&a,live[i])==1);live[i]=ALLOC(&a);assert(live[i]!=0xFFFFFFFFu);}
    // a reallocated slot reads as 0 on every thread, including values another thread left behind
    uint32_t s=live[0];assert(SET(&a,s,0x1111)==1&&SET(&b,s,0x2222)==1);assert(GET(&a,s)==0x1111&&GET(&b,s)==0x2222);
    assert(FREE(&a,s)==1);uint32_t t=ALLOC(&b);assert(t==s);assert(GET(&a,t)==0&&GET(&b,t)==0);
    // slots are per thread
    assert(SET(&b,t,7)==1&&GET(&a,t)==0&&GET(&b,t)==7);
    // double free and out-of-range indexes fail with ERROR_INVALID_PARAMETER
    assert(FREE(&a,t)==1);assert(FREE(&a,t)==0&&last_error==87);assert(FREE(&a,64)==0);assert(FREE(&a,0xFFFFFFFFu)==0);
    assert(SET(&a,0xFFFFFFFFu,1)==0&&last_error==87);assert(GET(&a,0xFFFFFFFFu)==0&&last_error==87);
    // when all 64 are in use TlsAlloc returns TLS_OUT_OF_INDEXES
    int n=0;while(ALLOC(&a)!=0xFFFFFFFFu)n++;assert(n==64-30-7);
    assert(FREE(&a,base[3])==1);assert(ALLOC(&a)==base[3]);
    puts("tls_slots_test: PASS");return 0;
}

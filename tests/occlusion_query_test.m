// Real-GPU Metal hardware occlusion & event query tests through the D3D9 COM layer.
#import <Foundation/Foundation.h>
#include <unistd.h>
#include "FNVGuestMemory.h"
#include "shims.h"
#ifndef FNV_METAL_SOURCE
#define FNV_METAL_SOURCE "../upstream/recomp/host/d3d9_metal.m"
#endif
#include FNV_METAL_SOURCE
#include <assert.h>

uint8_t *MEM;
HostImport host_imports[MAX_IMPORTS];
int host_import_count;
extern D3DState g_d3d;
extern const ShimTable d3d9_shims;
static uint32_t test_stack, device, dummy_tex;
static int failures;
enum { TEST_W = 128, TEST_H = 96 };

CAMetalLayer *fnv_surface(id<MTLDevice> d, CGSize s) { (void)d; (void)s; abort(); }
void fnv_resize_surface(CGSize s) { (void)s; }
void fnv_wait_active(void) {}
void input_script_tick(uint64_t f) { (void)f; }
void input_frame_draws(uint64_t n) { (void)n; }
int input_take_dump_request(void) { return 0; }
void rt_trap(CPU *c, int kind, uint32_t address) { (void)c; fprintf(stderr, "unexpected trap %d at %#x\n", kind, address); abort(); }

static void check(const char *name, int ok) { printf("%s: %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; }
static uint32_t invoke(ShimFn fn, const uint32_t *args, unsigned n) {
    CPU c = {0}; SETREG(&c, R_ESP, test_stack); P32(test_stack, 0x12345678);
    for (unsigned i = 0; i < n; i++) P32(test_stack + 4 + 4 * i, args[i]);
    fn(&c); assert(REG(&c, R_ESP) == test_stack + 4 + 4 * n); return REG(&c, R_EAX);
}
static uint32_t method(uint32_t object, unsigned slot, const uint32_t *extra, unsigned n) {
    uint32_t thunk = G32(G32(object) + 4 * slot), args[16] = {object};
    assert(n < 16 && thunk >= IMPORT_THUNK_BASE && (thunk - IMPORT_THUNK_BASE) % 4 == 0);
    unsigned i = (thunk - IMPORT_THUNK_BASE) / 4; assert(i < (unsigned)host_import_count);
    for (unsigned j = 0; j < n; j++) args[j + 1] = extra[j];
    return invoke(host_imports[i].fn, args, n + 1);
}
#define COM(o, s, ...) method(o, s, (uint32_t[]){__VA_ARGS__}, sizeof((uint32_t[]){__VA_ARGS__}) / 4)
#define HR(o, s, ...) assert(COM(o, s, __VA_ARGS__) == 0)
static uint32_t float_bits(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }

static void create_device(void) {
    uint32_t d3d = 0;
    for (int i = 0; i < d3d9_shims.count; i++) if (!strcmp(d3d9_shims.list[i].name, "Direct3DCreate9"))
        d3d = invoke(d3d9_shims.list[i].fn, (uint32_t[]){32}, 1);
    assert(d3d);
    uint32_t pp = gmalloc(56), out = gmalloc(4); memset(MEM + pp, 0, 56);
    P32(pp, TEST_W); P32(pp + 4, TEST_H); P32(pp + 8, 21); P32(pp + 12, 1);
    P32(pp + 32, 1); P32(pp + 40, 1); P32(pp + 44, 75); // EnableAutoDepthStencil + D24S8
    HR(d3d, 16, 0, 1, 0, 0, pp, out); device = G32(out); assert(device);
    gfree(pp); gfree(out);

    out = gmalloc(4);
    HR(device, 23, 4, 4, 1, 0, 21, 0, out, 0);
    dummy_tex = G32(out); assert(dummy_tex);
    uint32_t locked = gmalloc(8); HR(dummy_tex, 19, 0, locked, 0, 0);
    uint32_t pitch = G32(locked), px = G32(locked + 4);
    for (unsigned y = 0; y < 4; y++) for (unsigned x = 0; x < 4; x++) P32(px + y * pitch + 4 * x, 0xffffffffu);
    HR(dummy_tex, 20, 0);
    gfree(locked); gfree(out);
}

typedef struct { float x, y, z, rhw, u, v; } ScreenVertex;
static void draw_quad(float left, float top, float right, float bottom, float z, uint32_t color_mask, uint32_t z_write) {
    ScreenVertex verts[4] = {
        {left, top, z, 1, 0, 0}, {left, bottom, z, 1, 0, 1},
        {right, top, z, 1, 1, 0}, {right, bottom, z, 1, 1, 1},
    };
    uint32_t data = gmalloc(sizeof verts);
    memcpy(MEM + data, verts, sizeof verts);
    HR(device, 57, 27, 0);              // AlphaBlendEnable = 0
    HR(device, 57, 7, 1);               // ZEnable = 1
    HR(device, 57, 14, z_write);        // ZWriteEnable
    HR(device, 57, 23, 4);              // ZFunc = LessEqual
    HR(device, 57, 168, color_mask);    // ColorWriteEnable
    HR(device, 57, 22, 1);              // CullMode = None
    HR(device, 65, 0, dummy_tex);
    HR(device, 67, 0, 4, 2); HR(device, 67, 0, 2, 2); HR(device, 67, 0, 5, 2); HR(device, 67, 0, 1, 4); HR(device, 67, 0, 3, 1);
    HR(device, 69, 0, 6, 1); HR(device, 69, 0, 5, 1); HR(device, 69, 0, 7, 0); HR(device, 69, 0, 1, 3); HR(device, 69, 0, 2, 3);
    HR(device, 92, 0); HR(device, 107, 0);
    HR(device, 89, 0x104);
    HR(device, 83, 5, 2, data, sizeof(ScreenVertex));
    HR(device, 89, 0);
    gfree(data);
}

static uint32_t poll_query(uint32_t q) {
    uint32_t out = gmalloc(4);
    P32(out, 0xdeadbeefu);
    uint32_t rc = 1;
    for (int i = 0; i < 500; i++) {
        rc = COM(q, 7, out, 4, 1); // GetData(out, 4, D3DGETDATA_FLUSH)
        if (rc == 0) break;
        usleep(1000);
    }
    uint32_t val = G32(out);
    gfree(out);
    assert(rc == 0);
    return val;
}

int main(void) { @autoreleasepool {
    setenv("FNV_HEADLESS", "1", 1); unsetenv("FNV_RECORD"); unsetenv("FNV_DUMP_FRAMES");
    id<MTLDevice> available = MTLCreateSystemDefaultDevice();
    if (!available) {
        puts("SKIP: no Metal device; occlusion query GPU checks skipped.");
        return 77;
    }
    MEM = fnv_reserve_guest(); assert(MEM != MAP_FAILED);
    test_stack = gmalloc(256);
    create_device();

    // 1. Support probes
    check("CreateQuery supports occlusion probe", COM(device, 118, 9, 0) == 0);
    check("CreateQuery supports event probe", COM(device, 118, 8, 0) == 0);
    check("CreateQuery rejects unsupported query type", COM(device, 118, 1, 0) == 0x8876086Au);

    uint32_t qout = gmalloc(4);
    HR(device, 118, 9, qout); uint32_t q_unocc = G32(qout);
    HR(device, 118, 9, qout); uint32_t q_full_occ = G32(qout);
    HR(device, 118, 9, qout); uint32_t q_half_occ = G32(qout);
    HR(device, 118, 9, qout); uint32_t q_empty = G32(qout);
    HR(device, 118, 8, qout); uint32_t q_ev = G32(qout);
    gfree(qout);

    // Clear color + depth (1.0)
    HR(device, 43, 0, 0, 3, 0xff000000u, float_bits(1.0f), 0);
    HR(device, 41);

    // 2. Unoccluded 32x32 quad at [32..64, 32..64], z=0.8
    HR(q_unocc, 6, 2); // BEGIN
    draw_quad(32, 32, 64, 64, 0.8f, 0, 0);
    HR(q_unocc, 6, 1); // END

    // 3. Opaque closer wall at [32..64, 32..64], z=0.2 (writes depth)
    draw_quad(32, 32, 64, 64, 0.2f, 15, 1);

    // 4. Fully occluded 32x32 quad behind wall at z=0.8
    HR(q_full_occ, 6, 2);
    draw_quad(32, 32, 64, 64, 0.8f, 0, 0);
    HR(q_full_occ, 6, 1);

    // 5. Half-occluded 32x32 quad at [48..80, 32..64], z=0.8 (16x32 behind wall, 16x32 open)
    HR(q_half_occ, 6, 2);
    draw_quad(48, 32, 80, 64, 0.8f, 0, 0);
    HR(q_half_occ, 6, 1);

    // 6. Empty occlusion query (0 draws)
    HR(q_empty, 6, 2);
    HR(q_empty, 6, 1);

    // 7. Event query
    HR(q_ev, 6, 1);
    HR(device, 42);

    uint32_t v_unocc = poll_query(q_unocc);
    uint32_t v_full = poll_query(q_full_occ);
    uint32_t v_half = poll_query(q_half_occ);
    uint32_t v_empty = poll_query(q_empty);
    uint32_t v_ev = poll_query(q_ev);

    printf("occlusion counts: unoccluded=%u occluded=%u half=%u empty=%u event=%u\n",
           v_unocc, v_full, v_half, v_empty, v_ev);
    check("unoccluded 32x32 quad counts 1024 pixels", v_unocc == 1024);
    check("fully occluded quad behind wall counts 0 pixels", v_full == 0);
    check("half-occluded 32x32 quad counts 512 pixels", v_half == 512);
    check("empty occlusion query counts 0 pixels", v_empty == 0);
    check("event query completes with 1", v_ev == 1);

    // 8. Edge cases: EndIssue::Keep (duplicate END preserves resolved result)
    HR(q_unocc, 6, 1);
    check("duplicate END preserves prior unoccluded count", poll_query(q_unocc) == 1024);

    // 9. Edge cases: open query (BEGIN without END) returns S_FALSE, then END resolves
    qout = gmalloc(4);
    HR(device, 118, 9, qout); uint32_t q_open = G32(qout);
    HR(device, 118, 9, qout); uint32_t q_both = G32(qout);
    HR(device, 118, 9, qout); uint32_t q_mp1 = G32(qout);
    HR(device, 118, 9, qout); uint32_t q_mp2 = G32(qout);

    HR(q_open, 6, 2); // BEGIN
    draw_quad(0, 0, 16, 16, 0.5f, 0, 0);
    P32(qout, 0);
    check("open query between BEGIN and END returns S_FALSE", COM(q_open, 7, qout, 4, 0) == 1);
    HR(q_open, 6, 1); // END
    check("open query resolves 256 pixels after END", poll_query(q_open) == 256);

    // 10. Edge cases: combined D3DISSUE_BEGIN | D3DISSUE_END in one call
    HR(q_both, 6, 3);
    check("combined BEGIN|END resolves to 0", poll_query(q_both) == 0);

    // 11. Multi-render-pass test in the same command buffer (regression test for Apple Silicon
    //     MTLVisibilityResultModeDisabled(offset: 0) clobbering pass 1's slot 0 count).
    uint32_t tex_out = gmalloc(4), bb_out = gmalloc(4), rt2_out = gmalloc(4);
    HR(device, 23, TEST_W, TEST_H, 1, 1 /* D3DUSAGE_RENDERTARGET */, 21 /* A8R8G8B8 */, 0, tex_out, 0);
    uint32_t rt2_tex = G32(tex_out); assert(rt2_tex);
    HR(rt2_tex, 18, 0, rt2_out); uint32_t rt2_surf = G32(rt2_out); assert(rt2_surf);
    HR(device, 38, 0, bb_out); uint32_t bb_surf = G32(bb_out); assert(bb_surf);

    HR(device, 43, 0, 0, 3, 0xff000000u, float_bits(1.0f), 0);
    HR(device, 41);
    HR(q_mp1, 6, 2);
    draw_quad(32, 32, 64, 64, 0.8f, 0, 0);
    HR(q_mp1, 6, 1);

    // Switch render target and draw to force end_pass() + new Metal render pass
    HR(device, 37, 0, rt2_surf);
    HR(device, 43, 0, 0, 1, 0xff000000u, float_bits(1.0f), 0);
    draw_quad(0, 0, 16, 16, 0.5f, 15, 0);
    HR(device, 37, 0, bb_surf);

    // Second query back on the backbuffer within the same command buffer
    HR(q_mp2, 6, 2);
    draw_quad(32, 32, 64, 64, 0.8f, 0, 0);
    HR(q_mp2, 6, 1);
    HR(device, 42);

    uint32_t v_mp1 = poll_query(q_mp1), v_mp2 = poll_query(q_mp2);
    printf("multi-pass counts: mp1=%u mp2=%u\n", v_mp1, v_mp2);
    check("multi-pass pass 1 query keeps 1024 pixels", v_mp1 == 1024);
    check("multi-pass pass 2 query counts 1024 pixels", v_mp2 == 1024);

    COM(rt2_surf, 2); COM(rt2_tex, 2); COM(bb_surf, 2);
    COM(q_open, 2); COM(q_both, 2); COM(q_mp1, 2); COM(q_mp2, 2);
    gfree(tex_out); gfree(bb_out); gfree(rt2_out); gfree(qout);

    COM(q_unocc, 2); COM(q_full_occ, 2); COM(q_half_occ, 2); COM(q_empty, 2); COM(q_ev, 2);
    COM(dummy_tex, 2);
    return failures ? 1 : 0;
}}

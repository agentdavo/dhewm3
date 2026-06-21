/*
 * cmask_validate.c — bare-metal RVV gate for the colour-mask shading early-out.
 *
 * Doom 3's stencil / shadow-volume passes draw screen-filling geometry with
 * glColorMask(0,0,0,0). sgl_raster_tri_prog's batched path now skips the varying
 * gather AND the fragment-VM dispatch entirely when color_mask==0, while still
 * resolving coverage + early-Z and laying down depth when depth_write is set.
 *
 * The host can't run RVV, so this validates the early-out on QEMU (-cpu rva23s64)
 * by driving the rasteriser with a MOCK batched shader that counts its own calls:
 *   1. cmask=0xF, zwrite=1  -> colour written, depth updated, VM called
 *   2. cmask=0,   zwrite=1  -> colour UNCHANGED, depth updated, VM NOT called
 *   3. cmask=0,   zwrite=0  -> colour + depth both UNCHANGED, VM NOT called
 *
 * Linked with boot + lean libc + fb.c + raster.c (no glsl.c — the mock stands in
 * for the fragment VM, so this isolates the rasteriser's gating logic).
 */
#include "libc/libc.h"
#include "boot/runtime.h"          /* sbi_shutdown */
#include "sgl.h"
#include "sg_core.h"
#include "sg_raster.h"

/* mock batched fragment shader: paints every lane red, counts invocations. */
static int g_vm_calls = 0;
static int mock_frag_simt(void *ctx, const float *varys, int vl, float *out)
{
    (void)ctx; (void)varys;
    ++g_vm_calls;
    for (int L = 0; L < vl; ++L) {
        out[0*SGL_SIMT_LANES + L] = 1.f;  /* R */
        out[1*SGL_SIMT_LANES + L] = 0.f;  /* G */
        out[2*SGL_SIMT_LANES + L] = 0.f;  /* B */
        out[3*SGL_SIMT_LANES + L] = 1.f;  /* A */
    }
    return 1;
}
/* scalar per-pixel hook is never reached when frag_simt is non-NULL on RVV, but
 * the signature requires it. */
static int mock_frag(void *ctx, const float *vary, float rgba[4])
{
    (void)ctx; (void)vary; rgba[0]=1.f; rgba[1]=0.f; rgba[2]=0.f; rgba[3]=1.f; return 1;
}

static void draw_tri(sgl_framebuffer *fb, int cmask, int zwrite)
{
    sgl_drawstate st; memset(&st, 0, sizeof st);
    st.tex = 0; st.depth_test = 1; st.depth_func = SGL_DEPTH_LESS;
    st.depth_write = zwrite; st.color_mask = cmask; st.blend = SGL_BLEND_NONE;
    st.scissor_on = 0;
    /* a lower-left half-screen triangle at z=0.5, w=1 (invw=1), no varyings */
    sgl_pvert a = { 2.5f,  2.5f, 0.5f, 1.f, 0 };
    sgl_pvert b = { 13.5f, 2.5f, 0.5f, 1.f, 0 };
    sgl_pvert c = { 2.5f, 13.5f, 0.5f, 1.f, 0 };
    sgl_raster_tri_prog(fb, &a, &b, &c, 0, &st, mock_frag, mock_frag_simt, 0);
}

int kmain(void)
{
    printf("\n==== colour-mask early-out validation (rva23s64) ====\n");
    sgl_framebuffer *fb = sgl_fb_create(16, 16);
    const uint32_t CLEAR = 0xFF202020u;
    const int PX = 4*16 + 4;           /* interior pixel (4,4) */
    int pass = 1;

    /* case 1: full colour write — baseline */
    sgl_clear(fb, CLEAR, 1.0f); g_vm_calls = 0;
    draw_tri(fb, 0xF, 1);
    uint32_t c1 = sgl_pixel(fb, 4, 4); float z1 = fb->depth[PX]; int n1 = g_vm_calls;
    int ok1 = (c1 != CLEAR) && (z1 > 0.4f && z1 < 0.6f) && (n1 > 0);
    printf("[cmask] full(0xF) zwr=1: color=%08x depthx1000=%d vmcalls=%d -> %s\n",
           c1, (int)(z1*1000.f), n1, ok1 ? "OK" : "FAIL"); pass &= ok1;

    /* case 2: colour masked off, depth still written, VM bypassed */
    sgl_clear(fb, CLEAR, 1.0f); g_vm_calls = 0;
    draw_tri(fb, 0, 1);
    uint32_t c2 = sgl_pixel(fb, 4, 4); float z2 = fb->depth[PX]; int n2 = g_vm_calls;
    int ok2 = (c2 == CLEAR) && (z2 > 0.4f && z2 < 0.6f) && (n2 == 0);
    printf("[cmask] off(0)   zwr=1: color=%08x depthx1000=%d vmcalls=%d -> %s\n",
           c2, (int)(z2*1000.f), n2, ok2 ? "OK" : "FAIL"); pass &= ok2;

    /* case 3: colour masked off, no depth write — pure no-op pass, VM bypassed */
    sgl_clear(fb, CLEAR, 1.0f); g_vm_calls = 0;
    draw_tri(fb, 0, 0);
    uint32_t c3 = sgl_pixel(fb, 4, 4); float z3 = fb->depth[PX]; int n3 = g_vm_calls;
    int ok3 = (c3 == CLEAR) && (z3 > 0.99f) && (n3 == 0);
    printf("[cmask] off(0)   zwr=0: color=%08x depthx1000=%d vmcalls=%d -> %s\n",
           c3, (int)(z3*1000.f), n3, ok3 ? "OK" : "FAIL"); pass &= ok3;

    printf("[cmask] %s\n", pass ? "ALL PASS" : "*** FAILURE ***");
    sgl_fb_destroy(fb);
    sbi_shutdown();
    return 0;
}

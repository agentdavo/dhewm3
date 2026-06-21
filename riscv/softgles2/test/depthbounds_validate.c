/*
 * depthbounds_validate.c — bare-metal RVV gate for GL_EXT_depth_bounds_test.
 *
 * The depth-bounds test discards a fragment when the EXISTING (destination) depth
 * at its pixel is outside [zmin,zmax] — it tests the depth buffer, not the
 * fragment. Doom 3 sets it per light to the light's depth extent so geometry
 * outside the light volume in Z is neither shaded nor stencil-filled.
 *
 * This drives sgl_raster_tri_prog directly (mock batched shader counts calls) on
 * QEMU (-cpu rva23s64): lay down a depth prepass at z=0.5, then redraw with the
 * bounds set inside vs outside 0.5 and confirm the fragment VM is skipped and no
 * colour is written when the destination depth is out of bounds.
 */
#include "libc/libc.h"
#include "boot/runtime.h"
#include "sgl.h"
#include "sg_core.h"
#include "sg_raster.h"

static int g_vm_calls = 0;
static int mock_frag_simt(void *ctx, const float *varys, int vl, float *out)
{
    (void)ctx; (void)varys;
    ++g_vm_calls;
    for (int L = 0; L < vl; ++L) {
        out[0*SGL_SIMT_LANES + L] = 1.f; out[1*SGL_SIMT_LANES + L] = 0.f;
        out[2*SGL_SIMT_LANES + L] = 0.f; out[3*SGL_SIMT_LANES + L] = 1.f;
    }
    return 1;
}
static int mock_frag(void *ctx, const float *vary, float rgba[4])
{ (void)ctx; (void)vary; rgba[0]=1.f; rgba[1]=0.f; rgba[2]=0.f; rgba[3]=1.f; return 1; }

/* draw the half-screen tri at depth z, with optional depth-bounds [min,max]. */
static void draw_tri(sgl_framebuffer *fb, float z, int zwrite, int dfunc,
                     int db_on, float db_min, float db_max, int cmask)
{
    sgl_drawstate st; memset(&st, 0, sizeof st);
    st.depth_test = 1; st.depth_func = dfunc; st.depth_write = zwrite;
    st.color_mask = cmask; st.blend = SGL_BLEND_NONE;
    st.depth_bounds_on = db_on; st.depth_bounds_min = db_min; st.depth_bounds_max = db_max;
    sgl_pvert a = { 2.5f,  2.5f, z, 1.f, 0 };
    sgl_pvert b = { 13.5f, 2.5f, z, 1.f, 0 };
    sgl_pvert c = { 2.5f, 13.5f, z, 1.f, 0 };
    sgl_raster_tri_prog(fb, &a, &b, &c, 0, &st, mock_frag, mock_frag_simt, 0);
}

int kmain(void)
{
    printf("\n==== depth-bounds-test validation (rva23s64) ====\n");
    sgl_framebuffer *fb = sgl_fb_create(16, 16);
    const uint32_t CLEAR = 0xFF202020u;
    int pass = 1;

    /* prepass: lay down z=0.5 across the tri (depth buffer = 0.5 there). */
    /* case A: bounds [0.6,0.9] EXCLUDE the dst depth 0.5 -> fully culled. */
    sgl_clear(fb, CLEAR, 1.0f);
    draw_tri(fb, 0.5f, 1, SGL_DEPTH_LESS, 0, 0, 0, 0);   /* prepass, no bounds */
    g_vm_calls = 0;
    draw_tri(fb, 0.3f, 0, SGL_DEPTH_ALWAYS, 1, 0.6f, 0.9f, 0xF);  /* dst 0.5 outside */
    uint32_t cA = sgl_pixel(fb, 4, 4); int nA = g_vm_calls;
    int okA = (cA == CLEAR) && (nA == 0);
    printf("[db] bounds[0.6,0.9] dst=0.5: color=%08x vmcalls=%d -> %s\n",
           cA, nA, okA ? "OK (culled)" : "FAIL"); pass &= okA;

    /* case B: bounds [0.4,0.6] INCLUDE the dst depth 0.5 -> drawn. */
    sgl_clear(fb, CLEAR, 1.0f);
    draw_tri(fb, 0.5f, 1, SGL_DEPTH_LESS, 0, 0, 0, 0);
    g_vm_calls = 0;
    draw_tri(fb, 0.3f, 0, SGL_DEPTH_ALWAYS, 1, 0.4f, 0.6f, 0xF);  /* dst 0.5 inside */
    uint32_t cB = sgl_pixel(fb, 4, 4); int nB = g_vm_calls;
    int okB = (cB != CLEAR) && (nB > 0);
    printf("[db] bounds[0.4,0.6] dst=0.5: color=%08x vmcalls=%d -> %s\n",
           cB, nB, okB ? "OK (drawn)" : "FAIL"); pass &= okB;

    /* case C: depth-bounds OFF -> drawn regardless (no behavioural change). */
    sgl_clear(fb, CLEAR, 1.0f);
    draw_tri(fb, 0.5f, 1, SGL_DEPTH_LESS, 0, 0, 0, 0);
    g_vm_calls = 0;
    draw_tri(fb, 0.3f, 0, SGL_DEPTH_ALWAYS, 0, 0.6f, 0.9f, 0xF);  /* bounds set but OFF */
    uint32_t cC = sgl_pixel(fb, 4, 4); int nC = g_vm_calls;
    int okC = (cC != CLEAR) && (nC > 0);
    printf("[db] OFF (bounds ignored)   : color=%08x vmcalls=%d -> %s\n",
           cC, nC, okC ? "OK (drawn)" : "FAIL"); pass &= okC;

    printf("[db] %s\n", pass ? "ALL PASS" : "*** FAILURE ***");
    sgl_fb_destroy(fb);
    sbi_shutdown();
    return pass ? 0 : 1;
}

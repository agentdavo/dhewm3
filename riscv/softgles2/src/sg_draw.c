#if defined(SGL_BAREMETAL)
#  include "libc/libc.h"
#else
#  include <stdlib.h>
#  include <string.h>
#  include <stdint.h>
#  include <stdio.h>
#endif

#include "sg_state.h"
#include "sg_diag.h"
#include "sg_raster.h"
#include "sg_buffer.h"
#include "sg_texture.h"
#include "sg_shader.h"
#include "sg_draw.h"
#include "sg_vertex.h"
#include "sg_clip.h"
#include "sg_mt.h"
#include "sg_frag.h"
#include "sg_frame.h"
#include "sg_framebin.h"
#include "sg_prof.h"
#include "sgl_gl.h"   /* GL_SRC_ALPHA .. GL_FUNC_ADD for the no-op-layer test */

volatile unsigned long sgl_dbg_draw_glsl = 0;
volatile unsigned long sgl_dbg_draw_builtin = 0;
volatile unsigned long sgl_dbg_draw_fbo = 0;
volatile unsigned long sgl_dbg_draw_default = 0;
volatile unsigned long sgl_dbg_draw_scalar = 0;
volatile unsigned long sgl_dbg_draw_simt = 0;
volatile unsigned long sgl_dbg_draw_skipped_transparent = 0;
/* Per-draw diagnostic A/B; zero restores conservative batch-input setup. */
int sgl_batch_inputs_prune = 1;

/* A draw is a bit-exact no-op when it only ever produces the destination colour.
 * That happens for a fully-transparent layer (source alpha == 0 everywhere) under
 * standard alpha blending: out = src*0 + dst*(1-0) = dst, and out.a = 0 + dst.a*1.
 *
 * We can prove source alpha is 0 only when (a) the program's output alpha is 0
 * wherever the vertex-colour alpha is 0 (fs_alpha_zero_if_color_zero, set by the
 * classifier for the gl4es 2D modulate/overlay shader) and (b) every vertex's
 * colour alpha (vary[3]) is exactly 0. The blend equation must turn that into the
 * identity above, and the draw must have no depth/stencil side effects (skipping
 * it must not drop a write a later draw depends on). Generic GLES2 reasoning —
 * any non-matching draw simply isn't skipped. */
static int draw_is_transparent_noop(const sg_draw_packet *draw, const glsl_program *gp,
                                    const sgl_pvert *pv, GLsizei count, int nvary)
{
    if (!glsl_fs_alpha_zero_if_color_zero(gp)) return 0;
    if (nvary < 4) return 0;
    if (draw->state.blend == SGL_BLEND_NONE) return 0;
    if (draw->state.depth_write || draw->state.stencil_test) return 0;
    const sgl_blend_state *b = &draw->state.blendf;
    /* RGB: src*srcAlpha + dst*(1-srcAlpha) -> dst when srcAlpha==0. */
    if (b->eq_rgb != GL_FUNC_ADD || b->src_rgb != GL_SRC_ALPHA ||
        b->dst_rgb != GL_ONE_MINUS_SRC_ALPHA) return 0;
    /* Alpha: src term is srcAlpha*coeff == 0; dst term must be dst.a (factor 1 at
     * srcAlpha==0): ONE, or ONE_MINUS_SRC_ALPHA (= 1-0). src_a factor is irrelevant
     * (its term is multiplied by srcAlpha==0). */
    if (b->eq_a != GL_FUNC_ADD ||
        (b->dst_a != GL_ONE && b->dst_a != GL_ONE_MINUS_SRC_ALPHA)) return 0;
    for (GLsizei i = 0; i < count; ++i)
        if (!pv[i].vary || pv[i].vary[3] != 0.f) return 0;
    return 1;
}

typedef struct {
    sgl_framebuffer *fb;
    GLint vp_x, vp_y;
    GLsizei vp_w, vp_h;
    int mvp_set;
    const float *mvp;
    int front_ccw;
    int cull_face;
    GLenum cull_mode;
    const attrib_t *attribs;
} sg_draw_context;

static sg_draw_context draw_context_from_state(void)
{
    sg_draw_context dc;
    dc.fb = C.fb;
    dc.vp_x = C.vp_x;
    dc.vp_y = C.vp_y;
    dc.vp_w = C.vp_w;
    dc.vp_h = C.vp_h;
    dc.mvp_set = C.mvp_set;
    dc.mvp = C.mvp;
    dc.front_ccw = C.front_ccw;
    dc.cull_face = C.cull_face;
    dc.cull_mode = C.cull_mode;
    dc.attribs = C.attribs;
    return dc;
}

#if defined(SGL_BAREMETAL)
extern char __heap_end[];
extern uint64_t ram_end(void);   /* true guest-RAM top from the DTB (riscv/boot/runtime.c) */
/* Upper bound for a legitimate in-RAM pointer. dhewm3's malloc arena is extended
 * past __heap_end up to ram_end() (see dos_heap_limit in dos_syscalls.c), so the
 * Doom 3 vertex cache hands out client-array pointers ABOVE __heap_end. The fetch
 * backstop must use the real RAM top or it rejects them and returns the default
 * -- which zeroed heatHaze's normal-map UV (constant (0,0) -> refraction streaks).
 * ram_end() is already clamped below the DTB blob, so wild pointers past real RAM
 * are still rejected. */
static inline uintptr_t sgl_ram_hi(void)
{
    uintptr_t he = (uintptr_t)__heap_end;
    uint64_t  re = ram_end();
    return (re > (uint64_t)he) ? (uintptr_t)re : he;
}
#endif

static void fetch(const attrib_t *a, int vi, float *out, int outn, const float *def)
{
    if (!a->enabled) {
        const float *d = a->has_cur ? a->cur : def;
        for (int i = 0; i < outn; ++i) out[i] = d[i];
        return;
    }
    int comp = (a->type == GL_UNSIGNED_BYTE) ? 1 : 4;
    int stride = a->stride ? a->stride : a->size * comp;
    size_t need = (size_t)a->size * comp;               /* bytes this vertex reads */
    const uint8_t *p;
    if (a->buffer && a->buffer < MAX_BUFFERS && g_buffers[a->buffer].data) {
        /* VBO-backed: bounds-check against the buffer size so a stray/oversized vertex
         * index (vi) never walks off the end into a wild address (frame-77 load fault). */
        size_t off = (size_t)(uintptr_t)a->ptr + (size_t)vi * (size_t)stride;
        if (off + need > g_buffers[a->buffer].size) {
            for (int i = 0; i < outn; ++i) out[i] = def[i];
            return;
        }
        p = g_buffers[a->buffer].data + off;
    } else {
        /* Client-side array: no size is known; at least reject a null base. */
        if (!a->ptr) { for (int i = 0; i < outn; ++i) out[i] = def[i]; return; }
        p = (const uint8_t *)a->ptr + (size_t)vi * (size_t)stride;
    }
#if defined(SGL_BAREMETAL)
    /* Hard backstop: a stray vertex index or stale client pointer must never make the
     * rasteriser read outside guest RAM [0x80000000, ram_end()). Caught a load fault at
     * the reception-scene geometry (client-array attribute with a garbage vi). The upper
     * bound is the real RAM top (sgl_ram_hi), NOT __heap_end -- the vertex cache lives in
     * the malloc arena's extension above __heap_end. */
    {
        uintptr_t pp = (uintptr_t)p;
        if (pp < (uintptr_t)0x80000000UL || pp + need > sgl_ram_hi()) {
            for (int i = 0; i < outn; ++i) out[i] = def[i];
            return;
        }
    }
#endif
    for (int i = 0; i < outn; ++i) {
        if (i < a->size) {
            if (a->type == GL_FLOAT) { float f; memcpy(&f, p + i * 4, 4); out[i] = f; }
            else if (a->type == GL_UNSIGNED_BYTE) { uint8_t b = p[i]; out[i] = a->normalized ? b / 255.f : (float)b; }
            else out[i] = def[i];
        } else out[i] = def[i];
    }
}

static void xform(const float m[16], const float in[4], float out[4])
{
    out[0] = m[0] * in[0] + m[4] * in[1] + m[8]  * in[2] + m[12] * in[3];
    out[1] = m[1] * in[0] + m[5] * in[1] + m[9]  * in[2] + m[13] * in[3];
    out[2] = m[2] * in[0] + m[6] * in[1] + m[10] * in[2] + m[14] * in[3];
    out[3] = m[3] * in[0] + m[7] * in[1] + m[11] * in[2] + m[15] * in[3];
}

static sgl_scrvert process_vertex(const sg_draw_context *dc, int vi)
{
    static const float pdef[4] = {0, 0, 0, 1};
    static const float cdef[4] = {1, 1, 1, 1};
    static const float tdef[4] = {0, 0, 0, 0};
    float pos[4], col[4], uv[4], clip[4];
    fetch(&dc->attribs[SGL_ATTRIB_POSITION], vi, pos, 4, pdef);
    fetch(&dc->attribs[SGL_ATTRIB_COLOR],    vi, col, 4, cdef);
    fetch(&dc->attribs[SGL_ATTRIB_TEXCOORD], vi, uv,  2, tdef);

    if (dc->mvp_set) xform(dc->mvp, pos, clip);
    else { clip[0] = pos[0]; clip[1] = pos[1]; clip[2] = pos[2]; clip[3] = pos[3]; }

    float invw = (clip[3] != 0.f) ? 1.f / clip[3] : 1.f;
    float ndcx = clip[0] * invw, ndcy = clip[1] * invw, ndcz = clip[2] * invw;

    sgl_scrvert s;
    s.x = (float)dc->vp_x + (ndcx * 0.5f + 0.5f) * (float)dc->vp_w;
    s.y = (float)dc->vp_y + (1.f - (ndcy * 0.5f + 0.5f)) * (float)dc->vp_h;
    s.z = C.depth_near + (ndcz * 0.5f + 0.5f) * (C.depth_far - C.depth_near);   /* glDepthRangef */
    s.invw = invw;
    s.r = col[0] * invw; s.g = col[1] * invw; s.b = col[2] * invw; s.a = col[3] * invw;
    s.u = uv[0] * invw;  s.v = uv[1] * invw;
    return s;
}

static void fill_drawstate(sgl_drawstate *st)
{
    sg_state_inputs si;
    sg_state_inputs_from_context(&si);
    sg_state_pack_drawstate(st, &si);
}

static const sgl_tex *current_tex(void)
{
    GLuint t = C.unit_tex[C.sampler_unit < MAX_TEXUNITS ? C.sampler_unit : 0];
    if (t && t < MAX_TEXTURES && g_textures[t].used && tex_has_storage(&g_textures[t]))
        return &g_textures[t].tex;
    return 0;
}

static const char *sgl_dbg_src_name(int indexed)
{
    return indexed ? "elements" : "arrays";
}

static void sgl_dbg_print_index_sample(const int *idx, GLsizei count)
{
    int n = count < 12 ? count : 12;
    printf("[sgldbg]   idx_first%d=", n);
    for (int i = 0; i < n; ++i) {
        printf("%s%d", i ? "," : "", idx[i]);
    }
    printf("\n");
}

static void sgl_dbg_print_attribs(const sg_draw_context *dc)
{
    for (int i = 0; i < SGL_MAX_VERTEX_ATTRIBS; ++i) {
        const attrib_t *a = &dc->attribs[i];
        if (!a->enabled && !a->ptr && !a->has_cur) continue;
        printf("[sgldbg]   attr%d en=%d buf=%u size=%d type=0x%x norm=%d stride=%d ptr=%p cur=(%.4g,%.4g,%.4g,%.4g) has_cur=%d\n",
               i, a->enabled, a->buffer, a->size, a->type, a->normalized,
               a->stride, a->ptr, a->cur[0], a->cur[1], a->cur[2], a->cur[3], a->has_cur);
    }
}

static void sgl_dbg_print_attr_bounds(const sg_draw_context *dc, const int *idx, GLsizei count, int loc)
{
    static const float def[4] = {0, 0, 0, 1};
    if (loc < 0 || loc >= SGL_MAX_VERTEX_ATTRIBS) return;
    const attrib_t *a = &dc->attribs[loc];
    if (!a->enabled && !a->ptr && !a->has_cur) return;

    int n = count < 32 ? count : 32;
    float mn[4] = {0, 0, 0, 0};
    float mx[4] = {0, 0, 0, 0};
    int bad = 0;
    for (int i = 0; i < n; ++i) {
        float v[4];
        fetch(a, idx[i], v, 4, def);
        for (int c = 0; c < 4; ++c) {
            if (!(v[c] == v[c])) bad++;
            if (i == 0 || v[c] < mn[c]) mn[c] = v[c];
            if (i == 0 || v[c] > mx[c]) mx[c] = v[c];
        }
    }
    printf("[sgldbg]   attr%d first%d min=(%.5g,%.5g,%.5g,%.5g) max=(%.5g,%.5g,%.5g,%.5g) bad=%d\n",
           loc, n, mn[0], mn[1], mn[2], mn[3], mx[0], mx[1], mx[2], mx[3], bad);
}

static void sgl_dbg_print_draw_header(unsigned long draw_no, GLenum mode, GLsizei count,
                                      GLenum index_type, GLint first, int indexed)
{
    printf("[sgldbg] frame=%lu draw=%lu src=%s mode=0x%x count=%d index_type=0x%x first=%d program=%u fb=%u target=%s vp=%d,%d %dx%d\n",
           sgl_dbg_frame_no, draw_no, sgl_dbg_src_name(indexed), mode, count,
           index_type, first, C.cur_program, C.framebuffer,
           C.framebuffer ? "fbo" : "default", C.vp_x, C.vp_y, C.vp_w, C.vp_h);
    printf("[sgldbg]   state depth=%d func=0x%x write=%d stencil=%d blend=%d cull=%d mode=0x%x scissor=%d color_mask=0x%x dbounds=%d[%.5g,%.5g]\n",
           C.depth_test, C.depth_func, C.depth_write, C.stencil_test,
           C.blend_on, C.cull_face, C.cull_mode, C.scissor_on, C.color_mask,
           C.depth_bounds_on, C.depth_bounds_min, C.depth_bounds_max);
    printf("[sgldbg]   tex active=%u sampler=%d tex2d={%u,%u,%u,%u,%u,%u,%u,%u} cube={%u,%u,%u,%u,%u,%u,%u,%u}\n",
           C.active_unit, C.sampler_unit,
           C.unit_tex[0], C.unit_tex[1], C.unit_tex[2], C.unit_tex[3],
           C.unit_tex[4], C.unit_tex[5], C.unit_tex[6], C.unit_tex[7],
           C.unit_cube[0], C.unit_cube[1], C.unit_cube[2], C.unit_cube[3],
           C.unit_cube[4], C.unit_cube[5], C.unit_cube[6], C.unit_cube[7]);
}

static void sgl_dbg_print_projected(unsigned long draw_no, GLenum mode, const sgl_pvert *pv, GLsizei count)
{
    if (!pv || count <= 0) return;
    float minx = pv[0].x, maxx = pv[0].x;
    float miny = pv[0].y, maxy = pv[0].y;
    float minz = pv[0].z, maxz = pv[0].z;
    float miniw = pv[0].invw, maxiw = pv[0].invw;
    int bad = 0, behind = 0;
    for (GLsizei i = 0; i < count; ++i) {
        const sgl_pvert *p = &pv[i];
        if (!(p->x == p->x) || !(p->y == p->y) || !(p->z == p->z) || !(p->invw == p->invw)) bad++;
        if (p->invw <= 0.f) behind++;
        if (p->x < minx) minx = p->x; else if (p->x > maxx) maxx = p->x;
        if (p->y < miny) miny = p->y; else if (p->y > maxy) maxy = p->y;
        if (p->z < minz) minz = p->z; else if (p->z > maxz) maxz = p->z;
        if (p->invw < miniw) miniw = p->invw; else if (p->invw > maxiw) maxiw = p->invw;
    }
    printf("[sgldbg]   projected draw=%lu mode=0x%x out_count=%d bbox=(%.2f,%.2f)-(%.2f,%.2f) z=[%.5g,%.5g] invw=[%.5g,%.5g] behind=%d bad=%d\n",
           draw_no, mode, count, minx, miny, maxx, maxy, minz, maxz, miniw, maxiw, behind, bad);
    if (count >= 3) {
        printf("[sgldbg]   pv0=(%.2f,%.2f,%.5g,iw=%.5g) pv1=(%.2f,%.2f,%.5g,iw=%.5g) pv2=(%.2f,%.2f,%.5g,iw=%.5g)\n",
               pv[0].x, pv[0].y, pv[0].z, pv[0].invw,
               pv[1].x, pv[1].y, pv[1].z, pv[1].invw,
               pv[2].x, pv[2].y, pv[2].z, pv[2].invw);
    }
}

static void emit_triangle(const sg_draw_context *dc, sgl_scrvert a, sgl_scrvert b, sgl_scrvert c, const sgl_drawstate *st)
{
    float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    int front = dc->front_ccw ? (area < 0.f) : (area > 0.f);
    if (dc->cull_face) {
        int cull = (dc->cull_mode == GL_FRONT_AND_BACK) ? 1 : (dc->cull_mode == GL_BACK) ? !front : front;
        if (cull) return;
    }
    sgl_drawstate face_st = *st;
    face_st.stencil_frontface = front;
    sgl_raster_tri(dc->fb, a, b, c, &face_st);
}

static void sg_vertex_attr_fetch_cb(void *ctx, int loc, int vi, float *out, int outn, const float *def)
{
    const sg_draw_context *dc = (const sg_draw_context *)ctx;
    fetch(&dc->attribs[loc], vi, out, outn, def);
}

static void emit_tri_prog(const sgl_pvert *a, const sgl_pvert *b, const sgl_pvert *c,
                         const sg_draw_context *dc, int nvary, const sgl_drawstate *st,
                         sgl_frag_simt_fn frag_simt, void *ctx)
{
    float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    int front = dc->front_ccw ? (area < 0.f) : (area > 0.f);
    if (dc->cull_face) {
        int cull = (dc->cull_mode == GL_FRONT_AND_BACK) ? 1 : (dc->cull_mode == GL_BACK) ? !front : front;
        if (cull) return;
    }
    sgl_drawstate face_st = *st;
    face_st.stencil_frontface = front;
    sgl_raster_tri_prog(dc->fb, a, b, c, nvary, &face_st, sg_frag_cb_scalar, frag_simt, ctx);
}

/* After a draw that wrote depth, mark hierarchical-Z dirty over the draw bbox.
 * Refresh is deferred and coalesced until the next whole-triangle cull (see
 * sgl_hiz_mark_dirty_rect) so a stream of depth-prepass / lit draws pays one
 * rescan instead of one per draw. Skipped for EQUAL-only light passes that do
 * not grow the occluder front (common Doom interaction after z-fill). */
static void hiz_sync_after_draw(const sg_draw_packet *draw, const sgl_framebuffer *fb,
                                const sgl_pvert *pv, GLsizei count)
{
    if (!(sgl_hiz_enabled && draw->state.depth_test && draw->state.depth_write &&
          fb && fb->depth))
        return;
    if (draw->state.depth_func == SGL_DEPTH_EQUAL)
        return;
    float minx = pv[0].x, maxx = pv[0].x, miny = pv[0].y, maxy = pv[0].y;
    for (GLsizei i = 1; i < count; ++i) {
        float x = pv[i].x, y = pv[i].y;
        if (x < minx) minx = x; else if (x > maxx) maxx = x;
        if (y < miny) miny = y; else if (y > maxy) maxy = y;
    }
    sgl_hiz_mark_dirty_rect(fb, (int)minx, (int)miny, (int)(maxx + 1.f), (int)(maxy + 1.f));
}

/* ---- C3: near-plane clipping ------------------------------------------------
 *
 * A vertex behind the near plane (clip z + w < 0) projects to a mirrored or
 * infinite screen position. The vertex stage has already discarded clip-space w
 * (pv keeps only invw), so for the rare triangles that straddle the near plane
 * we re-run the scalar VS to recover exact pre-divide clip-space, clip in
 * homogeneous space (sgl_clip_near_tri), then re-project. The whole draw is
 * rebuilt as GL_TRIANGLES so every dispatch route (immediate / MT / FrameBin)
 * inherits clipping from this single point. The common case (nothing behind the
 * near plane) costs one O(count) scan and is otherwise untouched. */

/* Is this projected vertex behind the near plane? invw <= 0 means clip w <= 0
 * (behind the camera); otherwise undo the depth-range remap and test ndc z < -1
 * (in front of the camera but beyond the near plane). */
static int pv_behind_near(const sgl_pvert *v, float dn, float df)
{
    if (v->invw <= 0.f) return 1;
    float zs = df - dn;
    if (zs == 0.f) return 0;
    float z01 = (v->z - dn) / zs;
    return z01 < -1e-6f;
}

/* Re-run the scalar vertex shader for one vertex to recover clip-space gl_Position. */
static void clip_space_for_vertex(const sg_draw_context *dc, glsl_program *gp,
                                  int vi, float clip_out[4])
{
    static const float def[4] = {0, 0, 0, 1};
    float attribs[GLSL_MAX_ATTRIBS][4];
    float varydump[SGL_MAX_VARY];
    for (int loc = 0; loc < GLSL_MAX_ATTRIBS; ++loc) {
        if (loc < SGL_MAX_VERTEX_ATTRIBS) fetch(&dc->attribs[loc], vi, attribs[loc], 4, def);
        else for (int c = 0; c < 4; ++c) attribs[loc][c] = def[c];
    }
    glsl_run_vertex(gp, attribs, clip_out, varydump);
}

/* Project a clip-space vertex to a screen-space sgl_pvert. Matches
 * sg_vertex_postprocess_batch exactly (viewport + glDepthRangef + invw). */
static sgl_pvert project_clip(const float clip[4], const sg_draw_context *dc, const float *vary)
{
    float invw = (clip[3] != 0.f) ? 1.f / clip[3] : 1.f;
    float ndcx = clip[0] * invw, ndcy = clip[1] * invw, ndcz = clip[2] * invw;
    sgl_pvert p;
    p.x = (float)dc->vp_x + (ndcx * 0.5f + 0.5f) * (float)dc->vp_w;
    p.y = (float)dc->vp_y + (1.f - (ndcy * 0.5f + 0.5f)) * (float)dc->vp_h;
    p.z = C.depth_near + (ndcz * 0.5f + 0.5f) * (C.depth_far - C.depth_near);
    p.invw = invw;
    p.vary = vary;
    return p;
}

typedef struct {
    const sg_draw_context *dc;
    glsl_program *gp;
    const sgl_pvert *pv_in;
    const int *idx;
    int nvary, vstride;
    float dn, df;
    sgl_pvert *pv_out;     /* rebuilt GL_TRIANGLES stream */
    float *vb_out;         /* varying store for newly created (clipped) vertices */
    int out_verts;         /* cursor into pv_out */
    int vb_cursor;         /* cursor into vb_out (floats) */
    int cap_verts;         /* pv_out / vb_out capacity (in vertices) */
} sg_clip_ctx;

static void sg_clip_push(sg_clip_ctx *cc, const sgl_pvert *v)
{
    if (cc->out_verts < cc->cap_verts) cc->pv_out[cc->out_verts++] = *v;
}

/* Clip one source triangle (given by draw-order positions) into the GL_TRIANGLES
 * output stream. Pass-through when no vertex is behind the near plane. */
static void sg_clip_src_tri(sg_clip_ctx *cc, int ia, int ib, int ic)
{
    const sgl_pvert *a = &cc->pv_in[ia], *b = &cc->pv_in[ib], *c = &cc->pv_in[ic];
    /* A projected coord this far off-screen (in pixels) means a grazing vertex
     * with a tiny positive w slipped past the near-only test; rasterising it
     * blows edge-function precision (garbage band). Such a triangle must take the
     * guard-band clip path even though no vertex is behind the near plane. */
#define SG_CLIP_GUARD_PX 8192.0f
    int graze = (a->x < -SG_CLIP_GUARD_PX || a->x > SG_CLIP_GUARD_PX ||
                 a->y < -SG_CLIP_GUARD_PX || a->y > SG_CLIP_GUARD_PX ||
                 b->x < -SG_CLIP_GUARD_PX || b->x > SG_CLIP_GUARD_PX ||
                 b->y < -SG_CLIP_GUARD_PX || b->y > SG_CLIP_GUARD_PX ||
                 c->x < -SG_CLIP_GUARD_PX || c->x > SG_CLIP_GUARD_PX ||
                 c->y < -SG_CLIP_GUARD_PX || c->y > SG_CLIP_GUARD_PX);
    if (!graze &&
        !pv_behind_near(a, cc->dn, cc->df) &&
        !pv_behind_near(b, cc->dn, cc->df) &&
        !pv_behind_near(c, cc->dn, cc->df)) {
        sg_clip_push(cc, a); sg_clip_push(cc, b); sg_clip_push(cc, c);
        return;
    }

    float cp[3][4];
    clip_space_for_vertex(cc->dc, cc->gp, cc->idx[ia], cp[0]);
    clip_space_for_vertex(cc->dc, cc->gp, cc->idx[ib], cp[1]);
    clip_space_for_vertex(cc->dc, cc->gp, cc->idx[ic], cp[2]);
    const float *iv[3] = { a->vary, b->vary, c->vary };
    float out_pos[SGL_CLIP_MAXV][4];
    float out_vary[SGL_CLIP_MAXV * SGL_MAX_VARY];
    /* guard=4: clip only what projects beyond 4x the clip volume, so on-screen
     * geometry is untouched and grazing verts are bounded to ~4x the viewport. */
    int n = sgl_clip_frustum_tri(cp, iv, cc->nvary, out_pos, out_vary, 4.0f);
    if (n < 3) return;   /* fully outside the near/guard volume */

    sgl_pvert proj[SGL_CLIP_MAXV];
    for (int j = 0; j < n; ++j) {
        const float *vstore;
        if (cc->vb_cursor + cc->vstride <= cc->cap_verts * cc->vstride) {
            float *dst = cc->vb_out + cc->vb_cursor;
            for (int k = 0; k < cc->nvary; ++k) dst[k] = out_vary[j * cc->nvary + k];
            for (int k = cc->nvary; k < cc->vstride; ++k) dst[k] = 0.f;
            cc->vb_cursor += cc->vstride;
            vstore = dst;
        } else {
            vstore = a->vary;   /* varying-store overflow: degrade, don't corrupt */
        }
        proj[j] = project_clip(out_pos[j], cc->dc, vstore);
    }
    for (int j = 1; j + 1 < n; ++j) {   /* triangle fan -> GL_TRIANGLES */
        sg_clip_push(cc, &proj[0]); sg_clip_push(cc, &proj[j]); sg_clip_push(cc, &proj[j + 1]);
    }
}

static void draw_prog(GLenum mode, GLsizei count, const int *idx, glsl_program *gp, unsigned long draw_no)
{
    sg_draw_context dc = draw_context_from_state();
    int dbg_selected = sgl_diag_draw_selected(draw_no);
    /* heatHaze path-B diagnostic: only for a selected heatHaze draw (bm_sglDbgDraws 1).
     * Captures attrib-8 (MultiTexCoord0) enable/pointer state and the actual UV the
     * shader will fetch, so we can tell if the re-enable guard delivered VARYING per-
     * vertex UVs (fix worked) or still a constant/garbage (fix didn't). Main-hart. */
    const char *hz_kname = glsl_kernel_name(gp);
    int hz_probe = dbg_selected && hz_kname &&
                   (!strcmp(hz_kname, "heathaze") || !strcmp(hz_kname, "heathaze_mask"));
    int hz_loc = -1;
    int nvary = glsl_num_varying_floats(gp);
    if (nvary > SGL_MAX_VARY) nvary = SGL_MAX_VARY;
    int vstride = nvary > 0 ? nvary : 1;

    sg_frag_ctx fc;
    sg_frag_ctx_init(&fc, gp, 0);
    unsigned cube_units = glsl_cube_sampler_unit_mask(gp);   /* samplerCube units -> CUBE binding */
    for (int u = 0; u < GLSL_MAX_SAMPLERS; ++u) {
        GLuint t = 0;
        if (u < MAX_TEXUNITS) t = (cube_units & (1u << u)) ? C.unit_cube[u] : C.unit_tex[u];
        fc.samplers[u] = (t && t < MAX_TEXTURES && g_textures[t].used && tex_has_storage(&g_textures[t])) ? &g_textures[t].tex : 0;
    }

    if (C.cur_program && C.cur_program < MAX_PROGRAMS) {
        prog_obj *po = &g_programs[C.cur_program];
        for (int b = 0; b < po->nabind; ++b) {
            if (!strcmp(po->abind[b].name, "_gl4es_Color")) {
                int loc = po->abind[b].index;
                if (loc >= 0 && loc < SGL_MAX_VERTEX_ATTRIBS &&
                    !C.attribs[loc].enabled && !C.attribs[loc].has_cur) {
                    C.attribs[loc].cur[0] = C.attribs[loc].cur[1] =
                    C.attribs[loc].cur[2] = C.attribs[loc].cur[3] = 1.f;
                    C.attribs[loc].has_cur = 1;
                }
                break;
            }
        }
        /* heatHaze streak diagnosis (path-B). The MultiTexCoord0 slot (location 8,
         * carrying the normal-map deform UV) is diagnosed, NOT patched: the probe below
         * proved location 8 is already enabled for every heatHaze draw (no enable-bit
         * leak), yet the CLIENT-ARRAY heatHaze draws (buf=0) fetch a constant (0,0) UV
         * while the VBO-backed ones (buf=254/255) fetch correct varying UVs. So the
         * real fault is a bad/stale client-side MultiTexCoord0 pointer (fetch()'s
         * bare-metal bounds backstop returns the {0,0,..} default), collapsing the
         * deform to a constant -> streaks. Fixing that is the remaining path-B work;
         * the shipping default is path-A (bm_heathazeDeform 0 -> undistorted glass). */
        if (hz_probe) {
            for (int b = 0; b < po->nabind; ++b)
                if (!strcmp(po->abind[b].name, "_gl4es_MultiTexCoord0")) {
                    int loc = po->abind[b].index;
                    if (loc >= 0 && loc < SGL_MAX_VERTEX_ATTRIBS) hz_loc = loc;
                    break;
                }
        }
    }

    if (hz_probe && hz_loc >= 0) {
        const attrib_t *a = &C.attribs[hz_loc];
        static const float hz_tdef[4] = {0, 0, 0, 0};
        float uv0[2] = {0, 0}, uv1[2] = {0, 0};
        int i0 = idx ? idx[0] : 0;
        int i1 = (count > 1) ? (idx ? idx[1] : 1) : i0;
        fetch(a, i0, uv0, 2, hz_tdef);
        fetch(a, i1, uv1, 2, hz_tdef);
        /* Replicate fetch()'s address + branch decision to record WHY buf=0 heatHaze
         * draws return (0,0). path: v=vbo-read V=vbo-oob->def c=client-read
         * N=client-null->def B=baremetal-backstop->def D=disabled->def. addr* is the
         * absolute byte address fetch would read for i0/i1 (client) or data+off (vbo).
         * Verdict (per the path-B table): ptr==0x1/NULL => gl4es never bound a client
         * array; strd wrong => bind stride/type bug; path=B with a plausible ptr =>
         * fetch()'s bare-metal range is too tight for client arrays; path=c/v yet
         * v0==v1 => an indexing bug, not bounds. */
        int comp = (a->type == GL_UNSIGNED_BYTE) ? 1 : 4;
        int strd = a->stride ? a->stride : a->size * comp;
        size_t need = (size_t)a->size * comp;
        uintptr_t base = (uintptr_t)a->ptr, addr0 = 0, addr1 = 0;
        char path = '?';
        if (!a->enabled) path = 'D';
        else if (a->buffer && a->buffer < MAX_BUFFERS && g_buffers[a->buffer].data) {
            uintptr_t off0 = base + (size_t)i0 * strd, off1 = base + (size_t)i1 * strd;
            addr0 = (uintptr_t)g_buffers[a->buffer].data + off0;
            addr1 = (uintptr_t)g_buffers[a->buffer].data + off1;
            path = (off0 + need > g_buffers[a->buffer].size) ? 'V' : 'v';
        } else if (!a->ptr) {
            path = 'N';
        } else {
            addr0 = base + (size_t)i0 * strd; addr1 = base + (size_t)i1 * strd;
            path = 'c';
#if defined(SGL_BAREMETAL)
            if (addr0 < (uintptr_t)0x80000000UL || addr0 + need > sgl_ram_hi()) path = 'B';
#endif
        }
        /* Main-hart print (float + %p ok here). */
        printf("[hz] draw=%lu %s loc=%d en=%d buf=%u | ptr=%p strd=%d type=0x%x size=%d norm=%d cur=%d"
               " | count=%d i0=%d i1=%d addr0=%p addr1=%p path=%c | v0=(%.5g,%.5g) v1=(%.5g,%.5g)\n",
               draw_no, hz_kname, hz_loc, a->enabled, (unsigned)a->buffer,
               a->ptr, strd, (unsigned)a->type, a->size, a->normalized, a->has_cur,
               (int)count, i0, i1, (void *)addr0, (void *)addr1, path,
               uv0[0], uv0[1], uv1[0], uv1[1]);
    }

    /* Per-frame arena: reclaimed wholesale at present, not leaked per draw. In
     * immediate mode these are consumed within the draw; FrameBin keeps them
     * alive until the frame flush. */
    sgl_pvert *pv = (sgl_pvert *)sg_frame_alloc(sizeof(sgl_pvert) * (size_t)count);
    float *vb = (float *)sg_frame_alloc(sizeof(float) * (size_t)count * vstride);
    if (!pv || !vb) { set_err(GL_INVALID_OPERATION); return; }
    if (dbg_selected && sgl_dbg_draw_detail >= 2) {
        sgl_dbg_print_attr_bounds(&dc, idx, count, SGL_ATTRIB_POSITION);
        sgl_dbg_print_attr_bounds(&dc, idx, count, SGL_ATTRIB_COLOR);
        sgl_dbg_print_attr_bounds(&dc, idx, count, SGL_ATTRIB_TEXCOORD);
        for (int loc = 3; loc < SGL_MAX_VERTEX_ATTRIBS; ++loc) {
            if (dc.attribs[loc].enabled || dc.attribs[loc].ptr || dc.attribs[loc].has_cur)
                sgl_dbg_print_attr_bounds(&dc, idx, count, loc);
        }
    }
    uint64_t pf_t0 = sgl_prof_now();
    sg_vertex_prog_cached(count, idx, gp, vstride, pv, vb, dc.mvp_set, dc.mvp,
                         dc.vp_x, dc.vp_y, dc.vp_w, dc.vp_h,
                         C.depth_near, C.depth_far,
                         sg_vertex_attr_fetch_cb, &dc);
    if (sgl_prof_enabled) { sgl_prof_vertex += sgl_prof_now() - pf_t0; sgl_prof_draws++; sgl_prof_tris += (uint64_t)(count / 3); }
    /* C3: near-plane clip. One scan; pass through untouched if nothing is behind
     * the near plane, else rebuild the draw as a clipped GL_TRIANGLES stream so
     * all dispatch routes below inherit clipping. */
    if (count >= 3) {
        int any_behind = 0;
        for (GLsizei i = 0; i < count; ++i) {
            const sgl_pvert *v = &pv[i];
            /* behind near, OR a grazing vertex (tiny +w) that projected far off
             * screen -- both need the guard-band clip (see sg_clip_src_tri). */
            if (pv_behind_near(v, C.depth_near, C.depth_far) ||
                v->x < -8192.0f || v->x > 8192.0f || v->y < -8192.0f || v->y > 8192.0f) {
                any_behind = 1; break;
            }
        }
        if (any_behind) {
            int cap = (int)count * 3;   /* guard-band clip can fan a triangle to a few tris */
            sgl_pvert *pv2 = (sgl_pvert *)sg_frame_alloc(sizeof(sgl_pvert) * (size_t)cap);
            float *vb2 = (float *)sg_frame_alloc(sizeof(float) * (size_t)cap * (size_t)vstride);
            if (pv2 && vb2) {
                sg_clip_ctx cc = { &dc, gp, pv, idx, nvary, vstride,
                                   C.depth_near, C.depth_far, pv2, vb2, 0, 0, cap };
                if (mode == GL_TRIANGLES) {
                    for (GLsizei i = 0; i + 2 < count; i += 3)
                        sg_clip_src_tri(&cc, (int)i, (int)i + 1, (int)i + 2);
                } else if (mode == GL_TRIANGLE_STRIP) {
                    for (GLsizei i = 0; i + 2 < count; ++i) {
                        if (i & 1) sg_clip_src_tri(&cc, (int)i + 1, (int)i, (int)i + 2);
                        else       sg_clip_src_tri(&cc, (int)i, (int)i + 1, (int)i + 2);
                    }
                } else if (mode == GL_TRIANGLE_FAN) {
                    for (GLsizei i = 1; i + 1 < count; ++i)
                        sg_clip_src_tri(&cc, 0, (int)i, (int)i + 1);
                }
                pv = pv2;
                count = (GLsizei)cc.out_verts;
                mode = GL_TRIANGLES;
                if (count < 3) return;   /* clipped away entirely */
            }
            /* else: allocation failed -> fall through unclipped (graceful) */
        }
    }

    if (dbg_selected) {
        sgl_dbg_print_projected(draw_no, mode, pv, count);
        printf("[sgldbg]   layout nvary=%d color_mask=0x%x discard=%d\n",
               nvary, C.color_mask, glsl_fs_has_discard(gp));
    }

#if !defined(SGL_BAREMETAL)
    if (getenv("SGL_TRACE") && count >= 3) {
        fprintf(stderr, "[sgl draw] mode=%u count=%d nvary=%d depth=%d blend=%d  v0=(%.2f,%.2f z=%.2f iw=%.3f) v1=(%.2f,%.2f) v2=(%.2f,%.2f)\n",
                mode, count, nvary, C.depth_test, C.blend_mode,
                pv[0].x, pv[0].y, pv[0].z, pv[0].invw, pv[1].x, pv[1].y, pv[2].x, pv[2].y);
        for (int i = 0; i < SGL_MAX_VERTEX_ATTRIBS; ++i)
            if (C.attribs[i].enabled || C.attribs[i].ptr)
                fprintf(stderr, "   [attr %d] en=%d buf=%u size=%d type=0x%x stride=%d ptr=%p\n",
                        i, C.attribs[i].enabled, C.attribs[i].buffer, C.attribs[i].size,
                        C.attribs[i].type, C.attribs[i].stride, C.attribs[i].ptr);
    }
#endif

    int fs_simt_ok = glsl_fs_simt_ok(gp);
    int fs_cost = glsl_fs_cost(gp);
#if !defined(SGL_BAREMETAL)
    (void)fs_cost;
#endif
    if (sgl_diag_enabled) {
        sgl_dbg_draw_glsl++;
        if (fs_simt_ok) sgl_dbg_draw_simt++;
        else sgl_dbg_draw_scalar++;
    }

    sg_draw_packet draw = {0};
    draw.fb = dc.fb;
    draw.state.tex = 0;
    fill_drawstate(&draw.state);
    draw.front_ccw = dc.front_ccw;
    draw.cull_face = dc.cull_face;
    draw.cull_mode = dc.cull_mode;
    draw.nvary = nvary;
    draw.fs_cost = fs_cost;
    draw.fs_simt_ok = fs_simt_ok;
    draw.state.has_discard = glsl_fs_has_discard(gp);   /* SIMT alpha-test: defer depth-write, kill coverage */
    /* Only meaningful beside a bound fs_kernel: the VM addresses varyings by
     * offset, so it must always see the full set. glsl_fs_classify() leaves the
     * mask 0 for VM programs, which the rasteriser reads as "interpolate all". */
    draw.state.vary_live_mask = glsl_fs_vary_live_mask(gp);
    if (sgl_batch_inputs_prune) {
        draw.state.derivative_mask = glsl_fs_derivative_mask(gp);
        draw.state.skip_derivatives = draw.state.derivative_mask == 0;
        draw.state.skip_fragcoord = !glsl_fs_uses_fragcoord(gp);
    }
    {   /* diagnostic: classify this draw's blend so [fswork] shows opaque vs additive */
        int cls = (draw.state.blend == SGL_BLEND_NONE) ? 0
                : (draw.state.blendf.dst_rgb == GL_ONE &&
                   (draw.state.blendf.src_rgb == GL_ONE || draw.state.blendf.src_rgb == GL_SRC_ALPHA)) ? 1 : 2;
        glsl_dbg_record_draw_class(gp, cls);
    }
    draw.fs_scalar = sg_frag_cb_scalar;
    draw.fs_simt = fs_simt_ok ? sg_frag_cb_simt : 0;
    draw.frag_ctx = &fc;

    /* Fully-transparent layer: writes only the destination colour -> skip entirely
     * (bit-exact). Covers every dispatch route (immediate / MT / FrameBin) from one
     * point. The GUI/menu cinematic stacks many such layers during fades. */
    if (draw_is_transparent_noop(&draw, gp, pv, count, nvary)) {
        sgl_dbg_draw_skipped_transparent++;   /* per-frame; reset in sgl_prof_frame_report */
        return;   /* pv/vb are frame-arena owned (reclaimed at present) */
    }

    if (sgl_framebin_enabled) {
        /* Defer: record this draw; the whole frame is binned + shaded across
         * harts at present. pv/vb are frame-arena owned (alive until flush). */
        /* HiZ and deferral are mutually exclusive. The pyramid is maintained by
         * hiz_sync_after_draw() below, which the deferred path never reaches, so
         * it would go stale against a depth buffer written later at flush time.
         * Stale-far entries are merely conservative, but glClear seeds the
         * pyramid immediately (sg_state.c) while the clear itself is only
         * replayed at flush -- that ordering can leave entries NEARER than the
         * real buffer, which culls geometry that should be drawn. Rather than
         * carry that risk, drop HiZ while deferring: it only ever fires on
         * LESS/LEQUAL draws, i.e. the depth-fill pass (everything else is
         * GL_EQUAL), and r_useDepthOnlyFill already makes that pass nearly free.
         * Revisit by replaying clears into the pyramid at flush. */
        if (sgl_hiz_enabled) sgl_hiz_enabled = 0;
        sg_framebin_record_draw(&draw, mode, count, pv, gp);
        return;
    }

    uint64_t pf_td = sgl_prof_now();
    int pf_disp = sg_mt_prog_dispatch(&draw, mode, count, pv,
                                      dc.fb ? dc.fb->w : 0, dc.fb ? dc.fb->h : 0, 1);
    if (sgl_prof_enabled) sgl_prof_dispatch += sgl_prof_now() - pf_td;
    if (pf_disp) {
        hiz_sync_after_draw(&draw, dc.fb, pv, count);
        return;   /* pv/vb are frame-arena owned (reclaimed at present) */
    }
    uint64_t pf_tr = sgl_prof_now();

    if (mode == GL_TRIANGLES) {
        for (GLsizei i = 0; i + 2 < count; i += 3) emit_tri_prog(&pv[i], &pv[i+1], &pv[i+2], &dc, draw.nvary,
                                                                     &draw.state, draw.fs_simt, draw.frag_ctx);
    } else if (mode == GL_TRIANGLE_STRIP) {
        for (GLsizei i = 0; i + 2 < count; ++i) {
            if (i & 1) emit_tri_prog(&pv[i+1], &pv[i], &pv[i+2], &dc, draw.nvary,
                                     &draw.state, draw.fs_simt, draw.frag_ctx);
            else       emit_tri_prog(&pv[i], &pv[i+1], &pv[i+2], &dc, draw.nvary,
                                     &draw.state, draw.fs_simt, draw.frag_ctx);
        }
    } else if (mode == GL_TRIANGLE_FAN) {
        for (GLsizei i = 1; i + 1 < count; ++i) emit_tri_prog(&pv[0], &pv[i], &pv[i+1], &dc, draw.nvary,
                                                                 &draw.state, draw.fs_simt, draw.frag_ctx);
    }
    if (sgl_prof_enabled) sgl_prof_raster += sgl_prof_now() - pf_tr;
    hiz_sync_after_draw(&draw, dc.fb, pv, count);
    /* pv/vb are frame-arena owned (reclaimed at present), not freed per draw. */
}

static void draw_indexed(GLenum mode, GLsizei count, const int *idx,
                         unsigned long draw_no, GLenum index_type, GLint first, int indexed)
{
    if (!C.fb || count < 3) return;
    if (!C.fb->color || !C.fb->depth) { set_err(GL_INVALID_FRAMEBUFFER_OPERATION); return; }
    if (sgl_diag_draw_selected(draw_no)) {
        sg_draw_context dc = draw_context_from_state();
        sgl_dbg_print_draw_header(draw_no, mode, count, index_type, first, indexed);
        sgl_dbg_print_index_sample(idx, count);
        if (sgl_dbg_draw_detail >= 1) sgl_dbg_print_attribs(&dc);
    }
    if (sgl_diag_enabled) {
        if (C.framebuffer) sgl_dbg_draw_fbo++; else sgl_dbg_draw_default++;
    }
    glsl_program *gp = cur_gp();
    if (gp) { draw_prog(mode, count, idx, gp, draw_no); return; }
    if (sgl_diag_enabled) sgl_dbg_draw_builtin++;
    sgl_drawstate st;
    sg_draw_context dc = draw_context_from_state();
    st.tex = current_tex();
    fill_drawstate(&st);

    if (mode == GL_TRIANGLES) {
        for (GLsizei i = 0; i + 2 < count; i += 3)
            emit_triangle(&dc, process_vertex(&dc, idx[i]), process_vertex(&dc, idx[i + 1]), process_vertex(&dc, idx[i + 2]), &st);
    } else if (mode == GL_TRIANGLE_STRIP) {
        for (GLsizei i = 0; i + 2 < count; ++i) {
            if (i & 1) emit_triangle(&dc, process_vertex(&dc, idx[i + 1]), process_vertex(&dc, idx[i]), process_vertex(&dc, idx[i + 2]), &st);
            else       emit_triangle(&dc, process_vertex(&dc, idx[i]), process_vertex(&dc, idx[i + 1]), process_vertex(&dc, idx[i + 2]), &st);
        }
    } else if (mode == GL_TRIANGLE_FAN) {
        for (GLsizei i = 1; i + 1 < count; ++i)
            emit_triangle(&dc, process_vertex(&dc, idx[0]), process_vertex(&dc, idx[i]), process_vertex(&dc, idx[i + 1]), &st);
    }
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    GLTRACE("[drawarr] mode=0x%x first=%d count=%d\n", mode, first, count);
    int *idx = (int *)sg_frame_alloc(sizeof(int) * (size_t)count);
    if (!idx) { set_err(GL_INVALID_OPERATION); return; }
    for (GLsizei i = 0; i < count; ++i) idx[i] = first + i;
    draw_indexed(mode, count, idx, sgl_diag_note_draw(), 0, first, 0);
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    GLTRACE("[drawelt] mode=0x%x count=%d\n", mode, count);
    if (count <= 0) return;
    size_t elem = (type == GL_UNSIGNED_BYTE) ? 1 : (type == GL_UNSIGNED_SHORT) ? 2
                : (type == GL_UNSIGNED_INT) ? 4 : 0;
    if (!elem) { set_err(GL_INVALID_ENUM); return; }
    const uint8_t *base = indices;
    if (C.element_buffer && C.element_buffer < MAX_BUFFERS && g_buffers[C.element_buffer].data) {
        /* `indices` is a byte OFFSET into the bound ELEMENT_ARRAY_BUFFER. Bounds-check it:
         * a stale binding paired with a raw absolute client pointer (which gl4es passes on
         * the 32-bit-index passthrough path) would otherwise form a wild base and read
         * ~4GB past RAM. If the range does not fit the buffer, the binding/offset is bogus. */
        size_t off = (size_t)(uintptr_t)indices;
        if (off + (size_t)count * elem > g_buffers[C.element_buffer].size) {
            set_err(GL_INVALID_OPERATION); return;
        }
        base = g_buffers[C.element_buffer].data + off;
    }

    int *idx = (int *)sg_frame_alloc(sizeof(int) * (size_t)count);
    if (!idx) { set_err(GL_INVALID_OPERATION); return; }
    for (GLsizei i = 0; i < count; ++i) {
        if (elem == 1)      idx[i] = base[i];
        else if (elem == 2) { uint16_t v; memcpy(&v, base + (size_t)i * 2, 2); idx[i] = v; }
        else                { uint32_t v; memcpy(&v, base + (size_t)i * 4, 4); idx[i] = (int)v; }
    }
    draw_indexed(mode, count, idx, sgl_diag_note_draw(), type, 0, 1);
}

void glFlush(void) { sg_framebin_flush(); }   /* drain deferred frame (no-op if immediate) */
void glFinish(void) { glFlush(); }

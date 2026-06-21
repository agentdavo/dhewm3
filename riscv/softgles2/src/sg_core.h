/* Core internal data contracts for softgles2 execution units.
 *
 * These are internal details shared by raster/state/vertex/fragment pipelines and
 * are intentionally kept in one small header so worker handoff contracts stay
 * explicit.
 */
#ifndef SOFTGLES2_SG_CORE_H
#define SOFTGLES2_SG_CORE_H

#include "sgl.h"
#include "sg_align.h"

/* Texture object as seen by the rasteriser. */
typedef struct sgl_tex {
    int       w, h;
    uint32_t *texels;       /* RGBA8, w*h, 0xAABBGGRR */
    int       min_filter;   /* 0 = nearest, 1 = linear (mag uses same for v1) */
    int       wrap;         /* 0 = clamp-to-edge, 1 = repeat                  */
    int       is_cube;      /* 1 = textureCube samples the cube_faces array   */
    const struct sgl_tex *cube_faces[6];
} sgl_tex;

/* Screen-space vertex: post-projection position + per-vertex varyings.
 * Colour and texcoords are stored pre-multiplied by invw for perspective
 * correct interpolation (divide by interpolated invw in the fragment loop). */
typedef struct {
    float x, y, z, invw;
    float r, g, b, a;       /* colour * invw  */
    float u, v;             /* texcoord * invw */
} sgl_scrvert;

/* Blend classification. NONE = opaque overwrite (fast path). ALPHA/ADD are kept
 * as enum values for callers/tests but the rasteriser routes every enabled blend
 * through the GENERIC path (full glBlendFunc/glBlendEquation matrix). */
enum { SGL_BLEND_NONE = 0, SGL_BLEND_ALPHA = 1, SGL_BLEND_ADD = 2, SGL_BLEND_GENERIC = 3 };

/* Per-draw blend descriptor: the real GLES2 separate RGB/alpha factors +
 * equations + glBlendColor constant. Carried in the draw snapshot so MT/FrameBin
 * workers blend each recorded draw with its own state. GL enums fit in uint16. */
typedef struct {
    uint16_t src_rgb, dst_rgb;   /* glBlendFuncSeparate factors */
    uint16_t src_a,   dst_a;
    uint16_t eq_rgb,  eq_a;      /* glBlendEquationSeparate equations */
    uint16_t _pad;
    float    color[4];            /* glBlendColor */
} sgl_blend_state;

enum {
    SGL_STENCIL_KEEP      = 0x1E00,
    SGL_STENCIL_REPLACE   = 0x1E01,
    SGL_STENCIL_INCR      = 0x1E02,
    SGL_STENCIL_DECR      = 0x1E03,
    SGL_STENCIL_INVERT    = 0x150A,
    SGL_STENCIL_INCR_WRAP = 0x8507,
    SGL_STENCIL_DECR_WRAP = 0x8508
};

typedef struct {
    int func;
    uint8_t ref;
    uint8_t read_mask;
    uint8_t write_mask;
    int sfail;
    int zfail;
    int zpass;
} sgl_stencil_face;

/* Immutable hot-path draw snapshot.
 *
 * Global GL state lives in gl_ctx, but raster/MT workers consume this compact
 * snapshot. Keep it trivially copyable, hot fields grouped, and 64-byte aligned
 * so arrays of submitted draw records have predictable cache/RVV boundaries.
 */
typedef struct {
    const sgl_tex *tex;     /* NULL = untextured (colour only) */
    int depth_test;         /* 0/1 */
    int blend;              /* SGL_BLEND_* */
    int depth_func;         /* SGL_DEPTH_* comparison (default LESS) */
    int depth_write;        /* 0/1 write depth on pass (glDepthMask) */
    int color_mask;         /* bit0=R bit1=G bit2=B bit3=A (0xF = all) */
    int scissor_on;         /* 0/1 enable scissor rect test */
    int sx0, sy0, sx1, sy1; /* scissor rect in framebuffer pixels (inclusive) */
    int stencil_test;       /* 0/1 */
    sgl_stencil_face stencil_front;
    sgl_stencil_face stencil_back;
    int stencil_frontface;  /* 1 = front-facing primitive, 0 = back-facing */
    /* Parallel raster clip: restrict this triangle to framebuffer rect
     * [clip_x0, clip_x1) x [clip_y0, clip_y1). clip_x1/clip_y1 == 0 is the
     * sentinel for "no clip", so the serial path leaves these zeroed. Used by
     * dOS hart tile workers so disjoint tiles never write the same pixels. */
    int clip_x0, clip_y0, clip_x1, clip_y1;
    int has_discard;        /* FS uses discard: shade before depth-write, AND kill into coverage */
    sgl_blend_state blendf; /* real glBlendFunc/Equation/Color (used when blend != NONE) */
    float poly_off_factor;  /* glPolygonOffset factor (depth-slope scale) */
    float poly_off_units;   /* glPolygonOffset units  (resolvable-difference scale) */
    int   poly_off_on;      /* GL_POLYGON_OFFSET_FILL enabled */
    int   depth_bounds_on;  /* GL_EXT_depth_bounds_test: cull where dst depth outside [min,max] */
    float depth_bounds_min, depth_bounds_max;
    int _layout_pad[5];     /* pad sizeof up to a 64-byte multiple (SG_RVV_ALIGN) */
} sgl_drawstate SG_ALIGNED_TYPE(SG_RVV_ALIGN);

SG_STATIC_ASSERT(sizeof(sgl_drawstate) % SG_RVV_ALIGN == 0);

/* Tolerance for EQUAL/NOTEQUAL depth compares. The lit-world renderer fills
 * depth in one pass then re-draws the same geometry with GL_EQUAL for each light.
 * Perspective interpolation can differ by a few ULPs between passes (and gl_FragDepth-free
 * fixed-function still re-runs the edge walk), so a strict == drops most lit
 * fragments. A small relative epsilon keeps the lighting passes landing. */
#define SGL_DEPTH_EPS 1e-5f

static inline int sgl_depth_pass(int func, float z, float zb)
{
    switch (func) {
    case SGL_DEPTH_NEVER:    return 0;
    case SGL_DEPTH_LESS:     return z <  zb;
    case SGL_DEPTH_EQUAL: {
        float d = z - zb; if (d < 0.f) d = -d;
        return d <= SGL_DEPTH_EPS;
    }
    case SGL_DEPTH_LEQUAL:   return z <= zb;
    case SGL_DEPTH_GREATER:  return z >  zb;
    case SGL_DEPTH_NOTEQUAL: {
        float d = z - zb; if (d < 0.f) d = -d;
        return d >  SGL_DEPTH_EPS;
    }
    case SGL_DEPTH_GEQUAL:   return z >= zb;
    case SGL_DEPTH_ALWAYS:   return 1;
    default:                 return z <  zb;
    }
}

static inline int sgl_stencil_pass(const sgl_stencil_face *sf, uint8_t value)
{
    unsigned lhs = (unsigned)value & sf->read_mask;
    unsigned rhs = (unsigned)sf->ref & sf->read_mask;
    switch (sf->func) {
    case SGL_DEPTH_NEVER:    return 0;
    case SGL_DEPTH_LESS:     return lhs <  rhs;
    case SGL_DEPTH_EQUAL:    return lhs == rhs;
    case SGL_DEPTH_LEQUAL:   return lhs <= rhs;
    case SGL_DEPTH_GREATER:  return lhs >  rhs;
    case SGL_DEPTH_NOTEQUAL: return lhs != rhs;
    case SGL_DEPTH_GEQUAL:   return lhs >= rhs;
    case SGL_DEPTH_ALWAYS:   return 1;
    default:                 return 1;
    }
}

static inline uint8_t sgl_stencil_apply_op(const sgl_stencil_face *sf, int op, uint8_t cur)
{
    unsigned v = cur;
    switch (op) {
    case SGL_STENCIL_KEEP:      return cur;
    case SGL_STENCIL_REPLACE:   v = sf->ref; break;
    case SGL_STENCIL_INCR:      v = (v == 255u) ? 255u : v + 1u; break;
    case SGL_STENCIL_DECR:      v = (v == 0u) ? 0u : v - 1u; break;
    case SGL_STENCIL_INCR_WRAP: v = (v + 1u) & 255u; break;
    case SGL_STENCIL_DECR_WRAP: v = (v - 1u) & 255u; break;
    case SGL_STENCIL_INVERT:    v = (~v) & 255u; break;
    default:                    return cur;
    }
    return (uint8_t)((cur & ~sf->write_mask) | (v & sf->write_mask));
}

/* Raster entry points moved to sg_raster.h. */

/* SoA batch width for the RVV fragment/vertex VM. MUST equal GLSL_SIMT_MAXLANE.
 * Set for VLEN=1024 with LMUL=m2 (64× f32). At runtime vsetvl returns the actual
 * hardware lane count (e.g. 8 at VLEN=256/m2); the SoA stride remains this value
 * so code is VLEN-agnostic. Lower to 16 for VLEN <= 256 to reduce stack. */
#define SGL_SIMT_LANES 64
/* Max varying floats we thread through programmable fragment programs. */
#define SGL_MAX_VARY 32

typedef struct {
    float        x, y, z, invw;
    const float *vary;          /* nvary floats (raw, not premultiplied) */
} sgl_pvert;

/* Returns 0 to discard the fragment, else writes rgba and returns 1. */
typedef int (*sgl_frag_fn)(void *ctx, const float *vary_interp, float rgba[4]);

/* Batched fragment hook. varys/out are SoA with a fixed SGL_SIMT_LANES-float
 * stride per component: varys[varyfloat*SGL_SIMT_LANES + lane], out RGBA in
 * out[channel*SGL_SIMT_LANES + lane] for channel 0..3. vl = active lanes
 * (<= SGL_SIMT_LANES). The callback shades ALL vl lanes (coverage/depth masking
 * is the rasteriser's job, applied at store time). */
typedef int (*sgl_frag_simt_fn)(void *ctx, const float *varys, int vl, float *out);

#endif /* SOFTGLES2_SG_CORE_H */

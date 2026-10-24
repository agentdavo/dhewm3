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

/* 4096x4096 (Doom 3's largest) needs 13 levels; 16 covers up to 32768. */
#define SGL_MAX_MIP 16

struct sgl_mip_level { int w, h; uint32_t *texels; };

/* Texture object as seen by the rasteriser.
 *
 * w/h/texels ALWAYS describe level 0 and mip[0] aliases them, so every existing
 * user of those three fields keeps working untouched. Levels 1..nmip-1 are the
 * reduced images, used only by the LOD path in sg_math.h. */
typedef struct sgl_tex {
    int       w, h;
    uint32_t *texels;       /* RGBA8, w*h, 0xAABBGGRR */
    int       min_filter;   /* 0 = nearest, 1 = linear (mag uses same for v1) */
    int       wrap;         /* 0 = clamp-to-edge, 1 = repeat                  */
    int       is_cube;      /* 1 = textureCube samples the cube_faces array   */
    int       is_depth;     /* 1 = level-0 texels are raw IEEE-754 depth bits  */
    const struct sgl_tex *cube_faces[6];
    /* Mip pyramid. Before this existed the sampler read full-resolution texels
     * for every fragment however minified the surface was, so each bilinear
     * gather straddled distant texels and missed cache -- and glTexImage2D
     * silently DISCARDED every level > 0 the application uploaded.
     *
     * Held BEHIND A POINTER, not inline. An inline [16] array added 256 bytes to
     * sgl_tex, and the sampler builds a by-value level view per texture fetch --
     * measured at +15.5% guest instructions, i.e. the pyramid cost more than the
     * cache locality it bought. Keep this struct small. */
    struct sgl_mip_level *mips;   /* NULL, or nmip entries; [0] aliases w/h/texels */
    int       nmip;         /* valid levels, >= 1 once level 0 exists */
    int       mip_filter;   /* 0 = level 0 only, 1 = nearest level, 2 = linear between levels */
    /* GL_TEXTURE_MAX_ANISOTROPY_EXT. 1 = isotropic (the common case, byte-identical
     * to the pre-aniso sampler). Doom 3 requests up to 8 (Ultra). glGenTextures
     * memsets sgl_tex to 0, so the sampler treats any value < 1 as 1 -- a
     * zero-initialised texture behaves exactly as before this field existed. */
    float     max_aniso;
    int       opaque_alpha; /* RGB storage: alpha is 1; occupies former tail padding on RV64. */
} sgl_tex;

/* Analytic screen-space UV derivatives for one lane-batch, in NORMALISED
 * texcoord units (the sampler scales by tex->w/h to texels). Passed by pointer:
 * NULL means "no derivative info" and the RVV sampler falls back to its prior
 * per-batch two-lane estimate, so callers that cannot supply derivatives keep
 * their exact previous behaviour. */
typedef struct sgl_uv_deriv {
    float dudx, dvdx, dudy, dvdy;
} sgl_uv_deriv;

/* Analytic screen-space derivative of ONE perspective-correct varying at a pixel.
 *
 * The rasteriser interpolates a varying as  val = (Σ w_i p_i) / (Σ w_i iw_i),
 * where w_i = e_i*invArea are the barycentric weights, iw_i = vertex 1/w, and
 * p_i = vertex_value * iw_i (the premultiplied plane coeff vpa/vpb/vpc). The
 * edge functions e_i are linear in screen (x,y) with de_i/dx = Ax[i],
 * de_i/dy = Ay[i]. Differentiating the quotient analytically (no cross-lane
 * sampling) gives both partials from the plane data alone:
 *
 *   d(val)/dx = ( dnum/dx - val * ddenom/dx ) / denom
 *   dnum/dx  = invArea * Σ Ax_i p_i ,  ddenom/dx = invArea * Σ Ax_i iw_i   (y analog)
 *
 * `e[3]` are the edge values at the pixel whose derivative is wanted (any
 * interior pixel of the batch). This is exact for the perspective-correct
 * interpolation, matching the rasteriser's own u,v at that pixel. */
static inline void sgl_vary_screen_deriv(const float e[3], const float Ax[3],
        const float Ay[3], float invArea, const float iw[3], const float p[3],
        float *ddx, float *ddy)
{
    float w0 = e[0] * invArea, w1 = e[1] * invArea, w2 = e[2] * invArea;
    float denom = w0 * iw[0] + w1 * iw[1] + w2 * iw[2];
    float s = (denom != 0.f) ? 1.f / denom : 0.f;
    float val = (w0 * p[0] + w1 * p[1] + w2 * p[2]) * s;
    float ddenomdx = invArea * (Ax[0] * iw[0] + Ax[1] * iw[1] + Ax[2] * iw[2]);
    float ddenomdy = invArea * (Ay[0] * iw[0] + Ay[1] * iw[1] + Ay[2] * iw[2]);
    float dnumdx   = invArea * (Ax[0] * p[0]  + Ax[1] * p[1]  + Ax[2] * p[2]);
    float dnumdy   = invArea * (Ay[0] * p[0]  + Ay[1] * p[1]  + Ay[2] * p[2]);
    *ddx = (dnumdx - val * ddenomdx) * s;
    *ddy = (dnumdy - val * ddenomdy) * s;
}

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
    /* Live-varying mask: bit k set = the fragment shader reads varying float k,
     * so the perspective interpolation can skip the rest. 0 = unknown,
     * interpolate everything (the safe default and what the VM always gets).
     * Carried BY VALUE, not as a pointer, because this struct is snapshotted
     * into FrameBin draw records and must stay trivially copyable. */
    unsigned int vary_live_mask;
    unsigned int derivative_mask; /* zero = conservative all-varying setup */
    int skip_derivatives;          /* classified shader does not consume them */
    int skip_fragcoord;            /* no gl_FragCoord read in the shader */
    int _layout_pad[1];     /* preserve the existing draw-state size */
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
    /* OpenGL compares (reference & mask) FUNC (stored stencil & mask). */
    unsigned lhs = (unsigned)sf->ref & sf->read_mask;
    unsigned rhs = (unsigned)value & sf->read_mask;
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
#ifndef SGL_SIMT_LANES
#define SGL_SIMT_LANES 64
#endif
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

#if defined(SGL_BAREMETAL)
#  include "libc/libc.h"
#else
#  include <stdlib.h>
#  include <string.h>
#  include <stdint.h>
#  include <stdio.h>
#endif

#include "glsl.h"
#include "sg_state.h"
#include "sg_framebin.h"
#include "sg_raster.h"

#if defined(__riscv_v_intrinsic) && __riscv_v_intrinsic >= 1000000
#  include <riscv_vector.h>
#  define SG_STATE_RVV 1
#else
#  define SG_STATE_RVV 0
#endif

gl_ctx C;

void set_err(GLenum e) { if (C.error == GL_NO_ERROR) C.error = e; }

static int valid_blend_factor(GLenum f)
{
    switch (f) {
    case GL_ZERO: case GL_ONE:
    case GL_SRC_COLOR: case GL_ONE_MINUS_SRC_COLOR:
    case GL_SRC_ALPHA: case GL_ONE_MINUS_SRC_ALPHA:
    case GL_DST_ALPHA: case GL_ONE_MINUS_DST_ALPHA:
    case GL_DST_COLOR: case GL_ONE_MINUS_DST_COLOR:
    case GL_SRC_ALPHA_SATURATE:
    case GL_CONSTANT_COLOR: case GL_ONE_MINUS_CONSTANT_COLOR:
    case GL_CONSTANT_ALPHA: case GL_ONE_MINUS_CONSTANT_ALPHA:
        return 1;
    default:
        return 0;
    }
}

static int valid_blend_equation(GLenum mode)
{
    return mode == GL_FUNC_ADD || mode == GL_FUNC_SUBTRACT || mode == GL_FUNC_REVERSE_SUBTRACT;
}

static void update_blend_mode(void)
{
    /* NONE = disabled, or an identity blend (src*1 + dst*0, ADD) on BOTH RGB and
     * alpha == a plain overwrite. Everything else uses the GENERIC rasteriser
     * path, which honors the real per-channel factors + equations. */
    int identity = (C.blend_src_rgb == GL_ONE && C.blend_dst_rgb == GL_ZERO &&
                    C.blend_src_alpha == GL_ONE && C.blend_dst_alpha == GL_ZERO &&
                    C.blend_eq_rgb == GL_FUNC_ADD && C.blend_eq_alpha == GL_FUNC_ADD);
    C.blend_mode = (!C.blend_on || identity) ? SGL_BLEND_NONE : SGL_BLEND_GENERIC;
}

static int valid_stencil_func(GLenum func)
{
    switch (func) {
    case GL_NEVER: case GL_LESS: case GL_EQUAL: case GL_LEQUAL:
    case GL_GREATER: case GL_NOTEQUAL: case GL_GEQUAL: case GL_ALWAYS:
        return 1;
    default:
        return 0;
    }
}

static int valid_stencil_op(GLenum op)
{
    switch (op) {
    case GL_KEEP: case GL_REPLACE: case GL_INCR: case GL_DECR:
    case GL_INCR_WRAP: case GL_DECR_WRAP: case GL_INVERT:
        return 1;
    default:
        return 0;
    }
}

static void set_stencil_func(sgl_stencil_face *sf, GLenum func, GLint ref, GLuint mask)
{
    sf->func = (int)func;
    sf->ref = (uint8_t)(ref & 0xFF);
    sf->read_mask = (uint8_t)(mask & 0xFF);
}

static void set_stencil_op(sgl_stencil_face *sf, GLenum fail, GLenum zfail, GLenum zpass)
{
    sf->sfail = (int)fail;
    sf->zfail = (int)zfail;
    sf->zpass = (int)zpass;
}

void sglMakeCurrent(sgl_framebuffer *fb)
{
    C.default_fb = fb;
    C.fb = fb;
    C.framebuffer = 0;
    C.renderbuffer = 0;
    C.vp_x = 0; C.vp_y = 0; C.vp_w = fb ? fb->w : 0; C.vp_h = fb ? fb->h : 0;
    C.depth_near = 0.f; C.depth_far = 1.f;
    C.clear_depth = 1.f;
    C.clear_stencil = 0;
    C.front_ccw = 1; C.cull_mode = GL_BACK;
    C.stencil_test = 0;
    C.depth_func = SGL_DEPTH_LESS;
    C.depth_write = 1;
    C.stencil_front.func = C.stencil_back.func = SGL_DEPTH_ALWAYS;
    C.stencil_front.ref = C.stencil_back.ref = 0;
    C.stencil_front.read_mask = C.stencil_back.read_mask = 0xFFu;
    C.stencil_front.write_mask = C.stencil_back.write_mask = 0xFFu;
    C.stencil_front.sfail = C.stencil_back.sfail = SGL_STENCIL_KEEP;
    C.stencil_front.zfail = C.stencil_back.zfail = SGL_STENCIL_KEEP;
    C.stencil_front.zpass = C.stencil_back.zpass = SGL_STENCIL_KEEP;
    C.color_mask = 0xF;
    C.scissor_on = 0;
    C.scissor_x = 0; C.scissor_y = 0; C.scissor_w = fb ? fb->w : 0; C.scissor_h = fb ? fb->h : 0;
    C.depth_bounds_on = 0; C.depth_bounds_min = 0.f; C.depth_bounds_max = 1.f;
    C.poly_off_factor = 0.f; C.poly_off_units = 0.f; C.poly_off_on = 0;
    C.blend_on = 0;
    C.blend_mode = SGL_BLEND_NONE;
    C.blend_src_rgb = C.blend_src_alpha = GL_ONE;
    C.blend_dst_rgb = C.blend_dst_alpha = GL_ZERO;
    C.blend_eq_rgb = C.blend_eq_alpha = GL_FUNC_ADD;
    C.blend_color[0] = C.blend_color[1] = C.blend_color[2] = C.blend_color[3] = 0.f;
    C.sampler_unit = 0;
    C.pack_alignment = 4;
    C.unpack_alignment = 4;
    C.error = GL_NO_ERROR;
}

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { C.vp_x = x; C.vp_y = y; C.vp_w = w; C.vp_h = h; }
void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) { C.clear_r = r; C.clear_g = g; C.clear_b = b; C.clear_a = a; }
void glClearDepthf(GLclampf d) { C.clear_depth = d; }
void glClearStencil(GLint s) { C.clear_stencil = s & 0xFF; }

void glDepthFunc(GLenum func)
{
    switch (func) {
    case GL_NEVER: case GL_LESS: case GL_EQUAL: case GL_LEQUAL:
    case GL_GREATER: case GL_NOTEQUAL: case GL_GEQUAL: case GL_ALWAYS:
        C.depth_func = (int)func; break;
    default: C.depth_func = SGL_DEPTH_LESS; break;
    }
}
void glDepthMask(GLboolean flag) { C.depth_write = flag ? 1 : 0; }
void glDepthRangef(GLclampf n, GLclampf f) { C.depth_near = n; C.depth_far = f; }
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    C.color_mask = (r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0);
}
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
    C.scissor_x = x; C.scissor_y = y; C.scissor_w = w; C.scissor_h = h;
}
void glPolygonOffset(GLfloat factor, GLfloat units) { C.poly_off_factor = factor; C.poly_off_units = units; }
/* GL_EXT_depth_bounds_test: cull a fragment when the EXISTING (destination) depth
 * at its pixel is outside [zmin,zmax]. Doom 3 sets this per light to its depth
 * range so geometry outside the light volume's Z extent isn't shaded/filled. */
void glDepthBoundsEXT(GLclampf zmin, GLclampf zmax)
{
    if (zmin < 0.f) zmin = 0.f; else if (zmin > 1.f) zmin = 1.f;
    if (zmax < 0.f) zmax = 0.f; else if (zmax > 1.f) zmax = 1.f;
    C.depth_bounds_min = zmin; C.depth_bounds_max = zmax;
}
void glCullFace(GLenum mode)  { C.cull_mode = mode; }
void glFrontFace(GLenum mode) { C.front_ccw = (mode == GL_CCW); }

void glEnable(GLenum cap)
{
    if (cap == GL_DEPTH_TEST) C.depth_test = 1;
    else if (cap == GL_CULL_FACE) C.cull_face = 1;
    else if (cap == GL_STENCIL_TEST) C.stencil_test = 1;
    else if (cap == GL_BLEND) { C.blend_on = 1; update_blend_mode(); }
    else if (cap == GL_SCISSOR_TEST) C.scissor_on = 1;
    else if (cap == GL_POLYGON_OFFSET_FILL) C.poly_off_on = 1;
    else if (cap == GL_DEPTH_BOUNDS_TEST_EXT) C.depth_bounds_on = 1;
}
void glDisable(GLenum cap)
{
    if (cap == GL_DEPTH_TEST) C.depth_test = 0;
    else if (cap == GL_CULL_FACE) C.cull_face = 0;
    else if (cap == GL_STENCIL_TEST) C.stencil_test = 0;
    else if (cap == GL_BLEND) { C.blend_on = 0; update_blend_mode(); }
    else if (cap == GL_SCISSOR_TEST) C.scissor_on = 0;
    else if (cap == GL_POLYGON_OFFSET_FILL) C.poly_off_on = 0;
    else if (cap == GL_DEPTH_BOUNDS_TEST_EXT) C.depth_bounds_on = 0;
}

GLboolean glIsEnabled(GLenum cap)
{
    if (cap == GL_DEPTH_TEST) return C.depth_test ? GL_TRUE : GL_FALSE;
    if (cap == GL_CULL_FACE) return C.cull_face ? GL_TRUE : GL_FALSE;
    if (cap == GL_STENCIL_TEST) return C.stencil_test ? GL_TRUE : GL_FALSE;
    if (cap == GL_BLEND) return C.blend_on ? GL_TRUE : GL_FALSE;
    if (cap == GL_SCISSOR_TEST) return C.scissor_on ? GL_TRUE : GL_FALSE;
    if (cap == GL_POLYGON_OFFSET_FILL) return C.poly_off_on ? GL_TRUE : GL_FALSE;
    if (cap == GL_DEPTH_BOUNDS_TEST_EXT) return C.depth_bounds_on ? GL_TRUE : GL_FALSE;
    return GL_FALSE;
}

void glBlendFunc(GLenum sfactor, GLenum dfactor)
{
    glBlendFuncSeparate(sfactor, dfactor, sfactor, dfactor);
}

void glBlendFuncSeparate(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha)
{
    if (!valid_blend_factor(srcRGB) || !valid_blend_factor(dstRGB) ||
        !valid_blend_factor(srcAlpha) || !valid_blend_factor(dstAlpha)) {
        set_err(GL_INVALID_ENUM);
        return;
    }
    C.blend_src_rgb = srcRGB;
    C.blend_dst_rgb = dstRGB;
    C.blend_src_alpha = srcAlpha;
    C.blend_dst_alpha = dstAlpha;
    update_blend_mode();
}

void glBlendEquation(GLenum mode)
{
    glBlendEquationSeparate(mode, mode);
}

void glBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha)
{
    if (!valid_blend_equation(modeRGB) || !valid_blend_equation(modeAlpha)) {
        set_err(GL_INVALID_ENUM);
        return;
    }
    C.blend_eq_rgb = modeRGB;
    C.blend_eq_alpha = modeAlpha;
    update_blend_mode();
}

void glBlendColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha)
{
    C.blend_color[0] = red < 0.f ? 0.f : (red > 1.f ? 1.f : red);
    C.blend_color[1] = green < 0.f ? 0.f : (green > 1.f ? 1.f : green);
    C.blend_color[2] = blue < 0.f ? 0.f : (blue > 1.f ? 1.f : blue);
    C.blend_color[3] = alpha < 0.f ? 0.f : (alpha > 1.f ? 1.f : alpha);
}

void glStencilFunc(GLenum func, GLint ref, GLuint mask)
{
    glStencilFuncSeparate(GL_FRONT_AND_BACK, func, ref, mask);
}

void glStencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask)
{
    if (!valid_stencil_func(func)) { set_err(GL_INVALID_ENUM); return; }
    if (face == GL_FRONT || face == GL_FRONT_AND_BACK) set_stencil_func(&C.stencil_front, func, ref, mask);
    if (face == GL_BACK || face == GL_FRONT_AND_BACK) set_stencil_func(&C.stencil_back, func, ref, mask);
    if (face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) set_err(GL_INVALID_ENUM);
}

void glStencilMask(GLuint mask)
{
    glStencilMaskSeparate(GL_FRONT_AND_BACK, mask);
}

void glStencilMaskSeparate(GLenum face, GLuint mask)
{
    uint8_t m = (uint8_t)(mask & 0xFF);
    if (face == GL_FRONT || face == GL_FRONT_AND_BACK) C.stencil_front.write_mask = m;
    if (face == GL_BACK || face == GL_FRONT_AND_BACK) C.stencil_back.write_mask = m;
    if (face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) set_err(GL_INVALID_ENUM);
}

void glStencilOp(GLenum fail, GLenum zfail, GLenum zpass)
{
    glStencilOpSeparate(GL_FRONT_AND_BACK, fail, zfail, zpass);
}

void glStencilOpSeparate(GLenum face, GLenum fail, GLenum zfail, GLenum zpass)
{
    if (!valid_stencil_op(fail) || !valid_stencil_op(zfail) || !valid_stencil_op(zpass)) {
        set_err(GL_INVALID_ENUM);
        return;
    }
    if (face == GL_FRONT || face == GL_FRONT_AND_BACK) set_stencil_op(&C.stencil_front, fail, zfail, zpass);
    if (face == GL_BACK || face == GL_FRONT_AND_BACK) set_stencil_op(&C.stencil_back, fail, zfail, zpass);
    if (face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) set_err(GL_INVALID_ENUM);
}

GLenum glGetError(void) { GLenum e = C.error; C.error = GL_NO_ERROR; return e; }

const GLubyte *glGetString(GLenum name)
{
    switch (name) {
    case GL_VENDOR:   return (const GLubyte *)"dhewm3-riscv";
    case GL_RENDERER: return (const GLubyte *)"softgles2 (RVV software rasteriser)";
    case GL_VERSION:  return (const GLubyte *)"OpenGL ES 2.0 (softgles2 v0)";
    case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte *)"OpenGL ES GLSL ES 1.00 (built-in)";
    /* Advertise every capability softgles2 actually backs so gl4es enables max
     * features: NPOT textures (no POT check in glTexImage2D), 8-bit + 24-bit + packed
     * depth/stencil renderbuffer formats. Each gates a gl4es hardext.* flag at probe time.
     *
     * NOTE: GL_OES_element_index_uint is intentionally NOT advertised. softgles2 decodes
     * 32-bit indices correctly, but enabling it flips gl4es from deep-copying indices into
     * a fresh USHORT buffer (and resetting the element-array binding) to passing Doom3's
     * raw absolute client index pointer straight through WITHOUT resetting the binding —
     * which, against a stale softgles2 element_buffer, forms a wild rebase (garbage vertex
     * index, ~4GB-past-RAM read). Doom3 md5/world meshes are <65535 verts so 16-bit indices
     * are lossless. Re-enable only once gl4es resets the index binding on the raw path. */
    case GL_EXTENSIONS: return (const GLubyte *)"GL_EXT_texture_compression_s3tc GL_OES_texture_compression_S3TC GL_EXT_depth_bounds_test GL_OES_texture_npot GL_OES_rgb8_rgba8 GL_OES_depth24 GL_OES_packed_depth_stencil";
    default: return (const GLubyte *)"";
    }
}

void glGetIntegerv(GLenum pname, GLint *data)
{
    if (!data) return;
    switch (pname) {
    case GL_MAX_TEXTURE_SIZE:                 *data = 4096; break;
    case GL_NUM_COMPRESSED_TEXTURE_FORMATS:   *data = 4; break;
    case GL_COMPRESSED_TEXTURE_FORMATS:
        data[0] = GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
        data[1] = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
        data[2] = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
        data[3] = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        break;
    case 0x851C: case 0x84E8:                 *data = 4096; break;
    case GL_MAX_VERTEX_ATTRIBS:               *data = SGL_MAX_VERTEX_ATTRIBS; break;
    case 0x8DFC:                               *data = 8;   break;
    case 0x8872: case 0x8B4D: case 0x84E2:    *data = MAX_TEXUNITS; break;
    case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS:          *data = 0; break;
    case 0x8DFB: case 0x8DFD: *data = GLSL_MAX_UNIFORM_VECTORS; break;
    case 0x8B49: *data = GLSL_MAX_UNIFORM_VECTORS * 4; break;
    case 0x0D3A: data[0] = 4096; data[1] = 4096; break;   /* GL_MAX_VIEWPORT_DIMS: 2 ints */
    case GL_ACTIVE_TEXTURE:                   *data = GL_TEXTURE0 + (GLint)C.active_unit; break;
    case GL_TEXTURE_BINDING_2D:               *data = (GLint)C.unit_tex[C.active_unit]; break;
    case GL_TEXTURE_BINDING_CUBE_MAP:         *data = (GLint)C.unit_cube[C.active_unit]; break;
    case GL_BLEND_SRC: case GL_BLEND_SRC_RGB: *data = (GLint)C.blend_src_rgb; break;
    case GL_BLEND_DST: case GL_BLEND_DST_RGB: *data = (GLint)C.blend_dst_rgb; break;
    case GL_BLEND_SRC_ALPHA:                  *data = (GLint)C.blend_src_alpha; break;
    case GL_BLEND_DST_ALPHA:                  *data = (GLint)C.blend_dst_alpha; break;
    case GL_BLEND_EQUATION:                   *data = (GLint)C.blend_eq_rgb; break;
    case GL_BLEND_EQUATION_ALPHA:             *data = (GLint)C.blend_eq_alpha; break;
    case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: case GL_ALPHA_BITS: *data = C.fb ? 8 : 0; break;
    case GL_DEPTH_BITS:                       *data = C.fb && C.fb->depth ? 24 : 0; break;
    case GL_STENCIL_BITS:                     *data = C.fb && C.fb->stencil ? 8 : 0; break;
    case GL_STENCIL_CLEAR_VALUE:              *data = C.clear_stencil; break;
    case GL_STENCIL_FUNC:                     *data = C.stencil_front.func; break;
    case GL_STENCIL_REF:                      *data = C.stencil_front.ref; break;
    case GL_STENCIL_VALUE_MASK:               *data = C.stencil_front.read_mask; break;
    case GL_STENCIL_WRITEMASK:                *data = C.stencil_front.write_mask; break;
    case GL_STENCIL_FAIL:                     *data = C.stencil_front.sfail; break;
    case GL_STENCIL_PASS_DEPTH_FAIL:          *data = C.stencil_front.zfail; break;
    case GL_STENCIL_PASS_DEPTH_PASS:          *data = C.stencil_front.zpass; break;
    case GL_STENCIL_BACK_FUNC:                *data = C.stencil_back.func; break;
    case GL_STENCIL_BACK_REF:                 *data = C.stencil_back.ref; break;
    case GL_STENCIL_BACK_VALUE_MASK:          *data = C.stencil_back.read_mask; break;
    case GL_STENCIL_BACK_WRITEMASK:           *data = C.stencil_back.write_mask; break;
    case GL_STENCIL_BACK_FAIL:                *data = C.stencil_back.sfail; break;
    case GL_STENCIL_BACK_PASS_DEPTH_FAIL:     *data = C.stencil_back.zfail; break;
    case GL_STENCIL_BACK_PASS_DEPTH_PASS:     *data = C.stencil_back.zpass; break;
    case GL_ARRAY_BUFFER_BINDING:             *data = (GLint)C.array_buffer; break;
    case GL_ELEMENT_ARRAY_BUFFER_BINDING:     *data = (GLint)C.element_buffer; break;
    case GL_FRAMEBUFFER_BINDING:              *data = (GLint)C.framebuffer; break;
    case GL_RENDERBUFFER_BINDING:             *data = (GLint)C.renderbuffer; break;
    case GL_PACK_ALIGNMENT:                   *data = C.pack_alignment; break;
    case GL_UNPACK_ALIGNMENT:                 *data = C.unpack_alignment; break;
    case GL_IMPLEMENTATION_COLOR_READ_TYPE:   *data = GL_UNSIGNED_BYTE; break;
    case GL_IMPLEMENTATION_COLOR_READ_FORMAT: *data = GL_RGBA; break;
    default: *data = 0; break;
    }
}

void glGetFloatv(GLenum pname, GLfloat *data)
{
    GLint v = 0;
    if (!data) return;
    if (pname == GL_BLEND_COLOR) {
        data[0] = C.blend_color[0];
        data[1] = C.blend_color[1];
        data[2] = C.blend_color[2];
        data[3] = C.blend_color[3];
        return;
    }
    /* Multi-value float queries: the single-int glGetIntegerv funnel below would fill
     * only data[0]. Answer them fully so gl4es/Doom3 read valid ranges. */
    switch (pname) {
    case 0x846D: case 0x0B12: data[0] = 1.0f; data[1] = 1.0f; return;   /* GL_ALIASED_POINT_SIZE_RANGE / GL_SMOOTH_POINT_SIZE_RANGE */
    case 0x846E: case 0x0B22: data[0] = 1.0f; data[1] = 1.0f; return;   /* GL_ALIASED_LINE_WIDTH_RANGE / GL_SMOOTH_LINE_WIDTH_RANGE */
    case 0x0D3A:              data[0] = 4096.0f; data[1] = 4096.0f; return;   /* GL_MAX_VIEWPORT_DIMS */
    default: break;
    }
    glGetIntegerv(pname, &v);
    *data = (GLfloat)v;
}

void glGetBooleanv(GLenum pname, GLboolean *data)
{
    GLint v = 0;
    if (!data) return;
    glGetIntegerv(pname, &v);
    *data = v ? GL_TRUE : GL_FALSE;
}

void glClear(GLbitfield mask)
{
    if (!C.fb) return;
    uint32_t R = (uint32_t)(C.clear_r * 255.f + 0.5f);
    uint32_t G = (uint32_t)(C.clear_g * 255.f + 0.5f);
    uint32_t B = (uint32_t)(C.clear_b * 255.f + 0.5f);
    uint32_t A = (uint32_t)(C.clear_a * 255.f + 0.5f);   /* C5: honor glClearColor alpha */
    uint32_t c = R | (G << 8) | (B << 16) | (A << 24);
    if (sgl_framebin_enabled) {
        /* Defer: ordered against the deferred draws so it wipes only what
         * precedes it in submission order (see FrameBin). */
        sg_framebin_record_clear(C.fb, mask, c, C.clear_depth, (uint8_t)C.clear_stencil);
        return;
    }
    size_t n = (size_t)C.fb->w * C.fb->h;
    float cd = C.clear_depth;
    uint8_t cs = (uint8_t)C.clear_stencil;
#if SG_STATE_RVV
    /* Per-frame fullscreen clear: vectorise the e32 colour/depth and e8 stencil
     * fills (the 2D overlay clears colour+depth every frame). */
    if ((mask & GL_COLOR_BUFFER_BIT) && C.fb->color)
        for (size_t i = 0; i < n; ) { size_t vl = __riscv_vsetvl_e32m8(n - i);
            __riscv_vse32_v_u32m8(&C.fb->color[i], __riscv_vmv_v_x_u32m8(c, vl), vl); i += vl; }
    if ((mask & GL_DEPTH_BUFFER_BIT) && C.fb->depth)
        for (size_t i = 0; i < n; ) { size_t vl = __riscv_vsetvl_e32m8(n - i);
            __riscv_vse32_v_f32m8(&C.fb->depth[i], __riscv_vfmv_v_f_f32m8(cd, vl), vl); i += vl; }
    if ((mask & GL_STENCIL_BUFFER_BIT) && C.fb->stencil)
        for (size_t i = 0; i < n; ) { size_t vl = __riscv_vsetvl_e8m8(n - i);
            __riscv_vse8_v_u8m8(&C.fb->stencil[i], __riscv_vmv_v_x_u8m8(cs, vl), vl); i += vl; }
#else
    if ((mask & GL_COLOR_BUFFER_BIT) && C.fb->color)
        for (size_t i = 0; i < n; ++i) C.fb->color[i] = c;
    if ((mask & GL_DEPTH_BUFFER_BIT) && C.fb->depth)
        for (size_t i = 0; i < n; ++i) C.fb->depth[i] = cd;
    if ((mask & GL_STENCIL_BUFFER_BIT) && C.fb->stencil)
        for (size_t i = 0; i < n; ++i) C.fb->stencil[i] = cs;
#endif
    /* Seed hierarchical-Z to match the depth clear so the first draw of the
     * frame culls nothing (every tile == far) and later draws cull against it. */
    if ((mask & GL_DEPTH_BUFFER_BIT) && C.fb->depth)
        sgl_hiz_clear(C.fb, cd);
}

void glEnableVertexAttribArray(GLuint i)  { GLTRACE("[ena] %u\n", i); if (i < SGL_MAX_VERTEX_ATTRIBS) C.attribs[i].enabled = 1; }
void glDisableVertexAttribArray(GLuint i) { GLTRACE("[dis] %u\n", i); if (i < SGL_MAX_VERTEX_ATTRIBS) C.attribs[i].enabled = 0; }

static void set_attrib_cur(GLuint i, float x, float y, float z, float w)
{
    if (i >= SGL_MAX_VERTEX_ATTRIBS) return;
    C.attribs[i].cur[0]=x; C.attribs[i].cur[1]=y; C.attribs[i].cur[2]=z; C.attribs[i].cur[3]=w;
    C.attribs[i].has_cur = 1;
}
void glVertexAttrib1f(GLuint i, GLfloat x)                                  { set_attrib_cur(i,x,0.f,0.f,1.f); }
void glVertexAttrib2f(GLuint i, GLfloat x, GLfloat y)                       { set_attrib_cur(i,x,y,0.f,1.f); }
void glVertexAttrib3f(GLuint i, GLfloat x, GLfloat y, GLfloat z)            { set_attrib_cur(i,x,y,z,1.f); }
void glVertexAttrib4f(GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { set_attrib_cur(i,x,y,z,w); }
void glVertexAttrib1fv(GLuint i, const GLfloat *v) { set_attrib_cur(i,v[0],0.f,0.f,1.f); }
void glVertexAttrib2fv(GLuint i, const GLfloat *v) { set_attrib_cur(i,v[0],v[1],0.f,1.f); }
void glVertexAttrib3fv(GLuint i, const GLfloat *v) { set_attrib_cur(i,v[0],v[1],v[2],1.f); }
void glVertexAttrib4fv(GLuint i, const GLfloat *v) { set_attrib_cur(i,v[0],v[1],v[2],v[3]); }

void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *ptr)
{
    GLTRACE("[vap] idx=%u size=%d type=0x%x stride=%d ptr=%p buf=%u\n", index, size, type, stride, ptr, C.array_buffer);
    if (index >= SGL_MAX_VERTEX_ATTRIBS) { set_err(GL_INVALID_VALUE); return; }
    attrib_t *a = &C.attribs[index];
    a->size = size; a->type = type; a->normalized = normalized; a->stride = stride; a->ptr = ptr; a->buffer = C.array_buffer;
}

void glVertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void *ptr)
{
    glVertexAttribPointer(index, size, type, GL_FALSE, stride, ptr);
}

void sg_state_inputs_from_context(sg_state_inputs *out)
{
    if (!out) return;
    out->depth_test = C.depth_test;
    out->blend_mode = C.blend_mode;
    out->depth_func = C.depth_func;
    out->depth_write = C.depth_write;
    out->color_mask = C.color_mask;
    out->stencil_test = C.stencil_test;
    out->stencil_front = C.stencil_front;
    out->stencil_back = C.stencil_back;
    out->scissor_on = C.scissor_on;
    out->scissor_x = C.scissor_x;
    out->scissor_y = C.scissor_y;
    out->scissor_w = C.scissor_w;
    out->scissor_h = C.scissor_h;
    out->depth_bounds_on = C.depth_bounds_on;
    out->depth_bounds_min = C.depth_bounds_min;
    out->depth_bounds_max = C.depth_bounds_max;
    out->fb = C.fb;
    out->blendf.src_rgb = (uint16_t)C.blend_src_rgb;
    out->blendf.dst_rgb = (uint16_t)C.blend_dst_rgb;
    out->blendf.src_a   = (uint16_t)C.blend_src_alpha;
    out->blendf.dst_a   = (uint16_t)C.blend_dst_alpha;
    out->blendf.eq_rgb  = (uint16_t)C.blend_eq_rgb;
    out->blendf.eq_a    = (uint16_t)C.blend_eq_alpha;
    out->blendf._pad    = 0;
    out->blendf.color[0] = C.blend_color[0];
    out->blendf.color[1] = C.blend_color[1];
    out->blendf.color[2] = C.blend_color[2];
    out->blendf.color[3] = C.blend_color[3];
    out->poly_off_factor = C.poly_off_factor;
    out->poly_off_units  = C.poly_off_units;
    out->poly_off_on     = C.poly_off_on;
}

void sg_state_pack_drawstate(sgl_drawstate *out, const sg_state_inputs *in)
{
    if (!out || !in) return;

    out->depth_test = in->depth_test;
    out->blend = in->blend_mode;
    out->blendf = in->blendf;
    out->poly_off_factor = in->poly_off_factor;
    out->poly_off_units  = in->poly_off_units;
    out->poly_off_on     = in->poly_off_on;
    out->depth_func = in->depth_func;
    out->depth_write = in->depth_write;
    out->color_mask = in->color_mask;
    out->stencil_test = in->stencil_test;
    out->stencil_front = in->stencil_front;
    out->stencil_back = in->stencil_back;
    out->stencil_frontface = 1;
    out->depth_bounds_on  = in->depth_bounds_on;
    out->depth_bounds_min = in->depth_bounds_min;
    out->depth_bounds_max = in->depth_bounds_max;
    out->clip_x0 = 0;
    out->clip_y0 = 0;
    out->clip_x1 = 0;
    out->clip_y1 = 0;

    if (in->scissor_on && in->fb) {
        int x0 = in->scissor_x;
        int y0 = in->scissor_y;
        int x1 = in->scissor_x + (int)in->scissor_w - 1;
        int y1 = in->scissor_y + (int)in->scissor_h - 1;
        int fy0 = in->fb->h - 1 - y1;
        int fy1 = in->fb->h - 1 - y0;
        if (x0 < 0) x0 = 0;
        if (fy0 < 0) fy0 = 0;
        if (x1 > in->fb->w - 1) x1 = in->fb->w - 1;
        if (fy1 > in->fb->h - 1) fy1 = in->fb->h - 1;
        out->scissor_on = 1;
        out->sx0 = x0;
        out->sy0 = fy0;
        out->sx1 = x1;
        out->sy1 = fy1;
    } else {
        out->scissor_on = 0;
        out->sx0 = 0;
        out->sy0 = 0;
        out->sx1 = in->fb ? in->fb->w - 1 : 0;
        out->sy1 = in->fb ? in->fb->h - 1 : 0;
    }
}

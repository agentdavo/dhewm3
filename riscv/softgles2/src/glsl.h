/*
 * softgles2 — GLSL ES 1.00 interpreter (subset).
 *
 * Compiles a vertex + fragment shader pair into ASTs and evaluates them:
 *   - vertex stage: reads vertex attributes, writes gl_Position + varyings;
 *   - fragment stage: reads interpolated varyings, samples textures, writes
 *     gl_FragColor (or discards).
 *
 * Supported subset (v1): types float/vec2/vec3/vec4/mat4/sampler2D; qualifiers
 * attribute/uniform/varying/const; main() only; locals; assignment with swizzle
 * lvalues; + - * / unary- , relational + ternary, swizzles, constructors;
 * builtins texture2D/normalize/dot/cross/length/distance/mix/clamp/min/max/
 * abs/sign/floor/ceil/fract/mod/step/smoothstep/reflect/sqrt; mat4*vec4. No
 * user functions / loops / branches yet (ternary + discard cover the common
 * gl4es-generated shape); transcendentals (pow/exp/log/trig) come with libm.
 *
 * Execution is scalar here (correct reference + host path). The vectorised RVV
 * fragment VM (design doc 12) layers on top of the same AST next.
 */
#ifndef SOFTGLES2_GLSL_H
#define SOFTGLES2_GLSL_H

#include "sg_core.h"   /* single source of truth for sgl_tex / draw state / SIMD params */

#define GLSL_MAX_VARYING_FLOATS 32
#define GLSL_MAX_ATTRIBS         16
#define GLSL_MAX_SAMPLERS        8

/* Uniform storage / reported caps. ustore is the COMBINED VS+FS uniform float
 * store; the per-stage GL_MAX_*_UNIFORM_VECTORS we advertise (glGetIntegerv) must
 * be truthful — two stages each using GLSL_MAX_UNIFORM_VECTORS vec4 must fit:
 *   GLSL_USTORE_FLOATS >= 2 * GLSL_MAX_UNIFORM_VECTORS * 4.
 * 128 meets the GLES2 minimums (vertex 128, fragment 16); 1024 floats = 256 vec4
 * combined holds 128+128. glsl_link guards against overflow (fails cleanly). */
#define GLSL_MAX_UNIFORM_VECTORS 128          /* reported per-stage cap (vec4)   */
#define GLSL_USTORE_FLOATS       1024         /* combined VS+FS uniform store     */
#define GLSL_MAX_UNIFORMS        64           /* distinct uniform slots           */

typedef struct glsl_program glsl_program;

glsl_program *glsl_program_create(void);
void          glsl_program_destroy(glsl_program *p);

/* stage: 0 = vertex, 1 = fragment. Returns 1 on success, 0 on parse error
 * (message written to err). */
int  glsl_attach_source(glsl_program *p, int stage, const char *src, char *err, int errlen);

/* Standalone compile check for a single shader (glCompileShader): lexes+parses
 * and gates on #version (only GLSL ES 1.00 / "#version 100" / none). Returns 1
 * if it would compile, 0 otherwise. Lets gl4es's GLSL-dialect probes fail
 * honestly so it emits #version 100 shaders our interpreter accepts. */
int  glsl_try_compile(int stage, const char *src, char *err, int errlen);
int  glsl_link(glsl_program *p, char *err, int errlen);

void glsl_bind_attrib(glsl_program *p, int index, const char *name);  /* before link */
int  glsl_attrib_location(const glsl_program *p, const char *name);  /* -1 if none */
int  glsl_uniform_location(glsl_program *p, const char *name);       /* -1 if none */
int  glsl_num_varying_floats(const glsl_program *p);
/* Pointer to the program's combined VS+FS uniform store (GLSL_USTORE_FLOATS
 * floats). Used to snapshot uniforms for deferred (FrameBin) shading. */
const float *glsl_ustore(const glsl_program *p);
int  glsl_fs_has_ir(const glsl_program *p);   /* A2: 1 if the FS runs via the bytecode IR */

/* Active attribute / uniform introspection (for glGetProgramiv/glGetActive*).
 * gltype is the GLES2 type enum (GL_FLOAT, GL_FLOAT_VEC2..4, GL_FLOAT_MAT4,
 * GL_SAMPLER_2D). Return 1 on success, 0 if index out of range. */
int  glsl_num_attribs(const glsl_program *p);
int  glsl_get_attrib(const glsl_program *p, int index, char *name, int namelen,
                     int *size, int *gltype, int *location);
int  glsl_num_uniforms(const glsl_program *p);
unsigned glsl_cube_sampler_unit_mask(const glsl_program *p);  /* units bound to samplerCube */
int  glsl_get_uniform(const glsl_program *p, int index, char *name, int namelen,
                      int *size, int *gltype);

void glsl_uniform(glsl_program *p, int loc, const float *data, int nfloats);
void glsl_uniform_sampler(glsl_program *p, int loc, int unit);

/* Vertex stage. attribs[location] holds the (up to vec4) attribute value.
 * Writes clip[4] (gl_Position) and the flat varying float array. */
void glsl_run_vertex(glsl_program *p,
                     const float attribs[GLSL_MAX_ATTRIBS][4],
                     float clip[4], float *varying_out);

/* Fragment stage. varying[] is the interpolated flat varying array; samplers[]
 * are the textures bound to each unit. Writes rgba[4]; returns 0 if discarded. */
int  glsl_run_fragment(glsl_program *p, const float *varying,
                       const sgl_tex *const samplers[GLSL_MAX_SAMPLERS],
                       float rgba[4]);

/* As glsl_run_fragment, but reads uniforms from `ustore` (NULL -> p->ustore).
 * The deferred FrameBin raster passes a per-draw uniform snapshot so worker
 * harts shade each recorded command with its own uniforms. */
int  glsl_run_fragment_u(glsl_program *p, const float *varying,
                         const sgl_tex *const samplers[GLSL_MAX_SAMPLERS],
                         const float *ustore, float rgba[4]);

/* Tree-walk fragment evaluation: the correctness oracle and the fallback when the
 * A2 bytecode IR can't compile a shader. glsl_run_fragment() uses the IR when
 * available and this otherwise; tests compare the two. */
int  glsl_run_fragment_ref(glsl_program *p, const float *varying,
                           const sgl_tex *const samplers[GLSL_MAX_SAMPLERS],
                           float rgba[4]);

/* A3: SIMT-on-RVV fragment executor — shades up to GLSL_SIMT_MAXLANE pixels per
 * IR op. Inputs/outputs are SoA with a fixed GLSL_SIMT_MAXLANE-float stride per
 * component: varys[(varyfloat)*GLSL_SIMT_MAXLANE + lane], out[(channel)*GLSL_SIMT
 * _MAXLANE + lane] for channel 0..3. vl = active lane count (<= GLSL_SIMT_MAXLANE).
 * Only valid when glsl_fs_simt_ok(); otherwise use the scalar path per pixel. */
#define GLSL_SIMT_MAXLANE SGL_SIMT_LANES   /* single source of truth: sg_core.h */
extern int glsl_link_dump;
int  glsl_run_fragment_simt(glsl_program *p, const float *varys, int nvaryf, int vl,
                            const sgl_tex *const samplers[GLSL_MAX_SAMPLERS], float *out);
/* As glsl_run_fragment_simt, but reads uniforms from `ustore` (NULL -> p->ustore). */
int  glsl_run_fragment_simt_u(glsl_program *p, const float *varys, int nvaryf, int vl,
                              const sgl_tex *const samplers[GLSL_MAX_SAMPLERS],
                              const float *ustore, float *out);
int  glsl_fs_simt_ok(const glsl_program *p);  /* FS has IR AND is SIMT-vectorisable */
int  glsl_fs_has_discard(const glsl_program *p);  /* FS uses discard -> SIMT emits a per-lane kill mask */
/* Bit k = the bound fs_kernel reads varying float k; 0 = interpolate all. */
unsigned int glsl_fs_vary_live_mask(const glsl_program *p);
unsigned int glsl_fs_derivative_mask(const glsl_program *p);
int glsl_fs_uses_fragcoord(const glsl_program *p);
int  glsl_fs_alpha_zero_if_color_zero(const glsl_program *p);  /* out.a==0 wherever vary[3] (Color.a)==0 */
void glsl_dbg_record_draw_class(glsl_program *p, int cls);     /* 0=opaque 1=additive 2=alpha (glsl_link_dump) */
int  glsl_fs_has_kernel(const glsl_program *p);  /* 1 if a specialised kernel is bound */
const char *glsl_kernel_name(const glsl_program *p);  /* bound kernel label ("heathaze", ...) or "VM"/0 */
int  glsl_fs_cost(const glsl_program *p);     /* cheap draw-time fragment cost estimate */

/* SIMT vertex transform — process up to GLSL_SIMT_MAXLANE vertices per IR op
 * (lanes = vertices). attrs are SoA: 4 floats per attribute location,
 * attrs[(loc*4 + comp)*GLSL_SIMT_MAXLANE + lane]. Writes clip (gl_Position, 4 SoA
 * rows) and vary_out (glsl_num_varying_floats SoA rows), each stride
 * GLSL_SIMT_MAXLANE. vl = active vertex count (<= GLSL_SIMT_MAXLANE). The
 * gl_Position = MVP*pos transform uses the batched mat4*vec4 (16 vfmacc.vf)
 * kernel. Only valid when glsl_vs_simt_ok(); else use glsl_run_vertex per vertex.
 * Bit-exact (modulo signed zero) vs the scalar glsl_run_vertex. */
int  glsl_run_vertex_simt(glsl_program *p, const float *attrs, int vl,
                          float *clip, float *vary_out);
/* Variant for packed/compacted attribute streams: `loc_map[loc]` maps shader
 * attribute location -> attrs row. A negative/absent/out-of-range entry maps to
 * zero.
 * Callers that already use full location-indexed SoA rows may pass `loc_map=0`.
 */
int  glsl_run_vertex_simt_mapped(glsl_program *p, const float *attrs,
                                 const int *loc_map, int vl,
                                 float *clip, float *vary_out);
int  glsl_vs_simt_ok(const glsl_program *p);  /* VS has IR AND is SIMT-vectorisable */

#endif /* SOFTGLES2_GLSL_H */

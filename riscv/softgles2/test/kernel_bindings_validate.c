/* Dynamic sampler values, uniform values, and relinked attribute locations. */
#include "libc/libc.h"
#include "boot/runtime.h"
#include "glsl_internal.h"

static const char *vs =
"attribute vec4 _gl4es_Vertex; attribute vec4 _gl4es_Color;\n"
"uniform mat4 _gl4es_ModelViewProjectionMatrix;\n"
"varying vec4 Color; varying vec4 TexCoord_0;\n"
"void main(){gl_Position=_gl4es_ModelViewProjectionMatrix*_gl4es_Vertex;"
"Color=_gl4es_Color;TexCoord_0=vec4(0.5,0.5,0.0,1.0);}";
static const char *fs =
"precision mediump float; varying vec4 Color; varying vec4 TexCoord_0;\n"
"uniform sampler2D _gl4es_TexSampler_0;\n"
"void main(){gl_FragColor=Color*texture2D(_gl4es_TexSampler_0,TexCoord_0.xy);}";
static float vary[GLSL_MAX_VARYING_FLOATS * GLSL_SIMT_MAXLANE];
static float out[4 * GLSL_SIMT_MAXLANE], clip[4 * GLSL_SIMT_MAXLANE];
static float attrs[GLSL_MAX_ATTRIBS * 4 * GLSL_SIMT_MAXLANE];
static uint32_t red = 0xff0000ffu, green = 0xff00ff00u;

int kmain(void)
{
    glsl_program *p = glsl_program_create();
    char err[160];
    if (!p || !glsl_attach_source(p, 0, vs, err, sizeof err) ||
        !glsl_attach_source(p, 1, fs, err, sizeof err) || !glsl_link(p, err, sizeof err)) {
        printf("[bindings] FAIL link: %s\n", err); return 1;
    }
    sgl_tex a = {0}, b = {0};
    a.texels = &red; a.w = a.h = 1;
    b.texels = &green; b.w = b.h = 1;
    const sgl_tex *textures[GLSL_MAX_SAMPLERS] = {&a, &b};
    for (int c = 0; c < 4; ++c) vary[c * GLSL_SIMT_MAXLANE] = 1.f;
    vary[4 * GLSL_SIMT_MAXLANE] = vary[5 * GLSL_SIMT_MAXLANE] = 0.5f;
    if (!p->fs_kernel || !p->vs_kernel) { puts("[bindings] FAIL missing kernel"); return 1; }
    int sampler = glsl_uniform_location(p, "_gl4es_TexSampler_0");
    for (int unit = 0; unit < 2; ++unit) {
        glsl_uniform_sampler(p, sampler, unit);
        if (!p->fs_kernel(p, vary, 1, textures, p->ustore, out) ||
            out[0] != (unit == 0 ? 1.f : 0.f) ||
            out[GLSL_SIMT_MAXLANE] != (unit == 1 ? 1.f : 0.f)) {
            puts("[bindings] FAIL sampler update"); return 1;
        }
    }
    /* A successful relink must rebuild the attribute metadata. */
    glsl_bind_attrib(p, 2, "_gl4es_Vertex");
    if (!glsl_link(p, err, sizeof err)) { puts("[bindings] FAIL relink"); return 1; }
    float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    int loc = glsl_uniform_location(p, "_gl4es_ModelViewProjectionMatrix");
    attrs[(2 * 4) * GLSL_SIMT_MAXLANE] = 0.25f;
    attrs[(2 * 4 + 3) * GLSL_SIMT_MAXLANE] = 1.f;
    for (int scale = 1; scale <= 2; ++scale) {
        m[0] = (float)scale; glsl_uniform(p, loc, m, 16);
        if (!p->vs_kernel || !p->vs_kernel(p, attrs, 0, 1, clip, vary) || clip[0] != 0.25f * scale) {
            puts("[bindings] FAIL relink/uniform update"); return 1;
        }
    }
    puts("[bindings] PASS sampler update, relink, attribute remap, matrix update");
    glsl_program_destroy(p);
    return 0;
}

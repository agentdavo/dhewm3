/* Standalone host unit test for the GLSL ES interpreter (no rasteriser). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "glsl.h"

static const char *VS =
    "#version 100\n"
    "attribute vec4 position;\n"
    "attribute vec4 color;\n"
    "attribute vec2 texcoord;\n"
    "uniform mat4 mvp;\n"
    "varying vec4 v_color;\n"
    "varying vec2 v_uv;\n"
    "void main(){\n"
    "  gl_Position = mvp * position;\n"
    "  v_color = color;\n"
    "  v_uv = texcoord;\n"
    "}\n";

static const char *FS =
    "precision mediump float;\n"
    "uniform sampler2D tex0;\n"
    "varying vec4 v_color;\n"
    "varying vec2 v_uv;\n"
    "void main(){\n"
    "  vec4 t = texture2D(tex0, v_uv);\n"
    "  gl_FragColor = v_color * t;\n"
    "}\n";

static int fails = 0;
static void chk(const char *what, float got, float want)
{
    float d = got - want; if (d < 0) d = -d;
    int ok = d < 1e-4f;
    printf("  %-22s got=%.4f want=%.4f  %s\n", what, got, want, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

int main(void)
{
    char err[128] = {0};
    glsl_program *p = glsl_program_create();
    if (!glsl_attach_source(p, 0, VS, err, sizeof err)) { printf("VS compile error: %s\n", err); return 1; }
    if (!glsl_attach_source(p, 1, FS, err, sizeof err)) { printf("FS compile error: %s\n", err); return 1; }
    if (!glsl_link(p, err, sizeof err)) { printf("link error: %s\n", err); return 1; }

    printf("locations: position=%d color=%d texcoord=%d  mvp=%d tex0=%d  varyingFloats=%d\n",
           glsl_attrib_location(p, "position"), glsl_attrib_location(p, "color"),
           glsl_attrib_location(p, "texcoord"),
           glsl_uniform_location(p, "mvp"), glsl_uniform_location(p, "tex0"),
           glsl_num_varying_floats(p));

    /* mvp = translation by (0.1, 0.2, 0) in column-major order */
    float mvp[16] = {1,0,0,0,  0,1,0,0,  0,0,1,0,  0.1f,0.2f,0,1};
    glsl_uniform(p, glsl_uniform_location(p, "mvp"), mvp, 16);
    glsl_uniform_sampler(p, glsl_uniform_location(p, "tex0"), 0);

    /* a 2x2 texture: red, green / blue, white */
    uint32_t texels[4] = {
        0xFF0000FFu, /* (0,0) R */  0xFF00FF00u, /* (1,0) G */
        0xFFFF0000u, /* (0,1) B */  0xFFFFFFFFu, /* (1,1) W */
    };
    sgl_tex tex = {
        .w = 2, .h = 2, .texels = texels,
        .min_filter = 0, .wrap = 0,
    };
    const sgl_tex *samplers[GLSL_MAX_SAMPLERS] = {0};
    samplers[0] = &tex;

    /* vertex */
    float attribs[GLSL_MAX_ATTRIBS][4] = {{0}};
    int lp = glsl_attrib_location(p, "position");
    int lc = glsl_attrib_location(p, "color");
    int lt = glsl_attrib_location(p, "texcoord");
    attribs[lp][0]=0.5f; attribs[lp][1]=0.5f; attribs[lp][2]=0.0f; attribs[lp][3]=1.0f;
    attribs[lc][0]=1.0f; attribs[lc][1]=0.5f; attribs[lc][2]=0.25f; attribs[lc][3]=1.0f;
    attribs[lt][0]=0.0f; attribs[lt][1]=0.0f;  /* sample texel (0,0) = red */

    float clip[4], vary[GLSL_MAX_VARYING_FLOATS];
    glsl_run_vertex(p, attribs, clip, vary);
    printf("VS: clip=(%.3f,%.3f,%.3f,%.3f)\n", clip[0],clip[1],clip[2],clip[3]);
    printf("--- vertex stage (mat4*vec4 translate) ---\n");
    chk("clip.x", clip[0], 0.6f);   /* 0.5 + 0.1 */
    chk("clip.y", clip[1], 0.7f);   /* 0.5 + 0.2 */
    chk("clip.w", clip[3], 1.0f);

    /* fragment: v_color * texel(0,0)=red(1,0,0,1) -> (1,0,0,0.25? no a=1*1)=(1,0,0,1) */
    float rgba[4];
    int kept = glsl_run_fragment(p, vary, samplers, rgba);
    printf("FS: kept=%d rgba=(%.3f,%.3f,%.3f,%.3f)\n", kept, rgba[0],rgba[1],rgba[2],rgba[3]);
    printf("--- fragment stage (color * texture) ---\n");
    chk("frag.r", rgba[0], 1.0f);   /* 1.0 * 1.0 */
    chk("frag.g", rgba[1], 0.0f);   /* 0.5 * 0.0 */
    chk("frag.b", rgba[2], 0.0f);   /* 0.25 * 0.0 */
    chk("frag.a", rgba[3], 1.0f);   /* 1.0 * 1.0 */

    /* sample the white texel (1,1): color * white = color */
    vary[4]=1.0f; vary[5]=1.0f;
    glsl_run_fragment(p, vary, samplers, rgba);
    printf("--- fragment stage (white texel) ---\n");
    chk("frag.r(W)", rgba[0], 1.0f);
    chk("frag.g(W)", rgba[1], 0.5f);
    chk("frag.b(W)", rgba[2], 0.25f);

    glsl_program_destroy(p);
    printf(fails ? "\nRESULT: %d FAILURE(S)\n" : "\nRESULT: all checks passed\n", fails);
    return fails ? 1 : 0;
}

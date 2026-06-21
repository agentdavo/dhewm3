/*
 * Host test harness for softgles2's GLSL ES compiler (glsl.c).
 *
 * gl4es generates GLSL at runtime; this lets us feed those captured shaders to
 * glsl_try_compile() on the host and iterate the compiler in sub-second cycles
 * instead of a multi-minute QEMU boot. Stage is inferred from the filename
 * ("frag" -> fragment, else vertex), or pass it explicitly is not needed.
 *
 *   cc -I../src -I../include glsl_host_test.c ../src/glsl.c -o /tmp/glsltest
 *   /tmp/glsltest shader1.glsl shader2.glsl ...
 *
 * Exit code = number of shaders that FAILED to compile (0 = all good).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int glsl_try_compile(int stage, const char *src, char *err, int errlen);

/* glsl.c's evaluator references the rasteriser's texture fetch; stub it for the
 * compile-only harness (we only exercise parse/compile here, not evaluation). */
struct sgl_tex;
void sgl_sample(const struct sgl_tex *t, float u, float v, float out[4])
{ (void)t; (void)u; (void)v; out[0]=out[1]=out[2]=out[3]=0.f; }

int main(int argc, char **argv)
{
    int failed = 0;
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) { printf("%-22s CANNOT OPEN\n", argv[i]); failed++; continue; }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        char *s = (char *)malloc(n + 1);
        size_t rd = fread(s, 1, n, f); s[rd] = 0; fclose(f);
        int stage = strstr(argv[i], "frag") ? 1 : 0;
        char err[256]; err[0] = 0;
        int ok = glsl_try_compile(stage, s, err, (int)sizeof err);
        printf("%-22s %s%s%s\n", argv[i],
               ok ? "OK  " : "FAIL", ok ? "" : " : ", ok ? "" : err);
        if (!ok) failed++;
        free(s);
    }
    printf("---- %d/%d failed ----\n", failed, argc - 1);
    return failed;
}

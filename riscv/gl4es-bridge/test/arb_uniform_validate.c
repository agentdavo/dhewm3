/* Sparse, out-of-order reflection and relinking against production fpe.c. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "fpe.h"
#include "../glx/hardext.h"
hardext_t hardext;
static program_t p;
static void reflect(const char *name, int location) {
    char buf[128]; strcpy(buf, name);
    assert(builtin_CheckUniform(&p, buf, location, 1) == 1);
}
static void check(const int *locs, int limit, int capacity, int expected) {
    int active = 0;
    for (int i = 0; i < capacity; ++i) if (locs[i] >= 0) {
        assert(i < limit); ++active;
    }
    assert(active == expected && limit <= capacity);
}
int main(void) {
    builtin_Init(&p);
    reflect("_gl4es_Vertex_ProgramEnv_17", 71);
    reflect("_gl4es_Vertex_ProgramEnv_4", 41);
    reflect("_gl4es_Vertex_ProgramEnv_17", 72);
    reflect("_gl4es_Fragment_ProgramEnv_21", 91);
    reflect("_gl4es_Fragment_ProgramEnv_0", 90);
    reflect("_gl4es_Vertex_ProgramLocal_7", 12);
    reflect("_gl4es_Fragment_ProgramLocal_9", 13);
    check(p.vtx_progenv, p.has_vtx_progenv, MAX_VTX_PROG_ENV_PARAMS, 2);
    check(p.frg_progenv, p.has_frg_progenv, MAX_FRG_PROG_ENV_PARAMS, 2);
    check(p.vtx_progloc, p.has_vtx_progloc, MAX_VTX_PROG_LOC_PARAMS, 1);
    check(p.frg_progloc, p.has_frg_progloc, MAX_FRG_PROG_LOC_PARAMS, 1);
    assert(p.vtx_progenv[17] == 72 && p.vtx_progenv[4] == 41);
    printf("ARB sparse reflection: active=6, bounded_slots=%d, former_slots=%d\n",
        p.has_vtx_progenv+p.has_frg_progenv+p.has_vtx_progloc+p.has_frg_progloc,
        MAX_VTX_PROG_ENV_PARAMS+MAX_FRG_PROG_ENV_PARAMS+MAX_VTX_PROG_LOC_PARAMS+MAX_FRG_PROG_LOC_PARAMS);
    builtin_Init(&p);
    reflect("_gl4es_Vertex_ProgramEnv_0", 5);
    check(p.vtx_progenv, p.has_vtx_progenv, MAX_VTX_PROG_ENV_PARAMS, 1);
    check(p.frg_progenv, p.has_frg_progenv, MAX_FRG_PROG_ENV_PARAMS, 0);
    assert(p.has_frg_progenv == 0 && p.has_vtx_progenv == 1);
    puts("ARB relink: PASS");
    return 0;
}

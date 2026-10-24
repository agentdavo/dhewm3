/* Concurrent batch state must belong to the calling thread, including tails. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "sg_frag.h"

/* PE retains the other public executor entry points. They are outside this
 * batch-state test; abort if a future edit accidentally invokes a VM fallback. */
int ir_run_fragment_u(glsl_program *p, const float *v, const sgl_tex *const *s,
                      const float *u, float rgba[4]) { abort(); }
void glsl_run_vertex(glsl_program *p, const float a[GLSL_MAX_ATTRIBS][4],
                     float c[4], float *v) { abort(); }

static pthread_barrier_t barrier;
static int failures[4];

static void *worker(void *arg)
{
    int id = (int)(intptr_t)arg;
    for (int round = 0; round < 1000; ++round) {
        int base = id * 10000 + round;
        uint32_t packed[SGL_SIMT_LANES];
        for (int i = 0; i < SGL_SIMT_LANES; ++i) packed[i] = 2u * i;
        if (round & 1) sgl_simt_set_fragcoord_packed(base, packed, id, SGL_SIMT_LANES);
        else sgl_simt_set_fragcoord_run(base, id, SGL_SIMT_LANES);
        sgl_scalar_set_fragcoord(base, id);
        for (int k = 0; k < SGL_MAX_VARY; ++k) {
            sgl_simt_vary_ddx[k] = (float)(base + k);
            sgl_simt_vary_ddy[k] = (float)(base - k);
        }
        sgl_simt_deriv_valid = id;
        pthread_barrier_wait(&barrier);
        for (int i = 0; i < SGL_SIMT_LANES; ++i) {
            float x = (float)(base + ((round & 1) ? 2*i : i)) + .5f;
            if (sgl_simt_frag_x[i] != x || sgl_simt_frag_y[i] != id + .5f) ++failures[id];
        }
        for (int k = 0; k < SGL_MAX_VARY; ++k)
            if (sgl_simt_vary_ddx[k] != base+k || sgl_simt_vary_ddy[k] != base-k) {
                if (round == 0 && k == 0) printf("thread %d: dx=%g dy=%g expected=%d\n", id,
                    sgl_simt_vary_ddx[k], sgl_simt_vary_ddy[k], base);
                ++failures[id];
            }
        if (sgl_simt_deriv_valid != id || sgl_scalar_frag_x != base+.5f ||
            sgl_scalar_frag_y != id+.5f) ++failures[id];
        pthread_barrier_wait(&barrier);
    }
    return 0;
}

int main(void)
{
    pthread_t threads[4];
    if (pthread_barrier_init(&barrier, 0, 4)) return 2;
    for (int i = 0; i < 4; ++i)
        if (pthread_create(&threads[i], 0, worker, (void *)(intptr_t)i)) return 2;
    int total = 0;
    for (int i = 0; i < 4; ++i) { pthread_join(threads[i], 0); total += failures[i]; }
    pthread_barrier_destroy(&barrier);
    printf("[frag-tls] lanes=%d varyings=%d failures=%d %s\n", SGL_SIMT_LANES,
           SGL_MAX_VARY, total, total ? "FAIL" : "PASS");
    return total != 0;
}

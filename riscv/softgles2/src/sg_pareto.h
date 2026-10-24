/* Optional sampled RVV batch timings. Compile out of production entirely. */
#ifndef SGL_PARETO_H
#define SGL_PARETO_H
#include <stdint.h>
#ifndef SGL_PARETO
#define SGL_PARETO 0
#endif
void sgl_pareto_report(void);
#if SGL_PARETO && defined(__riscv)
extern int sgl_pareto_enabled;
uint64_t *sgl_pareto_select(unsigned mask);
static inline uint64_t sgl_pareto_clock(void) {
    uint64_t t; __asm__ volatile("rdtime %0" : "=r"(t) :: "memory"); return t;
}
#define PARETO_BEGIN() uint64_t *pt = sgl_pareto_enabled ? sgl_pareto_select(cmask) : 0; \
    uint64_t ptime = pt ? sgl_pareto_clock() : 0; unsigned pstage = 0
#define PARETO_STAGE(next) do { if(pt) { uint64_t now=sgl_pareto_clock(); \
    pt[pstage]+=now-ptime; ptime=now; pstage=(next); } } while(0)
#define PARETO_END() PARETO_STAGE(0)
#else
#define PARETO_BEGIN() ((void)0)
#define PARETO_STAGE(next) ((void)0)
#define PARETO_END() ((void)0)
#endif
#endif

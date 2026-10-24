/* Boundary nesting and requested-allocation conservation, without rendering. */
#include "../profile.h"
#include <stdint.h>
#include <stddef.h>
static uint64_t clock_tick;
uint64_t timer_ticks(void) { return ++clock_tick; }
uint64_t timer_ns_from_ticks(uint64_t t) { return t * 1000; }
extern void *shim_malloc(size_t);
extern void *shim_calloc(size_t,size_t);
extern void *shim_realloc(void*,size_t);
extern void shim_free(void*);
extern char *shim_strdup(const char*);
int main(void) {
    shim_span outer=shim_enter();
    uint64_t d=shim_driver_enter();
    uint64_t nested=shim_driver_enter();
    shim_driver_leave(nested);
    shim_driver_leave(d);
    shim_leave(outer,0,"test");
    void *p=shim_malloc(100), *q=shim_calloc(3,20);
    p=shim_realloc(p,200);
    char *s=shim_strdup("test");
    shim_free(q); shim_free(s);
    shim_profile_report();
    shim_free(p);
    shim_profile_report();
    return 0;
}

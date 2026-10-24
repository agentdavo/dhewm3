#ifndef GL4ES_BOUNDARY_PROFILE_H
#define GL4ES_BOUNDARY_PROFILE_H
#include <stdint.h>
typedef struct { uint64_t start, driver; int outer; } shim_span;
shim_span shim_enter(void);
void shim_leave(shim_span span, unsigned id, const char *name);
uint64_t shim_driver_enter(void);
void shim_driver_leave(uint64_t start);
void shim_profile_report(void);
#endif

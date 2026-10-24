/* Diagnostic only. Single GL submission thread; worker raster time is excluded
 * at the GLES boundary. No allocator interposition outside the gl4es archive. */
#include "profile.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
extern uint64_t timer_ticks(void);
extern uint64_t timer_ns_from_ticks(uint64_t);
static unsigned depth, driver_depth;
static uint64_t driver_ticks, inclusive_ticks, exclusive_ticks, api_calls;
static struct { const char *name; uint64_t ticks, calls; } apis[512];
shim_span shim_enter(void) {
    shim_span s = {timer_ticks(), driver_ticks, depth++ == 0}; return s;
}
void shim_leave(shim_span s, unsigned id, const char *name) {
    uint64_t elapsed = timer_ticks() - s.start;
    --depth;
    if (s.outer) {
        uint64_t own = elapsed - (driver_ticks - s.driver);
        inclusive_ticks += elapsed; exclusive_ticks += own; ++api_calls;
        if (id < 512) { apis[id].name=name; apis[id].ticks+=own; ++apis[id].calls; }
    }
}
uint64_t shim_driver_enter(void) { ++driver_depth; return timer_ticks(); }
void shim_driver_leave(uint64_t start) {
    uint64_t elapsed=timer_ticks()-start;
    if (--driver_depth == 0) driver_ticks += elapsed;
}
/* Requested bytes, not malloc arena capacity. Fixed table is profiler memory,
 * explicitly reported separately. Tombstones preserve open-addressing probes. */
#define NALLOC 131072u
static struct { void *p; size_t bytes; } allocs[NALLOC];
static uint64_t live, peak, count, peak_count, allocated, freed, misses, overflow;
static uint64_t frame_allocs, frame_frees;
static struct { void *pc; uint64_t bytes, calls; } sites[256];
static void site(void *pc,size_t n) {
    for(unsigned i=0;i<256;++i) if(!sites[i].pc || sites[i].pc==pc) {
        sites[i].pc=pc; sites[i].bytes+=n; ++sites[i].calls; return;
    }
}
static unsigned slot(void *p, int insert) {
    unsigned h=(unsigned)(((uintptr_t)p >> 4)*11400714819323198485ull)&(NALLOC-1);
    unsigned tomb=NALLOC;
    for (unsigned n=0;n<NALLOC;++n,h=(h+1)&(NALLOC-1)) {
        if (allocs[h].p==p) return h;
        if (allocs[h].p==(void*)1 && tomb==NALLOC) tomb=h;
        if (!allocs[h].p) return insert ? (tomb<NALLOC?tomb:h) : NALLOC;
    }
    return insert?tomb:NALLOC;
}
static void add(void *p,size_t n) {
    if (!p) return;
    unsigned h=slot(p,1);
    if(h==NALLOC) { ++overflow; return; }
    allocs[h].p=p; allocs[h].bytes=n;
    live+=n; allocated+=n; ++count; ++frame_allocs;
    if(live>peak)peak=live;
    if(count>peak_count)peak_count=count;
}
static void del(void *p) {
    if(!p)return;
    unsigned h=slot(p,0);
    if(h==NALLOC) { ++misses; return; }
    live-=allocs[h].bytes; freed+=allocs[h].bytes; --count; ++frame_frees;
    allocs[h].p=(void*)1;
}
void *shim_malloc(size_t n) { void*p=malloc(n); add(p,n); if(p)site(__builtin_return_address(0),n); return p; }
void *shim_calloc(size_t n,size_t s) { void*p=calloc(n,s); add(p,n*s); if(p)site(__builtin_return_address(0),n*s); return p; }
void shim_free(void*p) { del(p); free(p); }
void *shim_realloc(void*p,size_t n) {
    /* Remove old attribution before realloc can invalidate its address. */
    unsigned h=p?slot(p,0):NALLOC; size_t old=h<NALLOC?allocs[h].bytes:0;
    if(p)del(p);
    void*q=realloc(p,n);
    if(q)add(q,n); else if(p && n && h<NALLOC)add(p,old);
    return q;
}
char *shim_strdup(const char*s) { size_t n=strlen(s)+1; char*p=shim_malloc(n); if(p)memcpy(p,s,n); return p; }
void shim_profile_report(void) {
    static unsigned frame;
    printf("[shim] frame=%u calls=%lu inclusive_us=%lu driver_us=%lu exclusive_us=%lu live_bytes=%lu peak_bytes=%lu blocks=%lu peak_blocks=%lu allocs=%lu frees=%lu allocated_bytes=%lu freed_bytes=%lu untracked_frees=%lu overflow=%lu profiler_static_bytes=%lu depth=%u\n",
        ++frame,(unsigned long)api_calls,(unsigned long)(timer_ns_from_ticks(inclusive_ticks)/1000),
        (unsigned long)(timer_ns_from_ticks(driver_ticks)/1000),(unsigned long)(timer_ns_from_ticks(exclusive_ticks)/1000),
        (unsigned long)live,(unsigned long)peak,(unsigned long)count,(unsigned long)peak_count,
        (unsigned long)frame_allocs,(unsigned long)frame_frees,(unsigned long)allocated,(unsigned long)freed,
        (unsigned long)misses,(unsigned long)overflow,(unsigned long)(sizeof(allocs)+sizeof(apis)+sizeof(sites)),depth);
    for(int rank=0;rank<5;++rank) {
        unsigned best=0; for(unsigned i=1;i<512;++i)if(apis[i].ticks>apis[best].ticks)best=i;
        if(!apis[best].ticks)break;
        printf("[shim-api] %s calls=%lu exclusive_us=%lu\n",apis[best].name,(unsigned long)apis[best].calls,(unsigned long)(timer_ns_from_ticks(apis[best].ticks)/1000));
        apis[best].ticks=0;
    }
    memset(apis,0,sizeof(apis));
    for(unsigned i=0;i<256;++i) if(sites[i].calls)
        printf("[shim-alloc] pc=%p calls=%lu bytes=%lu\n", sites[i].pc,(unsigned long)sites[i].calls,(unsigned long)sites[i].bytes);
    memset(sites,0,sizeof(sites));
    driver_ticks=inclusive_ticks=exclusive_ticks=api_calls=0;
    frame_allocs=frame_frees=0;
}

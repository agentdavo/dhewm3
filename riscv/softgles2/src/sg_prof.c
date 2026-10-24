/* Per-stage render profiler (see header). */
#if defined(SGL_BAREMETAL)
#  include "libc/libc.h"
#else
#  include <stdio.h>
#endif
#include "sg_prof.h"
#include "sg_mt_internal.h"   /* MTR_* reject reasons + sgl_dbg_mt_reject[] */

extern uint64_t timer_ticks(void);

/* Ticks -> nanoseconds against the REAL timebase (boot/runtime.c reads
 * `timebase-frequency` from the device tree). This used to be a hardcoded
 * `ticks / 10000`, which bakes in QEMU virt's 10 MHz mtime: on real RVA23
 * silicon clocking mtime at 1 MHz or 24 MHz every number printed below would
 * have been wrong by that ratio, silently and with nothing to flag it.
 *
 * Note this buys no extra PRECISION -- the counter is 100 ns granular at 10 MHz
 * either way, and the accumulators were always uint64. What it buys is
 * portability and a self-describing unit. Printed to microseconds (%03lu) rather
 * than tenths of a millisecond, which matters for the sub-millisecond buckets
 * (bin, wait, release) where 0.1 ms resolution was quantising badly. */
#if defined(SGL_BAREMETAL)
extern uint64_t timer_ns_from_ticks(uint64_t ticks);
#else
static inline uint64_t timer_ns_from_ticks(uint64_t ticks) { (void)ticks; return 0; }
#endif
#define PF_MSA(t) (unsigned long)(timer_ns_from_ticks(t) / 1000000ull)
#define PF_MSB(t) (unsigned long)((timer_ns_from_ticks(t) / 1000ull) % 1000ull)

#if !defined(SGL_BAREMETAL)
/* Host builds have no rdtime cycle counter (it lives in boot/runtime on bare
 * metal). Profiling timings are 0 on host; the RVV target measures real cycles
 * on QEMU. Provided here so GLCORE links cleanly for host tests. */
uint64_t timer_ticks(void) { return 0; }
#endif

int      sgl_prof_enabled = 0;
int      sgl_prof_quiet = 0;
uint64_t sgl_prof_last_present, sgl_prof_last_total;
uint64_t sgl_prof_vertex, sgl_prof_dispatch, sgl_prof_raster, sgl_prof_present;
uint64_t sgl_prof_present_copy, sgl_prof_present_xfer, sgl_prof_present_flush;
uint64_t sgl_prof_draws, sgl_prof_tris, sgl_prof_frags;
uint64_t sgl_prof_mt_bin, sgl_prof_mt_release, sgl_prof_mt_mainbusy, sgl_prof_mt_wait;
uint64_t sgl_prof_hart_busy[SGL_PROF_MAXHARTS];
uint64_t sgl_prof_hart_tiles[SGL_PROF_MAXHARTS];
uint64_t sgl_prof_shade;
uint64_t sgl_prof_frag_lanes;
uint64_t sgl_prof_vs_gather;
uint64_t sgl_prof_vs_index, sgl_prof_vs_simt, sgl_prof_vs_scalar, sgl_prof_vs_cache;

void sgl_prof_enable(int on) {
    sgl_prof_enabled = on;
    /* State the timebase every measurement below is divided by. A wrong timebase
     * scales every reported time by a constant and is otherwise invisible, so it
     * should never be an unstated assumption in a log someone reads later. */
#if defined(SGL_BAREMETAL)
    if (on) {
        extern unsigned long timer_hz(void);
        extern int timer_hz_is_from_dtb(void);
        /* "dtb" vs "COMPILE-TIME FALLBACK" is the part that matters: QEMU virt
         * clocks mtime at exactly TIMEBASE_HZ, so a broken FDT walk reports the
         * identical frequency a working one does and cannot be told apart from
         * the number alone. */
        printf("[rprof-timebase] %lu Hz (%lu ns/tick) source=%s\n",
               timer_hz(), timer_hz() ? 1000000000ul / timer_hz() : 0ul,
               timer_hz_is_from_dtb() ? "dtb" : "COMPILE-TIME FALLBACK");
    }
#endif
}

uint64_t sgl_prof_now(void) { return sgl_prof_enabled ? timer_ticks() : 0; }

void sgl_prof_frame_report(void)
{
    if (!sgl_prof_enabled) return;
    /* `mt` MUST be in the total.
     *
     * This is the second time this profiler has mis-attributed the frame's
     * dominant cost. Handover 10 §2 found that `raster` only accumulates when MT
     * dispatch DECLINED a draw, so it was pure single-hart time being read as
     * total raster cost. Under FrameBin the mistake inverts: the raster work all
     * moves into the deferred flush, `raster` goes to 0.000ms in every frame, and
     * a total of vertex+dispatch+raster+present silently omits the entire
     * parallel section.
     *
     * Measured: the engine's own backend wall clock said 3458-3949ms/frame while
     * this line reported total=604-843ms, with mainbusy=2163-2588ms unaccounted.
     * `total + mtsum` reconciles with the engine. Anyone diffing the old `total`
     * across FrameBin on/off would have read a ~10x speedup that does not exist.
     *
     * mtsum is the SUBMITTER's view of the parallel section (bin + release +
     * its own share + straggler wait), so it is wall time and belongs in a wall
     * time total. The per-hart busy figures below are NOT added -- summing those
     * would count the same wall time once per hart. */
    uint64_t mtsum_total = sgl_prof_mt_bin + sgl_prof_mt_release +
                           sgl_prof_mt_mainbusy + sgl_prof_mt_wait;
    uint64_t total = sgl_prof_vertex + sgl_prof_dispatch + sgl_prof_raster +
                     sgl_prof_present + mtsum_total;
    extern volatile unsigned long sgl_dbg_mt_engaged, sgl_dbg_mt_cover_admit;
    extern volatile unsigned long sgl_dbg_draw_skipped_transparent;
    /* Print-suppression only. Every RESET below stays unconditional: skipping a
     * reset would turn these into cumulative counters for whoever is reading
     * them instead (the overlay), which is a subtler failure than silence. */
    const int quiet = sgl_prof_quiet;
    if (!quiet)
    printf("[rprof] draws=%lu tris=%lu frags=%lu skipxp=%lu mt_engaged=%lu mt_blits=%lu | vertex=%lu.%03lums dispatch=%lu.%03lums "
           "raster=%lu.%03lums mt=%lu.%03lums present=%lu.%03lums total=%lu.%03lums | present copy_gamma=%lu.%03lums xfer=%lu.%03lums flush=%lu.%03lums\n",
           (unsigned long)sgl_prof_draws, (unsigned long)sgl_prof_tris, (unsigned long)sgl_prof_frags,
           (unsigned long)sgl_dbg_draw_skipped_transparent,
           (unsigned long)sgl_dbg_mt_engaged, (unsigned long)sgl_dbg_mt_cover_admit,
           PF_MSA(sgl_prof_vertex), PF_MSB(sgl_prof_vertex),
           PF_MSA(sgl_prof_dispatch), PF_MSB(sgl_prof_dispatch),
           PF_MSA(sgl_prof_raster), PF_MSB(sgl_prof_raster),
           PF_MSA(mtsum_total), PF_MSB(mtsum_total),
           PF_MSA(sgl_prof_present), PF_MSB(sgl_prof_present),
           PF_MSA(total), PF_MSB(total),
           PF_MSA(sgl_prof_present_copy), PF_MSB(sgl_prof_present_copy),
           PF_MSA(sgl_prof_present_xfer), PF_MSB(sgl_prof_present_xfer),
           PF_MSA(sgl_prof_present_flush), PF_MSB(sgl_prof_present_flush));
    /* Guest instructions retired per frame, and instructions per fragment.
     *
     * Every other number on these lines is derived from rdtime, i.e. HOST wall
     * time, which moves when the host gets busy and cannot tell "the guest did
     * less work" from "the machine was quieter". instret counts guest work only.
     * For comparing two builds -- which is what optimisation actually needs --
     * this is the trustworthy figure and the ms columns are not.
     * Prints only when firmware left the counter readable from S-mode. */
#if defined(SGL_BAREMETAL)
    { extern uint64_t bm_rdinstret(void) __attribute__((weak));
      static uint64_t last_instret;
      if (bm_rdinstret) {
          uint64_t now = bm_rdinstret();
          if (now) {
              uint64_t d = now - last_instret;
              last_instret = now;
              if (!quiet)
              printf("[rprof-instret] frame=%lu.%03lu Minsn  %lu insn/frag\n",
                     (unsigned long)(d / 1000000ull),
                     (unsigned long)((d / 1000ull) % 1000ull),
                     (unsigned long)(sgl_prof_frags ? d / sgl_prof_frags : 0));
          }
      } }
#endif
    { extern unsigned long dos_heap_committed_bytes(void) __attribute__((weak));
      extern unsigned long dos_heap_capacity_bytes(void) __attribute__((weak));
      if (dos_heap_committed_bytes && dos_heap_capacity_bytes && !quiet) {
          unsigned long u=dos_heap_committed_bytes(), c=dos_heap_capacity_bytes();
          printf("[rprof-heap] committed=%luMB capacity=%luMB (%lu%%)\n",
                 u>>20, c>>20, c ? u*100/c : 0);
      } }
    { extern volatile unsigned long sgl_prof_vs_simt_draws, sgl_prof_vs_simt_verts;
      extern volatile unsigned long sgl_prof_vs_scalar_draws, sgl_prof_vs_scalar_verts;
      extern volatile unsigned long sgl_prof_vs_kernel_batches, sgl_prof_vs_kernel_verts;
      if (!quiet)
      printf("[rprof-vs] simt=%lu draws/%lu verts kernel=%lu batches/%lu verts scalar=%lu draws/%lu transformed-verts\n",
             sgl_prof_vs_simt_draws, sgl_prof_vs_simt_verts,
             sgl_prof_vs_kernel_batches, sgl_prof_vs_kernel_verts,
             sgl_prof_vs_scalar_draws, sgl_prof_vs_scalar_verts);
      sgl_prof_vs_simt_draws=sgl_prof_vs_simt_verts=0;
      sgl_prof_vs_scalar_draws=sgl_prof_vs_scalar_verts=0;
      sgl_prof_vs_kernel_batches=sgl_prof_vs_kernel_verts=0; }
    /* Vertex-stage draw-size distribution. The vertex transform is the last
     * serial stage in the frame, and how to parallelise it depends on whether
     * the vertices live in a few big draws (cheap: dispatch those to the hart
     * pool) or are spread thin across many small ones (needs the deferred
     * transform). `big%` is the share of all vertices in draws >= 256 verts,
     * which is the fraction a per-draw MT threshold could reach. */
    { extern volatile unsigned long sgl_prof_vs_hist_draws[], sgl_prof_vs_hist_verts[];
      static const char *lbl[8] = { "<64", "64", "128", "256", "512", "1k", "2k", ">=4k" };
      unsigned long tot = 0, big = 0;
      for (int b = 0; b < 8; ++b) { tot += sgl_prof_vs_hist_verts[b]; if (b >= 3) big += sgl_prof_vs_hist_verts[b]; }
      if (!quiet) {
      printf("[rprof-vshist]");
      for (int b = 0; b < 8; ++b)
          printf(" %s=%lu/%lu", lbl[b], sgl_prof_vs_hist_draws[b], sgl_prof_vs_hist_verts[b]);
      printf(" | big(>=256)=%lu%% of %lu verts\n", tot ? big * 100 / tot : 0, tot); }
      for (int b = 0; b < 8; ++b) { sgl_prof_vs_hist_draws[b] = 0; sgl_prof_vs_hist_verts[b] = 0; } }
    if (!quiet)
    printf("[rprof-vstime] index=%lu.%03lums gather=%lu.%03lums simt=%lu.%03lums scalar=%lu.%03lums cache=%lu.%03lums\n",
           PF_MSA(sgl_prof_vs_index), PF_MSB(sgl_prof_vs_index),
           PF_MSA(sgl_prof_vs_gather), PF_MSB(sgl_prof_vs_gather),
           PF_MSA(sgl_prof_vs_simt), PF_MSB(sgl_prof_vs_simt),
           PF_MSA(sgl_prof_vs_scalar), PF_MSB(sgl_prof_vs_scalar),
           PF_MSA(sgl_prof_vs_cache), PF_MSB(sgl_prof_vs_cache));
    /* dispatch sub-attribution + per-hart utilisation (Lever 1 measurement) */
    uint64_t mtsum = sgl_prof_mt_bin + sgl_prof_mt_release + sgl_prof_mt_mainbusy + sgl_prof_mt_wait;
    if (mtsum && !quiet) {
        printf("[rprof-mt] bin=%lu.%03lums release=%lu.%03lums mainbusy=%lu.%03lums wait=%lu.%03lums | frags=%lu",
               PF_MSA(sgl_prof_mt_bin), PF_MSB(sgl_prof_mt_bin),
               PF_MSA(sgl_prof_mt_release), PF_MSB(sgl_prof_mt_release),
               PF_MSA(sgl_prof_mt_mainbusy), PF_MSB(sgl_prof_mt_mainbusy),
               PF_MSA(sgl_prof_mt_wait), PF_MSB(sgl_prof_mt_wait),
               (unsigned long)sgl_prof_frags);
        printf(" | hart busy/tiles");
        uint64_t hartsum = 0;
        for (int h = 0; h < SGL_PROF_MAXHARTS; ++h) {
            hartsum += sgl_prof_hart_busy[h];
            if (sgl_prof_hart_tiles[h])
                printf(" h%d=%lu.%03lums/%lu", h,
                       PF_MSA(sgl_prof_hart_busy[h]), PF_MSB(sgl_prof_hart_busy[h]),
                       (unsigned long)sgl_prof_hart_tiles[h]);
        }
        /* shade = total time in frag_simt across harts; wrapper = the rest of the
         * per-batch raster work (coverage/early-Z/interp/blend/quantise/store). */
        uint64_t wrapper = hartsum > sgl_prof_shade ? hartsum - sgl_prof_shade : 0;
        unsigned long shpct = hartsum ? (unsigned long)(sgl_prof_shade * 100 / hartsum) : 0;
        unsigned long occ = sgl_prof_frag_lanes ? (unsigned long)(sgl_prof_frags * 100 / sgl_prof_frag_lanes) : 100;
        printf(" | shade=%lu.%03lums (%lu%%) wrapper=%lu.%03lums lanes=%lu occ=%lu%%\n",
               PF_MSA(sgl_prof_shade), PF_MSB(sgl_prof_shade), shpct,
               PF_MSA(wrapper), PF_MSB(wrapper),
               (unsigned long)sgl_prof_frag_lanes, occ);
    }

    /* Why draws were declined to the single-hart path. Printed unconditionally,
     * NOT inside the `if (mtsum)` above: a frame where every draw is rejected has
     * no MT time at all, and that is exactly the frame we most need to see.
     * prims= is the triangle count that went single-hart for each reason, which
     * is what ranks the gates — a gate rejecting one huge draw matters more than
     * one rejecting many trivial draws. */
    {
        static const char *mtr_name[MTR_N] = {
            "small", "noprim", "norefs", "active", "binrefs", "span", "score"
        };
        unsigned long rejtot = 0;
        for (int r = 0; r < MTR_N; ++r) rejtot += sgl_dbg_mt_reject[r];
        if (rejtot && !quiet) {
            printf("[rprof-mtrej] total=%lu", rejtot);
            for (int r = 0; r < MTR_N; ++r)
                if (sgl_dbg_mt_reject[r])
                    printf(" %s=%lu/prims=%lu", mtr_name[r],
                           (unsigned long)sgl_dbg_mt_reject[r],
                           (unsigned long)sgl_dbg_mt_reject_prims[r]);
            printf("\n");
        }
        for (int r = 0; r < MTR_N; ++r)
            sgl_dbg_mt_reject[r] = sgl_dbg_mt_reject_prims[r] = 0;
    }

    /* Phase 0: every 120 frames, dump the shader-workload ranking (which programs
     * shade the most fragments, kernel vs VM) — the archetype-specialisation
     * work-list. No-op unless glsl_link_dump populated the registry. */
    { extern void glsl_dbg_workload_report(void); static unsigned _fswork_fr = 0;
      ++_fswork_fr;
      if (!quiet && (_fswork_fr == 2u || (_fswork_fr % 10u) == 0u)) glsl_dbg_workload_report(); }

    /* Mip pyramid census, once every 10 frames. Without this a run in which the
     * application uploads no levels and calls no glGenerateMipmap is
     * indistinguishable from a working one -- both just look like "no speedup". */
    { extern unsigned long sgl_dbg_mip_levels_stored, sgl_dbg_mip_generated;
      static unsigned _mip_fr = 0;
      if (!quiet && (++_mip_fr % 10u) == 0u)
          printf("[mip] app-uploaded levels=%lu generated-pyramids=%lu\n",
                 sgl_dbg_mip_levels_stored, sgl_dbg_mip_generated); }

    { extern void sgl_light_probe_report_reset(void); sgl_light_probe_report_reset(); }

    /* Latch before zeroing, for in-frame consumers that cannot see these until
     * the frame is over (see sg_prof.h). */
    sgl_prof_last_present = sgl_prof_present;
    sgl_prof_last_total = total;

    sgl_prof_vertex = sgl_prof_dispatch = sgl_prof_raster = sgl_prof_present = 0;
    sgl_prof_present_copy = sgl_prof_present_xfer = sgl_prof_present_flush = 0;
    sgl_prof_draws = sgl_prof_tris = sgl_prof_frags = 0;
    sgl_dbg_mt_engaged = sgl_dbg_mt_cover_admit = 0;   /* per-frame engagement */
    { extern volatile unsigned long sgl_dbg_draw_skipped_transparent;
      sgl_dbg_draw_skipped_transparent = 0; }   /* per-frame transparent-layer skips */
    sgl_prof_mt_bin = sgl_prof_mt_release = sgl_prof_mt_mainbusy = sgl_prof_mt_wait = 0;
    sgl_prof_shade = 0;
    sgl_prof_frag_lanes = 0;
    sgl_prof_vs_gather = sgl_prof_vs_index = sgl_prof_vs_simt = sgl_prof_vs_scalar = sgl_prof_vs_cache = 0;
    for (int h = 0; h < SGL_PROF_MAXHARTS; ++h) { sgl_prof_hart_busy[h] = 0; sgl_prof_hart_tiles[h] = 0; }
}

/* Sample 1/64 batches using high LCG bits, independently on each worker.
 * Each worker owns its slot; frame presentation runs after the worker join.
 * Timings are worker wall ticks, not additive frame latency or CPU cycles.
 * Triangle/row setup and profiler selection are outside these intervals.
 */
#include "sg_pareto.h"
#if SGL_PARETO && defined(__riscv)
int sgl_pareto_enabled = 0;
static struct { uint64_t bins[16][6]; } pareto_slots[16] __attribute__((aligned(64)));
static unsigned pareto_next;
static __thread unsigned pareto_slot, pareto_rng;
uint64_t *sgl_pareto_select(unsigned mask) {
    if (!pareto_slot) {
        pareto_slot = __atomic_fetch_add(&pareto_next,1,__ATOMIC_RELAXED)+1;
        pareto_rng = pareto_slot * 2654435761u;
    }
    pareto_rng = pareto_rng * 1664525u + 1013904223u;
    if ((pareto_rng >> 26) || pareto_slot > 16) return 0;
    uint64_t *p=pareto_slots[pareto_slot-1].bins[mask&15]; ++p[5]; return p;
}
void sgl_pareto_report(void) {
    if (!sgl_pareto_enabled) return;
    for (unsigned m=0;m<16;++m) {
        uint64_t sum[6]={0};
        for(unsigned h=0;h<16;++h) for(unsigned k=0;k<6;++k) {
            sum[k]+=pareto_slots[h].bins[m][k]; pareto_slots[h].bins[m][k]=0;
        }
        if(sum[5]) printf("[pareto] mask=%u samples=%lu coverage_us=%lu interp_us=%lu deriv_us=%lu shader_us=%lu output_us=%lu\n",
            m,(unsigned long)sum[5],(unsigned long)(timer_ns_from_ticks(sum[0])/1000),
            (unsigned long)(timer_ns_from_ticks(sum[1])/1000),(unsigned long)(timer_ns_from_ticks(sum[2])/1000),
            (unsigned long)(timer_ns_from_ticks(sum[3])/1000),(unsigned long)(timer_ns_from_ticks(sum[4])/1000));
    }
}
#else
void sgl_pareto_report(void) {}
#endif

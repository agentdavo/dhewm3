/*
 * mt_layout_bench.c
 *
 * Small host-side benchmark harness for sg_mt_layout_choose policy sweeps.
 * Keeps behavior unchanged (defaults preserved) and prints candidate picks for a
 * synthetic workload matrix so layout thresholds can be tuned from measured
 * timing + layout shape samples.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <stdint.h>

#include "sg_mt_internal.h"

static void benchmark_case(const char *name, int nprim, int fbw, int fbh,
                          const sg_mt_layout_policy *policy, int iters)
{
    struct timespec t0;
    struct timespec t1;
    sg_mt_tile_layout layout = {0};
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < iters; ++i) {
        layout = sg_mt_layout_choose(nprim, fbw, fbh, policy);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    long long dt_ns = (long long)(t1.tv_sec - t0.tv_sec) * 1000000000LL + (t1.tv_nsec - t0.tv_nsec);
    long long ns = (iters > 0) ? dt_ns / (long long)iters : 0;
    printf("%s,%d,%d,%d,%d,%d,%d,%lld\n",
           name, nprim, fbw, fbh, layout.w, layout.h, iters, ns);
}

typedef struct { int x0, y0, x1, y1; } synth_prim_box;

static inline synth_prim_box synth_box_for_prim(int i, int fbw, int fbh)
{
    uint32_t h = (uint32_t)i * 0x9E3779B9u + 0x7F4A7C15u;
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;

    int x0 = (int)(h % (uint32_t)(fbw > 1 ? fbw : 1));
    int y0 = (int)((h >> 16) % (uint32_t)(fbh > 1 ? fbh : 1));
    int maxw = fbw > 1 ? (fbw / 16 + 1) : 1;
    int maxh = fbh > 1 ? (fbh / 16 + 1) : 1;
    int w = 1 + (int)(h % (uint32_t)maxw);
    int hh = 1 + (int)((h >> 8) % (uint32_t)maxh);

    int x1 = x0 + w; if (x1 >= fbw) x1 = fbw - 1;
    int y1 = y0 + hh; if (y1 >= fbh) y1 = fbh - 1;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    return (synth_prim_box){ x0, y0, x1, y1 };
}

static long long layout_bench_sim_cost(int nprim, int fbw, int fbh,
                                      const sg_mt_layout_policy *policy,
                                      int iters, long long *score_out)
{
    struct timespec t0;
    struct timespec t1;
    if (iters < 1) iters = 1;

    volatile long long sink = 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < iters; ++i) {
        sg_mt_tile_layout l = sg_mt_layout_choose(nprim, fbw, fbh, policy);
        if (score_out) {
            int tx = (fbw + l.w - 1) / l.w;
            int ty = (fbh + l.h - 1) / l.h;
            *score_out = (long long)nprim * tx * ty;
        }
        int tx = (fbw + l.w - 1) / l.w;
        int ty = (fbh + l.h - 1) / l.h;
        int safe_tx = tx > 0 ? tx : 1;
        int safe_ty = ty > 0 ? ty : 1;
        sink += (long long)(i + 1) * safe_tx + safe_ty;
        for (int p = 0; p < nprim; ++p) {
            synth_prim_box b = synth_box_for_prim((uint32_t)(i * 1315423911u + p), fbw, fbh);
            int bx0 = b.x0 / l.w;
            int bx1 = b.x1 / l.w;
            int by0 = b.y0 / l.h;
            int by1 = b.y1 / l.h;
            if (bx0 < 0) bx0 = 0;
            if (by0 < 0) by0 = 0;
            if (bx1 >= safe_tx) bx1 = safe_tx - 1;
            if (by1 >= safe_ty) by1 = safe_ty - 1;
            for (int ty2 = by0; ty2 <= by1; ++ty2)
                for (int tx2 = bx0; tx2 <= bx1; ++tx2)
                    sink += (long long)(ty2 * safe_tx + tx2);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    long long dt_ns = (long long)(t1.tv_sec - t0.tv_sec) * 1000000000LL +
                     (long long)(t1.tv_nsec - t0.tv_nsec);
    if (sink == 0) sink = 1; /* keep result live */
    return dt_ns / (long long)iters;
}

static int next_nprim(int nprim, int stride, int max_prim)
{
    if (nprim <= 0) return max_prim + 1;
    if (nprim >= max_prim) return max_prim + 1;
    if (stride <= 1) return nprim + 1;
    if (nprim > INT_MAX / stride) return max_prim + 1;

    int next = nprim * stride;
    return (next < nprim) ? max_prim + 1 : next;
}

static long long layout_score(int nprim, int fbw, int fbh, int w, int h)
{
    int tx = (fbw + w - 1) / w;
    int ty = (fbh + h - 1) / h;
    if (tx < 1) tx = 1;
    if (ty < 1) ty = 1;
    int tiles = tx * ty;
    return (long long)nprim * 1000LL - (long long)tiles * 8LL;
}

static long long layout_bench_cost(int nprim, int fbw, int fbh,
                                  const sg_mt_layout_policy *p,
                                  int iters, long long *score_out)
{
    struct timespec t0;
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    sg_mt_tile_layout layout = sg_mt_layout_choose(nprim, fbw, fbh, p);
    for (int i = 1; i < iters; ++i) {
        layout = sg_mt_layout_choose(nprim, fbw, fbh, p);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long long dt_ns = (long long)(t1.tv_sec - t0.tv_sec) * 1000000000LL +
                     (long long)(t1.tv_nsec - t0.tv_nsec);
    if (score_out) *score_out = layout_score(nprim, fbw, fbh, layout.w, layout.h);
    return (iters > 0) ? dt_ns / (long long)iters : 0;
}

int main(int argc, char **argv)
{
    int iters = 200000;
    int do_tune = 0;
    int min_prim = 256;
    int max_prim = 32768;
    int fbw = 1920;
    int fbh = 1080;
    int stride = 1;
    int do_recommend = 0;
    int sim_work = 0;
    sg_mt_layout_policy policy = sg_mt_layout_policy_default();
    const char *bench_kind = "bench";
    long long (*bench_fn)(int,int,int,const sg_mt_layout_policy*,int,long long*) = layout_bench_cost;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--iters") && i + 1 < argc) {
            iters = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--min-prim") && i + 1 < argc) {
            min_prim = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--max-prim") && i + 1 < argc) {
            max_prim = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--fbw") && i + 1 < argc) {
            fbw = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--fbh") && i + 1 < argc) {
            fbh = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--mul2")) {
            stride = 2;
        } else if (!strcmp(argv[i], "--tune")) {
            do_tune = 1;
        } else if (!strcmp(argv[i], "--simbench")) {
            sim_work = 1;
            bench_fn = layout_bench_sim_cost;
            bench_kind = "sim";
        } else if (!strcmp(argv[i], "--recommend")) {
            do_recommend = 1;
        }
    }

    if (iters < 1) iters = 1;
    if (min_prim < 1) min_prim = 1;
    if (max_prim < min_prim) max_prim = min_prim;
    if (fbw < 1) fbw = 1;
    if (fbh < 1) fbh = 1;
    if (do_tune) {
        /* Local copy of candidates so we can evaluate and pick stable winners. */
        int split_thr[] = { 512, 1024, 2048, 4096, 8192 };
        int split_area[] = { 1, 2, 4 };
        int split_active[] = { 12, 24, 36, 48 };
        int split_thr_n = (int)(sizeof(split_thr) / sizeof(split_thr[0]));
        int split_area_n = (int)(sizeof(split_area) / sizeof(split_area[0]));
        int split_active_n = (int)(sizeof(split_active) / sizeof(split_active[0]));

        long long best_total_ns = LLONG_MAX;
        sg_mt_layout_policy best_reco = policy;

        printf("# MT layout tuning sweep\n");
        printf("# args: iters=%d fbw=%d fbh=%d prim_range=[%d,%d] stride=%dX bench=%s\n",
               iters, fbw, fbh, min_prim, max_prim, stride, bench_kind);
        if (sim_work)
            printf("# simbench: synthetic primitive-to-tile bin-work used with sg_mt_layout_choose\n");
        else
            printf("# tunebench: pure sg_mt_layout_choose call timing (candidate filter only)\n");
        printf("nprim,split_prim_threshold,split_area_div,min_active_tiles,layout_w,layout_h,ns_per_call,score\n");

        for (int nprim = min_prim; nprim <= max_prim; nprim = next_nprim(nprim, stride, max_prim)) {
            long long best_ns = LLONG_MAX;
            long long best_score = LLONG_MAX;
            sg_mt_layout_policy best_policy = policy;
            sg_mt_tile_layout best_layout = {SGL_MT_TILE_W, SGL_MT_TILE_H};

            for (int ti = 0; ti < split_thr_n; ++ti) {
                for (int ai = 0; ai < split_area_n; ++ai) {
                    for (int mi = 0; mi < split_active_n; ++mi) {
                        sg_mt_layout_policy p = {split_thr[ti], split_area[ai], split_active[mi]};
                        long long score = 0;
                        long long ns = bench_fn(nprim, fbw, fbh, &p, iters, &score);
                        if (ns < best_ns || (ns == best_ns && score < best_score)) {
                            best_ns = ns;
                            best_score = score;
                            best_policy = p;
                            best_layout = sg_mt_layout_choose(nprim, fbw, fbh, &p);
                        }
                    }
                }
            }
            printf("%d,%d,%d,%d,%d,%d,%lld,%lld\n",
                   nprim, best_policy.split_prim_threshold, best_policy.split_area_div,
                   best_policy.min_active_tiles, best_layout.w, best_layout.h, best_ns, best_score);
        }

        if (do_recommend) {
            for (int ti = 0; ti < split_thr_n; ++ti) {
                for (int ai = 0; ai < split_area_n; ++ai) {
                    for (int mi = 0; mi < split_active_n; ++mi) {
                        sg_mt_layout_policy p = {split_thr[ti], split_area[ai], split_active[mi]};
                        long long ns_sum = 0;
                        for (int nprim = min_prim; nprim <= max_prim; nprim = next_nprim(nprim, stride, max_prim)) {
                            long long s = 0;
                            long long ns = bench_fn(nprim, fbw, fbh, &p, iters, &s);
                            ns_sum += ns;
                        }
                        if (ns_sum < best_total_ns) {
                            best_total_ns = ns_sum;
                            best_reco = p;
                        }
                    }
                }
            }
            printf("# RECOMMENDED split policy for this sweep: split_prim_threshold=%d, split_area_div=%d, min_active_tiles=%d (sum_ns=%lld)\n",
                   best_reco.split_prim_threshold, best_reco.split_area_div, best_reco.min_active_tiles, best_total_ns);
        }
        return 0;
    }
    if (do_recommend) {
        printf("[mt_layout_bench] --recommend only valid with --tune\n");
    }

    typedef struct {
        const char *name;
        sg_mt_layout_policy p;
    } policy_row;

    policy_row policies[] = {
        { "default", policy },
        { "aggr_split", { SGL_MT_TILE_SPLIT_PRIMS / 2, 1, 12 } },
        { "no_split", { SGL_MT_TILE_SPLIT_PRIMS * 16, 2, 96 } },
    };
    int npolicies = (int)(sizeof(policies) / sizeof(policies[0]));

    printf("# MT layout benchmark\n");
    printf("# args: iters=%d fbw=%d fbh=%d prim_range=[%d,%d] stride=%dx bench=%s\n",
           iters, fbw, fbh, min_prim, max_prim, stride, bench_kind);
    printf("policy,nprim,fbw,fbh,layout_w,layout_h,iters,ns_per_call,score\n");

    for (int p = 0; p < npolicies; ++p) {
        for (int nprim = min_prim; nprim <= max_prim; nprim = next_nprim(nprim, stride, max_prim)) {
            sg_mt_tile_layout best = sg_mt_layout_choose(nprim, fbw, fbh, &policies[p].p);
            long long score = layout_score(nprim, fbw, fbh, best.w, best.h);
            long long ns = bench_fn(nprim, fbw, fbh, &policies[p].p, iters, 0);
            printf("%s,%d,%d,%d,%d,%d,%d,%lld,%lld\n",
                   policies[p].name, nprim, fbw, fbh, best.w, best.h, iters, ns, score);
        }
    }

    if (sim_work) {
        for (int nprim = min_prim; nprim <= max_prim; nprim = next_nprim(nprim, stride, max_prim)) {
            long long score = 0;
            long long ns = layout_bench_sim_cost(nprim, fbw, fbh, &policy, iters, &score);
            printf("null-policy-sim,%d,%d,%d,%d,%d,%d,%lld,%lld\n",
                   nprim, fbw, fbh, SGL_MT_TILE_W, SGL_MT_TILE_H, iters, ns, score);
        }
    } else {
        benchmark_case("default-null", min_prim, fbw, fbh, 0, iters);
        benchmark_case("default-null", max_prim, fbw, fbh, 0, iters);
    }

    return 0;
}

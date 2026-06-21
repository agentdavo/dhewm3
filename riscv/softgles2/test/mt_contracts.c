/* Contract checks for multi-hart raster dispatch structures.
 *
 * These are host-side tests that assert immutable job payload layout invariants:
 *  - immutable payload contracts (dispatch-owned backing arrays + in-range invariants);
 *  - tile offsets are monotonic and terminate at bin_refs;
 *  - active tile list ordering/uniqueness when an explicit list is supplied;
 *  - active tile list is always treated as compact owner-free dispatch metadata.
 */
#include "sg_mt_internal.h"

#include <stdio.h>
static int checks = 0;
static int failures = 0;

#define CHECK(expr, msg) do {                                                      \
    ++checks;                                                                      \
    if (!(expr)) {                                                                 \
        ++failures;                                                                \
        printf("[mt-contract] FAIL: %s\n", (msg));                                  \
    }                                                                              \
} while (0)

int main(void)
{
    sgl_framebuffer fb;
    sgl_drawstate st = {0};
    sg_draw_packet draw = { 0 };
    draw.fb = &fb;
    draw.state = st;

    sgl_pvert pv[4] = {
        {0, 0, 0, 1, 0},
        {0, 0, 0, 1, 0},
        {0, 0, 0, 1, 0},
        {0, 0, 0, 1, 0},
    };
    sg_prim_desc prims[2] = {
        {0, 1, 2, 0.1f},
        {1, 2, 3, 0.2f},
    };

    int offsets_valid[3] = {0, 1, 2};
    int refs_valid[2] = {0, 1};
    int active_valid[2] = {0, 1};

    sg_prog_job job_ok = {
        &draw, pv, prims, 2, offsets_valid, refs_valid,
        active_valid, 2, 1, 2, 4, 2, {1, 2}
    };
    CHECK(sg_mt_validate_job(&job_ok, 2, 2), "valid immutable job validates");

    sg_prog_job job_bad_offsets = job_ok;
    int offsets_bad[3] = {1, 1, 2};
    job_bad_offsets.tile_offsets = offsets_bad;
    CHECK(!sg_mt_validate_job(&job_bad_offsets, 2, 2), "offsets must start at zero");

    sg_prog_job job_offsets_desc = job_ok;
    int offsets_desc[3] = {0, 2, 1};
    job_offsets_desc.tile_offsets = offsets_desc;
    CHECK(!sg_mt_validate_job(&job_offsets_desc, 2, 2), "offsets must be monotonic non-decreasing");

    sg_prog_job job_bad_active = job_ok;
    int active_bad[2] = {-1, 1};
    job_bad_active.active_tiles = active_bad;
    CHECK(!sg_mt_validate_job(&job_bad_active, 2, 2), "active tiles must be non-negative/in-range");

    sg_prog_job job_active_unsorted = job_ok;
    int active_unsorted[2] = {1, 0};
    job_active_unsorted.active_tiles = active_unsorted;
    CHECK(!sg_mt_validate_job(&job_active_unsorted, 2, 2), "active tile list must be sorted");

    sg_prog_job job_active_dupe = job_ok;
    int active_dupe[2] = {0, 0};
    job_active_dupe.active_tiles = active_dupe;
    CHECK(!sg_mt_validate_job(&job_active_dupe, 2, 2), "active tile list must be unique");

    sg_prog_job job_active_neg = job_ok;
    job_active_neg.active_count = -1;
    CHECK(!sg_mt_validate_job(&job_active_neg, 2, 2), "active_count must be non-negative");

    sg_prog_job job_too_many_active = job_ok;
    job_too_many_active.active_count = 3;
    CHECK(!sg_mt_validate_job(&job_too_many_active, 2, 2), "active_count must not exceed tile count");

    sg_prog_job job_no_active = {
        &draw, pv, prims, 2, offsets_valid, refs_valid,
        0, 0, 1, 2, 4, 2, {1, 2}
    };
    CHECK(sg_mt_validate_job(&job_no_active, 2, 2), "null active list is valid with zero active count");
    CHECK(sg_mt_validate_job(&job_no_active, 2, 2), "active_count=0 with NULL active list is treated as implicit full-set scan");

    sg_prog_job job_no_active_bad_count = {
        &draw, pv, prims, 2, offsets_valid, refs_valid,
        0, -1, 1, 2, 4, 2, {1, 2}
    };
    CHECK(!sg_mt_validate_job(&job_no_active_bad_count, 2, 2), "null active list still enforces non-negative count");

    sg_prog_job job_no_active_bad_count2 = {
        &draw, pv, prims, 2, offsets_valid, refs_valid,
        0, 3, 1, 2, 4, 2, {1, 2}
    };
    CHECK(!sg_mt_validate_job(&job_no_active_bad_count2, 2, 2), "null active list still enforces in-range count");
    sg_prog_job job_refs_null = job_ok;
    job_refs_null.tile_refs = 0;
    CHECK(!sg_mt_validate_job(&job_refs_null, 2, 2), "non-zero bin_refs requires non-null tile_refs");
    sg_prog_job job_nonnull_zero_active = {
        &draw, pv, prims, 2, offsets_valid, refs_valid,
        active_valid, 0, 1, 2, 4, 2, {1, 2}
    };
    CHECK(!sg_mt_validate_job(&job_nonnull_zero_active, 2, 2), "non-empty active list with zero count rejected");

    sg_prog_job job_tiles_mismatch = {
        &draw, pv, prims, 2, offsets_valid, refs_valid,
        0, 0, 3, 1, 4, 2, {1, 2}
    };
    CHECK(!sg_mt_validate_job(&job_tiles_mismatch, 3, 2), "tiles_x * tiles_y must match total_tiles");

    {
        sg_mt_layout_policy policy_default = sg_mt_layout_policy_default();
        sg_mt_tile_layout layout_default = sg_mt_layout_choose(0, 1024, 1024, &policy_default);
        sg_mt_tile_layout layout_null = sg_mt_layout_choose(0, 1024, 1024, 0);
        CHECK(layout_default.w == layout_null.w && layout_default.h == layout_null.h,
              "NULL policy uses default layout policy");

        sg_mt_layout_policy policy_no_split = policy_default;
        policy_no_split.split_prim_threshold = 1 << 30;
        sg_mt_tile_layout layout_split_off = sg_mt_layout_choose(4096, 1024, 1024, &policy_no_split);
        CHECK(layout_split_off.w == SGL_MT_TILE_W && layout_split_off.h == SGL_MT_TILE_H,
              "excessive split threshold disables split");
    }

    sg_prog_job job_bad_tiles = {
        &draw, pv, prims, 2, offsets_valid, refs_valid,
        active_valid, 2, 1, 2, 4, 2, {0, 1}
    };
    CHECK(!sg_mt_validate_job(&job_bad_tiles, 2, 2), "zero-area layout is rejected");

    sg_prog_job job_null_draw = job_ok;
    job_null_draw.draw = 0;
    CHECK(!sg_mt_validate_job(&job_null_draw, 2, 2), "dispatch requires non-null draw packet");

    sg_prog_job job_null_prims = job_ok;
    job_null_prims.prims = 0;
    CHECK(!sg_mt_validate_job(&job_null_prims, 2, 2), "dispatch requires non-null primitive descriptors");

    sg_prog_job job_bad_offsets_neg = job_ok;
    int offsets_neg[3] = {-1, 0, 2};
    job_bad_offsets_neg.tile_offsets = offsets_neg;
    CHECK(!sg_mt_validate_job(&job_bad_offsets_neg, 2, 2), "offsets must be >= 0");

    sg_prog_job job_refs_too_large = job_ok;
    CHECK(!sg_mt_validate_job(&job_refs_too_large, 2, 99), "bin_refs upper bound must match tile_offsets end");

    if (failures) {
        printf("[mt-contract] %d checks, %d failures\n", checks, failures);
        return 1;
    }
    printf("[mt-contract] %d checks, all PASS\n", checks);
    return 0;
}

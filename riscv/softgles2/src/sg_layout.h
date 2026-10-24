/* RVV/SIMT data-layout contracts.
 *
 * The hot paths operate on lane-contiguous SoA buffers:
 *   value[component * SGL_SIMT_LANES + lane]
 *
 * Keep alignment explicit at the layout boundary instead of relying on compiler
 * defaults or ad-hoc attributes in individual kernels.
 */
#ifndef SOFTGLES2_SG_LAYOUT_H
#define SOFTGLES2_SG_LAYOUT_H

#include "sg_align.h"
#include "sg_core.h"

typedef struct {
    float c[4][SGL_SIMT_LANES];
} sg_vec4_soa SG_ALIGNED_TYPE(SG_RVV_ALIGN);

/* Fragment shade buffer: 4 RGBA rows + a 5th per-lane discard/kill row (1=discard),
 * for SIMT alpha-test. Row 4 is written only by shaders with discard and read only
 * when the drawstate's has_discard is set. */
typedef struct {
    float c[5][SGL_SIMT_LANES];
} sg_fragout_soa SG_ALIGNED_TYPE(SG_RVV_ALIGN);

typedef struct {
    float c[SGL_MAX_VARY][SGL_SIMT_LANES];
} sg_vary_soa SG_ALIGNED_TYPE(SG_RVV_ALIGN);

typedef struct {
    union {
        float c[4][SGL_SIMT_LANES];
        float mat[16]; /* Uniform matrices must fit even with eight lanes. */
    };
    int n;
    int is_mat;
    int _pad[14];
} sg_simt_reg_soa SG_ALIGNED_TYPE(SG_RVV_ALIGN);

SG_STATIC_ASSERT(sizeof(sg_vec4_soa) % SG_RVV_ALIGN == 0);
SG_STATIC_ASSERT(sizeof(sg_vary_soa) % SG_RVV_ALIGN == 0);
SG_STATIC_ASSERT(sizeof(sg_simt_reg_soa) % SG_RVV_ALIGN == 0);

#endif /* SOFTGLES2_SG_LAYOUT_H */

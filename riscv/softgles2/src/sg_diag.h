/* Internal diagnostics ownership.
 *
 * This module owns the global diagnostics enable switch and shared counter
 * helpers used across draw/raster/shader/vertex/SIMT code. Individual modules
 * still own their counters; this header keeps the enable flag from becoming a
 * loose ad-hoc extern in every implementation file.
 */
#ifndef SOFTGLES2_SG_DIAG_H
#define SOFTGLES2_SG_DIAG_H

#include "sgl.h"

extern int sgl_diag_enabled;
extern volatile unsigned long sgl_dbg_tile_refs;
extern volatile unsigned long sgl_dbg_frame_no;
extern volatile unsigned long sgl_dbg_draw_no;
extern int sgl_dbg_draw_filter_enabled;
extern int sgl_dbg_draw_frame_first;
extern int sgl_dbg_draw_frame_last;
extern int sgl_dbg_draw_first;
extern int sgl_dbg_draw_last;
extern int sgl_dbg_draw_detail;

int sgl_diag_present_report(const sgl_framebuffer *fb, int capture_armed);
void sgl_diag_draw_filter_set(int enabled, int frame_first, int frame_last,
                              int draw_first, int draw_last, int detail);
unsigned long sgl_diag_note_draw(void);
int sgl_diag_draw_selected(unsigned long draw_no);

#define SGL_DIAG_INC(x) do { if (sgl_diag_enabled) (x)++; } while (0)
#define SGL_DIAG_ADD(x, n) do { if (sgl_diag_enabled) (x) += (n); } while (0)

#endif

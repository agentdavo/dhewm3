#include "sg_diag.h"

#if defined(SGL_BAREMETAL)
#  include "libc/libc.h"
#else
#  include <stdio.h>
#endif

int sgl_diag_enabled = 0;
volatile unsigned long sgl_dbg_tile_refs = 0;
volatile unsigned long sgl_dbg_frame_no = 0;
volatile unsigned long sgl_dbg_draw_no = 0;
int sgl_dbg_draw_filter_enabled = 0;
int sgl_dbg_draw_frame_first = -1;
int sgl_dbg_draw_frame_last = -1;
int sgl_dbg_draw_first = -1;
int sgl_dbg_draw_last = -1;
int sgl_dbg_draw_detail = 0;

extern volatile unsigned long sgl_dbg_tris;
extern volatile unsigned long sgl_dbg_tris_onscreen;
extern volatile unsigned long sgl_dbg_frag_zpass;
extern volatile unsigned long sgl_dbg_frag_cmask0;
extern volatile unsigned long sgl_dbg_frag_shaded;
extern volatile unsigned long sgl_dbg_frag_shader_nonzero;
extern volatile unsigned long sgl_dbg_frag_store;
extern volatile unsigned long sgl_dbg_frag_store_nonzero;
extern volatile unsigned long sgl_dbg_frag_ow;
extern volatile unsigned long sgl_dbg_frag_ow_black;
extern volatile unsigned long sgl_dbg_shader_link_glsl;
extern volatile unsigned long sgl_dbg_shader_link_builtin;
extern volatile unsigned long sgl_dbg_shader_link_fail;
extern volatile unsigned long sgl_dbg_draw_glsl;
extern volatile unsigned long sgl_dbg_draw_builtin;
extern volatile unsigned long sgl_dbg_draw_simt;
extern volatile unsigned long sgl_dbg_draw_scalar;
extern volatile unsigned long sgl_dbg_draw_fbo;
extern volatile unsigned long sgl_dbg_draw_default;
extern volatile unsigned long sgl_dbg_vs_simt;
extern volatile unsigned long sgl_dbg_vs_scalar;
extern volatile unsigned long sgl_dbg_vs_cache_hit;
extern volatile unsigned long sgl_dbg_vs_cache_miss;
extern volatile unsigned long sgl_dbg_tex2d_rvv;
extern volatile unsigned long sgl_dbg_tex2d_scalar;

static unsigned long sgl_diag_count_nonzero_rgb(const sgl_framebuffer *fb)
{
    unsigned long nonzero = 0;
    if (fb && fb->color) {
        long n = (long)fb->w * fb->h;
        for (long i = 0; i < n; i++)
            if (fb->color[i] & 0x00FFFFFFu) nonzero++;
    }
    return nonzero;
}

static void sgl_diag_reset_frame_counters(void)
{
    sgl_dbg_tris = 0;
    sgl_dbg_tris_onscreen = 0;
    sgl_dbg_frag_zpass = 0;
    sgl_dbg_frag_cmask0 = 0;
    sgl_dbg_frag_shaded = 0;
    sgl_dbg_frag_shader_nonzero = 0;
    sgl_dbg_frag_store = 0;
    sgl_dbg_frag_store_nonzero = 0;
    sgl_dbg_frag_ow = 0;
    sgl_dbg_frag_ow_black = 0;
    sgl_dbg_shader_link_glsl = 0;
    sgl_dbg_shader_link_builtin = 0;
    sgl_dbg_shader_link_fail = 0;
    sgl_dbg_draw_glsl = 0;
    sgl_dbg_draw_builtin = 0;
    sgl_dbg_draw_simt = 0;
    sgl_dbg_draw_scalar = 0;
    sgl_dbg_draw_fbo = 0;
    sgl_dbg_draw_default = 0;
    sgl_dbg_vs_simt = 0;
    sgl_dbg_vs_scalar = 0;
    sgl_dbg_vs_cache_hit = 0;
    sgl_dbg_vs_cache_miss = 0;
    sgl_dbg_tile_refs = 0;
    sgl_dbg_tex2d_rvv = 0;
    sgl_dbg_tex2d_scalar = 0;
    sgl_dbg_draw_no = 0;
}

void sgl_diag_draw_filter_set(int enabled, int frame_first, int frame_last,
                              int draw_first, int draw_last, int detail)
{
    sgl_dbg_draw_filter_enabled = enabled ? 1 : 0;
    sgl_dbg_draw_frame_first = frame_first;
    sgl_dbg_draw_frame_last = frame_last;
    sgl_dbg_draw_first = draw_first;
    sgl_dbg_draw_last = draw_last;
    sgl_dbg_draw_detail = detail;
    if (enabled) {
        sgl_diag_enabled = 1;
    }
}

unsigned long sgl_diag_note_draw(void)
{
    if (!sgl_diag_enabled && !sgl_dbg_draw_filter_enabled) {
        return 0;
    }
    return ++sgl_dbg_draw_no;
}

int sgl_diag_draw_selected(unsigned long draw_no)
{
    if (!sgl_dbg_draw_filter_enabled) return 0;
    if (sgl_dbg_draw_frame_first >= 0 && (long)sgl_dbg_frame_no < sgl_dbg_draw_frame_first) return 0;
    if (sgl_dbg_draw_frame_last >= 0 && (long)sgl_dbg_frame_no > sgl_dbg_draw_frame_last) return 0;
    if (sgl_dbg_draw_first >= 0 && (long)draw_no < sgl_dbg_draw_first) return 0;
    if (sgl_dbg_draw_last >= 0 && (long)draw_no > sgl_dbg_draw_last) return 0;
    return 1;
}

int sgl_diag_present_report(const sgl_framebuffer *fb, int capture_armed)
{
    if (!sgl_diag_enabled || !fb) {
        return 0;
    }

    unsigned long nonzero = sgl_diag_count_nonzero_rgb(fb);
    int auto_dump = capture_armed && sgl_dbg_tris_onscreen > 12000;

    printf("[diag] gl present %dx%d  tris=%lu  onscreen=%lu  nonblack_px=%lu%s\n",
           fb->w, fb->h, sgl_dbg_tris, sgl_dbg_tris_onscreen, nonzero,
           capture_armed ? "  (ARMED: dumping)" : "");
    if (sgl_dbg_frag_zpass || sgl_dbg_frag_shaded || sgl_dbg_frag_cmask0) {
        printf("[diag] frag zpass=%lu cmask0=%lu shaded=%lu shader_nz=%lu store=%lu store_nz=%lu  ow=%lu ow_black=%lu\n",
               sgl_dbg_frag_zpass, sgl_dbg_frag_cmask0, sgl_dbg_frag_shaded,
               sgl_dbg_frag_shader_nonzero, sgl_dbg_frag_store, sgl_dbg_frag_store_nonzero,
               sgl_dbg_frag_ow, sgl_dbg_frag_ow_black);
    }
    if (sgl_dbg_shader_link_glsl || sgl_dbg_shader_link_builtin || sgl_dbg_shader_link_fail ||
        sgl_dbg_draw_glsl || sgl_dbg_draw_builtin) {
        printf("[diag] shader link_glsl=%lu link_builtin=%lu link_fail=%lu draw_glsl=%lu draw_builtin=%lu simt=%lu scalar=%lu  draw_fbo=%lu draw_default=%lu\n",
               sgl_dbg_shader_link_glsl, sgl_dbg_shader_link_builtin, sgl_dbg_shader_link_fail,
               sgl_dbg_draw_glsl, sgl_dbg_draw_builtin, sgl_dbg_draw_simt, sgl_dbg_draw_scalar,
               sgl_dbg_draw_fbo, sgl_dbg_draw_default);
    }
    if (sgl_dbg_vs_simt || sgl_dbg_vs_scalar || sgl_dbg_vs_cache_hit || sgl_dbg_vs_cache_miss ||
        sgl_dbg_tile_refs || sgl_dbg_tex2d_rvv || sgl_dbg_tex2d_scalar) {
        printf("[diag] pipe vs_simt=%lu vs_scalar=%lu vs_cache_hit=%lu vs_cache_miss=%lu tile_refs=%lu tex2d_rvv=%lu tex2d_scalar=%lu\n",
               sgl_dbg_vs_simt, sgl_dbg_vs_scalar, sgl_dbg_vs_cache_hit, sgl_dbg_vs_cache_miss,
               sgl_dbg_tile_refs, sgl_dbg_tex2d_rvv, sgl_dbg_tex2d_scalar);
    }
    if (auto_dump) {
        printf("[diag] auto-dumping first world frame (onscreen=%lu nonblack=%lu)\n",
               sgl_dbg_tris_onscreen, nonzero);
    }
    sgl_diag_reset_frame_counters();
    sgl_dbg_frame_no++;
    return auto_dump;
}

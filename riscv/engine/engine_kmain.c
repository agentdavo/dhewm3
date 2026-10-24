/*
 * engine_kmain.c — bare-metal entry adapter for the dhewm3 engine (Path B).
 *
 * The CRT chain is:  start.S:_start  ->  runtime.c:_cstart  (zeroes .bss/.tbss,
 * runs .init_array C++ static ctors)  ->  kmain()  [here]  ->  dhewm3_main().
 *
 * kmain() is the symbol riscv/boot/runtime.c calls. We initialise the rdtime
 * epoch (so Sys_MillisecondsPrecise starts > 0) and hand off to the engine's
 * dhewm3_main() (riscv/engine/sys_dos.cpp), which runs common->Init() +
 * common->Frame() forever. We pass an argv with a single program-name slot;
 * dhewm3_main strips argv[0] exactly like the desktop main() does.
 */

#include <stdio.h>
#include "boot/runtime.h"
#include "texture_profile.h"
#include <string.h>

static uint64_t tex_ticks[BM_TEX_PHASES], tex_calls[BM_TEX_PHASES], tex_bytes[BM_TEX_PHASES];
static struct { uint64_t ticks; char name[128]; } tex_slow[8];
uint64_t BM_TextureBegin(void) { return timer_ticks(); }
void BM_TextureReset(void) {
    memset(tex_ticks, 0, sizeof(tex_ticks));
    memset(tex_calls, 0, sizeof(tex_calls));
    memset(tex_bytes, 0, sizeof(tex_bytes));
    memset(tex_slow, 0, sizeof(tex_slow));
}
void BM_TextureEnd(int phase, uint64_t start, uint64_t bytes, const char *name) {
    uint64_t elapsed = timer_ticks()-start;
    if ((unsigned)phase >= BM_TEX_PHASES) return;
    tex_ticks[phase] += elapsed; ++tex_calls[phase]; tex_bytes[phase] += bytes;
    if (phase == BM_TEX_LOAD && name) {
        for (int i=0; i<8; ++i) if (elapsed > tex_slow[i].ticks) {
            for (int j=7; j>i; --j) tex_slow[j]=tex_slow[j-1];
            tex_slow[i].ticks=elapsed;
            snprintf(tex_slow[i].name, sizeof(tex_slow[i].name), "%s", name);
            break;
        }
    }
}
void BM_TextureReport(const char *label) {
    static const char *names[] = {"load-inclusive", "source-read-decode", "generate-inclusive",
        "mip-generation", "gl-upload-inclusive", "soft-upload-inclusive", "soft-copy",
        "tga-file-read-inflate", "tga-load-inclusive", "heightmap", "addnormals"};
    printf("[texture-time] phase=%s (nested inclusive timings; do not sum)\n", label);
    for (int i=0; i<BM_TEX_PHASES; ++i)
        printf("[texture-time] bucket=%s calls=%llu us=%llu bytes=%llu\n", names[i],
            (unsigned long long)tex_calls[i],
            (unsigned long long)(timer_ns_from_ticks(tex_ticks[i])/1000),
            (unsigned long long)tex_bytes[i]);
    for (int i=0; i<8; ++i) if (tex_slow[i].ticks)
        printf("[texture-slow] us=%llu name=%s\n",
            (unsigned long long)(timer_ns_from_ticks(tex_slow[i].ticks)/1000), tex_slow[i].name);
}

extern int  dhewm3_main(int argc, char **argv);
extern void BM_InitTime(void);

/* Real DTB timebase: never the deterministic game clock. The epoch starts at
 * C-runtime entry, after OpenSBI; firmware/host launch time is not included. */
static uint64_t bm_boot_epoch, bm_boot_previous;
static int bm_boot_first_present, bm_boot_first_map_present;
extern volatile int bm_map_spawn_seq;
void BM_BootBegin(uint64_t ticks) { bm_boot_epoch = bm_boot_previous = ticks; }
void BM_BootMark(const char *stage) {
    if (!strcmp(stage, "textures-upload-start")) BM_TextureReset();
    uint64_t now = timer_ticks();
    printf("[boot-time] total_ms=%llu delta_ms=%llu stage=%s\n",
        (unsigned long long)(timer_ns_from_ticks(now-bm_boot_epoch)/1000000),
        (unsigned long long)(timer_ns_from_ticks(now-bm_boot_previous)/1000000), stage);
    bm_boot_previous = now;
    if (!strcmp(stage, "textures-upload-ready")) BM_TextureReport(stage);
}
void BM_BootPresented(void) {
    if (!bm_boot_first_present) {
        bm_boot_first_present = 1;
        BM_BootMark("first-display-present");
    }
    if (bm_map_spawn_seq && !bm_boot_first_map_present) {
        bm_boot_first_map_present = 1;
        BM_BootMark("first-map-present");
        BM_TextureReport("through-first-map-present");
    }
}

#ifndef BM_DEFER_PRECACHE
#define BM_DEFER_PRECACHE 0
#endif
#ifndef BM_QUIET_ASSET_LOADS
#define BM_QUIET_ASSET_LOADS 0
#endif
#ifndef BM_IO_STATS
#define BM_IO_STATS 0
#endif
#ifndef BM_SIMD_BENCH
#define BM_SIMD_BENCH 0
#endif
#ifndef BM_RENDER_PROF
#define BM_RENDER_PROF 0
#endif
#define BM_STR2(x) #x
#define BM_STR(x) BM_STR2(x)
#ifndef SGL_SIMT_LANES
#define SGL_SIMT_LANES 64
#endif

/* Boot-time platform census.
 *
 * Every line here corresponds to something that previously failed SILENTLY:
 *  - missing SBI HSM degraded to single-hart with no diagnostic at all;
 *  - a DTB whose timebase could not be read left the profiler dividing by a
 *    compile-time guess, and since QEMU virt's timebase equals that guess, a
 *    broken parse was indistinguishable from a working one;
 *  - the virtio-mmio slot census: 7 of the 8 slots QEMU virt provides are in
 *    use, so adding one more device pushes some driver's scan_slots() off the
 *    end and it simply finds nothing.
 */
static void bm_platform_census(void)
{
    static const struct { long eid; const char *name; } exts[] = {
        { 0x48534DL,    "HSM"  },   /* hart start/stop -- SMP depends on it   */
        { 0x54494D45L,  "TIME" },   /* sbi_set_timer fallback when no Sstc    */
        { 0x735049L,    "sPI"  },   /* IPI -- the wfi wakeup path             */
        { 0x53525354L,  "SRST" },   /* clean power-off                        */
        { 0x4442434EL,  "DBCN" },   /* bulk console write                     */
        { 0x504D55L,    "PMU"  },   /* perf counters; Sscofpmf sampling needs it */
    };
    unsigned i;

    printf("[doomos-plat] timebase %lu Hz (%s)\n",
           timer_hz(), timer_hz_is_from_dtb() ? "from dtb" : "COMPILE-TIME FALLBACK");

    /* Physical RAM vs the linked window. Warns when the image is linked for a
     * bigger machine than it is running on -- see dos_heap_limit(). */
    { extern void bm_report_ram_window(void); bm_report_ram_window(); }

    /* Probed by execution, not read off -march: cbo.zero is emitted
     * unconditionally but traps as illegal in S-mode unless firmware set
     * menvcfg.CBZE, so a "NO" here is the difference between a working image and
     * a fatal trap inside memset on other firmware. */
    printf("[doomos-plat] isa probe: zicboz=%s zfa=%s zvbb=%s rdcycle=%s rdinstret=%s\n",
           boot_rv_isa.zicboz ? "yes" : "NO",
           boot_rv_isa.zfa    ? "yes" : "NO",
           boot_rv_isa.zvbb   ? "yes" : "NO",
           boot_rv_isa.cycle  ? "yes" : "NO",
           boot_rv_isa.instret? "yes" : "NO");

    printf("[doomos-plat] sbi ext:");
    for (i = 0; i < sizeof(exts) / sizeof(exts[0]); ++i)
        printf(" %s=%s", exts[i].name, sbi_probe_extension(exts[i].eid) ? "yes" : "NO");
    printf("\n");

    /* virtio-mmio slot occupancy. QEMU virt lays out 8 slots of 0x200 from
     * 0x10001000; a slot with DeviceID 0 is present-but-unused. */
    {
        unsigned used = 0, empty = 0;
        printf("[doomos-plat] virtio-mmio slots:");
        for (i = 0; i < 8; ++i) {
            uintptr_t base = 0x10001000UL + (uintptr_t)i * 0x1000UL;
            volatile uint32_t *r = (volatile uint32_t *)base;
            if (r[0] != 0x74726976UL) { printf(" -"); continue; }   /* no magic */
            if (r[2]) { printf(" %u:id%u", i, (unsigned)r[2]); ++used; }
            else      { printf(" %u:free", i); ++empty; }
        }
        printf("  (%u used, %u free)\n", used, empty);
    }
}

int kmain(void)
{
    BM_BootMark("engine-entry");
    const rv_platform_caps *caps = rv_get_platform_caps();
#ifdef __riscv_vector
    printf("[doomos-cpu] RVV VLEN=%lu bits; e32m1=%lu e32m2=%lu lanes; SoftGLES batch=%u\n",
           caps->vlenb * 8, caps->vlmax_e32m1, caps->vlmax_e32m2,
           (unsigned)SGL_SIMT_LANES);
    if (caps->vlmax_e32m2 != (unsigned long)SGL_SIMT_LANES)
        panic("RVV VLEN does not match the DoomOS/SoftGLES build");
#else
    /* Scalar build (-march without V): there is no vsetvl to interrogate, so
     * rv_platform_caps is all zeroes and the VLEN-vs-batch check below would
     * panic on every boot. SGL_SIMT_LANES is then just the SoA stride the
     * softgles2 reference paths loop over, not a hardware lane count. */
    (void)caps;
    printf("[doomos-cpu] no V extension (scalar build); SoftGLES batch=%u lanes\n",
           (unsigned)SGL_SIMT_LANES);
#endif
    bm_platform_census();
#ifndef BM_BOOT_TO_MENU
#define BM_BOOT_TO_MENU 0
#endif
	static char  arg0[] = "dhewm3";
	static char  a1[]="+set", a2[]="r_mode", a3[]="-1";
	/* Resolution ladder (CMake BAREMETAL_FB_WIDTH/HEIGHT, default 768x432):
	 * keep 16:9 and SIMT/MT alignment — width multiple of SGL_SIMT_LANES,
	 * height multiple of (SGL_SIMT_LANES/4). TCG profiles use 512x288 or 640x360. */
#ifndef BM_FB_WIDTH
#define BM_FB_WIDTH 768
#endif
#ifndef BM_FB_HEIGHT
#define BM_FB_HEIGHT 432
#endif
	static char  a4[]="+set", a5[]="r_customWidth", a6[]=BM_STR(BM_FB_WIDTH);
	static char  a7[]="+set", a8[]="r_customHeight", a9[]=BM_STR(BM_FB_HEIGHT);
	static char  a10[]="+set", a11[]="r_glprof", a12[]="0";
	/* +map is the FALLBACK (intro cinematic). When savefs has replayed the
	 * persisted dostest.save, sys_dos.cpp issues "loadGame dostest" at frame 2
	 * (the proven timing — a +loadGame startup command runs too early and fails),
	 * dropping us into live gameplay for a deterministic lit capture.
	 *
	 * BM_BOOT_TO_MENU=1 drops +map so the engine reaches the main loop and renders
	 * the DOOM 3 demo title/menu screen (no map-spawn wall) — the reliable headless
	 * capture target. Under TCG the demo_mars_city1 spawn never completes inside the
	 * run budget, so the +map path stays stuck in the load. Frame capture is host-
	 * side via QEMU `screendump` (riscv/scripts/capture_screendump.sh). */
#ifndef BM_BOOT_TO_MENU
#define BM_BOOT_TO_MENU 0
#endif
	static char  a13[]="+map", a14[]="game/demo_mars_city1";
	static char  a24[]="+set", a25[]="g_bmDeferPrecache", a26[]=BM_STR(BM_DEFER_PRECACHE);
	static char  a27[]="+set", a28[]="bm_quietAssetLoads", a29[]=BM_STR(BM_QUIET_ASSET_LOADS);
	static char  a30[]="+set", a31[]="bm_ioStats", a32[]=BM_STR(BM_IO_STATS);
	static char  a33[]="+set", a34[]="bm_simdBench", a35[]=BM_STR(BM_SIMD_BENCH);
	static char  a36[]="+set", a37[]="bm_renderProf", a38[]=BM_STR(BM_RENDER_PROF);
	/* Deterministic frame content. Without com_fixedTic the engine advances game
	 * time from the wall clock, so a build that renders faster shows DIFFERENT
	 * content at the same frame index -- which makes any frame-digest comparison
	 * between two builds report differences that are purely speed. Required for
	 * the [fbhash] gate to mean anything. */
#ifndef BM_FIXED_TIC
#define BM_FIXED_TIC 0
#endif
	static char  a91[]="+set", a92[]="com_fixedTic", a93[]=BM_STR(BM_FIXED_TIC);
	/* softgles2 on-screen stats/fps HUD. Off by default: it enables the render
	 * profiler to get its counters, and it draws over the frame -- neither is
	 * wanted in a capture or an A/B run. Toggleable at runtime too
	 * (`bm_sglOverlay 1` on the console) now that HID exists. */
#ifndef BM_SGL_OVERLAY
#define BM_SGL_OVERLAY 0
#endif
	static char  a94[]="+set", a95[]="bm_sglOverlay", a96[]=BM_STR(BM_SGL_OVERLAY);
	/* Headless "walk through Mars City" demo controls:
	 *  g_skipCinematic 1 : auto-fast-forward the mars_city1 intro cinematic (no ESC
	 *                      key exists on bare metal) so the player gains control and
	 *                      the lit world renders (game/Game_local.cpp SetCamera hook).
	 *  in_scriptedMove 1 : inject a deterministic scripted usercmd (walk fwd + slow
	 *                      yaw) in idUsercmdGenLocal::MakeCurrent(), since the SDL3
	 *                      backend produces no input events. Tunable at boot via
	 *                      in_scriptedFwd / in_scriptedYawPerTic / in_scriptedRun. */
	static char  a40[]="+set", a41[]="g_skipCinematic", a42[]="1";
	static char  a43[]="+set", a44[]="in_scriptedMove", a45[]="1";
	/* High-quality profile: stencil shadows ON (Doom 3's signature look; the
	 * ~5M-frags/frame cost is the price). Combine with BAREMETAL_MACHINE_SPEC=3
	 * (Ultra: full-res uncompressed textures) to stress the renderer for a TCG
	 * bottleneck study. For the cheap interactive/bench profile set this back to
	 * "0" (world stays fully lit: ambient+diffuse+bump+specular, far faster). A
	 * gentle continuous yaw (0.6 deg/tic) sweeps the view as the player walks. */
	static char  a46[]="+set", a47[]="r_shadows", a48[]="1";
	static char  a49[]="+set", a50[]="in_scriptedYawPerTic", a51[]="3.0";
	static char  a58[]="+set", a59[]="in_scriptedFwd", a60[]="0";
	/* Bare metal has no hardware gamma ramp (GLimp_SetGamma is a no-op). Use
	 * r_gammaInShader so dhewm3 injects brightness + pow into ARB fragment
	 * programs; gl4es/softgles2 carry program.env[21] through to execution.
	 * r_gamma/r_brightness were previously 2.0/1.3 to compensate for the
	 * (now-fixed) dark interaction lighting; that sqrt(1/2.0)=0.5 gamma stretched
	 * the 8-bit darks into visible banding. With lighting correct, use a gentle
	 * near-identity gamma (exponent 1/1.1) so darks are not over-stretched. */
	static char  a52[]="+set", a53[]="r_gamma", a54[]="1.1";
	static char  a55[]="+set", a56[]="r_brightness", a57[]="1.0";
	/* DIAGNOSTIC (temporary): force the interaction light term to unity so world surfaces
	 * shade as diffuse*color only — if texture detail then appears, the flat look is the
	 * light term (null light-proj/falloff samplers). Remove once the flat-shading is fixed. */
	static char  a61[]="+set", a62[]="bm_iakLightUnity", a63[]="0";
	/* The idSIMD_RVV-vs-generic differential harness (BM_RunSIMDDiffTest in
	 * sys_dos.cpp) stays available but off by default; enable with a boot
	 * "+set bm_simdTest 1" to run it after common->Init and power off. */
	/* Walk-to-NPC capture: bm_scenicWarp 0 releases the fixed setviewpos survey
	 * vantage so the scripted forward usercmd (in_scriptedFwd 100) actually walks
	 * the player through the level toward the animated NPCs. */
	static char  a64[]="+set", a65[]="bm_scenicWarp", a66[]="1";
	/* bm_npcCam is a capture aid; keep it off for the default playable/windowed
	 * boot so the command buffer is not spammed with setviewpos every frame. */
	static char  a67[]="+set", a68[]="bm_npcCam", a69[]="0";
	/* Keep the stock catch-up cap. Letting a slow TCG render trigger dozens of
	 * game tics just turns every displayed frame into a game-logic burst. */
	static char  a70[]="+set", a71[]="com_maxCatchupTics", a72[]="10";
	/* softgles2 filtered draw diagnostics. Disabled by default; enable with a
	 * narrow frame/draw range when inspecting a specific render issue. */
	static char  a73[]="+set", a74[]="bm_sglDbgDraws", a75[]="0";
	static char  a76[]="+set", a77[]="bm_sglDbgFrameFirst", a78[]="-1";
	static char  a79[]="+set", a80[]="bm_sglDbgFrameLast", a81[]="-1";
	static char  a82[]="+set", a83[]="bm_sglDbgDrawFirst", a84[]="-1";
	static char  a85[]="+set", a86[]="bm_sglDbgDrawLast", a87[]="-1";
	static char  a88[]="+set", a89[]="bm_sglDbgDetail", a90[]="0";
	/* Fog lights ENABLED (Doom 3's intended atmosphere). The two fog bugs are
	 * fixed: (1) the fog FS was misclassified onto the envcombine kernel (dropped
	 * tex alpha) -> now routes to dual_proj_mul; (2) the fog VS was bound to the
	 * overlay/mvp fast-path kernels, which read MultiTexCoord and ignore glTexGen,
	 * so the OBJECT_PLANE distance texcoord was discarded and fogImage sampled at a
	 * fixed opaque texel (uniform amber sheet). softgles2's classifier now keeps any
	 * texgen program on the VM VS (iak_uses_texgen), which evaluates the texgen dot,
	 * so fog renders as a proper near-clear->far-opaque gradient. Disable with
	 * +set r_skipFogLights 1 if a specific scene ever needs it. */
	static char  a97[]="+set", a98[]="r_skipFogLights", a99[]="0";
#if BM_BOOT_TO_MENU
	(void)a13; (void)a14;   /* +map omitted: boot straight to the demo title/menu */
	static char *argv[] = { arg0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a24, a25, a26, a27, a28, a29, a30, a31, a32, a33, a34, a35, a36, a37, a38, a40, a41, a42, a43, a44, a45, a46, a47, a48, a49, a50, a51, a52, a53, a54, a55, a56, a57, a58, a59, a60, a61, a62, a63, a64, a65, a66, a67, a68, a69, a70, a71, a72, a73, a74, a75, a76, a77, a78, a79, a80, a81, a82, a83, a84, a85, a86, a87, a88, a89, a90, a91, a92, a93, a94, a95, a96, a97, a98, a99, 0 };
	/* Derived, never hardcoded. It WAS hardcoded (79 here, 81 below) and went
	 * stale the moment the com_fixedTic triple was appended: the array held 82
	 * entries, argc said 79, and the three silently dropped were exactly
	 * "+set com_fixedTic 1". Every [fbhash] comparison since then ran with
	 * wall-clock game time and reported speed differences as pixel differences.
	 * -1 for the NULL terminator. */
	const int bm_argc = (int)(sizeof(argv) / sizeof(argv[0])) - 1;
#else
	static char *argv[] = { arg0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a24, a25, a26, a27, a28, a29, a30, a31, a32, a33, a34, a35, a36, a37, a38, a40, a41, a42, a43, a44, a45, a46, a47, a48, a49, a50, a51, a52, a53, a54, a55, a56, a57, a58, a59, a60, a61, a62, a63, a64, a65, a66, a67, a68, a69, a70, a71, a72, a73, a74, a75, a76, a77, a78, a79, a80, a81, a82, a83, a84, a85, a86, a87, a88, a89, a90, a91, a92, a93, a94, a95, a96, a97, a98, a99, 0 };
	const int bm_argc = (int)(sizeof(argv) / sizeof(argv[0])) - 1;   /* see above */
#endif
	/* To capture a rendered frame headlessly, build with BM_BOOT_TO_MENU=1 and run
	 * riscv/scripts/capture_screendump.sh: the engine scans each frame out to a
	 * virtio-gpu device and the host grabs it via the QEMU monitor `screendump`. */

	/* Unbuffered serial console: the engine runs an endless common->Frame()
	 * loop and is killed by the QEMU timeout (never a clean exit()), so a
	 * block-buffered stdout would silently drop every line. Unbuffered means
	 * each printf reaches SBI immediately — output streams, and anything up to
	 * a hang/fault is visible. */
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	BM_InitTime();

	/* TODO: parse a kernel command line / DTB bootargs into argv for cvars. */
	printf("bm argv:");
	for (int i = 1; i < bm_argc; ++i) {
		printf(" %s", argv[i]);
	}
	printf("\n");
	return dhewm3_main(bm_argc, argv);
}

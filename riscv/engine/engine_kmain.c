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

extern int  dhewm3_main(int argc, char **argv);
extern void BM_InitTime(void);

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

int kmain(void)
{
#ifndef BM_BOOT_TO_MENU
#define BM_BOOT_TO_MENU 0
#endif
	static char  arg0[] = "dhewm3";
	static char  a1[]="+set", a2[]="r_mode", a3[]="-1";
	/* 768x432: exact 16:9 and aligned to the softgles2 SIMT backend (width =
	 * 12*64 = e32m2 warp, no scanline tail; height = 27*16 = MT tile, no partial
	 * tile row). Frame capture is now host-side via QEMU `screendump`
	 * (capture_screendump.sh), so the old 320x240 serial-dump size constraint is
	 * gone — render at the real default resolution. Matches base/autoexec.cfg. */
	static char  a4[]="+set", a5[]="r_customWidth", a6[]="768";
	static char  a7[]="+set", a8[]="r_customHeight", a9[]="432";
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
	static char  a36[]="+set", a37[]="bm_renderProf", a38[]="0";
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
	/* Bare-metal capture/perf profile: under TCG the software rasteriser is CPU-bound
	 * (~5M frags/frame with stencil shadows), so drop shadows for the interactive
	 * walkthrough — the world stays fully lit (ambient+diffuse+bump+specular) and each
	 * frame renders far faster, which is what makes on-device motion visible. A gentle
	 * continuous yaw (0.6 deg/tic) sweeps the view as the player walks forward. */
	static char  a46[]="+set", a47[]="r_shadows", a48[]="0";
	static char  a49[]="+set", a50[]="in_scriptedYawPerTic", a51[]="3.0";
	static char  a58[]="+set", a59[]="in_scriptedFwd", a60[]="0";
	/* Bare metal has no hardware gamma ramp (GLimp_SetGamma is a no-op), so with the
	 * default r_gamma/r_brightness of 1 the software-rendered frame is much darker than
	 * on a monitor (which would apply its own ~2.2 gamma). r_gammaInShader (default 1)
	 * bakes gamma+brightness into the fragment program, which softgles2 DOES honor —
	 * so lift them here to bring the dim Mars City interiors up to a visible level. */
	static char  a52[]="+set", a53[]="r_gamma", a54[]="2.0";
	static char  a55[]="+set", a56[]="r_brightness", a57[]="1.3";
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
	static char  a64[]="+set", a65[]="bm_scenicWarp", a66[]="0";
	/* bm_npcCam 1: cycle the view through vantages framing the map's animated md5
	 * NPCs (see sys_dos.cpp) — the definitive skeletal-animation capture. */
	static char  a67[]="+set", a68[]="bm_npcCam", a69[]="1";
	/* Cinematic fast-forward: raise the per-frame game-tic catch-up cap (stock 10) so each
	 * slow TCG render advances ~0.5s of cinematic time, letting the long intro reach the
	 * Sarge character beat in a sane number of captured frames. */
	static char  a70[]="+set", a71[]="com_maxCatchupTics", a72[]="30";
	/* softgles2 filtered draw diagnostics. Leave disabled for normal captures; set
	 * bm_sglDbgDraws 1 and narrow frame/draw ranges before enabling detail 2. */
	static char  a73[]="+set", a74[]="bm_sglDbgDraws", a75[]="1";
	static char  a76[]="+set", a77[]="bm_sglDbgFrameFirst", a78[]="27";
	static char  a79[]="+set", a80[]="bm_sglDbgFrameLast", a81[]="28";
	static char  a82[]="+set", a83[]="bm_sglDbgDrawFirst", a84[]="1";
	static char  a85[]="+set", a86[]="bm_sglDbgDrawLast", a87[]="350";
	static char  a88[]="+set", a89[]="bm_sglDbgDetail", a90[]="2";
#if BM_BOOT_TO_MENU
	(void)a13; (void)a14;   /* +map omitted: boot straight to the demo title/menu */
	static char *argv[] = { arg0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a24, a25, a26, a27, a28, a29, a30, a31, a32, a33, a34, a35, a36, a37, a38, a40, a41, a42, a43, a44, a45, a46, a47, a48, a49, a50, a51, a52, a53, a54, a55, a56, a57, a58, a59, a60, a61, a62, a63, a64, a65, a66, a67, a68, a69, a70, a71, a72, a73, a74, a75, a76, a77, a78, a79, a80, a81, a82, a83, a84, a85, a86, a87, a88, a89, a90, 0 };
	const int bm_argc = 79;
#else
	static char *argv[] = { arg0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a24, a25, a26, a27, a28, a29, a30, a31, a32, a33, a34, a35, a36, a37, a38, a40, a41, a42, a43, a44, a45, a46, a47, a48, a49, a50, a51, a52, a53, a54, a55, a56, a57, a58, a59, a60, a61, a62, a63, a64, a65, a66, a67, a68, a69, a70, a71, a72, a73, a74, a75, a76, a77, a78, a79, a80, a81, a82, a83, a84, a85, a86, a87, a88, a89, a90, 0 };
	const int bm_argc = 81;
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

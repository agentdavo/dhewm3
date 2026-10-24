/*
 * sys_baremetal.cpp — bare-metal Sys_* + GLimp_* for dhewm3 (Phase 5 / doc 15).
 *
 * SKELETON: compile-oriented. Every signature matches neo/sys/sys_public.h and
 * neo/renderer/tr_local.h exactly (verified). Bodies are minimal impls or TODO
 * stubs. NOT a full engine build — this is the seam that turns the stage-1
 * runtime (riscv/boot, kmain) into a host that satisfies the engine's Sys_* /
 * GLimp_* link surface with NO OS, SDL, libdl, pthreads or newlib threads.
 *
 * In the real Phase-5 layout this single skeleton is split into the bm_*.cpp
 * files of doc 15 §2 (bm_time/bm_threads/bm_events/bm_cpu/bm_dll/bm_console/
 * bm_files/bm_net/bm_main + glimp_softgl + cxxrt/*). Kept as ONE file here so
 * the whole Sys_* contract is visible and reviewable in one place.
 *
 * ===========================================================================
 * Per-Sys_* bare-metal strategy (one line each) — the contract from doc 15
 * ===========================================================================
 *  Sys_Init                  : print heap + RVV banner, BM_InitTime already run.
 *  Sys_Shutdown              : no-op.
 *  Sys_Quit                  : sbi_shutdown() (runtime.h).
 *  Sys_Error                 : printf + panic() (no return).
 *  Sys_MillisecondsPrecise   : rdtime via timer_ticks(), epoch-biased >0.
 *  Sys_Milliseconds          : INLINE in header — DO NOT define here.
 *  Sys_Sleep                 : cooperative yield + busy-wait on rdtime (pause).
 *  Sys_SleepUntilPrecise     : busy-wait on rdtime to ~0.01ms.
 *  Sys_Printf / VPrintf      : vprintf -> SBI console (libc).
 *  Sys_DebugPrintf / VPrintf : same (SBI write is atomic-ish, single hart).
 *  Sys_ConsoleInput          : NULL for bring-up (SBI getchar later).
 *  Sys_GetProcessorId        : CPUID_GENERIC (generic idSIMD path).
 *  Sys_FPU_SetFTZ/DAZ/Prec   : no-op (RV64D has no FTZ/DAZ bits).
 *  Sys_GetSystemRam          : heap window size in MB (DTB later).
 *  Sys_GetDriveFreeSpace     : large constant.
 *  Sys_Lock/UnlockMemory     : true (no paging).
 *  Sys_SetPhysicalWorkMemory : no-op.
 *  Sys_DLL_*                 : stubs (HARDLINK_GAME => never functionally used).
 *  Sys_GenerateEvents        : pump input source -> ring (stub/demo for now).
 *  Sys_GetEvent              : pop ring; SE_NONE when empty.
 *  Sys_ClearEvents           : reset ring.
 *  Sys_*Input* (kbd/mouse/joy): poll stubs (virtio-input in Phase 6).
 *  Sys_GetConsoleKey/MapChar : minimal keymap (identity for bring-up).
 *  Sys_*ScancodeName*        : "SC_?" stubs.
 *  Sys_GrabMouseCursor       : no-op (no window manager).
 *  Sys_CreateThread          : register cooperative; async tic folded to main.
 *  Sys_DestroyThread         : clear handle.
 *  Sys_Enter/LeaveCritical   : no-op (single cooperative hart).
 *  Sys_Trigger/WaitForEvent  : sticky latch (single hart, producer already ran).
 *  Sys_Init/ShutdownThreads  : zero the latch array.
 *  Sys_*Net* / idPort/idTCP  : no-ops, no TCP/IP stack.
 *  Sys_GetPath               : synthetic /base prefix; CONFIG/SAVE fail.
 *  Sys_ListFiles/Mkdir/...   : stubs now; FS layer (Phase 6) replaces them.
 *  GLimp_Init                : softgles2 surface @ 768x432 + sgl_bridge_init.
 *  GLimp_SwapBuffers         : present softgles2 framebuffer (PPM/RVFB later).
 *  GLimp_ExtensionPointer    : in qgl_gl4es.cpp (separate TU).
 * ===========================================================================
 */

// --- engine headers (exact contract) ----------------------------------------
#include "sys/platform.h"
#include "framework/Common.h"
#include "framework/KeyInput.h"
#include "sys/sys_public.h"
#include "renderer/tr_local.h"     // glimpParms_t, GLimp_*, glConfig

// --- our bare-metal runtime + softgles2 bridge -------------------------------
extern "C" {
#include "boot/runtime.h"          // timer_ticks, TIMEBASE_HZ, panic, sbi_shutdown
#include "boot/bm_pause.h"
#include "libc/libc.h"             // vprintf, malloc, memset, strcmp (riscv/libc)

// softgles2 framebuffer + the gl4es bridge (riscv/gl4es-bridge/bridge.h).
// sgl_bridge_init(fb) makes fb current, points gl4es's GLES2 resolver at
// softgles2 (sgl2_*), and runs initialize_gl4es(). After it returns the engine
// can call gl* (= gl4es) and the qgl loader (qgl_gl4es.cpp) resolves them.
struct sgl_framebuffer;
struct sgl_framebuffer *sgl_fb_create(int w, int h);       // softgles2 API (TODO: confirm signature)
void                    sgl_fb_present(struct sgl_framebuffer *fb); // TODO: confirm present entry
void                    sgl_bridge_init(struct sgl_framebuffer *fb);
}

// Default bare-metal resolution. 768x432 = exact 16:9, aligned to the softgles2
// SIMT backend: width = 12*64 (e32m2 warp = 64 px, no scanline tail), height =
// 27*16 (MT tile = 64x16, no partial tile row). Overridden by r_mode -1 +
// r_customWidth/r_customHeight in base/autoexec.cfg; this is the fallback.
#define BM_DEFAULT_WIDTH  768
#define BM_DEFAULT_HEIGHT 432

// from the linker script (riscv/engine/engine.ld; boot/linker.ld provides these
// in the stage-1 build).
extern "C" char __heap_start[];
extern "C" char __heap_end[];

// RISC-V Zihintpause hint (mirrors posix_main.cpp:436-438 D3_CpuPause for rv64).
static ID_INLINE void BM_CpuPause(void) {
	bm_pause();
}

// =====================================================================
// 3. Timing — rdtime
// =====================================================================
static uint64_t bm_timebase_hz = TIMEBASE_HZ;   // compile-time fallback only
static uint64_t bm_epoch_ticks = 0;

extern "C" void BM_InitTime(void) {
	// Take the DTB-discovered timebase. Left at the compile-time TIMEBASE_HZ,
	// every Doom 3 frame time and every Sys_MillisecondsPrecise() would be wrong
	// by the ratio of the real timebase on any part not clocking mtime at 10 MHz.
	bm_timebase_hz = timer_hz();
	bm_epoch_ticks = timer_ticks();
	// bias epoch back ~1.5ms so the first Sys_MillisecondsPrecise() > 0
	// (matches POSIX behaviour, posix_main.cpp:481-491).
	uint64_t bias = (bm_timebase_hz / 1000000ULL) * 1500ULL;
	bm_epoch_ticks = (bias < bm_epoch_ticks) ? (bm_epoch_ticks - bias) : 0;
}

double Sys_MillisecondsPrecise(void) {
	uint64_t now = timer_ticks() - bm_epoch_ticks;
	return (double)now * (1000.0 / (double)bm_timebase_hz);
}
// NOTE: Sys_Milliseconds() is ID_INLINE in sys_public.h:170 — do NOT define it.

// forward decl: cooperative pump (§6)
extern "C" void BM_CoopYield(void);

void Sys_Sleep(int msec) {
	if (msec <= 0) { BM_CpuPause(); return; }
	BM_CoopYield();                       // a sleep is a good time to run the tic
	// Park rather than spin. bm_wait_until() arms the S-mode timer (Sstc's
	// stimecmp where the DTB advertises it, else SBI TIME) and then `wfi`, which
	// under QEMU MTTCG genuinely hands the host core back -- BM_CpuPause() is a
	// nop there and gave it back to nobody. Arming before parking is what makes
	// the wake guaranteed; a bare `wfi` with sie.STIE clear halts forever.
	bm_wait_until(timer_ticks() + timer_ticks_from_us((uint64_t)msec * 1000ull));
}

void Sys_SleepUntilPrecise(double targetTimeMS) {
	while (targetTimeMS - Sys_MillisecondsPrecise() >= 0.01)
		BM_CpuPause();
}

// =====================================================================
// 5. Console / printing
// =====================================================================
void Sys_VPrintf(const char *msg, va_list arg) { vprintf(msg, arg); }
void Sys_Printf(const char *msg, ...) {
	va_list a; va_start(a, msg); vprintf(msg, a); va_end(a);
}
void Sys_DebugPrintf(const char *fmt, ...) {
	va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a);
}
void Sys_DebugVPrintf(const char *fmt, va_list arg) { vprintf(fmt, arg); }

char *Sys_ConsoleInput(void) {
	return NULL; // TODO: SBI legacy getchar (EID 0x02) + idEditField line loop.
}
void Sys_ShowConsole(int visLevel, bool quitOnClose) { (void)visLevel; (void)quitOnClose; }
void Sys_ShowWindow(bool show) { (void)show; }
bool Sys_IsWindowVisible(void) { return true; }

// clipboard — optional everywhere
char *Sys_GetClipboardData(void) { return NULL; }
void  Sys_FreeClipboardData(char *data) { (void)data; }
void  Sys_SetClipboardData(const char *string) { (void)string; }

// =====================================================================
// 12. CPU / FPU
// =====================================================================
int  Sys_GetProcessorId(void) { return CPUID_GENERIC; } // generic idSIMD path
void Sys_FPU_SetPrecision(void) {}                       // rv64d always 64-bit
void Sys_FPU_SetFTZ(bool enable) { (void)enable; }       // no FTZ bit on RV base
void Sys_FPU_SetDAZ(bool enable) { (void)enable; }       // no DAZ bit on RV base

// =====================================================================
// 4. Memory
// =====================================================================
int  Sys_GetSystemRam(void) {
	// heap window size in MB (DTB-discovered RAM later, doc 15 §4).
	return (int)(((uintptr_t)__heap_end - (uintptr_t)__heap_start) >> 20);
}
int  Sys_GetDriveFreeSpace(const char *path) { (void)path; return 1024 * 1024; } // 1 TB-ish
bool Sys_LockMemory(void *ptr, int bytes) { (void)ptr; (void)bytes; return true; }
bool Sys_UnlockMemory(void *ptr, int bytes) { (void)ptr; (void)bytes; return true; }
void Sys_SetPhysicalWorkMemory(int minBytes, int maxBytes) { (void)minBytes; (void)maxBytes; }

// =====================================================================
// 8. DLL — HARDLINK_GAME => stubs (symbols must link; never functionally used)
// =====================================================================
uintptr_t Sys_DLL_Load(const char *dllName) { (void)dllName; return 0; }
void     *Sys_DLL_GetProcAddress(uintptr_t h, const char *p) { (void)h; (void)p; return NULL; }
void      Sys_DLL_Unload(uintptr_t h) { (void)h; }

// =====================================================================
// 6. Threads — cooperative, single hart
// =====================================================================
// dummy SDL_Thread so the xthreadInfo::threadHandle member (forward-declared
// `struct SDL_Thread;` in sys_public.h:379) has a type, without pulling SDL.
struct SDL_Thread;   // never defined

static bool       g_triggered[MAX_TRIGGER_EVENTS];

// async-tic registration (the only thread the engine actually creates;
// Common.cpp:3179 Sys_CreateThread(AsyncThread,...)). On bare metal we DON'T
// run AsyncThread's blocking while(runAsyncThread) loop; instead the main loop
// pumps the registered callback at 60Hz via BM_CoopYield (doc 15 §6.4).
typedef int (*bm_coopfn_t)(void *);
static bm_coopfn_t g_asyncFn   = 0;
static void       *g_asyncParm = 0;
static double      g_nextTicMs = 0.0;

extern "C" void BM_CoopYield(void) {
	if (!g_asyncFn) return;
	double now = Sys_MillisecondsPrecise();
	if (now >= g_nextTicMs) {
		g_asyncFn(g_asyncParm);              // = idCommonLocal::AsyncThread body once,
		                                     //   or common->Async() directly (see §6.4)
		g_nextTicMs = now + (1000.0 / 60.0); // 60 Hz async tic
	}
}

void Sys_InitThreads(void)     { memset(g_triggered, 0, sizeof(g_triggered)); g_nextTicMs = 0.0; }
void Sys_ShutdownThreads(void) {}

void Sys_CreateThread(xthread_t function, void *parms, xthreadInfo &info, const char *name) {
	// Only AsyncThread reaches here (networking download thread is never spawned,
	// see Sys_*Net* stubs). Register it as the cooperative tic rather than
	// running its blocking loop.
	// TODO: with the one permitted engine edit (Common.cpp:3178 BAREMETAL guard)
	//       this won't even be called for AsyncThread; until then, registering
	//       the function body would loop forever, so we register a tic instead.
	info.name         = name;
	info.threadHandle = (SDL_Thread *)1;     // non-NULL sentinel so Shutdown() runs
	info.threadId     = 1;
	g_asyncFn   = (bm_coopfn_t)function;     // see BM_RegisterAsyncTic note below
	g_asyncParm = parms;
}
void Sys_DestroyThread(xthreadInfo &info) {
	g_asyncFn = 0; g_asyncParm = 0;
	info.name = NULL; info.threadHandle = NULL; info.threadId = 0;
}
const char *Sys_GetThreadName(int *index) { if (index) *index = -1; return "main"; }
bool        Sys_IsMainThread(void)        { return true; }

void Sys_EnterCriticalSection(int index) { (void)index; } // single hart, no contention
void Sys_LeaveCriticalSection(int index) { (void)index; }

void Sys_TriggerEvent(int index) { g_triggered[index] = true; }   // latch
void Sys_WaitForEvent(int index) {
	// single hart: the producer has already run, so the latch is set => no spin.
	while (!g_triggered[index]) BM_CoopYield();
	g_triggered[index] = false;
}

// =====================================================================
// 7. Events / input
// =====================================================================
#define BM_EVENT_QUEUE 256   // power of two
static sysEvent_t g_evq[BM_EVENT_QUEUE];
static unsigned   g_evHead = 0, g_evTail = 0;

static void BM_PushEvent(sysEventType_t t, int v, int v2, void *p, int plen) {
	sysEvent_t *e = &g_evq[g_evHead & (BM_EVENT_QUEUE - 1)];
	e->evType = t; e->evValue = v; e->evValue2 = v2; e->evPtr = p; e->evPtrLength = plen;
	g_evHead++;
}

void Sys_GenerateEvents(void) {
	// TODO: pump the input source -> BM_PushEvent. Sources (doc 15 §7.2):
	//   1) virtio-input HID (Phase 6)  2) scripted/demo stream (headless harness)
	//   3) SBI console as a crude dev-console keyboard.
}
sysEvent_t Sys_GetEvent(void) {
	if (g_evTail == g_evHead) {
		sysEvent_t none; none.evType = SE_NONE; none.evValue = 0; none.evValue2 = 0;
		none.evPtrLength = 0; none.evPtr = NULL; return none;
	}
	return g_evq[g_evTail++ & (BM_EVENT_QUEUE - 1)];
}
void Sys_ClearEvents(void) { g_evHead = g_evTail = 0; }

void Sys_InitInput(void)     {}
void Sys_ShutdownInput(void) {}
void Sys_InitScanTable(void) {}   // already a no-op off-Win32 (events.cpp)

unsigned char Sys_GetConsoleKey(bool shifted) { (void)shifted; return '`'; }
unsigned char Sys_MapCharForKey(int key)      { return (unsigned char)(key & 0xff); }

const char *Sys_GetScancodeName(int key)                 { (void)key; return "SC_?"; }
const char *Sys_GetLocalizedScancodeName(int key)        { (void)key; return "SC_?"; }
const char *Sys_GetLocalizedScancodeNameUTF8(int key)    { (void)key; return "SC_?"; }
int         Sys_GetKeynumForScancodeName(const char *nm) { (void)nm; return 0; }
const char *Sys_GetLocalizedJoyKeyName(int key)          { (void)key; return "JOY_?"; }

int  Sys_PollKeyboardInputEvents(void) { return 0; }
int  Sys_ReturnKeyboardInputEvent(const int n, int &ch, bool &state) { (void)n; ch = 0; state = false; return 0; }
void Sys_EndKeyboardInputEvents(void) {}

int  Sys_PollMouseInputEvents(void) { return 0; }
int  Sys_ReturnMouseInputEvent(const int n, int &action, int &value) { (void)n; action = 0; value = 0; return 0; }
void Sys_EndMouseInputEvents(void) {}

void Sys_SetRumble(int device, int low, int hi) { (void)device; (void)low; (void)hi; }
int  Sys_PollJoystickInputEvents(int deviceNum) { (void)deviceNum; return 0; }
int  Sys_ReturnJoystickInputEvent(const int n, int &action, int &value) { (void)n; action = 0; value = 0; return 0; }
void Sys_EndJoystickInputEvents(void) {}

void Sys_GrabMouseCursor(bool grabIt) { (void)grabIt; }
void Sys_SetInteractiveIngameGuiActive(bool active, idUserInterface *ui) { (void)active; (void)ui; }

// =====================================================================
// 9. Networking — stub (no TCP/IP stack)
// =====================================================================
idPort::idPort()  {}
idPort::~idPort() {}
bool idPort::InitForPort(int portNumber) { (void)portNumber; return false; }
bool idPort::GetPacket(netadr_t &from, void *data, int &size, int maxSize) {
	(void)from; (void)data; (void)maxSize; size = 0; return false;
}
bool idPort::GetPacketBlocking(netadr_t &from, void *data, int &size, int maxSize, int timeout) {
	(void)from; (void)data; (void)maxSize; (void)timeout; size = 0; return false;
}
void idPort::SendPacket(const netadr_t to, const void *data, int size) { (void)to; (void)data; (void)size; }
void idPort::Close() {}

idTCP::idTCP()  {}
idTCP::~idTCP() {}
bool idTCP::Init(const char *host, short port) { (void)host; (void)port; return false; }
void idTCP::Close() {}
int  idTCP::Read(void *data, int size)  { (void)data; (void)size; return -1; }
int  idTCP::Write(void *data, int size) { (void)data; (void)size; return -1; }

void Sys_InitNetworking(void)     {}
void Sys_ShutdownNetworking(void) {}
bool Sys_StringToNetAdr(const char *s, netadr_t *a, bool doDNSResolve) { (void)s; (void)a; (void)doDNSResolve; return false; }
const char *Sys_NetAdrToString(const netadr_t a) { (void)a; return "0.0.0.0"; }
bool Sys_IsLANAddress(const netadr_t a) { (void)a; return false; }
bool Sys_CompareNetAdrBase(const netadr_t a, const netadr_t b) { (void)a; (void)b; return false; }

// =====================================================================
// 10. Files / paths — stubs now; FS layer (Phase 6, doc 16) replaces these
// =====================================================================
bool Sys_GetPath(sysPath_t type, idStr &path) {
	switch (type) {
		case PATH_BASE: path = "/base"; return true;   // synthetic FS prefix (doc 16)
		case PATH_EXE:  path = "/dhewm3"; return true;
		case PATH_CONFIG:
		case PATH_SAVE:
		default:        return false;                  // engine tolerates missing
	}
}
int Sys_ListFiles(const char *directory, const char *extension, idList<class idStr> &list) {
	(void)directory; (void)extension; (void)list;
	return -1;   // -1 = directory not found; FS layer (doc 16 §4.2) replaces this
}
void      Sys_Mkdir(const char *path) { (void)path; }
ID_TIME_T Sys_FileTimeStamp(FILE *fp) { (void)fp; return 0; }

// =====================================================================
// 10/2. main bridge + Sys lifecycle
// =====================================================================
extern "C" unsigned long BM_GetVlenb(void); // TODO: read vlenb CSR (riscv/boot)

void Sys_Init(void) {
	common->Printf("dhewm3 bare-metal: %d MB heap\n", Sys_GetSystemRam());
}
void Sys_Shutdown(void) {}
void Sys_Quit(void) { sbi_shutdown(); }

void Sys_Error(const char *error, ...) {
	va_list a; va_start(a, error);
	Sys_Printf("Sys_Error: ");
	Sys_DebugVPrintf(error, a);
	Sys_Printf("\n");
	va_end(a);
	panic("Sys_Error");   // runtime.c -> sbi_shutdown (noreturn)
}

/*
 * main() — replaces sys/linux/main.cpp. Renamed dhewm3_main under BAREMETAL so
 * the freestanding CRT keeps a single real entry; riscv/engine/engine_kmain.c's
 * kmain() calls it after BM_InitTime() (doc 15 §10.1).
 */
#ifdef BAREMETAL
extern "C" int dhewm3_main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
	Sys_InitThreads();
	common->Init((argc > 1) ? argc - 1 : 0, (argc > 1) ? &argv[1] : NULL);
	while (1) {
		common->Frame();   // Common.cpp:2492 — events + game + render + swap
		BM_CoopYield();    // drive the cooperative 60Hz async tic
	}
	return 0;
}

// =====================================================================
// 11. GLimp_* over softgles2 / gl4es  (replaces sys/glimp.cpp)
//     GLimp_ExtensionPointer lives in qgl_gl4es.cpp (separate TU).
// =====================================================================
static struct sgl_framebuffer *g_glimpFB = 0;
static glimpParms_t            g_curParms;
static int                     g_swapInterval = 0;

bool GLimp_Init(glimpParms_t parms) {
	common->Printf("Initializing bare-metal OpenGL (softgles2 + gl4es)\n");

	int w = parms.width  ? parms.width  : BM_DEFAULT_WIDTH;
	int h = parms.height ? parms.height : BM_DEFAULT_HEIGHT;

	// 1) softgles2 default framebuffer (RGBA8 + depth/stencil), malloc'd from
	//    our heap. TODO: confirm sgl_fb_create signature/format flags vs the
	//    softgles2 API actually shipped (riscv/softgles2 / gl4es-bridge).
	g_glimpFB = sgl_fb_create(w, h);
	if (!g_glimpFB) {
		common->Warning("GLimp_Init: sgl_fb_create(%d,%d) failed", w, h);
		return false;
	}

	// 2) make it current + init gl4es against it. After this, gl* (=gl4es) is
	//    live, so the qgl loader that runs right after GLimp_Init in
	//    RenderSystem_init.cpp:822 (via GLimp_ExtensionPointer -> qgl_gl4es.cpp)
	//    finds real function pointers.
	sgl_bridge_init(g_glimpFB);

	// 3) populate glConfig (VERIFIED field names, RenderSystem.h:47-118).
	//    NOTE: glConfig.displayFrequency (int) is a glConfig field, NOT a
	//    glimpParms_t field (doc 00 §C.1). winWidth/winHeight are FLOAT.
	glConfig.vidWidth  = w;
	glConfig.vidHeight = h;
	glConfig.winWidth  = (float)w;
	glConfig.winHeight = (float)h;
	glConfig.colorBits   = 24;
	glConfig.alphabits   = 8;
	glConfig.depthBits   = 24;
	glConfig.stencilBits = 8;
	glConfig.displayFrequency    = 60;
	glConfig.isFullscreen        = true;
	glConfig.shouldFillWindowAlpha = false;
	glConfig.isWayland           = false;
	glConfig.haveDebugContext    = false;

	g_curParms = parms;
	g_curParms.width = w; g_curParms.height = h;
	return true;
}

bool GLimp_SetScreenParms(glimpParms_t parms) {
	// Re-create the surface only if the size changed; else nothing to do.
	if (parms.width == g_curParms.width && parms.height == g_curParms.height)
		return true;
	return GLimp_Init(parms);   // TODO: tear down old FB first (heap reuse)
}

void GLimp_Shutdown(void) {
	common->Printf("Shutting down bare-metal OpenGL\n");
	// TODO: sgl_fb_destroy(g_glimpFB) once the softgles2 API exposes it.
	g_glimpFB = 0;
}

void GLimp_SwapBuffers(void) {
	// TODO: flush any gl4es pending work, then present the softgles2 FB.
	//       Headless: emit PPM/RVFB over SBI (doc 17 §6.2). Later: ramfb/
	//       virtio-gpu blit (RVV blit kernel, not scalar copy).
	if (g_glimpFB)
		sgl_fb_present(g_glimpFB);
}

void GLimp_SetGamma(unsigned short red[256], unsigned short green[256], unsigned short blue[256]) {
	(void)red; (void)green; (void)blue;
	// no-op: prefer r_gammaInShader 1 (gamma baked into ARB fp, draw_arb2.cpp).
}
void GLimp_ResetGamma(void) {}

void GLimp_ActivateContext(void)   {}   // single SW context, always current
void GLimp_DeactivateContext(void) {}

void GLimp_GrabInput(int flags) { (void)flags; } // no cursor / window manager

bool GLimp_SetSwapInterval(int swapInterval) { g_swapInterval = swapInterval; return true; }
int  GLimp_GetSwapInterval(void)             { return g_swapInterval; }

bool GLimp_SetWindowResizable(bool enableResizable) { (void)enableResizable; return false; }
void GLimp_UpdateWindowSize(void) {}     // fixed FB; vid/win already set in Init

glimpParms_t GLimp_GetCurState(void) { return g_curParms; }
float        GLimp_GetDisplayRefresh(void) { return 60.0f; } // feeds 60Hz path (Common.cpp)

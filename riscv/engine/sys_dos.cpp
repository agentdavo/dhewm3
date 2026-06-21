/*
 * sys_dos.cpp — bare-metal Sys_* layer for dhewm3, PATH B (SDL3-on-dOS).
 *
 * Replaces sys/posix/posix_main.cpp + sys/posix/posix_net.cpp + sys/linux/main.cpp
 * for the bare-metal RISC-V "dOS" target. It provides ONLY the Sys_* surface that
 * those three POSIX files provide and that is NOT already supplied by the stock,
 * SDL3-based, platform-portable engine files we KEEP under Path B:
 *
 *     sys/cpu.cpp        -> Sys_FPU_*, Sys_GetProcessorId       (kept, SDL)
 *     sys/threads.cpp    -> Sys_CreateThread/Sleep/mutex/...    (kept, SDL)
 *     sys/events.cpp     -> Sys_GetEvent/input/scancodes/...    (kept, SDL)
 *     sys/glimp.cpp      -> GLimp_* (SDL_GL_* -> dos driver)     (kept, SDL)
 *     sys/sys_local.cpp  -> idSysLocal forwarders, Sys_TimeStampToStr (kept)
 *
 * So this file deliberately does NOT define any GLimp_*, threads, input, cpu, or
 * idSysLocal symbol — those would clash with the kept SDL files.
 *
 * The OS bottom-end (newlib syscalls over SBI, getenv/getcwd/select/gettimeofday)
 * lives in riscv/engine/dos_syscalls.c (a copy of riscv/dos/dos_syscalls.c, kept
 * separate so the engine link is self-contained and not coupled to the dos test
 * payload). The CRT (_start/_cstart) is riscv/boot; kmain() is in engine_kmain.c.
 *
 * Signatures verified against neo/sys/sys_public.h.
 */

#include "sys/platform.h"
#include "framework/Common.h"
#include "framework/CVarSystem.h"   // idCVar (bm_ioStats, bm_renderProf, ...)
#include "framework/CmdSystem.h"    // cmdSystem (savetest: drive saveGame/loadGame)
#include "framework/Session.h"      // session->GetCurrentMapName() (savetest)
#include "idlib/math/Simd.h"
#include "idlib/math/Simd_Generic.h"
#include "idlib/geometry/DrawVert.h"      // bm_simdTest: idDrawVert
#include "idlib/geometry/JointTransform.h"// bm_simdTest: idJointMat / idJointQuat
#include "idlib/math/Plane.h"             // bm_simdTest: idPlane
#include "renderer/Model.h"               // bm_simdTest: dominantTri_s / glIndex_t
#include "sys/sys_public.h"
#include "sys/sys_local.h"   // idSysLocal::OpenURL / StartProcess (else from posix_main/linux main)
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

extern "C" {
#include "boot/runtime.h"   /* timer_ticks, TIMEBASE_HZ, panic, sbi_shutdown */
#include "engine/dos_virtio_blk.h"  /* vblk_init / vblk_read / vblk_write     */
#include "engine/dosfs.h"           /* dosfs_init / dosfs_list (doc 12)       */
#include "engine/dos_ramfs.h"       /* writable save/config RAM overlay       */
#include "engine/savefs.h"          /* persistent journal (doc 13)            */
}
#include <stdlib.h>

extern "C" char __heap_start[];
extern "C" char __heap_end[];

// ===========================================================================
// Timing — rdtime CSR (10 MHz on QEMU virt). Backs Sys_MillisecondsPrecise,
// from which Sys_Milliseconds (inline in sys_public.h) is derived.
// ===========================================================================
static uint64_t bm_epoch_ticks = 0;

extern "C" void BM_InitTime(void) {
	bm_epoch_ticks = timer_ticks();
	// bias epoch back ~1.5 ms so the first Sys_MillisecondsPrecise() > 0
	// (matches posix_main.cpp Posix_InitTime behaviour).
	uint64_t bias = (TIMEBASE_HZ / 1000000ULL) * 1500ULL;
	bm_epoch_ticks = (bias < bm_epoch_ticks) ? (bm_epoch_ticks - bias) : 0;
}

double Sys_MillisecondsPrecise(void) {
	uint64_t now = timer_ticks() - bm_epoch_ticks;
	return (double)now * (1000.0 / (double)TIMEBASE_HZ);
}

// RISC-V Zihintpause hint (mirrors posix_main.cpp D3_CpuPause rv64 branch).
static ID_INLINE void BM_CpuPause(void) {
	__asm__ __volatile__(".insn i 0x0F, 0, x0, x0, 0x010"); // pause
}

void Sys_SleepUntilPrecise(double targetTimeMS) {
	while (targetTimeMS - Sys_MillisecondsPrecise() >= 0.01)
		BM_CpuPause();
	// TODO: sbi_set_timer + recoverable timer trap to wfi instead of busy-wait.
}

// ===========================================================================
// Console / printing  (vprintf -> newlib -> _write -> SBI DBCN)
// ===========================================================================
void Sys_VPrintf(const char *msg, va_list arg) { vprintf(msg, arg); }
void Sys_Printf(const char *msg, ...) {
	va_list a; va_start(a, msg); vprintf(msg, a); va_end(a);
}
void Sys_DebugPrintf(const char *fmt, ...) {
	va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a);
}
void Sys_DebugVPrintf(const char *fmt, va_list arg) { vprintf(fmt, arg); }

void Sys_ShowConsole(int visLevel, bool quitOnClose) { (void)visLevel; (void)quitOnClose; }

// Clipboard — posix_main routes these through SDL; on bare metal there is no
// clipboard, so stub them (engine tolerates NULL).
char *Sys_GetClipboardData(void)             { return NULL; }
void  Sys_FreeClipboardData(char *data)      { (void)data; }
void  Sys_SetClipboardData(const char *str)  { (void)str; }

// ===========================================================================
// Memory  (no paging; heap is the linker window backing malloc/_sbrk)
// ===========================================================================
int  Sys_GetSystemRam(void) {
	return (int)(((uintptr_t)__heap_end - (uintptr_t)__heap_start) >> 20); // MB
}
int  Sys_GetDriveFreeSpace(const char *path) {
	(void)path;
	// When the savefs journal is mounted, report its remaining write-region in MB
	// so SaveGame's >=25 MB gate refuses gracefully as the region fills. Otherwise
	// report "plenty" (the volatile RAM overlay).
	if (savefs_active()) return (int)(savefs_free_bytes() >> 20);
	return 1024 * 1024;
}
bool Sys_LockMemory(void *ptr, int bytes)    { (void)ptr; (void)bytes; return true; }
bool Sys_UnlockMemory(void *ptr, int bytes)  { (void)ptr; (void)bytes; return true; }
void Sys_SetPhysicalWorkMemory(int minBytes, int maxBytes) { (void)minBytes; (void)maxBytes; }

// ===========================================================================
// DLL — HARDLINK_GAME => never functionally used; symbols just need to link.
// ===========================================================================
uintptr_t Sys_DLL_Load(const char *dllName) { (void)dllName; return 0; }
void     *Sys_DLL_GetProcAddress(uintptr_t h, const char *p) { (void)h; (void)p; return NULL; }
void      Sys_DLL_Unload(uintptr_t h) { (void)h; }

// ===========================================================================
// Files / paths — stubs now; a synthetic FS layer (later phase) replaces these.
// ===========================================================================
bool Sys_GetPath(sysPath_t type, idStr &path) {
	switch (type) {
		case PATH_BASE: path = "/";       return true;
		case PATH_EXE:  path = "/dhewm3"; return true;
		case PATH_CONFIG:
		case PATH_SAVE: path = "/home";   return true;
		default:        return false;     // engine tolerates missing config/save
	}
}
// Sys_ListFiles over dosfs. Semantics mirror posix_main.cpp: extension "/" =
// directories only; "" / NULL = all files; ".pk4" = that extension; append base
// names only; return -1 if the directory does not exist, else the count.
static void bm_list_append_cb(const char *base, int /*is_dir*/, void *user) {
	((idList<idStr> *)user)->Append(idStr(base));
}
int Sys_ListFiles(const char *directory, const char *extension, idList<class idStr> &list) {
	int count = 0;
	int saw = 0;
	list.Clear();
	// engine OS paths look like "/base" (fs_basepath="/"); dosfs is image-relative.
	const char *rel = (directory && directory[0] == '/') ? directory + 1 : directory;
	if (dosfs_mounted()) {
		int n = dosfs_list(rel ? rel : "", extension ? extension : "", bm_list_append_cb, &list);
		if (n >= 0) {
			count += n;
			saw = 1;
		}
	}
	{
		int n = dos_ramfs_list(rel ? rel : "", extension ? extension : "", bm_list_append_cb, &list);
		if (n >= 0) {
			count += n;
			saw = 1;
		}
	}
	return saw ? count : -1;
}
void      Sys_Mkdir(const char *path) { (void)dos_ramfs_mkdir(path); }
// Stable non-zero timestamp from the image build epoch (a frozen image never
// changes, so reload-detection comparisons are harmless).
ID_TIME_T Sys_FileTimeStamp(FILE *fp) { (void)fp; return (ID_TIME_T)dosfs_build_epoch(); }

char *Sys_ConsoleInput(void) {
	return NULL;   // TODO: SBI legacy getchar + idEditField line loop
}

// ===========================================================================
// Networking — no TCP/IP stack on bare metal: stubs (replaces posix_net.cpp).
// ===========================================================================
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
bool Sys_StringToNetAdr(const char *s, netadr_t *a, bool doDNSResolve) {
	(void)s; (void)a; (void)doDNSResolve; return false;
}
const char *Sys_NetAdrToString(const netadr_t a) { (void)a; return "0.0.0.0"; }
bool Sys_IsLANAddress(const netadr_t a) { (void)a; return false; }
bool Sys_CompareNetAdrBase(const netadr_t a, const netadr_t b) { (void)a; (void)b; return false; }

// ===========================================================================
// Lifecycle + entry point
// ===========================================================================
// idSysLocal vtable entries normally provided by posix_main.cpp / linux/main.cpp.
// No URL launcher / no subprocess on bare metal: no-ops.
void idSysLocal::OpenURL(const char *url, bool quit)       { (void)url; (void)quit; }
void idSysLocal::StartProcess(const char *exeName, bool quit) { (void)exeName; (void)quit; }

void Sys_Init(void) {
	common->Printf("dhewm3 bare-metal (dOS): %d MB heap\n", Sys_GetSystemRam());
}
void Sys_Shutdown(void) {}
void Sys_Quit(void) { sbi_shutdown(); }

void Sys_Error(const char *error, ...) {
	va_list a; va_start(a, error);
	Sys_Printf("Sys_Error: ");
	Sys_DebugVPrintf(error, a);
	Sys_Printf("\n");
	va_end(a);
	panic("Sys_Error");   // -> sbi_shutdown (noreturn)
}

static void BM_RamFsSelfTest(void) {
	const char *path = "/home/base/.dosramfs-test";
	const char *path2 = "/home/base/.dosramfs-test2";
	const char *a = "dos";
	const char *b = "fs";
	char buf[8] = {};
	struct stat st;
	FILE *f;

	Sys_Mkdir("/home");
	Sys_Mkdir("/home/base");

	f = fopen(path, "wb");
	if (!f || fwrite(a, 1, strlen(a), f) != strlen(a)) {
		if (f) fclose(f);
		Sys_Printf("ramfs: write self-test failed\n");
		return;
	}
	fclose(f);

	f = fopen(path, "ab");
	if (!f || fwrite(b, 1, strlen(b), f) != strlen(b)) {
		if (f) fclose(f);
		Sys_Printf("ramfs: append self-test failed\n");
		return;
	}
	fclose(f);

	f = fopen(path, "rb");
	if (!f || fread(buf, 1, 5, f) != 5 || memcmp(buf, "dosfs", 5) != 0) {
		if (f) fclose(f);
		Sys_Printf("ramfs: readback self-test failed\n");
		return;
	}
	fclose(f);

	if (stat(path, &st) != 0 || st.st_size != 5 ||
			dos_ramfs_list("/home/base", "", NULL, NULL) < 1) {
		Sys_Printf("ramfs: stat/list self-test failed\n");
		return;
	}

	if (rename(path, path2) != 0 || remove(path2) != 0) {
		Sys_Printf("ramfs: rename/unlink self-test failed\n");
		return;
	}

	Sys_Printf("ramfs: rw self-test ok (/home)\n");
}

// Bring-up self-tests for the savefs persistence path. Set to 1 to re-arm the
// driver write/flush + reboot-persistence self-tests; the production
// mount/replay/commit wiring stays intact either way (the compiler dead-strips
// the gated calls when 0). Verified PASS 2026-06-21. See riscv/docs/fs/13-savefs.md.
#define DOS_SAVEFS_SELFTEST 0

#if DOS_SAVEFS_SELFTEST
// P0/driver: raw write+flush+readback against a scratch sector the append log
// never reaches (the region's last sector). On a second boot of the SAME image
// the marker is still present, proving FLUSH durability end-to-end.
static void BM_VblkWriteSelfTest(void) {
	static uint8_t buf[512] __attribute__((aligned(64)));
	const char marker[16] = { 'D','O','S','-','V','B','L','K','-','P','E','R','S','I','S','T' };
	uint64_t lba = savefs_scratch_lba();
	if (!lba) { Sys_Printf("vblk: write self-test skipped (no write-region)\n"); return; }
	if (vblk_read(lba, buf, 1) != 0) { Sys_Printf("vblk: self-test read failed\n"); return; }
	int present = (memcmp(buf, marker, sizeof marker) == 0);
	Sys_Printf("vblk: scratch pattern %s before write\n",
		present ? "PRESENT (persisted across reboot)" : "absent (first boot)");
	memset(buf, 0, sizeof buf);
	memcpy(buf, marker, sizeof marker);
	if (vblk_write_blocks(lba, 1, buf) != 0) { Sys_Printf("vblk: self-test WRITE failed\n"); return; }
	if (vblk_flush() < 0)                     { Sys_Printf("vblk: self-test FLUSH failed\n"); return; }
	memset(buf, 0, sizeof buf);
	if (vblk_read(lba, buf, 1) != 0 || memcmp(buf, marker, sizeof marker) != 0) {
		Sys_Printf("vblk: self-test readback FAIL\n"); return;
	}
	Sys_Printf("vblk: write+flush+readback self-test PASS\n");
}

// P2/end-to-end: exercise the real fopen->_open->ramfs->_close->savefs_commit_file
// path. A boot counter written here is replayed back on the next boot of the SAME
// image, proving commit-on-close + replay-on-boot persistence.
static void BM_SaveFsPersistTest(void) {
	const char *path = "/home/.savefs-boots";
	char buf[32] = {};
	long boots = 0;
	FILE *f = fopen(path, "rb");
	if (f) {
		size_t n = fread(buf, 1, sizeof buf - 1, f);
		buf[n] = 0;
		fclose(f);
		boots = atol(buf);
		Sys_Printf("savefs: persist marker found, boots=%ld (replayed across reboot)\n", boots);
	} else {
		Sys_Printf("savefs: persist marker absent (first boot)\n");
	}
	boots++;
	f = fopen(path, "wb");
	if (!f) { Sys_Printf("savefs: persist marker write open failed\n"); return; }
	// Format the integer by hand: idlib's Str.h poisons snprintf in engine C++.
	char out[32];
	char tmp[24];
	int len = 0, ti = 0;
	long v = boots;
	if (v == 0) tmp[ti++] = '0';
	while (v > 0) { tmp[ti++] = (char)('0' + (v % 10)); v /= 10; }
	while (ti > 0) out[len++] = tmp[--ti];
	out[len++] = '\n';
	if (fwrite(out, 1, (size_t)len, f) != (size_t)len) { fclose(f); Sys_Printf("savefs: persist marker write failed\n"); return; }
	fclose(f);   // -> _close -> savefs_commit_file (+ flush)
	Sys_Printf("savefs: persist marker written, boots=%ld (committed)\n", boots);
}
#endif // DOS_SAVEFS_SELFTEST

/*
 * dhewm3_main — replaces sys/linux/main.cpp main(). The freestanding CRT keeps a
 * single real entry (_start -> _cstart -> kmain); engine_kmain.c's kmain() calls
 * BM_InitTime() then this. SDL is initialised inside common->Init() exactly as on
 * the desktop SDL3 build (no SDL_main needed; the dos video driver auto-selects).
 */
// Bare-metal framebuffer capture is done host-side: dos_gl_present() scans each
// frame out to the virtio-gpu device, and the host grabs it with QEMU's monitor
// `screendump` (riscv/scripts/capture_screendump.sh). No serial framebuffer dump.
extern "C" void bm_io_stats_print(void);
static idCVar bm_ioStats( "bm_ioStats", "0", CVAR_SYSTEM | CVAR_BOOL,
	"bare-metal: print low-frequency virtio/dosfs/file-extension I/O timing summaries" );
idCVar bm_quietAssetLoads( "bm_quietAssetLoads", "0", CVAR_SYSTEM | CVAR_BOOL,
	"bare-metal: suppress high-volume per-asset load messages while preserving warnings/errors" );
static idCVar bm_simdBench( "bm_simdBench", "0", CVAR_SYSTEM | CVAR_BOOL,
	"bare-metal: run a small idSIMDProcessor microbench after engine init" );
static idCVar bm_simdTest( "bm_simdTest", "0", CVAR_SYSTEM | CVAR_BOOL,
	"bare-metal: run idSIMD::Test_f (RVV vs generic differential correctness test), then power off" );
static idCVar bm_renderProf( "bm_renderProf", "0", CVAR_SYSTEM | CVAR_BOOL,
	"bare-metal: per-stage softgles2 render profiler (one [rprof] line per frame)" );
static idCVar bm_sglDbgDraws( "bm_sglDbgDraws", "0", CVAR_SYSTEM | CVAR_BOOL,
	"bare-metal: enable filtered softgles2 per-draw diagnostics" );
static idCVar bm_sglDbgFrameFirst( "bm_sglDbgFrameFirst", "-1", CVAR_SYSTEM | CVAR_INTEGER,
	"bare-metal: first softgles2 present frame included in per-draw diagnostics (-1 = any)" );
static idCVar bm_sglDbgFrameLast( "bm_sglDbgFrameLast", "-1", CVAR_SYSTEM | CVAR_INTEGER,
	"bare-metal: last softgles2 present frame included in per-draw diagnostics (-1 = any)" );
static idCVar bm_sglDbgDrawFirst( "bm_sglDbgDrawFirst", "-1", CVAR_SYSTEM | CVAR_INTEGER,
	"bare-metal: first glDraw call included in per-draw diagnostics (-1 = any)" );
static idCVar bm_sglDbgDrawLast( "bm_sglDbgDrawLast", "-1", CVAR_SYSTEM | CVAR_INTEGER,
	"bare-metal: last glDraw call included in per-draw diagnostics (-1 = any)" );
static idCVar bm_sglDbgDetail( "bm_sglDbgDetail", "1", CVAR_SYSTEM | CVAR_INTEGER,
	"bare-metal: softgles2 draw diagnostic detail level: 1 state/attribs, 2 add input min/max" );
static idCVar bm_iakLightUnity( "bm_iakLightUnity", "0", CVAR_SYSTEM | CVAR_BOOL,
	"diag: force softgles2 interaction light term to unity (diffuse-only) to isolate the flat-world cause" );
static idCVar bm_scenicWarp( "bm_scenicWarp", "1", CVAR_SYSTEM | CVAR_BOOL,
	"bare-metal: pin the player at a fixed scenic vantage (setviewpos, frames 8-60) for a static survey; set 0 to let the scripted usercmd walk the level (walk-to-NPC capture)" );
static idCVar bm_npcCam( "bm_npcCam", "0", CVAR_SYSTEM | CVAR_BOOL,
	"bare-metal: cycle the view through vantages that frame the map's animated md5 NPCs (Betruger, Campbell, seated marines) for a definitive skeletal-animation capture; overrides bm_scenicWarp" );
extern "C" void sgl_prof_enable( int on );
extern "C" void sgl_diag_draw_filter_set( int enabled, int frame_first, int frame_last,
	int draw_first, int draw_last, int detail );
extern "C" int sgl_dbg_iak_light_unity;

extern "C" int bm_io_stats_enabled(void) {
	return bm_ioStats.GetBool() ? 1 : 0;
}

static float BM_AbsFloat( float v ) {
	return v < 0.0f ? -v : v;
}

static float BM_MaxDiff( const float *a, const float *b, int count ) {
	float maxDiff = 0.0f;
	for ( int i = 0; i < count; ++i ) {
		float d = BM_AbsFloat( a[i] - b[i] );
		if ( d > maxDiff ) {
			maxDiff = d;
		}
	}
	return maxDiff;
}

static void BM_SIMDBenchOne( const char *name, uint64_t genericTicks, uint64_t simdTicks, uint64_t elements, float maxDiff, volatile float sink ) {
	uint64_t genericNs = genericTicks * 1000000000ULL / TIMEBASE_HZ;
	uint64_t simdNs = simdTicks * 1000000000ULL / TIMEBASE_HZ;
	uint64_t genericPs = elements ? ( genericNs * 1000ULL ) / elements : 0;
	uint64_t simdPs = elements ? ( simdNs * 1000ULL ) / elements : 0;
	uint64_t speedPct = simdNs ? ( genericNs * 100ULL ) / simdNs : 0;

	Sys_Printf( "[simdbench] %-10s generic_ns=%llu simd_ns=%llu generic_ps_per_elem=%llu simd_ps_per_elem=%llu speed_pct=%llu diff=%d sink=%d\n",
		name,
		(unsigned long long)genericNs,
		(unsigned long long)simdNs,
		(unsigned long long)genericPs,
		(unsigned long long)simdPs,
		(unsigned long long)speedPct,
		(int)( maxDiff * 1000000.0f ),
		(int)sink );
}

static void BM_SIMDBenchDot( const char *name, uint64_t genericTicks, uint64_t simdTicks, uint64_t elements, float genericDot, float simdDot, volatile float sink ) {
	float diff = BM_AbsFloat( genericDot - simdDot );
	BM_SIMDBenchOne( name, genericTicks, simdTicks, elements, diff, sink );
}

static void BM_RunSIMDBench(void) {
	static float a[4096] __attribute__((aligned(64)));
	static float b[4096] __attribute__((aligned(64)));
	static float c[4096] __attribute__((aligned(64)));
	static float g[4096] __attribute__((aligned(64)));
	idSIMD_Generic genericProcessor;
	const int count = 4096;
	const int iters = 1024;
	uint64_t t0, tg, ts;
	float genericDot = 0.0f;
	float simdDot = 0.0f;
	volatile float sink = 0.0f;

	for ( int i = 0; i < count; ++i ) {
		a[i] = (float)( ( i & 255 ) - 128 ) * 0.25f;
		b[i] = (float)( ( i * 13 ) & 127 ) * 0.5f;
		c[i] = 0.0f;
		g[i] = 0.0f;
	}

	Sys_Printf( "[simdbench] processor=%s count=%d iters=%d\n", SIMDProcessor->GetName(), count, iters );

	t0 = timer_ticks();
	for ( int i = 0; i < iters; ++i ) genericProcessor.Add( g, a, b, count );
	tg = timer_ticks() - t0;
	t0 = timer_ticks();
	for ( int i = 0; i < iters; ++i ) SIMDProcessor->Add( c, a, b, count );
	ts = timer_ticks() - t0;
	sink += c[iters & (count - 1)] + g[iters & (count - 1)];
	BM_SIMDBenchOne( "Add", tg, ts, (uint64_t)count * iters, BM_MaxDiff( c, g, count ), sink );

	for ( int i = 0; i < count; ++i ) {
		c[i] = 0.0f;
		g[i] = 0.0f;
	}
	t0 = timer_ticks();
	for ( int i = 0; i < iters; ++i ) genericProcessor.MulAdd( g, 1.25f, a, count );
	tg = timer_ticks() - t0;
	t0 = timer_ticks();
	for ( int i = 0; i < iters; ++i ) SIMDProcessor->MulAdd( c, 1.25f, a, count );
	ts = timer_ticks() - t0;
	sink += c[(iters * 3) & (count - 1)] + g[(iters * 3) & (count - 1)];
	BM_SIMDBenchOne( "MulAdd", tg, ts, (uint64_t)count * iters, BM_MaxDiff( c, g, count ), sink );

	t0 = timer_ticks();
	for ( int i = 0; i < iters; ++i ) genericProcessor.Clamp( g, a, -8.0f, 8.0f, count );
	tg = timer_ticks() - t0;
	t0 = timer_ticks();
	for ( int i = 0; i < iters; ++i ) SIMDProcessor->Clamp( c, a, -8.0f, 8.0f, count );
	ts = timer_ticks() - t0;
	sink += c[(iters * 5) & (count - 1)] + g[(iters * 5) & (count - 1)];
	BM_SIMDBenchOne( "Clamp", tg, ts, (uint64_t)count * iters, BM_MaxDiff( c, g, count ), sink );

	t0 = timer_ticks();
	for ( int i = 0; i < iters; ++i ) genericProcessor.Dot( genericDot, a, b, count );
	tg = timer_ticks() - t0;
	t0 = timer_ticks();
	for ( int i = 0; i < iters; ++i ) SIMDProcessor->Dot( simdDot, a, b, count );
	ts = timer_ticks() - t0;
	sink += simdDot + genericDot;
	BM_SIMDBenchDot( "Dot", tg, ts, (uint64_t)count * iters, genericDot, simdDot, sink );

	t0 = timer_ticks();
	for ( int i = 0; i < iters; ++i ) genericProcessor.Memcpy( g, a, count * (int)sizeof( float ) );
	tg = timer_ticks() - t0;
	t0 = timer_ticks();
	for ( int i = 0; i < iters; ++i ) SIMDProcessor->Memcpy( c, a, count * (int)sizeof( float ) );
	ts = timer_ticks() - t0;
	sink += c[(iters * 7) & (count - 1)] + g[(iters * 7) & (count - 1)];
	BM_SIMDBenchOne( "Memcpy", tg, ts, (uint64_t)count * sizeof( float ) * iters, BM_MaxDiff( c, g, count ), sink );
}

// Drive a REAL in-game savegame round-trip with no interactive input, to prove
// savefs persists actual savegames/<name>.{save,tga,txt}. Auto-sequences across
// two boots of the same image:
//   boot A (save absent)  -> wait for a spawned map, "saveGame dostest", confirm
//                            the .save lands in ramfs (-> committed+flushed), quit.
//   boot B (save replayed)-> "loadGame dostest", confirm we end up on a map, quit.
// Set to 1 to arm; 0 dead-strips it. See riscv/docs/fs/13-savefs.md §3.
#define DOS_SAVEFS_SAVETEST 0
#if DOS_SAVEFS_SAVETEST
static void BM_SaveGameRoundTrip(int frame) {
	static const char *SAVEKEY = "/home/base/savegames/dostest.save";
	static int phase = -1;   // -1 undecided, 0 = save mode, 1 = load mode
	static int done = 0;
	uint64_t sz = 0; int isdir = 0; uint32_t mt = 0;

	if (!savefs_active() || done) return;
	int exists = (dos_ramfs_stat(SAVEKEY, &sz, &isdir, &mt) == 0 && sz > 0);
	if (phase < 0) {
		phase = exists ? 1 : 0;
		Sys_Printf("savetest: %s mode (dostest.save %s at boot)\n",
			phase ? "LOAD" : "SAVE", exists ? "present" : "absent");
	}

	if (phase == 0) {                       // SAVE: needs a spawned map
		const char *mn = session ? session->GetCurrentMapName() : NULL;
		// Save on the FIRST frame the map is up — gameplay frames are minutes
		// each under TCG, so don't wait for frame 30.
		if (mn && mn[0]) {
			Sys_Printf("savetest: map '%s' up at frame %d; issuing saveGame\n", mn, frame);
			cmdSystem->BufferCommandText(CMD_EXEC_NOW, "saveGame dostest\n");
			if (dos_ramfs_stat(SAVEKEY, &sz, &isdir, &mt) == 0 && sz > 0) {
				Sys_Printf("savetest: SAVE ok, dostest.save %llu bytes -> committed+flushed\n",
					(unsigned long long)sz);
				done = 1;
				Sys_Quit();                 // clean power-off (commit already durable)
			}
		}
		if (frame > 6000) { Sys_Printf("savetest: SAVE gave up (no spawned map by frame %d)\n", frame); done = 1; }
	} else {                                // LOAD: the save was replayed from disk
		if (frame == 2) {
			Sys_Printf("savetest: replayed dostest.save (%llu bytes); issuing loadGame\n",
				(unsigned long long)sz);
			cmdSystem->BufferCommandText(CMD_EXEC_NOW, "loadGame dostest\n");
		}
		if (frame > 4 && (frame % 30 == 0)) {
			const char *mn = session ? session->GetCurrentMapName() : NULL;
			if (mn && mn[0]) {
				Sys_Printf("savetest: LOAD ok, now on map '%s' from the persisted save\n", mn);
				done = 1;
				Sys_Quit();
			}
		}
		if (frame > 6000) { Sys_Printf("savetest: LOAD gave up\n"); done = 1; }
	}
}
#endif

// ===========================================================================
// bm_simdTest — purpose-built idSIMD_RVV vs idSIMD_Generic differential test.
//
// Feeds identical deterministic inputs to the active processor (idSIMD_RVV on
// RISC-V) and to a fresh idSIMD_Generic (the scalar id Tech 4 reference), then
// compares outputs. Sidesteps the id4 idSIMD::Test_f timing scaffolding, which
// faults under bare-metal TCG. Covers the hand-written RVV kernels — especially
// the ones with fixed bugs — plus a few controls that are known to match.
// ===========================================================================
extern idSIMDProcessor *generic;    // reference: idSIMD_Generic
extern idSIMDProcessor *processor;  // active: idSIMD_RVV on RISC-V

static unsigned int bm_sd_rng = 0x9e3779b9u;
static float BM_SDRand( void ) {                 // ~[-1, 1)
	bm_sd_rng = bm_sd_rng * 1664525u + 1013904223u;
	return (float)( (int)( ( bm_sd_rng >> 8 ) & 0xFFFFFF ) ) * ( 2.0f / (float)0x1000000 ) - 1.0f;
}
static unsigned int BM_SDIdx( unsigned int mod ) {
	bm_sd_rng = bm_sd_rng * 1664525u + 1013904223u;
	return ( bm_sd_rng >> 9 ) % mod;
}
static int bm_sd_fails = 0;
static void BM_SDCmp( const char *name, const float *g, const float *r, int n, float tol ) {
	float maxd = 0.0f; int bi = -1;
	for ( int i = 0; i < n; i++ ) {
		float d = g[i] - r[i]; if ( d < 0.0f ) d = -d;
		if ( d > maxd ) { maxd = d; bi = i; }
	}
	int ok = ( maxd <= tol );
	if ( !ok ) { bm_sd_fails++; }
	Sys_Printf( "[simddiff] %-30s %s  maxdiff=%ld e-6 (tol %ld e-6) @%d\n",
		name, ok ? "PASS" : "FAIL", (long)( maxd * 1.0e6f ), (long)( tol * 1.0e6f ), bi );
}
static void BM_SDUnitQuat( float q[4] ) {
	float x = BM_SDRand(), y = BM_SDRand(), z = BM_SDRand(), w = BM_SDRand() + 1.5f;
	float inv = 1.0f / __builtin_sqrtf( x*x + y*y + z*z + w*w );
	q[0] = x*inv; q[1] = y*inv; q[2] = z*inv; q[3] = w*inv;
}

static void BM_RunSIMDDiffTest( void ) {
	Sys_Printf( "[simddiff] ===== idSIMD_RVV vs idSIMD_Generic: %s vs %s =====\n",
		processor ? processor->GetName() : "?", generic ? generic->GetName() : "?" );
	if ( !generic || !processor ) { Sys_Printf( "[simddiff] processors not initialised\n" ); return; }
	bm_sd_rng = 0x9e3779b9u; bm_sd_fails = 0;

	// ---- controls (audit: these match generic) ----
	{
		enum { N = 260 };
		static float a[N], b[N], g[N], r[N];
		for ( int i = 0; i < N; i++ ) { a[i] = BM_SDRand() * 10.0f; b[i] = BM_SDRand() * 10.0f; }
		generic->Add( g, a, b, N ); processor->Add( r, a, b, N );  BM_SDCmp( "Add[] (control)", g, r, N, 1e-4f );
		generic->Mul( g, 3.0f, a, N ); processor->Mul( r, 3.0f, a, N ); BM_SDCmp( "Mul const (control)", g, r, N, 1e-4f );
		float gmn, gmx, rmn, rmx; generic->MinMax( gmn, gmx, a, N ); processor->MinMax( rmn, rmx, a, N );
		float g2[2] = { gmn, gmx }, r2[2] = { rmn, rmx };  BM_SDCmp( "MinMax (control)", g2, r2, 2, 1e-5f );
	}
	// ---- TransformJoints (fixed: store width 3->4) ----
	{
		enum { NJ = 48 };
		static idJointMat gm[NJ], rm[NJ]; static int parents[NJ];
		parents[0] = -1;
		for ( int i = 1; i < NJ; i++ ) parents[i] = i / 2;               // bounded tree, < i
		for ( int i = 0; i < NJ; i++ ) {
			float *m = gm[i].ToFloatPtr(), *m2 = rm[i].ToFloatPtr();
			for ( int k = 0; k < 12; k++ ) { m[k] = BM_SDRand(); m2[k] = m[k]; }
		}
		generic->TransformJoints( gm, parents, 1, NJ - 1 );
		processor->TransformJoints( rm, parents, 1, NJ - 1 );
		BM_SDCmp( "TransformJoints", gm[0].ToFloatPtr(), rm[0].ToFloatPtr(), NJ * 12, 1e-4f );
	}
	// ---- ConvertJointQuatsToJointMats (fixed: R -> R^T) ----
	{
		enum { NJ = 48 };
		static idJointQuat jq[NJ]; static idJointMat gm[NJ], rm[NJ];
		for ( int i = 0; i < NJ; i++ ) {
			float q[4]; BM_SDUnitQuat( q ); jq[i].q.Set( q[0], q[1], q[2], q[3] );
			jq[i].t.Set( BM_SDRand()*10.0f, BM_SDRand()*10.0f, BM_SDRand()*10.0f );
		}
		generic->ConvertJointQuatsToJointMats( gm, jq, NJ );
		processor->ConvertJointQuatsToJointMats( rm, jq, NJ );
		BM_SDCmp( "ConvertJointQuatsToJointMats", gm[0].ToFloatPtr(), rm[0].ToFloatPtr(), NJ * 12, 1e-4f );
	}
	// ---- BlendJoints (fixed: byte-offset vluxei/vsoxei addressing + 2nd invsqrt Newton step) ----
	{
		enum { NJ = 48 };
		static idJointQuat gj[NJ], rj[NJ], bj[NJ]; static int index[NJ];
		for ( int i = 0; i < NJ; i++ ) {
			float q[4]; BM_SDUnitQuat( q ); gj[i].q.Set( q[0], q[1], q[2], q[3] );
			gj[i].t.Set( BM_SDRand(), BM_SDRand(), BM_SDRand() ); rj[i] = gj[i];
			BM_SDUnitQuat( q ); bj[i].q.Set( q[0], q[1], q[2], q[3] );
			bj[i].t.Set( BM_SDRand(), BM_SDRand(), BM_SDRand() ); index[i] = i;
		}
		generic->BlendJoints( gj, bj, 0.35f, index, NJ );
		processor->BlendJoints( rj, bj, 0.35f, index, NJ );
		BM_SDCmp( "BlendJoints", (const float *)gj, (const float *)rj, NJ * 7, 1e-4f );
	}
	// ---- DeriveTriPlanes (byte-offset gather + reversed cross) ----
	// The cross product, normalize, and d = -(n.a) formula all match generic. The
	// ONLY divergence is normalize precision: generic uses idMath::RSqrt (Quake fast
	// invsqrt, ~0.17% error); RVV uses vfrsqrt7 + a Newton step (~0.006%, actually
	// MORE accurate). That ~1.7e-3 relative normal error rides the plane distance
	// d = n.a, which scales with the vertex coordinate — so keep coords modest (8, not
	// 80) and allow 2e-2 so the precision gap stays under tol while a real formula bug
	// (O(1) relative -> O(coord) distance error) is still caught.
	{
		enum { NV = 256, NT = 128, NI = NT * 3 };
		static idDrawVert verts[NV]; static int indexes[NI]; static idPlane gp[NT], rp[NT];
		for ( int i = 0; i < NV; i++ ) verts[i].xyz.Set( BM_SDRand()*8.0f, BM_SDRand()*8.0f, BM_SDRand()*8.0f );
		for ( int i = 0; i < NI; i++ ) indexes[i] = (int)BM_SDIdx( NV );
		generic->DeriveTriPlanes( gp, verts, NV, indexes, NI );
		processor->DeriveTriPlanes( rp, verts, NV, indexes, NI );
		BM_SDCmp( "DeriveTriPlanes", gp[0].ToFloatPtr(), rp[0].ToFloatPtr(), NT * 4, 2e-2f );
	}
	// ---- DeriveUnsmoothedTangents (byte-offset gather + reversed bitangent) ----
	// normal, tangent and bitangent formulas + idDrawVert field offsets all match
	// generic. With normalizationScale=1 the outputs are UNNORMALIZED cross products,
	// whose magnitude grows ~coord^3 (bitangent = normal x tangent); at coord 40 that
	// is ~1e6, so FMA-vs-separate-rounding (~1e-7 relative) shows as ~0.03 absolute.
	// Real usage passes 1/|..| scales (outputs ~unit); keep coords modest (4) here so
	// the outputs are O(100s) and the 1e-2 tol cleanly separates precision (~1e-5) from
	// a real formula bug (O(1) relative -> huge absolute).
	{
		enum { NV = 256 };
		static idDrawVert gv[NV], rv[NV]; static dominantTri_s dt[NV];
		for ( int i = 0; i < NV; i++ ) {
			gv[i].xyz.Set( BM_SDRand()*4.0f, BM_SDRand()*4.0f, BM_SDRand()*4.0f );
			gv[i].st.Set( BM_SDRand(), BM_SDRand() );
			gv[i].normal.Set( BM_SDRand(), BM_SDRand(), BM_SDRand() );
			rv[i] = gv[i];
			dt[i].v2 = (glIndex_t)BM_SDIdx( NV ); dt[i].v3 = (glIndex_t)BM_SDIdx( NV );
			dt[i].normalizationScale[0] = 1.0f; dt[i].normalizationScale[1] = 1.0f; dt[i].normalizationScale[2] = 1.0f;
		}
		generic->DeriveUnsmoothedTangents( gv, dt, NV );
		processor->DeriveUnsmoothedTangents( rv, dt, NV );
		float maxd = 0.0f; int bi = -1;
		for ( int i = 0; i < NV; i++ ) {
			const float *gf = gv[i].tangents[0].ToFloatPtr();   // tangents[0..1] contiguous (6 floats)
			const float *rf = rv[i].tangents[0].ToFloatPtr();
			for ( int k = 0; k < 6; k++ ) { float d = gf[k]-rf[k]; if(d<0)d=-d; if(d>maxd){maxd=d;bi=i*6+k;} }
		}
		int ok = ( maxd <= 1e-2f ); if ( !ok ) bm_sd_fails++;
		Sys_Printf( "[simddiff] %-30s %s  maxdiff=%ld e-6 (tol 10000 e-6) @%d\n",
			"DeriveUnsmoothedTangents", ok?"PASS":"FAIL", (long)( maxd*1.0e6f ), bi );
	}
	// ---- MixSound (fixed: strided interleaved load/store) ----
	{
		enum { NS = MIXBUFFER_SAMPLES };
		static float src1[NS], src2[NS*2], gbuf[NS*6], rbuf[NS*6];
		float lastV2[2] = { 0.5f, 0.7f }, curV2[2] = { 0.9f, 1.1f };
		float lastV6[6] = { .4f,.5f,.6f,.7f,.8f,.9f }, curV6[6] = { .9f,.8f,.7f,.6f,.5f,.4f };
		for ( int i = 0; i < NS;   i++ ) src1[i] = BM_SDRand();
		for ( int i = 0; i < NS*2; i++ ) src2[i] = BM_SDRand();
		for ( int i = 0; i < NS*2; i++ ) { gbuf[i] = BM_SDRand(); rbuf[i] = gbuf[i]; }
		generic->MixSoundTwoSpeakerMono( gbuf, src1, NS, lastV2, curV2 );
		processor->MixSoundTwoSpeakerMono( rbuf, src1, NS, lastV2, curV2 );
		BM_SDCmp( "MixSoundTwoSpeakerMono", gbuf, rbuf, NS*2, 1e-4f );
		for ( int i = 0; i < NS*2; i++ ) { gbuf[i] = BM_SDRand(); rbuf[i] = gbuf[i]; }
		generic->MixSoundTwoSpeakerStereo( gbuf, src2, NS, lastV2, curV2 );
		processor->MixSoundTwoSpeakerStereo( rbuf, src2, NS, lastV2, curV2 );
		BM_SDCmp( "MixSoundTwoSpeakerStereo", gbuf, rbuf, NS*2, 1e-4f );
		for ( int i = 0; i < NS*6; i++ ) { gbuf[i] = BM_SDRand(); rbuf[i] = gbuf[i]; }
		generic->MixSoundSixSpeakerStereo( gbuf, src2, NS, lastV6, curV6 );
		processor->MixSoundSixSpeakerStereo( rbuf, src2, NS, lastV6, curV6 );
		BM_SDCmp( "MixSoundSixSpeakerStereo", gbuf, rbuf, NS*6, 1e-4f );
	}
	Sys_Printf( "[simddiff] ===== %s: %d method(s) FAILED =====\n",
		bm_sd_fails ? "MISMATCH" : "ALL PASS", bm_sd_fails );
}

extern "C" int dhewm3_main(int argc, char **argv) {
	Sys_InitThreads();   // from sys/threads.cpp (SDL)

	// Mount the read-only asset image (virtio-blk -> dosfs) BEFORE common->Init,
	// so fileSystem->Init() can list + open base/*.pk4 and load default.cfg.
	if (vblk_init() == 0 && dosfs_init(&vblk_read) == 0) {
		Sys_Printf("dosfs: mounted, %d files, %llu sectors on virtio-blk\n",
			dosfs_entry_count(), (unsigned long long)vblk_capacity_sectors());
	} else {
		Sys_Printf("dosfs: no asset image (default.cfg will be missing)\n");
	}
	// The ramfs self-test runs while savefs is still inactive, so its throwaway
	// file is NOT journalled.
	BM_RamFsSelfTest();

	// Mount + replay the persistent savefs journal (the write-region appended to
	// the same image) BEFORE common->Init, so replayed config/saves are visible to
	// fileSystem->Init(). A legacy image (no write-region) -> volatile-ramfs mode.
	{
		static uint8_t s_super[512] __attribute__((aligned(64)));
		if (vblk_read(0, s_super, 1) == 0 && savefs_mount(s_super) == 0) {
			int nrec = savefs_replay();
			Sys_Printf("savefs: mounted, %d records replayed, %llu MiB free\n",
				nrec, (unsigned long long)(savefs_free_bytes() >> 20));
#if DOS_SAVEFS_SELFTEST
			BM_VblkWriteSelfTest();
			BM_SaveFsPersistTest();
#endif
		} else {
			Sys_Printf("savefs: no write-region (saves are volatile)\n");
		}
	}

	/* Deterministic capture: if savefs replayed the persisted savegame into the
	 * readable game path, drop straight into live gameplay via loadGame (issued at
	 * frame 2 below — the proven timing; a +loadGame startup command runs too early
	 * and fails). The +map intro cinematic in argv is the fallback when no save. */
	int bm_have_save = 0;
	{
		uint64_t sz = 0; int isdir = 0; uint32_t mt = 0;
		if (dos_ramfs_stat("/home/base/savegames/dostest.save", &sz, &isdir, &mt) == 0 && sz > 0) {
			bm_have_save = 1;
			Sys_Printf("savefs: restored savegames/dostest.save (%llu bytes); loadGame at frame 2\n",
				(unsigned long long)sz);
		} else {
			Sys_Printf("savefs: no dostest.save record (using +map cinematic fallback)\n");
		}
	}

	common->Init((argc > 1) ? argc - 1 : 0, (argc > 1) ? &argv[1] : NULL);
	if ( bm_simdBench.GetBool() ) {
		BM_RunSIMDBench();
	}
	if ( bm_renderProf.GetBool() ) {
		extern int glsl_link_dump;
		sgl_prof_enable( 1 );
		glsl_link_dump = 1;   /* also dump each linked FS fingerprint */
	}
	sgl_diag_draw_filter_set( bm_sglDbgDraws.GetBool() ? 1 : 0,
		bm_sglDbgFrameFirst.GetInteger(), bm_sglDbgFrameLast.GetInteger(),
		bm_sglDbgDrawFirst.GetInteger(), bm_sglDbgDrawLast.GetInteger(),
		bm_sglDbgDetail.GetInteger() );
	if ( bm_iakLightUnity.GetBool() ) {
		sgl_dbg_iak_light_unity = 1;
	}
	if ( bm_simdTest.GetBool() ) {
		// Differential correctness test of the active SIMD processor (idSIMD_RVV on
		// RISC-V) vs a fresh idSIMD_Generic: idSIMD::Test_f feeds identical seeded inputs
		// to both and prints "ok"/red "X" per method (mismatch > 1e-5). Runs pre-map so a
		// simd-test boot is fast; powers off when done.
		BM_RunSIMDDiffTest();
		Sys_Printf( "[simdtest] ===== done; powering off =====\n" );
		Sys_Quit();
	}
#if DOS_SAVEFS_SAVETEST
	// The map load is the long pole; the r_glprof per-GL-call serial stream makes
	// it far slower. Silence it for the save round-trip (does not touch the argv).
	cvarSystem->SetCVarInteger("r_glprof", 0);
#endif
	for (int frame = 0; ; ++frame) {
		// Per-frame heartbeat: the 768x432 software render is very slow under TCG,
		// so this shows forward progress vs a hang inside common->Frame(). The host
		// capture (capture_screendump.sh) watches the render markers and grabs the
		// virtio-gpu scanout with QEMU `screendump`, then powers off via the monitor.
		Sys_Printf("[dos] >> frame %d  t=%.0f ms\n", frame, Sys_MillisecondsPrecise());
#if !DOS_SAVEFS_SAVETEST
		// Deterministic loadGame at frame 2 (proven timing) when the save was
		// replayed — lands us in live gameplay for a guaranteed lit capture. The
		// +map cinematic already ran as the fallback; loadGame switches off it.
		if (bm_have_save && frame == 2) {
			Sys_Printf("savefs: issuing loadGame dostest\n");
			cmdSystem->BufferCommandText(CMD_EXEC_NOW, "loadGame dostest\n");
		}
#endif
		// Headless demo: warp the player to a scenic, detailed Mars City interior
		// vantage once gameplay is live (coords harvested from getviewpos on a prior
		// run: draws:775 rich area vs. the sparse hangar-spawn wall). Issued for a few
		// frames so it wins over any map-scripted teleport, then the scripted usercmd
		// walks/pans to survey. setviewpos is an SP cheat -> no-op before player spawn.
		// Pin the player at a scenic interior EVERY frame after the map's scripted
		// intro-teleport (which fires ~frame 7 and yanks us to the dim hangar), with a
		// frame-stepped yaw so the view still sweeps a 360. Held (not one-shot) so the
		// held vantage actually presents past the teleport. Manual int format: idStr
		// snprintf is poisoned in engine C++.
		if ( bm_npcCam.GetBool() && frame >= 1 ) {
			// Definitive skeletal-animation capture: warp the view to a vantage ~140 units
			// in FRONT of an animated md5 actor (coords harvested from demo_mars_city1.map;
			// camera at eye height, yaw facing the actor), cycling actors every 6 frames so
			// a wall/occlusion on one still yields a clean shot on another. These actors
			// (Betruger, Campbell, the two seated reception marines) are the joint pipeline's
			// end product: BlendJoints->ConvertJointQuatsToJointMats->TransformJoints->skin.
			static const char *npc_views[] = {
				"setviewpos -3128 -1353 150 45\n",     // char_marine_young_chair1_1 @(-3064 -1289 128) a225 (seated, 90u)
				"setviewpos -3081 -1354 150 135\n",    // char_marine_asian_chair1_1 @(-3145 -1290 128) a315 (seated, 90u)
				"setviewpos -1274 -1545 195 195\n",    // char_betruger_1        @(-1361 -1568 170) a15  (standing, 90u)
				"setviewpos -1576 -1596 195 90\n",     // char_campbell_bfgcase_1 @(-1576 -1506 170) a270 (standing, 90u)
			};
			int idx = ( ( frame - 1 ) / 4 ) % 4;
			cmdSystem->BufferCommandText( CMD_EXEC_NOW, npc_views[idx] );
		} else if ( bm_scenicWarp.GetBool() && frame >= 8 && frame <= 60 ) {
			int yaw = ( frame * 24 ) % 360;
			char cmd[64];
			const char *pfx = "setviewpos -700 -1380 176 ";
			int len = 0;
			for ( const char *c = pfx; *c; ++c ) cmd[len++] = *c;
			char tmp[8]; int ti = 0, v = yaw;
			if ( v == 0 ) tmp[ti++] = '0';
			while ( v > 0 ) { tmp[ti++] = (char)( '0' + ( v % 10 ) ); v /= 10; }
			while ( ti > 0 ) cmd[len++] = tmp[--ti];
			cmd[len++] = '\n'; cmd[len] = 0;
			cmdSystem->BufferCommandText( CMD_EXEC_NOW, cmd );
		}
		common->Frame();
		// Headless: log the player viewpoint each in-world frame. Confirms the scripted
		// walk is actually translating (position changes frame-to-frame, not just the
		// yaw) and harvests "(x y z) yaw" coords for a scenic setviewpos. getviewpos is
		// an SP cheat command; it no-ops until a live local player exists.
		if ( frame >= 2 ) {
			cmdSystem->BufferCommandText( CMD_EXEC_NOW, "getviewpos\n" );
		}
		if ( bm_ioStats.GetBool() && ( frame < 3 || ( frame > 0 && ( frame % 100 ) == 0 ) ) ) {
			bm_io_stats_print();
		}
#if DOS_SAVEFS_SAVETEST
		BM_SaveGameRoundTrip(frame);
#endif
	}
	return 0;
}

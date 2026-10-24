/*
 * dos_syscalls.c — newlib bottom-end + POSIX stub layer for the bare-metal
 * dhewm3 ENGINE (Path B). A superset of riscv/dos/dos_syscalls.c: it adds the
 * extra libc/POSIX functions the full engine + SDL3 + gl4es reference that the
 * dos *smoke test* did not. Kept in riscv/engine/ (not the dos test dir) so the
 * engine link is self-contained.
 *
 * newlib syscalls routed to SBI / the linker heap window, plus gettimeofday
 * (rdtime CSR), select (SDL's sleep primitive), and POSIX stubs that newlib
 * only declares. Read-only assets are served by dosfs; config/save writes are
 * served by a RAM overlay until a persistent writable block filesystem lands.
 */
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/select.h>
#include <sys/types.h>      /* off_t (== _off_t == long, 64-bit on rv64) */
#include <fcntl.h>          /* O_ACCMODE / O_RDONLY                      */
#include <errno.h>
#include "boot/bm_pause.h"
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "boot/runtime.h"
#include "engine/dosfs.h"   /* dosfs reader: _open/_read/_lseek/... back-end */
#include "engine/dos_ramfs.h"
#include "engine/savefs.h"  /* persistence: commit a writable fd on close */
#include "engine/dos_virtio_blk.h"

extern char __heap_start[], __heap_end[];
extern char __stack_bottom[], __stack_top[];

/* ---- fd table (file I/O over read-only dosfs + writable ramfs) ---- *
 * fds 0/1/2 are reserved for the SBI console; engine files use fd >= 3. */
#define DOS_FD_BASE 3
#define DOS_MAX_FD  32
enum fd_kind { FD_FREE = 0, FD_DOSFS, FD_RAMFS };
enum file_ext_bucket {
	EXT_OTHER = 0,
	EXT_PK4,
	EXT_MAP,
	EXT_PROC,
	EXT_CM,
	EXT_AAS,
	EXT_LWO,
	EXT_ASE,
	EXT_TGA,
	EXT_CFG,
	EXT_SOUND,
	EXT_COUNT
};
typedef struct {
	uint64_t opens;
	uint64_t reads;
	uint64_t bytes;
	uint64_t ticks;
} file_ext_stat;
typedef struct {
	enum fd_kind kind;
	int          dirty;   /* a writable ramfs fd that was actually written */
	unsigned char ext_bucket;
	dosfs_file   ro;
	dos_ramfs_file rw;
} dos_fd;
static dos_fd g_fds[DOS_MAX_FD];   /* index = fd - DOS_FD_BASE */
static file_ext_stat g_ext_stats[EXT_COUNT];

static char ascii_lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int suffix_ci(const char *p, const char *ext)
{
	size_t lp, le, i;
	if (!p || !ext) return 0;
	lp = strlen(p);
	le = strlen(ext);
	if (lp < le) return 0;
	p += lp - le;
	for (i = 0; i < le; ++i) if (ascii_lc(p[i]) != ascii_lc(ext[i])) return 0;
	return 1;
}

static unsigned char ext_bucket_for_path(const char *p)
{
	if (suffix_ci(p, ".pk4"))  return EXT_PK4;
	if (suffix_ci(p, ".map"))  return EXT_MAP;
	if (suffix_ci(p, ".proc")) return EXT_PROC;
	if (suffix_ci(p, ".cm"))   return EXT_CM;
	if (suffix_ci(p, ".aas"))  return EXT_AAS;
	if (suffix_ci(p, ".lwo"))  return EXT_LWO;
	if (suffix_ci(p, ".ase"))  return EXT_ASE;
	if (suffix_ci(p, ".tga"))  return EXT_TGA;
	if (suffix_ci(p, ".cfg"))  return EXT_CFG;
	if (suffix_ci(p, ".ogg") || suffix_ci(p, ".wav")) return EXT_SOUND;
	return EXT_OTHER;
}

static const char *ext_name(int bucket)
{
	static const char *names[EXT_COUNT] = {
		"other", "pk4", "map", "proc", "cm", "aas", "lwo", "ase", "tga", "cfg", "sound"
	};
	return (bucket >= 0 && bucket < EXT_COUNT) ? names[bucket] : "bad";
}

void bm_io_stats_print(void)
{
	vblk_stats vs;
	dosfs_stats ds;
	int i;
	vblk_stats_snapshot(&vs);
	dosfs_stats_snapshot(&ds);
	printf("[io] vblk read_calls=%llu sectors=%llu ms=%llu write_calls=%llu write_sectors=%llu write_ms=%llu flush_calls=%llu flush_ms=%llu\n",
		(unsigned long long)vs.read_calls,
		(unsigned long long)vs.read_sectors,
		(unsigned long long)(timer_ns_from_ticks(vs.read_ticks) / 1000000ULL),
		(unsigned long long)vs.write_calls,
		(unsigned long long)vs.write_sectors,
		(unsigned long long)(timer_ns_from_ticks(vs.write_ticks) / 1000000ULL),
		(unsigned long long)vs.flush_calls,
		(unsigned long long)(timer_ns_from_ticks(vs.flush_ticks) / 1000000ULL));
	printf("[io] dosfs pread=%llu requested=%llu served=%llu head_bounce=%llu tail_bounce=%llu direct_reads=%llu direct_sectors=%llu cache_hits=%llu cache_misses=%llu\n",
		(unsigned long long)ds.pread_calls,
		(unsigned long long)ds.requested_bytes,
		(unsigned long long)ds.served_bytes,
		(unsigned long long)ds.head_bounce_reads,
		(unsigned long long)ds.tail_bounce_reads,
		(unsigned long long)ds.direct_read_calls,
		(unsigned long long)ds.direct_sectors,
		(unsigned long long)ds.cache_hits,
		(unsigned long long)ds.cache_misses);
	for (i = 0; i < EXT_COUNT; ++i) {
		file_ext_stat *s = &g_ext_stats[i];
		if (!s->opens && !s->reads && !s->bytes) continue;
		printf("[io] ext=%s opens=%llu reads=%llu bytes=%llu ms=%llu\n",
			ext_name(i),
			(unsigned long long)s->opens,
			(unsigned long long)s->reads,
			(unsigned long long)s->bytes,
			(unsigned long long)(timer_ns_from_ticks(s->ticks) / 1000000ULL));
	}
}

static void list_noop_cb(const char *base, int is_dir, void *user)
{
	(void)base;
	(void)is_dir;
	(void)user;
}

/* ---- SMP-safe heap growth + newlib malloc-arena lock ----
 *
 * Worker harts (dos_thread.c band-parallel raster) run engine code and can
 * allocate, so both _sbrk and newlib's malloc arena must tolerate concurrent
 * callers across harts. This gcc16 newlib uses classic (non-retargetable)
 * locking: every malloc/free/realloc is wrapped in __malloc_lock/__malloc_unlock,
 * whose stock versions in libc.a are no-ops. We override them (our objects link
 * before -lc, so the strong symbols win) with a recursive hart spinlock keyed on
 * the hartid stashed in sscratch by start.S. Recursive so an IRQ that allocates
 * on a hart already inside malloc cannot self-deadlock — IRQ entry now returns
 * via sret. _sbrk additionally grows the linker heap window with a lock-free CAS
 * so any non-malloc caller stays correct without taking the arena lock. */
struct _reent;   /* forward decl: the hooks only need the (ignored) pointer */

static inline unsigned long heap_hartid(void)
{
	unsigned long h;
	__asm__ __volatile__("csrr %0, sscratch" : "=r"(h));
	return h;
}

static inline void heap_pause(void)
{
	bm_pause();
}

static volatile long heap_lock_owner = -1; /* hartid holding the lock, -1 = free */
static int           heap_lock_depth;      /* recursion depth, owner-hart only    */

void __malloc_lock(struct _reent *reent)
{
	(void)reent;
	long self = (long)heap_hartid();
	if (__atomic_load_n(&heap_lock_owner, __ATOMIC_RELAXED) == self) {
		++heap_lock_depth;          /* re-entrant: owner only, no atomics needed */
		return;
	}
	for (;;) {
		long expected = -1;
		if (__atomic_compare_exchange_n(&heap_lock_owner, &expected, self,
		                                0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
			break;
		heap_pause();
	}
	heap_lock_depth = 1;
}

void __malloc_unlock(struct _reent *reent)
{
	(void)reent;
	if (--heap_lock_depth > 0)
		return;                     /* still nested on the owner hart */
	__atomic_store_n(&heap_lock_owner, -1, __ATOMIC_RELEASE);
}

static char *volatile g_heap_cur;

/* Top of the heap arena.
 *
 * The linker script stops the RAM window at 1920 MiB so that every SYMBOL stays
 * within medany's +/-2 GiB AUIPC reach. That constraint is real, but it applies
 * to symbol addressing -- not to an arena discovered at run time and handed out
 * as ordinary pointers. With -m 4G, RAM runs to 0x180000000 while __heap_end
 * sits at 0xF8200000, so honouring the link-time limit threw away 2176 MiB of
 * real memory on a machine whose map loads are memory-hungry.
 *
 * The DTB can only ever move this limit DOWN. That is the whole rule, and both
 * halves of it were wrong before:
 *
 *  1. It never clamped. On a machine with less DRAM than the 1920 MiB link
 *     window, ram_end() < __heap_end, the old code returned __heap_end
 *     unchanged, and _sbrk handed out hundreds of MiB past the end of physical
 *     memory -- faulting far from the cause, or silently aliasing on a part
 *     that wraps. Clamping turns that into an honest ENOMEM at the overrun.
 *
 *  2. It DID extend, and extending is unsound. __heap_end is not an arbitrary
 *     cap, it IS __stack_bottom: the 8 MiB boot stack lives immediately above
 *     it, and on a part with a framebuffer carved from the top of DRAM that
 *     aperture is up there too. _sbrk bounds the break by [__heap_start, limit]
 *     and nothing else -- sbrk cannot express a hole -- so any limit above
 *     __heap_end puts the stack inside the heap arena. That was true on every
 *     path, including QEMU; it merely had 2.44x of margin instead of 1.24x and
 *     so never fired. "Reclaim the RAM past the link window" was the stated
 *     reason, but it was never a measured need: peak demand is 750 MiB against
 *     a 1832 MiB linked heap (riscv/docs/scalar-rv64imafdc.md).
 *
 * So: extend never, clamp when the DTB says DRAM stops sooner. If a workload
 * ever genuinely needs more than the linked window, the honest knob is
 * LENGTH(RAM) in engine.ld -- a deliberate act, bounded by medany's +/-2 GiB
 * reach -- not an accident of where firmware happened to leave the DTB.
 *
 * NOTE: this cannot rescue the STACK. If DRAM ends below __stack_top the stack
 * is outside physical memory and no heap policy can fix it -- the image has to
 * be relinked for that machine. bm_report_ram_window() says so out loud. */
static char *dos_heap_limit(void)
{
	static char *cached;
	char *lim = __atomic_load_n(&cached, __ATOMIC_RELAXED);

	if (!lim) {
		uint64_t rend = ram_end();
		lim = __heap_end;
		/* rend > __heap_start: a DTB window exists (both accessors are 0
		 * without one) and leaves at least some room to allocate in.
		 * ram_base() <= __heap_start: that window really is the bank the
		 * image was loaded into. Given both, DRAM's end is the truth --
		 * but only DOWNWARD. See the note above on why it never wins
		 * upward. */
		if (rend > (uint64_t)(uintptr_t)__heap_start &&
		    ram_base() <= (uint64_t)(uintptr_t)__heap_start &&
		    rend < (uint64_t)(uintptr_t)__heap_end)
			lim = (char *)(uintptr_t)rend;
		__atomic_store_n(&cached, lim, __ATOMIC_RELAXED);
	}
	return lim;
}

/* Boot report: the physical RAM window the DTB describes, against the one this
 * image was linked for. Silence here used to be the whole problem -- a machine
 * smaller than the link window looked identical to a matching one until
 * something faulted deep inside an allocation. */
void bm_report_ram_window(void)
{
	uint64_t rbase = ram_base(), rend = ram_end();
	unsigned long heap_mb = (unsigned long)(dos_heap_limit() - __heap_start) >> 20;

	if (!rend) {
		printf("[doomos-mem] no usable DTB memory node; using the linked window "
		       "(heap %luMB, stack top %p)\n", heap_mb, (void *)__stack_top);
		return;
	}
	/* "usable", not "dram": ram_end() is already clamped below the DTB blob, so
	 * this is the window the allocator may use, NOT the part's physical extent.
	 * Printing the blob address next to it makes a heap lost to a low DTB
	 * diagnosable from one line instead of inferred. */
	printf("[doomos-mem] usable=%#llx..%#llx (%lluMB)  dtb=%#llx  linked stack_top=%p  heap=%luMB%s\n",
	       (unsigned long long)rbase, (unsigned long long)rend,
	       (unsigned long long)((rend - rbase) >> 20), (unsigned long long)boot_dtb,
	       (void *)__stack_top, heap_mb,
	       (rend < (uint64_t)(uintptr_t)__heap_end) ? "  (heap CLAMPED below DTB/DRAM end)" : "");

	/* The heap now follows DRAM, but the stack is linked above the heap and
	 * cannot be moved at run time. If it lands outside DRAM the image is simply
	 * built for a bigger machine -- fail loudly rather than at a random depth. */
	if ((uint64_t)(uintptr_t)__stack_top > rend)
		printf("*** WARNING: linked stack top %p is ABOVE DRAM end %#llx by %lluMB.\n"
		       "    This image is linked for a larger machine and WILL fault in the\n"
		       "    stack. Relink engine.ld's RAM LENGTH to fit this part.\n",
		       (void *)__stack_top, (unsigned long long)rend,
		       (unsigned long long)(((uint64_t)(uintptr_t)__stack_top - rend) >> 20));
}

unsigned long dos_heap_committed_bytes(void)
{
	char *p = __atomic_load_n(&g_heap_cur, __ATOMIC_RELAXED);
	return p ? (unsigned long)(p - __heap_start) : 0;
}

unsigned long dos_heap_capacity_bytes(void)
{
	return (unsigned long)(dos_heap_limit() - __heap_start);
}

void *_sbrk(int incr)
{

	for (;;) {
		char *old = __atomic_load_n(&g_heap_cur, __ATOMIC_RELAXED);
		if (old == 0) {
			char *init = 0;
			if (!__atomic_compare_exchange_n(&g_heap_cur, &init, __heap_start,
			                                 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
				continue;           /* another hart initialised cur; reload */
			old = __heap_start;
		}
		char *neu = old + incr;     /* incr may be negative (newlib heap shrink) */
		if (neu < __heap_start || neu > dos_heap_limit()) { errno = ENOMEM; return (void *)-1; }
		if (__atomic_compare_exchange_n(&g_heap_cur, &old, neu,
		                                0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
			return old;
		/* lost the CAS to a concurrent hart; retry with reloaded old */
	}
}

/* ---- newlib syscalls ---- *
 * File I/O is served by the flat read-only dosfs over virtio-blk. Every engine
 * file path (idFile_Permanent stdio + minizip fopen64/fseeko64) funnels through
 * these reentrant bottom-ends, so a dosfs-backed _open/_read/_lseek/_close/
 * _fstat is sufficient to list + open + read the base/ pk4s and default.cfg.
 * Read-only: O_WRONLY/O_CREAT return EROFS so config/save writes degrade. */
int  _write(int file, char *ptr, int len)
{
	dos_fd *fd;
	int64_t n;
	if (file < DOS_FD_BASE) {
		sbi_console_write(ptr, (unsigned long)len);
		return len;
	}
	if (file - DOS_FD_BASE >= DOS_MAX_FD) { errno = EBADF; return -1; }
	fd = &g_fds[file - DOS_FD_BASE];
	if (fd->kind != FD_RAMFS) { errno = EBADF; return -1; }
	n = dos_ramfs_write(&fd->rw, ptr, (uint64_t)(unsigned)len);
	if (n < 0) { errno = ENOSPC; return -1; }
	fd->dirty = 1;   /* mark for savefs commit on close */
	return (int)n;
}

int  _open(const char *p, int f, int m)
{
	int i;
	unsigned char bucket = ext_bucket_for_path(p);
	(void)m;
	int writable = ((f & O_ACCMODE) == O_WRONLY || (f & O_ACCMODE) == O_RDWR);
	for (i = 0; i < DOS_MAX_FD; ++i) if (g_fds[i].kind == FD_FREE) break;
	if (i == DOS_MAX_FD) { errno = EMFILE; return -1; }

	if (writable || (f & O_CREAT)) {
		if (dos_ramfs_open(p, f, &g_fds[i].rw) != 0) { errno = ENOSPC; return -1; }
		g_fds[i].kind = FD_RAMFS;
		g_fds[i].dirty = 0;
		g_fds[i].ext_bucket = bucket;
		g_ext_stats[bucket].opens++;
		return DOS_FD_BASE + i;
	}

	if (dos_ramfs_open(p, f, &g_fds[i].rw) == 0) {
		g_fds[i].kind = FD_RAMFS;
		g_fds[i].dirty = 0;
		g_fds[i].ext_bucket = bucket;
		g_ext_stats[bucket].opens++;
		return DOS_FD_BASE + i;
	}

	if (!dosfs_mounted()) { errno = ENOENT; return -1; }
	if (dosfs_open(p, &g_fds[i].ro) != 0) { errno = ENOENT; return -1; }
	g_fds[i].kind = FD_DOSFS;
	g_fds[i].ext_bucket = bucket;
	g_ext_stats[bucket].opens++;
	return DOS_FD_BASE + i;
}

int  _read(int file, char *ptr, int len)
{
	dosfs_file *f;
	dos_fd *fd;
	int64_t n;
	uint64_t t0;
	if (file < DOS_FD_BASE) { (void)ptr; (void)len; return 0; }   /* stdin: EOF */
	if (file - DOS_FD_BASE >= DOS_MAX_FD) { errno = EBADF; return -1; }
	fd = &g_fds[file - DOS_FD_BASE];
	t0 = timer_ticks();
	if (fd->kind == FD_RAMFS) {
		n = dos_ramfs_read(&fd->rw, ptr, (uint64_t)(unsigned)len);
		if (n < 0) { errno = EBADF; return -1; }
		g_ext_stats[fd->ext_bucket].reads++;
		g_ext_stats[fd->ext_bucket].bytes += (uint64_t)n;
		g_ext_stats[fd->ext_bucket].ticks += timer_ticks() - t0;
		return (int)n;
	}
	if (fd->kind != FD_DOSFS) { errno = EBADF; return -1; }
	f = &fd->ro;
	n = dosfs_read(f, ptr, (uint64_t)(unsigned)len);
	if (n < 0) { errno = EIO; return -1; }
	g_ext_stats[fd->ext_bucket].reads++;
	g_ext_stats[fd->ext_bucket].bytes += (uint64_t)n;
	g_ext_stats[fd->ext_bucket].ticks += timer_ticks() - t0;
	return (int)n;
}

int  _close(int file)
{
	dosfs_file *f;
	dos_fd *fd;
	if (file < DOS_FD_BASE) return 0;
	if (file - DOS_FD_BASE >= DOS_MAX_FD) { errno = EBADF; return -1; }
	fd = &g_fds[file - DOS_FD_BASE];
	if (fd->kind == FD_RAMFS) {
		/* persist the file before the live handle drops: snapshot its current
		 * bytes into the journal. Only a writable fd that was actually written. */
		if (savefs_active() && fd->rw.writable && fd->dirty) {
			const char *pth = dos_ramfs_path(&fd->rw);
			if (pth) (void)savefs_commit_file(pth);
		}
		if (dos_ramfs_close(&fd->rw) != 0) { errno = EBADF; return -1; }
		fd->kind = FD_FREE;
		return 0;
	}
	if (fd->kind != FD_DOSFS) { errno = EBADF; return -1; }
	f = &fd->ro;
	if (!f->used) { errno = EBADF; return -1; }
	f->used = 0;
	fd->kind = FD_FREE;
	return 0;
}

/* 64-bit _off_t (LP64): minizip's fseeko64/ftello64 funnel here; the 483 MB pak
 * and any offset within the image must not truncate. */
_off_t _lseek(int file, _off_t ptr, int dir)
{
	dosfs_file *f;
	dos_fd *fd;
	int64_t pos;
	if (file < DOS_FD_BASE) { (void)ptr; (void)dir; return 0; }
	if (file - DOS_FD_BASE >= DOS_MAX_FD) { errno = EBADF; return -1; }
	fd = &g_fds[file - DOS_FD_BASE];
	if (fd->kind == FD_RAMFS) {
		pos = dos_ramfs_seek(&fd->rw, (int64_t)ptr, dir);
		if (pos < 0) { errno = EINVAL; return -1; }
		return (_off_t)pos;
	}
	if (fd->kind != FD_DOSFS) { errno = EBADF; return -1; }
	f = &fd->ro;
	if (!f->used) { errno = EBADF; return -1; }
	pos = dosfs_seek(f, (int64_t)ptr, dir);
	if (pos < 0) { errno = EINVAL; return -1; }
	return (_off_t)pos;
}

int  _isatty(int file)                    { return (file < DOS_FD_BASE) ? 1 : 0; }

int  _fstat(int file, struct stat *st)
{
	dosfs_file *f;
	dos_fd *fd;
	uint64_t sz;
	uint32_t mt;
	if (file < DOS_FD_BASE) { st->st_mode = S_IFCHR; return 0; }  /* console */
	if (file - DOS_FD_BASE >= DOS_MAX_FD) { errno = EBADF; return -1; }
	fd = &g_fds[file - DOS_FD_BASE];
	if (fd->kind == FD_RAMFS) {
		if (dos_ramfs_fstat(&fd->rw, &sz, &mt) != 0) { errno = EBADF; return -1; }
		st->st_mode = S_IFREG;
		st->st_size = (off_t)sz;
		st->st_mtime = (time_t)mt;
		return 0;
	}
	if (fd->kind != FD_DOSFS) { errno = EBADF; return -1; }
	f = &fd->ro;
	if (!f->used) { errno = EBADF; return -1; }
	st->st_mode  = S_IFREG;
	st->st_size  = (off_t)f->size;
	st->st_mtime = (time_t)dosfs_build_epoch();
	return 0;
}

int  _stat(const char *p, struct stat *st)
{
	int64_t sz;
	uint64_t rsz;
	uint32_t mt;
	int is_dir;
	if (dos_ramfs_stat(p, &rsz, &is_dir, &mt) == 0) {
		st->st_mode = is_dir ? S_IFDIR : S_IFREG;
		st->st_size = (off_t)rsz;
		st->st_mtime = (time_t)mt;
		return 0;
	}
	if (!dosfs_mounted()) { errno = ENOENT; return -1; }
	sz = dosfs_size(p);
	if (sz < 0) {
		if (dosfs_list(p, "", list_noop_cb, 0) >= 0) {
			st->st_mode = S_IFDIR;
			st->st_size = 0;
			st->st_mtime = (time_t)dosfs_build_epoch();
			return 0;
		}
		errno = ENOENT;
		return -1;
	}
	st->st_mode  = S_IFREG;
	st->st_size  = (off_t)sz;
	st->st_mtime = (time_t)dosfs_build_epoch();
	return 0;
}
int  _link(const char *o, const char *n)  { (void)o; (void)n; errno = EMLINK; return -1; }
int  _unlink(const char *p)
{
	if (dos_ramfs_unlink(p) == 0) {
		if (savefs_active()) (void)savefs_commit_delete(p);   /* tombstone */
		return 0;
	}
	if (dosfs_mounted() && dosfs_size(p) >= 0) { errno = EROFS; return -1; }
	errno = ENOENT;
	return -1;
}
int unlink(const char *p) { return _unlink(p); }
int mkdir(const char *p, mode_t mode)
{
	(void)mode;
	if (dos_ramfs_mkdir(p) == 0) return 0;
	errno = ENOSPC;
	return -1;
}
int rmdir(const char *p) { (void)p; errno = ENOTEMPTY; return -1; }
int rename(const char *oldp, const char *newp)
{
	if (dos_ramfs_rename(oldp, newp) == 0) return 0;
	if (dosfs_mounted() && dosfs_size(oldp) >= 0) { errno = EROFS; return -1; }
	errno = ENOENT;
	return -1;
}
int  _kill(int pid, int sig)              { (void)pid; (void)sig; errno = EINVAL; return -1; }
int  _getpid(void)                        { return 1; }
void _exit(int code)                      { (void)code; sbi_shutdown(); for (;;) { } }

clock_t _times(struct tms *buf)
{
	uint64_t t = timer_ticks();
	clock_t c = (clock_t)(t / (timer_hz() / 100UL)); /* 100 Hz CLK_TCK-ish */
	if (buf) { buf->tms_utime = c; buf->tms_stime = 0; buf->tms_cutime = 0; buf->tms_cstime = 0; }
	return c;
}

int _gettimeofday(struct timeval *tv, void *tz)
{
	(void)tz;
	if (tv) {
		uint64_t t = timer_ticks();
		unsigned long hz = timer_hz();
		tv->tv_sec  = (long)(t / hz);
		tv->tv_usec = (long)(((t % hz) * 1000000UL) / hz);
	}
	return 0;
}

/* Zihintpause hint — yields the pipeline without parking the hart (mirrors
 * BM_CpuPause in sys_dos.cpp). We must NOT use `wfi` here: no timer interrupt is
 * programmed (see Sys_SleepUntilPrecise's TODO), so wfi would halt the hart
 * until an interrupt that never arrives, hanging the engine (e.g. the s_restart
 * Sys_Sleep during session init). Busy-poll rdtime instead. */
static inline void bm_cpu_pause(void)
{
	bm_pause();
}

/* SDL's unix timer uses select(0,...,timeout) as its sleep: bounded busy-wait on
 * rdtime until the timeout elapses; returns 0 (timeout). */
int select(int nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *to)
{
	(void)nfds; (void)r; (void)w; (void)e;
	if (to) {
		uint64_t usec  = (uint64_t)to->tv_sec * 1000000UL + (uint64_t)to->tv_usec;
		uint64_t ticks = timer_ticks_from_us(usec);
		uint64_t end   = timer_ticks() + ticks;
		while (timer_ticks() < end) bm_cpu_pause();
	}
	return 0;
}

/* ---- gl4es / SDL env ---- */
char *getenv(const char *name)
{
	if (!name) return 0;
	if (!strcmp(name, "LIBGL_NOPSA"))    return "1";  /* no shader-binary cache (no file I/O) */
	if (!strcmp(name, "LIBGL_NOBANNER")) return "1";  /* quieter boot log */
	if (!strcmp(name, "LIBGL_ES"))       return "2";  /* GLES2 backend */
	if (!strcmp(name, "LIBGL_AVOID16BITS")) return "1"; /* preserve RGB8/RGBA8 uploads */
	return 0;
}
char *getcwd(char *buf, size_t size) { if (buf && size >= 2) { buf[0] = '/'; buf[1] = 0; return buf; } return 0; }

/* ---- POSIX functions SDL/engine reference but newlib only declares ---- */
#include <signal.h>
int  sigaction(int sig, const struct sigaction *act, struct sigaction *old) { (void)sig; (void)act; (void)old; return 0; }
long sysconf(int name)              { (void)name; return -1; }
int  getpagesize(void)             { return 4096; }
int  gethostname(char *n, size_t l) { if (n && l) n[0] = 0; return 0; }
int  fdatasync(int fd)             { (void)fd; return 0; }
int  fsync(int fd)                 { (void)fd; return 0; }
unsigned int sleep(unsigned int s) { (void)s; return 0; }
int  usleep(unsigned int us)
{
	uint64_t end = timer_ticks() + timer_ticks_from_us((uint64_t)us);
	while (timer_ticks() < end) bm_cpu_pause();
	return 0;
}

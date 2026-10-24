/*
 * newlib syscall + OS-stub layer for the bare-metal gl4es bridge payload.
 *
 * The bridge payload links gl4es + newlib libc/libm (gl4es uses printf/malloc/
 * qsort/sin/... heavily), so we supply newlib's bottom-end syscalls routed to
 * SBI/our heap, plus the few OS-ish functions gl4es calls that bare metal lacks:
 *   - getenv: hardcode LIBGL flags (no shader-cache file I/O, GLES2, quiet)
 *   - getcwd: "/"     - _open: fail  -> fopen() returns NULL -> shader cache off
 *   - _gettimeofday: from the rdtime CSR
 */
#include <sys/stat.h>
#include <sys/time.h>
#include <errno.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include "boot/runtime.h"

extern char __heap_start[], __heap_end[];

void *_sbrk(int incr)
{
    static char *cur = 0;
    if (!cur) cur = __heap_start;
    char *prev = cur;
    if (cur + incr > __heap_end) { errno = ENOMEM; return (void *)-1; }
    cur += incr;
    return prev;
}

int _write(int file, char *ptr, int len)
{
    (void)file;
    /* DBCN bulk write (1 ecall/chunk); falls back to per-char legacy putchar.
     * Essential: per-char legacy SBI putchar is ~1.6 ms/char under QEMU TCG. */
    sbi_console_write(ptr, (unsigned long)len);
    return len;
}

int  _read(int file, char *ptr, int len)   { (void)file; (void)ptr; (void)len; return 0; }
int  _open(const char *p, int f, int m)     { (void)p; (void)f; (void)m; errno = ENOSYS; return -1; }
int  _close(int file)                       { (void)file; return -1; }
int  _lseek(int file, int ptr, int dir)     { (void)file; (void)ptr; (void)dir; return 0; }
int  _isatty(int file)                      { (void)file; return 1; }
int  _fstat(int file, struct stat *st)      { (void)file; st->st_mode = S_IFCHR; return 0; }
int  _kill(int pid, int sig)                { (void)pid; (void)sig; errno = EINVAL; return -1; }
int  _getpid(void)                          { return 1; }
void _exit(int code)                        { (void)code; sbi_shutdown(); for (;;) { } }

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

/* ---- gl4es OS-ish stubs ---- */
char *getenv(const char *name)
{
    if (!name) return 0;
    if (!strcmp(name, "LIBGL_NOPSA"))    return "1";   /* no shader-binary cache (no file I/O) */
    if (!strcmp(name, "LIBGL_NOBANNER")) return "1";   /* quieter boot log */
    if (!strcmp(name, "LIBGL_ES"))       return "2";   /* GLES2 backend */
    return 0;
}

char *getcwd(char *buf, size_t size)
{
    if (buf && size >= 2) { buf[0] = '/'; buf[1] = 0; return buf; }
    return 0;
}

/*
 * dOS bottom-end: the minimal OS surface SDL3 + newlib need on bare metal.
 *
 * newlib syscalls routed to SBI/our heap, plus the two functions SDL's unix
 * timer/time backends call directly: gettimeofday (rdtime CSR) and select
 * (used as SDL's sleep primitive — stubbed to a bounded WFI spin for now).
 * Links with libSDL3-riscv.a + newlib libc/libm.
 */
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/select.h>
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

int  _write(int file, char *ptr, int len) { (void)file; sbi_console_write(ptr, (unsigned long)len); return len; }
int  _read(int file, char *ptr, int len)  { (void)file; (void)ptr; (void)len; return 0; }
int  _open(const char *p, int f, int m)    { (void)p; (void)f; (void)m; errno = ENOSYS; return -1; }
int  _close(int file)                      { (void)file; return -1; }
int  _lseek(int file, int ptr, int dir)    { (void)file; (void)ptr; (void)dir; return 0; }
int  _isatty(int file)                     { (void)file; return 1; }
int  _fstat(int file, struct stat *st)     { (void)file; st->st_mode = S_IFCHR; return 0; }
int  _kill(int pid, int sig)               { (void)pid; (void)sig; errno = EINVAL; return -1; }
int  _getpid(void)                         { return 1; }
void _exit(int code)                       { (void)code; sbi_shutdown(); for (;;) { } }

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

/* SDL's unix timer uses select(0,...,timeout) as its sleep; returns 0 (timeout).
 *
 * This used to be `while (timer_ticks() < end) asm volatile("wfi");` with
 * sie.STIE clear and no timer armed. `wfi` only resumes on an interrupt ENABLED
 * in sie, so on any hart without SEIE that halts PERMANENTLY -- a latent hang
 * waiting for an interrupt that was never going to arrive. bm_wait_until() arms
 * the timer (Sstc stimecmp where available) before parking, which is what makes
 * the wake guaranteed. */
int select(int nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *to)
{
    (void)nfds; (void)r; (void)w; (void)e;
    if (to) {
        uint64_t usec = (uint64_t)to->tv_sec * 1000000UL + (uint64_t)to->tv_usec;
        bm_wait_until(timer_ticks() + timer_ticks_from_us(usec));
    }
    return 0;
}

/* gl4es / SDL env: keep quiet, no shader cache, force GLES2 */
char *getenv(const char *name)
{
    if (!name) return 0;
    if (!strcmp(name, "LIBGL_NOPSA"))    return "1";
    if (!strcmp(name, "LIBGL_NOBANNER")) return "1";
    if (!strcmp(name, "LIBGL_ES"))       return "2";
    return 0;
}
char *getcwd(char *buf, size_t size) { if (buf && size >= 2) { buf[0] = '/'; buf[1] = 0; return buf; } return 0; }

/* POSIX functions SDL references but newlib only declares (no bare-metal impl). */
#include <signal.h>
int  sigaction(int sig, const struct sigaction *act, struct sigaction *old) { (void)sig; (void)act; (void)old; return 0; }
long sysconf(int name)            { (void)name; return -1; }
int  getpagesize(void)            { return 4096; }
int  gethostname(char *n, size_t l) { if (n && l) n[0] = 0; return 0; }
int  fdatasync(int fd)            { (void)fd; return 0; }

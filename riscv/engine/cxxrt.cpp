/*
 * cxxrt.cpp — minimal C++ runtime glue for the bare-metal dhewm3 engine.
 *
 * With -nostdlib we link libsupc++ + libgcc (the DWARF unwinder lives in this
 * toolchain's libgcc.a — there is NO separate libgcc_eh.a for newlib gcc16, see
 * riscv/docs/design/31-engine-crossbuild-status.md). libsupc++ provides
 * __cxa_throw / __cxa_allocate_exception / __cxa_begin_catch / std::terminate /
 * __gxx_personality_v0; libgcc provides _Unwind_*. What is NOT in either, and
 * what we therefore supply here:
 *
 *   - operator new / new[] / delete / delete[]  (+ sized + nothrow)  -> malloc/free
 *   - __cxa_pure_virtual / __cxa_deleted_virtual                     -> abort/panic
 *   - __cxa_guard_acquire / release / abort   (single-hart plain byte guards)
 *   - __cxa_atexit / __dso_handle             (no-op; we sbi_shutdown, never exit)
 *   - __verbose_terminate_handler override    (route to panic, avoid demangler)
 *
 * Build flags (toolchain): -fno-rtti -fno-threadsafe-statics, EXCEPTIONS ON.
 * -fno-threadsafe-statics means GCC would normally NOT emit __cxa_guard_*; we
 * still provide them so an externally-compiled TU (e.g. a prebuilt lib member or
 * a TU that re-enables threadsafe statics) links cleanly.
 */

#include <stddef.h>
#include <stdlib.h>   /* malloc / free / abort (newlib) */

extern "C" {
#include "boot/runtime.h"   /* panic() -> sbi_shutdown (noreturn) */
}

/* ---- operator new / delete --------------------------------------------------
 * idLib overrides global new/delete in some configs (ID_REDIRECT_NEWDELETE),
 * but the default engine build does NOT, so the C++ runtime expects the standard
 * global operators. Route them to malloc/free (which back onto the linker heap
 * window via _sbrk in the dOS syscall layer). On OOM we panic rather than throw
 * std::bad_alloc (the engine treats allocation failure as fatal anyway). */

void *operator new(size_t n)
{
	if (n == 0) n = 1;
	void *p = malloc(n);
	if (!p) panic("operator new: out of memory");
	return p;
}

void *operator new[](size_t n)
{
	return operator new(n);
}

void operator delete(void *p)        throw() { free(p); }
void operator delete[](void *p)      throw() { free(p); }

/* C++14 sized-deallocation forms (emitted by gcc16 in C++14+ mode). */
void operator delete(void *p,   size_t) throw() { free(p); }
void operator delete[](void *p, size_t) throw() { free(p); }

/* nothrow forms (placement-style; used by some library code). */
namespace std { struct nothrow_t; }
void *operator new(size_t n,   const std::nothrow_t &) throw() { return malloc(n ? n : 1); }
void *operator new[](size_t n, const std::nothrow_t &) throw() { return malloc(n ? n : 1); }
void operator delete(void *p,   const std::nothrow_t &) throw() { free(p); }
void operator delete[](void *p, const std::nothrow_t &) throw() { free(p); }

/* ---- C++ ABI helpers (Itanium ABI, used by libsupc++ / generated code) ---- */
extern "C" {

/* Called when a pure-virtual / deleted-virtual function is invoked: a bug. */
void __cxa_pure_virtual(void)    { panic("__cxa_pure_virtual"); }
void __cxa_deleted_virtual(void) { panic("__cxa_deleted_virtual"); }

/*
 * Function-local static initialisation guards. Single cooperative hart, so a
 * plain byte is sufficient (no atomics / no recursion handling needed).
 * On RV64 the Itanium ABI guard is a 64-bit object; the low byte is the
 * "initialised" flag, byte 1 is the "in progress" flag we use for the trivial
 * recursion guard (unused here but kept correct).
 */
int  __cxa_guard_acquire(long long *g)
{
	char *b = (char *)g;
	return !b[0];        /* 1 => caller should run the initialiser */
}
void __cxa_guard_release(long long *g) { ((char *)g)[0] = 1; }
void __cxa_guard_abort(long long *g)   { (void)g; }

/*
 * Static-destructor registration. We never run an orderly shutdown (Sys_Quit /
 * Sys_Error go straight to sbi_shutdown), so registering destructors is a no-op.
 * __dso_handle is the per-DSO cookie passed to __cxa_atexit; one is enough.
 */
void *__dso_handle = 0;
int  __cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
	(void)fn; (void)arg; (void)dso;
	return 0;
}
/* plain C atexit() too, in case a libc TU references it */
int atexit(void (*fn)(void)) { (void)fn; return 0; }

} // extern "C"

/*
 * Override libstdc++'s verbose terminate handler so an uncaught exception (or
 * std::terminate) panics with a clear message instead of pulling in the name
 * demangler / writing to a missing stderr. Matches the engine's "errors are
 * fatal" model. Defined in namespace __gnu_cxx as the real symbol.
 */
namespace __gnu_cxx {
	void __verbose_terminate_handler();
	void __verbose_terminate_handler()
	{
		panic("C++ terminate: uncaught exception");
	}
}

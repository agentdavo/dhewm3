/*
 * Minimal DoomOS hart-backed thread primitive.
 *
 * This is intentionally narrower than a scheduler: it starts one cooperative
 * engine function on an OpenSBI HSM secondary hart and lets callers join it.
 */
#ifndef DHEWM3_DOS_THREAD_H
#define DHEWM3_DOS_THREAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*dos_thread_fn)(void *);

void          dos_threads_init(void);
void         *dos_thread_create(dos_thread_fn fn, void *arg, const char *name);
void          dos_thread_join(void *handle);
int           dos_thread_is_handle(const void *handle);
unsigned long dos_thread_hartid(const void *handle);
unsigned long dos_thread_self_id(void);
int           dos_thread_is_worker(void);

/* 1 (and prints which worker) if any worker stack has run past its low end into
 * its poisoned red zone. There is no MMU guard page here, so this is how a stack
 * overflow becomes visible instead of silently corrupting the neighbouring
 * worker's stack. */
int           dos_thread_stack_overflowed(void);

/* Secondary-hart entry point implemented in dos_hart_entry.S. */
void          dos_hart_entry(void);
void          dos_thread_entry_c(void *opaque);

#ifdef __cplusplus
}
#endif

#endif /* DHEWM3_DOS_THREAD_H */

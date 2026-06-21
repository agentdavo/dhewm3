/*
 * Freestanding stub <dlfcn.h> for the bare-metal gl4es build.
 *
 * gl4es's loader.h includes <dlfcn.h> unconditionally on non-Windows, even with
 * NO_LOADER (where the dlopen/dlsym calls are compiled out). newlib has no
 * dlfcn.h, so this satisfies the include with inert inline stubs — no link
 * symbols, no OS dependency. Placed on the gl4es riscv build's -I path.
 */
#ifndef SGL_FREESTANDING_DLFCN_H
#define SGL_FREESTANDING_DLFCN_H

#define RTLD_LAZY    0x0001
#define RTLD_NOW     0x0002
#define RTLD_LOCAL   0x0000
#define RTLD_GLOBAL  0x0100
#define RTLD_DEFAULT ((void *)0)
#define RTLD_NEXT    ((void *)-1L)

static inline void       *dlopen(const char *file, int mode) { (void)file; (void)mode; return (void *)0; }
static inline void       *dlsym(void *handle, const char *name) { (void)handle; (void)name; return (void *)0; }
static inline int         dlclose(void *handle) { (void)handle; return 0; }
static inline const char *dlerror(void) { return (const char *)0; }

#endif /* SGL_FREESTANDING_DLFCN_H */

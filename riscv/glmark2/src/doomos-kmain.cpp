extern "C" int main(int argc, char **argv);

#include <stdio.h>

extern "C" {
#include "engine/dos_virtio_blk.h"
#include "engine/dosfs.h"
#include "engine/dos_thread.h"
}

#ifndef GLMARK2_DOOMOS_SIZE
#define GLMARK2_DOOMOS_SIZE "320x240"
#endif

#ifndef GLMARK2_DOOMOS_BENCHMARK
#define GLMARK2_DOOMOS_BENCHMARK "build:use-vbo=false:duration=1"
#endif

/* Split BENCHMARK string by '|' into multiple argv entries:
 *   "build:use-vbo=false|texture:filter=nearest"
 * expands to:
 *   --benchmark "build:use-vbo=false" --benchmark "texture:filter=nearest"
 * The compile-time GLMARK2_DOOMOS_BENCHMARK string may contain '|' separators. */
static int split_benchmarks(const char *src, char **entries, int max)
{
    int n = 0;
    const char *p = src;
    while (*p && n < max) {
        entries[n++] = (char *)p;
        while (*p && *p != '|') p++;
        if (*p == '|') {
            *(char *)p = '\0';
            p++;
        }
    }
    return n;
}

extern "C" void
kmain(void)
{
    static char arg0[] = "glmark2-es2-doomos";
    static char arg1[] = "--size";
    static char arg2[] = GLMARK2_DOOMOS_SIZE;
    static char arg3[] = "--benchmark";
    static char bench_buf[] = GLMARK2_DOOMOS_BENCHMARK;
    static char *bench_entries[64];
    static char *argv_buf[2 + 2 * 64 + 1]; /* arg0, --size, SIZE, (--benchmark BENCH)*N, NULL */
    int argc;
    char **argv;

    if (bench_buf[0] == '\0') {
        /* No benchmarks passed: use glmark2's built-in default list */
        argv_buf[0] = arg0; argv_buf[1] = arg1; argv_buf[2] = arg2; argv_buf[3] = nullptr;
        argc = 3; argv = argv_buf;
        printf("[glmark2] kmain: entering main (default benchmarks)\n");
    } else {
        int nb = split_benchmarks(bench_buf, bench_entries, 64);
        int i = 0;
        argv_buf[i++] = arg0;
        argv_buf[i++] = arg1;
        argv_buf[i++] = arg2;
        for (int j = 0; j < nb && i < (int)(sizeof(argv_buf)/sizeof(argv_buf[0]) - 1); j++) {
            argv_buf[i++] = arg3;  /* --benchmark */
            argv_buf[i++] = bench_entries[j];
        }
        argv_buf[i] = nullptr;
        argc = i; argv = argv_buf;
        printf("[glmark2] kmain: entering main (%d benchmark(s): ", nb);
        for (int j = 0; j < nb; j++)
            printf("%s%s", j ? " | " : "", bench_entries[j]);
        printf(")\n");
    }

    if (vblk_init() == 0 && dosfs_init(&vblk_read) == 0) {
        printf("[glmark2] dosfs: mounted %d files\n", dosfs_entry_count());
    } else {
        printf("[glmark2] dosfs: mount failed\n");
    }
    dos_threads_init();
    (void) main(argc, argv);
    printf("[glmark2] kmain: main returned\n");
}

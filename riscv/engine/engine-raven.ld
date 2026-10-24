/*
 * engine-raven.ld — engine.ld retargeted for the Lumen Raven SoC.
 *
 * EXACTLY ONE LINE DIFFERS from engine.ld: the MEMORY LENGTH. Everything below
 * is a byte-for-byte copy and must stay that way — the script already derives
 * __stack_top, __stack_bottom and __heap_end from ORIGIN(RAM) + LENGTH(RAM),
 * so retargeting genuinely is one number and a second difference is a mistake.
 * If engine.ld changes, regenerate and restore this header:
 *
 *     sed 's/LENGTH = 1920M/LENGTH = 1020M/' engine.ld > engine-raven.ld
 *
 * A copy that drifts silently is the hazard this note exists to prevent.
 *
 * WHY 1020 MiB — derived, not chosen:
 *
 *   Raven DRAM               0x80000000 .. 0xc0000000   1 GiB
 *   Firmware / DTB reserved  0x80000000 .. 0x80200000   payload base, as D1
 *   Framebuffer aperture     top of DRAM, 2 MiB reserved
 *   Engine window            0x80200000 .. 0xbfe00000 = 1020 MiB
 *
 *   1 GiB is a STRUCTURAL ceiling, not a tunable: a DRAM channel's beat index
 *   is u24, so a channel caps at 128 MiB and eight cap at exactly 1 GiB
 *   (lumen: src/lib/lumen/raven-memory-map.ts). Raising it means widening that
 *   index across the fabric, which no link script justifies.
 *
 *   The framebuffer is carved from the TOP of DRAM and its size follows the
 *   declared display mode (lumen: ravenFramebufferRange()).
 *
 *   THIS RESERVATION DEPENDS ON  RAVEN_BOOT_MODE = "VGA"  AND BREAKS WITHOUT IT.
 *
 *   The aperture is page_round_up(width * height * 4), so it scales with the
 *   mode, and 2 MiB is a margin over VGA rather than a bound on anything:
 *
 *       VGA      640x480    0x12C000   1.17 MiB   fits
 *       SVGA     800x600    0x1D5000   1.83 MiB   barely fits
 *       XGA     1024x768    0x300000   3.00 MiB   DOES NOT FIT
 *       1080p   1920x1080   0x7E9000   7.91 MiB   no
 *       4K UHD  3840x2160  0x1FA4000  31.64 MiB   no
 *
 *   At XGA or above this window overlaps the aperture and the display eats the
 *   top of the heap. That presents as memory corruption, not as a mode change,
 *   which is why the dependency is named here rather than left implicit. A .ld
 *   file cannot call ravenFramebufferRange() to derive the number; if this ever
 *   moves into a generated script, derive it and delete this table.
 *
 *   (The 0x200000 in lumen's mmio.ts is NOT this number. That is the decode
 *   aperture of a separate ISS framebuffer *device* with its own backing store,
 *   which is not in DRAM at all. Two framebuffers, two addresses, one name —
 *   mmio.ts says so at the entry. Do not reconcile them; they are not the same
 *   object.)
 *
 * Verified by linking against this script rather than by arithmetic:
 *
 *   __stack_top     0xbfe00000     ORIGIN + LENGTH
 *   __stack_bottom  0xbf600000     __stack_top - STACK_SIZE (8 MiB)
 *   __heap_end      0xbf600000
 *   __heap_start    0x85154380     __bss_end of the scalar build
 *
 *   Heap 933 MiB + stack 8 MiB = 941 MiB usable, against the 1841 MiB the
 *   QEMU link offers. Roughly half.
 *
 * DOES IT FIT? YES — booted, not calculated. 2026-09-21, at QEMU -m 1G, which
 * is the faithful Raven DRAM condition rather than arithmetic about it:
 * **72 world frames rendered with ZERO allocation failures** on this window.
 *
 * The prior projection agreed, for the record: allocator high-water is 750 MiB
 * converged over 109 frames, against 933 MiB here — 183 MiB spare, 80%
 * utilised. But a boot at the real size is the evidence, and the projection
 * was only ever a reason to try it.
 *
 *   Two things keep that from being a comfortable number. The measurement ran
 *   with RENDER_PROF=1, which defaults bm_benchVantage/bm_benchFreeze to 1, so
 *   the camera sits at the scenic vantage rather than spawn: biased UP, so
 *   conservative, but not the plain-spawn figure. And it is one map.
 *
 *   Read the PLATEAU, not the first line. The first [rprof-heap] emitted is
 *   ~200 MiB, during loading-screen frames before texture upload; the stages
 *   run 200 -> 279 -> 557 -> 750 MiB. A historical 210 MiB figure in
 *   engine-baseline.log is probably that same trap.
 *
 * THE DEVICE TREE HAS TO BE PLACED DELIBERATELY, and this is the constraint
 * most likely to be missed. ram_init_from_dtb() clamps ram_end() below the DTB
 * when the blob sits inside the declared bank, so a low DTB costs heap
 * one-for-one — and at 80% utilisation there is nothing to absorb it.
 *
 * Use  mksdcard.py --fdt-addr 0xbfe00000  for a Raven image. An earlier version
 * of this header said to follow the existing `fdt move` pattern, which was
 * WRONG: boot.scr's default is `fdt move ${fdtcontroladdr} 0x100000000`, and
 * 4 GiB does not exist on a 1 GiB part, so `fdt move` fails outright and the
 * SD path could never have booted on Raven. 0xbfe00000 is the 848 KiB gap
 * between __stack_top and the framebuffer aperture — above the heap, so it
 * costs none of it, and below the aperture, so it collides with nothing.
 * Validated end to end 2026-09-21; QEMU at -m 1G independently places the DTB
 * at exactly this address unprompted.
 *
 * OPEN, AND NOT THIS FILE'S TO SETTLE: what /memory should declare. If it
 * declares the full 1 GiB then ram_end() = 0xc0000000 > __heap_end, the EXTEND
 * branch in dos_heap_limit() fires, and the limit becomes 0xc0000000 — which
 * spans the 8 MiB boot stack at [0xbf600000,0xbfe00000) AND the framebuffer
 * aperture at 0xbfed4000. _sbrk bounds only `< __heap_start` and `> limit`; it
 * has no stack exclusion, and sbrk cannot express a hole. At 750 MiB demand the
 * 183 MiB margin means it does not arise in practice, but the reported capacity
 * would be wrong and a larger map would reach it. The same shape exists on the
 * QEMU direct-boot path with a 2.4x margin instead of a thin one. Raised with
 * the runtime's owner rather than worked around here.
 *
 * engine.ld's own lineage follows, retained so the two can be diffed.
 *
 * A copy of riscv/boot/linker.ld with two load-bearing changes for the full
 * engine (see riscv/docs/design/30-engine-bringup.md §4 and §5):
 *
 *   1. RAM LENGTH raised from 192 MiB to 3584 MiB. The engine + idLib + game +
 *      SDL3 + gl4es + softgles2 image is far bigger than the boot smoke tests,
 *      and real assets need a large heap. Run QEMU with -m 4G. (DTB parsing in a
 *      later phase replaces this static cap with the discovered RAM size.)
 *
 *   2. KEEP .eh_frame / .eh_frame_hdr (boot/linker.ld DISCARDs them). Exceptions
 *      are ON: common->Error() throws idException caught in Frame()/Init()
 *      (framework/Common.cpp), so the DWARF unwinder needs these sections.
 *      __eh_frame_hdr_start/end are provided in case a startup __register_frame()
 *      is needed (see 31-engine-crossbuild-status.md "exceptions" risk).
 *
 * Loaded by QEMU's -kernel at 0x80200000 (OpenSBI hands off to S-mode there).
 */

OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
    /*
     * RAM window. Kept just under 2 GiB so that ALL symbols (notably __stack_top
     * at the very top and the high heap) stay within the +/-2 GiB reach of the
     * -mcmodel=medany PC-relative (AUIPC) addressing used by the engine code and
     * the boot CRT. A 3584M window overflows this (R_RISCV_PCREL_HI20 truncation
     * on __stack_top/__heap_end). 1920 MiB gives a comfortable margin and is far
     * larger than the 192 MiB boot window; run QEMU with -m 2G (or more).
     * A later phase that parses the DTB + uses an explicit GOT/medium+ model can
     * lift this; for the cross-build milestone 1920 MiB is plenty.
     */
    RAM (rwxa) : ORIGIN = 0x80200000, LENGTH = 1020M
}

PHDRS
{
    text  PT_LOAD      FLAGS(5);  /* PF_R | PF_X */
    data  PT_LOAD      FLAGS(6);  /* PF_R | PF_W */
    tls   PT_TLS       FLAGS(4);  /* PF_R */
    stack PT_GNU_STACK FLAGS(6);  /* PF_R | PF_W */
}

STACK_SIZE = 8M;   /* engine uses _alloca() heavily; matches desktop 8 MB stack */

SECTIONS
{
    .text : ALIGN(4K)
    {
        KEEP(*(.text.init))      /* _start must be first */
        *(.text .text.*)
    } > RAM :text

    .rodata : ALIGN(8)
    {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    } > RAM :text

    /* C++ exception unwind tables — KEPT (exceptions are on). */
    .eh_frame_hdr : ALIGN(8)
    {
        PROVIDE(__eh_frame_hdr_start = .);
        *(.eh_frame_hdr)
        PROVIDE(__eh_frame_hdr_end = .);
    } > RAM :text
    .eh_frame : ALIGN(8)
    {
        PROVIDE(__eh_frame_start = .);
        KEEP(*(.eh_frame))
        PROVIDE(__eh_frame_end = .);
    } > RAM :text
    .gcc_except_table : ALIGN(8)
    {
        *(.gcc_except_table .gcc_except_table.*)
    } > RAM :text

    .init_array ALIGN(4K) : ALIGN(8)
    {
        PROVIDE(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE(__init_array_end = .);
    } > RAM :data

    /* .fini_array is unused (we sbi_shutdown(), never exit()), but keep it so
     * libstdc++ / __cxa_atexit registrations have a defined home if emitted. */
    .fini_array : ALIGN(8)
    {
        PROVIDE(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array))
        PROVIDE(__fini_array_end = .);
    } > RAM :data

    .data ALIGN(4K) : ALIGN(8)
    {
        *(.data .data.*)
        *(.sdata .sdata.*)
        PROVIDE(__global_pointer$ = . + 0x800);
    } > RAM :data

    /* Thread-local storage. tp is set to __tls_base in start.S; .tdata is the
     * (loaded) initial image, .tbss is zeroed by _cstart. SDL3 + gl4es use TLS. */
    .tdata : ALIGN(64)
    {
        PROVIDE(__tls_base = .);
        *(.tdata .tdata.* .gnu.linkonce.td.*)
        PROVIDE(__tdata_end = .);
    } > RAM :data :tls
    .tbss (NOLOAD) : ALIGN(16)
    {
        PROVIDE(__tbss_start = .);
        *(.tbss .tbss.* .gnu.linkonce.tb.* .tcommon)
        PROVIDE(__tbss_end = .);
    } > RAM :data :tls

    /* Main-hart TLS is used in place, rather than copied by an OS loader. */
    .bss ALIGN(__tbss_end, 4K) (NOLOAD) : ALIGN(16)
    {
        PROVIDE(__bss_start = .);
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        PROVIDE(__bss_end = .);
    } > RAM :data

    PROVIDE(end          = __bss_end);
    PROVIDE(__heap_start = __bss_end);

    PROVIDE(__stack_top    = ORIGIN(RAM) + LENGTH(RAM));
    PROVIDE(__stack_bottom = __stack_top - STACK_SIZE);
    PROVIDE(__heap_end     = __stack_bottom);

    /DISCARD/ : { *(.comment) *(.note .note.*) }
}

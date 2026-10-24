/*
 * engine.ld — dhewm3 ENGINE bare-metal RISC-V64 linker script (Path B).
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
    RAM (rwxa) : ORIGIN = 0x80200000, LENGTH = 1920M
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

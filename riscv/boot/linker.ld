/*
 * dhewm3 RISC-V64 bare-metal linker script.
 *
 * Loaded by QEMU's `-kernel` at 0x80200000, which is where OpenSBI (the
 * default M-mode firmware on the `virt` machine) hands control to the next
 * stage in S-mode. RAM on `virt` starts at 0x80000000; OpenSBI lives in the
 * low region, so the payload is placed at 0x80200000.
 *
 * The window is intentionally conservative (192 MiB) so it is valid even with
 * a modest `-m 256M`. The full engine target raises this and detects real RAM
 * size from the device tree (see ROADMAP.md, Phase 4).
 */

OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
    RAM (rwxa) : ORIGIN = 0x80200000, LENGTH = 192M
}

PHDRS
{
    text  PT_LOAD      FLAGS(5);  /* PF_R | PF_X */
    data  PT_LOAD      FLAGS(6);  /* PF_R | PF_W */
    tls   PT_TLS       FLAGS(4);  /* PF_R */
    stack PT_GNU_STACK FLAGS(6);  /* PF_R | PF_W */
}

STACK_SIZE = 256K;

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
        *(.eh_frame_hdr)
        KEEP(*(.eh_frame))
        *(.gcc_except_table .gcc_except_table.*)
    } > RAM :text

    .init_array ALIGN(4K) : ALIGN(8)
    {
        PROVIDE(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE(__init_array_end = .);
    } > RAM :data

    .data ALIGN(4K) : ALIGN(8)
    {
        *(.data .data.*)
        *(.sdata .sdata.*)
        PROVIDE(__global_pointer$ = . + 0x800);
    } > RAM :data

    /* Thread-local storage. tp is set to __tls_base in start.S; .tdata is the
     * (loaded) initial image, .tbss is zeroed by _cstart. Required because
     * gl4es uses THREAD_LOCAL guards in its proc loader. */
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

    /* Unlike an OS loader, the boot hart uses the TLS image in place. Reserve
     * TBSS physically: ELF TLS NOBITS otherwise need not advance normal BSS. */
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

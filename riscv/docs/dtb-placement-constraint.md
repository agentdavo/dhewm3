# Platform constraint: DTB placement relative to the guest heap

**Applies to:** any RISC-V target booting a DoomOS-class image that sizes its
heap from the device tree. Not DoomOS-specific — it is a property of where
firmware leaves the blob.

## The constraint

**The device tree blob must be placed above the guest's `__heap_end`.**

A DTB placed inside the heap's address range is subtracted from the heap
one-for-one, silently, at boot.

## Why

`ram_init_from_dtb()` (`riscv/boot/runtime.c`) discovers the DRAM bank from
`/memory`, then deliberately clamps the usable end below the blob:

```c
if (boot_dtb >= base && boot_dtb < base + size)
    g_ram_end = boot_dtb & ~(uint64_t)0xFFFFu;   /* stay below the blob */
```

That clamp is correct — overwriting the blob would take the timebase and hart
list with it. But `ram_end()` is also what bounds the heap, so wherever firmware
drops the DTB becomes the ceiling. A blob at, say, `0x9fe00000` on a part whose
heap starts at `0x85154380` costs 100% of everything above it: the heap stops
429 MiB in, regardless of how much DRAM the part actually has.

The cost is invisible. There is no error — the allocator simply reports a
smaller capacity, and the first symptom is an out-of-memory failure during map
load on a machine that has plenty of RAM.

## The fix

Relocate the blob above the linked window before handing off. The reference
implementation is the U-Boot script in `riscv/image/mksdcard.py`:

```
fdt addr ${fdtcontroladdr}
fdt move ${fdtcontroladdr} 0x100000000 0x10000
setenv fdt_high 0xffffffffffffffff
bootm 0x84000000 - 0x100000000
```

`0x100000000` is above the entire 1920 MiB link window, so `ram_end()` is never
clamped into the heap. On a part with less physical DRAM than that, place the
blob in the gap between `__heap_end` and the top of DRAM — the boot stack and
any framebuffer aperture already live there and the heap is barred from it.

## Checking it

The image prints its view at boot:

```
[doomos-mem] dram=0x80000000..0x100000000 (2048MB)  linked stack_top=0xf8200000  heap=1832MB
```

`heap=` is the arena the allocator will actually hand out. If it is materially
smaller than `__heap_end - __heap_start`, the DTB is sitting in the heap's way.
A `(heap CLAMPED to DRAM)` suffix means DRAM itself, not the blob, is the limit.

## Why it matters more on a small part

On a 1 GiB part the budget has no slack to absorb this. Measured demand for
`demo_mars_city1` at 768x432 is 750 MiB of heap on top of an 81.3 MiB static
image — 831 MiB of 1024, about 81% — leaving roughly 183 MiB spare against a
1020 MiB link window. A DTB landing 200 MiB low does not reduce the margin, it
removes it.

(That 750 MiB is the converged high-water across 109 rendered frames. Do not use
the first `[rprof-heap]` sample: it is emitted during loading-screen frames,
reads ~200 MiB, and is not a measurement of the series.)

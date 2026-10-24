# Physical hardware contract

The target matches QEMU `virt,acpi=off`, with four RVA23S64 harts and 4 GiB
of RAM. The current game build uses lp64d, medany, VLEN=1024 and additional
ISA extensions selected by `riscv/toolchain/baremetal-rva23s64.cmake` and
`riscv/run-win.sh`.
RVA23S64 alone does not imply those additional requirements. Keep the CPU
configuration alongside the device tree, since the tree is not a complete
description of vector configuration or attached virtio devices.

The runtime discovers usable harts from the DTB and OpenSBI HSM. Four harts
is a launch setting, not a separate engine build. The Windows launcher and
demorun default to four, matching the existing Linux launcher.

## Export the device tree on Windows

Run from MSYS2 UCRT64 in the repository root:

```sh
VIDEO=gpu DTB_DUMP="$PWD/build-rv/virt-rva23s64-4harts.dtb" riscv/run-win.sh
dtc -I dtb -O dts -o build-rv/virt-rva23s64-4harts.dts build-rv/virt-rva23s64-4harts.dtb
```

Export with the same `DISK`, `VIDEO`, `MEM`, `SMP` and CPU overrides as the
intended launch. DTB_DUMP exits before booting. The initial exported tree on
2026-09-20 used QEMU 11.1.1, VIDEO=gpu and no asset disk, and contains CPU
nodes 0 through 3. Virtio MMIO nodes describe transports; drivers identify
the attached device through registers. A future disk attachment must be
recorded in the launch configuration as well.

## Boot and storage

The combined DoomOS/game ELF remains RAM-linked at 0x80200000. Direct QEMU
`-kernel` loading does not validate physical ROM, DDR initialization or
loading the executable from SD/flash. Booting from SD into RAM can preserve
this ELF layout; true XIP needs linker and startup changes.

Assets currently use the custom dosfs format over virtio-blk. A physical
implementation needs the same guest-visible interface or a driver for the
chosen SD controller. The default virt configuration does not select that
SD controller for us.

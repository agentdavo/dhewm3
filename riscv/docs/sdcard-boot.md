# DoomOS disk boot on QEMU virt

The new disk is `build-rv/sdcard/doomos-sdcard.img`. Unlike the older
`doom-truecolour.img` asset disk, it contains the executable as well as assets
and the persistent save journal. `build-rv/sdcard/doomos-firmware.bin` is the
separate OpenSBI 1.8.1 + U-Boot 2025.01 firmware bundle.

Boot chain: QEMU reset ROM → OpenSBI → U-Boot → disk `boot.scr` → disk
`DOOMOS.UIM` → DoomOS/game. The launcher passes **no `-kernel` or `-initrd`**.
OpenSBI stays resident to provide SBI services. U-Boot relinquishes control
before the game starts. No Linux kernel or userspace is involved.

## Run on Windows

From the repository root:

```powershell
C:/Python314/python.exe riscv/scripts/sdcard_boot.py
```

This opens a GTK QEMU window: four RVA23S64 harts, VLEN 1024, the optional
extensions required by the shipping game, and 4 GiB RAM. The default disposable
snapshot protects the delivered image. Add `--persist` to retain saves/config
in the image. Close the window or press Ctrl-C in the launching terminal to exit.
`--display sdl` selects SDL instead. `--image` selects a different image.

```powershell
C:/Python314/python.exe riscv/scripts/sdcard_boot.py --check
```

The headless check waits for the first map frame, checks the disk-boot markers,
and shuts down its QEMU process. Logs, the exact QEMU command, hashes and result
are saved under `build-rv/sdcard/check/`. The first loading-screen present is
distinct from the first map frame. Host-observed marker timings include
firmware; DoomOS `[boot-time]` totals start in the DoomOS runtime.

## Rebuild

Build the game with the existing Windows build workflow first. Then:

```powershell
wsl -d Ubuntu -- bash riscv/image/build-boot-firmware.sh
C:/Python314/python.exe riscv/image/mksdcard.py
```

The firmware build uses Ubuntu WSL's existing host compiler, Make, DTC and
`riscv64-linux-gnu-` cross compiler. Missing bison/flex/m4 packages are downloaded
and extracted locally; the script does not install system packages. Sources,
objects, dependency files and temporary files stay in `build/toolchain/`.
GNU Make with `--jobserver-style=pipe` is used because WSL's Windows-mounted
filesystem does not support the default FIFO jobserver. The game remains built
with the repository's Windows GCC 16 medany toolchain.

Firmware source archives are version- and hash-pinned. U-Boot deliberately has
SMP disabled: it leaves secondary harts stopped in OpenSBI, allowing DoomOS's
existing SBI HSM startup to own all four harts. This does not disable game SMP.
U-Boot autoboot has zero delay. Its fixed boot command reads `boot.scr` from
virtio disk 0, partition 1; it does not require saved firmware environment.

The Python image builder needs only the standard library. It packages ELF
PT_LOAD file bytes into a CRC-protected U-Boot legacy kernel image, with load
and entry address `0x80200000`. DoomOS's existing CRT clears BSS/TBSS. The script
uses U-Boot's kernel handoff ABI for the hart ID and firmware-generated DTB; it
does not turn the payload into Linux. The DTB is placed above the game's static
heap/stack region before handoff.

`mksdcard.py --assets PATH --elf PATH --out PATH` selects inputs/output.
The default assets input is `build-rv/doom-truecolour.img`. Existing savefs
contents are copied verbatim; rebuilds do not incorporate saves subsequently
written to an older output image. A manifest records input and image SHA-256s.

## Layout and compatibility

| Region | Start sector | Size/type |
|---|---:|---|
| MBR | 0 | Standard 512-byte partition table |
| Boot partition | 2048 | 64 MiB FAT16, type `0x0e`, active |
| Assets/save partition | 133120 | Existing DOSFS image, type `0xda` |

The boot FAT contains `BOOT.SCR` and `DOOMOS.UIM`. The second partition holds
the original DOSFS and its appended savefs journal. DoomOS selects the unique
type-`0xda` primary partition, validates its bounds and magic, rejects overlap,
and applies its offset to both reads and writes. Filesystem-relative offsets
and savefs format are unchanged. Old unpartitioned DOSFS disks still work.

## Physical SD scope

This is an SD-style raw disk layout, tested through QEMU virt's **virtio-blk**
transport. It is not a board-specific boot-ROM image. The eventual physical
board needs compatible first-stage firmware/OpenSBI, a suitable U-Boot build
or equivalent loader, and a DoomOS driver for its actual SD controller.
Neither DDR initialization nor physical SD electrical/controller behavior is
validated by this QEMU test. Do not assume that writing this file to a card
makes an unspecified RISC-V board boot.

The firmware-on-flash, game-and-assets-on-SD arrangement is deliberate. QEMU
loads the separate firmware bundle to emulate the firmware already present on
a board; the executable and assets are fetched from the disk image itself.

Reference: [QEMU virt boot options](https://www.qemu.org/docs/master/system/riscv/virt.html)
and [OpenSBI QEMU virt platform](https://github.com/riscv-software-src/opensbi/blob/v1.8.1/docs/platform/qemu_virt.md).

## Validation on 2026-09-21

The delivered disk is 619,708,416 bytes (591 MiB). The adjacent `README.txt`
summarizes its layout and launch instructions; `SHA256SUMS.txt` records the
delivered disk, firmware and matching debug ELF hashes.

- Two firmware-only cold boots reached the first map frame. The final firmware
  run passed checksum, partition mount, savefs, VLEN 1024 and four-render-hart
  checks. Its logs are in `build-rv/sdcard/persistence-check/`; the first run is
  in `build-rv/sdcard/check/`. Each has the exact QEMU arguments and image hashes.
- Final firmware run: disk handoff observed at 0.43 s, DOSFS mount at 0.64 s,
  map-ready at 50.32 s, first map frame at 153.80 s from host process launch.
  These are functional-run timings, not a controlled comparison with direct
  ELF boot. Initial game/render work still dominates (103.52 s from map-ready
  to first map frame); do not interpret the disk-boot change as a performance win.
- `game.png` in each check directory shows the rendered Mars City hangar at
  768x432. This validates visible output, not temporal tearing behavior.
- Independent `fsck.fat -n` accepted the boot volume. `mcopy` extracted the
  executable; both U-Boot CRCs passed and its payload matched the ELF load bytes.
- Twelve host C tests passed for legacy media, valid MBR, bounds, overflow,
  duplicate DOSFS partitions and overlaps.
- A persistent test copy advanced from 50 to 58 valid savefs records. A full
  byte comparison found 275,472 changed bytes, all inside the savefs journal;
  MBR, FAT boot partition and read-only assets were unchanged. The host savefs
  reader validated the records, and a fresh boot replayed all 58 successfully.
  The delivered image itself retains the original journal and hash.
- A direct-ELF regression smoke boot still mounted the old unpartitioned image
  at volume LBA 0. Both short smoke results are in
  `build-rv/sdcard/replay-and-legacy.json`. All test QEMU guests were shut down.

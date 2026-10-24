#!/usr/bin/env python3
"""Create a QEMU-virt DoomOS SD-style disk: MBR, FAT16 boot, DOSFS/savefs.

Pure Python; no mounted filesystem, drive access, or root privileges required.
The OpenSBI/U-Boot firmware is separate, as it would be in board boot flash.
"""
import argparse
import binascii
import hashlib
import json
from pathlib import Path
import shutil
import struct

ROOT = Path(__file__).resolve().parents[2]
SECTOR = 512
BOOT_LBA = 2048
BOOT_SECTORS = 131072  # 64 MiB FAT16
DATA_LBA = BOOT_LBA + BOOT_SECTORS


def sha256(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def elf_payload(path):
    elf = path.read_bytes()
    if elf[:7] != b'\x7fELF\x02\x01\x01' or struct.unpack_from('<HH', elf, 16) != (2, 243):
        raise ValueError('Expected a little-endian RISC-V ELF64 executable')
    entry, phoff = struct.unpack_from('<QQ', elf, 24)
    phsize, phnum = struct.unpack_from('<HH', elf, 54)
    if phsize != 56 or phoff + phsize * phnum > len(elf):
        raise ValueError('Invalid ELF program headers')
    segments = []
    for i in range(phnum):
        typ, flags, off, va, pa, size, mem, align = struct.unpack_from('<IIQQQQQQ', elf, phoff + i * phsize)
        if typ != 1:
            continue
        if va != pa or size > mem or off + size > len(elf) or pa < 0x80200000 or pa + mem > 0xf0000000:
            raise ValueError('Unsupported ELF load segment')
        segments.append((pa, elf[off:off + size], mem, flags))
    if not segments or min(s[0] for s in segments) != 0x80200000 or entry != 0x80200000:
        raise ValueError('Expected DoomOS entry/load address 0x80200000')
    end = max(pa + len(data) for pa, data, _, _ in segments)
    if end - entry > 32 * 1024 * 1024:
        raise ValueError('Payload exceeds reserved boot staging area')
    result = bytearray(end - entry)
    for pa, data, _, _ in segments:
        result[pa - entry:pa - entry + len(data)] = data
    # DoomOS _cstart clears BSS/TBSS; don't inflate the disk with those zeros.
    return bytes(result), entry


def uimage(data, name, kind, load=0, entry=0):
    header = bytearray(struct.pack('>7I4B32s', 0x27051956, 0, 0, len(data), load,
                                  entry, binascii.crc32(data), 5, 26, kind, 0,
                                  name.encode('ascii')[:32]))
    struct.pack_into('>I', header, 4, binascii.crc32(header))
    return bytes(header) + data


def fat16(files):
    """Small fixed FAT16 volume, mirrored FATs, root 8.3 files only."""
    volume = bytearray(BOOT_SECTORS * SECTOR)
    volume[:3] = b'\xeb\x3c\x90'
    volume[3:11] = b'DOOMOS  '
    struct.pack_into('<HBHBHHBHHHII', volume, 11, 512, 4, 1, 2, 512, 0,
                     0xf8, 128, 63, 255, BOOT_LBA, BOOT_SECTORS)
    struct.pack_into('<BBB I 11s 8s', volume, 36, 0x80, 0, 0x29, 0x444f4f4d,
                     b'DOOMOS BOOT', b'FAT16   ')
    volume[510:512] = b'\x55\xaa'
    fat = bytearray(128 * SECTOR)
    struct.pack_into('<HH', fat, 0, 0xfff8, 0xffff)
    root = (1 + 2 * 128) * SECTOR
    data_start = root + 512 * 32
    volume[root:root + 11] = b'DOOMOS BOOT'
    volume[root + 11] = 0x08
    cluster = 2
    for n, (shortname, data) in enumerate(files):
        if len(shortname) != 11 or n >= 511:
            raise ValueError('Invalid FAT root entry')
        count = (len(data) + 2047) // 2048
        if not count or data_start + (cluster - 2 + count) * 2048 > len(volume):
            raise ValueError('Boot partition full or empty file')
        rec = root + (n + 1) * 32
        volume[rec:rec + 11] = shortname.encode('ascii')
        volume[rec + 11] = 0x20
        struct.pack_into('<H', volume, rec + 24, 0x21)  # 1980-01-01
        struct.pack_into('<HI', volume, rec + 26, cluster, len(data))
        for c in range(cluster, cluster + count):
            struct.pack_into('<H', fat, 2 * c, c + 1 if c + 1 < cluster + count else 0xffff)
        off = data_start + (cluster - 2) * 2048
        volume[off:off + len(data)] = data
        cluster += count
    volume[512:129 * 512] = fat
    volume[129 * 512:257 * 512] = fat
    return volume


def build(elf, assets, out, fdt_addr=0x100000000):
    if out.resolve() in (elf.resolve(), assets.resolve()):
        raise ValueError('Output must not overwrite an input')
    with assets.open('rb') as f:
        sb = f.read(512)
    if len(sb) != 512 or struct.unpack_from('<III', sb) != (0x53464f44, 1, 512):
        raise ValueError('Assets must be an existing unpartitioned DOSFS image')
    if assets.stat().st_size % 512:
        raise ValueError('DOSFS image is not sector aligned')
    data_sectors = assets.stat().st_size // 512
    if DATA_LBA + data_sectors >= 2**32:
        raise ValueError('Image exceeds MBR addressing')
    payload, entry = elf_payload(elf)
    kernel = uimage(payload, 'DoomOS riscv64', 2, entry, entry)
    # The DTB is moved clear of the guest's heap before handoff: ram_init_from_dtb()
    # clamps the usable RAM end below the blob, so a DTB left inside the heap's
    # range is subtracted from the heap one-for-one and silently
    # (riscv/docs/dtb-placement-constraint.md).
    #
    # 0x100000000 (4 GiB) suits QEMU virt at -m 4G because it is above the whole
    # 1920 MiB link window. It is NOT a universal target: on a part with less
    # physical DRAM that address does not exist and `fdt move` fails outright.
    # A 1 GiB part linked with engine-raven.ld wants the gap between __stack_top
    # (0xbfe00000) and the framebuffer aperture -- hence --fdt-addr.
    fdt = f'{fdt_addr:#x}'.encode('ascii')
    script = b'''echo [sdboot] Loading DoomOS from virtio 0:1
if load virtio 0:1 0x84000000 doomOS.uim; then
  fdt addr ${fdtcontroladdr}
  fdt move ${fdtcontroladdr} ''' + fdt + b''' 0x10000
  setenv fdt_high 0xffffffffffffffff
  echo [sdboot] Booting disk payload with firmware device tree
  bootm 0x84000000 - ''' + fdt + b'''
fi
echo [sdboot] FAILED
'''
    script_image = uimage(struct.pack('>II', len(script), 0) + script, 'DoomOS disk boot', 6)
    boot = fat16([('BOOT    SCR', script_image), ('DOOMOS  UIM', kernel)])
    mbr = bytearray(512)
    for i, (active, typ, start, count) in enumerate(((0x80, 0x0e, BOOT_LBA, BOOT_SECTORS),
                                                    (0, 0xda, DATA_LBA, data_sectors))):
        struct.pack_into('<B3sB3sII', mbr, 446 + i * 16, active, b'\xfe\xff\xff', typ,
                         b'\xfe\xff\xff', start, count)
    mbr[510:512] = b'\x55\xaa'
    out.parent.mkdir(parents=True, exist_ok=True)
    # Build a sibling temporary file then replace, so interrupted builds leave
    # the previous complete image intact. Existing input savefs is preserved.
    tmp = out.with_suffix(out.suffix + '.tmp')
    with tmp.open('wb') as f:
        f.write(mbr)
        f.seek(BOOT_LBA * 512)
        f.write(boot)
        f.seek(DATA_LBA * 512)
        with assets.open('rb') as source:
            shutil.copyfileobj(source, f, 1024 * 1024)
        f.truncate(((f.tell() + (1 << 20) - 1) // (1 << 20)) * (1 << 20))
    tmp.replace(out)
    manifest = dict(image=str(out.resolve()), image_bytes=out.stat().st_size,
                    image_sha256=sha256(out), elf=str(elf.resolve()), elf_sha256=sha256(elf),
                    assets=str(assets.resolve()), assets_sha256=sha256(assets),
                    payload_bytes=len(payload), entry=hex(entry),
                    partitions=[dict(type='FAT16', start_lba=BOOT_LBA, sectors=BOOT_SECTORS),
                                dict(type='DOSFS+savefs', start_lba=DATA_LBA, sectors=data_sectors)])
    out.with_suffix('.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (out.parent / 'README.txt').write_text(f'''DoomOS RISC-V64 disk image
=========================

{out.name}: {out.stat().st_size:,} bytes ({out.stat().st_size // (1 << 20)} MiB)
{out.with_suffix('.json').name}: image/input SHA-256 hashes and metadata.
doomos-firmware.bin: separate OpenSBI 1.8.1 + U-Boot 2025.01 firmware.

IMAGE LAYOUT (512-byte sectors)
  LBA 0: MBR partition table.
  Partition 1: LBA {BOOT_LBA}, {BOOT_SECTORS} sectors (64 MiB), FAT16,
               type 0x0e, active. BOOT.SCR loads DOOMOS.UIM.
  Partition 2: LBA {DATA_LBA}, {data_sectors} sectors ({data_sectors * 512:,} bytes),
               type 0xda. Existing DOSFS assets and savefs journal.
  The file ends with padding to a whole MiB.

BOOT: QEMU reset -> OpenSBI -> U-Boot -> disk script -> DoomOS/game.
Firmware is separate, as it would be in board boot flash. No Linux is used.
The executable comes from the disk, not a host-loaded game ELF.
Target: QEMU virt, 4 RVA23S64 harts, VLEN 1024, 4 GiB RAM, virtio-blk/GPU.
The launcher supplies the additional ISA extensions needed by this build.

Run from {ROOT}:
  python riscv/scripts/sdcard_boot.py --image "{out.resolve()}"

Default: visible QEMU window with disposable writes. Add --persist to keep
saves/config, or --check for a headless boot-through-first-map-frame check.
Close the window or press Ctrl-C to exit.

PHYSICAL SD: This SD-style layout is tested through QEMU virtio-blk. A board
still needs matching boot firmware and a DoomOS SD-controller driver. This is
not a universal board boot-ROM image; no physical card has been flashed.

Build/layout details: {ROOT / 'riscv/docs/sdcard-boot.md'}

Matching QEMU device-tree exports, when generated, are kept alongside this image:
virt-rva23s64-4harts.dtb (binary), virt-rva23s64-4harts.dts (readable), and
dtb-export-command.json (export arguments). These are before firmware fixups.
VLEN=1024 comes from the QEMU CPU arguments, not an explicit VLEN property.
''', encoding='utf-8')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--elf', type=Path, default=ROOT / 'build-rv/dhewm3')
    p.add_argument('--assets', type=Path, default=ROOT / 'build-rv/doom-truecolour.img')
    p.add_argument('--out', type=Path, default=ROOT / 'build-rv/sdcard/doomos-sdcard.img')
    p.add_argument('--fdt-addr', type=lambda v: int(v, 0), default=0x100000000,
                   help='where boot.scr relocates the DTB before bootm. Must be inside the '
                        'target\'s physical DRAM and ABOVE the guest __heap_end, or the heap '
                        'silently shrinks to it (see riscv/docs/dtb-placement-constraint.md). '
                        'Default 0x100000000 suits QEMU virt -m 4G; a 1 GiB part linked with '
                        'engine-raven.ld wants 0xbfe00000.')
    a = p.parse_args()
    build(a.elf, a.assets, a.out, a.fdt_addr)

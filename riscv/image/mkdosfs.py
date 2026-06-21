#!/usr/bin/env python3
# mkdosfs.py --root <staging> --out <doom.img>
#
# Emit a flat read-only "dosfs" image (see riscv/engine/dosfs_format.h) from a
# staging tree. Pure stdlib (struct, os) — no mtools, no mkfs, no root/loop.
#
# Layout (all LE, 512-byte sectors):
#   sector 0          : superblock
#   sector 1 ...      : dosfs_dirent[entry_count], padded to a sector edge
#   data_off ...      : concatenated file blobs, each sector-aligned
import os, struct, time, argparse, sys, binascii

SECTOR   = 512
MAGIC    = 0x53464F44          # 'D','O','F','S' little-endian
VERSION  = 1
PATH_MAX = 96
DIRENT   = 128                 # char path[96] + u64 offset + u64 size + pad
# superblock: magic,ver,sect,count, dir_off,data_off,image_bytes, epoch,flags
SUPER_FMT = "<IIIIQQQII"       # 48 bytes; reserved[] (the savefs region desc) follows

# --- savefs persistent write-region (see riscv/engine/savefs_format.h) ---
SAVEFS_REGION_MAGIC = 0x53465653   # 'S','V','F','S'
SAVEFS_JHDR_MAGIC   = 0x314A5653   # 'S','V','J','1'
SAVEFS_VERSION      = 1
REGION_DESC_OFF     = 48           # byte offset of the desc inside the superblock
REGION_DESC_FMT     = "<IIQII"     # region_magic,version,region_lba,region_sectors,flags (24B)
JHDR_FMT            = "<IIIIQQ"    # magic,version,sector_size,hdr_crc,region_sectors,epoch_base (32B)

def align(n, a=SECTOR):
    return (n + a - 1) // a * a

def walk(root):
    files = []  # (rel_lowercased, abs_path, size)
    for dirpath, _dirs, names in os.walk(root):
        for nm in names:
            ap  = os.path.join(dirpath, nm)
            rel = os.path.relpath(ap, root).replace(os.sep, "/").lower()
            if len(rel) + 1 > PATH_MAX:
                sys.exit(f"path too long ({len(rel)+1} > {PATH_MAX}): {rel}")
            files.append((rel, ap, os.path.getsize(ap)))
    files.sort(key=lambda f: f[0])   # ascending -> engine Sort() is a no-op
    return files

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--out",  required=True)
    ap.add_argument("--write-region-mib", type=int, default=64,
                    help="size of the appended savefs persistent write-region "
                         "(MiB, 0 = legacy image with no journal)")
    ap.add_argument("--quiet", action="store_true",
                    help="only print the image summary, not every staged file")
    a = ap.parse_args()

    files = walk(a.root)
    n     = len(files)
    if n == 0:
        sys.exit(f"no files under {a.root}")

    dir_off  = SECTOR                         # directory right after superblock
    data_off = align(dir_off + n * DIRENT)    # first blob, sector-aligned

    # assign each file a sector-aligned absolute offset
    entries, off = [], data_off
    for rel, _ap, sz in files:
        entries.append((rel, off, sz))
        off = align(off + sz)
    image_bytes = off
    epoch = int(time.time())

    # savefs write-region: appended after the read-only pack, on the same image.
    region_sectors = a.write_region_mib * (1 << 20) // SECTOR
    region_lba     = image_bytes // SECTOR
    have_region    = region_sectors >= 2

    with open(a.out, "wb") as f:
        # --- superblock (sector 0): 48-byte head + savefs region desc in reserved[] ---
        sb = bytearray(SECTOR)
        struct.pack_into(SUPER_FMT, sb, 0, MAGIC, VERSION, SECTOR, n,
                         dir_off, data_off, image_bytes, epoch, 1)
        if have_region:
            struct.pack_into(REGION_DESC_FMT, sb, REGION_DESC_OFF,
                             SAVEFS_REGION_MAGIC, SAVEFS_VERSION,
                             region_lba, region_sectors, 0)
        f.write(sb)
        # --- directory ---
        for rel, off_, sz in entries:
            p = rel.encode("ascii") + b"\x00"
            assert len(p) <= PATH_MAX
            rec = p + b"\x00" * (PATH_MAX - len(p)) + struct.pack("<QQ", off_, sz)
            assert len(rec) <= DIRENT
            f.write(rec + b"\x00" * (DIRENT - len(rec)))
        # --- pad to data_off, then stream each blob sector-aligned ---
        f.write(b"\x00" * (data_off - f.tell()))
        for (rel, off_, sz), (_r, src, _s) in zip(entries, files):
            assert f.tell() == off_, (f.tell(), off_, rel)
            with open(src, "rb") as g:
                while True:
                    chunk = g.read(1 << 20)
                    if not chunk:
                        break
                    f.write(chunk)
            pad = align(f.tell()) - f.tell()
            if pad:
                f.write(b"\x00" * pad)
        assert f.tell() == image_bytes, (f.tell(), image_bytes)

        # --- savefs write-region: jhdr @ region sector 0, then zeroed log area ---
        if have_region:
            assert f.tell() == region_lba * SECTOR
            jhdr = bytearray(SECTOR)
            struct.pack_into(JHDR_FMT, jhdr, 0,
                             SAVEFS_JHDR_MAGIC, SAVEFS_VERSION, SECTOR,
                             0,                       # hdr_crc placeholder
                             region_sectors, epoch)
            crc = binascii.crc32(jhdr) & 0xffffffff  # hdr_crc field is zero here
            struct.pack_into("<I", jhdr, 12, crc)
            f.write(jhdr)
            # zero the rest of the region (an empty log: replay stops at sector 1)
            remaining = (region_sectors - 1) * SECTOR
            CHUNK = 1 << 20
            zeros = b"\x00" * CHUNK
            while remaining > 0:
                w = CHUNK if remaining >= CHUNK else remaining
                f.write(zeros[:w])
                remaining -= w
        total = f.tell()

    print(f"dosfs: {n} files, dir_off={dir_off}, data_off={data_off}, "
          f"image={image_bytes} bytes ({image_bytes // (1<<20)} MiB)")
    if have_region:
        print(f"savefs: write-region {a.write_region_mib} MiB "
              f"(lba={region_lba}, {region_sectors} sectors); total image "
              f"{total} bytes ({total // (1<<20)} MiB)")
    else:
        print("savefs: no write-region (legacy image)")
    if not a.quiet:
        for rel, off_, sz in entries:
            print(f"  {rel:32s} off={off_:>12d} size={sz}")

if __name__ == "__main__":
    main()

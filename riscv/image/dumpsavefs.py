#!/usr/bin/env python3
# dumpsavefs.py --img doom.img [--extract DIR] [--quiet]
#
# Host-side reader/oracle for the savefs append-only journal (see
# riscv/engine/savefs_format.h). Mirrors the target's savefs.c verbatim: parse the
# region descriptor in the dosfs superblock reserved[], validate the journal
# header, scan records seq-order with the same stop-at-first-bad + CRC rule, reduce
# to the live set (latest PUT per path, minus DELETEs), and optionally extract.
#
# Pure stdlib — no QEMU, no loopback. Lets you pull config/saves off an image and
# diff them against what the engine wrote (the reboot-persistence test oracle).
import argparse, struct, binascii, os, sys

SECTOR              = 512
REGION_DESC_OFF     = 48
REGION_DESC_FMT     = "<IIQII"     # region_magic,version,region_lba,region_sectors,flags
JHDR_FMT            = "<IIIIQQ"    # magic,version,sector_size,hdr_crc,region_sectors,epoch_base
REC_FMT             = "<IIQIIII"   # magic,hdr_crc,seq,op,data_len,data_crc,path_len  (32B), then path[160]
REC_PATH_OFF        = 32
REC_PATH_MAX        = 160

SAVEFS_REGION_MAGIC = 0x53465653
SAVEFS_JHDR_MAGIC   = 0x314A5653
SAVEFS_REC_MAGIC    = 0x31525653
OP_PUT, OP_DELETE   = 1, 2


def crc_zeroing(sector, off):
    """CRC32 of a 512B sector with its 4-byte crc field at `off` zeroed."""
    b = bytearray(sector)
    b[off:off + 4] = b"\x00\x00\x00\x00"
    return binascii.crc32(bytes(b)) & 0xffffffff


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", required=True)
    ap.add_argument("--extract", help="write the live file set into this directory")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()

    f = open(a.img, "rb")
    super_sector = f.read(SECTOR)
    rm, ver, region_lba, region_sectors, _flags = struct.unpack_from(
        REGION_DESC_FMT, super_sector, REGION_DESC_OFF)
    if rm != SAVEFS_REGION_MAGIC:
        print("savefs: no write-region (legacy image)")
        return 0
    if ver != 1:
        sys.exit(f"savefs: unsupported region version {ver}")

    f.seek(region_lba * SECTOR)
    jhdr = f.read(SECTOR)
    jm, jver, jsec, jcrc, jregsec, jepoch = struct.unpack_from(JHDR_FMT, jhdr, 0)
    if jm != SAVEFS_JHDR_MAGIC:
        sys.exit("savefs: bad journal header magic")
    if crc_zeroing(jhdr, 12) != jcrc:
        sys.exit("savefs: journal header CRC mismatch")

    end_lba = region_lba + region_sectors
    lba = region_lba + 1
    seq = 1
    live = {}        # path -> bytes (latest PUT)
    nrec = 0
    while lba < end_lba:
        f.seek(lba * SECTOR)
        sect = f.read(SECTOR)
        if len(sect) < SECTOR:
            break
        magic, hdr_crc, rseq, op, data_len, data_crc, path_len = struct.unpack_from(REC_FMT, sect, 0)
        if magic != SAVEFS_REC_MAGIC:
            break
        if crc_zeroing(sect, 4) != hdr_crc:
            break
        if rseq != seq:
            break
        if path_len >= REC_PATH_MAX:
            break
        path = sect[REC_PATH_OFF:REC_PATH_OFF + path_len].decode("latin1")
        if op == OP_PUT:
            dsec = (data_len + SECTOR - 1) // SECTOR
            if lba + 1 + dsec > end_lba:
                break
            payload = b""
            if data_len > 0:
                f.seek((lba + 1) * SECTOR)
                payload = f.read(dsec * SECTOR)[:data_len]
                if (binascii.crc32(payload) & 0xffffffff) != data_crc:
                    break
            live[path] = payload
            lba += 1 + dsec
            seq += 1
            nrec += 1
        elif op == OP_DELETE:
            live.pop(path, None)
            lba += 1
            seq += 1
            nrec += 1
        else:
            break

    if not a.quiet:
        print(f"savefs: region lba={region_lba} sectors={region_sectors} "
              f"({region_sectors * SECTOR // (1 << 20)} MiB), epoch={jepoch}")
        print(f"savefs: {nrec} valid records, head at lba {lba} "
              f"({(end_lba - lba) * SECTOR // (1 << 20)} MiB free)")
        print(f"savefs: {len(live)} live files:")
        for p in sorted(live):
            print(f"  {p:48s} {len(live[p])} bytes")

    if a.extract:
        for p, data in live.items():
            dst = os.path.join(a.extract, p)
            os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
            with open(dst, "wb") as g:
                g.write(data)
        print(f"savefs: extracted {len(live)} files into {a.extract}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

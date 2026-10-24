#!/usr/bin/env bash
#
# mkimage.sh — build doom.img (a flat read-only dosfs) from base/.
#
# Stages exactly what the engine needs to clear "Couldn't load default.cfg":
#   base/demo00.pk4   (~483 MB)  the asset pack minizip reads (contains default.cfg)
#   base/*.cfg                   any loose config (gamepad.cfg, the 13B default.cfg stub)
# then invokes the pure-python packer (dd/mtools/root not required).
#
# Usage:   ./mkimage.sh
#          OUT=/tmp/doom.img STAGE=/tmp/stage ./mkimage.sh
# Boot:    DISK=$OUT ../run.sh ../../build-rv/dhewm3
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"          # riscv/image
PROJ="$(cd "$HERE/../.." && pwd)"              # repo root (dhewm3)
STAGE="${STAGE:-$HERE/staging}"
OUT="${OUT:-$PROJ/doom.img}"
BASE="${BASE:-$PROJ/base}"
# Size of the appended savefs persistent write-region (MiB; 0 = legacy/no journal).
WRITE_REGION_MIB="${WRITE_REGION_MIB:-64}"

[ -f "$BASE/demo00.pk4" ] || { echo "error: $BASE/demo00.pk4 not found"; exit 1; }

rm -rf "$STAGE"
mkdir -p "$STAGE/base"
cp -f "$BASE/demo00.pk4" "$STAGE/base/"
# loose .cfg files are optional; ignore the NTFS Zone.Identifier sidecar files.
# IMPORTANT: do NOT stage a loose base/default.cfg — directories are searched
# before paks (AddGameDirectory inserts paks AFTER the dir), so a loose
# default.cfg would shadow the real one inside demo00.pk4. The repo's
# base/default.cfg is only a 13-byte placeholder; the authoritative default.cfg
# lives inside demo00.pk4 and minizip extracts it.
for c in "$BASE"/*.cfg; do
    [ -e "$c" ] || continue
    case "$(basename "$c")" in
        default.cfg) continue ;;   # skip the stub; real one is in the pak
    esac
    cp -f "$c" "$STAGE/base/"
done

# NOTE: rebuilding doom.img RE-ZEROES the savefs write-region — saves/config are
# wiped on a fresh image build. Persistence is across REBOOTS of the same image
# file, not across image rebuilds. The durable dev workflow reuses one doom.img.
python3 "$HERE/mkdosfs.py" --root "$STAGE" --out "$OUT" --write-region-mib "$WRITE_REGION_MIB"
echo "built $OUT  ($(stat -c%s "$OUT") bytes)"
echo "boot:  DISK=$OUT $PROJ/riscv/run.sh $PROJ/build-rv/dhewm3"

#!/usr/bin/env bash
# Capture contiguous DoomOS gameplay frames through QEMU's monitor and encode MP4.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FRAMES="${FRAMES:-60}"
FPS="${FPS:-10}"
OUT="${OUT:-$ROOT/riscv/doom3-rvv-gameplay.mp4}"
CAPDIR="${CAPDIR:-$ROOT/riscv/video_frames}"
LOG="${LOG:-/tmp/dhewm3-video-capture.log}"
MON="${MONITOR_SOCKET:-/tmp/dhewm3-video-monitor.sock}"
DISK="${DISK:-/tmp/dhewm3-tile128-profile.img}"

mkdir -p "$CAPDIR"
find "$CAPDIR" -maxdepth 1 -name 'frame_*.ppm' -delete
rm -f "$MON" "$LOG"

env VIDEO=gpu DISPLAY_BACKEND=none MONITOR_SOCKET="$MON" DISK="$DISK" \
    TIMEOUT="${TIMEOUT:-1200}" SMP="${SMP:-8}" TCG_TB_SIZE="${TCG_TB_SIZE:-512}" \
    "$ROOT/riscv/run.sh" "$ROOT/build-rv/dhewm3" >"$LOG" 2>&1 &
qpid=$!
trap 'kill "$qpid" 2>/dev/null || true' EXIT

for _ in $(seq 1 600); do [[ -S "$MON" ]] && break; sleep 1; done
[[ -S "$MON" ]] || { echo "monitor socket did not appear" >&2; exit 1; }

# getviewpos is printed only by the live gameplay loop after map load/spawn.
while kill -0 "$qpid" 2>/dev/null && ! grep -q '^(-700 -1380' "$LOG"; do sleep 1; done
kill -0 "$qpid" 2>/dev/null || { echo "QEMU exited before gameplay" >&2; exit 1; }

last=$(grep -c '^frame:' "$LOG" || true)
for i in $(seq 1 "$FRAMES"); do
    # Wait for a complete newly-presented frame before touching the monitor.
    # Software rendering starts clearing the shared framebuffer for the next
    # frame immediately, so an unsynchronised screendump commonly captures that
    # clear (a black image) even though the previous frame rendered correctly.
    while kill -0 "$qpid" 2>/dev/null; do
        now=$(grep -c '^frame:' "$LOG" || true)
        (( now > last )) && { last=$now; break; }
        sleep 0.1
    done
    kill -0 "$qpid" 2>/dev/null || { echo "QEMU exited while waiting for frame $i" >&2; exit 1; }

    file=$(printf '%s/frame_%04d.ppm' "$CAPDIR" "$i")
    # Let the asynchronous virtio-GPU scanout update after the present marker.
    # Do not stop QEMU: its stopped virtio scanout screendump is all-zero.
    sleep "${CAPTURE_SETTLE:-0.25}"
    printf 'screendump %s\n' "$file" | nc -N -U "$MON" >/dev/null 2>&1 || true
    for _ in $(seq 1 600); do
        [[ -s "$file" ]] && break
        kill -0 "$qpid" 2>/dev/null || break
        sleep 0.1
    done
    if [[ ! -s "$file" ]]; then
        echo "failed to capture frame $i" >&2
        exit 1
    fi
    echo "captured $i/$FRAMES"
done

ffmpeg -y -hide_banner -loglevel warning -framerate "$FPS" \
    -i "$CAPDIR/frame_%04d.ppm" -c:v libx264 -preset slow -crf 18 \
    -pix_fmt yuv420p -movflags +faststart "$OUT"
echo "$OUT"

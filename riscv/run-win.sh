#!/usr/bin/env bash
#
# run-win.sh — Windows / MSYS2 (UCRT64) launcher for the bare-metal dhewm3 payload.
#
# The Linux launcher (run.sh) relies on taskset, /proc, unix monitor sockets and a
# qemu-11.0.1/build tree that doesn't exist on Windows. This one boots the same
# QEMU `virt` + OpenSBI + rva23s64 configuration with the MSYS2 UCRT64 (pacman)
# qemu-system-riscv64.exe — verified to accept the full rva23s64 CPU string and
# hand OpenSBI off to S-mode at 0x80200000 (QEMU 11.1.1 / OpenSBI v1.8.1).
#
#   riscv/run-win.sh [payload.elf]
#
# Env:
#   QEMU=<path to qemu-system-riscv64.exe>  default: the MSYS2 UCRT64 package
#                                           (/ucrt64/bin), or a locally built qemu
#                                           at riscv/tools/qemu/build-win if present
#   DISK=<raw image>   asset disk (the engine WRITES to it: use a scratch copy of doom.img)
#   LOG=<file>         serial console log (default: riscv/run-win.log)
#   SMP=4 MEM=4G TIMEOUT=1800 RVV_VLEN=1024
#   DTB_DUMP=<file>    export this configuration's device tree and exit before boot
#   VIDEO=gpu          add a virtio-gpu scanout (screendump via the monitor)
#   MONITOR_PORT=4455  HMP monitor on tcp:127.0.0.1:<port> (send "screendump C:/x.ppm")
#   GDB_PORT=4469      optional localhost debugger for profiling command mailbox
#
# Run from an MSYS2 UCRT64 shell (the qemu exe needs the ucrt64 DLLs on PATH).
set -euo pipefail

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
ELF="${1:-$PROJ/build-rv/dhewm3}"
[ -f "$ELF" ] || { echo "error: payload '$ELF' not found" >&2; exit 1; }

BUILT="$PROJ/riscv/tools/qemu/build-win/qemu-system-riscv64.exe"
PACMAN_QEMU="/ucrt64/bin/qemu-system-riscv64.exe"
if [ -z "${QEMU:-}" ]; then
    # Anchor to the MSYS2 pacman package by default; only use a local build if present.
    if [ -x "$PACMAN_QEMU" ]; then QEMU="$PACMAN_QEMU"; else QEMU="$BUILT"; fi
fi
BIOS_L=()
# The pacman qemu bundles its firmware (-bios default finds OpenSBI); a locally
# built, uninstalled qemu needs -L pointing at its pc-bios tree. Set QEMU_L to the
# pc-bios dir of any from-source build (e.g. the RVV-TCG-patched qemu-build tree).
if [ -n "${QEMU_L:-}" ]; then BIOS_L=(-L "$QEMU_L")
elif [ "$QEMU" = "$BUILT" ]; then BIOS_L=(-L "$(cygpath -m "$PROJ/riscv/tools/qemu/pc-bios")"); fi

SMP="${SMP:-4}"; MEM="${MEM:-4G}"; TIMEOUT="${TIMEOUT:-1800}"; RVV_VLEN="${RVV_VLEN:-1024}"
LOG="${LOG:-$PROJ/riscv/run-win.log}"
DISK="${DISK:-}"; VIDEO="${VIDEO:-}"; MONITOR_PORT="${MONITOR_PORT:-}"
# DISPLAY_BACKEND=gtk|sdl opens a REAL on-screen window (the engine flushes the
# softgles2 framebuffer to a virtio-gpu scanout, auto-enabled below). Leave empty
# for the headless default (-display none; capture via MONITOR_PORT screendump).
DISPLAY_BACKEND="${DISPLAY_BACKEND:-}"
CPU="${CPU:-rva23s64,vlen=${RVV_VLEN},v=true,vext_spec=v1.0,elen=64,zbc=true,zbkb=true,zbkc=true,zbkx=true,zfh=true,zvfh=true,zvfbfmin=true,zvfbfwma=true,zvbc=true,zvkng=true,zvksg=true}"

# native exe: hand it Windows-style paths and stop MSYS2 rewriting the args
w() { cygpath -m "$1"; }
export MSYS2_ARG_CONV_EXCL='*'

# A visible window needs a scanout to draw into; imply the virtio-gpu device.
if [ -n "$DISPLAY_BACKEND" ]; then
    DISP=( -display "$DISPLAY_BACKEND" )
    [ "$VIDEO" = gpu ] || VIDEO=gpu
else
    DISP=( -display none )
fi

MACHINE="virt,acpi=off"
if [ -n "${DTB_DUMP:-}" ]; then
    mkdir -p "$(dirname "$DTB_DUMP")"
    MACHINE+=",dumpdtb=$(w "$DTB_DUMP")"
fi

ARGS=(
    "${BIOS_L[@]}"
    "${DISP[@]}" -serial "file:$(w "$LOG")"
    -machine "$MACHINE"
    -accel "tcg,thread=multi,tb-size=${TCG_TB_SIZE:-256}"
    -cpu "$CPU" -smp "$SMP" -m "$MEM"
    -bios default -kernel "$(w "$ELF")"
    -global virtio-mmio.force-legacy=false
    -netdev user,id=net0 -device virtio-net-device,netdev=net0
    -device virtio-rng-pci
    -device virtio-keyboard-device -device virtio-tablet-device
)
if [ -n "$MONITOR_PORT" ]; then
    ARGS+=( -monitor "tcp:127.0.0.1:$MONITOR_PORT,server,nowait" )
else
    ARGS+=( -monitor none )
fi
[ -n "${GDB_PORT:-}" ] && ARGS+=( -gdb "tcp:127.0.0.1:$GDB_PORT" )
[ -n "$DISK" ]       && ARGS+=( -drive "file=$(w "$DISK"),format=raw,id=hd0,if=none" -device virtio-blk-device,drive=hd0 )
[ "$VIDEO" = gpu ]   && ARGS+=( -device virtio-gpu-device )

echo "+ $QEMU  -cpu $CPU  -smp $SMP  -m $MEM  ${DISK:+-disk $DISK}  ${DISPLAY_BACKEND:+-display $DISPLAY_BACKEND}  -> $LOG"
exec timeout "$TIMEOUT" "$QEMU" "${ARGS[@]}"

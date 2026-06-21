#!/usr/bin/env bash
#
# Boot a dhewm3 RISC-V64 payload on QEMU's `virt` machine via OpenSBI.
#
# Prefers a locally-built QEMU 11 (which knows the `rva23s64` CPU profile used
# in the project brief); otherwise falls back to the system QEMU with `-cpu max`
# (which still provides the full V extension). Override anything via env vars:
#
#   MEM=4G SMP=2 DISK=doom.img TIMEOUT=60 ./run.sh build/payload.elf
#   SMP=8,cores=4,threads=2 ./run.sh build/payload.elf
#   TCG_TB_SIZE=256 ./run.sh build/payload.elf
#   PIN_HARTS=1 PIN_HART_CPUS=0,1,2,3 ./run.sh build/payload.elf
#   AIA=aplic ./run.sh build/payload.elf
#   AIA=aplic-imsic ./run.sh build/payload.elf
#
set -euo pipefail

ELF="${1:-build/payload.elf}"
[ -f "$ELF" ] || { echo "error: payload '$ELF' not found (run 'make' first)"; exit 1; }

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
QEMU_CPU_RVA23S64="rva23s64,vlen=1024,v=true,vext_spec=v1.0,elen=64,zbc=true,zbkb=true,zbkc=true,zbkx=true,zfh=true,zvfh=true,zvfbfmin=true,zvfbfwma=true,zvbc=true,zvkng=true,zvksg=true"
NEW_QEMU="$PROJ/qemu-11.0.1/build/qemu-system-riscv64"
BIOS_L=()
if [ -x "$NEW_QEMU" ]; then
    QEMU="$NEW_QEMU"
    CPU="${CPU:-$QEMU_CPU_RVA23S64}"
    # the built QEMU isn't installed; point it at its bundled firmware
    BIOS_L=(-L "$PROJ/qemu-11.0.1/pc-bios")
else
    QEMU="$(command -v qemu-system-riscv64)"
    CPU="${CPU:-max}"
fi

MEM="${MEM:-4G}"
# 4 harts: one boot/main hart, one dhewm3 AsyncThread hart, and the remaining
# harts available for softgles2 tile-parallel raster workers.
SMP="${SMP:-4}"
SMP_COUNT_TEXT="${SMP%%,*}"
case "$SMP_COUNT_TEXT" in
    ""|*[!0-9]*)
        echo "error: SMP must start with a guest hart count (got '$SMP')" >&2
        exit 1
        ;;
esac
SMP_COUNT=$((10#$SMP_COUNT_TEXT))
if [ "$SMP_COUNT" -lt 1 ]; then
    echo "error: SMP hart count must be at least 1 (got '$SMP')" >&2
    exit 1
fi
TIMEOUT="${TIMEOUT:-30}"
TCG_TB_SIZE="${TCG_TB_SIZE:-256}"
DISK="${DISK:-}"
METRICS="${METRICS:-}"
METRICS_LOG="${METRICS_LOG:-$PROJ/metrics.log}"
AUDIO_WAV="${AUDIO_WAV:-}"
VIDEO="${VIDEO:-}"
DISPLAY_BACKEND="${DISPLAY_BACKEND:-gtk}"
MONITOR_SOCKET="${MONITOR_SOCKET:-}"
PIN_HARTS="${PIN_HARTS:-0}"
PIN_HART_CPUS="${PIN_HART_CPUS:-}"
AIA="${AIA:-}"
MACHINE="virt,acpi=off"
case "$AIA" in
    ""|none|off)
        ;;
    aplic|aplic-imsic)
        MACHINE+=",aia=$AIA"
        ;;
    *)
        echo "error: unsupported AIA='$AIA' (use: aplic, aplic-imsic, none/off)" >&2
        exit 1
        ;;
esac

pin_qemu_harts() {
    local qemu_pid="$1"
    local cpu_list="$2"
    local -a cpus=()
    local -a tids=()
    local deadline tid task comm i n

    if ! command -v taskset >/dev/null 2>&1; then
        printf '%s\n' "[pin] taskset not found; leaving QEMU vCPU threads unpinned" >&2 || true
        return 0
    fi

    if [ -n "$cpu_list" ]; then
        IFS=',' read -r -a cpus <<< "$cpu_list"
    else
        for ((i = 0; i < SMP_COUNT; i++)); do
            cpus+=( "$i" )
        done
    fi

    deadline=$((SECONDS + 10))
    while [ "$SECONDS" -lt "$deadline" ]; do
        tids=()
        for task in "/proc/$qemu_pid/task/"[0-9]*; do
            [ -e "$task/comm" ] || continue
            tid="${task##*/}"
            comm="$(<"$task/comm")"
            case "$comm" in
                CPU\ *|vcpu*)
                    tids+=( "$tid:$comm" )
                    ;;
            esac
        done
        [ "${#tids[@]}" -ge "$SMP_COUNT" ] && break
        kill -0 "$qemu_pid" 2>/dev/null || return 0
        sleep 0.05
    done

    if [ "${#tids[@]}" -eq 0 ]; then
        printf '%s\n' "[pin] no QEMU vCPU threads found under /proc/$qemu_pid/task; leaving unpinned" >&2 || true
        return 0
    fi

    mapfile -t tids < <(printf '%s\n' "${tids[@]}" | sort -t: -k2,2V)
    n="${#tids[@]}"
    [ "$n" -gt "$SMP_COUNT" ] && n="$SMP_COUNT"
    [ "$n" -gt "${#cpus[@]}" ] && n="${#cpus[@]}"

    if [ "$n" -lt "$SMP_COUNT" ]; then
        printf '%s\n' "[pin] warning: pinning $n/$SMP_COUNT QEMU vCPU threads (cpus=${cpus[*]})" >&2 || true
    fi

    for ((i = 0; i < n; i++)); do
        tid="${tids[$i]%%:*}"
        comm="${tids[$i]#*:}"
        printf '%s\n' "[pin] hart/vCPU $i thread $tid ($comm) -> host CPU ${cpus[$i]}" >&2 || true
        if ! taskset -pc "${cpus[$i]}" "$tid" >/dev/null; then
            printf '%s\n' "[pin] warning: failed to pin thread $tid to host CPU ${cpus[$i]}" >&2 || true
        fi
    done
}

ARGS=(
    "${BIOS_L[@]}"
    -machine "$MACHINE"
    # Multi-threaded TCG: run each guest hart on its own HOST thread. Without
    # this, all -smp harts round-robin on a single host thread and the softgles2
    # tile-parallel raster workers get NO real parallelism (they just time-slice
    # the one core). The larger TB cache keeps Doom 3/gl4es/softgles2 translated
    # blocks hot during map load + render frames. No KVM: x86 host can't KVM a
    # RISC-V guest, so TCG is the floor regardless.
    -accel "tcg,thread=multi,tb-size=$TCG_TB_SIZE"
    -cpu "$CPU"
    -smp "$SMP"
    -m "$MEM"
    -bios default
    -kernel "$ELF"
    # Force MODERN virtio-mmio (version 2) on the pre-created virt MMIO buses.
    # QEMU defaults virtio-mmio to legacy (version 1); the dOS virtio drivers
    # (blk, and the future snd) speak modern v2 only (VIRTIO_F_VERSION_1).
    -global virtio-mmio.force-legacy=false
    -netdev user,id=net0
    -device virtio-net-device,netdev=net0
    -device virtio-rng-pci
    # Input on the virt MMIO bus (DeviceID=18). The dОС virtio-input driver
    # (riscv/dos/video-dos/dos_virtio_input.c) scans for these; harmless to boots
    # that don't use input. Tablet = absolute coords for the menu cursor.
    -device virtio-keyboard-device
    -device virtio-tablet-device
)

if [ "$VIDEO" = "gpu" ]; then
    # Real QEMU 2D output: dOS scans the virtio-mmio GPU device (DeviceID=16)
    # and flushes the softgles2 framebuffer to scanout 0. Use e.g.
    #   VIDEO=gpu DISPLAY_BACKEND=gtk ...
    # or DISPLAY_BACKEND=none for a headless virtio-gpu protocol smoke.
    if [ -n "$MONITOR_SOCKET" ]; then
        ARGS=( -display "$DISPLAY_BACKEND" -serial stdio "${ARGS[@]}" )
    else
        ARGS=( -display "$DISPLAY_BACKEND" -serial mon:stdio "${ARGS[@]}" )
    fi
    ARGS+=( -device virtio-gpu-device )
else
    ARGS=( -nographic "${ARGS[@]}" )
fi

if [ -n "$MONITOR_SOCKET" ]; then
    rm -f "$MONITOR_SOCKET"
    ARGS+=( -monitor "unix:$MONITOR_SOCKET,server,nowait" )
fi

if [ -n "$DISK" ]; then
    # Explicit MMIO block device (the "*-device" suffix lands it in the virt MMIO
    # window at 0x10001000+, DeviceID=2) — the transport the dOS virtio-blk driver
    # scans. "if=virtio" alone could auto-create a virtio-blk-PCI instead.
    ARGS+=( -drive  "file=$DISK,format=raw,id=hd0,if=none"
            -device "virtio-blk-device,drive=hd0" )
fi

if [ -n "$METRICS" ]; then
    # Dedicated metrics channel: a virtio-console (virtio DeviceID 3) on the virt
    # MMIO bus, routed to its OWN host file ($METRICS_LOG) — keeps dОС perf
    # metrics off uart0, where the SBI console log + framebuffer base64 dumps
    # live. The dОС TX driver (riscv/smp/vcon.c) scans MMIO for DeviceID 3;
    # riscv/scripts/dostop.py renders the stream like top.
    : > "$METRICS_LOG"          # start each run with a clean log
    ARGS+=( -chardev "file,id=met,path=$METRICS_LOG"
            -device  virtio-serial-device,id=vser0
            -device  "virtconsole,chardev=met,bus=vser0.0,name=metrics" )
fi

if [ -n "$AUDIO_WAV" ]; then
    # Attach QEMU's virtio-sound device on the virtio-mmio bus and record host
    # output to a WAV file. The dOS virtio-snd driver scans MMIO DeviceID=25.
    ARGS+=( -audiodev "wav,id=snd0,path=$AUDIO_WAV"
            -device "virtio-sound-device,audiodev=snd0,streams=1" )
fi

PIN_SUMMARY=""
if [ "$PIN_HARTS" = "1" ]; then
    PIN_SUMMARY="  -pin-harts ${PIN_HART_CPUS:-0..$((SMP_COUNT - 1))}"
fi
echo "+ $(basename "$QEMU")  -accel tcg,thread=multi,tb-size=$TCG_TB_SIZE  -cpu $CPU  -m $MEM  -smp $SMP  ${DISK:+-drive $DISK}${METRICS:+  -metrics $METRICS_LOG}${AUDIO_WAV:+  -audio $AUDIO_WAV}${VIDEO:+  -video $VIDEO/$DISPLAY_BACKEND}${MONITOR_SOCKET:+  -monitor $MONITOR_SOCKET}$PIN_SUMMARY"
echo "+ machine: $MACHINE"
echo "+ kernel: $ELF"
echo "---------------------------------------------------------------"
if [ "$PIN_HARTS" != "1" ]; then
    exec timeout "$TIMEOUT" "$QEMU" "${ARGS[@]}"
fi

"$QEMU" "${ARGS[@]}" &
qemu_pid="$!"
trap 'kill "$qemu_pid" 2>/dev/null || true' INT TERM EXIT
pin_qemu_harts "$qemu_pid" "$PIN_HART_CPUS"

timeout_rc=0
timeout "$TIMEOUT" tail --pid="$qemu_pid" -f /dev/null >/dev/null 2>&1 || timeout_rc="$?"
if kill -0 "$qemu_pid" 2>/dev/null; then
    kill "$qemu_pid" 2>/dev/null || true
    wait "$qemu_pid" 2>/dev/null || true
    exit "$timeout_rc"
fi

trap - INT TERM EXIT
wait "$qemu_pid"

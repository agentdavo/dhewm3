#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/riscv/glmark2"
BUILD="${BUILD:-$SRC/build-cmake-doomos}"
TOOLCHAIN="${TOOLCHAIN:-$ROOT/riscv/toolchain/baremetal-rva23s64.cmake}"
DATA_PATH="${DATA_PATH:-/glmark2}"
TARGET="${TARGET:-glmark2-es2-doomos}"
ELF="$BUILD/$TARGET"
ACTION="${1:-build}"
GLMARK2_SIZE="${GLMARK2_SIZE:-320x240}"
GLMARK2_DURATION="${GLMARK2_DURATION:-1}"
GLMARK2_FRAMES="${GLMARK2_FRAMES:-}"
# If GLMARK2_BENCHMARK is unset (not just empty), use default.
# Set GLMARK2_BENCHMARK="" to skip --benchmark and run the built-in default list.
if [ -z "${GLMARK2_BENCHMARK+x}" ]; then
  if [ -n "$GLMARK2_FRAMES" ]; then
    GLMARK2_BENCHMARK="build:use-vbo=false:duration=999:nframes=$GLMARK2_FRAMES"
  else
    GLMARK2_BENCHMARK="build:use-vbo=false:duration=$GLMARK2_DURATION"
  fi
fi
GLMARK2_IMAGE_VERBOSE="${GLMARK2_IMAGE_VERBOSE:-0}"
GLMARK2_FRAMEBIN="${GLMARK2_FRAMEBIN:-1}"
GLMARK2_PROFILE="${GLMARK2_PROFILE:-0}"

case "$GLMARK2_DURATION" in
  ''|*[!0-9]*) GLMARK2_TIMEOUT_DEFAULT=120 ;;
  *) GLMARK2_TIMEOUT_DEFAULT=$((120 + GLMARK2_DURATION * 30)) ;;
esac
if [ -n "$GLMARK2_FRAMES" ]; then
  case "$GLMARK2_FRAMES" in
    ''|*[!0-9]*) ;;
    *) GLMARK2_TIMEOUT_DEFAULT=$((120 + GLMARK2_FRAMES * 20)) ;;
  esac
fi

case "$ACTION" in
  build|run|image-data|clean|help) shift || true ;;
  *) ACTION=build ;;
esac

usage() {
  cat <<EOF
usage: $0 [build|run|image-data|clean] [cmake/build args...]

env:
  BUILD=$BUILD
  DATA_PATH=$DATA_PATH
  GLMARK2_IMG=<image path>        default: $BUILD/glmark2.img
  GLMARK2_STAGE=<stage path>      default: $BUILD/stage
  GLMARK2_SIZE=$GLMARK2_SIZE
  GLMARK2_DURATION=$GLMARK2_DURATION
  GLMARK2_FRAMES=$GLMARK2_FRAMES
  GLMARK2_BENCHMARK=$GLMARK2_BENCHMARK
  GLMARK2_IMAGE_VERBOSE=$GLMARK2_IMAGE_VERBOSE
  GLMARK2_FRAMEBIN=$GLMARK2_FRAMEBIN
  GLMARK2_PROFILE=$GLMARK2_PROFILE
  TIMEOUT=$GLMARK2_TIMEOUT_DEFAULT SMP=4 TCG_TB_SIZE=1024 PIN_HARTS=1 PIN_HART_CPUS=0,1,2,3
EOF
}

configure() {
  cmake -S "$SRC" -B "$BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Release \
    -DGLMARK2_DATA_PATH="$DATA_PATH" \
    -DGLMARK2_DOOMOS_SIZE="$GLMARK2_SIZE" \
    -DGLMARK2_DOOMOS_BENCHMARK="$GLMARK2_BENCHMARK" \
    -DGLMARK2_DOOMOS_FRAMEBIN="$GLMARK2_FRAMEBIN" \
    -DGLMARK2_DOOMOS_PROFILE="$GLMARK2_PROFILE" \
    "$@"
}

build() {
  configure "$@"
  cmake --build "$BUILD" --target "$TARGET"
}

image_data() {
  local img="${GLMARK2_IMG:-$BUILD/glmark2.img}"
  local stage="${GLMARK2_STAGE:-$BUILD/stage}"
  rm -rf "$stage"
  mkdir -p "$stage/glmark2"
  cp -a "$SRC/data/." "$stage/glmark2/"
  local quiet_arg=(--quiet)
  if [[ "$GLMARK2_IMAGE_VERBOSE" == "1" ]]; then
    quiet_arg=()
  fi
  python3 "$ROOT/riscv/image/mkdosfs.py" --root "$stage" --out "$img" --write-region-mib 0 "${quiet_arg[@]}" >&2
  echo "$img"
}

case "$ACTION" in
  build)
    build "$@"
    ;;
  run)
    build "$@"
    img="$(image_data)"
    env \
      DISK="${DISK:-$img}" \
      TIMEOUT="${TIMEOUT:-$GLMARK2_TIMEOUT_DEFAULT}" \
      SMP="${SMP:-4}" \
      TCG_TB_SIZE="${TCG_TB_SIZE:-1024}" \
      PIN_HARTS="${PIN_HARTS:-1}" \
      PIN_HART_CPUS="${PIN_HART_CPUS:-0,1,2,3}" \
      "$ROOT/riscv/run.sh" "$ELF"
    ;;
  image-data)
    image_data
    ;;
  clean)
    cmake --build "$BUILD" --target clean
    ;;
  help)
    usage
    ;;
esac

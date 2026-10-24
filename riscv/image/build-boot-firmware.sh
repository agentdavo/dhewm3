#!/usr/bin/env bash
# Run from Ubuntu WSL; every download, object and scratch file stays in the repo.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="$ROOT/build/toolchain"
OUT="$ROOT/build-rv/sdcard"
mkdir -p "$WORK/boot-downloads" "$WORK/boot-deps" "$WORK/tmp" "$OUT"
export TMPDIR="$WORK/tmp"
cd "$WORK/boot-downloads"
if ! command -v bison >/dev/null || ! command -v flex >/dev/null; then
    compgen -G 'bison_*.deb' >/dev/null || apt download bison
    compgen -G 'flex_*.deb' >/dev/null || apt download flex
    compgen -G 'm4_*.deb' >/dev/null || apt download m4
    for p in bison_*.deb flex_*.deb m4_*.deb; do dpkg-deb -x "$p" "$WORK/boot-deps"; done
    export PATH="$WORK/boot-deps/usr/bin:$PATH"
    export BISON_PKGDATADIR="$WORK/boot-deps/usr/share/bison"
    export M4="$WORK/boot-deps/usr/bin/m4"
fi
test -f u-boot-2025.01.tar.bz2 || curl -fL -o u-boot-2025.01.tar.bz2 https://ftp.denx.de/pub/u-boot/u-boot-2025.01.tar.bz2
test -f opensbi-1.8.1.tar.gz || curl -fL -o opensbi-1.8.1.tar.gz https://github.com/riscv-software-src/opensbi/archive/refs/tags/v1.8.1.tar.gz
sha256sum u-boot-2025.01.tar.bz2 opensbi-1.8.1.tar.gz > "$OUT/firmware-sources.sha256"
sha256sum -c <<'SUMS'
cdef7d507c93f1bbd9f015ea9bc21fa074268481405501945abc6f854d5b686f  u-boot-2025.01.tar.bz2
fb1ae61a85e966322101acb1c982f84d9eaafc4de7dd474a7d7546d9bb321c6f  opensbi-1.8.1.tar.gz
SUMS
test -d "$WORK/src/u-boot-2025.01" || tar -xf u-boot-2025.01.tar.bz2 -C "$WORK/src"
test -d "$WORK/src/opensbi-1.8.1" || tar -xf opensbi-1.8.1.tar.gz -C "$WORK/src"
UB="$WORK/src/u-boot-2025.01"
OBJ="$WORK/obj/u-boot-doomos"
export CROSS_COMPILE="${CROSS_COMPILE:-riscv64-linux-gnu-}"
make -C "$UB" O="$OBJ" qemu-riscv64_smode_defconfig
# DoomOS owns secondary-hart startup through SBI HSM. U-Boot must leave them stopped.
"$UB/scripts/config" --file "$OBJ/.config" --disable SMP --enable LEGACY_IMAGE_FORMAT \
    --disable TOOLS_MKEFICAPSULE --disable EFI_CAPSULE_AUTHENTICATE \
    --disable TOOLS_KWBIMAGE --disable TOOLS_LIBCRYPTO --disable TOOLS_FIT_SIGNATURE \
    --enable USE_BOOTCOMMAND --set-str BOOTCOMMAND 'virtio scan; if load virtio 0:1 0x85000000 boot.scr; then source 0x85000000; fi; echo DoomOS disk boot failed' \
    --set-val BOOTDELAY 0
make -C "$UB" O="$OBJ" olddefconfig
make --jobserver-style=pipe -C "$UB" O="$OBJ" -j"${JOBS:-12}"
make --jobserver-style=pipe -C "$WORK/src/opensbi-1.8.1" O="$WORK/obj/opensbi-doomos" \
    PLATFORM=generic FW_PAYLOAD_PATH="$OBJ/u-boot.bin" -j"${JOBS:-12}"
cp "$WORK/obj/opensbi-doomos/platform/generic/firmware/fw_payload.bin" "$OUT/doomos-firmware.bin"
cp "$OBJ/.config" "$OUT/u-boot.config"
echo "Firmware: $OUT/doomos-firmware.bin"

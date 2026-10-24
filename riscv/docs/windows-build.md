# DoomOS on Windows (MSYS2 UCRT64)

How to build and boot the bare-metal dhewm3 RISC-V64 image ("DoomOS") on Windows,
anchored to the MSYS2 stack at **`c:\msys64`** plus a one-time medany
runtime-library build in **`C:\projects\dhewm3\build\toolchain\install`**.

Historical verification (2026-09-06, previous toolchain location): builds with the medany toolchain and boots on the pacman
`qemu-system-riscv64` 11.1.1 (OpenSBI v1.8.1) → S-mode @ `0x80200000`, rendering
`game/demo_mars_city1` through softgles2 across 8 harts.

---

## 1. Prerequisites (all from `c:\msys64` via pacman)

Run in a **UCRT64** shell (`C:\msys64\ucrt64.exe`):

```sh
pacman -S --needed \
  mingw-w64-ucrt-x86_64-qemu \
  mingw-w64-ucrt-x86_64-riscv64-unknown-elf-binutils \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-gmp mingw-w64-ucrt-x86_64-mpfr mingw-w64-ucrt-x86_64-mpc \
  mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
  texinfo
```

- `qemu-system-riscv64` 11.1.1 accepts `-cpu rva23s64` and bundles OpenSBI —
  nothing needs building from source on the runtime side.
- The pacman `riscv64-unknown-elf` **binutils** (2.47) are code-model-agnostic
  and are reused; only its gcc/newlib are unusable (see §3).
- `texinfo` supplies `makeinfo`, which the libgloss build needs.

## 2. The medany toolchain (one-time, `C:\projects\dhewm3\build\toolchain\install`)

The build recipe lives at `C:\projects\dhewm3\riscv\build-toolchain-win.sh`. Run it from
a UCRT64 shell:

```sh
JOBS=12 bash riscv/build-toolchain-win.sh
```

It downloads gcc 16.1.0 + newlib 4.6.0.20260123, builds a combined tree
configured `--with-cmodel=medany --with-arch=rv64gc --with-abi=lp64d
--disable-multilib --disable-bootstrap --enable-languages=c,c++`, reusing
`c:\msys64`'s host gcc / gmp-mpfr-mpc, and installs to `C:\projects\dhewm3\build\toolchain\install`
The code-model-independent MSYS2 binutils are copied into that prefix.
The compiler still needs the MSYS2 UCRT64 DLLs on PATH.

Downloads, sources, intermediate objects, installation and the build log all
stay under `build/toolchain/`. The recipe uses relative source paths so GCC�s
native Windows code generators can read their generated input lists.

After building, run `bash riscv/validate-toolchain.sh`. This checks the default
code model and links an RVA23/RVV C++ payload using exceptions, allocation and
math at `0x80200000`. Results are under `build/toolchain/validation/`.

Verify medany is the default and that the runtime libs are PC-relative:

```sh
riscv64-unknown-elf-gcc -Q --help=target | grep mcmodel     # -> medany
riscv64-unknown-elf-objdump -r "$(riscv64-unknown-elf-gcc -print-file-name=libc.a)" \
  | grep -c R_RISCV_HI20                                     # -> 0 (all PCREL_HI20)
```

### Why medany is mandatory

The image loads at **`0x80200000`** (QEMU `virt` RAM starts at `0x80000000`;
OpenSBI takes the first 2 MiB and hands the S-mode kernel off just above it).
That address is past the **medlow** code model's ±2 GiB-around-zero reach, so
medlow `%hi`/`%lo` (`R_RISCV_HI20`) relocations overflow:

```
relocation truncated to fit: R_RISCV_HI20 against symbol `__malloc_av_'
```

The pacman `riscv64-unknown-elf` package ships **medlow** archives for *all* of
`libc` / `libm` / `libgcc` / `libsupc++` / `libstdc++`. Because C++ exceptions +
RTTI are load-bearing in the engine (`common->Error()` throws `idException`), the
link pulls `libsupc++`/`libstdc++`/`libgcc` too — so rebuilding only newlib is
not enough. Everything must be medany, which is what `--with-cmodel=medany`
produces for the whole runtime.

## 3. Build the engine

```sh
riscv/build-win.sh                 # -> C:/projects/dhewm3/build-rv/dhewm3
# overrides:
BUILD=C:/projects/dhewm3/build-rv TOOLCHAIN_BIN=C:/projects/dhewm3/build/toolchain/install/bin riscv/build-win.sh
```

`build-win.sh` guards that the toolchain defaults to medany, then configures the
same CMake option set as the Linux `build-rv` tree (SDL3 + gl4es + softgles2,
BASE demo). The prebuilt `riscv/sdl3/libSDL3-riscv.a` and
`riscv/gl4es/lib/libGL-riscv.a` are already medany and are linked as-is.

To rebuild gl4es after changing a bridge patch, run this from MSYS2 UCRT64
before rebuilding the engine. Use a separate build directory to avoid an old
CMake compiler/platform cache, and make Git available for patch application:

```sh
export PATH="/c/Program Files/Git/cmd:$PATH"
RV=/c/projects/dhewm3/build/toolchain/install/bin/riscv64-unknown-elf- \
GL4ES_BUILD_DIR=/c/projects/dhewm3/riscv/gl4es/build-riscv-hz \
bash riscv/gl4es-bridge/build-gl4es.sh riscv
bash riscv/build-win.sh
```

The script resolves Windows executable suffixes and native include paths,
applies the patches idempotently, and copies the archive to `libGL-riscv.a`.

## 4. Boot it

```sh
cp doom.img build-rv/doom-scratch.img        # the engine WRITES to the disk (savefs)
DISK="$PWD/build-rv/doom-scratch.img" riscv/run-win.sh
```

`run-win.sh` boots the pacman qemu (`-cpu rva23s64,vlen=1024,...`,
`-machine virt,acpi=off`, `-accel tcg,thread=multi`, `-bios default`) and writes
the serial console to `riscv/run-win.log` (override with `LOG=`). Useful env:
`SMP=8 MEM=4G TIMEOUT=1800 RVV_VLEN=1024 VIDEO=gpu MONITOR_PORT=4455`.

A healthy boot ends up in the render loop:

```
[doomos-cpu] RVV VLEN=1024 bits; ...
dosfs: mounted, 4 files, 1075520 sectors on virtio-blk
dhewm3 1.5.5.1305 generic-riscv64 ... using SDL v3.4.10
OpenGL renderer: GL4ES using softgles2 (RVV software rasteriser)
bm map file found: maps/game/demo_mars_city1.map
[speeds] render:184ms  bk(backend):179ms
```

## 5. Gotchas seen while bringing this up on Windows

- **Native tools can't read `/c/...` paths.** MSYS2's `make` auto-converts
  `/c/...` *argv* to `C:/...` when it execs native tools, but paths written
  *inside* generated files (e.g. gcc's `gtyp-input.list`) are read at runtime and
  are not converted, so `gengtype.exe` fails with `line-map.h: No such file or
  directory`. Fix: run `configure` via **mixed `C:/...` paths** (the recipe does
  this with `cygpath -m`), which bakes Windows paths into the generated files.
- **`makeinfo` missing** → libgloss `porting.info` fails with `Error 127`.
  Install `texinfo`.
- **`cc1plus` needs the ucrt64 DLLs on `PATH`.** The medany gcc's `cc1`/`cc1plus`
  link against `c:\msys64\ucrt64\bin` DLLs (gmp/mpfr/mpc/zstd/libstdc++-6).
  Always build/run from a **UCRT64 shell**; a bare `--version` works without them
  but any real compile silently exits 1.
- **Boot writes to the disk** — always boot a scratch copy of `doom.img`.

## Build location and WSL

Keep engine outputs in `build-rv/` and validation outputs in `riscv/build/`.
The medany toolchain, downloads and intermediates belong in `build/toolchain/`.
Optional local QEMU builds belong in `riscv/tools/`. These directories are ignored by Git.
Installed system tools can still be selected using PATH or explicit overrides.

For the checkout on `C:\projects\dhewm3`, prefer native MSYS2 builds.
WSL can access it at `/mnt/c/projects/dhewm3`, but Linux compilation across
that mount adds filesystem overhead. Do not share a CMake build cache between
Windows and Linux: if testing WSL, use a separate repository-local `build-rv-wsl/`
and a Linux medany cross compiler. WSL does not inherently improve the generated
RISC-V executable; compare equivalent compiler versions and target flags.

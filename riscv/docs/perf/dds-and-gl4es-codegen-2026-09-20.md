# Doom DDS and gl4es RISC-V code generation

## Changes

The engine validates a DDS layout before uploading any payload. It checks header
sizes, dimensions, mip counts, face completeness, channel masks and payload bounds.
Each supplied face and mip is uploaded independently. Supported original Doom
formats include DXT1 RGB/RGBA, DXT3, DXT5/RXGB, masked RGB/RGBA, A8, luminance and
the engine's shared-palette indexed normals. RXGB already stores normal X in alpha;
the loader preserves that layout. Raw DDS pixels expand to RGBA8.

The bare-metal gl4es path forwards S3TC blocks directly to softgles2, avoiding the
old shim decode and regeneration of authored mip levels. softgles2 accepts mip
and cube-face uploads and block-aligned subupdates, preserves edge blocks, and
waits for deferred draws before replacing sampled storage. DXT1 RGB's three-color
mode produces opaque black; DXT1 RGBA produces transparent black. DXT3/5 always
use four-color interpolation. These rules follow the
[Khronos S3TC specification](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_texture_compression_s3tc.txt).

The optional RVV decoder batches complete blocks at VLEN 512/1024; smaller VLENs
and partial edge blocks use scalar decoding. The follow-up boot and decoder
measurements in `dds-boot-ab-2026-09-20.md` selected scalar DDS block decoding as
the QEMU default. Set `sgl_dxt_vector_decode=1` for hardware comparisons or build
with `SGL_DXT_VECTOR_DEFAULT=1`; other renderer RVV paths are unchanged. This
avoids forcing a fixed 16-lane vector on VLEN 128. All vector loops use actual VL.
Compressed upload work is included in
the existing softgles texture timer. Debug symbols `sgl_dbg_dds_uploads`,
`sgl_dbg_dds_mips` and `sgl_dbg_dds_bytes` count successful API uploads.

Full-quality source-texture defaults remain in force. DDS can be exercised with
`image_usePrecompressedTextures 1; image_useNormalCompression 2; reloadImages reload`.
DDS compression can introduce losses already present in the asset; RGBA8 storage
does not recover those losses.

## Compiler audit

`riscv/gl4es-bridge/test/audit_riscv_codegen.py` recompiles all 69 configured gl4es
translation units with `-march=rv64gcv`, preserving the other build flags. All
compiled successfully with the repository GCC 16.1 toolchain. Objects, individual
vectorization reports and `summary.json` are in `build-rv/gl4es-rv64gcv-audit/`.
The shipping library retains the configured RVA23 ISA and VLEN minimum; this audit
does not replace its archive. Build scripts export compile commands for repeatability.

BGRA/RGBA conversion now expresses fixed four-byte pixels as typed contiguous row
accesses, rather than runtime integer-pointer strides. It uses ordinary C and
retains forward-loop overlap semantics without a `restrict` assumption. GCC's
dynamic vector cost model produced an alias-checked RVV loop, but that experimental
setting was rejected: in QEMU it was slower than scalar conversion at every VLEN.
At VLEN 1024 the three experimental vector timings were 4.27/5.10/4.20 ms versus
3.55/3.71/3.24 ms for scalar, for 2,097,152 pixels. These are emulator measurements,
not predictions for physical RVV silicon. Production keeps the normal `-O2` cost
model; no blanket fast-math, unsafe alias assumptions or forced vectorization.

## Validation

- DDS layout tests: 187 synthetic cases; all 3,642 demo DDS payloads accepted
  (1,994 DXT1, 855 DXT3, 750 RXGB, 38 RGB24, 5 RGBA32).
- Production compressed API tests: 601 cases pass at each of VLEN 128/256/512/1024 against an
  independent bit-reader reference: mip chains, cube faces, alpha modes, odd edges,
  malformed sizes, update bounds, preserved storage and generated-mip updates.
- Production C pixel swizzle compiled with plain RV64GCV: 1,026 widths, including
  zero, odd lengths, tails, exact alias and both overlap directions, at all four VLENs.
- Full engine and gl4es archive rebuilt with the repository medany toolchain.
- End-to-end Mars City reload: 16,827 successful compressed uploads, including
  14,774 non-base mip levels, totaling 80,904,232 input bytes. The original mip
  chain reaches softgles2. `build-rv/dds-wall-validated.png` is a checked scanout
  capture after reload; `r_skipFogLights` remained zero. Source-texture quality
  settings were restored afterwards (command 3 completed), and
  `build-rv/dds-final-full-quality.png` was captured and inspected. No GL error
  or invalid-DDS messages appeared in the integration log. A still image does not establish absence
  of temporal tearing.

The SDL QEMU launch stalled before renderer initialization. The same ELF ran
headlessly with virtio-gpu and a direct scanout capture. This was not counted as
a successful windowed-display validation. The running ELF snapshot is
`build-rv/dds-integration-running.elf`; the final `build-rv/dhewm3` additionally
removes an unnecessary DDS layout stack copy and has been rebuilt successfully.

DDS microbenchmarks are sensitive to QEMU scheduling and host contention; the
available runs do not justify a whole-game speedup claim.

## Modern DDS assessment

DX10 header parsing accepts single 2D BC1/2/3 UNORM resources. Existing desktop BC7
capability gating is preserved, but softgles2 does not implement BC7 decoding.
BC4/5, BC6H, BC7, sRGB DXGI variants, arrays and volume textures require additional
decoders and/or texture API support. They are not advertised as supported by
softgles2. The [Microsoft DDS programming guide](https://learn.microsoft.com/en-us/windows/win32/direct3ddds/dx-graphics-dds-pguide)
describes the extended header and layout considerations for that next stage.

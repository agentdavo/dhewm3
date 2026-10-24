# DDS startup measurement and backend follow-up

## Controlled boots before further decoder tuning

One instrumented ELF, QEMU 11.1.1 TCG, four harts, VLEN 1024, 4 GiB,
768x432, headless virtio-gpu. Each run uses a temporary snapshot of the same
asset disk; writes and temporary files remain under `build-rv/`. Normal startup
defaults and the backing disk are unchanged. The order was source, legacy DDS,
native DDS, source. The source repeat brackets host/cache variation.

Times start at C-runtime entry after OpenSBI and end at the first successful
map presentation, not process launch or a guarantee of interactive control.

| Mode | Bulk texture phase | Map ready | First map present |
|---|---:|---:|---:|
| Full-quality source, run 1 | 17.597 s | 50.944 s | 161.359 s |
| DDS, legacy gl4es decode | 8.662 s | 40.093 s | 81.054 s |
| DDS, direct softgles2 decode | 8.701 s | 40.690 s | 78.311 s |
| Full-quality source, run 2 | 18.148 s | 55.839 s | 155.345 s |

DDS substantially reduced startup in these runs, but changes the source texture
quality. Direct forwarding alone did **not** clearly reduce the bulk texture
phase relative to legacy DDS. The 2.743 s first-present difference between DDS
routes is a single comparison and is insufficient to establish a reliable
end-to-end gain. Initial frame view/draw counts also differ between source and
DDS modes; do not attribute the entire first-present difference to decoding.

## Texture call breakdown

These are inclusive nested timers, not additive CPU profiles.

| Mode | Engine GL upload calls | Within softgles2 | Difference |
|---|---:|---:|---:|
| Source, run 1 | 1.042 s | 0.643 s | 0.399 s |
| DDS, legacy | 4.306 s | 0.478 s | 3.829 s |
| DDS, native | 4.487 s | 4.433 s | 0.054 s |
| Source, run 2 | 1.136 s | 0.677 s | 0.459 s |

Direct forwarding moved decode work into softgles2 and removed almost all shim
work around the call. The backend's 4.433 s made it the next optimization target;
moving work between layers is not itself a speedup. The native route recorded
16,827 compressed uploads, 14,774 non-base mip levels and 80,904,232 input bytes
through first-map-present. The legacy and source controls recorded zero native
compressed uploads, confirming the switches were effective.

Raw milestones and texture reports: `build-rv/dds-boot-ab/results.json`.
Per-run serial logs and the ELF SHA-256 are in the same directory.
Reproduce with `riscv/scripts/texture_boot_ab.py` after building the instrumented
ELF. Debugger switches `bm_texture_boot_dds` and `gl4es_native_s3tc` select the
comparison paths; their normal defaults remain source textures and native DDS
when DDS is explicitly enabled. Gameplay behavior has not been changed.

## Selected softgles2 improvement

The short scalar DXT routines were themselves being auto-vectorized by GCC 16.
In these QEMU tests the small palette lookups and sixteen-pixel loops were much
slower than scalar instructions. The production change disables automatic
vectorization only for the color palette, alpha palette and scalar block decoder
on RISC-V GCC. Scalar block decoding is now the default. The explicit batched
RVV decoder remains available through `sgl_dxt_vector_decode=1` (or the build
define `SGL_DXT_VECTOR_DEFAULT=1`) for physical-hardware comparisons. Fragment
shading, rasterization, texture sampling and other RVV paths are unchanged.

The isolated tests compared complete production texture objects, with separate
storage, inside one QEMU process. After warmup they alternated order for eight
measured samples per format. At VLEN 1024 the median scalar candidate was
3.90–8.87 times faster across the four S3TC formats. Short timings still had large
outliers, so full-game results below carry more weight than those ratios.

Both default and explicit-RVV modes pass 601 independent-reference checks at
each of VLEN 128/256/512/1024, including mipmaps, cubemaps, alpha modes, edges,
invalid updates and compressed updates to generated mip levels.

## Full boots after tuning

Same configuration, asset snapshot and native DDS route as above:

| Measurement | Native DDS before | Tuned run 1 | Tuned run 2 |
|---|---:|---:|---:|
| Bulk texture phase | 8.701 s | 6.279 s | 5.785 s |
| Engine GL upload calls, inclusive | 4.487 s | 1.448 s | 1.313 s |
| Within softgles2, inclusive | 4.433 s | 1.388 s | 1.272 s |
| Map ready | 40.690 s | 47.033 s | 39.577 s |
| First map present | 78.311 s | 83.001 s | 78.085 s |

Softgles2 upload/decode time decreased 69–71%; bulk texture loading decreased
28–34%. Upload counts and bytes matched the original native run exactly.
No GL-error or invalid-DDS messages appeared in the tuned logs. Total boot time
did not establish a reliable improvement: earlier initialization phases varied,
and substantial initial-frame work remains. This is a loading-path improvement,
not evidence of increased steady-state FPS or physical-silicon performance.

gl4es's measured difference around the tuned upload calls was only 41–60 ms;
further invasive shim changes are not justified by this texture profile.

Tuned logs, milestones, nested timers and ELF SHA-256 are in
`build-rv/dds-boot-tuned/`. The pre-tuning executable is retained as
`build-rv/dds-boot-ab/baseline.elf`; the current rebuilt executable is
`build-rv/dhewm3`. Tests are `dds_texture_validate.py` (default and `--rvv`),
`dds_decode_ab.py`, and `dds_decode_pair.py` under `riscv/scripts/`.
All temporary snapshots were closed after measurement; the backing disk and
normal source-texture quality settings were not modified.

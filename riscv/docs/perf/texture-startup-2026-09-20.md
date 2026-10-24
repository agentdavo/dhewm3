# Texture startup investigation

Configuration: stock Windows QEMU 11.1.1, RVA23S64, four harts, VLEN1024,
full-resolution uncompressed source textures, 768x432. The previously named
"texture uploads" boot phase includes source file reads, ZIP/TGA decoding,
image programs, mip generation, engine processing and the actual GL upload.

## First instrumented pass

`build-rv/doom-texture-profile.log`, EndLevelLoad window only:

| Nested bucket | Seconds | Calls |
| --- | ---: | ---: |
| Entire image load | 34.527 | 1,945 |
| Source read/decode/image program | 25.974 | 1,938 |
| GenerateImage including mips/uploads | 4.716 | 1,938 |
| Mip generation | 0.521 | 14,009 |
| GL upload, including softgles2 | 2.349 | 16,277 |
| softgles2 upload, including copy | 1.655 | 16,277 |
| Texel copy/conversion | 1.351 | 16,277 |

These are nested, inclusive times and must not be summed. Uploaded RGBA
storage across levels was 431,453,896 bytes. Source/image-program work
accounted for 75.2% of the measured image-load time; all softgles2 upload
work was 4.8%. The GL-call time minus softgles2 time was approximately
0.694 s. Run-to-run host scheduling and caches affect the absolute values;
do not subtract buckets from another boot's 51.77 s phase measurement.

Slowest image loads included the dropship addnormals/heightmap program
(1.422 s), player body (0.723 s), berserk fist (0.681 s), and player arms
(0.629/0.626 s). These times include their source loading, not just normal
map arithmetic. Instrumentation now also splits TGA file read/inflate,
TGA load inclusive, heightmap conversion and addnormals.

## Instrumentation

Counters use the real DTB timebase and report once at EndLevelLoad and again
through first map present, capturing deferred image work. They count calls,
RGBA storage bytes and elapsed microseconds without per-texel timing or
per-upload logging. Eight slowest image names are retained in fixed storage.
Source instrumentation is limited to bare-metal builds; standalone renderer
payloads can omit the weak engine hooks. The current narrow test does not
profile every compressed/cubemap/background-loading path independently.

The full-quality build is deliberately retained. Restoring downsizing or
lossy DDS normal maps would change the workload and would not demonstrate
an upload optimization.

## Finer second pass

`build-rv/doom-texture-detail.log`, same configuration, EndLevelLoad window:

| Bucket | Seconds | Calls |
| --- | ---: | ---: |
| Entire image load, inclusive | 31.023 | 1,945 |
| Source/image-program work, inclusive | 23.205 | 1,938 |
| TGA file read (includes PK4 inflate) | 10.301 | 2,095 |
| TGA load including file read | 12.476 | 2,095 |
| Heightmap conversion | 4.191 | 129 |
| Normal-map combination | 6.616 | 115 |
| Image generation including mip/upload | 4.133 | 1,938 |
| Mip generation | 0.470 | 14,009 |
| GL upload including softgles2 | 2.010 | 16,277 |
| softgles2 upload including copy | 1.374 | 16,277 |
| Texel copy/conversion | 1.119 | 16,277 |

TGA load minus its file read is about 2.175 s. GL upload minus softgles2 is
about 0.637 s. These differences are approximate elapsed-time attribution,
not sampled CPU profiles. Storage reads/inflate and normal-map construction
are the primary opportunities; softgles2 upload is 4.4% of inclusive image
loading in this pass. No upload optimization has been claimed from the
run-to-run timing difference.

Priorities: investigate PK4 read/inflate cost, byte-equivalent vectorization
of heightmap/addnormals, or lossless offline preparation of normal maps and
mips. Keep the current full-colour results as the comparison reference.
The direct RGBA copy/conversion loop remains a smaller backend opportunity.


## Four-hart preparation and direct uploads

Retained implementation: `R_HeightmapToNormalMap` and `R_AddNormalMaps` dispatch
independent row ranges through the existing softgles2 worker pool. Allocations,
resampling, parsing, file access and GL publication remain on the main thread.
The dispatch joins before returning. Images below 16,384 pixels, fewer than four
rows, disabled parallel mode and nested scheduler use take the serial path.
No additional full-image buffers are allocated in normal operation. The optional
validation mode allocates one reference output at a time and is off by default.

`doom-texture-validate.log` reports 244 byte-exact comparisons: 129 height maps
and 115 normal blends, using four harts (three workers) at VLEN1024. This compares
the production parallel result with the same unchanged arithmetic run serially;
it checks partitioning/synchronization, not the mathematical model independently.

Two fresh boots of the same ELF, with reference comparisons disabled:

| EndLevelLoad bucket | Serial rows (s) | Four-hart rows (s) |
|---|---:|---:|
| Texture-loading phase, including progress rendering | 28.559 | 18.963 |
| Image loading, inclusive | 27.993 | 18.579 |
| Height maps | 3.773 | 1.050 |
| Normal blending | 5.851 | 1.512 |
| TGA file read/inflate | 9.353 | 8.159 |
| softgles2 upload, inclusive | 0.857 | 0.741 |
| Texel copy/conversion | 0.568 | 0.493 |

The directly affected normal work fell from 9.624 to 2.562 seconds (3.76x).
The loading phase fell by 9.596 seconds (33.6%); some of this difference is host
variation, as the unchanged TGA path also became faster. These are single-run
QEMU wall times, not physical-hardware CPU cycles. Both runs include the direct
RGBA memcpy improvement; this pair does not isolate its benefit.

Parallel boot reached map-ready at 62.829 s and first-map-present at 162.262 s.
The serial comparison stopped after map-ready, so there is no paired first-frame
speedup claim. About 99.4 s still lies between map-ready and first presentation.
The reception screenshot `build-rv/texture-parallel-wall.png` retains smooth
lighting and wall detail. A still image does not establish absence of tearing.

Reproduction (Windows/MSYS2, repo-local toolchain):

- `python riscv/scripts/texture_startup_probe.py validate` checks actual map data.
- `python riscv/scripts/texture_startup_probe.py serial` disables row dispatch.
- `python riscv/scripts/texture_startup_probe.py parallel` uses shipping defaults.
- Quit each QEMU before the next run; all logs stay under `build-rv`.
- The game ELF is VLEN1024-specific; these are not full-game tests at smaller VLENs.

One SDL launch stalled after save replay before texture work began and was
restarted. This preexisting intermittent launch problem is not counted as a
successful benchmark. Successful runs are recorded in `doom-texture-serial.log`
and `doom-texture-parallel.log`.

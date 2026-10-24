# True-colour audit, 2026-09-20

The reported wall posterization was visible in the QEMU screendump, despite
an RGBA8888 framebuffer. Doom reported r8/g8/b8/a8, depth24/stencil8; the
virtio resource uses R8G8B8A8_UNORM. A captured frame had 16,965 distinct RGB
colours and all 256 levels in each channel. This was not a palette display.

## Texture configuration

The old launch used the Low preset: DDS/compression enabled, diffuse cap256,
specular cap64, bump cap256 and material high-quality flags ignored. A live
reload using uncompressed, full-resolution source textures improved available
texture detail but did not remove the large colour bands.

The bare-metal texture-quality override now applies before renderer initialization, after startup
configs/command-line processing, and after machine presets. It disables
compression, precompressed DDS loading, normal compression, all three
downsize options, forced downsizing, high-quality flag suppression, compact
texture format selection and mip-level debug colours. Trilinear filtering
and zero LOD bias are selected. This leaves workload settings independent
of texture quality. Explicit runtime cvar changes still permit low-quality experiments.

The source autoexec uses gamma1.1/brightness1.0 and matching texture settings.
gl4es is explicitly instructed to avoid 16-bit upload conversions. softgles2
stores colour textures and the output framebuffer as RGBA8. Fog, diffuse,
bump, specular and shadows were enabled in the audited running game.
`bm_presentGamma=0` avoids a second gamma pass. `bm_dither=1` was already
active: its standalone test default of zero is overridden by the engine.

## Gamma arithmetic defect

The shared scalar/RVV log2 mantissa polynomial approached 0.6918785 at m=2,
instead of 1. Adding the integer base-two exponent produced discontinuities
at every power-of-two boundary. Gamma1.1 uses the general pow path and exposed
these as colour steps. Matching scalar/RVV approximations could not detect
this error with differential tests against each other.

Replace the mantissa approximation with the atanh series for log2, and extend
exp2 to sixth order. Guard exponent underflow against bit-wraparound. Keep
the existing RVV shortcuts for common integer powers.

`softgles2/test/colour_math_validate.c` compares 65,536 ramp samples for each
of five exponents against independent libm powf, checks finite results and
near-monotonicity, and limits absolute error to 0.000039 (0.01 of an RGBA8
step). Host scalar PASS: maximum error27 millionths. QEMU RVV PASS at VLEN
128/256/512/1024: maximum error11 millionths, or 0.002805 of a colour step.
Logs and the local runner are in `build-rv/colour-validation/`.

Full-resolution uncompressed textures increase memory consumption and may
increase cache misses. The live reload listed 439.7 MB while touching all
registered images; this is not a measured process-memory peak. Performance
comparisons require a fixed scene and should distinguish texture quality
from the gamma correction.

## Visual verification

The rebuilt game booted with compression, precompressed textures, normal
compression and downsizing all zero; gamma1.1 and fog remained enabled.
The wall at `setviewpos -700 -1380 176.5 270` showed smooth lighting after
the math correction. Before: `build-rv/colour-full-wall.png` (full-quality
textures with the old math). After: `build-rv/truecolour-wall.png`.
Some unavailable BFG/plasma weapon source images in the demo asset pack
still generate missing-image warnings; this does not establish full retail
asset coverage. The current ELF includes the subsequent boot-timing hooks.

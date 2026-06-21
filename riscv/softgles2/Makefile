# softgles2 v0 — host golden images + bare-metal RISC-V payloads.
#
#   make host    x86 reference triangle  -> build/tri.ppm
#   make rv      bare-metal RVV triangle payload
#   make glhost  x86 GLES2-API scene     -> build/gl.ppm
#   make glrv    bare-metal RVV GLES2-API payload
#   make glrun   boot the GLES2 payload on QEMU
#   make all     everything

# ---- host (x86, scalar path) ----
HOST_CC     ?= cc
HOST_CFLAGS := -O2 -fno-math-errno -Iinclude -Isrc -Itest -Wall -Wextra

# ---- RISC-V bare metal (RVV v1.0 path) ----
# Prefer the /opt/riscv GCC 16 toolchain (full RVV v1.0 intrinsics).
CROSS  ?= $(shell test -x /opt/riscv/bin/riscv64-unknown-elf-gcc && printf /opt/riscv/bin/riscv64-unknown-elf- || printf riscv64-unknown-elf-)
RVCC   := $(CROSS)gcc
RVSIZE := $(CROSS)size
# rva23s64 + explicit optional extensions, matched by run.sh's QEMU CPU
# line (vlen=1024, ELEN=64). Needs /opt/riscv GCC 16. NOTE: zvl1024b is MANDATORY
# for codegen fidelity — the SIMT VM and kernels are pinned to e32m2 (= 64 lanes =
# SGL_SIMT_LANES) which only holds at VLEN=1024; zvl256b strip-mines x4 and adds
# spurious vsetvls, so its disassembly does NOT represent the shipping build (RVV
# audit §1). Keep this aligned with riscv/toolchain/baremetal-rva23s64.cmake.
ARCH   ?= rva23s64_zbc_zbkb_zbkc_zbkx_zfh_zvfh_zvfbfwma_zvbc_zvkng_zvksg_zvl1024b
ABI    ?= lp64d

# -mrvv-vector-bits=zvl: VLEN is fixed at the zvl minimum (1024) on this target,
# so fold every runtime `csrr vlenb` into an immediate (RVV audit §2: simt_run_ir
# -8.8%, sgl_raster_tri_prog -22.6%). Matches the CMake shipping flags.
RVCFLAGS := -march=$(ARCH) -mabi=$(ABI) -mcmodel=medany -mrvv-vector-bits=zvl \
            -ffreestanding -nostdlib -fno-pic -fno-common \
            -fno-tree-loop-distribute-patterns -fno-math-errno \
            -O2 -g -Wall -Wextra -ffunction-sections -fdata-sections \
            -DSGL_BAREMETAL -Iinclude -Isrc -Itest -I..
RVLDFLAGS := -nostdlib -Wl,--gc-sections -Wl,-T../boot/linker.ld -lgcc

RT     := ../boot/start.S ../boot/runtime.c ../boot/sbi.c ../boot/trap.c \
          ../libc/string.c ../libc/printf.c ../libc/malloc.c
CORE   := src/fb.c src/sg_diag.c src/sg_raster.c
GLSLCORE := src/glsl.c src/glsl_lex.c src/glsl_parse.c src/glsl_ir.c src/glsl_simt.c src/sg_kernels.c
GLCORE := $(CORE) src/gles2.c $(GLSLCORE) \
          src/sg_mt.c src/sg_mt_prim.c src/sg_mt_tiles.c \
          src/sg_mt_raster.c src/sg_mt_sched.c \
          src/sg_state.c src/sg_vertex.c src/sg_frag.c src/sg_frame.c src/sg_framebin.c src/sg_prof.c \
          src/sg_buffer.c src/sg_texture.c src/sg_framebuffer.c \
          src/sg_shader.c src/sg_draw.c src/sg_clip.c

BUILD := build

FRAMEBENCH_MODES := vertex arith tex_nearest tex_bilinear \
	overdraw1 overdraw2 overdraw4 overdraw8 \
	tex64_clamp tex64_repeat tex512_clamp tex512_repeat tex2048_clamp tex2048_repeat \
	tex64_bilinear tex512_bilinear tex2048_bilinear \
	blend discard depth stencil colormask
FRAMEBENCH_TARGETS := $(addprefix framebench_,$(FRAMEBENCH_MODES))
FRAMEBENCH_RUN_TARGETS := $(addsuffix run,$(FRAMEBENCH_TARGETS))

.PHONY: all host nearclip nearcliprv nearcliprun nearclipglhost blendrv blendrun blendhost polyoffrv polyoffrun polyoffhost rv glhost glrv glsltest cubehost compressedhost fbohost stencilhost apihost glsl_glhost glsl_glrv glsl_glrun run glrun simtrv simtrun iakernrv iakernrun ovkrv ovkrun atestrv atestrun cmaskrv cmaskrun vtxrv vtxrun rvvmath rvvmathrun fbbenchrv fbbenchrun framebenchrv framebenchrun $(FRAMEBENCH_TARGETS) $(FRAMEBENCH_RUN_TARGETS) mt_layout_bench mt_contracts full fullrun hiz hizrun fp16 fp16run mtoverlay mtoverlayrun clean
all: host rv glhost glrv glsl_glhost glsl_glrv cubehost compressedhost fbohost stencilhost apihost full

mt_contracts: | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) src/sg_mt.c src/sg_mt_prim.c src/sg_mt_tiles.c src/sg_mt_sched.c test/mt_contracts.c -o $(BUILD)/mt_contracts
	./$(BUILD)/mt_contracts

mt_layout_bench: | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) src/sg_mt.c src/sg_mt_sched.c src/sg_mt_prim.c src/sg_mt_tiles.c test/mt_layout_bench.c -o $(BUILD)/mt_layout_bench

# Full standalone renderer demo: real GLSL (A2 IR), VBOs, two textures
# (bilinear+nearest), depth-tested overdraw, lambert lighting, alpha blend -> PNG.
full: | $(BUILD)
	$(RVCC) $(RVCFLAGS) $(RVLDFLAGS) $(RT) $(GLCORE) test/softgles2_full.c -o $(BUILD)/full_rv.elf
	@$(RVSIZE) $(BUILD)/full_rv.elf
fullrun: full
	../run.sh $(BUILD)/full_rv.elf

# Hierarchical-Z occlusion cull bench: front-to-back overdraw, hi-Z OFF vs ON.
hiz: | $(BUILD)
	$(RVCC) $(RVCFLAGS) $(RVLDFLAGS) $(RT) $(GLCORE) test/hiz_bench.c -o $(BUILD)/hiz_bench.elf
	@$(RVSIZE) $(BUILD)/hiz_bench.elf
hizrun: hiz
	../run.sh $(BUILD)/hiz_bench.elf

# fp16 (zvfh) shading bench: 2D-overlay archetype, fp32 64-lane vs fp16 128-lane.
fp16: | $(BUILD)
	$(RVCC) $(RVCFLAGS) $(RVLDFLAGS) $(RT) $(GLCORE) test/fp16_bench.c -o $(BUILD)/fp16_bench.elf
	@$(RVSIZE) $(BUILD)/fp16_bench.elf
fp16run: fp16
	../run.sh $(BUILD)/fp16_bench.elf

# Coverage-aware MT gate: gl4es-style 4-vertex fullscreen blit must engage harts.
mtoverlay: | $(BUILD)
	$(RVCC) $(RVCFLAGS) $(RVLDFLAGS) $(RT) $(GLCORE) test/mt_overlay_test.c -o $(BUILD)/mt_overlay_test.elf
	@$(RVSIZE) $(BUILD)/mt_overlay_test.elf
mtoverlayrun: mtoverlay
	../run.sh $(BUILD)/mt_overlay_test.elf

# A3 SIMT-on-RVV gate: validate SIMT executor vs scalar-IR oracle + measure (QEMU).
simtrv: | $(BUILD)
	$(RVCC) $(RVCFLAGS) $(RVLDFLAGS) $(RT) $(GLSLCORE) test/simt_validate.c -o $(BUILD)/simt_validate.elf
	@$(RVSIZE) $(BUILD)/simt_validate.elf
simtrun: simtrv
	../run.sh $(BUILD)/simt_validate.elf

# Interaction-archetype kernel proof: register-resident kernel vs the per-op VM
# (bit-exact + speedup) on a representative bump+diffuse+specular shader (QEMU).
iakernrv: | $(BUILD)
	$(RVCC) $(RVCFLAGS) $(RVLDFLAGS) $(RT) src/sg_diag.c $(GLSLCORE) test/iakern_validate.c -o $(BUILD)/iakern_validate.elf
	@$(RVSIZE) $(BUILD)/iakern_validate.elf
iakernrun: iakernrv
	../run.sh $(BUILD)/iakern_validate.elf

# Real gl4es-emitted interaction shader (id=2): classifier must bind the
# interaction_gl4es kernel; kernel output must be bit-identical to the VM.
iakgl4esrv: | $(BUILD)
	$(RVCC) $(RVCFLAGS) $(RVLDFLAGS) $(RT) src/sg_diag.c $(GLSLCORE) test/iakern_gl4es_validate.c -o $(BUILD)/iakern_gl4es_validate.elf
	@$(RVSIZE) $(BUILD)/iakern_gl4es_validate.elf
iakgl4esrun: iakgl4esrv
	../run.sh $(BUILD)/iakern_gl4es_validate.elf

# 2D-overlay kernel + coherent gather: classifier binds it, bit-identical vs VM.
ovkrv: | $(BUILD)
	$(RVCC) $(RVCFLAGS) -DSGL_GATHER_RUNTIME_TOGGLE $(RVLDFLAGS) $(RT) src/sg_diag.c $(GLSLCORE) test/overlay_validate.c -o $(BUILD)/overlay_validate.elf
	@$(RVSIZE) $(BUILD)/overlay_validate.elf
ovkrun: ovkrv
	../run.sh $(BUILD)/overlay_validate.elf

# alpha-test overlay (id=3) + 2-stage env-combine (id=8) kernels: bit-exact vs VM
# (incl. the discard kill mask for the alpha-test one). QEMU, uses glsl.c compiler.
atestrv: | $(BUILD)
	$(RVCC) $(RVCFLAGS) $(RVLDFLAGS) $(RT) src/sg_diag.c $(GLSLCORE) test/atest_validate.c -o $(BUILD)/atest_validate.elf
	@$(RVSIZE) $(BUILD)/atest_validate.elf
atestrun: atestrv
	../run.sh $(BUILD)/atest_validate.elf

# colour-mask shading early-out gate (Doom 3 stencil/shadow-volume passes): verify
# cmask==0 skips the fragment VM while depth-write still lands. QEMU, no glsl.c.
cmaskrv: | $(BUILD)
	$(RVCC) $(RVCFLAGS) $(RVLDFLAGS) $(RT) $(CORE) src/sg_prof.c test/cmask_validate.c -o $(BUILD)/cmask_validate.elf
	@$(RVSIZE) $(BUILD)/cmask_validate.elf
cmaskrun: cmaskrv
	../run.sh $(BUILD)/cmask_validate.elf

# GL_EXT_depth_bounds_test: dst-depth-bounds reject culls shading + stencil fill.
dbrv: | $(BUILD)
	$(RVCC) $(RVCFLAGS) $(RVLDFLAGS) $(RT) $(CORE) src/sg_prof.c test/depthbounds_validate.c -o $(BUILD)/depthbounds_validate.elf
	@$(RVSIZE) $(BUILD)/depthbounds_validate.elf
dbrun: dbrv
	../run.sh $(BUILD)/depthbounds_validate.elf

# C2 blend gate: real glBlendFunc/Equation/Color factors vs an independent
# GL-spec reference, across every factor + all three equations. QEMU.
blendrv: | $(BUILD)
	$(RVCC) $(RVCFLAGS) $(RVLDFLAGS) $(RT) $(GLCORE) test/blend_validate.c -o $(BUILD)/blend_validate.elf
	@$(RVSIZE) $(BUILD)/blend_validate.elf
blendrun: blendrv
	../run.sh $(BUILD)/blend_validate.elf

# #16 glPolygonOffset gate: coplanar decal wins/loses the depth test per the
# offset factor/units + enable. QEMU.
polyoffrv: | $(BUILD)
	$(RVCC) $(RVCFLAGS) $(RVLDFLAGS) $(RT) $(GLCORE) test/polyoff_validate.c -o $(BUILD)/polyoff_validate.elf
	@$(RVSIZE) $(BUILD)/polyoff_validate.elf
polyoffrun: polyoffrv
	../run.sh $(BUILD)/polyoff_validate.elf

# Host (x86) versions of the validation harnesses — fast, no QEMU. Same
# dual-mode sources as the *run targets above.
nearclipglhost: | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(GLCORE) test/nearclip_gl.c -o $(BUILD)/nearclip_gl_host -lm
	./$(BUILD)/nearclip_gl_host
blendhost: | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(GLCORE) test/blend_validate.c -o $(BUILD)/blend_validate_host -lm
	./$(BUILD)/blend_validate_host
polyoffhost: | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(GLCORE) test/polyoff_validate.c -o $(BUILD)/polyoff_validate_host -lm
	./$(BUILD)/polyoff_validate_host

# SIMT vertex (T&L) gate: validate glsl_run_vertex_simt vs scalar oracle + measure.
vtxrv: | $(BUILD)
	$(RVCC) $(RVCFLAGS) $(RVLDFLAGS) $(RT) $(GLSLCORE) src/sg_vertex.c test/vtx_validate.c -o $(BUILD)/vtx_validate.elf
	@$(RVSIZE) $(BUILD)/vtx_validate.elf
vtxrun: vtxrv
	../run.sh $(BUILD)/vtx_validate.elf

rvvmath: | $(BUILD)
	$(RVCC) $(RVCFLAGS) $(RVLDFLAGS) $(RT) test/rvv_math_test.c -o $(BUILD)/rvv_math_test.elf
	@$(RVSIZE) $(BUILD)/rvv_math_test.elf
rvvmathrun: rvvmath
	../run.sh $(BUILD)/rvv_math_test.elf

fbbenchrv: | $(BUILD)
	$(RVCC) $(RVCFLAGS) $(RVLDFLAGS) $(RT) $(CORE) test/fb_2048_bench.c -o $(BUILD)/fb_2048_bench.elf
	@$(RVSIZE) $(BUILD)/fb_2048_bench.elf
fbbenchrun: fbbenchrv
	../run.sh $(BUILD)/fb_2048_bench.elf

framebenchrv: | $(BUILD)
	$(RVCC) $(RVCFLAGS) $(RVLDFLAGS) $(RT) $(GLSLCORE) test/frame_2048_bench.c -o $(BUILD)/frame_2048_bench.elf
	@$(RVSIZE) $(BUILD)/frame_2048_bench.elf
framebenchrun: framebenchrv
	../run.sh $(BUILD)/frame_2048_bench.elf

$(FRAMEBENCH_TARGETS): framebench_%: | $(BUILD)
	$(RVCC) $(RVCFLAGS) -DFRAME_BENCH_MODE=\"$*\" $(RVLDFLAGS) $(RT) $(GLSLCORE) test/frame_2048_bench.c -o $(BUILD)/frame_2048_$*.elf
	@$(RVSIZE) $(BUILD)/frame_2048_$*.elf

$(FRAMEBENCH_RUN_TARGETS): framebench_%run: framebench_%
	../run.sh $(BUILD)/frame_2048_$*.elf

glsltest: | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(GLCORE) test/glsl_test.c -o $(BUILD)/glsl_test -lm
	./$(BUILD)/glsl_test

cubehost: | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(GLCORE) test/cube_host.c -o $(BUILD)/cube_host -lm
	./$(BUILD)/cube_host

compressedhost: | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(GLCORE) test/compressed_host.c -o $(BUILD)/compressed_host -lm
	./$(BUILD)/compressed_host

fbohost: | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(GLCORE) test/fbo_caps_host.c -o $(BUILD)/fbo_caps_host -lm
	./$(BUILD)/fbo_caps_host

stencilhost: | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(GLCORE) test/stencil_host.c -o $(BUILD)/stencil_host -lm
	./$(BUILD)/stencil_host

apihost: | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(GLCORE) test/api_surface_host.c -o $(BUILD)/api_surface_host -lm
	./$(BUILD)/api_surface_host

glsl_glhost: | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(GLCORE) test/glsl_gl_host.c -o $(BUILD)/glsl_gl_host -lm
	./$(BUILD)/glsl_gl_host

glsl_glrv: | $(BUILD)
	$(RVCC) $(RVCFLAGS) $(RVLDFLAGS) $(RT) $(GLCORE) test/glsl_gl_rv.c -o $(BUILD)/glsl_gl_rv.elf
	@$(RVSIZE) $(BUILD)/glsl_gl_rv.elf

glsl_glrun: glsl_glrv
	../run.sh $(BUILD)/glsl_gl_rv.elf

$(BUILD):
	mkdir -p $(BUILD)

host: | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(GLCORE) test/tri_host.c -o $(BUILD)/tri_host -lm
	./$(BUILD)/tri_host

# C3 near-plane clipper unit test (pure clip-space math, host, no QEMU).
nearclip: | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) src/sg_clip.c test/nearclip_validate.c -o $(BUILD)/nearclip -lm
	./$(BUILD)/nearclip

# C3 near-plane clip integration test: clip through the real pipeline equals
# directly rendering the pre-clipped polygon. QEMU (GLCORE host build is broken).
nearcliprv: | $(BUILD)
	$(RVCC) $(RVCFLAGS) $(RVLDFLAGS) $(RT) $(GLCORE) test/nearclip_gl.c -o $(BUILD)/nearclip_gl.elf
	@$(RVSIZE) $(BUILD)/nearclip_gl.elf
nearcliprun: nearcliprv
	../run.sh $(BUILD)/nearclip_gl.elf

rv: | $(BUILD)
	$(RVCC) $(RVCFLAGS) $(RVLDFLAGS) $(RT) $(CORE) test/tri_rv.c -o $(BUILD)/tri_rv.elf
	@$(RVSIZE) $(BUILD)/tri_rv.elf

glhost: | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) $(GLCORE) test/gl_host.c -o $(BUILD)/gl_host -lm
	./$(BUILD)/gl_host

glrv: | $(BUILD)
	$(RVCC) $(RVCFLAGS) $(RVLDFLAGS) $(RT) $(GLCORE) test/gl_rv.c -o $(BUILD)/gl_rv.elf
	@$(RVSIZE) $(BUILD)/gl_rv.elf

run: rv
	../run.sh $(BUILD)/tri_rv.elf

glrun: glrv
	../run.sh $(BUILD)/gl_rv.elf

clean:
	rm -rf $(BUILD)

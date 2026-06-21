# dOS (doomOS): bare-metal SDL3 payloads.
#
#   ./build-sdl3.sh      build libSDL3-riscv.a (run once / when SDL changes)
#   make sdl-smoke       link the SDL3 init smoke test payload (dummy driver)
#   make sdl-run         boot it on QEMU (run.sh's rva23s64 CPU line)
#   make sdl-dos         link the SDL3 -> gl4es -> softgles2 triangle payload
#   make sdl-dos-run     boot it on QEMU and capture the framebuffer dump

RVCROSS := /opt/riscv/bin/riscv64-unknown-elf-
RVCC    := $(RVCROSS)gcc
SIZE    := $(RVCROSS)size
ARCH    ?= rva23s64_zbc_zbkb_zbkc_zbkx_zfh_zvfh_zvfbfwma_zvbc_zvkng_zvksg_zvl256b
ABI     ?= lp64d
BOOT    := ../boot
SDLLIB  := ../sdl3/libSDL3-riscv.a
SG      := ../softgles2
GL4ES   := ../gl4es
BRIDGE  := ../gl4es-bridge
GL4ESLIB := $(GL4ES)/lib/libGL-riscv.a
BUILD   := build

RVCFLAGS  := -march=$(ARCH) -mabi=$(ABI) -mcmodel=medany -O2 -ffreestanding \
             -I.. -I../sdl3/include
RVLDFLAGS := -nostartfiles -Wl,-T$(BOOT)/linker.ld -Wl,--gc-sections

OBJS := $(BUILD)/start.o $(BUILD)/runtime.o $(BUILD)/sbi.o $(BUILD)/trap.o \
        $(BUILD)/dos_syscalls.o $(BUILD)/sdl_smoke.o

# --- gl4es -> softgles2 chain flags (mirror gl4es-bridge/Makefile) ---
SGFLAGS  := -march=$(ARCH) -mabi=$(ABI) -mcmodel=medany -O2 -ffreestanding \
            -fno-tree-loop-distribute-patterns -fno-math-errno \
            -I.. -I$(SG)/include -I$(SG)/src -I$(BRIDGE) -I$(GL4ES)/include
# Extra includes for the SDL "dos" payload: SDL headers + gl4es GL + softgles2 + bridge.
# -I. resolves the "video-dos/dos_gl.h" surface header (cwd = riscv/dos).
DOSCFLAGS := $(RVCFLAGS) -I. -I$(SG)/include -I$(BRIDGE) -I$(GL4ES)/include

# Namespaced softgles2 backend (sgl2_*), bare metal — same as gl4es-bridge.
SGBACKEND := $(BUILD)/sg_fb.o $(BUILD)/sg_raster.o $(BUILD)/sg_gles2.o $(BUILD)/sg_glsl.o

.PHONY: sdl-smoke sdl-run sdl-dos sdl-dos-run clean
$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/start.o:       $(BOOT)/start.S   | $(BUILD); $(RVCC) $(RVCFLAGS) -c $< -o $@
$(BUILD)/runtime.o:     $(BOOT)/runtime.c | $(BUILD); $(RVCC) $(RVCFLAGS) -c $< -o $@
$(BUILD)/sbi.o:         $(BOOT)/sbi.c     | $(BUILD); $(RVCC) $(RVCFLAGS) -c $< -o $@
$(BUILD)/trap.o:        $(BOOT)/trap.c    | $(BUILD); $(RVCC) $(RVCFLAGS) -c $< -o $@
$(BUILD)/dos_syscalls.o: dos_syscalls.c   | $(BUILD); $(RVCC) $(RVCFLAGS) -c $< -o $@
$(BUILD)/sdl_smoke.o:   test/sdl_smoke.c  | $(BUILD); $(RVCC) $(RVCFLAGS) -c $< -o $@

sdl-smoke: $(OBJS)
	$(RVCC) $(RVCFLAGS) $(RVLDFLAGS) $(OBJS) \
	  -Wl,--start-group $(SDLLIB) -lm -lc -lgcc -Wl,--end-group \
	  -o $(BUILD)/sdl_smoke.elf
	@$(SIZE) $(BUILD)/sdl_smoke.elf

sdl-run: sdl-smoke
	../run.sh $(BUILD)/sdl_smoke.elf

# --- SDL3 -> gl4es -> softgles2 triangle payload ---

# namespaced softgles2 backend objects (GLES2 entry points -> sgl2_*)
$(BUILD)/sg_%.o: $(SG)/src/%.c | $(BUILD)
	$(RVCC) $(SGFLAGS) -DSGL_BAREMETAL -include $(BRIDGE)/sgl2_names.h -c $< -o $@

$(BUILD)/bridge.o: $(BRIDGE)/bridge.c | $(BUILD)
	$(RVCC) $(SGFLAGS) -DSGL_BAREMETAL -c $< -o $@

$(BUILD)/dos_present.o: dos_present.c | $(BUILD)
	$(RVCC) $(SGFLAGS) -DSGL_BAREMETAL -I. -c $< -o $@

$(BUILD)/dos_virtio_gpu.o: dos_virtio_gpu.c | $(BUILD)
	$(RVCC) $(SGFLAGS) -DSGL_BAREMETAL -I. -c $< -o $@

$(BUILD)/sdl_dos.o: test/sdl_dos.c | $(BUILD)
	$(RVCC) $(DOSCFLAGS) -c $< -o $@

DOS_OBJS := $(BUILD)/start.o $(BUILD)/runtime.o $(BUILD)/sbi.o $(BUILD)/trap.o \
            $(BUILD)/dos_syscalls.o $(SGBACKEND) $(BUILD)/bridge.o \
            $(BUILD)/dos_present.o $(BUILD)/dos_virtio_gpu.o $(BUILD)/sdl_dos.o

sdl-dos: $(DOS_OBJS) $(SDLLIB) $(GL4ESLIB)
	$(RVCC) $(RVCFLAGS) $(RVLDFLAGS) $(DOS_OBJS) \
	  -Wl,--start-group $(SDLLIB) $(GL4ESLIB) -lm -lc -lgcc -Wl,--end-group \
	  -o $(BUILD)/sdl_dos.elf
	@$(SIZE) $(BUILD)/sdl_dos.elf

sdl-dos-run: sdl-dos
	../run.sh $(BUILD)/sdl_dos.elf

clean:
	rm -rf $(BUILD)

# gl4es <-> softgles2 bridge.
#
#   make gl4es        build the static gl4es libGL.a (host) via build-gl4es.sh
#   make host-smoke   build+run the desktop-GL -> gl4es -> softgles2 smoke test
#   make rv-smoke     build the bare-metal RISC-V bridge payload (no OS)
#   make rv-run       boot it on QEMU (run.sh's rva23s64 CPU line)
#
# The softgles2 backend is compiled with -include sgl2_names.h so its GLES2
# entry points become sgl2_* and do not collide with gl4es's desktop gl*.

SG     := ../softgles2
GL4ES  := ../gl4es
CC     ?= cc
BUILD  := build

CFLAGS         := -O2 -fno-math-errno -Wall -I$(SG)/include -I$(SG)/src -I. -I$(GL4ES)/include
BACKEND_CFLAGS := $(CFLAGS) -include sgl2_names.h

BACKEND := $(BUILD)/fb.o $(BUILD)/raster.o $(BUILD)/gles2.o $(BUILD)/glsl.o
GL4ES_LIB := $(GL4ES)/lib/libGL-host.a

# ---- bare-metal RISC-V payload (gl4es + softgles2 + newlib over SBI) ----
RVCROSS := /opt/riscv/bin/riscv64-unknown-elf-
RVCC    := $(RVCROSS)gcc
BOOT    := $(SG)/../boot
ARCH    ?= rva23s64_zbc_zbkb_zbkc_zbkx_zfh_zvfh_zvfbfwma_zvbc_zvkng_zvksg_zvl256b
ABI     ?= lp64d
RVCFLAGS := -march=$(ARCH) -mabi=$(ABI) -mcmodel=medany -O2 -ffreestanding \
            -fno-tree-loop-distribute-patterns -fno-math-errno \
            -I$(SG)/.. -I$(SG)/include -I$(SG)/src -I. -I$(GL4ES)/include
RVLDFLAGS := -nostartfiles -Wl,-T$(BOOT)/linker.ld -Wl,--gc-sections
RV_GL4ES := $(GL4ES)/lib/libGL-riscv.a
RV_BACKEND := $(BUILD)/rv_fb.o $(BUILD)/rv_raster.o $(BUILD)/rv_gles2.o $(BUILD)/rv_glsl.o

.PHONY: gl4es gl4es-rv host-smoke rv-smoke rv-run clean
gl4es:
	./build-gl4es.sh host
gl4es-rv:
	./build-gl4es.sh riscv

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: $(SG)/src/%.c | $(BUILD)
	$(CC) $(BACKEND_CFLAGS) -c $< -o $@

$(BUILD)/bridge.o: bridge.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

host-smoke: $(BACKEND) $(BUILD)/bridge.o test/bridge_smoke.c $(GL4ES_LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(BACKEND) $(BUILD)/bridge.o test/bridge_smoke.c $(GL4ES_LIB) -lm -o $(BUILD)/bridge_smoke
	./$(BUILD)/bridge_smoke

# namespaced softgles2 backend (sgl2_*), bare metal
$(BUILD)/rv_%.o: $(SG)/src/%.c | $(BUILD)
	$(RVCC) $(RVCFLAGS) -DSGL_BAREMETAL -include sgl2_names.h -c $< -o $@

rv-smoke: $(RV_BACKEND) | $(BUILD)
	$(RVCC) $(RVCFLAGS) -DSGL_BAREMETAL -c bridge.c          -o $(BUILD)/rv_bridge.o
	$(RVCC) $(RVCFLAGS)                  -c newlib_syscalls.c -o $(BUILD)/rv_syscalls.o
	$(RVCC) $(RVCFLAGS)                  -c $(BOOT)/runtime.c -o $(BUILD)/rv_runtime.o
	$(RVCC) $(RVCFLAGS)                  -c $(BOOT)/sbi.c     -o $(BUILD)/rv_sbi.o
	$(RVCC) $(RVCFLAGS)                  -c $(BOOT)/trap.c    -o $(BUILD)/rv_trap.o
	$(RVCC) $(RVCFLAGS)                  -c $(BOOT)/start.S   -o $(BUILD)/rv_start.o
	$(RVCC) $(RVCFLAGS)                  -c test/bridge_rv.c  -o $(BUILD)/rv_main.o
	$(RVCC) $(RVCFLAGS) $(RVLDFLAGS) \
	  $(BUILD)/rv_start.o $(BUILD)/rv_runtime.o $(BUILD)/rv_sbi.o $(BUILD)/rv_trap.o \
	  $(BUILD)/rv_syscalls.o $(RV_BACKEND) $(BUILD)/rv_bridge.o $(BUILD)/rv_main.o \
	  -Wl,--start-group $(RV_GL4ES) -lm -lc -lgcc -Wl,--end-group \
	  -o $(BUILD)/bridge_rv.elf
	@$(RVCROSS)size $(BUILD)/bridge_rv.elf

rv-run: rv-smoke
	../run.sh $(BUILD)/bridge_rv.elf

clean:
	rm -rf $(BUILD)

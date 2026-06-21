# dhewm3 RISC-V64 bare-metal payload.
#
#   make            build build/payload.elf
#   make run        build and boot it on QEMU (run.sh)
#   make clean
#
# Uses the bare-metal (newlib) cross toolchain by default.

# Prefer the /opt/riscv GCC 16 toolchain (full RVV v1.0 intrinsics + newlib
# libm/libstdc++); fall back to a system riscv64-unknown-elf- if absent.
CROSS  ?= $(shell test -x /opt/riscv/bin/riscv64-unknown-elf-gcc && printf /opt/riscv/bin/riscv64-unknown-elf- || printf riscv64-unknown-elf-)
CC      := $(CROSS)gcc
SIZE    := $(CROSS)size
OBJDUMP := $(CROSS)objdump

# rva23s64 + explicit optional extensions; mandatory for bare metal and matched
# by run.sh's QEMU CPU line (VLEN=1024, ELEN=64).
# Requires the /opt/riscv GCC 16 toolchain.
ARCH    ?= rva23s64_zbc_zbkb_zbkc_zbkx_zfh_zvfh_zvfbfmin_zvfbfwma_zvbc_zvkg_zvkned_zvknha_zvknhb_zvksc_zvksed_zvksg_zvksh_zvl1024b
ABI     ?= lp64d

CFLAGS  := -march=$(ARCH) -mabi=$(ABI) -mcmodel=medany \
           -ffreestanding -nostdlib -fno-pic -fno-common \
           -fno-tree-loop-distribute-patterns \
           -O2 -g -Wall -Wextra \
           -ffunction-sections -fdata-sections -I.

LDFLAGS := -nostdlib -Wl,--gc-sections -Wl,-Tboot/linker.ld -lgcc

SRCS := boot/start.S boot/runtime.c boot/sbi.c boot/trap.c boot/irq.c \
        libc/string.c libc/printf.c libc/malloc.c \
        kmain.c

BUILD := build
ELF   := $(BUILD)/payload.elf

.PHONY: all run demorun clean disasm
all: $(ELF)

$(ELF): $(SRCS) boot/linker.ld | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $(filter %.S %.c,$^) -o $@
	@echo "---- size ----"
	@$(SIZE) $@

$(BUILD):
	mkdir -p $(BUILD)

disasm: $(ELF)
	$(OBJDUMP) -d $(ELF) | less

run: $(ELF)
	./run.sh $(ELF)

# build + boot demo_mars_city1 on QEMU, capture instrumented serial log + frame PNG
# (engine ELF is built by the build-rv CMake tree, not this stage-1 Makefile).
demorun:
	./demorun.sh

clean:
	rm -rf $(BUILD)

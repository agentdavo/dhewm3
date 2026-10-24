# dOS SMP bring-up smoke test  (riscv/smp/)
# =========================================
#
#   make            build build/smp_smoke.elf
#   make run        boot it on QEMU (default SMP=2)
#   make run1       boot with -smp 1  (expect "1 harts up")
#   make run2       boot with -smp 2  (expect "2 harts up")
#   make run4       boot with -smp 4  (expect "4 harts up")
#   make runall     run the 1/2/4 matrix back to back
#   make clean      remove build/
#
# STRICT ISOLATION: this directory is self-contained. It reuses
# riscv/boot/{start.S,runtime.c,sbi.c,trap.c,linker.ld} and the boot/libc
# headers READ-ONLY, compiling them into THIS build dir. It does not touch
# neo/*, build-rv, riscv/engine/*, riscv/softgles2/*, riscv/gl4es*, riscv/dos/*.
#
# Mandatory ISA per the brief: rva23s64 plus the explicit extensions below,
# -mabi=lp64d -mcmodel=medany.

RVCROSS := /opt/riscv/bin/riscv64-unknown-elf-
RVCC    := $(RVCROSS)gcc
SIZE    := $(RVCROSS)size
ARCH    ?= rva23s64_zbc_zbkb_zbkc_zbkx_zfh_zvfh_zvfbfwma_zvbc_zvkng_zvksg_zvl256b
ABI     ?= lp64d
BOOT    := ../boot
LIBC    := ../libc
# run.sh lives at riscv/run.sh; this pointed at ../scripts, which does not
# exist, so every `make run*/srun*` target failed with "No such file".
SCRIPTS := ..
BUILD   := build

# Derived from ARCH's zvl<N>b, never hardcoded. run.sh defaults RVV_VLEN to 1024
# while these images are built zvl256b, and with -mrvv-vector-bits=zvl that
# mismatch is a hard panic at boot ("VLEN mismatch") -- every run target failed
# this way. Deriving it means changing ARCH cannot desynchronise the two.
RVV_VLEN ?= $(shell echo '$(ARCH)' | grep -oE 'zvl[0-9]+b' | grep -oE '[0-9]+')

# Per-hart sizing shared by C (kmain.c) and asm (secondary.S).
SMP_MAX_HARTS        := 8
SMP_HART_STACK_SIZE  := 65536
SMP_HART_SCRATCH_SIZE:= 256

SMPDEFS := -DSMP_MAX_HARTS=$(SMP_MAX_HARTS) \
           -DSMP_HART_STACK_SIZE=$(SMP_HART_STACK_SIZE) \
           -DSMP_HART_SCRATCH_SIZE=$(SMP_HART_SCRATCH_SIZE)

# -I.. so the reused boot sources find "libc/libc.h" and "boot/runtime.h" /
# "runtime.h" the same way they do in the engine build. -I. for smp_sbi.h.
RVCFLAGS  := -march=$(ARCH) -mabi=$(ABI) -mcmodel=medany -O2 -ffreestanding \
             -Wall -Wextra -I.. -I. -I$(BOOT) $(SMPDEFS)
RVLDFLAGS := -nostartfiles -Wl,-T$(BOOT)/linker.ld -Wl,--gc-sections

# Boot/libc objects shared by BOTH payloads (smoke test + scheduler test).
# fdt.o is REQUIRED, not optional: runtime.c's timebase/RAM/ISA discovery calls
# fdt_find_compatible/fdt_reg/fdt_next_node/... Leaving it out is a link error
# (the engine build hit the same gap; it only appeared to link there because
# --gc-sections happened to drop the timer path).
COMMON_OBJS := $(BUILD)/start.o $(BUILD)/runtime.o $(BUILD)/sbi.o $(BUILD)/trap.o \
               $(BUILD)/fdt.o \
               $(BUILD)/printf.o $(BUILD)/string.o $(BUILD)/malloc.o

# Smoke-test payload objects (unchanged).
OBJS := $(COMMON_OBJS) $(BUILD)/secondary.o $(BUILD)/kmain.o

# Scheduler-test payload objects (STEP 1 thread pool).
SCHED_OBJS := $(COMMON_OBJS) $(BUILD)/sched_secondary.o $(BUILD)/sched.o \
              $(BUILD)/sched_kmain.o

# "top" perf-monitor demo: scheduler + a virtio-console metrics channel (vcon).
TOP_OBJS := $(COMMON_OBJS) $(BUILD)/sched_secondary.o $(BUILD)/sched.o \
            $(BUILD)/vcon.o $(BUILD)/top_kmain.o

.PHONY: all run run1 run2 run4 runall clean \
        sched srun srun1 srun2 srun4 srunall \
        top trun trun1 trun2 trun4 trunall
all: $(BUILD)/smp_smoke.elf $(BUILD)/sched_test.elf $(BUILD)/top_test.elf
sched: $(BUILD)/sched_test.elf
top: $(BUILD)/top_test.elf

$(BUILD):
	mkdir -p $(BUILD)

# --- reused boot sources (READ-ONLY; compiled into this build dir) ---
$(BUILD)/start.o:    $(BOOT)/start.S   | $(BUILD); $(RVCC) $(RVCFLAGS) -c $< -o $@
$(BUILD)/runtime.o:  $(BOOT)/runtime.c | $(BUILD); $(RVCC) $(RVCFLAGS) -c $< -o $@
$(BUILD)/sbi.o:      $(BOOT)/sbi.c     | $(BUILD); $(RVCC) $(RVCFLAGS) -c $< -o $@
$(BUILD)/trap.o:     $(BOOT)/trap.c    | $(BUILD); $(RVCC) $(RVCFLAGS) -c $< -o $@
$(BUILD)/fdt.o:      $(BOOT)/fdt.c     | $(BUILD); $(RVCC) $(RVCFLAGS) -c $< -o $@

# --- reused libc (READ-ONLY) ---
$(BUILD)/printf.o:   $(LIBC)/printf.c  | $(BUILD); $(RVCC) $(RVCFLAGS) -c $< -o $@
$(BUILD)/string.o:   $(LIBC)/string.c  | $(BUILD); $(RVCC) $(RVCFLAGS) -c $< -o $@
$(BUILD)/malloc.o:   $(LIBC)/malloc.c  | $(BUILD); $(RVCC) $(RVCFLAGS) -c $< -o $@

# --- smp-local sources (smoke test) ---
$(BUILD)/secondary.o: secondary.S smp_sbi.h | $(BUILD); $(RVCC) $(RVCFLAGS) -c $< -o $@
$(BUILD)/kmain.o:     kmain.c smp_sbi.h     | $(BUILD); $(RVCC) $(RVCFLAGS) -c $< -o $@

# --- smp-local sources (scheduler test, STEP 1) ---
$(BUILD)/sched_secondary.o: sched_secondary.S                  | $(BUILD); $(RVCC) $(RVCFLAGS) -c $< -o $@
$(BUILD)/sched.o:           sched.c sched.h sched_sbi.h smp_sbi.h | $(BUILD); $(RVCC) $(RVCFLAGS) -c $< -o $@
$(BUILD)/sched_kmain.o:     sched_kmain.c sched.h smp_sbi.h    | $(BUILD); $(RVCC) $(RVCFLAGS) -c $< -o $@

# --- smp-local sources ("top" demo: virtio-console metrics + monitor) ---
$(BUILD)/vcon.o:      vcon.c vcon.h                  | $(BUILD); $(RVCC) $(RVCFLAGS) -c $< -o $@
$(BUILD)/top_kmain.o: top_kmain.c sched.h vcon.h smp_sbi.h | $(BUILD); $(RVCC) $(RVCFLAGS) -c $< -o $@

$(BUILD)/smp_smoke.elf: $(OBJS)
	$(RVCC) $(RVCFLAGS) $(RVLDFLAGS) $(OBJS) -lm -lc -lgcc -o $@
	@$(SIZE) $@

$(BUILD)/sched_test.elf: $(SCHED_OBJS)
	$(RVCC) $(RVCFLAGS) $(RVLDFLAGS) $(SCHED_OBJS) -lm -lc -lgcc -o $@
	@$(SIZE) $@

$(BUILD)/top_test.elf: $(TOP_OBJS)
	$(RVCC) $(RVCFLAGS) $(RVLDFLAGS) $(TOP_OBJS) -lm -lc -lgcc -o $@
	@$(SIZE) $@

# --- run targets (honour the existing run.sh: SMP + CPU + TIMEOUT env) ---
#
# NOTE: the OpenSBI cold-boot-lottery hang (start.S used to park any boot hart
# != 0) is now FIXED in riscv/boot/start.S (it boots whichever hart OpenSBI
# hands it, via an atomic boot-claim). Boots are reliable across -smp 1/2/4 and
# all lottery boot-harts, so the retry wrappers below are now just defensive
# belt-and-suspenders (they succeed on the first attempt).
RETRIES ?= 4
SMOKE_OK := SMP smoke:

define run_smp
	@ok=0; for i in $$(seq 1 $(RETRIES)); do \
	  echo ">>> [$(1) hart(s)] run.sh attempt $$i/$(RETRIES)"; \
	  if SMP=$(1) TIMEOUT=30 RVV_VLEN=$(RVV_VLEN) $(SCRIPTS)/run.sh $(BUILD)/smp_smoke.elf 2>&1 \
	       | tee /tmp/smp_run_$(1).log | grep -q "$(SMOKE_OK)"; then ok=1; break; fi; \
	  echo ">>> (boot hart != 0; retrying)"; \
	done; \
	if [ $$ok -ne 1 ]; then echo "FAILED after $(RETRIES) attempts"; exit 1; fi
endef

run: $(BUILD)/smp_smoke.elf
	RVV_VLEN=$(RVV_VLEN) $(SCRIPTS)/run.sh $(BUILD)/smp_smoke.elf

run1: $(BUILD)/smp_smoke.elf
	$(call run_smp,1)

run2: $(BUILD)/smp_smoke.elf
	$(call run_smp,2)

run4: $(BUILD)/smp_smoke.elf
	$(call run_smp,4)

runall: run1 run2 run4

# --- scheduler-test run targets (same retry-on-bad-boot-hart wrapper) ---
SCHED_OK := SCHED test: PASS

define run_sched
	@ok=0; for i in $$(seq 1 $(RETRIES)); do \
	  echo ">>> [sched, $(1) hart(s)] run.sh attempt $$i/$(RETRIES)"; \
	  if SMP=$(1) TIMEOUT=30 RVV_VLEN=$(RVV_VLEN) $(SCRIPTS)/run.sh $(BUILD)/sched_test.elf 2>&1 \
	       | tee /tmp/sched_run_$(1).log | grep -q "$(SCHED_OK)"; then ok=1; break; fi; \
	  echo ">>> (boot hart != 0; retrying)"; \
	done; \
	if [ $$ok -ne 1 ]; then echo "FAILED after $(RETRIES) attempts"; exit 1; fi
endef

srun: $(BUILD)/sched_test.elf
	RVV_VLEN=$(RVV_VLEN) $(SCRIPTS)/run.sh $(BUILD)/sched_test.elf

srun1: $(BUILD)/sched_test.elf
	$(call run_sched,1)

srun2: $(BUILD)/sched_test.elf
	$(call run_sched,2)

srun4: $(BUILD)/sched_test.elf
	$(call run_sched,4)

srunall: srun1 srun2 srun4

# --- "top" perf-monitor run targets ---
# METRICS=1 makes run.sh attach the virtio-console (-> metrics.log) so the
# per-second snapshots land on their own channel, off uart0. The uart0 mirror
# ([top] MET ...) is always present in the boot log regardless.
TOP_OK := top] done:

define run_top
	@ok=0; for i in $$(seq 1 $(RETRIES)); do \
	  echo ">>> [top, $(1) hart(s)] run.sh attempt $$i/$(RETRIES)"; \
	  if METRICS=1 SMP=$(1) TIMEOUT=45 $(SCRIPTS)/run.sh $(BUILD)/top_test.elf 2>&1 \
	       | tee /tmp/top_run_$(1).log | grep -q "$(TOP_OK)"; then ok=1; break; fi; \
	  echo ">>> (boot hart != 0; retrying)"; \
	done; \
	if [ $$ok -ne 1 ]; then echo "FAILED after $(RETRIES) attempts"; exit 1; fi
endef

trun: $(BUILD)/top_test.elf
	METRICS=1 $(SCRIPTS)/run.sh $(BUILD)/top_test.elf

trun1: $(BUILD)/top_test.elf
	$(call run_top,1)

trun2: $(BUILD)/top_test.elf
	$(call run_top,2)

trun4: $(BUILD)/top_test.elf
	$(call run_top,4)

trunall: trun1 trun2 trun4

clean:
	rm -rf $(BUILD)

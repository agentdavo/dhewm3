# dOS audio: bare-metal miniaudio mixer + OpenAL shim + virtio-snd sink.
#
#   ./fetch.sh           vendor miniaudio.h (run once)
#   make tone            link the tone-mix verify payload (serial sink, doc 13 §9)
#   make tone-run        boot it on QEMU and capture serial -> WAV
#   make tone-virtio-run boot it with the virtio-snd backend + -audiodev wav
#
# Build order / milestones: see docs/audio/00-audio.md §4-§5.
#
# LANDMINE (doc 20 §1.4): every audio TU MUST share the SAME MA_NO_* set. We get
# that from ma_config.h (included by each TU) AND the tone path needs generation,
# so DOS_AUDIO_WANT_GENERATION is passed to ALL TUs in the tone link.

RVCROSS := /opt/riscv/bin/riscv64-unknown-elf-
RVCC    := $(RVCROSS)gcc
SIZE    := $(RVCROSS)size
ARCH    ?= rva23s64_zbc_zbkb_zbkc_zbkx_zfh_zvfh_zvfbfwma_zvbc_zvkng_zvksg_zvl256b
ABI     ?= lp64d

BOOT    := ../boot
LIBC    := ../libc
DOS     := ../dos
FREESTD := ../gl4es-bridge/freestanding
BUILD   := build

# -I.. so "boot/runtime.h", "dos_audio.h" relative includes resolve; -I. for
# miniaudio.h / ma_config.h; -Iopenal for the shim-internal + AL/ headers.
RVCFLAGS := -march=$(ARCH) -mabi=$(ABI) -mcmodel=medany -O2 -g -ffreestanding \
            -Wall -Wextra -ffunction-sections -fdata-sections \
            -I. -I.. -Iopenal -I$(FREESTD)
RVLDFLAGS := -nostartfiles -Wl,-T$(BOOT)/linker.ld -Wl,--gc-sections

# tone harness needs ma_waveform -> generation enabled across the whole link
TONE_DEF := -DDOS_AUDIO_WANT_GENERATION

BOOT_OBJS := $(BUILD)/start.o $(BUILD)/runtime.o $(BUILD)/sbi.o $(BUILD)/trap.o \
             $(BUILD)/dos_syscalls.o $(BUILD)/printf.o $(BUILD)/string.o

.PHONY: tone tone-run tone-virtio-run clean
$(BUILD):
	mkdir -p $(BUILD)

# --- boot runtime + project libc (mirror dos/Makefile) ---
$(BUILD)/start.o:        $(BOOT)/start.S      | $(BUILD); $(RVCC) $(RVCFLAGS) -c $< -o $@
$(BUILD)/runtime.o:      $(BOOT)/runtime.c    | $(BUILD); $(RVCC) $(RVCFLAGS) -c $< -o $@
$(BUILD)/sbi.o:          $(BOOT)/sbi.c        | $(BUILD); $(RVCC) $(RVCFLAGS) -c $< -o $@
$(BUILD)/trap.o:         $(BOOT)/trap.c       | $(BUILD); $(RVCC) $(RVCFLAGS) -c $< -o $@
$(BUILD)/dos_syscalls.o: $(DOS)/dos_syscalls.c | $(BUILD); $(RVCC) $(RVCFLAGS) -c $< -o $@
$(BUILD)/printf.o:       $(LIBC)/printf.c     | $(BUILD); $(RVCC) $(RVCFLAGS) -c $< -o $@
$(BUILD)/string.o:       $(LIBC)/string.c     | $(BUILD); $(RVCC) $(RVCFLAGS) -c $< -o $@

# --- audio TUs ---
$(BUILD)/miniaudio_impl.o: miniaudio_impl.c | $(BUILD); $(RVCC) $(RVCFLAGS) $(TONE_DEF) -c $< -o $@
$(BUILD)/ma_stubs.o:       ma_stubs.c       | $(BUILD); $(RVCC) $(RVCFLAGS) -c $< -o $@
$(BUILD)/dos_audio.o:      dos_audio.c      | $(BUILD); $(RVCC) $(RVCFLAGS) $(TONE_DEF) -c $< -o $@
$(BUILD)/dos_audio_virtio.o: dos_audio.c    | $(BUILD); $(RVCC) $(RVCFLAGS) $(TONE_DEF) -DDOS_AUDIO_VIRTIO -c $< -o $@
$(BUILD)/dos_virtio_snd.o: virtio-snd/dos_virtio_snd.c | $(BUILD); $(RVCC) $(RVCFLAGS) -c $< -o $@
$(BUILD)/al_shim.o:        openal/al.c      | $(BUILD); $(RVCC) $(RVCFLAGS) $(TONE_DEF) -c $< -o $@
$(BUILD)/tone_mix.o:       test/tone_mix.c  | $(BUILD); $(RVCC) $(RVCFLAGS) $(TONE_DEF) -c $< -o $@

# --- tone-mix payload: serial sink (no QEMU audiodev needed) ---
TONE_OBJS := $(BOOT_OBJS) $(BUILD)/miniaudio_impl.o $(BUILD)/ma_stubs.o \
             $(BUILD)/dos_audio.o $(BUILD)/tone_mix.o

tone: $(TONE_OBJS)
	$(RVCC) $(RVCFLAGS) $(RVLDFLAGS) $(TONE_OBJS) \
	  -Wl,--start-group -lm -lc -lgcc -Wl,--end-group \
	  -o $(BUILD)/tone_mix.elf
	@$(SIZE) $(BUILD)/tone_mix.elf

tone-run: tone
	../run.sh $(BUILD)/tone_mix.elf

# --- tone-mix payload: virtio-snd sink (boot with -audiodev wav,...,streams=1) ---
TONE_VIRTIO_OBJS := $(BOOT_OBJS) $(BUILD)/miniaudio_impl.o $(BUILD)/ma_stubs.o \
                    $(BUILD)/dos_audio_virtio.o $(BUILD)/dos_virtio_snd.o $(BUILD)/tone_mix.o

tone-virtio: $(TONE_VIRTIO_OBJS)
	$(RVCC) $(RVCFLAGS) $(RVLDFLAGS) $(TONE_VIRTIO_OBJS) \
	  -Wl,--start-group -lm -lc -lgcc -Wl,--end-group \
	  -o $(BUILD)/tone_mix_virtio.elf
	@$(SIZE) $(BUILD)/tone_mix_virtio.elf

tone-virtio-run: tone-virtio
	AUDIO_WAV=/tmp/dos.wav ../run.sh $(BUILD)/tone_mix_virtio.elf

clean:
	rm -rf $(BUILD)

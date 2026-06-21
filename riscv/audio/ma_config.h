/*
 * ma_config.h — the SINGLE source of truth for miniaudio's MA_NO_* build set.
 *
 * THE LANDMINE (doc 20 §1.4): ma_engine_config / ma_engine contain
 *   #if !defined(MA_NO_DEVICE_IO) / #if !defined(MA_NO_RESOURCE_MANAGER)
 * struct fields. Every TU that includes "miniaudio.h" MUST use the IDENTICAL
 * MA_NO_* set, or the struct layouts skew between the implementation TU and the
 * consumer, field writes land at the wrong offset, and ma_engine_init() silently
 * returns -2 (MA_INVALID_ARGS). Measured skew on this toolchain:
 *   sizeof(ma_engine_config): 0xd8 WITH the set  vs 0x108 WITHOUT.
 *
 * So: include THIS header (not bare miniaudio.h) before any miniaudio use, OR
 * pass these as -D flags applied to ALL audio targets. Keep both in sync.
 *
 * NOTE on MA_NO_GENERATION: the OpenAL shim path (dhewm3 supplies its own PCM)
 * CAN define MA_NO_GENERATION. The tone-mix verify harness uses ma_waveform, so
 * it must NOT. Both are valid — but the set must be identical across the TUs
 * that are LINKED TOGETHER. The tone harness + impl form one link with
 * generation enabled; the shim payload + impl form another with it disabled.
 * Control it via the build (define DOS_AUDIO_WANT_GENERATION when linking the
 * tone harness) rather than per-TU drift.
 */
#ifndef DOS_MA_CONFIG_H
#define DOS_MA_CONFIG_H

#define MA_NO_DEVICE_IO          /* no backends / ma_device / ma_context        */
#define MA_NO_THREADING          /* legal ONLY with MA_NO_DEVICE_IO             */
#define MA_NO_ENCODING           /* never write files                          */
#define MA_NO_DECODING           /* dhewm3 decodes; auto-enables NO_RESOURCE_MANAGER */
#define MA_NO_WAV
#define MA_NO_FLAC
#define MA_NO_MP3
#define MA_NO_RUNTIME_LINKING    /* no dlopen (dlfcn shim is inert anyway)      */

#ifndef DOS_AUDIO_WANT_GENERATION
#define MA_NO_GENERATION         /* no ma_waveform/ma_noise (shim supplies PCM) */
#endif

/*
 * GCC-16 + freestanding newlib gotcha (doc 20 §1.3): newlib only declares
 * nanosleep under _POSIX_TIMERS, which the bare-metal config does NOT define;
 * miniaudio's ma_sleep__posix() references nanosleep unconditionally (compiled,
 * never executed under MA_NO_THREADING). GCC-16 makes implicit-declaration a
 * hard ERROR, so predeclare it here, before "miniaudio.h" is pulled in. The
 * matching link stub lives in ma_stubs.c.
 */
struct timespec;
int nanosleep(const struct timespec *req, struct timespec *rem);

#endif /* DOS_MA_CONFIG_H */

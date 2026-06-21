/*
 * tone_mix.c — the canonical headless audio verify (00-audio.md §4 step 2-3,
 * doc 13 §9 ladder rung 2). The audio twin of bridge-triangle.png.
 *
 * Builds a device-less ma_engine, plays a 440 Hz sine via ma_waveform, pulls
 * N mix blocks with ma_engine_read_pcm_frames, and pushes them through the
 * dos_audio sink (serial backend by default -> host dos_pcm2wav.py rebuilds a
 * WAV). PROVEN producer side in doc 20 §1.6 (peak 9830, RMS 6956); this version
 * routes through the dos_audio seam instead of inline base64.
 *
 * Build (tone path needs ma_waveform, so define DOS_AUDIO_WANT_GENERATION for
 * EVERY audio TU in this link — see ma_config.h landmine note):
 *   gcc ... -DDOS_AUDIO_WANT_GENERATION miniaudio_impl.c ma_stubs.c \
 *           dos_audio.c tone_mix.c  (+ boot runtime + libc)
 *
 * Verify:
 *   TIMEOUT=20 ./scripts/run.sh build/tone_mix.elf 2>&1 | tee /tmp/snd.log
 *   ./scripts/dos_pcm2wav.py /tmp/snd.log /tmp/out.wav
 *   ffprobe /tmp/out.wav      # 44100 Hz, 2ch, ~N*4096/44100 s, 440 Hz tone
 */
#include "ma_config.h"      /* IDENTICAL MA_NO_* set as miniaudio_impl.c */
#include "miniaudio.h"

#include "dos_audio.h"
#include "boot/runtime.h"   /* kmain, sbi_console_write */

#include <stdint.h>

#define TONE_BLOCKS  16     /* 16 * 4096 frames @ 44100 ≈ 1.49 s of audio */
#define TONE_FRAMES  4096   /* MIXBUFFER_SAMPLES */
#define TONE_CH      2

static void log_line(const char *s)
{
    unsigned long n = 0;
    while (s[n]) ++n;
    sbi_console_write(s, n);
}

int kmain(void)
{
    ma_engine engine;
    ma_engine_config ecfg;
    dos_audio_spec want, granted;
    int blk;

    log_line("[audio] tone_mix: device-less ma_engine -> dos_audio sink\n");

    ecfg = ma_engine_config_init();
    ecfg.noDevice           = MA_TRUE;
    ecfg.channels           = TONE_CH;
    ecfg.sampleRate         = 44100;
    ecfg.listenerCount      = 1;
    ecfg.periodSizeInFrames = TONE_FRAMES;
    if (ma_engine_init(&ecfg, &engine) != MA_SUCCESS) {
        log_line("[audio] ma_engine_init FAILED (check identical MA_NO_* set)\n");
        return 1;
    }

#ifdef DOS_AUDIO_WANT_GENERATION
    {
        ma_waveform wave;
        ma_sound    sound;
        ma_waveform_config wcfg = ma_waveform_config_init(
            ma_format_f32, TONE_CH, 44100, ma_waveform_type_sine, 0.3, 440.0);
        ma_waveform_init(&wcfg, &wave);
        ma_sound_init_from_data_source(&engine, &wave, 0, NULL, &sound);
        ma_sound_set_looping(&sound, MA_TRUE);
        ma_sound_start(&sound);
#endif

        /* open the sink as F32 (engine native); serial backend tags f32 (doc 13 §4.1) */
        want.rate     = 44100;
        want.channels = TONE_CH;
        want.format   = DOS_AUDIO_F32;
        if (dos_audio_open(&want, &granted) != 0) {
            log_line("[audio] dos_audio_open FAILED\n");
            return 2;
        }

        for (blk = 0; blk < TONE_BLOCKS; ++blk) {
            static float scratch[TONE_CH * TONE_FRAMES];
            ma_uint64 got = 0;
            ma_engine_read_pcm_frames(&engine, scratch, TONE_FRAMES, &got);
            if (got) dos_audio_write(scratch, (uint32_t)got);
        }

        dos_audio_close();      /* emits DOSPCMEND so the host can close the WAV */

#ifdef DOS_AUDIO_WANT_GENERATION
        ma_sound_uninit(&sound);
        ma_waveform_uninit(&wave);
    }
#endif

    ma_engine_uninit(&engine);
    log_line("[audio] tone_mix done; powering off.\n");
    return 0;
}

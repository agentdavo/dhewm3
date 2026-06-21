/*
 * dos_audio.c — dOS audio sink backends (doc 13 §3 / §4).
 *
 * ONE file, two backends selected at compile/link time:
 *   default            -> serial PCM dump  (backend B, doc 13 §3): base64 over
 *                         SBI DBCN, framed by DOSPCM/DOSPCMEND markers; host
 *                         dos_pcm2wav.py rebuilds a .wav. No QEMU audiodev.
 *   -DDOS_AUDIO_VIRTIO -> virtio-snd        (backend A, doc 13 §4): thin glue over
 *                         the virtio-snd MMIO driver (virtio-snd/dos_virtio_snd.c).
 *
 * Exactly one dos_audio_* set is linked per payload. The dos_audio_pump() that
 * reads ma_engine and calls dos_audio_write() lives in the OpenAL shim
 * (openal/al.c), where ma_engine is owned.
 *
 * STATUS: serial backend = scaffold of the proven path (doc 20 §1.6 dumped PCM
 * over DBCN successfully; this generalizes it behind the seam). virtio backend =
 * SKELETON; depends on the doc-12 driver (compile-oriented TODOs below).
 */
#include "dos_audio.h"
#include <string.h>

/* SBI console for the serial dump path (boot/runtime.h: sbi_console_write). */
#include "boot/runtime.h"

/* ===================================================================== *
 *  Backend B — serial PCM dump (default)                                *
 * ===================================================================== */
#ifndef DOS_AUDIO_VIRTIO

static dos_audio_spec g_spec;
static int            g_open;
static uint64_t       g_total_bytes;

/* base64, standard alphabet (matches dos_pcm2wav.py b64decode). */
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* 76-col line buffer + a 0..2 byte triplet carry that persists across write()
 * calls so the concatenated base64 stays one clean blob (doc 13 §3.2 trick). */
static char    g_line[80];
static int     g_col;
static uint8_t g_rem[3];
static int     g_remn;

static void emit(const char *s, unsigned long n)
{
    sbi_console_write(s, n);
}

/* tiny unsigned -> decimal (avoid sprintf dependency in the hot/serial path). */
static int u64_to_dec(char *out, uint64_t v)
{
    char tmp[24];
    int  n = 0, i;
    if (v == 0) { out[0] = '0'; return 1; }
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
    return n;
}

static void b64_putc(char c)
{
    g_line[g_col++] = c;
    if (g_col == 76) {
        g_line[g_col++] = '\n';
        emit(g_line, (unsigned long)g_col);
        g_col = 0;
    }
}

static void b64_triplet(uint8_t a, uint8_t b, uint8_t c)
{
    uint32_t v = ((uint32_t)a << 16) | ((uint32_t)b << 8) | (uint32_t)c;
    b64_putc(B64[(v >> 18) & 63]);
    b64_putc(B64[(v >> 12) & 63]);
    b64_putc(B64[(v >> 6) & 63]);
    b64_putc(B64[v & 63]);
}

/* Encode n bytes, carrying a 0..2 byte remainder between calls so triplets never
 * straddle a write() with mid-stream padding. */
static void emit_b64(const uint8_t *p, size_t n)
{
    size_t i = 0;

    /* fill the carry up to a full triplet first */
    while (g_remn && g_remn < 3 && i < n) g_rem[g_remn++] = p[i++];
    if (g_remn == 3) {
        b64_triplet(g_rem[0], g_rem[1], g_rem[2]);
        g_remn = 0;
    }

    /* whole triplets */
    while (i + 3 <= n) {
        b64_triplet(p[i], p[i + 1], p[i + 2]);
        i += 3;
    }

    /* stash 0..2 leftover bytes for next call / drain */
    while (i < n) g_rem[g_remn++] = p[i++];
}

int dos_audio_open(const dos_audio_spec *want, dos_audio_spec *granted)
{
    char hdr[64];
    int  n = 0;
    const char *fmt = (want->format == DOS_AUDIO_S16) ? "s16" : "f32";
    int  bps = (want->format == DOS_AUDIO_S16) ? 2 : 4;

    g_spec = *want;
    g_open = 1;
    g_total_bytes = 0;
    g_col = 0;
    g_remn = 0;

    /* "DOSPCM <rate> <channels> <fmt> <bps>\n" */
    memcpy(hdr + n, "DOSPCM ", 7); n += 7;
    n += u64_to_dec(hdr + n, want->rate);     hdr[n++] = ' ';
    n += u64_to_dec(hdr + n, want->channels); hdr[n++] = ' ';
    memcpy(hdr + n, fmt, 3); n += 3;          hdr[n++] = ' ';
    n += u64_to_dec(hdr + n, (uint64_t)bps);  hdr[n++] = '\n';
    emit(hdr, (unsigned long)n);

    if (granted) *granted = *want;
    return 0;
}

int dos_audio_write(const void *pcm, uint32_t frames)
{
    int    bps;
    size_t bytes;
    if (!g_open) return -1;
    bps   = (g_spec.format == DOS_AUDIO_S16) ? 2 : 4;
    bytes = (size_t)frames * g_spec.channels * (size_t)bps;
    emit_b64((const uint8_t *)pcm, bytes);
    g_total_bytes += bytes;
    return (int)frames;            /* serial never back-pressures */
}

int dos_audio_writable(void)
{
    return 0x7fffffff;
}

void dos_audio_drain(void)
{
    char tr[48];
    int  n = 0;
    if (!g_open) return;

    /* flush final 1..2 byte group with base64 padding */
    if (g_remn == 1) {
        uint32_t v = (uint32_t)g_rem[0] << 16;
        b64_putc(B64[(v >> 18) & 63]);
        b64_putc(B64[(v >> 12) & 63]);
        b64_putc('=');
        b64_putc('=');
        g_remn = 0;
    } else if (g_remn == 2) {
        uint32_t v = ((uint32_t)g_rem[0] << 16) | ((uint32_t)g_rem[1] << 8);
        b64_putc(B64[(v >> 18) & 63]);
        b64_putc(B64[(v >> 12) & 63]);
        b64_putc(B64[(v >> 6) & 63]);
        b64_putc('=');
        g_remn = 0;
    }
    if (g_col) { g_line[g_col++] = '\n'; emit(g_line, (unsigned long)g_col); g_col = 0; }

    /* "\nDOSPCMEND <total_bytes>\n" (leading \n separates from base64) */
    tr[n++] = '\n';
    memcpy(tr + n, "DOSPCMEND ", 10); n += 10;
    n += u64_to_dec(tr + n, g_total_bytes);
    tr[n++] = '\n';
    emit(tr, (unsigned long)n);
}

void dos_audio_close(void)
{
    if (g_open) { dos_audio_drain(); g_open = 0; }
}

/* ===================================================================== *
 *  Backend A — virtio-snd (compile with -DDOS_AUDIO_VIRTIO)             *
 * ===================================================================== */
#else  /* DOS_AUDIO_VIRTIO */

#include "virtio-snd/dos_virtio_snd.h"

static dos_audio_spec g_spec;
static dos_audio_format g_shim_format;   /* format the shim hands write() */

int dos_audio_open(const dos_audio_spec *want, dos_audio_spec *granted)
{
    /* virtio-snd PCM streams negotiate from a fixed (fmt,rate) enum. QEMU
     * virtio-sound-device advertises S16_LE; we convert F32->S16 in write().
     * Stream 0 is OUTPUT (doc 12 §0 direction rule). */
    vsnd_stream_params p;
    p.rate          = want->rate;
    p.channels      = (uint8_t)want->channels;
    p.fmt           = VSND_PCM_FMT_S16;
    p.period_frames = 1024;            /* ~23 ms @ 44.1k; >=4 periods buffered (doc 13 §4.2) */

    g_shim_format = want->format;

    if (vsnd_probe() != 0)                              return -1;
    if (vsnd_open_stream(VSND_DIR_OUTPUT, &p) != 0)     return -2;

    g_spec        = *want;
    g_spec.format = DOS_AUDIO_S16;     /* device format on the wire */
    if (granted) *granted = g_spec;

    /* TODO(doc 12 §6): pre-buffer 2-4 TX periods before start for a clean start. */
    if (vsnd_stream_start(VSND_DIR_OUTPUT) != 0)        return -3;
    return 0;
}

int dos_audio_write(const void *pcm, uint32_t frames)
{
    int16_t *scratch = vsnd_tx_scratch();   /* per-period staging, sized in open() */
    const int16_t *src;

    if (g_shim_format == DOS_AUDIO_F32) {
        /* F32 -> S16 clamp. TODO: route through ma_pcm_f32_to_s16 or the RVV
         * f32_to_s16_clamped kernel (doc 13 §6) once wired. */
        const float *f = (const float *)pcm;
        size_t count = (size_t)frames * g_spec.channels;
        size_t i;
        for (i = 0; i < count; ++i) {
            float v = f[i] * 32767.0f;
            if (v > 32767.0f)  v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            scratch[i] = (int16_t)v;
        }
        src = scratch;
    } else {
        src = (const int16_t *)pcm;
    }

    return vsnd_tx(src, frames);            /* enqueue on TX virtq; returns frames */
}

int dos_audio_writable(void)
{
    return vsnd_tx_avail_frames();
}

void dos_audio_drain(void)
{
    vsnd_tx_drain();
}

void dos_audio_close(void)
{
    vsnd_stream_stop(VSND_DIR_OUTPUT);
    vsnd_close();
}

#endif /* DOS_AUDIO_VIRTIO */

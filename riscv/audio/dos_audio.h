/*
 * dos_audio.h — dOS audio sink seam (doc 13 §2).
 *
 * The narrow C boundary between miniaudio's mixed PCM and the hardware (or
 * capture) sink. Audio analogue of the framebuffer sink (dos_present.c ->
 * virtio-gpu scanout). Single producer (the OpenAL shim pump, main thread).
 * No locking: bare metal, no audio thread.
 *
 * The active backend is chosen at LINK time (one dos_audio.c built with or
 * without DOS_AUDIO_VIRTIO), not at runtime — mirrors how dos_present.c is the
 * single FB sink linked into a payload. Default = serial dump.
 */
#ifndef DOS_AUDIO_H
#define DOS_AUDIO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PCM sample formats the sink accepts. miniaudio mixes in F32; the virtio path
 * converts to S16 (most QEMU virtio-snd jacks negotiate S16_LE). The serial dump
 * path emits whatever it is told and tags the format in the header. */
typedef enum {
    DOS_AUDIO_F32 = 0,   /* 32-bit float, interleaved, [-1,1] */
    DOS_AUDIO_S16 = 1    /* 16-bit signed, interleaved        */
} dos_audio_format;

typedef struct {
    uint32_t         rate;       /* e.g. 44100                       */
    uint32_t         channels;   /* 2 (stereo) typical; 6 for 5.1    */
    dos_audio_format format;     /* sample format of frames in write */
} dos_audio_spec;

/* Open the sink. Returns 0 on success, <0 on error. The backend may narrow the
 * request (virtio negotiates a supported rate/format) and report the granted
 * spec via *granted (may be NULL). */
int  dos_audio_open(const dos_audio_spec *want, dos_audio_spec *granted);

/* Push 'frames' interleaved frames (frames * channels samples) to the sink.
 * Returns frames accepted (>=0), or <0 on fatal error. May accept fewer than
 * requested if the sink ring is full (caller retries next tick). The serial
 * backend always accepts all frames. ALLOC-FREE: backend rings are sized in open(). */
int  dos_audio_write(const void *pcm, uint32_t frames);

/* Frames the sink can accept right now without blocking (virtio: free TX-ring
 * frames; serial: INT32_MAX). Lets the pump size its read to avoid overflow. */
int  dos_audio_writable(void);

/* Flush/finalise. Serial: emit the trailing DOSPCMEND marker so the host script
 * can close the WAV. Virtio: drain the TX queue. close() implies it. */
void dos_audio_drain(void);

/* Tear down. After close, open() may be called again. */
void dos_audio_close(void);

/* The pump (lives in the shim conceptually; declared here for the seam, doc 13
 * §5.2). Called once per sound update from the shim's alcProcessContext. Pulls
 * all frames the sink can take right now from the engine and forwards them.
 * Implemented in the OpenAL shim (openal/al.c) where ma_engine lives. */
void dos_audio_pump(void);

#ifdef __cplusplus
}
#endif
#endif /* DOS_AUDIO_H */

/*
 * dos_virtio_snd.h — bare-metal virtio-sound (virtio-MMIO) playback driver
 * surface (doc 12). The dos_audio.c virtio backend calls only this vsnd_* API;
 * all MMIO ring / control-protocol detail lives in dos_virtio_snd.c.
 *
 * Scope: discover the device on the virt MMIO bus, hand-shake, set up the
 * control + TX virtqueues, drive ONE stereo S16 48 kHz (or 44.1 kHz) OUTPUT
 * stream (stream id 0), push mixed PCM. Polling only — no PLIC/IRQ.
 */
#ifndef DOS_VIRTIO_SND_H
#define DOS_VIRTIO_SND_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PCM format selectors (subset of VIRTIO_SND_PCM_FMT_*, doc 12 §5.2). */
typedef enum {
    VSND_PCM_FMT_S16   = 5,    /* VIRTIO_SND_PCM_FMT_S16   */
    VSND_PCM_FMT_FLOAT = 19    /* VIRTIO_SND_PCM_FMT_FLOAT */
} vsnd_pcm_fmt;

/* Stream direction (doc 12 §5: D_OUTPUT=0, D_INPUT=1). Stream 0 is OUTPUT. */
typedef enum {
    VSND_DIR_OUTPUT = 0,
    VSND_DIR_INPUT  = 1
} vsnd_dir;

typedef struct {
    uint32_t     rate;            /* e.g. 44100 or 48000             */
    uint8_t      channels;        /* 2                               */
    vsnd_pcm_fmt fmt;             /* VSND_PCM_FMT_S16                */
    uint32_t     period_frames;   /* e.g. 1024; TX chunk granularity */
} vsnd_stream_params;

/* --- lifecycle (doc 12 §3-§5) --- */

/* Scan the 8 virt MMIO slots for DeviceID==25, run the virtio handshake,
 * negotiate VIRTIO_F_VERSION_1, read config, set up CONTROL(0)+TX(2) vqueues,
 * DRIVER_OK. Returns 0 on success, <0 on error. Idempotent after first success. */
int  vsnd_probe(void);

/* PCM_SET_PARAMS + PCM_PREPARE for the OUTPUT stream (id 0). Returns 0 / <0. */
int  vsnd_open_stream(vsnd_dir dir, const vsnd_stream_params *p);

/* PCM_START / PCM_STOP for the stream. Returns 0 / <0. */
int  vsnd_stream_start(vsnd_dir dir);
int  vsnd_stream_stop(vsnd_dir dir);

/* --- PCM I/O (TX, doc 12 §6) --- */

/* Per-period S16 staging buffer the dos_audio virtio backend converts F32 into
 * before vsnd_tx(). Sized for period_frames*channels in vsnd_open_stream(). */
int16_t *vsnd_tx_scratch(void);

/* Frames the TX ring can accept right now (free descriptors * period_frames).
 * Drives dos_audio_writable() so the pump never over-enqueues. */
int  vsnd_tx_avail_frames(void);

/* Enqueue 'frames' interleaved S16 frames on the TX virtqueue (stream 0).
 * Returns frames accepted (may be < frames if the ring is full), <0 on error. */
int  vsnd_tx(const int16_t *pcm, uint32_t frames);

/* Reap completed TX buffers, then wait for the ring to drain (shutdown). */
void vsnd_tx_drain(void);

/* PCM_RELEASE the stream and tear down (queues stay live for re-open). */
void vsnd_close(void);

#ifdef __cplusplus
}
#endif
#endif /* DOS_VIRTIO_SND_H */

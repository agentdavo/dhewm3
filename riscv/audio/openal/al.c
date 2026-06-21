/*
 * openal/al.c — the dOS OpenAL/ALC/EFX shim over miniaudio (docs 10 + 11).
 *
 * Implements the C-ABI dhewm3 calls (AL/al.h, AL/alc.h, AL/efx.h) by translating
 * ~40 AL symbols into miniaudio object setters. miniaudio is the mixer +
 * resampler + 3D panner (+ optional reverb node); this file owns the single
 * global ma_engine and the demand-driven pump to the dos_audio sink.
 *
 * STATUS: SKELETON. Every symbol in the doc-10 checklist (00-audio.md §6) is
 * present with the correct signature so dhewm3 links. Bodies are compile-oriented:
 * ALC/device + buffer/source state are wired enough to follow; the hot ma_sound
 * mapping (doc 11 §4) and the streaming FIFO (doc 11 §5) carry TODOs. EFX is
 * stubbed (build order M1 = dry; alcIsExtensionPresent("ALC_EXT_EFX") -> FALSE,
 * doc 11 §7 Option A) — flip SHIM_EFX_REVERB later for Option B.
 *
 * Build: must use the SAME MA_NO_* set as miniaudio_impl.c — include ma_config.h
 * before miniaudio.h (the struct-layout landmine, doc 20 §1.4).
 */
#include "ma_config.h"      /* MA_NO_* set + nanosleep predecl — BEFORE miniaudio.h */
#include "miniaudio.h"

#include "AL/al.h"
#include "AL/alc.h"
#include "AL/efx.h"
#include "AL/alext.h"
#include "openal/al.h"      /* shim-internal */
#include "dos_audio.h"

#include <stddef.h>
#include <string.h>

/* ================================================================== *
 *  Global engine + sink (doc 11 §2, doc 13 §5)                        *
 * ================================================================== */
static ma_engine g_engine;
static int       g_engine_inited;
static ALenum    g_al_error  = AL_NO_ERROR;
static ALCenum   g_alc_error = ALC_NO_ERROR;

/* Singleton ALC tags — the "device" is the single sink (doc 10 §4a). */
static ALCdevice  *g_device  = (ALCdevice *)0x1;   /* opaque non-NULL token */
static ALCcontext *g_context = (ALCcontext *)0x2;

/* EFX availability for M1 dry bring-up (doc 11 §7 Option A). Flip when reverb
 * (Option B) lands behind SHIM_EFX_REVERB. */
#ifdef SHIM_EFX_REVERB
static const int g_efx_present = 1;
#else
static const int g_efx_present = 0;
#endif

/* --- handle tables (doc 11 §3/§4) ----------------------------------- *
 * Handle = index+1 (handle 0 == AL "no buffer/source/effect" sentinel). */

typedef struct {
    int       inUse;
    ma_format format;      /* always ma_format_s16 (dhewm3 sends 16-bit) */
    ma_uint32 channels;    /* 1 or 2 (MONO16 / STEREO16) */
    ma_uint32 sampleRate;
    ma_uint64 frameCount;
    void     *pData;       /* owned copy of the PCM */
} shim_buffer_t;

typedef struct {
    int          inUse;
    int          haveSound;
    ma_sound     sound;
    ma_audio_buffer_ref ref;
    int          haveRef;
    /* deferred AL state (doc 11 §4) */
    float pos[3], gain, pitch;
    int   relative, looping;
    float refDist, maxDist, rolloff;
    ALuint boundBuffer;
    /* streaming FIFO (doc 11 §5) — TODO: ma_pcm_rb-backed custom data source */
    int   streaming;
    ALuint queue[SHIM_STREAM_DEPTH];
    int    qhead, qtail, qcount, processed;
    void  *streamData;       /* owned concatenated queued-buffer PCM */
    ma_uint64 streamFrames;
} shim_source_t;

typedef struct { int inUse; ALenum type; } shim_effect_t;
typedef struct { int inUse; ALenum type; float gain, gainhf; } shim_filter_t;
typedef struct { int inUse; ALuint effect; float gain; } shim_slot_t;

static shim_buffer_t g_buffers[SHIM_MAX_BUFFERS];
static shim_source_t g_sources[SHIM_MAX_SOURCES];
static shim_effect_t g_effects[SHIM_MAX_EFFECTS];
static shim_filter_t g_filters[SHIM_MAX_FILTERS];
static shim_slot_t   g_slots[SHIM_MAX_SLOTS];

/* index<->handle helpers (handle 0 reserved) */
static shim_buffer_t *buf_get(ALuint h)
{ return (h && h <= SHIM_MAX_BUFFERS && g_buffers[h-1].inUse) ? &g_buffers[h-1] : 0; }
static shim_source_t *src_get(ALuint h)
{ return (h && h <= SHIM_MAX_SOURCES && g_sources[h-1].inUse) ? &g_sources[h-1] : 0; }

static void src_release_sound(shim_source_t *s)
{
    if (!s) return;
    if (s->haveSound) {
        ma_sound_uninit(&s->sound);
        s->haveSound = 0;
    }
    if (s->haveRef) {
        ma_audio_buffer_ref_uninit(&s->ref);
        s->haveRef = 0;
    }
    if (s->streamData) {
        ma_free(s->streamData, NULL);
        s->streamData = NULL;
    }
    s->streamFrames = 0;
}

static void src_apply_state(shim_source_t *s)
{
    if (!s || !s->haveSound) return;
    ma_sound_set_volume(&s->sound, s->gain);
    ma_sound_set_pitch(&s->sound, s->pitch);
    ma_sound_set_positioning(&s->sound,
        s->relative ? ma_positioning_relative : ma_positioning_absolute);
    ma_sound_set_position(&s->sound, s->pos[0], s->pos[1], s->pos[2]);
    ma_sound_set_looping(&s->sound, (ma_bool32)s->looping);
    ma_sound_set_min_distance(&s->sound, s->refDist);
    ma_sound_set_max_distance(&s->sound, s->maxDist);
    ma_sound_set_rolloff(&s->sound, s->rolloff);
}

static int src_init_sound_from_buffer(shim_source_t *s, shim_buffer_t *b)
{
    ma_result r;
    if (!s || !b || !b->pData || !g_engine_inited) return -1;

    src_release_sound(s);
    r = ma_audio_buffer_ref_init(b->format, b->channels, b->pData, b->frameCount, &s->ref);
    if (r != MA_SUCCESS) return -2;
    s->haveRef = 1;

    r = ma_sound_init_from_data_source(&g_engine, (ma_data_source *)&s->ref, 0, NULL, &s->sound);
    if (r != MA_SUCCESS) {
        ma_audio_buffer_ref_uninit(&s->ref);
        s->haveRef = 0;
        return -3;
    }
    s->haveSound = 1;
    src_apply_state(s);
    return 0;
}

static int src_rebuild_stream_sound(shim_source_t *s)
{
    shim_buffer_t *first;
    ma_uint64 totalFrames = 0;
    size_t totalBytes = 0;
    uint8_t *dst;
    int i;
    ma_result r;

    if (!s || !g_engine_inited || s->qcount <= 0) return -1;
    first = buf_get(s->queue[s->qhead]);
    if (!first || !first->pData) return -2;

    for (i = 0; i < s->qcount; ++i) {
        int slot = (s->qhead + i) % SHIM_STREAM_DEPTH;
        shim_buffer_t *b = buf_get(s->queue[slot]);
        if (!b || !b->pData || b->channels != first->channels ||
            b->sampleRate != first->sampleRate || b->format != first->format) {
            return -3;
        }
        totalFrames += b->frameCount;
        totalBytes += (size_t)b->frameCount * b->channels * 2;
    }

    src_release_sound(s);
    s->streamData = ma_malloc(totalBytes, NULL);
    if (!s->streamData) return -4;

    dst = (uint8_t *)s->streamData;
    for (i = 0; i < s->qcount; ++i) {
        int slot = (s->qhead + i) % SHIM_STREAM_DEPTH;
        shim_buffer_t *b = buf_get(s->queue[slot]);
        size_t bytes = (size_t)b->frameCount * b->channels * 2;
        memcpy(dst, b->pData, bytes);
        dst += bytes;
    }

    r = ma_audio_buffer_ref_init(first->format, first->channels,
                                 s->streamData, totalFrames, &s->ref);
    if (r != MA_SUCCESS) {
        src_release_sound(s);
        return -5;
    }
    s->haveRef = 1;

    r = ma_sound_init_from_data_source(&g_engine, (ma_data_source *)&s->ref, 0, NULL, &s->sound);
    if (r != MA_SUCCESS) {
        src_release_sound(s);
        return -6;
    }

    s->haveSound = 1;
    s->streamFrames = totalFrames;
    s->processed = 0;
    src_apply_state(s);
    return 0;
}

static ALuint table_alloc(void *base, size_t stride, int count, size_t inuse_off)
{
    char *p = (char *)base;
    int i;
    for (i = 0; i < count; ++i) {
        int *inUse = (int *)(p + (size_t)i * stride + inuse_off);
        if (!*inUse) { *inUse = 1; return (ALuint)(i + 1); }
    }
    return 0;   /* exhausted */
}

/* ================================================================== *
 *  Engine init / shutdown / pump (doc 11 §2, doc 13 §5)               *
 * ================================================================== */
int shim_engine_inited(void) { return g_engine_inited; }

int shim_engine_init(uint32_t channels, uint32_t rate)
{
    ma_engine_config cfg;
    dos_audio_spec want, granted;

    if (g_engine_inited) return 0;

    cfg = ma_engine_config_init();
    cfg.noDevice           = MA_TRUE;            /* device-less manual read */
    cfg.channels           = channels;           /* 2 (doc 11 §6) */
    cfg.sampleRate         = rate;               /* 44100 PRIMARYFREQ */
    cfg.listenerCount      = 1;
    cfg.periodSizeInFrames = SHIM_MIX_FRAMES;    /* 4096 */
    if (ma_engine_init(&cfg, &g_engine) != MA_SUCCESS) {
        g_al_error = AL_OUT_OF_MEMORY;
        return -1;
    }

    /* Open the sink as F32 (engine native); the backend converts if it needs
     * S16 (doc 13 §4.1: pick F32-in across the seam). */
    want.rate     = rate;
    want.channels = channels;
    want.format   = DOS_AUDIO_F32;
    if (dos_audio_open(&want, &granted) != 0) {
        ma_engine_uninit(&g_engine);
        return -2;
    }

    g_engine_inited = 1;
    return 0;
}

void shim_engine_shutdown(void)
{
    int i;
    if (!g_engine_inited) return;
    for (i = 0; i < SHIM_MAX_SOURCES; ++i) {
        src_release_sound(&g_sources[i]);
    }
    dos_audio_close();
    ma_engine_uninit(&g_engine);
    g_engine_inited = 0;
}

/* The pump (doc 13 §5.2): pull what the sink can take, forward it. Called from
 * alcProcessContext (doc 13 §5.1 placement (a) — zero dhewm3 changes). */
void dos_audio_pump(void)
{
    static float scratch[SHIM_OUT_CHANNELS * SHIM_MIX_FRAMES];
    int       budget, want;
    ma_uint64 got = 0;

    if (!g_engine_inited) return;
    budget = dos_audio_writable();
    if (budget <= 0) return;                       /* back-pressured */
    want = (budget < SHIM_MIX_FRAMES) ? budget : SHIM_MIX_FRAMES;

    ma_engine_read_pcm_frames(&g_engine, scratch, (ma_uint64)want, &got);
    if (got) dos_audio_write(scratch, (uint32_t)got);
}

/* ================================================================== *
 *  ALC device / context (doc 10 §4a, doc 11 §2)                       *
 * ================================================================== */
ALCdevice *alcOpenDevice(const ALCchar *devicename)
{
    (void)devicename;
    return g_device;                               /* singleton sink */
}

ALCboolean alcCloseDevice(ALCdevice *device)
{
    (void)device;
    return ALC_TRUE;
}

ALCcontext *alcCreateContext(ALCdevice *device, const ALCint *attrlist)
{
    (void)device; (void)attrlist;                  /* HRTF/limiter attrs ignored */
    if (shim_engine_init(SHIM_OUT_CHANNELS, SHIM_OUT_RATE) != 0) return 0;
    return g_context;
}

ALCboolean alcMakeContextCurrent(ALCcontext *context)
{
    (void)context;
    return ALC_TRUE;
}

void alcDestroyContext(ALCcontext *context)
{
    (void)context;
    shim_engine_shutdown();
}

/* Pump hook lives here (doc 13 §5.1 (a)): dhewm3 calls alcProcessContext at the
 * end of every AsyncUpdateWrite (snd_system.cpp:916). */
void alcProcessContext(ALCcontext *context)
{
    (void)context;
    dos_audio_pump();
}

void alcSuspendContext(ALCcontext *context) { (void)context; /* no-op (doc 11 §2) */ }

ALCenum alcGetError(ALCdevice *device)
{
    ALCenum e = g_alc_error;
    (void)device;
    g_alc_error = ALC_NO_ERROR;
    return e;
}

ALCboolean alcIsExtensionPresent(ALCdevice *device, const ALCchar *extname)
{
    (void)device;
    if (!extname) return ALC_FALSE;
    if (strcmp(extname, "ALC_EXT_EFX") == 0)         return g_efx_present ? ALC_TRUE : ALC_FALSE;
    if (strcmp(extname, "ALC_ENUMERATE_ALL_EXT") == 0) return ALC_TRUE;
    /* HRTF / limiter / output_mode / disconnect -> FALSE (doc 10 §4b) */
    return ALC_FALSE;
}

void *alcGetProcAddress(ALCdevice *device, const ALCchar *funcname)
{
    (void)device;
    if (funcname && strcmp(funcname, "alcResetDeviceSOFT") == 0)
        return (void *)alcResetDeviceSOFT;         /* HRTF absent -> dhewm3 won't use it */
    return 0;
}

const ALCchar *alcGetString(ALCdevice *device, ALCenum param)
{
    (void)device;
    if (param == ALC_ALL_DEVICES_SPECIFIER) return "dOS virtio-snd\0";  /* dbl-NUL list */
    return "";
}

void alcGetIntegerv(ALCdevice *device, ALCenum param, ALCsizei size, ALCint *values)
{
    (void)device;
    if (!values || size < 1) return;
    if (param == ALC_CONNECTED) values[0] = 1;     /* never disconnects (doc 10 §4a) */
    else                        values[0] = 0;
}

ALCboolean alcResetDeviceSOFT(ALCdevice *device, const ALCint *attribs)
{
    (void)device; (void)attribs;
    return ALC_TRUE;                                /* no-op success (doc 11 §2) */
}

/* ================================================================== *
 *  AL buffers (doc 10 §4c, doc 11 §3)                                 *
 * ================================================================== */
void alGenBuffers(ALsizei n, ALuint *buffers)
{
    ALsizei i;
    for (i = 0; i < n; ++i) {
        ALuint h = table_alloc(g_buffers, sizeof(shim_buffer_t),
                               SHIM_MAX_BUFFERS, offsetof(shim_buffer_t, inUse));
        if (!h) { g_al_error = AL_OUT_OF_MEMORY; buffers[i] = 0; continue; }
        memset(&g_buffers[h-1], 0, sizeof(shim_buffer_t));
        g_buffers[h-1].inUse = 1;
        buffers[i] = h;
    }
}

void alDeleteBuffers(ALsizei n, const ALuint *buffers)
{
    ALsizei i;
    for (i = 0; i < n; ++i) {
        shim_buffer_t *b = buf_get(buffers[i]);
        if (!b) continue;                          /* tolerate stale ids (doc 10 §4c) */
        if (b->pData) { ma_free(b->pData, NULL); b->pData = 0; }
        b->inUse = 0;
    }
}

ALboolean alIsBuffer(ALuint buffer)
{
    /* Always succeed for live handles so dhewm3 takes the static-buffer path
     * (hardwareBuffer=true) — doc 10 §4c. */
    return buf_get(buffer) ? AL_TRUE : AL_FALSE;
}

void alBufferData(ALuint buffer, ALenum format, const ALvoid *data,
                  ALsizei size, ALsizei freq)
{
    shim_buffer_t *b = buf_get(buffer);
    ma_uint32 ch;
    if (!b) { g_al_error = AL_INVALID_NAME; return; }
    ch = (format == AL_FORMAT_MONO16) ? 1 : 2;
    if (b->pData) ma_free(b->pData, NULL);
    b->pData = ma_malloc((size_t)size, NULL);      /* copy: dhewm3 reuses its buf */
    if (!b->pData) { g_al_error = AL_OUT_OF_MEMORY; return; }
    memcpy(b->pData, data, (size_t)size);
    b->format     = ma_format_s16;
    b->channels   = ch;
    b->sampleRate = (ma_uint32)freq;
    b->frameCount = (ma_uint64)size / (ch * 2);
}

/* ================================================================== *
 *  AL sources (doc 10 §4d, doc 11 §4)                                 *
 * ================================================================== */
void alGenSources(ALsizei n, ALuint *sources)
{
    ALsizei i;
    for (i = 0; i < n; ++i) {
        ALuint h = table_alloc(g_sources, sizeof(shim_source_t),
                               SHIM_MAX_SOURCES, offsetof(shim_source_t, inUse));
        if (!h) { g_al_error = AL_OUT_OF_MEMORY; sources[i] = 0; continue; }
        memset(&g_sources[h-1], 0, sizeof(shim_source_t));
        g_sources[h-1].inUse  = 1;
        g_sources[h-1].gain   = 1.0f;
        g_sources[h-1].pitch  = 1.0f;
        sources[i] = h;
    }
}

void alDeleteSources(ALsizei n, const ALuint *sources)
{
    ALsizei i;
    for (i = 0; i < n; ++i) {
        shim_source_t *s = src_get(sources[i]);
        if (!s) continue;
        src_release_sound(s);
        s->inUse = 0;
    }
}

ALboolean alIsSource(ALuint source) { return src_get(source) ? AL_TRUE : AL_FALSE; }

void alSourcei(ALuint source, ALenum param, ALint value)
{
    shim_source_t *s = src_get(source);
    if (!s) { g_al_error = AL_INVALID_NAME; return; }
    switch (param) {
    case AL_SOURCE_RELATIVE:
        s->relative = value;
        if (s->haveSound)
            ma_sound_set_positioning(&s->sound,
                value ? ma_positioning_relative : ma_positioning_absolute);
        break;
    case AL_LOOPING:
        s->looping = value;
        if (s->haveSound) ma_sound_set_looping(&s->sound, (ma_bool32)value);
        break;
    case AL_BUFFER:
        src_release_sound(s);
        s->boundBuffer = (ALuint)value;
        s->streaming = 0;
        s->qhead = s->qtail = s->qcount = s->processed = 0;
        if (value != 0 && src_init_sound_from_buffer(s, buf_get((ALuint)value)) != 0)
            g_al_error = AL_INVALID_VALUE;
        break;
    case AL_DIRECT_FILTER:
        /* occlusion lowpass on dry path; AL_FILTER_NULL = bypass (doc 10 §4d).
         * TODO: set source biquad lowpass from filter handle. */
        break;
    default: break;
    }
}

void alSourcef(ALuint source, ALenum param, ALfloat value)
{
    shim_source_t *s = src_get(source);
    if (!s) { g_al_error = AL_INVALID_NAME; return; }
    switch (param) {
    case AL_GAIN:   s->gain  = value; if (s->haveSound) ma_sound_set_volume(&s->sound, value); break;
    case AL_PITCH:  s->pitch = value; if (s->haveSound) ma_sound_set_pitch (&s->sound, value); break;
    case AL_REFERENCE_DISTANCE: s->refDist = value; if (s->haveSound) ma_sound_set_min_distance(&s->sound, value); break;
    case AL_MAX_DISTANCE:       s->maxDist = value; if (s->haveSound) ma_sound_set_max_distance(&s->sound, value); break;
    case AL_ROLLOFF_FACTOR:
        /* dhewm3 sets 0 -> disable engine distance attenuation (CRITICAL, doc
         * 10 §4d/§6: dhewm3 does attenuation itself via AL_GAIN). */
        s->rolloff = value;
        if (s->haveSound) ma_sound_set_rolloff(&s->sound, value);
        break;
    default: break;
    }
}

void alSource3f(ALuint source, ALenum param, ALfloat v1, ALfloat v2, ALfloat v3)
{
    shim_source_t *s = src_get(source);
    if (!s) { g_al_error = AL_INVALID_NAME; return; }
    if (param == AL_POSITION) {
        s->pos[0] = v1; s->pos[1] = v2; s->pos[2] = v3;   /* coords pre-remapped (doc 10 §6) */
        if (s->haveSound) ma_sound_set_position(&s->sound, v1, v2, v3);
    }
}

void alSource3i(ALuint source, ALenum param, ALint v1, ALint v2, ALint v3)
{
    shim_source_t *s = src_get(source);
    if (!s) { g_al_error = AL_INVALID_NAME; return; }
    if (param == AL_AUXILIARY_SEND_FILTER) {
        /* route wet send to world reverb slot v1, send idx v2(=0), through
         * filter v3 (doc 10 §4d). EFX-off (M1): no-op. TODO Option B. */
        (void)v1; (void)v2; (void)v3;
    }
}

void alGetSourcei(ALuint source, ALenum param, ALint *value)
{
    shim_source_t *s = src_get(source);
    if (!value) return;
    if (!s) { *value = 0; g_al_error = AL_INVALID_NAME; return; }
    switch (param) {
    case AL_SOURCE_STATE:
        *value = (s->haveSound && ma_sound_is_playing(&s->sound)) ? AL_PLAYING : AL_STOPPED;
        break;
    case AL_BUFFERS_PROCESSED:
        if (s->streaming && s->qcount > 0 && (!s->haveSound || ma_sound_at_end(&s->sound))) {
            s->processed = s->qcount;
        }
        *value = s->processed;
        break;
    default:
        *value = 0;
        break;
    }
}

void alSourcePlay(ALuint source)
{
    shim_source_t *s = src_get(source);
    if (!s || !s->haveSound) return;
    ma_sound_seek_to_pcm_frame(&s->sound, 0);      /* AL play restarts (doc 11 §4) */
    ma_sound_start(&s->sound);
}

void alSourceStop(ALuint source)
{
    shim_source_t *s = src_get(source);
    if (!s || !s->haveSound) return;
    ma_sound_stop(&s->sound);
    ma_sound_seek_to_pcm_frame(&s->sound, 0);
}

void alSourcePause(ALuint source)
{
    shim_source_t *s = src_get(source);
    if (!s || !s->haveSound) return;
    ma_sound_stop(&s->sound);                       /* no rewind (doc 11 §4) */
}

void alSourceQueueBuffers(ALuint source, ALsizei n, const ALuint *buffers)
{
    shim_source_t *s = src_get(source);
    if (!s) { g_al_error = AL_INVALID_NAME; return; }
    s->streaming = 1;
    s->boundBuffer = 0;
    while (n-- > 0 && s->qcount < SHIM_STREAM_DEPTH) {
        s->queue[s->qtail] = *buffers++;
        s->qtail = (s->qtail + 1) % SHIM_STREAM_DEPTH;
        s->qcount++;
    }
    if (src_rebuild_stream_sound(s) != 0)
        g_al_error = AL_INVALID_VALUE;
}

void alSourceUnqueueBuffers(ALuint source, ALsizei n, ALuint *buffers)
{
    shim_source_t *s = src_get(source);
    ALsizei i;
    if (!s) { g_al_error = AL_INVALID_NAME; return; }
    if (n > s->processed) n = s->processed;
    for (i = 0; i < n; ++i) {
        if (buffers) buffers[i] = s->queue[s->qhead];
        s->queue[s->qhead] = 0;
        s->qhead = (s->qhead + 1) % SHIM_STREAM_DEPTH;
        s->qcount--;
        s->processed--;
    }
    if (s->qcount == 0)
        s->qhead = s->qtail = 0;
}

/* ================================================================== *
 *  AL listener (doc 10 §4e, doc 11 §6)                                *
 * ================================================================== */
void alListenerf(ALenum param, ALfloat value)
{
    if (param == AL_GAIN && g_engine_inited)
        ma_engine_set_volume(&g_engine, value);     /* 0 = out-of-world mute */
}

void alListenerfv(ALenum param, const ALfloat *values)
{
    if (!values || !g_engine_inited) return;
    if (param == AL_POSITION) {
        ma_engine_listener_set_position(&g_engine, 0, values[0], values[1], values[2]);
    } else if (param == AL_ORIENTATION) {
        ma_engine_listener_set_direction(&g_engine, 0, values[0], values[1], values[2]);
        ma_engine_listener_set_world_up (&g_engine, 0, values[3], values[4], values[5]);
    }
}

/* ================================================================== *
 *  AL state / queries (doc 10 §4g, §3d proc table)                    *
 * ================================================================== */
ALenum alGetError(void)
{
    ALenum e = g_al_error;
    g_al_error = AL_NO_ERROR;
    return e;
}

const ALchar *alGetString(ALenum param)
{
    switch (param) {
    case AL_VENDOR:   return "dOS";
    case AL_RENDERER: return "miniaudio/virtio-snd";
    case AL_VERSION:  return "1.1 ALSHIM";
    default:          return "";
    }
}

ALboolean alIsExtensionPresent(const ALchar *extname)
{
    (void)extname;
    return AL_FALSE;                                /* not used in sound path (doc 10 §4b) */
}

/* alGetProcAddress must return non-NULL for exactly the 17 EFX strings dhewm3
 * queries (doc 10 §3d). Static name->pointer table. */
void *alGetProcAddress(const ALchar *fname)
{
    static const struct { const char *name; void *fn; } tbl[] = {
        { "alGenEffects",                  (void *)alGenEffects },
        { "alDeleteEffects",               (void *)alDeleteEffects },
        { "alIsEffect",                    (void *)alIsEffect },
        { "alEffecti",                     (void *)alEffecti },
        { "alEffectf",                     (void *)alEffectf },
        { "alEffectfv",                    (void *)alEffectfv },
        { "alGenFilters",                  (void *)alGenFilters },
        { "alDeleteFilters",               (void *)alDeleteFilters },
        { "alIsFilter",                    (void *)alIsFilter },
        { "alFilteri",                     (void *)alFilteri },
        { "alFilterf",                     (void *)alFilterf },
        { "alGenAuxiliaryEffectSlots",     (void *)alGenAuxiliaryEffectSlots },
        { "alDeleteAuxiliaryEffectSlots",  (void *)alDeleteAuxiliaryEffectSlots },
        { "alIsAuxiliaryEffectSlot",       (void *)alIsAuxiliaryEffectSlot },
        { "alAuxiliaryEffectSloti",        (void *)alAuxiliaryEffectSloti },
        { "alAuxiliaryEffectSlotf",        (void *)alAuxiliaryEffectSlotf },
    };
    size_t i;
    if (!fname) return 0;
    for (i = 0; i < sizeof(tbl)/sizeof(tbl[0]); ++i)
        if (strcmp(fname, tbl[i].name) == 0) return tbl[i].fn;
    return 0;
}

/* ================================================================== *
 *  EFX (doc 10 §4f, doc 11 §7) — STUBBED for M1 dry bring-up          *
 *  All present + return success so the (rare) EFX path never asserts.  *
 *  TODO Option B: map EAXREVERB -> node-graph reverb behind            *
 *  SHIM_EFX_REVERB (doc 11 §7 step 1-5).                               *
 * ================================================================== */
void alGenEffects(ALsizei n, ALuint *effects)
{
    ALsizei i;
    for (i = 0; i < n; ++i)
        effects[i] = table_alloc(g_effects, sizeof(shim_effect_t),
                                 SHIM_MAX_EFFECTS, offsetof(shim_effect_t, inUse));
}
void alDeleteEffects(ALsizei n, const ALuint *effects)
{ ALsizei i; for (i = 0; i < n; ++i) if (effects[i] && effects[i] <= SHIM_MAX_EFFECTS) g_effects[effects[i]-1].inUse = 0; }
ALboolean alIsEffect(ALuint effect)
{ return (effect && effect <= SHIM_MAX_EFFECTS && g_effects[effect-1].inUse) ? AL_TRUE : AL_FALSE; }
void alEffecti (ALuint effect, ALenum param, ALint value)   { (void)effect; (void)param; (void)value; }
void alEffectf (ALuint effect, ALenum param, ALfloat value) { (void)effect; (void)param; (void)value; }
void alEffectfv(ALuint effect, ALenum param, const ALfloat *values) { (void)effect; (void)param; (void)values; }

void alGenFilters(ALsizei n, ALuint *filters)
{
    ALsizei i;
    for (i = 0; i < n; ++i)
        filters[i] = table_alloc(g_filters, sizeof(shim_filter_t),
                                 SHIM_MAX_FILTERS, offsetof(shim_filter_t, inUse));
}
void alDeleteFilters(ALsizei n, const ALuint *filters)
{ ALsizei i; for (i = 0; i < n; ++i) if (filters[i] && filters[i] <= SHIM_MAX_FILTERS) g_filters[filters[i]-1].inUse = 0; }
ALboolean alIsFilter(ALuint filter)
{ return (filter && filter <= SHIM_MAX_FILTERS && g_filters[filter-1].inUse) ? AL_TRUE : AL_FALSE; }
void alFilteri(ALuint filter, ALenum param, ALint value)   { (void)filter; (void)param; (void)value; }
void alFilterf(ALuint filter, ALenum param, ALfloat value) { (void)filter; (void)param; (void)value; }

void alGenAuxiliaryEffectSlots(ALsizei n, ALuint *slots)
{
    ALsizei i;
    for (i = 0; i < n; ++i)
        slots[i] = table_alloc(g_slots, sizeof(shim_slot_t),
                               SHIM_MAX_SLOTS, offsetof(shim_slot_t, inUse));
}
void alDeleteAuxiliaryEffectSlots(ALsizei n, const ALuint *slots)
{ ALsizei i; for (i = 0; i < n; ++i) if (slots[i] && slots[i] <= SHIM_MAX_SLOTS) g_slots[slots[i]-1].inUse = 0; }
ALboolean alIsAuxiliaryEffectSlot(ALuint slot)
{ return (slot && slot <= SHIM_MAX_SLOTS && g_slots[slot-1].inUse) ? AL_TRUE : AL_FALSE; }
void alAuxiliaryEffectSloti(ALuint slot, ALenum param, ALint value)   { (void)slot; (void)param; (void)value; }
void alAuxiliaryEffectSlotf(ALuint slot, ALenum param, ALfloat value) { (void)slot; (void)param; (void)value; }

/*
 * AL/alext.h — minimal OpenAL EFX (effects) + SOFT extension header for the
 * bare-metal dhewm3 build. See AL/al.h for rationale. Only the EFX/SOFT symbols
 * the dhewm3 sound code references are defined; values are the canonical
 * OpenAL-soft EFX constants. EFX functions are stubbed in openal_stub.cpp.
 */
#ifndef AL_ALEXT_H
#define AL_ALEXT_H

#include "AL/al.h"
#include "AL/alc.h"

#if defined(__cplusplus)
extern "C" {
#endif

/* ---- ALC_EXT_EFX context attributes / queries ---- */
#define ALC_EXT_EFX_NAME                         "ALC_EXT_EFX"
#define ALC_EFX_MAJOR_VERSION                    0x20001
#define ALC_EFX_MINOR_VERSION                    0x20002
#define ALC_MAX_AUXILIARY_SENDS                  0x20003

/* ---- Filter object ---- */
#define AL_FILTER_TYPE                           0x8001
#define AL_FILTER_NULL                           0x0000
#define AL_FILTER_LOWPASS                        0x0001
#define AL_LOWPASS_GAIN                          0x0001
#define AL_LOWPASS_GAINHF                        0x0002

/* ---- Effect object ---- */
#define AL_EFFECT_TYPE                           0x8001
#define AL_EFFECT_NULL                           0x0000
#define AL_EFFECT_EAXREVERB                      0x8000

/* ---- Auxiliary effect slot ---- */
#define AL_EFFECTSLOT_EFFECT                     0x0001
#define AL_EFFECTSLOT_GAIN                       0x0002
#define AL_EFFECTSLOT_NULL                       0x0000

/* ---- EAXREVERB effect parameters ---- */
#define AL_EAXREVERB_DENSITY                     0x0001
#define AL_EAXREVERB_DIFFUSION                   0x0002
#define AL_EAXREVERB_GAIN                        0x0003
#define AL_EAXREVERB_GAINHF                      0x0004
#define AL_EAXREVERB_GAINLF                      0x0005
#define AL_EAXREVERB_DECAY_TIME                  0x0006
#define AL_EAXREVERB_DECAY_HFRATIO              0x0007
#define AL_EAXREVERB_DECAY_LFRATIO              0x0008
#define AL_EAXREVERB_REFLECTIONS_GAIN          0x0009
#define AL_EAXREVERB_REFLECTIONS_DELAY         0x000A
#define AL_EAXREVERB_REFLECTIONS_PAN           0x000B
#define AL_EAXREVERB_LATE_REVERB_GAIN          0x000C
#define AL_EAXREVERB_LATE_REVERB_DELAY         0x000D
#define AL_EAXREVERB_LATE_REVERB_PAN           0x000E
#define AL_EAXREVERB_ECHO_TIME                  0x000F
#define AL_EAXREVERB_ECHO_DEPTH                 0x0010
#define AL_EAXREVERB_MODULATION_TIME           0x0011
#define AL_EAXREVERB_MODULATION_DEPTH          0x0012
#define AL_EAXREVERB_AIR_ABSORPTION_GAINHF     0x0013
#define AL_EAXREVERB_HFREFERENCE               0x0014
#define AL_EAXREVERB_LFREFERENCE               0x0015
#define AL_EAXREVERB_ROOM_ROLLOFF_FACTOR       0x0016
#define AL_EAXREVERB_DECAY_HFLIMIT             0x0017

/* EAXREVERB parameter clamp ranges (canonical OpenAL-soft EFX values).
 * snd_efxfile.cpp uses these via  AL_EAXREVERB_MIN_##property / MAX_##property
 * to clamp the gain parameters it parses from .efx files. Only the gain-type
 * properties dhewm3 actually clamps are provided. */
#define AL_EAXREVERB_MIN_GAIN                          0.0f
#define AL_EAXREVERB_MAX_GAIN                          1.0f
#define AL_EAXREVERB_MIN_GAINHF                        0.0f
#define AL_EAXREVERB_MAX_GAINHF                        1.0f
#define AL_EAXREVERB_MIN_GAINLF                        0.0f
#define AL_EAXREVERB_MAX_GAINLF                        1.0f
#define AL_EAXREVERB_MIN_REFLECTIONS_GAIN             0.0f
#define AL_EAXREVERB_MAX_REFLECTIONS_GAIN             3.16f
#define AL_EAXREVERB_MIN_LATE_REVERB_GAIN            0.0f
#define AL_EAXREVERB_MAX_LATE_REVERB_GAIN           10.0f
#define AL_EAXREVERB_MIN_AIR_ABSORPTION_GAINHF      0.892f
#define AL_EAXREVERB_MAX_AIR_ABSORPTION_GAINHF      1.0f

/* ---- EFX function-pointer typedefs (loaded via alGetProcAddress) ---- */
typedef void (AL_APIENTRY *LPALGENEFFECTS)( ALsizei, ALuint* );
typedef void (AL_APIENTRY *LPALDELETEEFFECTS)( ALsizei, const ALuint* );
typedef ALboolean (AL_APIENTRY *LPALISEFFECT)( ALuint );
typedef void (AL_APIENTRY *LPALEFFECTI)( ALuint, ALenum, ALint );
typedef void (AL_APIENTRY *LPALEFFECTF)( ALuint, ALenum, ALfloat );
typedef void (AL_APIENTRY *LPALEFFECTFV)( ALuint, ALenum, const ALfloat* );

typedef void (AL_APIENTRY *LPALGENFILTERS)( ALsizei, ALuint* );
typedef void (AL_APIENTRY *LPALDELETEFILTERS)( ALsizei, const ALuint* );
typedef ALboolean (AL_APIENTRY *LPALISFILTER)( ALuint );
typedef void (AL_APIENTRY *LPALFILTERI)( ALuint, ALenum, ALint );
typedef void (AL_APIENTRY *LPALFILTERF)( ALuint, ALenum, ALfloat );

typedef void (AL_APIENTRY *LPALGENAUXILIARYEFFECTSLOTS)( ALsizei, ALuint* );
typedef void (AL_APIENTRY *LPALDELETEAUXILIARYEFFECTSLOTS)( ALsizei, const ALuint* );
typedef ALboolean (AL_APIENTRY *LPALISAUXILIARYEFFECTSLOT)( ALuint );
typedef void (AL_APIENTRY *LPALAUXILIARYEFFECTSLOTI)( ALuint, ALenum, ALint );
typedef void (AL_APIENTRY *LPALAUXILIARYEFFECTSLOTF)( ALuint, ALenum, ALfloat );

/* ---- SOFT extensions referenced by snd_local.h / snd_system.cpp ---- */
#define ALC_SOFT_HRTF                            1
#define ALC_HRTF_SOFT                            0x1992
#define ALC_DONT_CARE_SOFT                       0x0002
#define ALC_HRTF_ID_SOFT                         0x1996

typedef ALCboolean (ALC_APIENTRY *LPALCRESETDEVICESOFT)( ALCdevice*, const ALCint* );

#if defined(__cplusplus)
}
#endif

#endif /* AL_ALEXT_H */

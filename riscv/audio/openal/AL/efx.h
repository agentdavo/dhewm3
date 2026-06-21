/*
 * AL/efx.h — dOS OpenAL shim EFX subset (doc 10 §3c EFX block, §3d, §1c).
 *
 * dhewm3 uses exactly ONE effect type (AL_EFFECT_EAXREVERB), ONE filter type
 * (AL_FILTER_LOWPASS), and ONE aux effect slot per sound world. These 17
 * functions are resolved at runtime via alGetProcAddress, but the prototypes +
 * LPAL* typedefs must exist so the game/ded build compiles. Values are EFX-spec /
 * OpenAL-soft canonical.
 *
 * dhewm3 includes <AL/alext.h>; that header (provided here as a thin alias or by
 * including this) must pull in these tokens. Keep alext.h #include "efx.h".
 */
#ifndef DOS_AL_EFX_H
#define DOS_AL_EFX_H

#include "al.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- effects --- */
#define AL_EFFECT_TYPE             0x8001
#define AL_EFFECT_EAXREVERB        0x8000

/* --- filters --- */
#define AL_FILTER_TYPE             0x8001
#define AL_FILTER_NULL             0x0000
#define AL_FILTER_LOWPASS          0x0001
#define AL_LOWPASS_GAIN            0x0001
#define AL_LOWPASS_GAINHF          0x0002

/* --- source EFX params --- */
#define AL_DIRECT_FILTER           0x20005
#define AL_AUXILIARY_SEND_FILTER   0x20006

/* --- aux effect slot --- */
#define AL_EFFECTSLOT_EFFECT       0x0001
#define AL_EFFECTSLOT_GAIN         0x0002
#define AL_EFFECTSLOT_NULL         0x0000

/* --- EAXREVERB params (all referenced in snd_efxfile.cpp:208-255) --- */
#define AL_EAXREVERB_DENSITY                 0x0001
#define AL_EAXREVERB_DIFFUSION               0x0002
#define AL_EAXREVERB_GAIN                    0x0003
#define AL_EAXREVERB_GAINHF                  0x0004
#define AL_EAXREVERB_GAINLF                  0x0005
#define AL_EAXREVERB_DECAY_TIME              0x0006
#define AL_EAXREVERB_DECAY_HFRATIO           0x0007
#define AL_EAXREVERB_DECAY_LFRATIO           0x0008
#define AL_EAXREVERB_REFLECTIONS_GAIN        0x0009
#define AL_EAXREVERB_REFLECTIONS_DELAY       0x000A
#define AL_EAXREVERB_REFLECTIONS_PAN         0x000B   /* fv (vec3) */
#define AL_EAXREVERB_LATE_REVERB_GAIN        0x000C
#define AL_EAXREVERB_LATE_REVERB_DELAY       0x000D
#define AL_EAXREVERB_LATE_REVERB_PAN         0x000E   /* fv (vec3) */
#define AL_EAXREVERB_ECHO_TIME               0x000F
#define AL_EAXREVERB_ECHO_DEPTH              0x0010
#define AL_EAXREVERB_MODULATION_TIME         0x0011
#define AL_EAXREVERB_MODULATION_DEPTH        0x0012
#define AL_EAXREVERB_AIR_ABSORPTION_GAINHF   0x0013
#define AL_EAXREVERB_HFREFERENCE             0x0014
#define AL_EAXREVERB_LFREFERENCE             0x0015
#define AL_EAXREVERB_ROOM_ROLLOFF_FACTOR     0x0016
#define AL_EAXREVERB_DECAY_HFLIMIT           0x0017

/*
 * MIN/MAX clamp bounds: snd_efxfile.cpp's mB_to_gain() macro expands to
 * AL_EAXREVERB_MIN_<PROP> / MAX_<PROP> for the gain props only (doc 10 §3c).
 * Define them (EFX-spec float values) or the build won't compile.
 */
#define AL_EAXREVERB_MIN_GAIN                    0.0f
#define AL_EAXREVERB_MAX_GAIN                    1.0f
#define AL_EAXREVERB_MIN_GAINHF                  0.0f
#define AL_EAXREVERB_MAX_GAINHF                  1.0f
#define AL_EAXREVERB_MIN_GAINLF                  0.0f
#define AL_EAXREVERB_MAX_GAINLF                  1.0f
#define AL_EAXREVERB_MIN_REFLECTIONS_GAIN        0.0f
#define AL_EAXREVERB_MAX_REFLECTIONS_GAIN        3.16f
#define AL_EAXREVERB_MIN_LATE_REVERB_GAIN        0.0f
#define AL_EAXREVERB_MAX_LATE_REVERB_GAIN        10.0f
#define AL_EAXREVERB_MIN_AIR_ABSORPTION_GAINHF   0.892f
#define AL_EAXREVERB_MAX_AIR_ABSORPTION_GAINHF   1.0f

/* --- EFX function prototypes (17, via alGetProcAddress at runtime) --- */
AL_API void      AL_APIENTRY alGenEffects(ALsizei n, ALuint *effects);
AL_API void      AL_APIENTRY alDeleteEffects(ALsizei n, const ALuint *effects);
AL_API ALboolean AL_APIENTRY alIsEffect(ALuint effect);
AL_API void      AL_APIENTRY alEffecti (ALuint effect, ALenum param, ALint value);
AL_API void      AL_APIENTRY alEffectf (ALuint effect, ALenum param, ALfloat value);
AL_API void      AL_APIENTRY alEffectfv(ALuint effect, ALenum param, const ALfloat *values);

AL_API void      AL_APIENTRY alGenFilters(ALsizei n, ALuint *filters);
AL_API void      AL_APIENTRY alDeleteFilters(ALsizei n, const ALuint *filters);
AL_API ALboolean AL_APIENTRY alIsFilter(ALuint filter);
AL_API void      AL_APIENTRY alFilteri(ALuint filter, ALenum param, ALint value);
AL_API void      AL_APIENTRY alFilterf(ALuint filter, ALenum param, ALfloat value);

AL_API void      AL_APIENTRY alGenAuxiliaryEffectSlots(ALsizei n, ALuint *slots);
AL_API void      AL_APIENTRY alDeleteAuxiliaryEffectSlots(ALsizei n, const ALuint *slots);
AL_API ALboolean AL_APIENTRY alIsAuxiliaryEffectSlot(ALuint slot);
AL_API void      AL_APIENTRY alAuxiliaryEffectSloti(ALuint slot, ALenum param, ALint value);
AL_API void      AL_APIENTRY alAuxiliaryEffectSlotf(ALuint slot, ALenum param, ALfloat value);

/* --- LPAL* function-pointer typedefs (dhewm3 casts proc-addr results to these,
 *     doc 10 §3d). Must match OpenAL-soft. --- */
typedef void      (AL_APIENTRY *LPALGENEFFECTS)(ALsizei, ALuint*);
typedef void      (AL_APIENTRY *LPALDELETEEFFECTS)(ALsizei, const ALuint*);
typedef ALboolean (AL_APIENTRY *LPALISEFFECT)(ALuint);
typedef void      (AL_APIENTRY *LPALEFFECTI)(ALuint, ALenum, ALint);
typedef void      (AL_APIENTRY *LPALEFFECTF)(ALuint, ALenum, ALfloat);
typedef void      (AL_APIENTRY *LPALEFFECTFV)(ALuint, ALenum, const ALfloat*);
typedef void      (AL_APIENTRY *LPALGENFILTERS)(ALsizei, ALuint*);
typedef void      (AL_APIENTRY *LPALDELETEFILTERS)(ALsizei, const ALuint*);
typedef ALboolean (AL_APIENTRY *LPALISFILTER)(ALuint);
typedef void      (AL_APIENTRY *LPALFILTERI)(ALuint, ALenum, ALint);
typedef void      (AL_APIENTRY *LPALFILTERF)(ALuint, ALenum, ALfloat);
typedef void      (AL_APIENTRY *LPALGENAUXILIARYEFFECTSLOTS)(ALsizei, ALuint*);
typedef void      (AL_APIENTRY *LPALDELETEAUXILIARYEFFECTSLOTS)(ALsizei, const ALuint*);
typedef ALboolean (AL_APIENTRY *LPALISAUXILIARYEFFECTSLOT)(ALuint);
typedef void      (AL_APIENTRY *LPALAUXILIARYEFFECTSLOTI)(ALuint, ALenum, ALint);
typedef void      (AL_APIENTRY *LPALAUXILIARYEFFECTSLOTF)(ALuint, ALenum, ALfloat);

#ifdef __cplusplus
}
#endif
#endif /* DOS_AL_EFX_H */

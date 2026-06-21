/*
 * AL/alext.h — dOS OpenAL shim extension header (doc 10 §0, §3c, §3d).
 *
 * dhewm3 includes <AL/al.h>, <AL/alc.h>, <AL/alext.h> (snd_local.h:42-44). This
 * header aggregates the EFX tokens/prototypes (efx.h) and the ALC SOFT bits.
 *
 * IMPORTANT (doc 10 §3c): do NOT #define ALC_SOFT_HRTF here — snd_local.h:49
 * declares the LPALCRESETDEVICESOFT typedef itself when ALC_SOFT_HRTF is absent.
 * We report HRTF absent (doc 10 §4b) so alcResetDeviceSOFT stays NULL. We still
 * provide the symbol/typedef for any direct reference.
 */
#ifndef DOS_AL_ALEXT_H
#define DOS_AL_ALEXT_H

#include "al.h"
#include "alc.h"
#include "efx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ALC_SOFT_HRTF device-reset entry (doc 10 §3d). Resolvable via
 * alcGetProcAddress("alcResetDeviceSOFT"); we keep it NULL by reporting HRTF
 * absent, but declare it for completeness. */
typedef ALCboolean (ALC_APIENTRY *LPALCRESETDEVICESOFT)(ALCdevice *device,
                                                        const ALCint *attribs);
ALC_API ALCboolean ALC_APIENTRY alcResetDeviceSOFT(ALCdevice *device,
                                                   const ALCint *attribs);

#ifdef __cplusplus
}
#endif
#endif /* DOS_AL_ALEXT_H */

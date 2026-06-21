/*
 * AL/alc.h — dOS OpenAL shim context/device header (doc 10 §3c-§3d, §1b).
 *
 * The "device" is the dOS virtio-snd/serial sink; there is exactly one. ALC
 * device/context are thin tags over the single global ma_engine. Match
 * OpenAL-soft numerics. All extern "C".
 */
#ifndef DOS_AL_ALC_H
#define DOS_AL_ALC_H

#ifndef ALC_API
#define ALC_API   extern
#endif
#ifndef ALC_APIENTRY
#define ALC_APIENTRY
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* --- ALC scalar types + opaque handles (doc 10 §3b) --- */
typedef char            ALCboolean;
typedef char            ALCchar;
typedef signed char     ALCbyte;
typedef unsigned char   ALCubyte;
typedef short           ALCshort;
typedef unsigned short  ALCushort;
typedef int             ALCint;
typedef unsigned int    ALCuint;
typedef int             ALCsizei;
typedef int             ALCenum;
typedef float           ALCfloat;
typedef double          ALCdouble;
typedef void            ALCvoid;

typedef struct ALCdevice  ALCdevice;
typedef struct ALCcontext ALCcontext;

/* --- constants (doc 10 §3c) --- */
#define ALC_FALSE                  0
#define ALC_TRUE                   1
#define ALC_NO_ERROR               0
#define ALC_INVALID_DEVICE         0xA001
#define ALC_INVALID_CONTEXT        0xA002
#define ALC_INVALID_ENUM           0xA003
#define ALC_INVALID_VALUE          0xA004
#define ALC_OUT_OF_MEMORY          0xA005

#define ALC_CONNECTED              0x313    /* alcGetIntegerv, ALC_EXT_disconnect */
#define ALC_ALL_DEVICES_SPECIFIER  0x1013

/*
 * SOFT/HRTF tokens: snd_local.h provides fallbacks if absent (doc 10 §3c). The
 * cleanest dOS choice is to NOT define ALC_SOFT_HRTF here and report HRTF absent
 * so alcResetDeviceSOFT stays NULL. The tokens below are provided defensively for
 * any direct reference; they match OpenAL-soft values.
 */
#define ALC_HRTF_SOFT              0x1992
#define ALC_HRTF_ID_SOFT           0x1996
#define ALC_DONT_CARE_SOFT         0x0002
#define ALC_OUTPUT_LIMITER_SOFT    0x199A
#define ALC_OUTPUT_MODE_SOFT       0x19AC

/* --- context / device functions (doc 10 §3d) --- */
ALC_API ALCdevice*  ALC_APIENTRY alcOpenDevice(const ALCchar *devicename);
ALC_API ALCboolean  ALC_APIENTRY alcCloseDevice(ALCdevice *device);
ALC_API ALCcontext* ALC_APIENTRY alcCreateContext(ALCdevice *device, const ALCint *attrlist);
ALC_API ALCboolean  ALC_APIENTRY alcMakeContextCurrent(ALCcontext *context);
ALC_API void        ALC_APIENTRY alcDestroyContext(ALCcontext *context);
ALC_API void        ALC_APIENTRY alcProcessContext(ALCcontext *context);
ALC_API void        ALC_APIENTRY alcSuspendContext(ALCcontext *context);
ALC_API ALCenum     ALC_APIENTRY alcGetError(ALCdevice *device);
ALC_API ALCboolean  ALC_APIENTRY alcIsExtensionPresent(ALCdevice *device, const ALCchar *extname);
ALC_API void*       ALC_APIENTRY alcGetProcAddress(ALCdevice *device, const ALCchar *funcname);
ALC_API const ALCchar* ALC_APIENTRY alcGetString(ALCdevice *device, ALCenum param);
ALC_API void        ALC_APIENTRY alcGetIntegerv(ALCdevice *device, ALCenum param,
                                                ALCsizei size, ALCint *values);

#ifdef __cplusplus
}
#endif
#endif /* DOS_AL_ALC_H */

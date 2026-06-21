/*
 * AL/al.h — dOS OpenAL shim core header (doc 10 §3a-§3d).
 *
 * dhewm3 #include <AL/al.h>; the tree ships NO system AL headers, so this is it.
 * Match OpenAL-soft numerics EXACTLY (dhewm3 casts alGetProcAddress results to
 * OpenAL-soft LPAL* typedefs — a mismatch is silent UB). All functions extern "C",
 * plain RISC-V calling convention (AL_APIENTRY empty), static-shim linkage.
 */
#ifndef DOS_AL_AL_H
#define DOS_AL_AL_H

/* --- calling-convention / linkage macros (doc 10 §3a) --- */
#ifndef AL_API
#define AL_API   extern        /* bare metal: no __declspec, static shim */
#endif
#ifndef AL_APIENTRY
#define AL_APIENTRY            /* empty: default RISC-V C calling convention */
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* --- scalar types (doc 10 §3b) --- */
typedef char            ALboolean;   /* AL_FALSE=0, AL_TRUE=1 */
typedef char            ALchar;
typedef signed char     ALbyte;
typedef unsigned char   ALubyte;
typedef short           ALshort;
typedef unsigned short  ALushort;
typedef int             ALint;
typedef unsigned int    ALuint;      /* buffer/source/effect/filter/slot HANDLES */
typedef int             ALsizei;
typedef int             ALenum;
typedef float           ALfloat;
typedef double          ALdouble;
typedef void            ALvoid;

/* --- core enum/constant values (doc 10 §3c) — must match OpenAL-soft --- */
#define AL_NONE                 0
#define AL_FALSE                0
#define AL_TRUE                 1
#define AL_NO_ERROR             0

/* error codes (returned by alGetError; OpenAL-soft values) */
#define AL_INVALID_NAME         0xA001
#define AL_INVALID_ENUM         0xA002
#define AL_INVALID_VALUE        0xA003
#define AL_INVALID_OPERATION    0xA004
#define AL_OUT_OF_MEMORY        0xA005

#define AL_SOURCE_RELATIVE      0x202
#define AL_PITCH                0x1003
#define AL_POSITION             0x1004
#define AL_VELOCITY             0x1006   /* implied; dhewm3 rarely sets it */
#define AL_LOOPING              0x1007
#define AL_BUFFER               0x1009
#define AL_GAIN                 0x100A   /* source AND listener gain */
#define AL_ORIENTATION          0x100F   /* listener at+up (6 floats) */
#define AL_SOURCE_STATE         0x1010
#define AL_INITIAL              0x1011
#define AL_PLAYING              0x1012
#define AL_PAUSED               0x1013
#define AL_STOPPED              0x1014
#define AL_BUFFERS_QUEUED       0x1015
#define AL_BUFFERS_PROCESSED    0x1016
#define AL_REFERENCE_DISTANCE   0x1020
#define AL_ROLLOFF_FACTOR       0x1021
#define AL_MAX_DISTANCE         0x1023

#define AL_FORMAT_MONO16        0x1101
#define AL_FORMAT_STEREO16      0x1103

#define AL_VENDOR               0xB001
#define AL_VERSION              0xB002
#define AL_RENDERER             0xB003
#define AL_EXTENSIONS           0xB004

/* --- buffers (doc 10 §3d) --- */
AL_API void      AL_APIENTRY alGenBuffers(ALsizei n, ALuint *buffers);
AL_API void      AL_APIENTRY alDeleteBuffers(ALsizei n, const ALuint *buffers);
AL_API ALboolean AL_APIENTRY alIsBuffer(ALuint buffer);
AL_API void      AL_APIENTRY alBufferData(ALuint buffer, ALenum format,
                                          const ALvoid *data, ALsizei size, ALsizei freq);

/* --- sources --- */
AL_API void      AL_APIENTRY alGenSources(ALsizei n, ALuint *sources);
AL_API void      AL_APIENTRY alDeleteSources(ALsizei n, const ALuint *sources);
AL_API ALboolean AL_APIENTRY alIsSource(ALuint source);
AL_API void      AL_APIENTRY alSourcei (ALuint source, ALenum param, ALint value);
AL_API void      AL_APIENTRY alSourcef (ALuint source, ALenum param, ALfloat value);
AL_API void      AL_APIENTRY alSource3f(ALuint source, ALenum param,
                                        ALfloat v1, ALfloat v2, ALfloat v3);
AL_API void      AL_APIENTRY alSource3i(ALuint source, ALenum param,
                                        ALint v1, ALint v2, ALint v3);
AL_API void      AL_APIENTRY alGetSourcei(ALuint source, ALenum param, ALint *value);
AL_API void      AL_APIENTRY alSourcePlay (ALuint source);
AL_API void      AL_APIENTRY alSourceStop (ALuint source);
AL_API void      AL_APIENTRY alSourcePause(ALuint source);
AL_API void      AL_APIENTRY alSourceQueueBuffers  (ALuint source, ALsizei n,
                                                    const ALuint *buffers);
AL_API void      AL_APIENTRY alSourceUnqueueBuffers(ALuint source, ALsizei n,
                                                    ALuint *buffers);

/* --- listener --- */
AL_API void      AL_APIENTRY alListenerf (ALenum param, ALfloat value);
AL_API void      AL_APIENTRY alListenerfv(ALenum param, const ALfloat *values);

/* --- state / queries --- */
AL_API ALenum        AL_APIENTRY alGetError(void);
AL_API const ALchar* AL_APIENTRY alGetString(ALenum param);
AL_API void*         AL_APIENTRY alGetProcAddress(const ALchar *fname);
AL_API ALboolean     AL_APIENTRY alIsExtensionPresent(const ALchar *extname);

#ifdef __cplusplus
}
#endif
#endif /* DOS_AL_AL_H */

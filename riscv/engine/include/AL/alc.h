/*
 * AL/alc.h — minimal, self-contained OpenAL context (ALC) header for the
 * bare-metal dhewm3 build. See AL/al.h for the rationale. Functions are stubbed
 * in neo/sys/stub/openal_stub.cpp. Canonical OpenAL 1.1 ABI values.
 */
#ifndef AL_ALC_H
#define AL_ALC_H

#if defined(__cplusplus)
extern "C" {
#endif

#ifndef ALC_API
 #define ALC_API extern
#endif
#ifndef ALC_APIENTRY
 #define ALC_APIENTRY
#endif

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

#define ALC_FALSE                                0
#define ALC_TRUE                                 1
#define ALC_NO_ERROR                             0

#define ALC_CONNECTED                            0x313
#define ALC_ALL_DEVICES_SPECIFIER                0x1013

/* ---- ALC entry points (stubbed in openal_stub.cpp) ---- */
ALC_API ALCcontext*   ALC_APIENTRY alcCreateContext( ALCdevice *device, const ALCint *attrlist );
ALC_API ALCboolean    ALC_APIENTRY alcMakeContextCurrent( ALCcontext *context );
ALC_API void          ALC_APIENTRY alcProcessContext( ALCcontext *context );
ALC_API void          ALC_APIENTRY alcSuspendContext( ALCcontext *context );
ALC_API void          ALC_APIENTRY alcDestroyContext( ALCcontext *context );

ALC_API ALCdevice*    ALC_APIENTRY alcOpenDevice( const ALCchar *devicename );
ALC_API ALCboolean    ALC_APIENTRY alcCloseDevice( ALCdevice *device );

ALC_API ALCenum       ALC_APIENTRY alcGetError( ALCdevice *device );
ALC_API ALCboolean    ALC_APIENTRY alcIsExtensionPresent( ALCdevice *device, const ALCchar *extname );
ALC_API void*         ALC_APIENTRY alcGetProcAddress( ALCdevice *device, const ALCchar *funcname );
ALC_API const ALCchar*ALC_APIENTRY alcGetString( ALCdevice *device, ALCenum param );
ALC_API void          ALC_APIENTRY alcGetIntegerv( ALCdevice *device, ALCenum param, ALCsizei size, ALCint *values );

#if defined(__cplusplus)
}
#endif

#endif /* AL_ALC_H */

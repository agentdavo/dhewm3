/*
 * AL/al.h — minimal, self-contained OpenAL core header for the bare-metal
 * dhewm3 build. There is no system OpenAL on the dOS target; the AL_* functions
 * are provided as no-op stubs by neo/sys/stub/openal_stub.cpp. This header only
 * needs to declare the types, function prototypes and enum constants that the
 * dhewm3 sound code (neo/sound/*) and the stub actually reference. Values are the
 * canonical OpenAL 1.1 ABI constants.
 *
 * This is NOT a full OpenAL header — it is a downstream build shim for the fork
 * (mirrors the SDL3 "no upstream platform arm" patch approach).
 */
#ifndef AL_AL_H
#define AL_AL_H

#if defined(__cplusplus)
extern "C" {
#endif

#ifndef AL_API
 #define AL_API extern
#endif
#ifndef AL_APIENTRY
 #define AL_APIENTRY
#endif

typedef char            ALboolean;
typedef char            ALchar;
typedef signed char     ALbyte;
typedef unsigned char   ALubyte;
typedef short           ALshort;
typedef unsigned short  ALushort;
typedef int             ALint;
typedef unsigned int    ALuint;
typedef int             ALsizei;
typedef int             ALenum;
typedef float           ALfloat;
typedef double          ALdouble;
typedef void            ALvoid;

/* Booleans / errors */
#define AL_FALSE                                 0
#define AL_TRUE                                  1
#define AL_NO_ERROR                              0

/* Source/listener properties */
#define AL_SOURCE_RELATIVE                       0x0202
#define AL_PITCH                                 0x1003
#define AL_POSITION                              0x1004
#define AL_LOOPING                               0x1007
#define AL_BUFFER                                0x1009
#define AL_GAIN                                  0x100A
#define AL_ORIENTATION                           0x100F
#define AL_SOURCE_STATE                          0x1010
#define AL_PLAYING                               0x1012
#define AL_BUFFERS_PROCESSED                     0x1016
#define AL_REFERENCE_DISTANCE                    0x1020
#define AL_ROLLOFF_FACTOR                        0x1021
#define AL_MAX_DISTANCE                          0x1023
#define AL_DIRECT_FILTER                         0x20005
#define AL_AUXILIARY_SEND_FILTER                 0x20006

/* Buffer formats */
#define AL_FORMAT_MONO16                         0x1101
#define AL_FORMAT_STEREO16                       0x1103

/* String queries */
#define AL_VENDOR                                0xB001
#define AL_VERSION                               0xB002
#define AL_RENDERER                              0xB003
#define AL_EXTENSIONS                            0xB004

/* ---- core entry points (stubbed in openal_stub.cpp) ---- */
AL_API void          AL_APIENTRY alEnable( ALenum capability );
AL_API void          AL_APIENTRY alDisable( ALenum capability );
AL_API ALenum        AL_APIENTRY alGetError( void );
AL_API ALboolean     AL_APIENTRY alIsExtensionPresent( const ALchar *extname );
AL_API void*         AL_APIENTRY alGetProcAddress( const ALchar *fname );
AL_API ALenum        AL_APIENTRY alGetEnumValue( const ALchar *ename );
AL_API const ALchar* AL_APIENTRY alGetString( ALenum param );
AL_API ALint         AL_APIENTRY alGetInteger( ALenum param );

AL_API void          AL_APIENTRY alListenerf( ALenum param, ALfloat value );
AL_API void          AL_APIENTRY alListenerfv( ALenum param, const ALfloat *values );

AL_API void          AL_APIENTRY alGenSources( ALsizei n, ALuint *sources );
AL_API void          AL_APIENTRY alDeleteSources( ALsizei n, const ALuint *sources );
AL_API ALboolean     AL_APIENTRY alIsSource( ALuint sid );
AL_API void          AL_APIENTRY alSourcef( ALuint sid, ALenum param, ALfloat value );
AL_API void          AL_APIENTRY alSource3f( ALuint sid, ALenum param, ALfloat v1, ALfloat v2, ALfloat v3 );
AL_API void          AL_APIENTRY alSourcei( ALuint sid, ALenum param, ALint value );
AL_API void          AL_APIENTRY alSource3i( ALuint sid, ALenum param, ALint v1, ALint v2, ALint v3 );
AL_API void          AL_APIENTRY alGetSourcei( ALuint sid, ALenum param, ALint *value );
AL_API void          AL_APIENTRY alSourcePlay( ALuint sid );
AL_API void          AL_APIENTRY alSourceStop( ALuint sid );
AL_API void          AL_APIENTRY alSourcePause( ALuint sid );
AL_API void          AL_APIENTRY alSourceQueueBuffers( ALuint sid, ALsizei n, const ALuint *buffers );
AL_API void          AL_APIENTRY alSourceUnqueueBuffers( ALuint sid, ALsizei n, ALuint *buffers );

AL_API void          AL_APIENTRY alGenBuffers( ALsizei n, ALuint *buffers );
AL_API void          AL_APIENTRY alDeleteBuffers( ALsizei n, const ALuint *buffers );
AL_API ALboolean     AL_APIENTRY alIsBuffer( ALuint bid );
AL_API void          AL_APIENTRY alBufferData( ALuint bid, ALenum format, const ALvoid *data, ALsizei size, ALsizei freq );

#if defined(__cplusplus)
}
#endif

#endif /* AL_AL_H */

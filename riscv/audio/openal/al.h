/*
 * openal/al.h — INTERNAL shim header (NOT the AL/al.h ABI header).
 *
 * Declares the shim-private types + the engine-owning glue (dos_audio_pump lives
 * in al.c because ma_engine is owned here, doc 13 §5.2). The public OpenAL ABI is
 * in openal/AL/{al,alc,efx,alext}.h. dhewm3 never includes this file.
 */
#ifndef DOS_OPENAL_SHIM_INTERNAL_H
#define DOS_OPENAL_SHIM_INTERNAL_H

#include <stdint.h>

/* Pool sizes (doc 10 §0/§4d: dhewm3 probes up to 256 sources, reserves 8). */
#define SHIM_MAX_SOURCES   256
#define SHIM_MAX_BUFFERS   4096   /* static + streaming buffer handles */
#define SHIM_MAX_EFFECTS   64
#define SHIM_MAX_FILTERS   64
#define SHIM_MAX_SLOTS     8      /* one aux effect slot per sound world */
#define SHIM_STREAM_DEPTH  3      /* AL 3-buffer streaming ring (doc 10 §0) */

/* Engine output (doc 11 §2/§9, doc 13 §1). */
#define SHIM_OUT_CHANNELS  2
#define SHIM_OUT_RATE      44100  /* PRIMARYFREQ */
#define SHIM_MIX_FRAMES    4096   /* MIXBUFFER_SAMPLES */

/* Init the global engine + open the sink. Called from alcCreateContext. */
int  shim_engine_init(uint32_t channels, uint32_t rate);
void shim_engine_shutdown(void);
int  shim_engine_inited(void);

#endif /* DOS_OPENAL_SHIM_INTERNAL_H */

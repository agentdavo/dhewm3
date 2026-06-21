/*
 * miniaudio_impl.c — the ONE translation unit that compiles miniaudio's
 * implementation for the bare-metal rva23s64 build.
 *
 * PROVEN (doc 20 §1): compiles clean under -Wall -Wextra with GCC 16 + newlib,
 * links self-contained, ~13 MB .o. Device-less ma_engine + ma_engine_read_pcm_frames
 * confirmed working.
 *
 * The MA_NO_* set + nanosleep predeclaration come from ma_config.h so every audio
 * TU stays byte-identical (see ma_config.h — the struct-layout landmine).
 */
#define MINIAUDIO_IMPLEMENTATION
#include "ma_config.h"     /* MA_NO_* set + nanosleep predecl — must precede miniaudio.h */
#include "miniaudio.h"

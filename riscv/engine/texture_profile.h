#ifndef BM_TEXTURE_PROFILE_H
#define BM_TEXTURE_PROFILE_H
#include <stdint.h>
enum { BM_TEX_LOAD, BM_TEX_DECODE, BM_TEX_GENERATE, BM_TEX_MIP,
       BM_TEX_GL, BM_TEX_SOFT, BM_TEX_COPY, BM_TEX_FILE, BM_TEX_TGA,
       BM_TEX_HEIGHT, BM_TEX_NORMALS, BM_TEX_PHASES };
#ifdef __cplusplus
extern "C" {
#endif
uint64_t BM_TextureBegin(void) __attribute__((weak));
void BM_TextureEnd(int phase, uint64_t start, uint64_t bytes, const char *name) __attribute__((weak));
void BM_TextureReset(void) __attribute__((weak));
void BM_TextureReport(const char *label) __attribute__((weak));
#ifdef __cplusplus
}
class BMTextureScope {
    int phase;
    uint64_t start, bytes;
    const char *name;
public:
    BMTextureScope(int p, const char *n = 0, uint64_t b = 0) :
        phase(p), start(BM_TextureBegin ? BM_TextureBegin() : 0), bytes(b), name(n) {}
    ~BMTextureScope() { if (BM_TextureEnd) BM_TextureEnd(phase,start,bytes,name); }
};
#endif
#endif

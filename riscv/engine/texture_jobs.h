#ifndef BM_TEXTURE_JOBS_H
#define BM_TEXTURE_JOBS_H
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*bm_texture_rows_fn)(void *ctx, int first, int end);
/* Main-thread only. Synchronous; callbacks must not allocate, submit GL work,
 * or use shared parser/filesystem state. Each owns [first,end) output rows. */
void BM_TextureRows(int rows, int width, bm_texture_rows_fn fn, void *ctx);
extern int bm_texture_parallel;
extern int bm_texture_validate;
#ifdef __cplusplus
}
#endif
#endif

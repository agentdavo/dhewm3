/* Shared internal alignment macros. */
#ifndef SOFTGLES2_SG_ALIGN_H
#define SOFTGLES2_SG_ALIGN_H

#define SG_RVV_ALIGN 64

#define SG_CONCAT2_(a, b) a##b
#define SG_CONCAT2(a, b) SG_CONCAT2_(a, b)
#define SG_STATIC_ASSERT(cond) typedef char SG_CONCAT2(sg_static_assert_, __LINE__)[(cond) ? 1 : -1]

#if defined(__GNUC__) || defined(__clang__)
#  define SG_ALIGNED_TYPE(n) __attribute__((aligned(n)))
#  define SG_ALIGNED_VAR(n)  __attribute__((aligned(n)))
#else
#  define SG_ALIGNED_TYPE(n)
#  define SG_ALIGNED_VAR(n)
#endif

#endif /* SOFTGLES2_SG_ALIGN_H */

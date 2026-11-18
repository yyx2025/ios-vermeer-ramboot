/* Audit-only replacement for upstream libc dependencies. */
#ifndef ZSTD_DEPS_COMMON
#define ZSTD_DEPS_COMMON
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
#define ZSTD_memcpy(d,s,l) __builtin_memcpy((d),(s),(l))
#define ZSTD_memmove(d,s,l) __builtin_memmove((d),(s),(l))
#define ZSTD_memset(p,v,l) __builtin_memset((p),(v),(l))
#endif
#ifdef ZSTD_DEPS_NEED_MALLOC
#ifndef ZSTD_DEPS_MALLOC
#define ZSTD_DEPS_MALLOC
#define ZSTD_malloc(s) ((void)(s), (void *)0)
#define ZSTD_calloc(n,s) ((void)(n), (void)(s), (void *)0)
#define ZSTD_free(p) ((void)(p))
#endif
#endif
#ifdef ZSTD_DEPS_NEED_MATH64
#ifndef ZSTD_DEPS_MATH64
#define ZSTD_DEPS_MATH64
#define ZSTD_div64(dividend,divisor) ((dividend)/(divisor))
#endif
#endif
#ifdef ZSTD_DEPS_NEED_ASSERT
#ifndef ZSTD_DEPS_ASSERT
#define ZSTD_DEPS_ASSERT
#define assert(c) ((void)0)
#endif
#endif
#ifdef ZSTD_DEPS_NEED_IO
#ifndef ZSTD_DEPS_IO
#define ZSTD_DEPS_IO
#define ZSTD_DEBUG_PRINT(...) ((void)0)
#endif
#endif

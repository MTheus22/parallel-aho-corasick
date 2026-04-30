#ifndef AC_COMMON_H
#define AC_COMMON_H

#include <stddef.h>
#include <stdint.h>

/* Single byte alphabet: every implementation here scans raw bytes, so we
 * always have 256 transitions per state. Patterns and text are byte
 * sequences (NUL is a valid byte). */
#define AC_ALPHABET_SIZE 256

/* Sentinel used in transition tables / linked lists. */
#define AC_NIL ((int32_t)-1)

/* Error codes returned by the public API. */
enum {
    AC_OK              = 0,
    AC_E_NOMEM         = -1,
    AC_E_INVAL         = -2,
    AC_E_PATTERN_EMPTY = -3,
    AC_E_NOT_FOUND     = -4,
    AC_E_THREAD        = -5,
};

#if defined(__GNUC__) || defined(__clang__)
#  define AC_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define AC_UNLIKELY(x) __builtin_expect(!!(x), 0)
#  define AC_INLINE      static inline __attribute__((always_inline))
#  define AC_RESTRICT    __restrict__
#else
#  define AC_LIKELY(x)   (x)
#  define AC_UNLIKELY(x) (x)
#  define AC_INLINE      static inline
#  define AC_RESTRICT
#endif

#endif /* AC_COMMON_H */

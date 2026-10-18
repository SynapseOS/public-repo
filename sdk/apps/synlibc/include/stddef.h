#ifndef STDDEF_H
#define STDDEF_H 1

#include <stdint.h>

typedef int64_t ssize_t;
typedef uintmax_t size_t;
typedef intmax_t ptrdiff_t;
typedef uint_least32_t wchar_t;
typedef uintptr_t nullptr_t;

#define NULL ((void *)0)
#define offsetof(s, m) __builtin_offsetof(s, m)
#define unreachable( ) __builtin_unreachable( )

#endif

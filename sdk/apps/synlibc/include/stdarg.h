#ifndef STDARG_H
#define STDARG_H 1

typedef __builtin_va_list va_list;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#define va_start(v, ...) __builtin_va_start(v, 0)
#else
#define va_start(v, l) __builtin_va_start(v, l)
#endif

#define va_end(v) __builtin_va_end(v)
#define va_arg(v, l) __builtin_va_arg(v, l)

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)
#define va_copy(d, s) __builtin_va_copy(d, s)
#endif

#endif

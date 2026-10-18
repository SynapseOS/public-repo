// inference.c: Выбор скалярного или AVX2/FMA вычисления Q8 GEMV
// Автор: Елчинян Арен Артурович, 2026
// Подсистема: synlibc, пользовательская библиотека нативного API
// Создан: 13.10.2026
// Последнее изменение: 14.10.2026

#include <synapse/inference.h>
#include <synapse/orbit.h>
#include "q8-dot.h"

static unsigned cached_backend; // 0 до инициализации, затем backend + 1

unsigned syn_inference_backend(void) {
	unsigned cached = __atomic_load_n(&cached_backend, __ATOMIC_ACQUIRE);

	if (cached) {
		return cached - 1;
	}

	unsigned a, b, c, d, backend = SYN_INFER_BACKEND_SCALAR;
	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
	unsigned maximum = a;

	if (maximum >= 7) {
		__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));

		if ((c & ((1u << 27) | (1u << 28) | (1u << 12))) ==
		    ((1u << 27) | (1u << 28) | (1u << 12))) {
			unsigned lo, hi;
			__asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));

			if ((lo & 6) == 6) {
				__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));

				if (b & (1u << 5)) {
					backend = SYN_INFER_BACKEND_AVX2_FMA;
				}
			}
		}
	}

	__atomic_store_n(&cached_backend, backend + 1, __ATOMIC_RELEASE);
	return backend;
}

static void gemv_scalar(float *out, const int8_t *x, const float *xs, const int8_t *w,
                        const float *ws, size_t n, size_t rows, size_t group) {
	for (size_t row = 0; row < rows; row++) {
		float value = 0;

		for (size_t j = 0; j < n; j += group) {
			int32_t dot = 0;

			for (size_t k = 0; k < group; k++) {
				dot += (int32_t)x[j + k] * w[row * n + j + k];
			}

			value += ((float)dot) * ws[(row * n + j) / group] * xs[j / group];
		}

		out[row] = value;
	}
}

__attribute__((target("avx2,fma"))) static void gemv_avx2(float *out, const int8_t *x,
                                                          const float *xs, const int8_t *w,
                                                          const float *ws, size_t n, size_t rows) {
	for (size_t row = 0; row < rows; row++) {
		float value = 0;

		for (size_t j = 0; j < n; j += 64) {
			int32_t dot = syn_q8_dot64(x + j, w + row * n + j);
			value += ((float)dot) * ws[(row * n + j) / 64] * xs[j / 64];
		}

		out[row] = value;
	}
	// GCC не отслеживает YMM-регистры встроенного asm для автоматического vzeroupper
	// Очищаем верхние половины перед возвратом в SSE-код libc
	__asm__ volatile("vzeroupper" ::: "memory");
}

int syn_q8_gemv(float *out, const int8_t *x, const float *xs, const int8_t *w, const float *ws,
                size_t n, size_t rows, size_t group, unsigned flags) {
	if (!out || !x || !xs || !w || !ws || !n || !group || group > 1024 || n % group ||
	    (flags & ~SYN_INFER_FORCE_SCALAR)) {
		return -SYN_EINVAL;
	}

	if (rows > SIZE_MAX / n || rows > SIZE_MAX / sizeof(float) ||
	    n / group > SIZE_MAX / sizeof(float) || rows > SIZE_MAX / sizeof(float) / (n / group)) {
		return -SYN_EOVERFLOW;
	}

	if (group == 64 && !(flags & SYN_INFER_FORCE_SCALAR) &&
	    syn_inference_backend( ) == SYN_INFER_BACKEND_AVX2_FMA) {
		gemv_avx2(out, x, xs, w, ws, n, rows);
	} else {
		gemv_scalar(out, x, xs, w, ws, n, rows, group);
	}

	return 0;
}

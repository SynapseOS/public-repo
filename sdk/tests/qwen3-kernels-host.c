/* Scalar oracle, signed extremes, tails, and deliberately unaligned buffers. */
#include <stdio.h>
#include <string.h>
#include <synapse/orbit.h>
#include <synapse/inference.h>
#include "../apps/synlibc/q8-dot.h"

static int32_t checked_dot(const int8_t *x, const int8_t *w, int count) {
	int32_t result = 0;
	int k = 0;

	if (syn_inference_backend( ) == SYN_INFER_BACKEND_AVX2_FMA)
		for (; k + 64 <= count; k += 64)
			result += syn_q8_dot64(x + k, w + k);
	for (; k < count; k++)
		result += (int32_t)x[k] * w[k];
	return result;
}

static uint32_t random_state = 123456789;

static uint32_t random_word(void) {
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}

static int gemv_cases(void) {
	static int8_t x[4096], w[4096 * 33];
	static float xs[4096], ws[4096 * 33], reference[33], fast[33], scalar[33];
	const size_t groups[] = { 1, 3, 8, 16, 32, 64, 128, 256, 1024 };

	for (size_t g = 0; g < sizeof(groups) / sizeof(groups[0]); g++) {
		size_t group = groups[g], n = group * 4, rows = 33;

		for (size_t i = 0; i < n; i++)
			x[i] = (int8_t)random_word( );
		for (size_t i = 0; i < n * rows; i++)
			w[i] = (int8_t)random_word( );
		for (size_t i = 0; i < n / group; i++)
			xs[i] = i % 2 ? 0.03125f : 0.0625f;
		for (size_t i = 0; i < rows * n / group; i++)
			ws[i] = i % 2 ? 0.125f : 0.25f;
		for (size_t row = 0; row < rows; row++) {
			reference[row] = 0;

			for (size_t j = 0; j < n; j += group) {
				int32_t sum = 0;

				for (size_t k = 0; k < group; k++)
					sum += (int32_t)x[j + k] * w[row * n + j + k];
				reference[row] += ((float)sum) * ws[(row * n + j) / group] * xs[j / group];
			}
		}

		if (syn_q8_gemv(fast, x, xs, w, ws, n, rows, group, 0) ||
		    syn_q8_gemv(scalar, x, xs, w, ws, n, rows, group, SYN_INFER_FORCE_SCALAR) ||
		    memcmp(fast, reference, sizeof(reference)) ||
		    memcmp(scalar, reference, sizeof(reference))) {
			fprintf(stderr, "GEMV_FAIL group=%zu\n", group);
			return 1;
		}
	}

	if (syn_q8_gemv(fast, x, xs, w, ws, 65, 1, 64, 0) != -SYN_EINVAL ||
	    syn_q8_gemv(NULL, x, xs, w, ws, 64, 1, 64, 0) != -SYN_EINVAL ||
	    syn_q8_gemv(fast, x, xs, w, ws, 64, SIZE_MAX, 64, 0) != -SYN_EOVERFLOW ||
	    syn_q8_gemv(fast, x, xs, w, ws, 64, 1, 64, 2) != -SYN_EINVAL)
		return 1;
	puts("SYN_Q8_GEMV_PASS dispatch/scalar, groups 1..1024, range errors");
	return 0;
}

int main(void) {
	int8_t x[272], w[272];

	for (int trial = 0; trial < 10000; trial++) {
		for (int i = 0; i < 272; i++) {
			x[i] = trial < 4 ? (trial & 1 ? 127 : -128) : (int8_t)random_word( );
			w[i] = trial < 4 ? (trial & 2 ? 127 : -128) : (int8_t)random_word( );
		}

		for (int n = 1; n <= 256; n++) {
			int offset = trial % 16;
			int32_t expected = 0;

			for (int i = 0; i < n; i++)
				expected += (int32_t)x[i + offset] * w[i + offset];
			int32_t actual = checked_dot(x + offset, w + offset, n);

			if (actual != expected) {
				fprintf(stderr, "DOT_FAIL trial=%d n=%d %d != %d\n", trial, n, actual, expected);
				return 1;
			}
		}
	}

	if (gemv_cases( ))
		return 1;
	puts("QWEN_DOT_PASS exact signed dot, extremes, tails, unaligned, 2560000 cases");
	return 0;
}

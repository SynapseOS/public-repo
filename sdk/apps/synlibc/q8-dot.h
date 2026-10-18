#ifndef SYN_Q8_DOT_H
#define SYN_Q8_DOT_H

#include <stdint.h>

static inline int32_t syn_q8_dot64(const int8_t *x, const int8_t *w) {
	int32_t result;
	__asm__ volatile("vpmovsxbw 0(%1), %%ymm0\n\t"
	                 "vpmovsxbw 0(%2), %%ymm1\n\t"
	                 "vpmaddwd %%ymm1, %%ymm0, %%ymm0\n\t"
	                 "vpmovsxbw 16(%1), %%ymm1\n\t"
	                 "vpmovsxbw 16(%2), %%ymm2\n\t"
	                 "vpmaddwd %%ymm2, %%ymm1, %%ymm1\n\t"
	                 "vpaddd %%ymm1, %%ymm0, %%ymm0\n\t"
	                 "vpmovsxbw 32(%1), %%ymm1\n\t"
	                 "vpmovsxbw 32(%2), %%ymm2\n\t"
	                 "vpmaddwd %%ymm2, %%ymm1, %%ymm1\n\t"
	                 "vpaddd %%ymm1, %%ymm0, %%ymm0\n\t"
	                 "vpmovsxbw 48(%1), %%ymm1\n\t"
	                 "vpmovsxbw 48(%2), %%ymm2\n\t"
	                 "vpmaddwd %%ymm2, %%ymm1, %%ymm1\n\t"
	                 "vpaddd %%ymm1, %%ymm0, %%ymm0\n\t"
	                 "vextracti128 $1, %%ymm0, %%xmm1\n\t"
	                 "vpaddd %%xmm1, %%xmm0, %%xmm0\n\t"
	                 "vphaddd %%xmm0, %%xmm0, %%xmm0\n\t"
	                 "vphaddd %%xmm0, %%xmm0, %%xmm0\n\t"
	                 "vmovd %%xmm0, %0"
	                 : "=r"(result)
	                 : "r"(x), "r"(w)
	                 : "ymm0", "ymm1", "ymm2", "memory");
	return result;
}

#endif

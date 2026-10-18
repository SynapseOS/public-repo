/* Reference and optional exact QWEN_FAST kernels, on either OS API. */
#ifdef QWEN_ORBIT
#include "qwen3-orbit-platform.h"
#endif
#define main qwen3_upstream_main
#include "../apps/apps/qwen3.c/runq.c"
#undef main
#include <inttypes.h>

static uint64_t bench_clock(void) {
#ifdef QWEN_ORBIT
	return syn_clock_us( );
#else
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
#endif
}

static const int prompt[] = { 151644, 872, 198, 9707, 151645, 198, 151644, 77091, 198 };

int main(int argc, char **argv) {
	if (argc != 3) {
		fprintf(stderr, "usage: qwen3-bench MODEL.bin DECODE_TOKENS\n");
		return 2;
	}

	int tokens = atoi(argv[2]);

	if (tokens < 8 || tokens > 1024)
		return 2;
#ifdef QWEN_ORBIT
	if (syn_orbit_create(&qwen_orbit, 32)) {
		fprintf(stderr, "Orbit unavailable\n");
		return 1;
	}

	const char *api = "synapi";
#else
	const char *api = "posix";
#endif
#ifdef QWEN_FAST
	const char *kernel = "avx2-dot-rope-cache";
#else
	const char *kernel = "reference";
#endif
	uint64_t started = bench_clock( );
	Transformer t;
	build_transformer(&t, argv[1], tokens + 32);

	if (t.config.dim != 1024 || t.config.n_layers != 28 || t.config.vocab_size != 151936) {
		fprintf(stderr, "Qwen3-0.6B required\n");
		return 1;
	}

	Sampler sampler;
	build_sampler(&sampler, t.config.vocab_size, 0.0f, 1.0f, 42);
	uint64_t load_us = bench_clock( ) - started;
	int prompt_count = sizeof(prompt) / sizeof(prompt[0]);
	/* Touch every compute weight before timing. Warmup tokens are excluded. */
	for (int i = 0; i < 4; i++)
		forward(&t, prompt[0], i);
	uint64_t prefill_start = bench_clock( );
	float *logits = NULL;

	for (int i = 0; i < prompt_count; i++)
		logits = forward(&t, prompt[i], i);
	int token = sample(&sampler, logits);
	uint64_t prefill_us = bench_clock( ) - prefill_start;
	uint64_t hash = 1469598103934665603ULL;
	uint64_t decode_start = bench_clock( );

	for (int i = 0; i < tokens; i++) {
		logits = forward(&t, token, prompt_count + i);
		token = sample(&sampler, logits);
		hash ^= (uint64_t)token;
		hash *= 1099511628211ULL;
	}

	uint64_t decode_us = bench_clock( ) - decode_start;

	if (!decode_us)
		return 1;
	/* Validate every final vocabulary logit outside the timed decode. */
	uint64_t logits_hash = 1469598103934665603ULL;
	const unsigned char *logit_bytes = (const unsigned char *)logits;

	for (size_t i = 0; i < (size_t)t.config.vocab_size * sizeof(float); i++) {
		logits_hash ^= logit_bytes[i];
		logits_hash *= 1099511628211ULL;
	}

	printf(
	    "QWEN3_RESULT {\"api\":\"%s\",\"kernel\":\"%s\",\"model\":\"Qwen3-0.6B\",\"quantization\":\"Q8_0\",\"threads\":1,\"warmup_tokens\":4,\"prompt_tokens\":%d,\"decode_tokens\":%d,\"load_us\":%" PRIu64
	    ",\"prefill_us\":%" PRIu64 ",\"decode_us\":%" PRIu64
	    ",\"tokens_per_sec\":%.6f,\"final_logits_hash\":\"%016" PRIx64
	    "\",\"token_hash\":\"%016" PRIx64 "\"}\n",
	    api, kernel, prompt_count, tokens, load_us, prefill_us, decode_us,
	    (double)tokens * 1000000 / decode_us, logits_hash, hash);
	fflush(stdout);
	free_sampler(&sampler);
	free_transformer(&t);
#ifdef QWEN_ORBIT
	if (syn_orbit_destroy(&qwen_orbit))
		return 1;
#endif
	return 0;
}

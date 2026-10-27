// qwen.c: Измерить фиксированные prefill и decode в существующем qwen3.c
// Автор: Елчинян Арен Артурович, 2026, x86_64, Linux ABI
// Подсистема: CPU-инференс Qwen3 для Linux и SynapseOS
// Первое изменение: 19.10.2026, последнее изменение: 19.10.2026
// Доступ: общедоступный

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <limits.h>

// Upstream подключается без изменения его исходного файла
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wfloat-conversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#define main qwen_upstream_main
#include QWEN_SOURCE
#undef main
#pragma GCC diagnostic pop

// Получить время независимо от времени опроса измерителя
static double bench_seconds(void) {
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }

    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

// Проверить ограниченное количество токенов
static int token_count(const char *text) {
    char *end = NULL;

    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 1 || value > 4096) {
        return 0;
    }

    return (int)value;
}

// Проверить формат и минимальный размер весов до передачи файла в upstream
static int validate_checkpoint(const char *path, int context) {
    FILE *input = fopen(path, "rb");
    Config config;

    if (input == NULL) {
        perror("checkpoint");
        return 0;
    }

    int valid = fread(&config, sizeof(config), 1, input) == 1;
    if (valid && fseek(input, 0, SEEK_END) == 0) {
        long length = ftell(input);
        valid = length >= 633495808L;
    } else {
        valid = 0;
    }

    (void)fclose(input);
    if (!valid) {
        return 0;
    }

    return config.magic_number == 0x616a6331 && config.version == 1
        && config.dim == 1024 && config.hidden_dim == 3072 && config.n_layers == 28
        && config.n_heads == 16 && config.n_kv_heads == 8 && config.vocab_size == 151936
        && config.head_dim == 128 && config.shared_classifier == 1 && config.group_size == 64
        && config.seq_len >= context;
}

// Выполнить одинаковое число forward без зависимости от раннего токена завершения
int main(int argc, char **argv) {
    if (argc != 4) {
        fputs("Usage: synbench-qwen MODEL INPUT_TOKENS DECODE_TOKENS\n", stderr);
        return 2;
    }

    int input_tokens = token_count(argv[2]);
    int decode_tokens = token_count(argv[3]);
    if (input_tokens == 0 || decode_tokens == 0 || input_tokens + decode_tokens > 4096) {
        return 2;
    }

    if (!validate_checkpoint(argv[1], input_tokens + decode_tokens)) {
        fputs("Expected complete Qwen3-0.6B qwen3.c Q8 group-64 checkpoint\n", stderr);
        return 1;
    }

    double start = bench_seconds();
    Transformer transformer;
    build_transformer(&transformer, argv[1], input_tokens + decode_tokens);
    double load_seconds = bench_seconds() - start;

    // Размеры Qwen3-0.6B проверяются отдельно от имени файла модели
    if (transformer.config.dim != 1024 || transformer.config.n_layers != 28) {
        fputs("Expected Qwen3-0.6B dimensions\n", stderr);
        free_transformer(&transformer);
        return 1;
    }

    Sampler sampler;
    build_sampler(&sampler, transformer.config.vocab_size, 0.0f, 1.0f, 42);
    start = bench_seconds();
    float *logits = NULL;

    // Фиксированная последовательность token ID устраняет зависимость от токенизатора
    for (int position = 0; position < input_tokens; position++) {
        logits = forward(&transformer, 1 + position % 1000, position);
    }

    double prefill_seconds = bench_seconds() - start;
    if (logits == NULL) {
        free_sampler(&sampler);
        free_transformer(&transformer);
        return 1;
    }

    int token = sample(&sampler, logits);
    uint64_t checksum = (uint64_t)(unsigned)token;
    double first_token_seconds = load_seconds + prefill_seconds;
    start = bench_seconds();

    for (int index = 0; index < decode_tokens; index++) {
        logits = forward(&transformer, token, input_tokens + index);
        token = sample(&sampler, logits);
        checksum = checksum * UINT64_C(1099511628211) ^ (uint64_t)(unsigned)token;
    }

    double decode_seconds = bench_seconds() - start;
    printf("SYNBENCH_RESULT {\"model_load_seconds\":%.9f,\"prefill_seconds\":%.9f,"
           "\"decode_seconds\":%.9f,\"first_token_with_load_seconds\":%.9f,"
           "\"input_tokens\":%d,\"decode_tokens\":%d,\"prefill_tokens_s\":%.6f,"
           "\"decode_tokens_s\":%.6f,\"token_checksum\":\"%016" PRIx64 "\"}\n",
           load_seconds, prefill_seconds, decode_seconds, first_token_seconds, input_tokens,
           decode_tokens, (double)input_tokens / prefill_seconds, (double)decode_tokens / decode_seconds, checksum);

    free_sampler(&sampler);
    free_transformer(&transformer);
    return 0;
}

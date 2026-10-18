// string.c: Строковые функции и форматирование вывода
// Автор: Елчинян Арен Артурович, 2026
// Подсистема: synlibc, пользовательская библиотека нативного API
// Создан: 09.10.2026
// Последнее изменение: 14.10.2026

#include <synlibc.h>

size_t strlen(const char *str) {
	size_t len = 0;

	while (str[len] != '\0') {
		len++;
	}

	return len;
}

char *strcpy(char *dest, const char *src) {
	char *d = dest;

	while ((*d++ = *src++))
		;
	return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
	size_t i;

	for (i = 0; i < n && src[i] != '\0'; i++) {
		dest[i] = src[i];
	}

	for (; i < n; i++) {
		dest[i] = '\0';
	}

	return dest;
}

int strcmp(const char *s1, const char *s2) {
	while (*s1 && (*s1 == *s2)) {
		s1++;
		s2++;
	}

	return (unsigned char)*s1 - (unsigned char)*s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
	for (size_t i = 0; i < n; i++) {
		if (s1[i] != s2[i]) {
			return (unsigned char)s1[i] - (unsigned char)s2[i];
		} else if (s1[i] == '\0') {
			return 0;
		}
	}

	return 0;
}

char *strcat(char *dest, const char *src) {
	char *d = dest;

	while (*d) {
		d++;
	}
	while ((*d++ = *src++))
		;
	return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
	char *d = dest;

	while (*d) {
		d++;
	}

	size_t i = 0;

	while (i < n && src[i] != '\0') {
		d[i] = src[i];
		i++;
	}

	d[i] = '\0';
	return dest;
}

char *strchr(const char *s, int c) {
	while (*s != (char)c) {
		if (*s == '\0') {
			return NULL;
		}

		s++;
	}

	return (char *)s;
}

char *strrchr(const char *s, int c) {
	const char *last = NULL;

	while (*s) {
		if (*s == (char)c) {
			last = s;
		}

		s++;
	}

	if (c == '\0') {
		return (char *)s;
	}

	return (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
	if (*needle == '\0') {
		return (char *)haystack;
	}

	size_t needle_len = strlen(needle);

	while (*haystack) {
		if (strncmp(haystack, needle, needle_len) == 0) {
			return (char *)haystack;
		}

		haystack++;
	}

	return NULL;
}

void *memmove(void *dest, const void *src, size_t n) {
	unsigned char *d = (unsigned char *)dest;
	const unsigned char *s = (const unsigned char *)src;

	if (d < s) {
		while (n--) {
			*d++ = *s++;
		}
	} else {
		d += n;
		s += n;

		while (n--) {
			*--d = *--s;
		}
	}

	return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
	const unsigned char *p1 = (const unsigned char *)s1;
	const unsigned char *p2 = (const unsigned char *)s2;

	while (n--) {
		if (*p1 != *p2) {
			return *p1 - *p2;
		}

		p1++;
		p2++;
	}

	return 0;
}

int putchar(int c) {
	char ch = (char)c;
	syn_log(&ch, 1);
	return (unsigned char)ch;
}

int puts(const char *s) {
	size_t len = strlen(s);
	syn_log(s, len);
	syn_log("\n", 1);
	return len + 1;
}

typedef struct {
	char *buf;
	size_t size; // Размер буфера, включая нулевой байт
	size_t pos; // Позиция вывода, может превышать размер буфера
} fmt_ctx_t;

static inline void fmt_emit(fmt_ctx_t *ctx, char c) {
	if (ctx->pos < ctx->size - 1) {
		ctx->buf[ctx->pos] = c;
	}

	ctx->pos++;
}

static inline void fmt_emit_str(fmt_ctx_t *ctx, const char *s, size_t len) {
	for (size_t i = 0; i < len; i++) {
		fmt_emit(ctx, s[i]);
	}
}

static void fmt_emit_num(fmt_ctx_t *ctx, uint64_t num, int base, int is_signed, int is_upper) {
	char tmp[64];
	int i = 0;
	uint64_t n = num;
	int neg = 0;

	if (is_signed && (int64_t)n < 0) {
		neg = 1;
		n = (uint64_t)(-(int64_t)n);
	}

	if (n == 0) {
		tmp[i++] = '0';
	} else {
		const char *digits = is_upper ? "0123456789ABCDEF" : "0123456789abcdef";

		while (n > 0) {
			tmp[i++] = digits[n % base];
			n /= base;
		}
	}

	if (neg) {
		fmt_emit(ctx, '-');
	}
	while (i > 0) {
		fmt_emit(ctx, tmp[--i]);
	}
}

// Ограничиваем запись размером буфера, но считаем полную длину вывода без нулевого
// байта
static int vformat_core(fmt_ctx_t *ctx, const char *format, va_list args) {
	for (const char *p = format; *p != '\0'; p++) {
		if (*p != '%') {
			fmt_emit(ctx, *p);
			continue;
		}

		p++;

		if (*p == '\0') {
			break;
		}

		// Разбираем модификатор длины аргумента
		int length = 0; // 0=int, 1=long, 2=long long, 3=size_t, 4=short, 5=char

		if (*p == 'l') {
			length = 1;
			p++;

			if (*p == 'l') {
				length = 2;
				p++;
			}
		} else if (*p == 'z') {
			length = 3;
			p++;
		} else if (*p == 'h') {
			length = 4;
			p++;

			if (*p == 'h') {
				length = 5;
				p++;
			}
		}

		if (*p == '\0') {
			break;
		}

		switch (*p) {
			case 'c': {
				char c = (char)va_arg(args, int);
				fmt_emit(ctx, c);
				break;
			}

			case 's': {
				const char *s = va_arg(args, const char *);

				if (!s) {
					s = "(null)";
				}

				fmt_emit_str(ctx, s, strlen(s));
				break;
			}

			case 'd':
			case 'i': {
				int64_t num;

				switch (length) {
					case 2:
						num = (int64_t)va_arg(args, long long);
						break;
					case 1:
						num = (int64_t)va_arg(args, long);
						break;
					case 3:
						num = (int64_t)va_arg(args, size_t);
						break;
					default:
						num = (int64_t)va_arg(args, int);
						break;
				}

				fmt_emit_num(ctx, (uint64_t)num, 10, 1, 0);
				break;
			}

			case 'u': {
				uint64_t num;

				switch (length) {
					case 2:
						num = va_arg(args, unsigned long long);
						break;
					case 1:
						num = va_arg(args, unsigned long);
						break;
					case 3:
						num = va_arg(args, size_t);
						break;
					default:
						num = va_arg(args, unsigned int);
						break;
				}

				fmt_emit_num(ctx, num, 10, 0, 0);
				break;
			}

			case 'x': {
				uint64_t num;

				switch (length) {
					case 2:
						num = va_arg(args, unsigned long long);
						break;
					case 1:
						num = va_arg(args, unsigned long);
						break;
					case 3:
						num = va_arg(args, size_t);
						break;
					default:
						num = va_arg(args, unsigned int);
						break;
				}

				fmt_emit_num(ctx, num, 16, 0, 0);
				break;
			}

			case 'X': {
				uint64_t num;

				switch (length) {
					case 2:
						num = va_arg(args, unsigned long long);
						break;
					case 1:
						num = va_arg(args, unsigned long);
						break;
					case 3:
						num = va_arg(args, size_t);
						break;
					default:
						num = va_arg(args, unsigned int);
						break;
				}

				fmt_emit_num(ctx, num, 16, 0, 1);
				break;
			}

			case 'o': {
				uint64_t num;

				switch (length) {
					case 2:
						num = va_arg(args, unsigned long long);
						break;
					case 1:
						num = va_arg(args, unsigned long);
						break;
					case 3:
						num = va_arg(args, size_t);
						break;
					default:
						num = va_arg(args, unsigned int);
						break;
				}

				fmt_emit_num(ctx, num, 8, 0, 0);
				break;
			}

			case 'p': {
				uint64_t num = (uint64_t)(uintptr_t)va_arg(args, void *);
				fmt_emit(ctx, '0');
				fmt_emit(ctx, 'x');
				fmt_emit_num(ctx, num, 16, 0, 0);
				break;
			}

			case '%':
				fmt_emit(ctx, '%');
				break;

			default:
				fmt_emit(ctx, '%');
				fmt_emit(ctx, *p);
				break;
		}
	}

	// Завершаем строку нулевым байтом
	if (ctx->buf && ctx->size > 0) {
		size_t term = (ctx->pos < ctx->size) ? ctx->pos : ctx->size - 1;
		ctx->buf[term] = '\0';
	}

	return (int)ctx->pos;
}

int vsnprintf(char *buf, size_t size, const char *format, va_list args) {
	if (!buf || size == 0) {
		// Считаем длину, не записывая в буфер вызывающей стороны
		fmt_ctx_t ctx = { .buf = NULL, .size = 0, .pos = 0 };
		// Однобайтовый буфер принимает только завершающий нулевой байт
		char dummy[1];
		ctx.buf = dummy;
		ctx.size = 1;
		return vformat_core(&ctx, format, args);
	}

	fmt_ctx_t ctx = { .buf = buf, .size = size, .pos = 0 };
	return vformat_core(&ctx, format, args);
}

int vsprintf(char *buf, const char *format, va_list args) {
	// Размер буфера обязан обеспечить вызывающий код
	return vsnprintf(buf, (size_t)-1, format, args);
}

int snprintf(char *buf, size_t size, const char *format, ...) {
	va_list args;
	va_start(args, format);
	int ret = vsnprintf(buf, size, format, args);
	va_end(args);
	return ret;
}

int sprintf(char *buf, const char *format, ...) {
	va_list args;
	va_start(args, format);
	int ret = vsnprintf(buf, (size_t)-1, format, args);
	va_end(args);
	return ret;
}

static void print_num(char *buf, size_t *idx, uint64_t num, int base, int is_signed, int is_upper) {
	char tmp[64];
	int i = 0;
	uint64_t n = num;
	int neg = 0;

	if (is_signed && (int64_t)n < 0) {
		neg = 1;
		n = (uint64_t)(-(int64_t)n);
	}

	if (n == 0) {
		tmp[i++] = '0';
	} else {
		const char *digits = is_upper ? "0123456789ABCDEF" : "0123456789abcdef";

		while (n > 0) {
			tmp[i++] = digits[n % base];
			n /= base;
		}
	}

	if (neg) {
		buf[(*idx)++] = '-';
	}
	while (i > 0) {
		buf[(*idx)++] = tmp[--i];
	}
}

int syn_printf(const char *format, ...) {
	char buf[SYN_LOG_MAX_LEN];
	size_t idx = 0;
	va_list args;
	va_start(args, format);

	for (const char *p = format; *p != '\0'; p++) {
		if (*p != '%') {
			buf[idx++] = *p;

			if (idx >= SYN_LOG_MAX_LEN - 1) {
				syn_log(buf, idx);
				idx = 0;
			}

			continue;
		}

		p++;

		if (*p == '\0') {
			break;
		}

		switch (*p) {
			case 'c': {
				char c = (char)va_arg(args, int);
				buf[idx++] = c;
				break;
			}

			case 's': {
				const char *s = va_arg(args, const char *);

				if (!s) {
					s = "(null)";
				}

				size_t len = strlen(s);

				if (idx + len >= SYN_LOG_MAX_LEN) {
					syn_log(buf, idx);
					idx = 0;
				}

				memcpy(&buf[idx], s, len);
				idx += len;
				break;
			}

			case 'd':
			case 'i': {
				int64_t num = va_arg(args, int64_t);
				print_num(buf, &idx, (uint64_t)num, 10, 1, 0);
				break;
			}

			case 'u': {
				uint64_t num = va_arg(args, uint64_t);
				print_num(buf, &idx, num, 10, 0, 0);
				break;
			}

			case 'x': {
				uint64_t num = va_arg(args, uint64_t);
				print_num(buf, &idx, num, 16, 0, 0);
				break;
			}

			case 'X': {
				uint64_t num = va_arg(args, uint64_t);
				print_num(buf, &idx, num, 16, 0, 1);
				break;
			}

			case 'p': {
				uint64_t num = (uint64_t)va_arg(args, void *);
				buf[idx++] = '0';
				buf[idx++] = 'x';
				print_num(buf, &idx, num, 16, 0, 0);
				break;
			}

			case '%':
				buf[idx++] = '%';
				break;
			default:
				buf[idx++] = '%';
				buf[idx++] = *p;
				break;
		}

		if (idx >= SYN_LOG_MAX_LEN - 64) {
			syn_log(buf, idx);
			idx = 0;
		}
	}

	va_end(args);

	if (idx > 0) {
		syn_log(buf, idx);
	}

	return idx;
}

int atoi(const char *str) {
	return (int)atol(str);
}

long atol(const char *str) {
	long res = 0;
	int sign = 1;

	while (*str == ' ' || *str == '\t' || *str == '\n') {
		str++;
	}

	if (*str == '-') {
		sign = -1;
		str++;
	} else if (*str == '+') {
		str++;
	}

	while (*str >= '0' && *str <= '9') {
		res = res * 10 + (*str - '0');
		str++;
	}

	return res * sign;
}

long long atoll(const char *str) {
	long long res = 0;
	int sign = 1;

	while (*str == ' ' || *str == '\t' || *str == '\n') {
		str++;
	}

	if (*str == '-') {
		sign = -1;
		str++;
	} else if (*str == '+') {
		str++;
	}

	while (*str >= '0' && *str <= '9') {
		res = res * 10 + (*str - '0');
		str++;
	}

	return res * sign;
}
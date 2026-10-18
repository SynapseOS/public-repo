// protocol.h: Формат запросов и ответов обнаружения LAN
// Автор: Елчинян Арен Артурович, 2026
// Проверка длины сообщений, nonce, подсети и печатаемых символов ответа
// Подсистема: пользовательские сетевые утилиты
// Первое изменение: до текущей уборки, исходная дата не установлена
// Последнее изменение: 14.10.2026

#ifndef SYNAPSE_LAN_PROTOCOL_H
#define SYNAPSE_LAN_PROTOCOL_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define LAN_PORT 39421
#define LAN_QUERY_SIZE 24
#define LAN_REPLY_SIZE 104

// Размер сообщения фиксирован, nonce копируется без изменений. Текст соседа требует
// очистки
static int lan_query_valid(const unsigned char *p, size_t n) {
	return n == LAN_QUERY_SIZE && !memcmp(p, "SYNLAN1Q", 8);
}

static int lan_reply_valid(const unsigned char *p, size_t n, const unsigned char *nonce) {
	return n == LAN_REPLY_SIZE && !memcmp(p, "SYNLAN1R", 8) && !memcmp(p + 8, nonce, 16);
}

static int lan_mask_valid(uint32_t m) {
	uint32_t inv = ~m;
	return m && !(inv & (inv + 1));
}

static int lan_onlink(uint32_t address, uint32_t own, uint32_t mask) {
	return own && lan_mask_valid(mask) && (address & mask) == (own & mask) &&
	       address != (own & mask) && address != ((own & mask) | ~mask);
}

static void lan_safe_text(char *dst, const unsigned char *src, size_t n) {
	size_t i = 0;

	for (; i < n && src[i]; i++) {
		dst[i] = (src[i] >= 32 && src[i] <= 126) ? (char)src[i] : '?';
	}

	dst[i] = 0;
}
#endif

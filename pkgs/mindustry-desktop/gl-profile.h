#ifndef SYNAPSE_GL_PROFILE_H
#define SYNAPSE_GL_PROFILE_H
// gl-profile.h: ограниченные показатели вызовов OpenGL в Mindustry
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: JNI и OpenGL
// Подсистема: графический клиент
// Первое изменение: 30.10.2026, последнее изменение: 31.10.2026
// Статус доступа: общедоступный
/* Optional JNI OpenGL wall-time attribution. Enabled by SYNAPSE_GL_PROFILE.
 * Calls execute on the context thread. Nested helper wrappers are not timed.
 * Wall time includes Mesa work and scheduler waits; it is not GPU time. */
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

namespace synapse_profile {
struct Record {
	const char *name;
	unsigned calls;
	unsigned long long ns;
};

static Record records[256];
static unsigned used;
static unsigned long long last;

struct BufferRecord {
	unsigned target;
	unsigned usage;
	unsigned calls;
	unsigned long long bytes;
};

static BufferRecord buffer_records[16];
static unsigned buffer_used;
static unsigned buffer_frames;
static unsigned buffer_calls;
static unsigned buffer_overflow;
static unsigned long long buffer_bytes;
static unsigned long long buffer_last;

// Проверить, запрошены ли показатели загрузки буферов
static bool buffer_enabled( ) {
	static const char *value = getenv("SYNAPSE_GL_BUFFER_PROFILE");
	return value != NULL && strcmp(value, "1") == 0;
}

// Учесть размер и назначение буфера до передачи OpenGL
static void buffer_data(unsigned target, int size, unsigned usage) {
	if (!buffer_enabled( )) {
		return;
	}

	unsigned long long bytes = size > 0 ? (unsigned)size : 0;
	buffer_calls++;
	buffer_bytes += bytes;

	unsigned index = 0;

	while (index < buffer_used) {
		if (buffer_records[index].target == target && buffer_records[index].usage == usage) {
			break;
		}

		index++;
	}

	if (index == buffer_used) {
		if (buffer_used == sizeof(buffer_records) / sizeof(buffer_records[0])) {
			buffer_overflow++;
			return;
		}

		buffer_records[index].target = target;
		buffer_records[index].usage = usage;
		buffer_used++;
	}

	buffer_records[index].calls++;
	buffer_records[index].bytes += bytes;
}

// Записать одну сводку загрузки буферов после смены кадра
void frame_end( ) {
	if (!buffer_enabled( )) {
		return;
	}

	struct timespec clock;
	clock_gettime(CLOCK_MONOTONIC, &clock);
	unsigned long long current = (unsigned long long)clock.tv_sec * 1000000000ULL + clock.tv_nsec;
	buffer_frames++;

	if (buffer_last == 0) {
		buffer_last = current;
	}

	if (current - buffer_last < 1000000000ULL) {
		return;
	}

	fprintf(stderr,
	        "SYNAPSE_GL_BUFFER_METRIC frames=%u calls=%u bytes=%llu calls_per_frame=%.2f "
	        "bytes_per_frame=%.1f overflow=%u",
	        buffer_frames, buffer_calls, buffer_bytes,
	        (double)buffer_calls / buffer_frames, (double)buffer_bytes / buffer_frames,
	        buffer_overflow);

	for (unsigned index = 0; index < buffer_used; index++) {
		fprintf(stderr, " target=0x%x/usage=0x%x:%u:%llu", buffer_records[index].target,
		        buffer_records[index].usage, buffer_records[index].calls,
		        buffer_records[index].bytes);
	}

	fputc('\n', stderr);
	fflush(stderr);
	memset(buffer_records, 0, sizeof(buffer_records));
	buffer_used = 0;
	buffer_frames = 0;
	buffer_calls = 0;
	buffer_overflow = 0;
	buffer_bytes = 0;
	buffer_last = current;
}

static bool enabled( ) {
	static bool yes = getenv("SYNAPSE_GL_PROFILE") != NULL;
	return yes;
}

static unsigned long long now( ) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (unsigned long long)t.tv_sec * 1000000000ULL + t.tv_nsec;
}

struct Scope {
	const char *name;
	unsigned long long start;

	explicit Scope(const char *n) : name(n), start(enabled( ) ? now( ) : 0) {}

	~Scope( ) {
		if (!start)
			return;
		unsigned i = 0;

		while (i < used && strcmp(records[i].name, name))
			i++;
		if (i == used) {
			if (used == 256)
				return;
			records[used++].name = name;
		}

		records[i].calls++;
		records[i].ns += now( ) - start;
		unsigned long long end = now( );

		if (!last)
			last = end;
		if (end - last >= 1000000000ULL) {
			fprintf(stderr, "SYNAPSE_GL_CALL_METRIC elapsed_ms=%.2f", (end - last) / 1e6);

			for (unsigned j = 0; j < used; j++) {
				if (records[j].calls)
					fprintf(stderr, " %s=%u:%.3f", records[j].name, records[j].calls,
					        records[j].ns / 1e6);
				records[j].calls = 0;
				records[j].ns = 0;
			}

			fputc('\n', stderr);
			last = end;
		}
	}
};
} // namespace synapse_profile
#endif

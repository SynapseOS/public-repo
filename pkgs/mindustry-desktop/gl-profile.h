#ifndef SYNAPSE_GL_PROFILE_H
#define SYNAPSE_GL_PROFILE_H
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

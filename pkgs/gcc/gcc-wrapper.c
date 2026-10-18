#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
	static const char *defaults[] = { "/usr/libexec/gcc/x86_64-linux-musl/14.3.0/gcc",
		                              "-B/usr/libexec/gcc/x86_64-linux-musl/14.3.0/",
		                              "-B/usr/lib/gcc/x86_64-linux-musl/14.3.0/",
		                              "-B/usr/lib/",
		                              "-L/usr/lib/gcc/x86_64-linux-musl/14.3.0",
		                              "-L/usr/lib",
		                              "-specs=/usr/lib/gcc/x86_64-linux-musl/14.3.0/specs",
		                              "-static",
		                              "-no-pie",
		                              "-fno-pie",
		                              "-fno-stack-protector",
		                              "-mno-red-zone" };
	size_t count = sizeof defaults / sizeof *defaults;
	char **args = calloc(count + (size_t)argc, sizeof *args);

	if (!args) {
		perror("gcc");
		return 1;
	}

	for (size_t i = 0; i < count; ++i)
		args[i] = (char *)defaults[i];
	for (int i = 1; i < argc; ++i)
		args[count + (size_t)i - 1] = argv[i];
	execv(args[0], args);
	fprintf(stderr, "gcc: %s: %s\n", args[0], strerror(errno));
	free(args);
	return 1;
}

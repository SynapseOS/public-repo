/* Real pip CLI entrypoint for an ELF-only guest exec loader. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
	char **args = calloc((size_t)argc + 3, sizeof(*args));

	if (!args) {
		perror("pip");
		return 1;
	}

	args[0] = "python3";
	args[1] = "-m";
	args[2] = "pip";

	for (int i = 1; i < argc; i++)
		args[i + 2] = argv[i];
	execv("/bin/python3", args);
	perror("pip: python3");
	free(args);
	return 1;
}

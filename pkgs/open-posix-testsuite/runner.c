/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static double monotonic(void) {
	struct timespec t;

	if (clock_gettime(CLOCK_MONOTONIC, &t))
		return 0;
	return t.tv_sec + t.tv_nsec / 1e9;
}

static const char *classify(int status) {
	if (WIFSIGNALED(status))
		return "SIGNALED";
	if (!WIFEXITED(status))
		return "ABNORMAL";
	switch (WEXITSTATUS(status)) {
		case 0:
			return "PASS";
		case 1:
			return "FAIL";
		case 2:
			return "UNRESOLVED";
		case 4:
			return "UNSUPPORTED";
		case 5:
			return "UNTESTED";
		case 126:
		case 127:
			return "EXEC_ERROR";
		default:
			return "ABNORMAL";
	}
}

int main(int argc, char **argv) {
	const char *root = "/usr/libexec/open-posix-testsuite", *out = "/tmp/open-posix-results",
	           *filter = "";
	unsigned timeout = 300, start = 0, limit = UINT_MAX;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--help")) {
			puts(
			    "Usage: open-posix-testsuite [--root DIR] [--out DIR] [--timeout SECONDS]\n       [--filter PREFIX] [--start INDEX] [--limit COUNT]\nDefault per-test timeout: 300 seconds (upstream default). Results: results.tsv and per-test logs.");
			return 0;
		}

		if (i + 1 >= argc) {
			fprintf(stderr, "missing argument: %s\n", argv[i]);
			return 2;
		}

		const char *key = argv[i++], *value = argv[i];

		if (!strcmp(key, "--root"))
			root = value;
		else if (!strcmp(key, "--out"))
			out = value;
		else if (!strcmp(key, "--filter"))
			filter = value;
		else {
			char *end;
			unsigned long n = strtoul(value, &end, 10);

			if (*end || n > UINT_MAX) {
				fprintf(stderr, "bad value: %s\n", value);
				return 2;
			}

			if (!strcmp(key, "--timeout") && n)
				timeout = n;
			else if (!strcmp(key, "--start"))
				start = n;
			else if (!strcmp(key, "--limit") && n)
				limit = n;
			else {
				fprintf(stderr, "bad option: %s\n", key);
				return 2;
			}
		}
	}

	char absolute_root[PATH_MAX], absolute_out[PATH_MAX], cwd_root[PATH_MAX];

	if (!getcwd(cwd_root, sizeof cwd_root)) {
		perror("getcwd");
		return 2;
	}

	if (root[0] != '/') {
		if (snprintf(absolute_root, sizeof absolute_root, "%s/%s", cwd_root, root) >=
		    (int)sizeof absolute_root)
			return 2;
		root = absolute_root;
	}

	if (out[0] != '/') {
		if (snprintf(absolute_out, sizeof absolute_out, "%s/%s", cwd_root, out) >=
		    (int)sizeof absolute_out)
			return 2;
		out = absolute_out;
	}

	char manifest[PATH_MAX], resultfile[PATH_MAX];

	if (snprintf(manifest, sizeof manifest, "%s/tests.tsv", root) >= (int)sizeof manifest ||
	    snprintf(resultfile, sizeof resultfile, "%s/results.tsv", out) >= (int)sizeof resultfile)
		return 2;
	FILE *inventory = fopen(manifest, "r");

	if (!inventory) {
		perror(manifest);
		return 2;
	}

	if (mkdir(out, 0755) && errno != EEXIST) {
		perror(out);
		fclose(inventory);
		return 2;
	}

	FILE *results = fopen(resultfile, "w");

	if (!results) {
		perror(resultfile);
		fclose(inventory);
		return 2;
	}

	setvbuf(results, NULL, _IOLBF, 0);
	setvbuf(stdout, NULL, _IOLBF, 0);
	fputs("index\ttest\tkind\tresult\texit\tsignal\tseconds\tlog\n", results);
	unsigned index = 0, total = 0, pass = 0, nonpass = 0, compiled = 0, buildfail = 0;
	char line[8192];

	while (fgets(line, sizeof line, inventory)) {
		unsigned current = index++;

		if (current < start)
			continue;
		char *id = strtok(line, "\t"), *kind = strtok(NULL, "\t"), *build = strtok(NULL, "\t"),
		     *relative = strtok(NULL, "\t\n");
		if (!id || !kind || !build || !relative) {
			fprintf(stderr, "bad inventory row %u\n", current);
			return 2;
		}

		if (strncmp(id, filter, strlen(filter)))
			continue;
		if (total >= limit)
			break;
		++total;
		char executable[PATH_MAX], log[PATH_MAX];

		if (snprintf(executable, sizeof executable, "%s/%s", root, relative) >=
		        (int)sizeof executable ||
		    snprintf(log, sizeof log, "%s/%06u.log", out, current) >= (int)sizeof log)
			return 2;
		const char *label = NULL;
		int status = 0;
		double elapsed = 0;

		if (strcmp(build, "OK")) {
			label = "BUILD_FAIL";
			++buildfail;
		} else if (!strcmp(kind, "BUILD")) {
			label = "BUILD_PASS";
			++compiled;
		} else {
			printf("BEGIN %u %s\n", current, id);
			double began = monotonic( );
			pid_t pid = fork( );

			if (pid < 0) {
				perror("fork");
				label = "HARNESS_ERROR";
			} else if (!pid) {
				if (setpgid(0, 0)) {
					perror("setpgid");
					_exit(126);
				}

				int fd = open(log, O_CREAT | O_TRUNC | O_WRONLY, 0644);

				if (fd < 0)
					_exit(126);
				if (dup2(fd, 1) < 0 || dup2(fd, 2) < 0)
					_exit(126);
				if (fd > 2)
					close(fd);
				/* Bound resources for conformance cases that intentionally exhaust memory.
     * Apply the identical limits in Linux and Synapse; report them externally. */
				struct rlimit core = { 0, 0 }, as = { 512UL * 1024 * 1024, 512UL * 1024 * 1024 };

				if (setrlimit(RLIMIT_CORE, &core) || setrlimit(RLIMIT_AS, &as)) {
					perror("setrlimit");
					_exit(126);
				}

				int in = open("/dev/null", O_RDONLY);

				if (in >= 0) {
					dup2(in, 0);

					if (in > 2)
						close(in);
				}

				char cwd[PATH_MAX];
				strcpy(cwd, executable);
				char *slash = strrchr(cwd, '/');

				if (slash)
					*slash = 0;
				if (chdir(cwd)) {
					perror("chdir");
					_exit(126);
				}

				setenv("LC_ALL", "C", 1);
				setenv("TMPDIR", "/tmp", 1);

				if (!strcmp(kind, "SHELL"))
					execl("/bin/bash", "bash", executable, (char *)NULL);
				else
					execl(executable, executable, (char *)NULL);
				perror("exec");
				_exit(127);
			} else {
				setpgid(pid, pid);

				for (;;) {
					pid_t r = waitpid(pid, &status, WNOHANG);

					if (r == pid)
						break;
					if (r < 0 && errno != EINTR) {
						label = "HARNESS_ERROR";
						break;
					}

					if (monotonic( ) - began >= timeout) {
						label = "TIMEOUT";
						kill(-pid, SIGKILL);
						kill(pid, SIGKILL);

						while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
						}

						break;
					}

					struct timespec nap = { 0, 10000000 };
					nanosleep(&nap, NULL);
				}
				/* Stop descendants left by a case, including after its parent exits. */
				kill(-pid, SIGKILL);

				if (!label)
					label = classify(status);
			}

			elapsed = monotonic( ) - began;

			if (!strcmp(label, "PASS"))
				++pass;
			else
				++nonpass;
		}

		fprintf(results, "%u\t%s\t%s\t%s\t%d\t%d\t%.3f\t%06u.log\n", current, id, kind, label,
		        WIFEXITED(status) ? WEXITSTATUS(status) : -1,
		        WIFSIGNALED(status) ? WTERMSIG(status) : 0, elapsed, current);
		printf("RESULT %u %s %s %.3fs\n", current, label, id, elapsed);
	}

	printf(
	    "OPEN_POSIX_RESULT total=%u runtime_pass=%u runtime_nonpass=%u build_pass=%u build_fail=%u\n",
	    total, pass, nonpass, compiled, buildfail);
	fclose(results);
	fclose(inventory);
	return nonpass || buildfail ? 1 : 0;
}

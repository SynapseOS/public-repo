#define _GNU_SOURCE
#include <synapse/syn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>

#define CHECK(x)                                                                                   \
	do {                                                                                           \
		if (!(x)) {                                                                                \
			fprintf(stderr, "SYNBENCH_FAIL line=%d check=%s errno=%d\n", __LINE__, #x, errno);     \
			return 1;                                                                              \
		}                                                                                          \
	} while (0)
#ifdef SYNBENCH_TRACE
#define TRACE(message)                                                                             \
	do {                                                                                           \
		static const char text[] = "synbench: " message "\n";                                      \
		syscall(100001, sizeof(text) - 1, text);                                                   \
	} while (0)
#else
#define TRACE(message) ((void)0)
#endif
static uint64_t next_id = 100;

static uint64_t wall_us(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static int finish(struct syn_orbit *o, struct syn_request q, struct syn_completion *out) {
	q.id = ++next_id;
	return syn_orbit_call(o, &q, out);
}

static int selftest(void) {
	TRACE("start");
	struct syn_orbit o, bad;
	CHECK(syn_orbit_create(&bad, 1) == -SYN_EINVAL);
	CHECK(syn_orbit_create(&bad, 3) == -SYN_EINVAL);
	CHECK(syn_orbit_create(&o, 32) == 0);
	TRACE("context created");
	struct syn_buffer buffer;
	CHECK(syn_buffer_create(&o, 4096, &buffer) == 0);
	CHECK(buffer.bytes == 4096);
	TRACE("buffer created");
	char path[128];
	snprintf(path, sizeof(path), "/tmp/synbench-%ld", (long)getpid( ));
	int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
	CHECK(fd >= 0);
	CHECK(write(fd, "old value", 9) == 9);
	CHECK(lseek(fd, 3, SEEK_SET) == 3);
	syn_cap_t file, ro;
	CHECK(syn_cap_import(&o, fd, SYN_RIGHT_READ | SYN_RIGHT_WRITE | SYN_RIGHT_STAT | SYN_RIGHT_MAP,
	                     &file) == 0);
	CHECK(syn_cap_restrict(&o, file, SYN_RIGHT_READ | SYN_RIGHT_STAT, &ro) == 0);
	syn_cap_t impossible;
	CHECK(syn_cap_restrict(&o, ro, SYN_RIGHT_WRITE, &impossible) == -SYN_EACCES);
	TRACE("capabilities imported");
	strcpy(buffer.data, "Orbit data");
	struct syn_completion out;
	CHECK(finish(&o,
	             (struct syn_request){ .opcode = SYN_OP_WRITE,
	                                   .object = file,
	                                   .buffer = buffer.cap,
	                                   .length = 10,
	                                   .offset = 16 },
	             &out) == 0 &&
	      out.result == 10);
	CHECK(out.flags == (SYN_CQE_TERMINAL | SYN_CQE_BUFFER_RELEASED));
	TRACE("write completed");
	CHECK(lseek(fd, 0, SEEK_CUR) == 3);
	memset(buffer.data, 0, buffer.bytes);
	CHECK(finish(&o,
	             (struct syn_request){ .opcode = SYN_OP_READ,
	                                   .object = ro,
	                                   .buffer = buffer.cap,
	                                   .length = 10,
	                                   .offset = 16 },
	             &out) == 0 &&
	      out.result == 10);
	CHECK(memcmp(buffer.data, "Orbit data", 10) == 0);
	CHECK(finish(&o,
	             (struct syn_request){
	                 .opcode = SYN_OP_WRITE, .object = ro, .buffer = buffer.cap, .length = 1 },
	             &out) == -SYN_EACCES);
	CHECK(finish(&o,
	             (struct syn_request){ .opcode = SYN_OP_READ,
	                                   .object = file,
	                                   .buffer = buffer.cap,
	                                   .buffer_offset = 4095,
	                                   .length = 2 },
	             &out) == -SYN_EINVAL);
	CHECK(finish(&o,
	             (struct syn_request){ .opcode = SYN_OP_READ,
	                                   .object = file,
	                                   .buffer = buffer.cap,
	                                   .length = 1,
	                                   .offset = UINT64_MAX },
	             &out) == -SYN_EINVAL);
	CHECK(finish(&o, (struct syn_request){ .opcode = 999 }, &out) == -SYN_EINVAL);
	CHECK(finish(&o, (struct syn_request){ .opcode = SYN_OP_NOP, .flags = 1 }, &out) ==
	      -SYN_EINVAL);
	TRACE("validation complete");
	CHECK(close(fd) == 0); /* accepted capability survives source fd close */
	CHECK(finish(&o, (struct syn_request){ .opcode = SYN_OP_STAT, .object = file }, &out) == 0 &&
	      out.result == 26);
	CHECK(syn_cap_drop(&o, ro) == 0);
	CHECK(finish(&o, (struct syn_request){ .opcode = SYN_OP_STAT, .object = ro }, &out) ==
	      -SYN_EBADF);
	CHECK(syn_cap_restrict(&o, file, SYN_RIGHT_STAT, &impossible) == 0 && impossible != ro);
	CHECK(syn_cap_drop(&o, impossible) == 0);
	void *mapping = syn_memory_map(&o, file, 4096, 0, SYN_PROT_READ, SYN_MAP_PRIVATE);
	CHECK((intptr_t)mapping > 0 && memcmp((char *)mapping + 16, "Orbit data", 10) == 0);
	CHECK(syn_memory_unmap(mapping, 4096) == 0);
	mapping = syn_memory_map(&o, 0, 4096, 0, SYN_PROT_READ | SYN_PROT_WRITE, SYN_MAP_PRIVATE);
	CHECK((intptr_t)mapping > 0);
	strcpy(mapping, "anonymous");
	CHECK(syn_memory_unmap(mapping, 4096) == 0);
	CHECK(syn_memory_unmap((void *)1, 4096) == -SYN_EINVAL);
	CHECK(syn_cap_drop(&o, file) == 0);
	CHECK(syn_open(&o, path, O_RDONLY, 0, SYN_RIGHT_READ | SYN_RIGHT_STAT, &ro) == 0);
	CHECK(finish(&o, (struct syn_request){ .opcode = SYN_OP_STAT, .object = ro }, &out) == 0 &&
	      out.result == 26);
	CHECK(syn_cap_drop(&o, ro) == 0);
	unlink(path);

	TRACE("files and mapping complete");
	syn_cap_t channel, scope;
	int channel_fd;
	CHECK(syn_channel_create(&o, &channel, &channel_fd) == 0);
	CHECK(syn_scope_create(&o, &scope) == 0);
	struct syn_request request = { .id = ++next_id,
		                           .opcode = SYN_OP_READ,
		                           .object = channel,
		                           .buffer = buffer.cap,
		                           .length = 8,
		                           .scope = scope };
	CHECK(syn_orbit_submit(&o, &request) == 0 && syn_orbit_enter(&o, 0, 0) >= 0);
	CHECK(syn_buffer_destroy(&o, &buffer) == -SYN_EBUSY);
	TRACE("cancel scope");
	CHECK(syn_scope_cancel(&o, scope) == 0);
	CHECK(syn_orbit_enter(&o, 1, syn_clock_us( ) + 1000000) >= 1);
	CHECK(syn_orbit_next(&o, &out) == 1 && out.id == request.id && out.status == SYN_ECANCELED);
	CHECK(finish(&o, (struct syn_request){ .opcode = SYN_OP_NOP, .scope = scope }, &out) ==
	      -SYN_ECANCELED);
	CHECK(syn_cap_drop(&o, scope) == 0);
	TRACE("scope tests complete");
	CHECK(finish(&o,
	             (struct syn_request){ .opcode = SYN_OP_READ,
	                                   .object = channel,
	                                   .buffer = buffer.cap,
	                                   .length = 8,
	                                   .deadline = syn_clock_us( ) + 3000 },
	             &out) == -SYN_ETIMEDOUT);
	TRACE("deadline complete");
	request = (struct syn_request){ .id = ++next_id,
		                            .opcode = SYN_OP_TIMER,
		                            .offset = syn_clock_us( ) + 1000000 };
	CHECK(syn_orbit_submit(&o, &request) == 0 && syn_orbit_enter(&o, 0, 0) >= 0);
	CHECK(syn_request_cancel(&o, request.id) == 0);
	CHECK(syn_orbit_enter(&o, 1, syn_clock_us( ) + 1000000) >= 1);
	CHECK(syn_orbit_next(&o, &out) == 1 && out.status == SYN_ECANCELED);
	CHECK(syn_request_cancel(&o, request.id) == -SYN_ENOENT);
	TRACE("cancel id complete");
	request = (struct syn_request){ .id = ++next_id,
		                            .opcode = SYN_OP_TIMER,
		                            .offset = syn_clock_us( ) + 3000 };
	CHECK(syn_orbit_submit(&o, &request) == 0 && syn_orbit_enter(&o, 0, 0) >= 0);
	usleep(30000); /* completion must appear without another ENTER */
	CHECK(syn_orbit_next(&o, &out) == 1 && out.id == request.id && out.status == 0);

	/* Separate processes share a channel fd, never the parent's Orbit context. */
	TRACE("timers complete");
	pid_t child = fork( );
	CHECK(child >= 0);

	if (!child) {
		if (syn_orbit_enter(&o, 0, 0) != -SYN_EBADF)
			_exit(20);
		struct syn_orbit own;
		struct syn_buffer b;
		syn_cap_t cap;

		if (syn_orbit_create(&own, 8) || syn_buffer_create(&own, 4096, &b) ||
		    syn_cap_import(&own, channel_fd, SYN_RIGHT_WRITE, &cap))
			_exit(21);
		memcpy(b.data, "ipc-pass", 8);
		struct syn_request q = {
			.opcode = SYN_OP_WRITE, .object = cap, .buffer = b.cap, .length = 8
		};
		struct syn_completion completion;

		if (syn_orbit_call(&own, &q, &completion) || completion.result != 8)
			_exit(22);
		syn_buffer_destroy(&own, &b);
		syn_cap_drop(&own, cap);
		syn_orbit_destroy(&own);
		_exit(7);
	}

	syn_cap_t process;
	CHECK(syn_process_import(&o, child, &process) == 0);
	CHECK(finish(&o,
	             (struct syn_request){ .opcode = SYN_OP_READ,
	                                   .object = channel,
	                                   .buffer = buffer.cap,
	                                   .length = 8,
	                                   .deadline = syn_clock_us( ) + 2000000 },
	             &out) == 0 &&
	      out.result == 8);
	CHECK(memcmp(buffer.data, "ipc-pass", 8) == 0);
	CHECK(finish(&o,
	             (struct syn_request){ .opcode = SYN_OP_PROCESS_WAIT,
	                                   .object = process,
	                                   .deadline = syn_clock_us( ) + 2000000 },
	             &out) == 0);
	int child_status;
	CHECK(waitpid(child, &child_status, 0) == child && WIFEXITED(child_status) &&
	      WEXITSTATUS(child_status) == 7);
	CHECK(syn_cap_drop(&o, process) == 0);
	CHECK(syn_cap_drop(&o, channel) == 0 && syn_close(channel_fd) == 0);

	TRACE("IPC process complete");
	/* Duplicate IDs must not overwrite an existing pending operation. */
	request = (struct syn_request){ .id = ++next_id, .opcode = SYN_OP_NOP };
	CHECK(syn_orbit_submit(&o, &request) == 0 && syn_orbit_submit(&o, &request) == 0);
	CHECK(syn_orbit_enter(&o, 2, 0) == 2);
	int valid_ids = 0, duplicate_ids = 0;

	for (int i = 0; i < 2; i++) {
		CHECK(syn_orbit_next(&o, &out) == 1 && out.id == request.id);
		valid_ids += out.status == 0;
		duplicate_ids += out.status == SYN_EEXIST;
	}

	CHECK(valid_ids == 1 && duplicate_ids == 1);

	/* Reserve CQ credits before acceptance: no overwrites when users stall. */
	for (unsigned i = 0; i < 32; i++) {
		request = (struct syn_request){ .id = ++next_id, .opcode = SYN_OP_NOP };
		CHECK(syn_orbit_submit(&o, &request) == 0);
	}

	CHECK(syn_orbit_enter(&o, 32, 0) == 32);
	uint32_t head = __atomic_load_n(&o.ring->sq_head, __ATOMIC_ACQUIRE);

	for (unsigned i = 0; i < 32; i++) {
		request = (struct syn_request){ .id = ++next_id, .opcode = SYN_OP_NOP };
		CHECK(syn_orbit_submit(&o, &request) == 0);
	}

	CHECK(syn_orbit_enter(&o, 0, 0) == 32 &&
	      __atomic_load_n(&o.ring->sq_head, __ATOMIC_ACQUIRE) == head);
	TRACE("teardown loops");

	for (unsigned i = 0; i < 32; i++)
		CHECK(syn_orbit_next(&o, &out) == 1 && out.status == 0);
	CHECK(syn_orbit_next(&o, &out) == 0 && syn_orbit_enter(&o, 32, 0) == 32);

	for (unsigned i = 0; i < 32; i++)
		CHECK(syn_orbit_next(&o, &out) == 1 && out.status == 0);
	uint32_t cq_head = o.ring->cq_head;
	__atomic_store_n(&o.ring->cq_head, cq_head + 1, __ATOMIC_RELEASE);
	CHECK(syn_orbit_enter(&o, 0, 0) == -SYN_EINVAL);
	__atomic_store_n(&o.ring->cq_head, cq_head, __ATOMIC_RELEASE);
	uint32_t sq_tail = o.ring->sq_tail;
	__atomic_store_n(&o.ring->sq_tail, sq_tail + 33, __ATOMIC_RELEASE);
	CHECK(syn_orbit_enter(&o, 0, 0) == -SYN_EINVAL);
	__atomic_store_n(&o.ring->sq_tail, sq_tail, __ATOMIC_RELEASE);
	CHECK(syn_orbit_enter(&o, 0, 0) == 0);
	TRACE("backpressure complete");
	/* Remapping/unmapping userspace does not release a backend's buffer lease. */
	CHECK(syn_channel_create(&o, &channel, &channel_fd) == 0);
	request = (struct syn_request){
		.id = ++next_id, .opcode = SYN_OP_READ, .object = channel, .buffer = buffer.cap, .length = 1
	};
	CHECK(syn_orbit_submit(&o, &request) == 0 && syn_orbit_enter(&o, 0, 0) >= 0);
	CHECK(syn_memory_unmap(buffer.data, buffer.bytes) == 0);
	CHECK(syn_request_cancel(&o, request.id) == 0 && syn_orbit_enter(&o, 1, 0) >= 1);
	CHECK(syn_orbit_next(&o, &out) == 1 && out.status == SYN_ECANCELED);
	CHECK(syn_cap_drop(&o, buffer.cap) == 0);
	CHECK(syn_cap_drop(&o, channel) == 0 && syn_close(channel_fd) == 0);
	struct syn_orbit_stats stats;
	CHECK(syn_orbit_stats(&o, &stats) == 0);
	CHECK(stats.accepted == stats.completed && !stats.pending && stats.cq_backpressure);
	CHECK(syn_orbit_destroy(&o) == 0);

	for (unsigned i = 0; i < 32; i++)
		CHECK(syn_orbit_create(&o, 2) == 0 && syn_orbit_destroy(&o) == 0);
	puts(
	    "SYNBENCH_CHECK_PASS capabilities rights generations leases cancellation deadlines timers IPC processes CQ backpressure malformed rings mappings teardown");
	return 0;
}

static int network_case(int type) {
	int sockets[2];
	struct sockaddr_in address[2];
	socklen_t size = sizeof(address[0]);

	for (int i = 0; i < 2; i++) {
		sockets[i] = socket(AF_INET, type, 0);
		CHECK(sockets[i] >= 0);
		address[i] = (struct sockaddr_in){ .sin_family = AF_INET,
			                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
		CHECK(bind(sockets[i], (void *)&address[i], sizeof(address[i])) == 0);
		CHECK(getsockname(sockets[i], (void *)&address[i], &size) == 0);
	}

	if (type == SOCK_STREAM)
		CHECK(listen(sockets[1], 4) == 0);
	CHECK(connect(sockets[0], (void *)&address[1], sizeof(address[1])) == 0);

	if (type == SOCK_DGRAM)
		CHECK(connect(sockets[1], (void *)&address[0], sizeof(address[0])) == 0);
	else {
		int accepted = accept(sockets[1], NULL, NULL);
		CHECK(accepted >= 0);
		CHECK(close(sockets[1]) == 0);
		sockets[1] = accepted;
	}

	struct syn_orbit o;
	struct syn_buffer b;
	syn_cap_t sendcap, recvcap;
	CHECK(syn_orbit_create(&o, 8) == 0 && syn_buffer_create(&o, 4096, &b) == 0);
	CHECK(syn_cap_import(&o, sockets[0], SYN_RIGHT_WRITE, &sendcap) == 0);
	CHECK(syn_cap_import(&o, sockets[1], SYN_RIGHT_READ | SYN_RIGHT_WAIT, &recvcap) == 0);
	memcpy(b.data, "orbit-udp", 9);
	struct syn_request recv = { .id = ++next_id,
		                        .opcode = SYN_OP_READ,
		                        .object = recvcap,
		                        .buffer = b.cap,
		                        .buffer_offset = 64,
		                        .length = 64,
		                        .deadline = syn_clock_us( ) + 1000000 };
	struct syn_request send = {
		.id = ++next_id, .opcode = SYN_OP_WRITE, .object = sendcap, .buffer = b.cap, .length = 9
	};
	CHECK(syn_orbit_submit(&o, &recv) == 0 && syn_orbit_submit(&o, &send) == 0 &&
	      syn_orbit_enter(&o, 2, syn_clock_us( ) + 1000000) >= 2);
	struct syn_completion out;
	int sent = 0, received = 0;

	while (syn_orbit_next(&o, &out) == 1) {
		CHECK(!out.status && out.result == 9);
		sent += out.id == send.id;
		received += out.id == recv.id;
	}

	CHECK(sent == 1 && received == 1 && memcmp((char *)b.data + 64, "orbit-udp", 9) == 0);
	CHECK(syn_buffer_destroy(&o, &b) == 0 && syn_cap_drop(&o, sendcap) == 0 &&
	      syn_cap_drop(&o, recvcap) == 0 && syn_orbit_destroy(&o) == 0);
	close(sockets[0]);
	close(sockets[1]);
	return 0;
}

static int network_test(void) {
	CHECK(network_case(SOCK_DGRAM) == 0);
	CHECK(network_case(SOCK_STREAM) == 0);
	puts("SYNBENCH_NETWORK_PASS UDP TCP native buffers async receive/send");
	return 0;
}

static int compare(const void *a, const void *b) {
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

static int benchmark(unsigned iterations) {
	const unsigned batches[] = { 1, 8, 32, 128 };
	const size_t lengths[] = { 64, 4096, 65536 };
	char path[128];
	snprintf(path, sizeof(path), "/tmp/synbench-bench-%ld", (long)getpid( ));
	int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
	CHECK(fd >= 0);
	unsigned char *data = malloc(65536);
	CHECK(data);
	memset(data, 0x5a, 65536);
	CHECK(write(fd, data, 65536) == 65536);
	struct syn_orbit o;
	struct syn_buffer b;
	syn_cap_t file;
	CHECK(syn_orbit_create(&o, 128) == 0 && syn_buffer_create(&o, 128 * 65536, &b) == 0);
	CHECK(syn_cap_import(&o, fd, SYN_RIGHT_READ | SYN_RIGHT_WRITE, &file) == 0);
	memset(b.data, 0x5a, b.bytes);

	for (unsigned operation = 0; operation < 3; operation++)
		for (unsigned l = 0; l < (operation ? 3u : 1u); l++) {
			size_t length = operation ? lengths[l] : 0;

			for (unsigned bi = 0; bi < 4; bi++) {
				unsigned batch = batches[bi], count = (iterations / batch) * batch;

				if (!count)
					count = batch;
				for (unsigned api = 0; api < 2; api++) {
					double samples[5], rates[5];
					uint64_t enter_calls = 0;

					for (unsigned repeat = 0; repeat < 5; repeat++) {
						struct syn_orbit_stats before, after;
						CHECK(syn_orbit_stats(&o, &before) == 0);
						uint64_t start = wall_us( );

						for (unsigned i = 0; i < count; i += batch) {
							if (!api)
								for (unsigned j = 0; j < batch; j++) {
									if (!operation) {
										struct timespec time;
										CHECK(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
									} else if (operation == 1)
										CHECK(pread(fd, (char *)b.data + j * 65536, length, 0) ==
										      (ssize_t)length);
									else
										CHECK(pwrite(fd, (char *)b.data + j * 65536, length, 0) ==
										      (ssize_t)length);
								}
							else {
								for (unsigned j = 0; j < batch; j++) {
									struct syn_request q = { .id = ++next_id,
										                     .opcode = operation == 0 ? SYN_OP_CLOCK
										                               : operation == 1
										                                   ? SYN_OP_READ
										                                   : SYN_OP_WRITE,
										                     .object = file,
										                     .buffer = b.cap,
										                     .buffer_offset = j * 65536,
										                     .length = length };
									CHECK(syn_orbit_submit(&o, &q) == 0);
								}

								CHECK(syn_orbit_enter(&o, batch, 0) >= (int)batch);

								for (unsigned j = 0; j < batch; j++) {
									struct syn_completion out;
									CHECK(syn_orbit_next(&o, &out) == 1 && !out.status &&
									      (!operation || out.result == (int64_t)length));
								}
							}
						}

						uint64_t elapsed = wall_us( ) - start;
						CHECK(elapsed);
						CHECK(syn_orbit_stats(&o, &after) == 0);
						enter_calls = after.enter_calls - before.enter_calls;
						samples[repeat] = (double)elapsed * 1000 / count;
						rates[repeat] = (double)count * 1000000 / elapsed;
					}

					qsort(samples, 5, sizeof(double), compare);
					qsort(rates, 5, sizeof(double), compare);
					printf(
					    "SYNBENCH_RESULT {\"api\":\"%s\",\"operation\":\"%s\",\"bytes\":%zu,\"batch\":%u,\"operations\":%u,\"repeats\":5,\"median_ns_per_op\":%.2f,\"median_ops_per_sec\":%.2f,\"orbit_enter_calls\":%llu}\n",
					    api ? "orbit" : "posix",
					    operation == 0   ? "clock"
					    : operation == 1 ? "pread"
					                     : "pwrite",
					    length, batch, count, samples[2], rates[2],
					    (unsigned long long)enter_calls);
					fflush(stdout);
				}
			}
		}

	CHECK(syn_buffer_destroy(&o, &b) == 0 && syn_cap_drop(&o, file) == 0 &&
	      syn_orbit_destroy(&o) == 0);
	free(data);
	close(fd);
	unlink(path);
	return 0;
}

int main(int argc, char **argv) {
	int checks = 1, bench = 1;
	unsigned iterations = 8192;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--check"))
			bench = 0;
		else if (!strcmp(argv[i], "--bench"))
			checks = 0;
		else if (!strcmp(argv[i], "--iterations") && i + 1 < argc) {
			char *end;
			unsigned long value = strtoul(argv[++i], &end, 10);

			if (*end || !value || value > 10000000)
				return 2;
			iterations = value;
		} else {
			fprintf(stderr, "usage: synbench [--check|--bench] [--iterations N]\n");
			return 2;
		}
	}

	if (checks && (selftest( ) || network_test( )))
		return 1;
	if (bench && benchmark(iterations))
		return 1;
	puts("SYNBENCH_PASS");
	return 0;
}

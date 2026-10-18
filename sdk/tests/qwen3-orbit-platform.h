/* Native OS boundary for the unmodified upstream qwen3.c compute engine.
 * File I/O, mapping, allocation and timing use Orbit. stdio is used only for
 * diagnostics and the final report, outside the measured decode interval. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <synapse/syn.h>
static struct syn_orbit qwen_orbit;

struct native_allocation {
	size_t mapped, bytes;
	uint64_t reserved[2];
};

static void *qwen_native_malloc(size_t bytes) {
	if (bytes > SIZE_MAX - sizeof(struct native_allocation) - 4095)
		return NULL;
	size_t total = (bytes + sizeof(struct native_allocation) + 4095) & ~(size_t)4095;
	void *p =
	    syn_memory_map(&qwen_orbit, 0, total, 0, SYN_PROT_READ | SYN_PROT_WRITE, SYN_MAP_PRIVATE);
	if ((intptr_t)p < 0)
		return NULL;
	struct native_allocation *header = p;
	header->mapped = total;
	header->bytes = bytes;
	return header + 1;
}

static void qwen_native_free(void *p) {
	if (!p)
		return;
	struct native_allocation *header = (struct native_allocation *)p - 1;
	syn_memory_unmap(header, header->mapped);
}

static void *qwen_native_calloc(size_t n, size_t bytes) {
	if (bytes && n > SIZE_MAX / bytes)
		return NULL;
	return qwen_native_malloc(n * bytes); /* fresh anonymous mappings are zeroed */
}

struct native_file {
	void *data;
	size_t bytes, position;
	int lent, slot;
};
static struct native_file *qwen_files[16];

static FILE *qwen_native_fopen(const char *path, const char *mode) {
	if (strcmp(mode, "rb"))
		return NULL;
	syn_cap_t cap;

	if (syn_open(&qwen_orbit, path, O_RDONLY, 0, SYN_RIGHT_READ | SYN_RIGHT_MAP | SYN_RIGHT_STAT,
	             &cap))
		return NULL;
	struct syn_request query = { .opcode = SYN_OP_STAT, .object = cap };
	struct syn_completion result;

	if (syn_orbit_call(&qwen_orbit, &query, &result) || result.result <= 0) {
		syn_cap_drop(&qwen_orbit, cap);
		return NULL;
	}

	struct native_file *f = qwen_native_calloc(1, sizeof(*f));

	if (!f) {
		syn_cap_drop(&qwen_orbit, cap);
		return NULL;
	}

	f->bytes = result.result;
	f->data = syn_memory_map(&qwen_orbit, cap, f->bytes, 0, SYN_PROT_READ, SYN_MAP_PRIVATE);
	syn_cap_drop(&qwen_orbit, cap);

	if ((intptr_t)f->data < 0) {
		qwen_native_free(f);
		return NULL;
	}

	for (int i = 0; i < 16; i++)
		if (!qwen_files[i]) {
			qwen_files[i] = f;
			f->slot = i;
			return (FILE *)f;
		}

	syn_memory_unmap(f->data, f->bytes);
	qwen_native_free(f);
	return NULL;
}

static int qwen_native_fseek(FILE *stream, long offset, int whence) {
	struct native_file *f = (void *)stream;
	int64_t base = whence == SEEK_SET   ? 0
	               : whence == SEEK_CUR ? (int64_t)f->position
	                                    : (int64_t)f->bytes;
	if (offset < -base || (uint64_t)(base + offset) > f->bytes)
		return -1;
	f->position = base + offset;
	return 0;
}

static long qwen_native_ftell(FILE *stream) {
	return ((struct native_file *)stream)->position;
}

static int qwen_native_fileno(FILE *stream) {
	return ((struct native_file *)stream)->slot;
}

static size_t qwen_native_fread(void *out, size_t size, size_t n, FILE *stream) {
	struct native_file *f = (void *)stream;

	if (!size || n > SIZE_MAX / size)
		return 0;
	size_t count = n * size;

	if (count > f->bytes - f->position)
		count = f->bytes - f->position;
	memcpy(out, (char *)f->data + f->position, count);
	f->position += count;
	return count / size;
}

static int qwen_native_fclose(FILE *stream) {
	struct native_file *f = (void *)stream;
	qwen_files[f->slot] = NULL;

	if (!f->lent)
		syn_memory_unmap(f->data, f->bytes);
	qwen_native_free(f);
	return 0;
}

static void *qwen_native_mmap(void *hint, size_t length, int prot, int flags, int fd,
                              off_t offset) {
	(void)hint;
	(void)prot;
	(void)flags;

	if (fd < 0 || fd >= 16 || !qwen_files[fd] || offset || length != qwen_files[fd]->bytes)
		return MAP_FAILED;
	qwen_files[fd]->lent = 1;
	return qwen_files[fd]->data;
}

#define malloc qwen_native_malloc
#define calloc qwen_native_calloc
#define free qwen_native_free
#define fopen qwen_native_fopen
#define fread qwen_native_fread
#define fclose qwen_native_fclose
#define fseek qwen_native_fseek
#define ftell qwen_native_ftell
#define fileno qwen_native_fileno
#define mmap qwen_native_mmap
#define munmap syn_memory_unmap

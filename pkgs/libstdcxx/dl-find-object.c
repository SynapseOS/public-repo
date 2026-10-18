/* x86_64 compiler unwinder bridge using musl's real ELF program-header API. */
#define _GNU_SOURCE
#include <link.h>
#include <stdint.h>
#include <string.h>

struct compiler_dl_object {
	unsigned long long flags;
	void *start, *end;
	struct link_map *map;
	void *eh_frame;
	unsigned long long reserved[7];
};

struct lookup {
	uintptr_t address;
	struct compiler_dl_object *result;
};

static int find(struct dl_phdr_info *info, size_t size, void *opaque) {
	struct lookup *query = opaque;
	uintptr_t first = UINTPTR_MAX, last = 0, eh = 0;
	int contains = 0;
	(void)size;

	for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
		const ElfW(Phdr) *p = &info->dlpi_phdr[i];
		uintptr_t start = info->dlpi_addr + p->p_vaddr;

		if (p->p_type == PT_LOAD) {
			uintptr_t end = start + p->p_memsz;

			if (start < first)
				first = start;
			if (end > last)
				last = end;
			if (query->address >= start && query->address < end)
				contains = 1;
		}

		if (p->p_type == PT_GNU_EH_FRAME)
			eh = start;
	}

	if (!contains)
		return 0;
	memset(query->result, 0, sizeof(*query->result));
	query->result->start = (void *)first;
	query->result->end = (void *)last;
	query->result->eh_frame = (void *)eh;
	return 1;
}

int _dl_find_object(void *address, struct compiler_dl_object *result) {
	struct lookup query = { (uintptr_t)address, result };
	return dl_iterate_phdr(find, &query) > 0 ? 0 : -1;
}

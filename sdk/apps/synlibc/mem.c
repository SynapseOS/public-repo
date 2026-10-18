// mem.c: Выделение памяти через нативный API
// Автор: Елчинян Арен Артурович, 2026
// Подсистема: synlibc, пользовательская библиотека нативного API
// Создан: 09.10.2026
// Последнее изменение: 14.10.2026

#include <synlibc.h>

// Простой аллокатор памяти
#define PAGE_SIZE 4096

typedef struct mem_block {
	struct mem_block *next;
	size_t size;
	bool is_free;
} mem_block_t;

static mem_block_t *heap_head = NULL;

void *malloc(size_t size) {
	if (size == 0) {
		return NULL;
	}

	mem_block_t *curr = heap_head;

	while (curr) {
		if (curr->is_free && curr->size >= size + sizeof(mem_block_t)) {
			curr->is_free = false;
			return (void *)(curr + 1);
		}

		curr = curr->next;
	}

	size_t alloc_size = size + sizeof(mem_block_t);
	size_t pages = (alloc_size + PAGE_SIZE - 1) / PAGE_SIZE;

	void *ptr = syn_alloc_pages(pages);

	if (!ptr) {
		return NULL;
	}

	mem_block_t *new_block = (mem_block_t *)ptr;
	new_block->size = pages * PAGE_SIZE;
	new_block->is_free = false;
	new_block->next = heap_head;
	heap_head = new_block;

	return (void *)(new_block + 1);
}

void free(void *ptr) {
	if (!ptr) {
		return;
	}

	mem_block_t *block = (mem_block_t *)ptr - 1;
	block->is_free = true;
}

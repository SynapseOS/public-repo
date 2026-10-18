// vgpu.c: Обёртки запросов к виртуальному framebuffer
// Автор: Елчинян Арен Артурович, 2026
// Подсистема: synlibc, пользовательская библиотека нативного API
// Создан: 09.10.2026
// Последнее изменение: 14.10.2026

#include <vgpu.h>
#include <syscalls.h>

int syn_vgpu_get_info(struct syn_vgpu_info *info) {
	if (!info) {
		return -1;
	}

	return (int)syscall1(SYS_SYN_VGPU_GET_INFO, (uint64_t)info);
}

int syn_vgpu_alloc_window(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
	return (int)syscall4(SYS_SYN_VGPU_ALLOC_WINDOW, (uint64_t)x, (uint64_t)y, (uint64_t)w,
	                     (uint64_t)h);
}

int syn_vgpu_free_window(int id) {
	return (int)syscall1(SYS_SYN_VGPU_FREE_WINDOW, (uint64_t)id);
}

int syn_vgpu_blit(int id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void *buf) {
	if (!buf) {
		return -1;
	}

	return (int)syscall6(SYS_SYN_VGPU_BLIT, (uint64_t)id, (uint64_t)x, (uint64_t)y, (uint64_t)w,
	                     (uint64_t)h, (uint64_t)buf);
}

int syn_vgpu_draw_rect(int id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
	return (int)syscall6(SYS_SYN_VGPU_DRAW_RECT, (uint64_t)id, (uint64_t)x, (uint64_t)y,
	                     (uint64_t)w, (uint64_t)h, (uint64_t)color);
}

int syn_vgpu_flush(int id) {
	return (int)syscall1(SYS_SYN_VGPU_FLUSH, (uint64_t)id);
}
// orbit.c: Доступ к очередям нативного API Orbit
// Автор: Елчинян Арен Артурович, 2026
// Подсистема: synlibc, пользовательская библиотека нативного API
// Создан: 13.10.2026
// Последнее изменение: 14.10.2026

#include <synapse/syn.h>

#if !defined(__x86_64__)
#error "Orbit syscall transport currently supports x86_64"
#endif

// Вызов отделён от syscall libc: возвращаем ошибки в пространстве Orbit
static long native_call(long number, long a, long b, long c) {
	long result;
	__asm__ volatile("syscall"
	                 : "=a"(result)
	                 : "a"(number), "D"(a), "S"(b), "d"(c)
	                 : "rcx", "r11", "cc", "memory");
	return result;
}

static void zero(void *ptr, size_t count) {
	unsigned char *p = ptr;

	while (count--) {
		*p++ = 0;
	}
}

int syn_orbit_create(struct syn_orbit *o, unsigned entries) {
	if (!o) {
		return -SYN_EINVAL;
	}

	zero(o, sizeof(*o));
	o->fd = -1;
	struct syn_setup setup = { .version = SYN_ORBIT_VERSION, .entries = entries };
	long fd = native_call(SYS_SYN_ORBIT_CREATE, (long)&setup, 0, 0);

	if (fd < 0) {
		return fd;
	}

	o->fd = fd;
	o->entries = entries;
	o->bytes = setup.ring_bytes;
	o->next_id = 1;
	o->ring = (void *)(uintptr_t)setup.ring_address;
	// Смещения берём из результата setup, а не из доступных для записи полей кольца
	o->sq = (void *)((char *)o->ring + sizeof(struct syn_ring));
	o->cq = (void *)((char *)o->sq + entries * sizeof(struct syn_request));
	return 0;
}

int syn_orbit_destroy(struct syn_orbit *o) {
	if (!o || o->fd < 0) {
		return -SYN_EBADF;
	}

	int error = syn_close(o->fd);
	int unmap = syn_memory_unmap(o->ring, o->bytes);
	zero(o, sizeof(*o));
	o->fd = -1;
	return error ? error : unmap;
}

int syn_orbit_submit(struct syn_orbit *o, const struct syn_request *q) {
	if (!o || o->fd < 0 || !q) {
		return -SYN_EINVAL;
	}

	uint32_t tail = __atomic_load_n(&o->ring->sq_tail, __ATOMIC_RELAXED);
	uint32_t head = __atomic_load_n(&o->ring->sq_head, __ATOMIC_ACQUIRE);

	if ((uint32_t)(tail - head) >= o->entries) {
		return -SYN_EAGAIN;
	}

	o->sq[tail & (o->entries - 1)] = *q;
	++o->outstanding;
	__atomic_store_n(&o->ring->sq_tail, tail + 1, __ATOMIC_RELEASE);
	return 0;
}

int syn_orbit_enter(struct syn_orbit *o, unsigned minimum, uint64_t deadline) {
	if (!o || o->fd < 0) {
		return -SYN_EBADF;
	}

	return native_call(SYS_SYN_ORBIT_ENTER, o->fd, minimum, deadline);
}

int syn_orbit_next(struct syn_orbit *o, struct syn_completion *completion) {
	if (!o || !completion || o->fd < 0) {
		return -SYN_EINVAL;
	}

	uint32_t head = __atomic_load_n(&o->ring->cq_head, __ATOMIC_RELAXED);
	uint32_t tail = __atomic_load_n(&o->ring->cq_tail, __ATOMIC_ACQUIRE);

	if (head == tail) {
		return 0;
	}

	if ((uint32_t)(tail - head) > o->entries) {
		return -SYN_EINVAL;
	}

	*completion = o->cq[head & (o->entries - 1)];

	if (o->outstanding) {
		--o->outstanding;
	}

	__atomic_store_n(&o->ring->cq_head, head + 1, __ATOMIC_RELEASE);
	return 1;
}

int syn_orbit_call(struct syn_orbit *o, struct syn_request *q, struct syn_completion *completion) {
	if (!o || !q || !completion) {
		return -SYN_EINVAL;
	}

	if (o->outstanding) {
		return -SYN_EBUSY;
	}

	if (!q->id) {
		q->id = o->next_id++;

		if (!o->next_id) {
			o->next_id = 1;
		}
	}

	int error = syn_orbit_submit(o, q);

	if (error) {
		return error;
	}

	error = syn_orbit_enter(o, 1, 0);

	if (error < 0) {
		return error;
	}

	error = syn_orbit_next(o, completion);

	if (error != 1) {
		return error < 0 ? error : -SYN_EIO;
	}

	return completion->status ? -(int)completion->status : 0;
}

int syn_orbit_control(struct syn_orbit *o, struct syn_control *ctl) {
	if (!o || !ctl || o->fd < 0) {
		return -SYN_EINVAL;
	}

	return native_call(SYS_SYN_ORBIT_CONTROL, o->fd, (long)ctl, 0);
}

static int get_cap(struct syn_orbit *o, struct syn_control *ctl, syn_cap_t *out) {
	if (!out) {
		return -SYN_EINVAL;
	}

	int error = syn_orbit_control(o, ctl);

	if (!error) {
		*out = ctl->object;
	}

	return error;
}

int syn_cap_import(struct syn_orbit *o, int fd, unsigned rights, syn_cap_t *out) {
	struct syn_control ctl = { .command = SYN_CTL_IMPORT, .rights = rights, .value = fd };
	return get_cap(o, &ctl, out);
}

int syn_cap_restrict(struct syn_orbit *o, syn_cap_t cap, unsigned rights, syn_cap_t *out) {
	struct syn_control ctl = { .command = SYN_CTL_RESTRICT, .rights = rights, .object = cap };
	return get_cap(o, &ctl, out);
}

int syn_cap_drop(struct syn_orbit *o, syn_cap_t cap) {
	struct syn_control ctl = { .command = SYN_CTL_DROP, .object = cap };
	return syn_orbit_control(o, &ctl);
}

int syn_open(struct syn_orbit *o, const char *path, unsigned flags, unsigned mode, unsigned rights,
             syn_cap_t *out) {
	struct syn_control ctl = { .command = SYN_CTL_OPEN,
		                       .rights = rights,
		                       .value = flags,
		                       .address = (uintptr_t)path,
		                       .length = mode };
	return get_cap(o, &ctl, out);
}

int syn_buffer_create(struct syn_orbit *o, size_t bytes, struct syn_buffer *out) {
	if (!out) {
		return -SYN_EINVAL;
	}

	struct syn_control ctl = { .command = SYN_CTL_BUFFER,
		                       .rights = SYN_RIGHT_READ | SYN_RIGHT_WRITE,
		                       .length = bytes };
	int error = syn_orbit_control(o, &ctl);

	if (!error) {
		*out = (struct syn_buffer){ ctl.object, (void *)(uintptr_t)ctl.address, ctl.length };
	}

	return error;
}

int syn_buffer_destroy(struct syn_orbit *o, struct syn_buffer *buffer) {
	if (!buffer) {
		return -SYN_EINVAL;
	}

	int error = syn_cap_drop(o, buffer->cap);

	if (error) {
		return error;
	}

	error = syn_memory_unmap(buffer->data, buffer->bytes);
	zero(buffer, sizeof(*buffer));
	return error;
}

int syn_scope_create(struct syn_orbit *o, syn_cap_t *out) {
	struct syn_control ctl = { .command = SYN_CTL_SCOPE };
	return get_cap(o, &ctl, out);
}

int syn_scope_cancel(struct syn_orbit *o, syn_cap_t scope) {
	struct syn_control ctl = { .command = SYN_CTL_CANCEL_SCOPE, .object = scope };
	return syn_orbit_control(o, &ctl);
}

int syn_request_cancel(struct syn_orbit *o, uint64_t id) {
	struct syn_control ctl = { .command = SYN_CTL_CANCEL_ID, .value = id };
	return syn_orbit_control(o, &ctl);
}

int syn_process_import(struct syn_orbit *o, int64_t pid, syn_cap_t *out) {
	struct syn_control ctl = { .command = SYN_CTL_PROCESS, .value = pid };
	return get_cap(o, &ctl, out);
}

int syn_channel_create(struct syn_orbit *o, syn_cap_t *cap, int *fd) {
	if (!cap || !fd) {
		return -SYN_EINVAL;
	}

	struct syn_control ctl = { .command = SYN_CTL_CHANNEL,
		                       .rights = SYN_RIGHT_READ | SYN_RIGHT_WRITE | SYN_RIGHT_WAIT };
	int error = get_cap(o, &ctl, cap);

	if (!error) {
		*fd = ctl.fd;
	}

	return error;
}

int syn_orbit_stats(struct syn_orbit *o, struct syn_orbit_stats *stats) {
	struct syn_control ctl = { .command = SYN_CTL_STATS, .address = (uintptr_t)stats };
	return syn_orbit_control(o, &ctl);
}

void *syn_memory_map(struct syn_orbit *o, syn_cap_t object, size_t bytes, uint64_t offset,
                     unsigned prot, unsigned flags) {
	if (!o || o->fd < 0) {
		return (void *)(intptr_t)-SYN_EBADF;
	}

	struct syn_mapping map = {
		.object = object, .length = bytes, .offset = offset, .prot = prot, .flags = flags
	};
	return (void *)native_call(SYS_SYN_ORBIT_MAP, o->fd, (long)&map, 0);
}

int syn_memory_unmap(void *address, size_t bytes) {
	return native_call(SYS_SYN_ORBIT_UNMAP, (long)address, bytes, 0);
}

int syn_close(int fd) {
	return native_call(SYS_SYN_ORBIT_CLOSE, fd, 0, 0);
}

uint64_t syn_clock_us(void) {
	return native_call(SYS_SYN_ORBIT_CLOCK, 0, 0, 0);
}

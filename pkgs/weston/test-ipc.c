// test-ipc.c: гостевая проверка обмена Wayland
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: musl и Linux
// Подсистема: проверки локальных сокетов
// Первое изменение: 17.10.2026, последнее изменение: 18.10.2026
// Статус доступа: общедоступный, MIT
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

// Передать дескриптор вместе с одним байтом потока
static ssize_t send_fd(int socket, int fd) {
	union {
		struct cmsghdr alignment;
		char bytes[CMSG_SPACE(sizeof(int))];
	} control = { 0 };

	char byte = 'S';
	struct iovec vector = { &byte, 1 };
	struct msghdr message = { .msg_iov = &vector,
		                      .msg_iovlen = 1,
		                      .msg_control = control.bytes,
		                      .msg_controllen = sizeof(control.bytes) };
	struct cmsghdr *header = CMSG_FIRSTHDR(&message);
	header->cmsg_len = CMSG_LEN(sizeof(int));
	header->cmsg_level = SOL_SOCKET;
	header->cmsg_type = SCM_RIGHTS;
	memcpy(CMSG_DATA(header), &fd, sizeof(fd));
	return sendmsg(socket, &message, MSG_NOSIGNAL);
}

// Проверить передачу уже удалённого файла и общую память между процессами
static void check_rights(void) {
	int pair[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
	assert((fcntl(pair[0], F_GETFD) & FD_CLOEXEC) != 0);
	char path[] = "/tmp/weston-ipc-XXXXXX";
	int file = mkstemp(path);
	assert(file >= 0);
	assert(unlink(path) == 0);
	assert(ftruncate(file, 4096) == 0);
	volatile char *view = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
	assert(view != MAP_FAILED);
	view[0] = 'A';
	pid_t child = fork( );
	assert(child >= 0);

	if (child == 0) {
		close(pair[0]);
		close(file);
		munmap((void *)view, 4096);

		union {
			struct cmsghdr alignment;
			char bytes[CMSG_SPACE(sizeof(int))];
		} control = { 0 };

		char byte = 0;
		struct iovec vector = { &byte, 1 };
		struct msghdr message = { .msg_iov = &vector,
			                      .msg_iovlen = 1,
			                      .msg_control = control.bytes,
			                      .msg_controllen = sizeof(control.bytes) };
		assert(recvmsg(pair[1], &message, MSG_CMSG_CLOEXEC) == 1 && byte == 'S');
		struct cmsghdr *header = CMSG_FIRSTHDR(&message);
		assert(header != NULL && header->cmsg_level == SOL_SOCKET &&
		       header->cmsg_type == SCM_RIGHTS);
		int received;
		memcpy(&received, CMSG_DATA(header), sizeof(received));
		assert((fcntl(received, F_GETFD) & FD_CLOEXEC) != 0);
		struct stat status;
		assert(fstat(received, &status) == 0 && status.st_size == 4096);
		char *shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, received, 0);
		assert(shared != MAP_FAILED && shared[0] == 'A');
		shared[0] = 'B';
		assert(write(pair[1], "OK", 2) == 2);
		close(received);
		munmap(shared, 4096);
		close(pair[1]);
		_exit(0);
	}

	close(pair[1]);
	assert(send_fd(pair[0], -1) == -1 && errno == EBADF);
	assert(send_fd(pair[0], file) == 1);
	close(file);
	char reply[2];
	assert(read(pair[0], reply, 2) == 2 && memcmp(reply, "OK", 2) == 0);
	assert(view[0] == 'B');
	int status;
	assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	assert(read(pair[0], reply, sizeof(reply)) == 0);
	close(pair[0]);
	munmap((void *)view, 4096);
	puts("SCM_RIGHTS_SHARED_MMAP_PASS");
}

// Проверить именованный сокет, неблокирующий приём и полномочия другой стороны
static void check_listener(void) {
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	snprintf(address.sun_path, sizeof(address.sun_path), "/tmp/weston-ipc-%ld.sock",
	         (long)getpid( ));
	socklen_t length =
	    (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(address.sun_path) + 1);
	int server = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
	assert(server >= 0);
	assert(bind(server, (struct sockaddr *)&address, length) == 0);
	assert(listen(server, 4) == 0);
	assert(accept(server, NULL, NULL) == -1 && errno == EAGAIN);
	pid_t child = fork( );
	assert(child >= 0);

	if (child == 0) {
		close(server);
		int client = socket(AF_UNIX, SOCK_STREAM, 0);
		assert(client >= 0 && connect(client, (struct sockaddr *)&address, length) == 0);
		assert(write(client, "ping", 4) == 4);
		char reply[4];
		assert(read(client, reply, sizeof(reply)) == 4 && memcmp(reply, "pong", 4) == 0);
		close(client);
		_exit(0);
	}

	struct pollfd ready = { .fd = server, .events = POLLIN };
	assert(poll(&ready, 1, 5000) == 1 && (ready.revents & POLLIN) != 0);
	int accepted = accept4(server, NULL, NULL, SOCK_CLOEXEC);
	assert(accepted >= 0 && (fcntl(accepted, F_GETFD) & FD_CLOEXEC) != 0);
	struct ucred credentials;
	socklen_t size = sizeof(credentials);
	assert(getsockopt(accepted, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0);
	assert(size == sizeof(credentials) && credentials.pid == child);
	char request[4];
	assert(read(accepted, request, sizeof(request)) == 4 && memcmp(request, "ping", 4) == 0);
	assert(write(accepted, "pong", 4) == 4);
	int status;
	assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	close(accepted);
	close(server);
	int again = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(again >= 0 && bind(again, (struct sockaddr *)&address, length) == 0);
	close(again);
	puts("UNIX_LISTENER_CREDENTIALS_PASS");
}

// Проверить ограничение очереди, ошибки и освобождение ресурсов на четырёх CPU
static void check_limits(void) {
	int pair[2];
	assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0);
	assert(send_fd(pair[0], pair[1]) == -1 && errno == EOPNOTSUPP);
	char *bytes = malloc(65536);
	assert(bytes != NULL);
	memset(bytes, 0x5a, 65536);
	size_t total = 0;
	ssize_t result;

	while ((result = write(pair[0], bytes, 65536)) > 0) {
		total += (size_t)result;
		assert(total <= 256U * 1024U);
	}

	assert(result == -1 && errno == EAGAIN && total != 0);
	size_t received = 0;

	while ((result = read(pair[1], bytes, 65536)) > 0) {
		for (ssize_t i = 0; i < result; i++) {
			assert((unsigned char)bytes[i] == 0x5a);
		}

		received += (size_t)result;
	}

	assert(result == -1 && errno == EAGAIN && received == total);
	assert(shutdown(pair[0], SHUT_WR) == 0);
	assert(read(pair[1], bytes, 1) == 0);
	close(pair[0]);
	close(pair[1]);
	free(bytes);
	pid_t children[4];

	for (unsigned worker = 0; worker < 4; worker++) {
		children[worker] = fork( );
		assert(children[worker] >= 0);

		if (children[worker] == 0) {
			for (unsigned i = 0; i < 128; i++) {
				int sockets[2];
				assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
				assert(write(sockets[0], "test", 4) == 4);
				close(sockets[0]);
				char reply[4];
				assert(read(sockets[1], reply, 4) == 4 && memcmp(reply, "test", 4) == 0);
				assert(read(sockets[1], reply, 4) == 0);
				close(sockets[1]);
			}

			_exit(0);
		}
	}

	for (unsigned worker = 0; worker < 4; worker++) {
		int status;
		assert(waitpid(children[worker], &status, 0) == children[worker]);
		assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	}

	puts("UNIX_QUEUE_LIMIT_AND_SMP_LIFETIME_PASS");
}

// Запустить проверки реального ядра без изменения пользовательских файлов
int main(void) {
	check_rights( );
	check_listener( );
	check_limits( );
	puts("WESTON_IPC_PASS");
	return 0;
}

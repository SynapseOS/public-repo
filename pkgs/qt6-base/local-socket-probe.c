// local-socket-probe.c: проверить имена локальных сокетов для Qt и Telegram
// Автор: Елчинян Арен Артурович, 2026, архитектура x86_64, ABI musl и Linux
// Подсистема: проверки графических пакетов
// Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
// Статус доступа: общедоступный, MIT
#define _GNU_SOURCE
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// Завершить проверку при нарушении ожидаемого результата
static void require(int condition, const char *operation) {
	if (condition == 0) {
		fprintf(stderr, "QT_LOCAL_SOCKET_FAIL %s errno=%d\n", operation, errno);
		exit(1);
	}
}

// Создать потоковый сокет с закрытием дескриптора после exec
static int new_socket(void) {
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	require(fd >= 0, "socket");
	return fd;
}

// Проверить адрес, дублирование имени, обмен байтом и освобождение имени
static void exchange(const struct sockaddr_un *address, socklen_t length, int pathname) {
	int server = new_socket( );
	require(bind(server, (const struct sockaddr *)address, length) == 0, "bind");
	require(listen(server, 4) == 0, "listen");
	int duplicate = new_socket( );
	errno = 0;
	require(bind(duplicate, (const struct sockaddr *)address, length) == -1 && errno == EADDRINUSE,
	        "duplicate bind");
	require(close(duplicate) == 0, "close duplicate");

	int client = new_socket( );
	require(connect(client, (const struct sockaddr *)address, length) == 0, "connect");
	int accepted = accept4(server, NULL, NULL, SOCK_CLOEXEC);
	require(accepted >= 0, "accept");
	struct sockaddr_un observed = { 0 };
	socklen_t observed_length = sizeof observed;
	require(getsockname(server, (struct sockaddr *)&observed, &observed_length) == 0 &&
	            observed_length == length && memcmp(&observed, address, length) == 0,
	        "getsockname");
	observed_length = sizeof observed;
	require(getpeername(client, (struct sockaddr *)&observed, &observed_length) == 0 &&
	            observed_length == length && memcmp(&observed, address, length) == 0,
	        "getpeername");
	require(write(client, "Q", 1) == 1, "write client");
	char byte = 0;
	require(read(accepted, &byte, 1) == 1 && byte == 'Q', "read server");
	require(shutdown(client, SHUT_WR) == 0, "shutdown");
	require(read(accepted, &byte, 1) == 0, "read EOF");
	require(close(accepted) == 0 && close(client) == 0 && close(server) == 0, "close sockets");

	if (pathname != 0) {
		errno = 0;
		int result = unlink(address->sun_path);
		require(result == 0 || errno == ENOENT, "unlink owned socket path");
	}

	server = new_socket( );
	require(bind(server, (const struct sockaddr *)address, length) == 0, "reuse name");
	require(close(server) == 0, "close reused socket");
	if (pathname != 0) {
		errno = 0;
		int result = unlink(address->sun_path);
		require(result == 0 || errno == ENOENT, "unlink reused socket path");
	}
}

// Собрать абстрактное имя с нулём внутри и значимым последним байтом
static socklen_t abstract_name(struct sockaddr_un *address, char suffix) {
	memset(address, 0, sizeof *address);
	address->sun_family = AF_UNIX;
	int count = snprintf(address->sun_path + 1, sizeof address->sun_path - 2, "synapse-qt-%ld",
	                     (long)getpid( ));
	require(count > 0 && (size_t)count < sizeof address->sun_path - 3, "abstract name length");
	address->sun_path[(size_t)count + 2] = suffix;
	return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + (size_t)count + 3);
}

// Проверить пути и двоичные имена без сетевого доступа
int main(void) {
	struct sockaddr_un first, second;
	socklen_t length = abstract_name(&first, 'a');
	abstract_name(&second, 'b');
	int a = new_socket( );
	int b = new_socket( );
	require(bind(a, (const struct sockaddr *)&first, length) == 0, "abstract a");
	require(bind(b, (const struct sockaddr *)&second, length) == 0, "embedded NUL distinction");
	require(close(a) == 0 && close(b) == 0, "close distinct names");
	exchange(&first, length, 0);

	memset(first.sun_path, 'x', sizeof first.sun_path);
	first.sun_path[0] = 0;
	exchange(&first, sizeof first, 0);

	char directory[] = "/tmp/qt-local-probe-XXXXXX";
	require(mkdtemp(directory) != NULL, "private directory");
	memset(&first, 0, sizeof first);
	first.sun_family = AF_UNIX;
	int count = snprintf(first.sun_path, sizeof first.sun_path, "%s/socket", directory);
	require(count > 0 && (size_t)count < sizeof first.sun_path, "pathname length");
	exchange(&first, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + (size_t)count + 1), 1);
	require(rmdir(directory) == 0, "remove private directory");
	puts("QT_LOCAL_SOCKET_PASS");
	return 0;
}

// smoke.c: проверить динамическую линковку glibc в SynapseOS
// Автор: Елчинян Арен Артурович
// Год: 2026
// Архитектура: x86_64
// ABI: Linux x86_64
// Описание: проверить TLS, потоки, NSS и динамическую загрузку библиотеки
// Подсистема: пакет glibc-dev
// Первое изменение: 25.10.2026
// Последнее изменение: 25.10.2026
// Статус доступа: общедоступный

#include <dlfcn.h>
#include <arpa/inet.h>
#include <langinfo.h>
#include <locale.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static __thread int thread_value;

// Проверить отдельное значение TLS в новом потоке
static void *check_thread(void *argument) {
	(void)argument;
	thread_value = 17;

	if (thread_value != 17) {
		return (void *)1;
	}

	return NULL;
}

// Проверить локали, упакованные с glibc
static int check_locales(void) {
	const char *names[] = {"en_US.UTF-8", "ru_RU.UTF-8"};

	for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
		if (setlocale(LC_ALL, names[index]) == NULL || strcmp(nl_langinfo(CODESET), "UTF-8") != 0) {
			fprintf(stderr, "GLIBC_SMOKE_FAIL locale=%s\n", names[index]);
			return 1;
		}
	}

	return 0;
}

// Проверить создание, чтение и удаление файла
static int check_file(void) {
	char path[] = "/tmp/glibc-smoke-XXXXXX";
	char result[5] = {0};
	int file = mkstemp(path);
	int failed = 0;

	if (file < 0) {
		perror("GLIBC_SMOKE_FAIL mkstemp");
		return 1;
	}

	if (write(file, "file", 4) != 4 || lseek(file, 0, SEEK_SET) != 0 ||
		read(file, result, 4) != 4 || strcmp(result, "file") != 0) {
		fprintf(stderr, "GLIBC_SMOKE_FAIL file_io\n");
		failed = 1;
	}

	if (close(file) != 0) {
		fprintf(stderr, "GLIBC_SMOKE_FAIL file_close\n");
		failed = 1;
	}

	if (unlink(path) != 0) {
		fprintf(stderr, "GLIBC_SMOKE_FAIL file_cleanup\n");
		failed = 1;
	}

	return failed;
}

// Проверить TCP через локальный интерфейс без внешнего сервера
static int check_tcp(void) {
	struct sockaddr_in address = {0};
	socklen_t address_size = sizeof(address);
	char result[5] = {0};
	int server = -1;
	int client = -1;
	int accepted = -1;
	int failed = 1;

	server = socket(AF_INET, SOCK_STREAM, 0);
	client = socket(AF_INET, SOCK_STREAM, 0);

	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = 0;

	if (server >= 0 && client >= 0 &&
		bind(server, (struct sockaddr *)&address, sizeof(address)) == 0 &&
		listen(server, 1) == 0 &&
		getsockname(server, (struct sockaddr *)&address, &address_size) == 0 &&
		connect(client, (struct sockaddr *)&address, address_size) == 0) {
		accepted = accept(server, NULL, NULL);

		if (accepted >= 0 && write(client, "tcp!", 4) == 4 &&
			read(accepted, result, 4) == 4 && strcmp(result, "tcp!") == 0) {
			failed = 0;
		}
	}

	if (accepted >= 0) {
		close(accepted);
	}

	if (client >= 0) {
		close(client);
	}

	if (server >= 0) {
		close(server);
	}

	if (failed != 0) {
		fprintf(stderr, "GLIBC_SMOKE_FAIL tcp\n");
	}

	return failed;
}

// Проверить минимальные функции динамической glibc
int main(void) {
	pthread_t thread;
	void *thread_result = NULL;
	struct addrinfo *addresses = NULL;
	void *math_library = NULL;
	double (*cosine)(double);
	int result;

	thread_value = 23;
	result = pthread_create(&thread, NULL, check_thread, NULL);

	if (result != 0) {
		fprintf(stderr, "GLIBC_SMOKE_FAIL pthread_create=%d\n", result);
		return 1;
	}

	result = pthread_join(thread, &thread_result);

	if (result != 0 || thread_result != NULL || thread_value != 23) {
		fprintf(stderr, "GLIBC_SMOKE_FAIL pthread_join=%d\n", result);
		return 1;
	}

	if (setlocale(LC_ALL, "C") == NULL) {
		fprintf(stderr, "GLIBC_SMOKE_FAIL locale\n");
		return 1;
	}

	if (check_locales() != 0 || check_file() != 0 || check_tcp() != 0) {
		return 1;
	}

	result = getaddrinfo("localhost", "80", NULL, &addresses);

	if (result != 0 || addresses == NULL) {
		fprintf(stderr, "GLIBC_SMOKE_FAIL getaddrinfo=%d\n", result);
		return 1;
	}

	freeaddrinfo(addresses);
	addresses = NULL;
	result = getaddrinfo("synapseos.ru", "443", NULL, &addresses);

	if (result != 0 || addresses == NULL) {
		fprintf(stderr, "GLIBC_SMOKE_FAIL dns=%d\n", result);
		return 1;
	}

	freeaddrinfo(addresses);
	math_library = dlopen("libm.so.6", RTLD_NOW);

	if (math_library == NULL) {
		fprintf(stderr, "GLIBC_SMOKE_FAIL dlopen=%s\n", dlerror());
		return 1;
	}

	cosine = (double (*)(double))dlsym(math_library, "cos");

	if (cosine == NULL || cosine(0.0) != 1.0) {
		fprintf(stderr, "GLIBC_SMOKE_FAIL dlsym\n");
		dlclose(math_library);
		return 1;
	}

	dlclose(math_library);
	puts("GLIBC_SMOKE_PASS");
	return 0;
}

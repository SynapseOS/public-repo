// async-probe.cpp: проверить фоновые задачи и последовательную очередь AyuGram
// Автор: Елчинян Арен Артурович, 2026, архитектура x86_64, ABI musl и Linux
// Подсистема: проверки графических пакетов
// Первое изменение: 19.10.2026, последнее изменение: 19.10.2026
// Статус доступа: общедоступный, MIT
#include <QtCore/QCoreApplication>
#include <QtCore/QSemaphore>
#include <QtCore/QThreadPool>
#include <QtCore/QDeadlineTimer>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <crl/crl_async.h>
#include <crl/crl_queue.h>

// Завершить проверку вместе с её фоновыми задачами при ошибке
[[noreturn]] static void fail(const char *message) {
	std::fputs(message, stderr);
	std::fflush(stderr);
	std::_Exit(1);
}

// Проверить вложенные фоновые задачи и порядок обработки очереди
int main(int argc, char **argv) {
	QCoreApplication application(argc, argv);
	constexpr int count = 32;
	QSemaphore completed;
	std::atomic<int> ready = 0;

	for (int i = 0; i < count; ++i) {
		crl::async([&] {
			crl::async([&] {
				ready.fetch_add(1);
				completed.release( );
			});
		});
	}

	if (!completed.tryAcquire(count, 5000) || ready.load( ) != count) {
		std::fprintf(stderr, "ASYNC_STATUS completed=%d active=%d maximum=%d ideal=%d\n",
		             ready.load( ), QThreadPool::globalInstance( )->activeThreadCount( ),
		             QThreadPool::globalInstance( )->maxThreadCount( ),
		             QThread::idealThreadCount( ));
		fail("AYUGRAM_ASYNC_PARALLEL_FAIL\n");
	}

	crl::queue queue;
	std::atomic<bool> ordered = true;
	ready.store(0);

	for (int i = 0; i < count; ++i) {
		queue.async([&, i] {
			if (ready.fetch_add(1) != i) {
				ordered.store(false);
			}

			completed.release( );
		});
	}

	if (!completed.tryAcquire(count, 5000) || !ordered.load( ) || ready.load( ) != count) {
		fail("AYUGRAM_ASYNC_QUEUE_FAIL\n");
	}

	if (!QThreadPool::globalInstance( )->waitForDone(QDeadlineTimer(5000))) {
		fail("AYUGRAM_ASYNC_DRAIN_FAIL\n");
	}

	std::puts("AYUGRAM_ASYNC_PASS");
	return 0;
}

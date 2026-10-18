// process-probe.cpp: проверить запуск дочернего процесса через Qt
// Автор: Елчинян Арен Артурович, 2026, архитектура x86_64, ABI musl и Linux
// Подсистема: проверки графических пакетов
// Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
// Статус доступа: общедоступный, MIT
#include <QCoreApplication>
#include <QProcess>
#include <cstdio>
#include <unistd.h>

// Ограничить зависание внешним сигналом, который не зависит от цикла событий Qt
int main(int argc, char **argv) {
	alarm(6);
	QCoreApplication application(argc, argv);
	QProcess child;
	if (application.arguments( ).contains(QStringLiteral("--force-fork"))) {
		child.setChildProcessModifier([]( ) {});
	}

	std::puts("QT_PROCESS_START");
	std::fflush(stdout);
	child.start(QStringLiteral("/bin/true"));
	std::puts("QT_PROCESS_STARTED_CALL");
	std::fflush(stdout);
	if (!child.waitForStarted(2000) || !child.waitForFinished(2000) || child.exitCode( ) != 0) {
		std::fprintf(stderr, "QT_PROCESS_FAIL %s\n", qPrintable(child.errorString( )));
		return 1;
	}

	alarm(0);
	std::puts("QT_PROCESS_PASS");
	return 0;
}

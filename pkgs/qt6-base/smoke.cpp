// smoke.cpp: проверка окна, русского ввода и таймера Qt в SynapseOS
// Автор: Елчинян Арен Артурович, 2026, архитектура x86_64, ABI musl и Linux
// Подсистема: графические пакеты
// Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
// Статус доступа: общедоступный, MIT
#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <atomic>
#include <cstdio>

static std::atomic<bool> painting_failed{ false };

// Не считать сработавший таймер успешным тестом при отказе буфера или рисования
static void report_message(QtMsgType type, const QMessageLogContext &context,
                           const QString &message) {
	(void)type;
	(void)context;
	if ((message.contains(QStringLiteral("QWaylandShmBuffer")) &&
	     message.contains(QStringLiteral("failed"))) ||
	    message.contains(QStringLiteral("Painter not active")) ||
	    message.contains(QStringLiteral("Paint device returned engine"))) {
		painting_failed.store(true);
	}

	std::fprintf(stderr, "%s\n", qPrintable(message));
}

// Создать окно без генераторов moc и завершить автоматическую проверку по таймеру
int main(int argc, char **argv) {
	qInstallMessageHandler(report_message);
	QApplication application(argc, argv);
	QWidget window;
	QVBoxLayout layout(&window);
	QLabel title(QString::fromUtf8("SynapseOS: Qt Widgets / Wayland"));
	QLineEdit input;
	QLabel status(QString::fromUtf8("Русский ввод: проверьте Alt+Shift и колесо мыши"));
	layout.addWidget(&title);
	layout.addWidget(&input);
	layout.addWidget(&status);
	window.setWindowTitle(QString::fromUtf8("SynapseOS Qt smoke test"));
	window.resize(640, 260);
	window.show( );
	std::printf("QT_SMOKE_STARTED Qt=%s platform=%s\n", qVersion( ),
	            qPrintable(QGuiApplication::platformName( )));
	std::fflush(stdout);

	if (application.arguments( ).contains(QStringLiteral("--self-test"))) {
		QTimer::singleShot(1500, &application, [&application]( ) {
			if (painting_failed.load( )) {
				std::puts("QT_SMOKE_FAIL");
				application.exit(1);
				return;
			}

			std::puts("QT_SMOKE_PASS");
			application.quit( );
		});
	}

	return application.exec( );
}

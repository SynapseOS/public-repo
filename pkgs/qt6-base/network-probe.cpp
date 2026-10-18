// network-probe.cpp: проверить TCP и уведомления Qt через тестовый сервер хоста
// Автор: Елчинян Арен Артурович, 2026, архитектура x86_64, ABI musl и Linux
// Подсистема: проверки графических пакетов
// Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
// Статус доступа: общедоступный, MIT
#include <QCoreApplication>
#include <QTcpSocket>
#include <QTimer>
#include <cstdio>
#include <unistd.h>

// Проверить соединение, отправку запроса и чтение ответа без учётных данных
int main(int argc, char **argv) {
	alarm(8);
	QCoreApplication application(argc, argv);
	QTcpSocket socket;
	QObject::connect(&socket, &QTcpSocket::connected, [&socket]( ) {
		std::puts("QT_NETWORK_CONNECTED");
		std::fflush(stdout);
		socket.write("HEAD / HTTP/1.0\r\n\r\n");
	});
	QObject::connect(&socket, &QTcpSocket::readyRead, [&application, &socket]( ) {
		QByteArray data = socket.readAll( );
		std::printf("QT_NETWORK_REPLY %lld\n", static_cast<long long>(data.size( )));
		std::puts("QT_NETWORK_PASS");
		application.exit(0);
	});
	QObject::connect(&socket, &QTcpSocket::errorOccurred, [&application, &socket]( ) {
		std::fprintf(stderr, "QT_NETWORK_FAIL %s\n", qPrintable(socket.errorString( )));
		application.exit(1);
	});
	QTimer::singleShot(5000, &application, [&application, &socket]( ) {
		std::fprintf(stderr, "QT_NETWORK_TIMEOUT state=%d error=%d %s\n", (int)socket.state( ),
		             (int)socket.error( ), qPrintable(socket.errorString( )));
		application.exit(1);
	});
	socket.connectToHost(QStringLiteral("10.0.2.2"), 18088);
	return application.exec( );
}

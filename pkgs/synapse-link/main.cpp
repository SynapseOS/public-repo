// main.cpp: Synapse Link, автор Елчинян Арен Артурович, 2026
// Архитектура x86_64, ABI musl, подсистема графических приложений
// Первое и последнее изменение: 27.11.2026, доступ общедоступный
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>
#include <cstdio>
#include <sys/ioctl.h>

// Проверить ответ телефона даже при повторном использовании имени устройства
static QString connection() {
    const QDir dev(QStringLiteral("/dev"));
    const auto nodes = dev.entryList({QStringLiteral("synapse-link*")}, QDir::System | QDir::Files);
    for (const QString &name : nodes) {
        QFile device(dev.filePath(name));
        if (device.open(QIODevice::ReadOnly) && ioctl(device.handle(), 0x53595001u, 0) == 1) {
            return QStringLiteral("SynapseOS подключена по USB · протокол 1");
        }
    }

    QFile inventory(QStringLiteral("/proc/usb"));
    if (!inventory.open(QIODevice::ReadOnly)) {
        return QStringLiteral("Не удалось прочитать список устройств USB");
    }

    for (const auto &row : inventory.readAll().split('\n')) {
        if (row.startsWith("18d1:4ee7 ") && row.endsWith(" connected")) {
            return QStringLiteral("Телефон обнаружен · канал SynapseOS недоступен");
        }
    }

    return QStringLiteral("Телефон отключён");
}

// Один и тот же запрос используется в окне и при проверке через гостевой агент
int main(int argc, char **argv) {
    if (argc == 2 && QByteArray(argv[1]) == "--status") {
        std::puts(connection().toUtf8().constData());
        return 0;
    }

    QApplication app(argc, argv);
    QWidget window;
    window.setWindowTitle(QStringLiteral("Synapse Link"));
    auto *layout = new QVBoxLayout(&window);
    layout->setContentsMargins(24, 24, 24, 24);
    auto *label = new QLabel(connection(), &window);
    layout->addWidget(label);
    QTimer refresh;
    QObject::connect(&refresh, &QTimer::timeout, &window, [label] { label->setText(connection()); });
    refresh.start(1500);
    window.resize(440, 140);
    window.show();
    return app.exec();
}

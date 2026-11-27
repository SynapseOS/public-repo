// main.cpp: Synapse Link, автор Елчинян Арен Артурович, 2026
// Архитектура x86_64, ABI musl, подсистема графических приложений
// Первое и последнее изменение: 27.11.2026, доступ общедоступный
#include "package.h"
#include <QApplication>
#include <QComboBox>
#include <QSettings>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>
#include <cstdio>
#include <QFileDialog>
#include <QInputDialog>
#include <QPushButton>
#include <QProcess>
#include <QRandomGenerator>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QThread>
#include <lumine/phone-link.h>
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

// Выбирать только узел, подтвердивший поддержку передачи приложений
static int transfer(const QString &path, const QString &name, bool remove) {
    const QByteArray id = name.toUtf8();
    const QRegularExpression valid(QStringLiteral("^[\\p{L}\\p{N}][\\p{L}\\p{N} ._-]{0,62}$"));
    if (id.size() > 63 || !valid.match(name).hasMatch()) {
        std::puts("Проверьте имя приложения");
        return 1;
    }
    QByteArray bytes;
    if (!remove) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() < 64 || file.size() > SYN_LINK_LIMIT + 65536) {
            std::puts("Нужен исполняемый файл ARM64 размером до 2 МиБ");
            return 1;
        }
        bytes = phoneExecutable(file);
        if (bytes.size() < 64 || bytes.size() > SYN_LINK_LIMIT || !bytes.startsWith("\x7f" "ELF\x02\x01\x01") ||
            quint8(bytes[18]) != 183 || bytes[19] != 0) {
            std::puts("Этот файл не является приложением ARM64");
            return 1;
        }
    }
    QDir dev(QStringLiteral("/dev"));
    QFile device;
    syn_link_status reply{};
    for (const auto &node : dev.entryList({QStringLiteral("synapse-link*")}, QDir::Files | QDir::System)) {
        device.setFileName(dev.filePath(node));
        if (device.open(QIODevice::ReadWrite) && ioctl(device.handle(), SYN_LINK_QUERY, &reply) == 0) break;
        device.close();
    }
    if (!device.isOpen()) {
        std::puts("Подключите телефон со сборкой, поддерживающей установку");
        return 1;
    }
    syn_link_frame frame{};
    frame.token = QRandomGenerator::global()->generate() | 1u;
    frame.op = remove ? SYN_LINK_REMOVE : SYN_LINK_INSTALL;
    frame.offset = bytes.size();
    frame.length = 96;
    memcpy(frame.data, id.constData(), size_t(id.size()));
    memcpy(frame.data + 64, QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).constData(), 32);
    const int fd = device.handle();
    if (ioctl(fd, SYN_LINK_SEND, &frame) || ioctl(fd, SYN_LINK_QUERY, &reply) || reply.token != frame.token) {
        std::puts("Телефон занят или связь прервана");
        return 1;
    }
    frame.op = SYN_LINK_DATA;
    for (frame.offset = 0; frame.offset < quint32(bytes.size()); frame.offset += frame.length) {
        frame.length = qMin(quint32(sizeof(frame.data)), quint32(bytes.size()) - frame.offset);
        memcpy(frame.data, bytes.constData() + frame.offset, frame.length);
        if (ioctl(fd, SYN_LINK_SEND, &frame)) {
            std::puts("Передача прервана");
            return 1;
        }
    }
    std::puts("На телефоне откройте «Все приложения» → Synapse Link и подтвердите запрос");
    std::fflush(stdout);
    QElapsedTimer deadline;
    deadline.start();
    while (deadline.elapsed() < 180000) {
        if (ioctl(fd, SYN_LINK_QUERY, &reply) || reply.token != frame.token || reply.state == SYN_LINK_ERROR) {
            std::puts("Телефон отклонил запрос или отключился");
            return 1;
        }
        if (reply.state == SYN_LINK_DONE) {
            std::puts(remove ? "Приложение удалено с телефона" : "Приложение установлено на телефоне");
            return 0;
        }
        QThread::msleep(250);
    }
    std::puts("Подтверждение от телефона не получено");
    return 1;
}

// Один и тот же запрос используется в окне и при проверке через гостевой агент
int main(int argc, char **argv) {
    if (argc == 2 && QByteArray(argv[1]) == "--status") {
        std::puts(connection().toUtf8().constData());
        return 0;
    }

    if (argc == 4 && QByteArray(argv[1]) == "--install") {
        return transfer(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]), false);
    }
    if (argc == 3 && QByteArray(argv[1]) == "--remove") {
        return transfer({}, QString::fromLocal8Bit(argv[2]), true);
    }
    QApplication app(argc, argv);
    QWidget window;
    window.setWindowTitle(QStringLiteral("Synapse Link"));
    auto *layout = new QVBoxLayout(&window);
    layout->setContentsMargins(24, 24, 24, 24);
    auto *label = new QLabel(connection(), &window);
    layout->addWidget(label);
    QSettings library("SynapseOS","Synapse Link");
    auto *saved = new QComboBox(&window);
    saved->addItem(QStringLiteral("Выбрать файл…"), QString());
    for (const auto &name : library.childKeys()) saved->addItem(name,library.value(name));
    layout->addWidget(saved);
    QString chosenPath, chosenName;
    auto *install = new QPushButton(QStringLiteral("Установить или обновить…"), &window);
    auto *remove = new QPushButton(QStringLiteral("Удалить с телефона…"), &window);
    auto *result = new QLabel(&window);
    result->setWordWrap(true);
    layout->addWidget(install);
    layout->addWidget(remove);
    layout->addWidget(result);
    QProcess worker;
    auto start = [&](bool deleting) {
        QString path;
        if (!deleting) {
            path = saved->currentData().toString();
            if (path.isEmpty() || !QFileInfo::exists(path)) path = QFileDialog::getOpenFileName(&window,
                QStringLiteral("Пакет SPKG или программа ARM64"));
            if (path.isEmpty()) return;
        }
        bool ok = false;
        const QString title = QInputDialog::getText(&window, QStringLiteral("Приложение"),
            QStringLiteral("Название плитки на телефоне"), QLineEdit::Normal, saved->currentIndex() ? saved->currentText() : QFileInfo(path).completeBaseName(), &ok);
        if (!ok || title.isEmpty()) return;
        chosenPath = path; chosenName = title;
        install->setEnabled(false);
        remove->setEnabled(false);
        result->setText(QStringLiteral("Отправка запроса…"));
        worker.start(QStringLiteral("/usr/libexec/qt6/synapse-link"), deleting ? QStringList{"--remove", title} :
            QStringList{"--install", path, title});
    };
    QObject::connect(install, &QPushButton::clicked, [&] { start(false); });
    QObject::connect(remove, &QPushButton::clicked, [&] { start(true); });
    QObject::connect(&worker, &QProcess::readyReadStandardOutput, [&] {
        result->setText(QString::fromUtf8(worker.readAllStandardOutput()).trimmed());
    });
    QObject::connect(&worker, &QProcess::errorOccurred, [&](QProcess::ProcessError) {
        result->setText(worker.errorString());
        install->setEnabled(true);
        remove->setEnabled(true);
    });
    QObject::connect(&worker, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                     [&](int code, QProcess::ExitStatus exit) {
        if (!code && exit == QProcess::NormalExit && !chosenPath.isEmpty()) {
            library.setValue(chosenName,chosenPath);
            int index = saved->findText(chosenName);
            if (index < 0) {saved->addItem(chosenName, chosenPath); index = saved->count()-1;}
            saved->setItemData(index,chosenPath);
            saved->setCurrentIndex(index);
        }
        install->setEnabled(true); remove->setEnabled(true);
    });
    QTimer refresh;
    QObject::connect(&refresh, &QTimer::timeout, &window, [label] { label->setText(connection()); });
    refresh.start(1500);
    window.resize(440, 140);
    window.show();
    return app.exec();
}
